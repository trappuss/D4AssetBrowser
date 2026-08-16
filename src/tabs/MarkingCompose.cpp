#include "tabs/MarkingCompose.h"

#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtGlobal>
#include <cmath>

// See MarkingCompose.h / STATUS.md for the model. All functions are data-driven — no per-marking
// constants. Grayscale (BC4) masks work because R==G collapses to the design value.

MarkingDef markingDef(const QString& d4, const QString& stem)
{
    MarkingDef m;
    if (stem.isEmpty()) return m;
    QFile f(d4 + QStringLiteral("/json/base/meta/MarkingShape/") + stem + QStringLiteral(".msh.json"));
    if (!f.open(QIODevice::ReadOnly)) return m;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    m.faceTex   = o.value(QStringLiteral("snoMaskFace")).toObject().value(QStringLiteral("name")).toString().section('/', -1);
    m.bodyTex   = o.value(QStringLiteral("snoMaskBody")).toObject().value(QStringLiteral("name")).toString().section('/', -1);
    m.colorStem = o.value(QStringLiteral("snoDefaultColor")).toObject().value(QStringLiteral("name")).toString().section('/', -1);
    m.emissive  = float(o.value(QStringLiteral("flEmissiveStrength")).toDouble(0.0));
    // The shape's own swatch. Read here rather than at the call site so every consumer of a
    // MarkingShape gets the same four facts from one parse.
    m.icon      = quint32(o.value(QStringLiteral("hIconImage")).toDouble(0.0));
    return m;
}

// MarkingColor 3-point ramp (shadow → mid → highlight). arPaintColorSamples are LINEAR floats —
// sRGB-encode (~pow 1/2.2) so the paint reads at its real brightness on the (sRGB) skin texture.
std::array<QColor,3> markingRamp(const QString& d4, const QString& stem)
{
    std::array<QColor,3> ramp{};
    if (stem.isEmpty()) return ramp;
    QFile f(d4 + QStringLiteral("/json/base/meta/MarkingColor/") + stem + QStringLiteral(".mcl.json"));
    if (!f.open(QIODevice::ReadOnly)) return ramp;
    const QJsonArray a = QJsonDocument::fromJson(f.readAll()).object()
                             .value(QStringLiteral("arPaintColorSamples")).toArray();
    auto enc = [](double v) { return int(qBound(0.0, std::pow(qBound(0.0, v, 1.0), 1.0/2.2) * 255.0, 255.0)); };
    for (int i = 0; i < 3 && i < a.size(); ++i) {
        const QJsonObject c = a[i].toObject();
        if (c.isEmpty()) continue;
        ramp[i] = QColor(enc(c.value(QStringLiteral("r")).toDouble()),
                         enc(c.value(QStringLiteral("g")).toDouble()),
                         enc(c.value(QStringLiteral("b")).toDouble()));
    }
    return ramp;
}

// Full paint material for a MarkingColor: ramp + flPaintRoughness/flPaintMetalness + fIsTattoo.
MarkingPaint markingPaint(const QString& d4, const QString& stem)
{
    MarkingPaint p;
    p.ramp = markingRamp(d4, stem);
    p.valid = p.ramp[0].isValid();
    if (stem.isEmpty()) return p;
    QFile f(d4 + QStringLiteral("/json/base/meta/MarkingColor/") + stem + QStringLiteral(".mcl.json"));
    if (!f.open(QIODevice::ReadOnly)) return p;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    p.isTattoo  = o.value(QStringLiteral("fIsTattoo")).toBool(false);
    // No tint, scale or darkening is applied to the ramp. The authored samples are used exactly as
    // they are, sRGB-encoded from the file's linear values and nothing more.
    //
    // This was verified rather than assumed: the samples are exactly (n/255)^2.2, so the encode
    // recovers the artist's original 8-bit colour; Inked Tattoo composites to #093437, which is
    // within a few units of the in-game reading. A 0.45 ink multiplier was tried here and was far
    // too dark. If tattoos ever look wrong again, the cause is downstream of this file — viewport
    // lighting lifts a dark CHROMATIC albedo much harder than a dark neutral one, which is why
    // bodymarking_bar044_stor (snoDefaultColor = null → the neutral fallback ramp) reads as clean
    // black ink while an authored teal-black does not. Fix it there, not by scaling the data.
    if (o.contains(QStringLiteral("flPaintRoughness"))) p.roughness = float(o.value(QStringLiteral("flPaintRoughness")).toDouble());
    if (o.contains(QStringLiteral("flPaintMetalness"))) p.metalness = float(o.value(QStringLiteral("flPaintMetalness")).toDouble());
    return p;
}

QColor rampLerp(const std::array<QColor,3>& r, float t)
{
    if (!r[0].isValid()) return QColor();
    const QColor a = r[0], b = r[1].isValid() ? r[1] : r[0], c = r[2].isValid() ? r[2] : b;
    auto mix = [](const QColor& x, const QColor& y, float f) {
        return QColor(int(x.red()+(y.red()-x.red())*f), int(x.green()+(y.green()-x.green())*f),
                      int(x.blue()+(y.blue()-x.blue())*f));
    };
    return t < 0.5f ? mix(a, b, t*2.0f) : mix(b, c, (t-0.5f)*2.0f);
}

// Per-mask black-level for the RED (coverage) channel. A marking mask's background should be pure
// black (0 coverage), but some masks carry a lifted/noisy dark background (authoring + BC4/BC7
// rounding) that would otherwise faintly tint the surrounding skin. Estimate the background as the
// most common RED value in the lower range — the design sits at HIGH red and is sparse next to the
// empty field — so callers can subtract it. Data-driven per mask, no fixed threshold: a clean mask
// (background already 0) returns ~0 → no change; a lifted one returns its pedestal → it's removed.
static float markingBlackLevel(const QImage& mask0)
{
    if (mask0.isNull()) return 0.0f;
    const QImage m = mask0.convertToFormat(QImage::Format_RGBA8888);
    int hist[256] = {0};
    for (int y = 0; y < m.height(); ++y) {
        const uchar* s = m.constScanLine(y);
        for (int x = 0; x < m.width(); ++x) ++hist[s[x*4+0]];
    }
    int peak = 0, peakN = -1;
    for (int i = 0; i < 128; ++i) if (hist[i] > peakN) { peakN = hist[i]; peak = i; }   // dark modal red
    return qBound(0.0f, (peak - 2) / 255.0f, 0.35f);   // -2 ignores ±1-LSB compression noise; cap = safety
}

// Coverage from a raw red byte after removing the mask's own black-level pedestal, renormalised to
// 0..1 so a real design edge still reaches full opacity (a levels/black-point remap, gradient-safe).
static inline float coverageOf(uchar red, float bg, float invSpan)
{
    return qBound(0.0f, (red / 255.0f - bg) * invSpan, 1.0f);
}

// Composite a D4 marking mask onto the skin: RED = coverage/opacity, GREEN = ramp position (ink→gold).
QImage applyMarking(QImage base, const QImage& mask0, const std::array<QColor,3>& ramp)
{
    if (base.isNull() || mask0.isNull() || !ramp[0].isValid()) return base;
    base = base.convertToFormat(QImage::Format_RGBA8888);
    // Composite at the LARGER of base/mask so a hi-res marking isn't downsampled into a lower-res
    // skin albedo; the skin is upscaled instead. No-op when the skin is already >= the mask.
    const int W = qMax(base.width(), mask0.width()), H = qMax(base.height(), mask0.height());
    if (base.width() != W || base.height() != H)
        base = base.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QImage mk = mask0.convertToFormat(QImage::Format_RGBA8888)
                          .scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const float bg = markingBlackLevel(mask0);              // subtract this mask's own dark background
    const float invSpan = 1.0f / qMax(1e-3f, 1.0f - bg);
    for (int y = 0; y < H; ++y) {
        uchar* d = base.scanLine(y); const uchar* s = mk.scanLine(y);
        for (int x = 0; x < W; ++x) {
            const float cov = coverageOf(s[x*4+0], bg, invSpan);   // R = coverage, black-level removed
            if (cov < 0.004f) continue;
            const float t = s[x*4+1] / 255.0f;      // G = ramp position (ink -> gold)
            const QColor c = rampLerp(ramp, t);
            d[x*4+0] = uchar(qBound(0, int(d[x*4+0]*(1-cov) + c.red()*cov),   255));
            d[x*4+1] = uchar(qBound(0, int(d[x*4+1]*(1-cov) + c.green()*cov), 255));
            d[x*4+2] = uchar(qBound(0, int(d[x*4+2]*(1-cov) + c.blue()*cov),  255));
        }
    }
    return base;
}

// Full marking material: albedo via the ramp, roughness across the design, metalness ONLY on the
// GOLD texels (G), and an emissive glow (gated by G) if the shape glows. base/orm modified in place.
QImage applyMarkingMaterial(QImage& base, QImage& orm, const QImage& mask0,
                            const MarkingPaint& paint, float emissiveStrength,
                            float skinRough, float skinMetal, float& outEmisMul)
{
    outEmisMul = 0.0f;
    if (mask0.isNull() || base.isNull() || !paint.valid) return QImage();
    base = applyMarking(base, mask0, paint.ramp);          // albedo (R=coverage, G=ramp/gold)
    const int W = base.width(), H = base.height();
    const QImage mk = mask0.convertToFormat(QImage::Format_RGBA8888)
                          .scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const float bg = markingBlackLevel(mask0);             // same black-level as the albedo composite
    const float invSpan = 1.0f / qMax(1e-3f, 1.0f - bg);

    if (paint.roughness >= 0.0f || paint.metalness >= 0.0f) {
        if (orm.isNull()) {
            orm = QImage(W, H, QImage::Format_RGBA8888);
            orm.fill(QColor(255, int(qBound(0.0f, skinRough, 1.0f) * 255),
                                 int(qBound(0.0f, skinMetal, 1.0f) * 255)));
        } else {
            orm = orm.convertToFormat(QImage::Format_RGBA8888)
                     .scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        const float rgh = qBound(0.0f, paint.roughness, 1.0f);
        const float mtl = qBound(0.0f, paint.metalness, 1.0f);
        for (int y = 0; y < H; ++y) {
            uchar* o = orm.scanLine(y); const uchar* s = mk.scanLine(y);
            for (int x = 0; x < W; ++x) {
                const float cov = coverageOf(s[x*4+0], bg, invSpan); if (cov < 0.004f) continue;
                const float gold = s[x*4+1] / 255.0f;   // metalness only on gold texels
                if (paint.roughness >= 0.0f) o[x*4+1] = uchar(qBound(0, int(o[x*4+1]*(1-cov) + rgh*255*cov), 255));
                if (paint.metalness >= 0.0f) o[x*4+2] = uchar(qBound(0, int(o[x*4+2]*(1-cov) + mtl*gold*255*cov), 255));
            }
        }
    }

    QImage emis;
    if (emissiveStrength > 0.001f) {
        const QImage me = mask0.convertToFormat(QImage::Format_RGBA8888);
        const int EW = me.width(), EH = me.height();
        emis = QImage(EW, EH, QImage::Format_RGBA8888); emis.fill(QColor(0, 0, 0, 255));
        const QColor glow = paint.ramp[2].isValid() ? paint.ramp[2]
                          : (paint.ramp[1].isValid() ? paint.ramp[1] : paint.ramp[0]);
        for (int y = 0; y < EH; ++y) {
            uchar* e = emis.scanLine(y); const uchar* s = me.constScanLine(y);
            for (int x = 0; x < EW; ++x) {
                const float cov = coverageOf(s[x*4+0], bg, invSpan); if (cov < 0.004f) continue;   // R = coverage
                const float g   = s[x*4+1] / 255.0f;                             // G = highlight-ness -> glows
                const float m = g * g;                          // only the bright (gold/cyan) parts emit
                e[x*4+0] = uchar(glow.red()   * m);
                e[x*4+1] = uchar(glow.green() * m);
                e[x*4+2] = uchar(glow.blue()  * m);
            }
        }
        outEmisMul = emissiveStrength;
    }
    return emis;
}

// Self-check of the marking model (run once at startup; "" = pass).
QString markingSelfTest()
{
    QImage base(3, 1, QImage::Format_RGBA8888);
    for (int x = 0; x < 3; ++x) base.setPixelColor(x, 0, QColor(180, 140, 120));   // skin
    QImage mask(3, 1, QImage::Format_RGBA8888);
    mask.setPixelColor(0, 0, QColor(0,   0,   0));   // skin : R=0 coverage -> untouched
    mask.setPixelColor(1, 0, QColor(255, 0,   0));   // ink  : R=255 cov, G=0   -> ramp shadow
    mask.setPixelColor(2, 0, QColor(255, 255, 0));   // gold : R=255 cov, G=255 -> ramp highlight
    const std::array<QColor,3> ramp = { QColor(10,10,10), QColor(100,100,100), QColor(240,240,240) };

    const QImage tinted = applyMarking(base, mask, ramp);
    if (qRed(tinted.pixel(0,0)) < 150) return QStringLiteral("marking: skin (R=0) was tinted");
    if (qRed(tinted.pixel(1,0)) > 60)  return QStringLiteral("marking: ink (G=0) not near ramp shadow");
    if (qRed(tinted.pixel(2,0)) < 200) return QStringLiteral("marking: gold (G=1) not near ramp highlight");

    MarkingPaint paint; paint.ramp = ramp; paint.valid = true; paint.roughness = 0.5f; paint.metalness = 0.8f;
    QImage b2 = base, orm; float em = 0.0f;
    applyMarkingMaterial(b2, orm, mask, paint, 0.0f, 0.5f, 0.0f, em);
    if (orm.isNull()) return QStringLiteral("marking: ORM not produced");
    const int mInk = qBlue(orm.pixel(1,0)), mGold = qBlue(orm.pixel(2,0));   // ORM.b = metalness
    if (mInk > 40)   return QStringLiteral("marking: ink metalness should be ~0, got %1").arg(mInk);
    if (mGold < 180) return QStringLiteral("marking: gold metalness should be high, got %1").arg(mGold);
    return QString();
}
