#include "tex/TexMeta.h"

#include "app/AppPaths.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QRegularExpression>
#include <QSize>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>

TexMeta parseTexMetaJson(const QByteArray& json)
{
    TexMeta m;
    if (json.isEmpty())
        return m;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return m;

    const QJsonObject o = doc.object();
    m.eTexFormat = o.value(QStringLiteral("eTexFormat")).toInt();
    m.width      = o.value(QStringLiteral("dwWidth")).toInt();
    m.height     = o.value(QStringLiteral("dwHeight")).toInt();
    m.depth      = o.value(QStringLiteral("dwDepth")).toInt(1);
    m.faceCount  = o.value(QStringLiteral("dwFaceCount")).toInt(1);
    m.mipMin     = o.value(QStringLiteral("dwMipMapLevelMin")).toInt();
    m.mipMax     = o.value(QStringLiteral("dwMipMapLevelMax")).toInt();

    // Atlas frames (optional).
    const QJsonArray frames = o.value(QStringLiteral("ptFrame")).toArray();
    m.frames.reserve(frames.size());
    for (const QJsonValue& fv : frames) {
        const QJsonObject f = fv.toObject();
        TexFrame fr;
        fr.handle = quint64(f.value(QStringLiteral("hImageHandle")).toDouble());
        fr.u0 = float(f.value(QStringLiteral("flU0")).toDouble());
        fr.v0 = float(f.value(QStringLiteral("flV0")).toDouble());
        fr.u1 = float(f.value(QStringLiteral("flU1")).toDouble());
        fr.v1 = float(f.value(QStringLiteral("flV1")).toDouble());
        m.frames.append(fr);
    }

    // serTex[]: per-subresource {offset, size} table — authoritative byte layout for the
    // face/mip slices (needed for cubemaps, where faces are stored consecutively).
    const QJsonArray ser = o.value(QStringLiteral("serTex")).toArray();
    m.subres.reserve(ser.size());
    for (const QJsonValue& sv : ser) {
        const QJsonObject s = sv.toObject();
        TexSubRes sr;
        sr.offset = quint32(s.value(QStringLiteral("dwOffset")).toDouble());
        sr.size   = quint32(s.value(QStringLiteral("dwSizeAndFlags")).toDouble());
        m.subres.append(sr);
    }

    m.valid = m.width > 0 && m.height > 0;
    return m;
}

// ── Texture dimension overrides ──────────────────────────────────────────────
// The CASC stores only headerless pixel payloads for textures; the real width/height live in
// d4data's .tex.json. When that snapshot is stale/missing after a game patch, decoding uses wrong
// dimensions (scrambled/blank). This table supplies the true pixel dimensions so the (correct)
// payload decodes properly — measured dimensions are plain facts, not asset content.
namespace {
struct WH { int w, h; };

const QHash<QString, WH>& builtinDims()
{
    static const QHash<QString, WH> kT = {
        { QStringLiteral("2DInventory_Items_AF_Uniques_001"), { 640, 192 } },
        { QStringLiteral("2DInventory_Items_Barbarian"), { 12600, 368 } },
        { QStringLiteral("2DInventory_Items_Boots_Generic"), { 248, 184 } },
        { QStringLiteral("2DInventory_Items_Druid"), { 11864, 368 } },
        { QStringLiteral("2DInventory_Items_Gloves_Generic"), { 248, 184 } },
        { QStringLiteral("2DInventory_Items_Helms_Generic"), { 184, 1320 } },
        { QStringLiteral("2DInventory_Items_Necromancer"), { 11136, 368 } },
        { QStringLiteral("2DInventory_Items_Rogue"), { 11840, 368 } },
        { QStringLiteral("2DInventory_Items_Sorcerer"), { 11136, 368 } },
        { QStringLiteral("2DInventory_Items_Torso_Generic"), { 248, 3504 } },
        { QStringLiteral("2DInventory_Items_Unique_Barbarian"), { 1864, 184 } },
        { QStringLiteral("2DInventory_Items_Unique_Druid"), { 888, 184 } },
        { QStringLiteral("2DInventory_Items_Unique_Necromancer"), { 1128, 184 } },
        { QStringLiteral("2DInventory_Items_Unique_Paladin"), { 1008, 184 } },
        { QStringLiteral("2DInventory_Items_Unique_Rogue"), { 888, 456 } },
        { QStringLiteral("2DInventory_Items_Unique_Sorcerer"), { 640, 456 } },
        { QStringLiteral("2DInventory_Items_Unique_Spiritborn"), { 856, 416 } },
        { QStringLiteral("2DInventory_Items_Unique_Warlock"), { 1008, 184 } },
        { QStringLiteral("2DInventory_Items_X2"), { 256, 960 } },
        { QStringLiteral("2DInventory_Items_e001_Barbarian"), { 824, 1000 } },
        { QStringLiteral("2DInventory_Items_e001_Druid"), { 1528, 640 } },
        { QStringLiteral("2DInventory_Items_e001_Necromancer"), { 640, 2048 } },
        { QStringLiteral("2DInventory_Items_e001_Rogue"), { 1616, 640 } },
        { QStringLiteral("2DInventory_Items_e001_Sorcerer"), { 152, 6560 } },
        { QStringLiteral("2DInventory_Items_e001_Spiritborn"), { 3664, 1184 } },
        { QStringLiteral("2DInventory_Items_e002_Barbarian"), { 1496, 640 } },
        { QStringLiteral("2DInventory_Items_e002_Druid"), { 976, 776 } },
        { QStringLiteral("2DInventory_Items_e002_Necromancer"), { 608, 1912 } },
        { QStringLiteral("2DInventory_Items_e002_Paladin"), { 488, 8504 } },
        { QStringLiteral("2DInventory_Items_e002_Rogue"), { 1464, 688 } },
        { QStringLiteral("2DInventory_Items_e002_Sorcerer"), { 976, 776 } },
        { QStringLiteral("2DInventory_Items_e002_Spiritborn"), { 152, 6064 } },
        { QStringLiteral("2DInventory_Items_e002_Warlock"), { 3936, 1272 } },
    };
    return kT;
}

// Lazily merge the built-in table with any user extensions, keyed by lowercased texture name.
const QHash<QString, WH>& dimTable()
{
    static QHash<QString, WH> merged;
    static bool built = false;
    static QMutex mtx;
    QMutexLocker lock(&mtx);
    if (built)
        return merged;
    built = true;
    for (auto it = builtinDims().constBegin(); it != builtinDims().constEnd(); ++it)
        merged.insert(it.key().toLower(), it.value());

    const QString base = AppPaths::dataDir();

    // (2) Text file: one "name<TAB or whitespace>W<...>H" per line ('#' = comment).
    QFile f(base + QStringLiteral("/texture_dims_override.txt"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream ts(&f);
        while (!ts.atEnd()) {
            const QString line = ts.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
            const QStringList p = line.split(QRegularExpression(QStringLiteral("[\\t ,]+")), Qt::SkipEmptyParts);
            if (p.size() < 3) continue;
            bool okW = false, okH = false;
            const int w = p[p.size() - 2].toInt(&okW), h = p[p.size() - 1].toInt(&okH);
            if (okW && okH && w > 0 && h > 0)
                merged.insert(QStringList(p.mid(0, p.size() - 2)).join(QLatin1Char(' ')).toLower(), WH{ w, h });
        }
    }

    // (3) PNG folder: filename stem = texture name, image size = real dims. Header-only read.
    const QDir pdir(base + QStringLiteral("/texture_overrides"));
    if (pdir.exists()) {
        const QStringList pngs = pdir.entryList(QStringList{ QStringLiteral("*.png") }, QDir::Files);
        for (const QString& fn : pngs) {
            QImageReader r(pdir.filePath(fn));
            const QSize sz = r.size();
            if (sz.isValid() && sz.width() > 0 && sz.height() > 0)
                merged.insert(QFileInfo(fn).completeBaseName().toLower(), WH{ sz.width(), sz.height() });
        }
    }
    return merged;
}
}  // namespace

bool textureDimOverride(const QString& texName, int& w, int& h)
{
    const auto& t = dimTable();
    const auto it = t.constFind(texName.toLower());
    if (it == t.constEnd())
        return false;
    w = it.value().w;
    h = it.value().h;
    return true;
}

// ── Per-frame icon overrides ─────────────────────────────────────────────────
// Maps (atlasSno, frameIdx) → an exported icon PNG. When a patch re-lays-out an atlas, the ptFrame
// UVs in d4data go stale (or are absent), so cropping the atlas miscrops. d4analyzer's TexFrames
// export produces one PNG per frame named "<name> [<atlasSno>] - <frameIdx> <frameName>.png", so we
// key the folder by (atlasSno, frameIdx) and the icon lookups resolve a handle to its frame index
// via the atlas ptFrame array.
namespace {
quint64 frameKey(int sno, int idx) { return (quint64(quint32(sno)) << 20) | quint32(idx & 0xFFFFF); }

const QHash<quint64, QString>& frameOverrideMap()
{
    static QHash<quint64, QString> m;
    static bool built = false;
    static QMutex mtx;
    QMutexLocker lock(&mtx);
    if (built)
        return m;
    built = true;
    const QDir dir(AppPaths::dataDir() + QStringLiteral("/icon_overrides"));
    if (!dir.exists())
        return m;
    // "…[<atlasSno>]…-<frameIdx>…" — d4analyzer's "{{FileName}} [{{SNO}}] - {{FrameIdx}} {{FrameName}}".
    static const QRegularExpression rx(QStringLiteral("\\[(\\d+)\\]\\s*-\\s*(\\d+)"));
    for (const QString& fn : dir.entryList(QStringList{ QStringLiteral("*.png") }, QDir::Files)) {
        const QRegularExpressionMatch mt = rx.match(fn);
        if (!mt.hasMatch()) continue;
        const int sno = mt.captured(1).toInt();
        const int idx = mt.captured(2).toInt();
        if (sno > 0 && idx >= 0) m.insert(frameKey(sno, idx), dir.filePath(fn));
    }
    return m;
}
}  // namespace

QImage frameIconOverride(int atlasSno, int frameIdx)
{
    if (atlasSno <= 0 || frameIdx < 0)
        return {};
    const auto& m = frameOverrideMap();
    const auto it = m.constFind(frameKey(atlasSno, frameIdx));
    if (it == m.constEnd())
        return {};
    return QImage(it.value());
}

int frameOverrideCount(int atlasSno)
{
    if (atlasSno <= 0)
        return 0;
    const auto& m = frameOverrideMap();
    int maxIdx = -1;
    for (int i = 0; ; ++i) {                 // contiguous from 0; stop at the first gap
        if (!m.contains(frameKey(atlasSno, i))) break;
        maxIdx = i;
    }
    return maxIdx + 1;
}
