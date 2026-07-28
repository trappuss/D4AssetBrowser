#include "app/ExportCapture.h"

#include "gl/GLModelWidget.h"
#include "gl/GifEncoder.h"

#include <QImage>
#include <QDebug>
#include <QElapsedTimer>
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

// Native alpha is the only transparency path. Difference matting used to be the alternative — two
// renders on black and white, recovering coverage from the difference — which doubled capture time
// for every frame and could not recover straight colour where alpha was low without amplifying
// noise. The pipeline preserves per-fragment alpha, so the second render bought nothing.
bool cropEnabled()
{
    return QSettings().value(QStringLiteral("export/gifCropToModel"), false).toBool();
}

// Normal opaque render, but with the alpha channel left as model coverage so the crop pass can
// find the subject. One render, not two — the backdrop still draws, it just does not mark alpha.
QImage grabCoverage(GLModelWidget* v)
{
    v->setCoverageAlpha(true);
    QImage img = v->grabFramebuffer().convertToFormat(QImage::Format_RGBA8888);
    v->setCoverageAlpha(false);
    return img.isNull() ? grabOpaque(v) : img;
}

// wantCoverage is passed explicitly rather than read from the setting here, because a still image
// must never take the coverage path: it would ship a PNG whose background is fully transparent
// while looking like a normal backdrop render. Only the GIF exporters ask for it, and only because
// they strip the alpha again once the crop box is known.
QImage captureFrame(GLModelWidget* v, bool transparent, bool wantCoverage = false)
{
    if (transparent) return grabNativeAlpha(v);
    return wantCoverage ? grabCoverage(v) : grabOpaque(v);
}

// Crop every frame to the union of the model's silhouette across the WHOLE sequence — one box for
// all frames, because a per-frame box would make the subject swim around as the crop chased it.
//
// The alpha channel is the silhouette: a transparent export has it natively, and an opaque one gets
// it from coverage mode. Opaque frames are then forced back to alpha 255, so the GIF encoder still
// takes its opaque path (and with it inter-frame differencing, which a 1-bit-alpha export cannot
// use). Returns false when nothing was found — an empty viewport must not crop to nothing.
bool cropFramesToModel(std::vector<std::vector<uint8_t>>& frames, int& gw, int& gh, bool keepAlpha)
{
    if (frames.empty() || gw <= 0 || gh <= 0) return false;
    // 8/255 rather than 0: anti-aliased silhouette edges and faint FX fade to near-zero coverage,
    // and a strict >0 test would chase single stray pixels out to the frame border.
    constexpr int kMinAlpha = 8;
    int x0 = gw, y0 = gh, x1 = -1, y1 = -1;
    for (const auto& f : frames)
        for (int y = 0; y < gh; ++y) {
            const uint8_t* row = f.data() + size_t(y) * size_t(gw) * 4u;
            for (int x = 0; x < gw; ++x)
                if (row[x * 4 + 3] >= kMinAlpha) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        }
    if (x1 < 0) return false;   // nothing drawn

    // A few pixels of air so the silhouette is not shaved flush against the edge.
    constexpr int kPad = 4;
    x0 = qMax(0, x0 - kPad); y0 = qMax(0, y0 - kPad);
    x1 = qMin(gw - 1, x1 + kPad); y1 = qMin(gh - 1, y1 + kPad);
    const int nw = x1 - x0 + 1, nh = y1 - y0 + 1;
    if (nw <= 0 || nh <= 0) return false;
    if (nw == gw && nh == gh && keepAlpha) return false;   // already tight — nothing to do

    for (auto& f : frames) {
        std::vector<uint8_t> nb(size_t(nw) * size_t(nh) * 4u);
        for (int y = 0; y < nh; ++y) {
            const uint8_t* src = f.data() + (size_t(y + y0) * size_t(gw) + size_t(x0)) * 4u;
            uint8_t* dst = nb.data() + size_t(y) * size_t(nw) * 4u;
            std::memcpy(dst, src, size_t(nw) * 4u);
            if (!keepAlpha)
                for (int x = 0; x < nw; ++x) dst[x * 4 + 3] = 255;   // coverage was a means, not output
        }
        f = std::move(nb);
    }
    gw = nw; gh = nh;
    return true;
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

// Encode the GIF, and when "Optimize to target size" is on, keep re-encoding until the result
// fits. Frames are captured once, so retries only re-encode — no extra rendering.
//
// Three things were wrong with the old loop, and together they are why the target was missed:
//
//   · CUTTING COLOURS MADE THE FILE BIGGER. Dither amplitude is scaled to palette coarseness
//     (256/colours × 3), so dropping 256 → 32 raised it eightfold. Ordered dithering replaces flat
//     runs with an 8×8 alternating pattern, which is precisely what LZW cannot compress — so the
//     optimiser's primary lever fought itself, and with dither on it could climb for several passes
//     before it ever started shrinking. Dither is now a lever of its own, tried before resolution
//     is sacrificed.
//   · IT SHIPPED THE LAST ATTEMPT, NOT THE BEST. Every pass overwrote the file, so a pass that came
//     out larger than an earlier one was what the user got.
//   · IT GAVE UP EARLY AND SILENTLY. Twelve passes, of which eight went on palette steps, left three
//     downscales of 15% — 0.61× linear, nowhere near enough for a file several times over budget,
//     and it returned success regardless.
//
// Downscaling is now aimed rather than nibbled: file size is close to linear in pixel count, so one
// pass at sqrt(target/actual) lands near the target instead of creeping toward it.
bool encodeWithBudget(const QString& path, std::vector<std::vector<uint8_t>>& buf,
                      int gw, int gh, int delayCs, bool loop, int transThresh, int maxColors)
{
    QSettings s;
    const bool optimize = s.value(QStringLiteral("export/gifOptimize"), false).toBool();
    const qint64 targetBytes =
        qint64(qBound(1, s.value(QStringLiteral("export/gifTargetMB"), 10).toInt(), 200)) * 1024 * 1024;

    // Ordered dithering breaks up palette banding. Without it, smooth shaded surfaces band, and on
    // a MOVING garment the band edges crawl across the surface frame to frame — indistinguishable
    // from simulation jitter even when the sim is fully deterministic. Kept on unless the budget
    // forces it off.
    const bool wantDither = s.value(QStringLiteral("export/gifDither"), true).toBool();

    QElapsedTimer clock; clock.start();
    std::vector<uint8_t> bytes, best;
    int bestW = gw, bestH = gh, bestColors = maxColors;
    bool bestDither = wantDither;

    auto attempt = [&](int colors, bool dither) -> qint64 {
        if (!GifEncoder::encodeToBuffer(bytes, buf, gw, gh, delayCs, loop, transThresh,
                                        colors, dither))
            return -1;
        if (best.empty() || bytes.size() < best.size()) {
            best = bytes; bestW = gw; bestH = gh; bestColors = colors; bestDither = dither;
        }
        return qint64(bytes.size());
    };

    qint64 sz = attempt(maxColors, wantDither);
    if (sz < 0) return false;
    if (!optimize || sz <= targetBytes) {
        qInfo("gif: %dx%d px, %d frame(s), %d colours, dither %s — %.2f MB in %lld ms",
              gw, gh, int(buf.size()), maxColors, wantDither ? "on" : "off",
              double(sz) / (1024.0 * 1024.0), clock.elapsed());
        return GifEncoder::writeBuffer(path.toStdString(), bytes);
    }

    // 1. Palette, down to a 32-colour floor. Cheapest in perceived quality per byte saved.
    int colors = maxColors;
    while (sz > targetBytes && colors > 32) {
        colors = qMax(32, colors * 3 / 4);
        sz = attempt(colors, wantDither);
        if (sz < 0) return false;
    }
    // 2. Dither off. At a coarse palette this is often the single largest saving available, because
    //    it hands LZW back the flat runs the Bayer pattern was breaking up.
    if (sz > targetBytes && wantDither) {
        sz = attempt(colors, false);
        if (sz < 0) return false;
    }
    const bool ditherNow = (sz <= targetBytes) ? bestDither : false;
    // 3. Resolution, aimed at the target rather than stepped toward it. A few passes because the
    //    size/pixel relationship is only approximately linear.
    for (int pass = 0; pass < 5 && sz > targetBytes; ++pass) {
        const double ratio = double(targetBytes) / double(sz);
        // 0.93 of the ideal ratio so a slight underestimate still lands under budget; floored so a
        // single pass cannot collapse the image, and capped so a pass always makes progress.
        const double k = qBound(0.35, std::sqrt(ratio) * 0.93, 0.92);
        const int nw = qMax(96, int(gw * k));
        const int nh = qMax(96, int(gh * k));
        if (nw >= gw && nh >= gh) break;             // at the 96px floor — nothing left to give
        downscaleFrames(buf, gw, gh, nw, nh);
        sz = attempt(colors, ditherNow);
        if (sz < 0) return false;
    }

    const bool hit = !best.empty() && qint64(best.size()) <= targetBytes;
    qInfo("gif: %s — %.2f MB vs %.2f MB target · %dx%d px, %d colours, dither %s · %lld ms",
          hit ? "target met" : "TARGET NOT REACHABLE — shipping the smallest encode",
          double(best.size()) / (1024.0 * 1024.0), double(targetBytes) / (1024.0 * 1024.0),
          bestW, bestH, bestColors, bestDither ? "on" : "off", clock.elapsed());
    return GifEncoder::writeBuffer(path.toStdString(), best);
}

}  // namespace

QString ExportCapture::imageFormat()
{
    const QString f = QSettings().value(QStringLiteral("export/imageFormat"),
                                        QStringLiteral("png")).toString().toLower();
    return (f == QLatin1String("jpg") || f == QLatin1String("webp")) ? f : QStringLiteral("png");
}

bool ExportCapture::saveImage(GLModelWidget* view, const QString& path)
{
    if (!view || path.isEmpty()) return false;
    // Alpha only where the container has it. JPEG has none at all, so asking for a transparent
    // background there would silently composite onto black.
    const QString ext = QFileInfo(path).suffix().toLower();
    const bool alphaOk = (ext != QLatin1String("jpg") && ext != QLatin1String("jpeg"));
    const bool wantT = transparentEnabled() && alphaOk;

    // Resolution. Above 100% the scene is genuinely re-rendered larger (real detail, real
    // anti-aliasing); at or below it the viewport grab is resampled down. Upscaling a grab would
    // only invent pixels, so it is not offered.
    const int pct = qBound(25, QSettings().value(QStringLiteral("export/imageScale"), 100).toInt(), 400);
    QImage img;
    if (pct > 100) {
        const int factor = qBound(2, (pct + 99) / 100, 4);   // 2x covers 101..200, 3x 201..300, 4x above
        // The capture flags have to be live across the render, not just the grab.
        if (wantT) view->setTransparentClear(true);
        img = view->grabSupersampled(factor);
        if (wantT) view->setTransparentClear(false);
        if (img.isNull()) img = captureFrame(view, wantT);   // driver refused the size — ship 1x
        else {
            img = img.convertToFormat(QImage::Format_RGBA8888);
            // Land on the requested percentage rather than the whole factor it was rendered at.
            const int tw = qMax(1, int(qint64(view->width()) * pct / 100));
            if (img.width() != tw)
                img = img.scaled(tw, qMax(1, img.height() * tw / img.width()),
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
    } else {
        img = captureFrame(view, wantT);
        if (!img.isNull() && pct < 100)
            img = img.scaled(qMax(1, img.width() * pct / 100), qMax(1, img.height() * pct / 100),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    if (img.isNull()) return false;
    if (!alphaOk) img = img.convertToFormat(QImage::Format_RGB888);

    // -1 lets Qt pick the format's own default; only a deliberate setting overrides it.
    const int q = qBound(1, QSettings().value(QStringLiteral("export/imageQuality"), 92).toInt(), 100);
    const bool lossy = (ext == QLatin1String("jpg") || ext == QLatin1String("jpeg")
                        || ext == QLatin1String("webp"));
    const bool ok = img.save(path, nullptr, lossy ? q : -1);
    qInfo("image: %s %dx%d%s", qPrintable(ext.toUpper()), img.width(), img.height(),
          pct > 100 ? " (supersampled)" : "");
    return ok;
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
    int frames        = qBound(8, s.value(QStringLiteral("export/gifTurntableFrames"), 48).toInt(), 240);
    const int fps     = qBound(1, s.value(QStringLiteral("export/gifFps"), 25).toInt(), 60);
    const int delayCs = qMax(2, 100 / fps);
    const int scalePct = qBound(25, s.value(QStringLiteral("export/gifScale"), 100).toInt(), 100);
    const int maxCols  = qBound(16, s.value(QStringLiteral("export/gifMaxColors"), 256).toInt(), 256);
    const bool cropToModel = cropEnabled() && !wantT;   // transparent captures already carry alpha
    const float startYaw = view->orbitYaw();
    const int prevFrame  = view->animFrame();

    // The clip plays THROUGH the orbit — but only if it is actually PLAYING. The turntable used to
    // hold one pose the whole way round, which is right for a static prop and a spinning statue for
    // anything with an idle. "A clip is loaded" is the wrong test though: a clip stays loaded when
    // you pause it on a frame you want to look at, and turning that pose into a walk cycle is not
    // what pausing meant. The transport is the intent, so the export follows it: playing gives an
    // animated turntable, paused or stopped orbits the pose you left on screen.
    const int clipN = view->animPlaying() ? view->animFrameCount() : 0;
    //
    // A GIF loops the entire sequence, so BOTH the orbit and the pose have to arrive back where
    // they started. The orbit always does (a full revolution by construction); the pose only does
    // if the clip completes a whole number of cycles across the capture. So the capture length is
    // snapped to the nearest whole number of clip loops — a 48-frame turntable of a 56-frame idle
    // becomes 56 frames, one revolution and one clip cycle, and the wrap is seamless at the clip's
    // authored speed.
    //
    // When a whole number of loops will not fit in the 240-frame ceiling, the clip is instead
    // mapped proportionally across the revolution: one cycle per turn. That still wraps seamlessly,
    // it just plays the clip at the turntable's speed rather than its own.
    bool authoredRate = false;
    if (clipN > 0) {
        const int loops = qMax(1, int(std::lround(double(frames) / double(clipN))));
        if (loops * clipN <= 240) { frames = qMax(8, loops * clipN); authoredRate = true; }
    }
    auto clipFrameFor = [&](int i) {
        return authoredRate ? (i % clipN) : int(qint64(i) * clipN / frames);
    };

    // (1) Settle steps per captured frame — see GLModelWidget::settleCloth. With no clip every
    // frame is the same pose and the cloth is simply allowed to hang; with one, setFrame already
    // advances the sim once, so only the extras are added here.
    const int physSteps = qBound(1, s.value(QStringLiteral("export/gifPhysicsSteps"), 3).toInt(), 8);
    const int extraSteps = clipN > 0 ? physSteps - 1 : physSteps;

    // Warm-up lap (not captured), for the same reason animLoopGif has one: stepping the pose from
    // wherever the preview sat to frame 0 whips the cloth, and the blow-up would be baked into the
    // first captured frames. It also leaves the sim at frame 0 in the state it will be in after the
    // last frame, so the GIF's wrap is seamless in the cloth as well as the pose.
    if (clipN > 0)
        for (int i = 0; i < frames; ++i) { view->setFrame(clipFrameFor(i)); view->settleCloth(extraSteps); }

    std::vector<std::vector<uint8_t>> buf; int gw = 0, gh = 0;
    auto restore = [&] { view->setOrbitYaw(startYaw); if (clipN > 0) view->setFrame(prevFrame); };
    for (int i = 0; i < frames; ++i) {
        view->setOrbitYaw(startYaw + float(i) / float(frames) * 2.0f * 3.14159265f);
        if (clipN > 0) view->setFrame(clipFrameFor(i));
        view->setCaptureTime(float(i) * float(delayCs) * 0.01f);   // uniform FX time per frame
        view->settleCloth(extraSteps);
        pushFrame(captureFrame(view, wantT, cropToModel), buf, gw, gh, scalePct);
        if (progress && !progress(i + 1, frames)) { restore(); return false; }
    }
    restore();
    // Says which of the two turntables you got, because "why is my GIF 56 frames when I asked for
    // 48" and "why is my model not moving" are both answered here.
    if (clipN > 0)
        qInfo("gif turntable: %d frame(s) — playing a %d-frame clip %s", frames, clipN,
              authoredRate ? "at its authored rate — whole loops per revolution"
                           : "mapped to one cycle per revolution (whole loops exceed the 240 cap)");
    else
        qInfo("gif turntable: %d frame(s) — static pose (%s)", frames,
              view->animFrameCount() > 0 ? "a clip is loaded but not playing — press Play to animate it"
                                         : "no clip loaded");

    if (buf.empty() || gw == 0) return false;
    if (cropEnabled()) {
        const int fw = gw, fh = gh;
        if (cropFramesToModel(buf, gw, gh, wantT))
            qInfo("gif: cropped to model — %dx%d from %dx%d (%.0f%% of the pixels)",
                  gw, gh, fw, fh, 100.0 * double(gw) * gh / (double(fw) * fh));
    }
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
    const bool cropToModel = cropEnabled() && !wantT;   // transparent captures already carry alpha
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
        pushFrame(captureFrame(view, wantT, cropToModel), buf, gw, gh, scalePct);
        if (progress && !progress(f + 1, n)) { view->setFrame(prev); return false; }
    }
    view->setFrame(prev);   // restore the frame the preview was on

    if (buf.empty() || gw == 0) return false;
    if (cropEnabled()) {
        const int fw = gw, fh = gh;
        if (cropFramesToModel(buf, gw, gh, wantT))
            qInfo("gif: cropped to model — %dx%d from %dx%d (%.0f%% of the pixels)",
                  gw, gh, fw, fh, 100.0 * double(gw) * gh / (double(fw) * fh));
    }
    return encodeWithBudget(path, buf, gw, gh, delayCs, /*loop=*/true,
                            /*transparentAlphaThreshold=*/wantT ? 128 : -1, maxCols);
}
