#include "app/ExportCapture.h"

#include "gl/GLModelWidget.h"
#include "gl/GifEncoder.h"

#include <QImage>
#include <QColor>
#include <QFileInfo>
#include <QSettings>
#include <QtGlobal>
#include <cstring>
#include <cmath>
#include <vector>
#include <utility>

namespace {

bool transparentEnabled()
{
    return QSettings().value(QStringLiteral("export/transparentBg"), false).toBool();
}

QImage grabOpaque(GLModelWidget* v)
{
    return v->grabFramebuffer().convertToFormat(QImage::Format_RGBA8888);
}

// Native-alpha capture: one render with the background cleared to alpha 0, reading the
// framebuffer's own alpha. Single render (no matting), but relies on the pipeline preserving
// per-fragment alpha through post-processing — offered as an alternative to difference matting.
QImage grabNativeAlpha(GLModelWidget* v)
{
    v->setTransparentClear(true);
    QImage img = v->grabFramebuffer().convertToFormat(QImage::Format_RGBA8888);
    v->setTransparentClear(false);
    return img.isNull() ? grabOpaque(v) : img;
}

// True-alpha capture via difference matting: render the SAME frame on a black and a white
// background, then recover per-pixel coverage (alpha) and straight colour. This needs no
// GL alpha plumbing and is robust to the shading pipeline, because the only thing that
// differs between the two renders is the background (IBL/ambient use a preset gradient, not
// the clear colour). Anti-aliased edges resolve to fractional alpha correctly.
QImage grabTransparent(GLModelWidget* v)
{
    const QColor orig = v->backgroundColor();
    v->setBackgroundColor(QColor(0, 0, 0));
    QImage b = v->grabFramebuffer().convertToFormat(QImage::Format_RGB888);
    v->setBackgroundColor(QColor(255, 255, 255));
    QImage w = v->grabFramebuffer().convertToFormat(QImage::Format_RGB888);
    v->setBackgroundColor(orig);
    if (b.isNull() || w.isNull() || b.size() != w.size()) return grabOpaque(v);

    const int W = b.width(), H = b.height();
    QImage out(W, H, QImage::Format_RGBA8888);
    for (int y = 0; y < H; ++y) {
        const uchar* rb = b.constScanLine(y);
        const uchar* rw = w.constScanLine(y);
        uchar* ro = out.scanLine(y);
        for (int x = 0; x < W; ++x) {
            const int bR = rb[x * 3], bG = rb[x * 3 + 1], bB = rb[x * 3 + 2];
            const int wR = rw[x * 3], wG = rw[x * 3 + 1], wB = rw[x * 3 + 2];
            int diff = ((wR - bR) + (wG - bG) + (wB - bB)) / 3;   // (1-A)*255, averaged
            diff = qBound(0, diff, 255);
            const int a = 255 - diff;                            // coverage
            int oR = 0, oG = 0, oB = 0;
            if (a > 0) {   // straight colour = premultiplied (black-bg) value / alpha
                oR = qBound(0, bR * 255 / a, 255);
                oG = qBound(0, bG * 255 / a, 255);
                oB = qBound(0, bB * 255 / a, 255);
            }
            ro[x * 4] = uchar(oR); ro[x * 4 + 1] = uchar(oG);
            ro[x * 4 + 2] = uchar(oB); ro[x * 4 + 3] = uchar(a);
        }
    }
    return out;
}

QImage captureFrame(GLModelWidget* v, bool transparent)
{
    if (!transparent) return grabOpaque(v);
    // export/transparentMode: 0 = difference matting (robust), 1 = native alpha (single render).
    return QSettings().value(QStringLiteral("export/transparentMode"), 0).toInt() == 1
               ? grabNativeAlpha(v) : grabTransparent(v);
}

// Append a QImage as tightly-packed RGBA to `frames`; the first frame fixes the size. scalePct<100
// downscales the whole GIF (the single biggest file-size lever, since size is ~quadratic in dimension).
bool pushFrame(const QImage& srcIn, std::vector<std::vector<uint8_t>>& frames, int& gw, int& gh, int scalePct = 100)
{
    QImage f = srcIn.convertToFormat(QImage::Format_RGBA8888);
    if (f.isNull()) return false;
    if (gw == 0) {
        int w = f.width(), h = f.height();
        if (scalePct > 0 && scalePct < 100) {
            w = qMax(16, w * scalePct / 100);
            h = qMax(16, h * scalePct / 100);
        }
        gw = w; gh = h;
    }
    if (gw == 0 || gh == 0) return false;
    if (f.width() != gw || f.height() != gh) f = f.scaled(gw, gh);
    std::vector<uint8_t> buf(size_t(gw) * size_t(gh) * 4u);
    for (int y = 0; y < gh; ++y)
        std::memcpy(buf.data() + size_t(y) * size_t(gw) * 4u, f.constScanLine(y), size_t(gw) * 4u);
    frames.push_back(std::move(buf));
    return true;
}

// Downscale every captured RGBA frame in-place to nw x nh (cheap, no re-render).
void downscaleFrames(std::vector<std::vector<uint8_t>>& frames, int& gw, int& gh, int nw, int nh)
{
    if (nw >= gw && nh >= gh) return;
    for (auto& f : frames) {
        QImage im(reinterpret_cast<const uchar*>(f.data()), gw, gh, QImage::Format_RGBA8888);
        QImage s = im.scaled(nw, nh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                     .convertToFormat(QImage::Format_RGBA8888);
        std::vector<uint8_t> nb(size_t(nw) * size_t(nh) * 4u);
        for (int y = 0; y < nh; ++y)
            std::memcpy(nb.data() + size_t(y) * size_t(nw) * 4u, s.constScanLine(y), size_t(nw) * 4u);
        f = std::move(nb);
    }
    gw = nw; gh = nh;
}

// Encode the GIF; if "Optimize to target size" is on and the file exceeds the
// target, iteratively cut palette colours then downscale the frames and re-encode
// until the file fits (or we hit the floor). Frames are captured once, so retries
// only re-encode — no extra rendering.
bool encodeWithBudget(const QString& path, std::vector<std::vector<uint8_t>>& buf,
                      int gw, int gh, int delayCs, bool loop, int transThresh, int maxColors)
{
    QSettings s;
    const bool optimize = s.value(QStringLiteral("export/gifOptimize"), false).toBool();
    const qint64 targetBytes =
        qint64(qBound(1, s.value(QStringLiteral("export/gifTargetMB"), 10).toInt(), 200)) * 1024 * 1024;

    // Ordered dithering breaks up palette banding. Without it, smooth shaded surfaces band, and on
    // a MOVING garment the band edges crawl across the surface frame to frame — indistinguishable
    // from simulation jitter even when the sim is fully deterministic.
    const bool dither = s.value(QStringLiteral("export/gifDither"), true).toBool();

    int colors = maxColors;
    for (int attempt = 0; attempt < 12; ++attempt) {
        if (!GifEncoder::encode(path.toStdString(), buf, gw, gh, delayCs, loop, transThresh,
                                colors, dither))
            return false;
        if (!optimize) return true;

        const qint64 sz = QFileInfo(path).size();
        if (sz <= 0 || sz <= targetBytes) return true;

        // Cut the palette first (big win, keeps resolution) down to a 32-colour floor.
        if (colors > 32) { colors = qMax(32, colors * 3 / 4); continue; }

        // Then shrink the frames ~15% per pass (quadratic size reduction) to a 96px floor.
        const int nw = qMax(96, gw * 85 / 100);
        const int nh = qMax(96, gh * 85 / 100);
        if (nw >= gw && nh >= gh) return true;   // can't shrink further — ship what we have
        downscaleFrames(buf, gw, gh, nw, nh);
    }
    return true;
}

}  // namespace

bool ExportCapture::saveImage(GLModelWidget* view, const QString& path)
{
    if (!view || path.isEmpty()) return false;
    // True transparency only for PNG (JPEG has no alpha).
    const bool wantT = transparentEnabled() && path.endsWith(QLatin1String(".png"), Qt::CaseInsensitive);
    const QImage img = captureFrame(view, wantT);
    if (img.isNull()) return false;
    return img.save(path);   // PNG/JPEG inferred from the extension
}

bool ExportCapture::turntableGif(GLModelWidget* view, const QString& path, const ProgressFn& progress)
{
    if (!view || path.isEmpty()) return false;
    // Deterministic sim: one cloth step per captured frame, and the orbit is treated as a camera
    // move rather than the model spinning. Without this the exported GIF twitches — see
    // GLModelWidget::setCaptureMode. Scoped so the early-outs below cannot leave it latched.
    GLModelWidget::CaptureScope capture(view);
    QSettings s;
    const bool wantT  = transparentEnabled();
    const int frames  = qBound(8, s.value(QStringLiteral("export/gifTurntableFrames"), 48).toInt(), 240);
    const int fps     = qBound(1, s.value(QStringLiteral("export/gifFps"), 25).toInt(), 60);
    const int delayCs = qMax(2, 100 / fps);
    const int scalePct = qBound(25, s.value(QStringLiteral("export/gifScale"), 100).toInt(), 100);
    const int maxCols  = qBound(16, s.value(QStringLiteral("export/gifMaxColors"), 256).toInt(), 256);
    const float startYaw = view->orbitYaw();

    // (1) Settle steps per captured frame — see GLModelWidget::settleCloth. A turntable has no
    // animation, so every frame is the same pose and the cloth should simply be allowed to hang.
    const int physSteps = qBound(1, s.value(QStringLiteral("export/gifPhysicsSteps"), 3).toInt(), 8);

    std::vector<std::vector<uint8_t>> buf; int gw = 0, gh = 0;
    for (int i = 0; i < frames; ++i) {
        view->setOrbitYaw(startYaw + float(i) / float(frames) * 2.0f * 3.14159265f);
        view->setCaptureTime(float(i) * float(delayCs) * 0.01f);   // uniform FX time per frame
        view->settleCloth(physSteps);
        pushFrame(captureFrame(view, wantT), buf, gw, gh, scalePct);
        if (progress && !progress(i + 1, frames)) { view->setOrbitYaw(startYaw); return false; }
    }
    view->setOrbitYaw(startYaw);   // restore the original angle

    if (buf.empty() || gw == 0) return false;
    return encodeWithBudget(path, buf, gw, gh, delayCs, /*loop=*/true,
                            /*transparentAlphaThreshold=*/wantT ? 128 : -1, maxCols);
}

bool ExportCapture::animLoopGif(GLModelWidget* view, const QString& path, const ProgressFn& progress)
{
    if (!view || path.isEmpty()) return false;
    const int n = view->animFrameCount();
    if (n <= 0) return false;
    // One cloth step per captured frame — the capture loop pumps events for the progress dialog,
    // so the idle settle timer would otherwise advance the sim a wall-clock-dependent number of
    // extra steps between frames. See GLModelWidget::setCaptureMode. Covers the warm-up lap too,
    // so the lap and the captured lap advance the sim identically and the loop wrap stays seamless.
    GLModelWidget::CaptureScope capture(view);
    const bool wantT = transparentEnabled();
    const float fr = view->animFrameRate() > 1.0f ? view->animFrameRate() : 30.0f;
    const int delayCs = qBound(2, int(std::lround(100.0 / double(fr))), 100);
    const int scalePct = qBound(25, QSettings().value(QStringLiteral("export/gifScale"), 100).toInt(), 100);
    const int maxCols  = qBound(16, QSettings().value(QStringLiteral("export/gifMaxColors"), 256).toInt(), 256);
    const int prev = view->animFrame();

    // Warm-up lap (not captured): step every frame once first. The capture otherwise TELEPORTS
    // the pose from whatever frame the preview was on to frame 0 — that whip explodes the cloth
    // sim and the blow-up is baked into the first captured frames. One silent lap lets the
    // physics reach its natural per-frame steady state; as a bonus the sim state at frame 0 then
    // matches the state after the last frame, so the GIF's loop wrap is seamless as well.
    // (1) settle steps + (3) rate matching.
    // The GIF's real playback rate is not the clip's rate: delayCs is an INTEGER number of
    // centiseconds, so a 30 fps clip ships as 3 cs = 33.3 fps and plays ~11% fast. The sim advanced
    // once per animation frame regardless, so cloth motion drifted against body motion. Scale the
    // steps-per-frame by (GIF frame duration / clip frame duration) and carry the fraction, so the
    // cloth advances at the rate the GIF will actually be shown at.
    const int physSteps = qBound(1, QSettings().value(QStringLiteral("export/gifPhysicsSteps"), 3).toInt(), 8);
    // Rate matching is folded in ONCE, as a constant — never per frame.
    //
    // A fractional steps-per-frame carried through an accumulator makes the integer step count
    // alternate (2.7 → 3,2,3,3,2…). Uneven advance per frame is the exact defect capture mode
    // exists to remove, and it shows up a few frames in, once the remainder has built up: the GIF
    // starts smooth and then judders. Every frame must advance the sim by the SAME amount.
    // The residual rate error (delayCs is whole centiseconds, so a 30 fps clip ships as 33.3 fps)
    // is a uniform ~11% speed difference — invisible, and far cheaper than reintroducing jitter.
    const double rateScale = qBound(0.25, (double(delayCs) / 100.0) * double(fr), 4.0);
    const int    stepsPerFrame = qBound(1, int(std::lround(double(physSteps) * rateScale)), 8);
    const int    extraPerFrame = stepsPerFrame - 1;   // setFrame() already advances the sim once

    for (int f = 0; f < n; ++f) {                 // warm-up lap: identical stepping, nothing captured
        view->setFrame(f);
        view->settleCloth(extraPerFrame);
    }

    std::vector<std::vector<uint8_t>> buf; int gw = 0, gh = 0;
    for (int f = 0; f < n; ++f) {
        view->setFrame(f);
        view->setCaptureTime(float(f) * float(delayCs) * 0.01f);   // uniform FX time per frame
        view->settleCloth(extraPerFrame);
        pushFrame(captureFrame(view, wantT), buf, gw, gh, scalePct);
        if (progress && !progress(f + 1, n)) { view->setFrame(prev); return false; }
    }
    view->setFrame(prev);   // restore the frame the preview was on

    if (buf.empty() || gw == 0) return false;
    return encodeWithBudget(path, buf, gw, gh, delayCs, /*loop=*/true,
                            /*transparentAlphaThreshold=*/wantT ? 128 : -1, maxCols);
}
