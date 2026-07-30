#include "model/MaterialDecode.h"

#include "index/SnoIndex.h"
#include "model/AppearanceMatBin.h"

#include "casc/CascReader.h"
#include "model/Material.h"
#include "tex/BcDecode.h"
#include "tex/TexMeta.h"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

namespace {
QByteArray readMat(const QString& d4, const QString& matName)
{
    if (matName.isEmpty() || d4.isEmpty()) return {};
    QFile f(QStringLiteral("%1/json/base/meta/Material/%2.mat.json").arg(d4, matName));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
}  // namespace

QImage MaterialDecode::texture(CascReader* reader, const QString& d4,
                               const QString& texName, qint64 texSno)
{
    if (!reader || texName.isEmpty() || texSno <= 0 || d4.isEmpty()) return {};
    TexMeta meta;
    QFile f(QStringLiteral("%1/json/base/meta/Texture/%2.tex.json").arg(d4, texName));
    if (f.open(QIODevice::ReadOnly)) meta = parseTexMetaJson(f.readAll());
    if (!meta.valid) return {};
    const QByteArray payload = reader->readPayloadBySno(quint64(texSno));
    if (payload.isEmpty()) return {};
    return BcDecode::decode(payload, meta.width, meta.height, meta.eTexFormat);
}

QImage MaterialDecode::byRole(CascReader* reader, const QString& d4,
                             const QString& matName, const char* role)
{
    const QByteArray j = readMat(d4, matName);
    if (j.isEmpty()) return {};
    const QLatin1String want(role);
    // Try every entry with this role, not just the first: D4 lists three detail-normal
    // and three detail-roughness slots (Detail Map 1/2/3). If Detail Map 1's texture is
    // absent/undecodable, the old code returned null and dropped detail entirely — now we
    // fall through to Map 2/3 (and likewise for any multi-slot role) so a single missing
    // texture no longer wipes the whole channel.
    for (const MatTexture& t : parseMaterialJson(j)) {
        if (t.role != want || t.texName.isEmpty()) continue;
        const QImage img = texture(reader, d4, t.texName, t.texSno);
        if (!img.isNull()) return img;
    }
    return {};
}

QImage MaterialDecode::baseColor(CascReader* reader, const QString& d4, const QString& matName)
{
    return byRole(reader, d4, matName, "BASE_COLOR");
}

// (detailComposite removed — the shader tiles detail maps live; nothing baked composites.)

QImage MaterialDecode::normalMap(CascReader* reader, const QString& d4, const QString& matName)
{
    return byRole(reader, d4, matName, "NORMAL");
}

QImage MaterialDecode::orm(CascReader* reader, const QString& d4, const QString& matName)
{
    const QByteArray j = readMat(d4, matName);
    if (j.isEmpty()) return {};
    QString rN, mN, aN; qint64 rS = 0, mS = 0, aS = 0;
    for (const MatTexture& t : parseMaterialJson(j)) {
        if (t.texName.isEmpty()) continue;
        if (rN.isEmpty() && t.role == QLatin1String("ROUGHNESS")) { rN = t.texName; rS = t.texSno; }
        if (mN.isEmpty() && t.role == QLatin1String("METALLIC"))  { mN = t.texName; mS = t.texSno; }
        if (aN.isEmpty() && t.role == QLatin1String("AO"))        { aN = t.texName; aS = t.texSno; }
    }
    const QImage rough = rN.isEmpty() ? QImage() : texture(reader, d4, rN, rS);
    const QImage metal = mN.isEmpty() ? QImage() : texture(reader, d4, mN, mS);
    const QImage ao    = aN.isEmpty() ? QImage() : texture(reader, d4, aN, aS);
    if (rough.isNull() && metal.isNull() && ao.isNull()) return {};
    int w = 1, h = 1;
    for (const QImage* im : {&rough, &metal, &ao})
        if (!im->isNull()) { w = qMax(w, im->width()); h = qMax(h, im->height()); }
    auto chan = [&](const QImage& im, int def) -> QImage {
        QImage out(w, h, QImage::Format_RGBA8888);
        if (im.isNull()) { out.fill(QColor(def, def, def)); return out; }
        return im.convertToFormat(QImage::Format_RGBA8888)
                 .scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    };
    const QImage R = chan(ao, 255), G = chan(rough, 153), B = chan(metal, 0);
    QImage out(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) {
        uchar* d = out.scanLine(y);
        const uchar* pr = R.constScanLine(y);
        const uchar* pg = G.constScanLine(y);
        const uchar* pb = B.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            d[x * 4 + 0] = pr[x * 4]; d[x * 4 + 1] = pg[x * 4];
            d[x * 4 + 2] = pb[x * 4]; d[x * 4 + 3] = 255;
        }
    }
    return out;
}

void MaterialDecode::detailMapsSeparate(CascReader* reader, const QString& d4, const QString& matName,
                                        const float nInt[3], const float rInt[3], const float rOff[3],
                                        QVector<QImage>& outNormals, QVector<QImage>& outRoughs,
                                        float& outNStrength, float& outRStrength, float& outROffset,
                                        float outScales[3], int& outMetalLayer)
{
    outNormals = QVector<QImage>(3);
    outRoughs  = QVector<QImage>(3);
    outNStrength = 0.0f; outRStrength = 0.0f; outROffset = 0.0f;
    outMetalLayer = -1;                                  // which detail map is a metal library map (by name)
    outScales[0] = outScales[1] = outScales[2] = 8.0f;   // sensible default if a map lacks anim
    const QByteArray j = readMat(d4, matName);
    if (j.isEmpty()) return;

    QString nName[3], rName[3];
    qint64  nSno[3] = {0, 0, 0}, rSno[3] = {0, 0, 0};
    for (const MatTexture& t : parseMaterialJson(j)) {
        if (t.texName.isEmpty()) continue;
        switch (t.slot) {
            // Detail-NORMAL slots also carry the per-map tiling scale (the rough map matches).
            case 212: nName[0] = t.texName; nSno[0] = t.texSno; outScales[0] = t.uScale; break;
            case 213: nName[1] = t.texName; nSno[1] = t.texSno; outScales[1] = t.uScale; break;
            case 214: nName[2] = t.texName; nSno[2] = t.texSno; outScales[2] = t.uScale; break;
            case 218: rName[0] = t.texName; rSno[0] = t.texSno; break;
            case 219: rName[1] = t.texName; rSno[1] = t.texSno; break;
            case 220: rName[2] = t.texName; rSno[2] = t.texSno; break;
            default: break;
        }
    }
    // Identify which detail slot is a METAL library map by its texture name. The dye mask segments
    // dyeable regions, not material types, so metal and leather can share a zone; metalness is the
    // real metal/non-metal signal. We route metal texels to this map instead of a leather one.
    // D4's detail library names metals many ways (Metal_*, Brass/Bronze/Iron/Steel/Gold/Silver/
    // Copper/Chrome, Chainmail, Corroded, Rust…) — match any of them so the metal map auto-detects
    // without a per-item override.
    static const char* kMetalTokens[] = {
        "Metal", "Brass", "Bronze", "Iron", "Steel", "Gold", "Silver", "Copper", "Chrome",
        "Chainmail", "Chainlink", "Corrod", "Rust", "Gilded", "Platemail"
    };
    for (int i = 0; i < 3 && outMetalLayer < 0; ++i) {
        if (nName[i].isEmpty()) continue;
        for (const char* tok : kMetalTokens)
            if (nName[i].contains(QLatin1String(tok), Qt::CaseInsensitive)) { outMetalLayer = i; break; }
    }

    int nc = 0, rc = 0;
    for (int i = 0; i < 3; ++i) {
        if (!nName[i].isEmpty()) {
            const QImage im = texture(reader, d4, nName[i], nSno[i]);
            if (!im.isNull()) { outNormals[i] = im; outNStrength += qMax(0.0f, nInt[i]); ++nc; }
        }
        if (!rName[i].isEmpty()) {
            const QImage im = texture(reader, d4, rName[i], rSno[i]);
            if (!im.isNull()) { outRoughs[i] = im; outRStrength += qMax(0.0f, rInt[i]); outROffset += rOff[i]; ++rc; }
        }
    }
    if (nc > 0) outNStrength /= float(nc);
    if (rc > 0) outRStrength /= float(rc);
}

void MaterialDecode::factors(CascReader*, const QString& d4, const QString& matName,
                             float& metal, float& rough)
{
    metal = 0.0f; rough = 0.6f;
    const QByteArray j = readMat(d4, matName);
    if (j.isEmpty()) return;
    const MaterialValues v = parseMaterialValues(j);
    if (v.hasMetal) metal = float(v.metal);
    if (v.hasRough) rough = float(v.rough);
}

QStringList MaterialDecode::appearanceRosterFromMeta(const QByteArray& meta, const SnoIndex* idx)
{
    QStringList out;
    const QVector<int> snos = AppearanceMatBin::snos(meta);
    if (snos.isEmpty()) return out;
    // sno -> name over both material groups (57 "Material (2)", 37 "Material") plus Cloth (11),
    // since an entry can be cloth-only. Built once per call; the caller does this at most once per
    // piece load, and caching it would outlive a d4data change.
    QHash<int, QString> byId;
    if (idx)
        for (int g : {57, 37, 11})
            for (const SnoEntry& e : idx->entries(g)) byId.insert(e.snoId, e.name);
    out.reserve(snos.size());
    for (int s : snos) out << (s ? byId.value(s) : QString());
    return out;
}

QStringList MaterialDecode::appearanceRoster(const QString& d4, const QString& appName)
{
    QStringList out;
    if (d4.isEmpty() || appName.isEmpty()) return out;
    QFile f(QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4, appName));
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonObject ro = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue& mv : ro.value(QStringLiteral("ptAppearanceMaterials")).toArray()) {
        const QJsonArray soas = mv.toObject().value(QStringLiteral("ptSOAs")).toArray();
        QString name;
        if (!soas.isEmpty()) {
            const QJsonObject s = soas[0].toObject();
            name = s.value(QStringLiteral("snoOverrideMaterial")).toObject().value(QStringLiteral("name")).toString();
            if (name.isEmpty()) name = s.value(QStringLiteral("snoMaterial")).toObject().value(QStringLiteral("name")).toString();
            if (name.isEmpty()) name = s.value(QStringLiteral("snoCloth")).toObject().value(QStringLiteral("name")).toString();
            // Last resort before giving up: a sub-object can reference ONLY the HQ cloth override
            // (spiF_stor198_TRS's chains, PalF_stor157's belt items — 14 in a 1200-piece sample).
            // Those resolved to an empty name, which is what left the part with no material.
            if (name.isEmpty()) name = s.value(QStringLiteral("snoHighQualityClothOverride"))
                                        .toObject().value(QStringLiteral("name")).toString();
        }
        out.append(name);
    }
    return out;
}
