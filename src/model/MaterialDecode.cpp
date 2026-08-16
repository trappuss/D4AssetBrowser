#include "model/MaterialDecode.h"

#include "index/SnoIndex.h"
#include "model/AppearanceMatBin.h"
#include "tex/TextureDefTable.h"

#include "casc/CascReader.h"
#include "model/Material.h"
#include "tex/BcDecode.h"
#include "tex/TexMeta.h"

#include <QByteArray>
#include <QFile>
#include <QMutex>
#include <QSet>
#include <QStringList>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <QCache>
#include <algorithm>   // std::sort — detectDyeBands' histogram clustering
#include <cmath>

namespace {

// Injected by MainWindow once the SNO index is up. Inside the anonymous namespace so it stays
// private to this TU; the two MaterialDecode:: functions below cannot be, because a qualified
// name from another namespace may not be DEFINED inside an unnamed one (C2888).
//
// MUTEX, not a bare global: the setter runs on reload()'s worker thread while snoForMaterial is
// called from the GUI thread (model load) and from the texture/wardrobe workers. Assigning a
// std::function while another thread copies or invokes it races on a heap-owning object — a crash,
// not a stale answer. Reloading from Settings while a model loads is enough to hit it. The reader
// takes a COPY under the lock and invokes it unlocked, so a slow resolver never blocks the setter.
QMutex g_resolverMx;
std::function<qint64(const QString&)> g_nameResolver;

// ── Run-scoped decode cache (MaterialDecode::TextureCacheScope) ──────────────────────────────
// SHARED, mutex-guarded. See the header for why this is not thread_local: exportModels() runs its
// items on a std::thread pool, so a per-thread cache would share nothing with the workers that are
// decoding the same textures at the same moment.
//
// Null when no scope is open, which is the state every non-export caller stays in — so the viewport
// and the panels pay nothing for this, not even the lock.
QMutex g_texMx;
QCache<QString, QImage>* g_texCache = nullptr;   // guarded by g_texMx
int    g_texCacheDepth = 0;                      // nesting refcount; only the outermost frees
qint64 g_texHits = 0, g_texMisses = 0, g_texSaved = 0;   // guarded by g_texMx

// Which threads take part. The cache itself has to be shared — the export pool needs it — but
// "shared" must not mean "ambient": a Bulk Extract runs off the GUI thread, so without this every
// Wardrobe panel fill and Catalogue thumbnail decoded while a run is in flight would be served
// from, and inserted into, the export's cache. That evicts the export's working set for browsing
// the user is doing anyway, and — worse — contaminates the very counters the change is judged by,
// crediting the export with reuse it never had.
//
// So a thread opts in by constructing a scope. exportModels() constructs one, and each of its pool
// workers constructs a nested one; nobody else does. thread_local, so testing it is free and needs
// no lock.
thread_local int g_texLocalDepth = 0;

// Kill-switch, read once. Lets a build measure itself with the cache off — see the header. Env-gated
// and permanent, like D4_DUMP_CLOTH and the rest: a performance claim nobody can re-check on their
// own machine and their own data is not a measurement, it is an assertion.
bool texCacheDisabled()
{
    static const bool off = !qEnvironmentVariableIsEmpty("D4_NO_TEXCACHE");
    return off;
}

// Budget override in MB, so the ceiling can be tuned or shrunk on a memory-tight machine without a
// rebuild and without another settings checkbox. Unset = the caller's default.
int texCacheBudgetOverrideMB()
{
    static const int mb = qEnvironmentVariableIntValue("D4_TEXCACHE_MB");
    return mb;
}

// Keyed on everything the decode depends on: the sno supplies the pixels, but the dimensions and
// format come from <d4>/…/<texName>.tex.json when that exists and from the CASC texture tables when
// it does not — and those two can disagree when the d4data snapshot predates the installed game.
// Keying on the sno alone would let a name-resolved decode and a table-resolved one collide. A
// string compare costs nothing next to a BC decode.
inline QString texCacheKey(const QString& d4, const QString& texName, qint64 texSno)
{
    return QString::number(texSno) + QLatin1Char('|') + texName + QLatin1Char('|') + d4;
}

// An encrypted material has no name — SnoIndex gives it "~unnamed_<sno>". That string still carries
// the only handle we need, so the sno is recovered from it rather than threading a second parameter
// through every MaterialDecode entry point.
int snoFromPlaceholder(const QString& matName)
{
    static const QLatin1String kPfx("~unnamed_");
    if (!matName.startsWith(kPfx)) return 0;
    bool ok = false;
    const int v = matName.mid(kPfx.size()).toInt(&ok);
    return ok ? v : 0;
}


// D4_DUMP_MAT=1 turns on the material-resolution trace (mat-resolve / mat-meta). Read ONCE into a
// static: it is consulted on paths that run 100k+ times per launch, and qEnvironmentVariableIsSet
// hits the process environment on every call.
bool dumpMat()
{
    static const bool on = qEnvironmentVariableIsSet("D4_DUMP_MAT");
    return on;
}

// ptMatTexList straight out of the material meta binary, for materials with no .mat.json.
// Layout derived from raw blobs and verified against armor_skin_mat's JSON exactly:
//   meta +0x38 : u32 dataOffset, u32 byteSize    (records are 48 bytes, count = byteSize / 48)
//   record +16 : eShaderTex   (the role slot - 1, 3, 62, 81 in the control, matching the JSON)
//   record +24 : snoTex       (0xFFFFFFFF for an unused slot)
QVector<MatTexture> matTexFromMeta(CascReader* reader, int matSno)
{
    QVector<MatTexture> out;
    if (!reader || matSno <= 0) return out;
    const QByteArray b = reader->readMetaBySno(quint64(matSno));
    if (b.size() < 0x40) return out;
    auto u32 = [&b](int o) -> quint32 {
        if (o < 0 || o + 4 > b.size()) return 0;
        return quint32(uchar(b[o])) | quint32(uchar(b[o + 1])) << 8
             | quint32(uchar(b[o + 2])) << 16 | quint32(uchar(b[o + 3])) << 24;
    };
    const int off = int(u32(0x38)), bytes = int(u32(0x3C));
    if (off <= 0 || bytes <= 0 || bytes % 48 != 0) return out;
    const int n = bytes / 48;
    if (off + n * 48 > b.size()) return out;
    for (int i = 0; i < n; ++i) {
        const int rec = off + i * 48;
        const quint32 sno = u32(rec + 24);
        if (sno == 0 || sno == 0xFFFFFFFFu) continue;   // unused slot
        MatTexture t;
        t.slot   = int(u32(rec + 16));
        t.role   = shaderSlotRole(t.slot);
        t.texSno = qint64(sno);
        // texName stays empty on purpose: an encrypted texture has none, and
        // MaterialDecode::texture no longer requires one.
        out.push_back(t);
    }

    // Say EXACTLY what this returned, once per material. This is the only place the texture SNOs
    // for a material with no .mat.json are decided, and when the model came out untextured there
    // was no way to tell whether the list was wrong or the decode was.
    //
    // What prompted it: DruM_stor235_HLM (appearance 2475263, a key we hold) reported failures on
    // snos 1551475/1551476, and the game's own EncryptedSNOs manifest lists both as group 9
    // APPEARANCES, not group 44 textures. Something is putting non-texture SNOs into a texture
    // list. Until this line says which, any fix is a guess.
    //
    // Rate-limited per material sno because it is called from the model, wardrobe and stable
    // workers; the set is mutex-guarded for the same reason. Env-gated for the same reason as
    // mat-resolve above — a corpus sweep must not pay for a debugging aid.
    if (dumpMat()) {
        static QMutex logMx;
        static QSet<int> logged;
        bool first = false;
        { QMutexLocker l(&logMx); first = !logged.contains(matSno); if (first) logged.insert(matSno); }
        if (first) {
            QStringList parts;
            for (const MatTexture& t : out)
                parts << QStringLiteral("slot %1 -> sno %2").arg(t.slot).arg(t.texSno);
            qInfo("mat-meta: material sno %d yielded %d texture(s) from %d record(s) at +%d: %s",
                  matSno, int(out.size()), n, off,
                  parts.isEmpty() ? "(none)" : qPrintable(parts.join(QStringLiteral(" | "))));
        }
    }
    return out;
}

QByteArray readMat(const QString& d4, const QString& matName)
{
    if (matName.isEmpty() || d4.isEmpty()) return {};
    QFile f(QStringLiteral("%1/json/base/meta/Material/%2.mat.json").arg(d4, matName));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Read an authored scalar from a material's ptRunTimeMaterialValues (real game data, e.g.
// "Normal Intensity - Detail Map 1"). Matches by name prefix; returns the fallback when absent.
//
// Takes the ALREADY-PARSED tUberMaterial object, not the raw bytes: bakeDetailForMaterial wants
// nine of these, and a .mat.json is a multi-KB document — re-running QJsonDocument::fromJson per
// scalar makes the parse, not the file read, the cost. WardrobeTab2's fxScalar() re-reads per call
// by design (it pulls one scalar at a time from its own decode pass) and is left alone.
float matScalar(const QJsonObject& um, const char* valueName, float fallback)
{
    const QLatin1String want(valueName);
    for (const QJsonValue& rv : um.value(QStringLiteral("ptRunTimeMaterialValues")).toArray())
        for (const QJsonValue& sv : rv.toObject().value(QStringLiteral("arMaterialScalarValues")).toArray()) {
            const QJsonObject tv = sv.toObject().value(QStringLiteral("tValue")).toObject();
            if (tv.value(QStringLiteral("snoMaterialValue")).toObject()
                  .value(QStringLiteral("name")).toString().startsWith(want, Qt::CaseInsensitive))
                return float(tv.value(QStringLiteral("value")).toDouble());
        }
    return fallback;
}
}  // namespace

MaterialDecode::TextureCacheScope::TextureCacheScope(int budgetMB)
{
    if (texCacheDisabled()) return;   // inert scope: no participation, no allocation, no counters
    m_active = true;
    ++g_texLocalDepth;   // this thread now participates (thread_local, no lock needed)
    QMutexLocker lock(&g_texMx);
    const int over = texCacheBudgetOverrideMB();
    const int budgetKB = qMax(16, over > 0 ? over : budgetMB) * 1024;
    // Allocate BEFORE taking the reference, so a throw here leaves the depth untouched rather than
    // stranding it at 1 with a null cache — which would silently disable caching for good.
    if (!g_texCache) g_texCache = new QCache<QString, QImage>(budgetKB);
    // An overlapping scope asking for more room gets it, rather than silently inheriting whichever
    // budget happened to be requested first.
    else if (g_texCache->maxCost() < budgetKB) g_texCache->setMaxCost(budgetKB);
    ++g_texCacheDepth;
    m_hits0 = g_texHits; m_misses0 = g_texMisses; m_saved0 = g_texSaved;
}

MaterialDecode::TextureCacheScope::~TextureCacheScope()
{
    if (!m_active) return;   // never took part; nothing to unwind
    --g_texLocalDepth;
    QMutexLocker lock(&g_texMx);
    if (--g_texCacheDepth == 0) { delete g_texCache; g_texCache = nullptr; }
}

qint64 MaterialDecode::TextureCacheScope::hits() const
{ QMutexLocker lock(&g_texMx); return g_texHits - m_hits0; }
qint64 MaterialDecode::TextureCacheScope::misses() const
{ QMutexLocker lock(&g_texMx); return g_texMisses - m_misses0; }
qint64 MaterialDecode::TextureCacheScope::bytesSaved() const
{ QMutexLocker lock(&g_texMx); return g_texSaved - m_saved0; }

void MaterialDecode::setNameResolver(std::function<qint64(const QString&)> fn)
{
    QMutexLocker lock(&g_resolverMx);
    g_nameResolver = std::move(fn);
}

qint64 MaterialDecode::snoForMaterial(const QString& matName)
{
    if (matName.isEmpty()) return 0;
    // Encrypted materials arrive as "~unnamed_<sno>" and carry their SNO in the name.
    if (const int ph = snoFromPlaceholder(matName)) return ph;
    // Named material: ask the live game index — the path that reaches anything the d4data
    // snapshot has no .mat.json for. Copy under the lock, call outside it.
    std::function<qint64(const QString&)> fn;
    {
        QMutexLocker lock(&g_resolverMx);
        fn = g_nameResolver;
    }
    const qint64 sno = fn ? fn(matName) : 0;
    // The other place a wrong SNO could enter the texture path: if this name resolves to something
    // that is not a Material, everything downstream reads the wrong meta blob and the "texture"
    // SNOs it extracts are whatever those bytes happen to be.
    //
    // ENV-GATED, and it has to be. The first version logged once per material NAME with no gate —
    // but the startup material-name sweep resolves 106,266 of them, so that was 106k mutex-guarded
    // set inserts and 106k disk-FLUSHED log lines during startup. A diagnostic must never be able
    // to cost more than the thing it is diagnosing.
    if (dumpMat()) {
        static QMutex logMx;
        static QSet<QString> logged;
        bool first = false;
        { QMutexLocker l(&logMx); first = !logged.contains(matName); if (first) logged.insert(matName); }
        if (first)
            qInfo("mat-resolve: '%s' -> sno %lld", qPrintable(matName), sno);
    }
    return sno;
}

QImage MaterialDecode::texture(CascReader* reader, const QString& d4,
                               const QString& texName, qint64 texSno)
{
    // texName may legitimately be empty now: an encrypted texture has no name, only a sno. The
    // JSON route needs one, the binary route does not, so the name is no longer a precondition.
    if (!reader || texSno <= 0) return {};
    // Served from the run-scoped cache when one is open (see TextureCacheScope). The copy is made
    // while the lock is held — QImage is implicitly shared so it is a refcount bump, but it has to
    // happen before another worker can evict the entry out from under the pointer.
    QString cacheKey;
    if (g_texLocalDepth > 0) {
        QMutexLocker lock(&g_texMx);
        if (g_texCache) {
            cacheKey = texCacheKey(d4, texName, texSno);
            if (const QImage* hit = g_texCache->object(cacheKey)) {
                ++g_texHits;
                g_texSaved += hit->sizeInBytes();
                return *hit;
            }
            ++g_texMisses;
        }
    }
    TexMeta meta;
    if (!texName.isEmpty() && !d4.isEmpty()) {
        QFile f(QStringLiteral("%1/json/base/meta/Texture/%2.tex.json").arg(d4, texName));
        if (f.open(QIODevice::ReadOnly)) meta = parseTexMetaJson(f.readAll());
    }
    if (!meta.valid) {
        // No .tex.json — every encrypted texture, and any asset newer than the d4data snapshot.
        // Dimensions come from CASC's bulk texture tables instead. See TextureDefTable for the
        // derived layout; the overlay holding a given encrypted texture is gated by the same TACT
        // key as its pixels, so if the payload decodes the definition does too.
        TextureDefTable::instance().ensureBuilt(reader);
        // Describe the payload we will ACTUALLY read, not the one this sno nominally owns. 36,930
        // assets carry no payload and share another sno's; for those the destination's definition
        // is the one that matches the bytes. Using the source's dimensions on a redirected payload
        // decodes garbage — or, once BcDecode's size check rejects it, nothing at all.
        const qint64 defSno = qint64(reader->payloadSourceSno(quint64(texSno)));
        const TextureDefTable::Def d = TextureDefTable::instance().lookup(int(defSno));
        if (d.valid()) {
            meta.width = d.width;
            meta.height = d.height;
            meta.eTexFormat = d.format;
            meta.valid = true;
        }
    }
    // Both routes have failed by here — JSON and the bulk texture tables — so this is the most
    // diagnostically useful moment in the whole texture pipeline, and it was silent. Rate-limited
    // per sno: a model with 20 parts sharing one bad texture should say it once.
    // MUTEX-GUARDED. This function is reached from the GUI thread (panel fills, viewport) AND from
    // the detached Bulk Extract worker, so a bare static QSet::insert here is a heap-corrupting
    // race — not a lost log line. The dumpMat() diagnostics below were written with a lock; these
    // two predate them and were missed.
    static QMutex warnMx;
    static QSet<qint64> warnedMeta, warnedPayload;
    auto firstTime = [](QSet<qint64>& set, qint64 sno) {
        QMutexLocker l(&warnMx);
        if (set.contains(sno)) return false;
        set.insert(sno);
        return true;
    };
    if (!meta.valid) {
        if (firstTime(warnedMeta, texSno)) {
            // Same reasoning as the payload branch below: say WHICH of the two tables should have
            // had it. An encrypted texture's definition lives in the per-key overlay, so a miss
            // here on a key we lack is expected, and a miss on a key we hold is a real gap.
            const QHash<int, QByteArray>& enc = reader->encryptedSnos();
            const auto hit = enc.constFind(int(texSno));
            const char* src = (hit == enc.constEnd())
                ? "unencrypted, so the global texture table should have carried it"
                : (reader->haveTactKey(hit.value())
                       ? "encrypted under a key we hold, so its per-key overlay table should have "
                         "carried it"
                       : "encrypted under a key we do NOT hold, so its overlay table is unreadable "
                         "- expected");
            qWarning("texture sno %lld: no dimensions from .tex.json or the bulk tables (%s) - "
                     "parts using it render untextured", qint64(texSno), src);
        }
        return {};
    }
    const QByteArray payload = reader->readPayloadBySno(quint64(texSno));
    if (payload.isEmpty()) {
        // Distinct from the above: we KNOW its size and format, the pixels are simply not here.
        //
        // The old message guessed — "usually a TACT key we do not hold" — and the guess was wrong
        // for 54 of the 57 textures in the first log this was measured on: those are not encrypted
        // at all. Guessing here cost real time chasing a decryption bug that was not one, so the
        // reason is now DERIVED from the three things that actually distinguish the cases: whether
        // the game lists the sno as encrypted, whether we hold that key, and whether the payload
        // exists in this install's path table at all.
        if (firstTime(warnedPayload, texSno)) {
            const QHash<int, QByteArray>& enc = reader->encryptedSnos();
            const auto hit = enc.constFind(int(texSno));
            const CascReader::PayloadVariants pv = reader->payloadVariants(quint64(texSno));
            const bool installed = (pv.payload > 0 || pv.paylow > 0);
            QString why;
            if (hit == enc.constEnd()) {
                why = installed
                    ? QStringLiteral("NOT encrypted and the payload IS in CASC, yet the read came "
                                     "back empty - this is a reader bug, not missing content")
                    : QStringLiteral("NOT encrypted, no payload of its own, and no shared-payload "
                                     "redirect either - genuinely absent from this install");
            } else if (!reader->haveTactKey(hit.value())) {
                why = QStringLiteral("LOCKED - encrypted with TACT key %1, which we do not hold%2")
                          .arg(QString::fromLatin1(hit.value().toHex()),
                               installed ? QStringLiteral(" (the pixels ARE installed; only the key "
                                                          "is missing)")
                                         : QString());
            } else {
                why = installed
                    ? QStringLiteral("encrypted with key %1 which we DO hold and the payload is "
                                     "installed - decryption itself failed, which is a bug")
                          .arg(QString::fromLatin1(hit.value().toHex()))
                    : QStringLiteral("encrypted with key %1 which we DO hold, but no payload is "
                                     "installed for it")
                          .arg(QString::fromLatin1(hit.value().toHex()));
            }
            qWarning("texture sno %lld: %dx%d fmt %d resolved but not decodable — %s",
                     qint64(texSno), meta.width, meta.height, meta.eTexFormat, qPrintable(why));
        }
        return {};
    }
    QImage out = BcDecode::decode(payload, meta.width, meta.height, meta.eTexFormat);
    // Only successful decodes are stored. Caching a null would turn one transient read failure
    // into a whole run's worth of missing textures, with nothing in the log to explain it.
    if (!cacheKey.isEmpty() && !out.isNull()) {
        QMutexLocker lock(&g_texMx);
        // Re-check: the scope can have closed while this decode was running.
        if (g_texCache)
            g_texCache->insert(cacheKey, new QImage(out), qMax(1, int(out.sizeInBytes() / 1024)));
    }
    return out;
}

QVector<MatTexture> MaterialDecode::texturesFor(CascReader* reader, const QString& d4,
                                                const QString& matName)
{
    const QByteArray j = readMat(d4, matName);
    return j.isEmpty() ? matTexFromMeta(reader, int(snoForMaterial(matName)))
                       : parseMaterialJson(j);
}

QVector<qint64> MaterialDecode::textureSnosFor(CascReader* reader, const QString& d4,
                                               const QString& matName)
{
    const QByteArray j = readMat(d4, matName);
    const QVector<MatTexture> texList = j.isEmpty()
        ? matTexFromMeta(reader, int(snoForMaterial(matName)))
        : parseMaterialJson(j);
    QVector<qint64> out;
    out.reserve(texList.size());
    for (const MatTexture& t : texList)
        if (t.texSno > 0) out.push_back(t.texSno);
    return out;
}

QImage MaterialDecode::byRole(CascReader* reader, const QString& d4,
                             const QString& matName, const char* role)
{
    // JSON first; binary when there is none (encrypted materials, and anything newer than the
    // d4data snapshot). Both produce the same MatTexture list, so everything downstream is shared.
    const QByteArray j = readMat(d4, matName);
    const QVector<MatTexture> texList = j.isEmpty()
        ? matTexFromMeta(reader, int(snoForMaterial(matName)))
        : parseMaterialJson(j);
    if (texList.isEmpty()) return {};
    const QLatin1String want(role);
    // Try every entry with this role, not just the first: D4 lists three detail-normal
    // and three detail-roughness slots (Detail Map 1/2/3). If Detail Map 1's texture is
    // absent/undecodable, the old code returned null and dropped detail entirely — now we
    // fall through to Map 2/3 (and likewise for any multi-slot role) so a single missing
    // texture no longer wipes the whole channel.
    for (const MatTexture& t : texList) {
        // Not gated on texName: encrypted textures have none, and texture() no longer needs one.
        if (t.role != want || t.texSno <= 0) continue;
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
    // JSON first; binary when there is none (encrypted materials, and anything newer than the
    // d4data snapshot). Both produce the same MatTexture list, so everything downstream is shared.
    const QByteArray j = readMat(d4, matName);
    const QVector<MatTexture> texList = j.isEmpty()
        ? matTexFromMeta(reader, int(snoForMaterial(matName)))
        : parseMaterialJson(j);
    if (texList.isEmpty()) return {};
    QString rN, mN, aN; qint64 rS = 0, mS = 0, aS = 0;
    for (const MatTexture& t : texList) {
        if (t.texSno <= 0) continue;   // name is absent for encrypted textures; the sno is not
        // "already picked?" keyed on the SNO. An encrypted texture's name is always empty, so an
        // rN.isEmpty() test never latches: every later ROUGHNESS overwrote the first, and the
        // decode below was skipped outright because the name it tested was blank. Net effect —
        // encrypted pieces rendered with NO roughness, metal or AO in the Wardrobe and Stable
        // viewports while the Models tab (which has its own, already-fixed copy of this pick)
        // looked correct, and the material panels listed all three maps.
        if (rS <= 0 && t.role == QLatin1String("ROUGHNESS")) { rN = t.texName; rS = t.texSno; }
        if (mS <= 0 && t.role == QLatin1String("METALLIC"))  { mN = t.texName; mS = t.texSno; }
        if (aS <= 0 && t.role == QLatin1String("AO"))        { aN = t.texName; aS = t.texSno; }
    }
    // Gate on the SNO — texture() has not needed a name since the encrypted-texture work.
    const QImage rough = rS <= 0 ? QImage() : texture(reader, d4, rN, rS);
    const QImage metal = mS <= 0 ? QImage() : texture(reader, d4, mN, mS);
    const QImage ao    = aS <= 0 ? QImage() : texture(reader, d4, aN, aS);
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
    const QVector<MatTexture> texList = j.isEmpty()
        ? matTexFromMeta(reader, int(snoForMaterial(matName)))
        : parseMaterialJson(j);
    if (texList.isEmpty()) return;

    QString nName[3], rName[3];
    qint64  nSno[3] = {0, 0, 0}, rSno[3] = {0, 0, 0};
    for (const MatTexture& t : texList) {
        if (t.texSno <= 0) continue;   // name is absent for encrypted textures; the sno is not
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
    // Still by NAME, and that is correct: this asks "is this a metal library map", which only the
    // name can answer. An encrypted map has none, so it simply never claims the metal slot — the
    // shader's metalness fallback covers that case.
    for (int i = 0; i < 3 && outMetalLayer < 0; ++i) {
        if (nName[i].isEmpty()) continue;
        for (const char* tok : kMetalTokens)
            if (nName[i].contains(QLatin1String(tok), Qt::CaseInsensitive)) { outMetalLayer = i; break; }
    }

    // Gated on the SNO, not the name — the same correction orm() and byRole() already carry. An
    // ENCRYPTED texture has no name and a perfectly good sno, and texture() has not needed a name
    // since that work, so testing the name here meant every encrypted material returned no detail
    // maps at all: no grain in the viewport and, once bakeDetail reached the export paths, none in
    // the file either. d4data ships ~1079 nameless materials, plus everything newer than the
    // snapshot, so this was not an edge case.
    int nc = 0, rc = 0;
    for (int i = 0; i < 3; ++i) {
        if (nSno[i] > 0) {
            const QImage im = texture(reader, d4, nName[i], nSno[i]);
            if (!im.isNull()) { outNormals[i] = im; outNStrength += qMax(0.0f, nInt[i]); ++nc; }
        }
        if (rSno[i] > 0) {
            const QImage im = texture(reader, d4, rName[i], rSno[i]);
            if (!im.isNull()) { outRoughs[i] = im; outRStrength += qMax(0.0f, rInt[i]); outROffset += rOff[i]; ++rc; }
        }
    }
    if (nc > 0) outNStrength /= float(nc);
    if (rc > 0) outRStrength /= float(rc);
}

// ── Detail compositing for EXPORT (moved verbatim from WardrobeTab2.cpp) ─────────────────────
// See MaterialDecode.h for what each of these does and why they now live here. They were file-local
// to the Wardrobe tab, which is the only reason `export/bakeDetail` ever worked for Wardrobe exports
// and was silently ignored by the Models tab, Stable and Bulk Extract.

QVector4D MaterialDecode::detectDyeBands(const QImage& dyeMaskIn)
{
    QVector4D def(0.063f, 0.345f, 0.596f, 0.831f);   // shipped fallback when there's no usable mask
    if (dyeMaskIn.isNull()) return def;
    const QImage m = dyeMaskIn.convertToFormat(QImage::Format_RGBA8888);
    const int W = m.width(), H = m.height();
    if (W < 2 || H < 2) return def;
    long hist[256] = {0}; long total = 0;
    const int sx = qMax(1, W / 256), sy = qMax(1, H / 256);   // sparse sample: fast, plenty accurate
    for (int y = 0; y < H; y += sy) {
        const uchar* s = m.constScanLine(y);
        for (int x = 0; x < W; x += sx) { ++hist[s[x * 4]]; ++total; }
    }
    if (total < 16) return def;
    // Merge adjacent populated bins (gap < 6) into clusters; a bin counts if it holds >0.4% of samples.
    const long minPop = qMax(2L, long(total / 250));
    struct Cl { double sum = 0; long pop = 0; };
    QVector<Cl> clusters;
    int lastBin = -100;
    for (int b = 0; b < 256; ++b) {
        if (hist[b] < minPop) continue;
        if (b - lastBin > 6 || clusters.isEmpty()) clusters.append(Cl{});
        Cl& c = clusters.last(); c.sum += double(b) * hist[b]; c.pop += hist[b];
        lastBin = b;
    }
    if (clusters.isEmpty()) return def;
    std::sort(clusters.begin(), clusters.end(), [](const Cl& a, const Cl& b) { return a.pop > b.pop; });
    if (clusters.size() > 4) clusters.resize(4);              // keep the four most-populated zones
    QVector<float> centres;
    for (const Cl& c : clusters) centres.append(float(c.sum / double(c.pop) / 255.0));
    std::sort(centres.begin(), centres.end());
    while (centres.size() < 4) centres.append(centres.last());
    return QVector4D(centres[0], centres[1], centres[2], centres[3]);
}

QVector4D MaterialDecode::deriveZoneMap(const QVector<QImage>& detailN, int metalLayer)
{
    QVector<int> nm;                                          // present NON-metal detail-map slot indices
    for (int i = 0; i < detailN.size() && i < 3; ++i)
        if (!detailN[i].isNull() && i != metalLayer) nm.append(i);
    auto pick = [&](int zoneIdx) -> int {                    // zoneIdx 1..3 → non-metal map (clamped)
        if (nm.isEmpty()) return -1;
        return nm[qMin(zoneIdx - 1, nm.size() - 1)];
    };
    return QVector4D(float(-1), float(pick(1)), float(pick(2)), float(pick(3)));
}

void MaterialDecode::bakeDetail(QImage& normal, QImage& orm, const QImage& dyeMask,
                                const QImage detN[3], const QImage detR[3], const QVector3D& scale,
                                const QVector4D& zoneMap, const QVector4D& bands, int metalLayer,
                                float nInt, float rInt, float rOff)
{
    if (normal.isNull()) return;
    normal = normal.convertToFormat(QImage::Format_RGBA8888);
    const int W = normal.width(), H = normal.height();
    const bool hasOrm = !orm.isNull();
    if (hasOrm) {   // ORM is indexed with the NORMAL's x/y below, so it MUST match the normal's size
        orm = orm.convertToFormat(QImage::Format_RGBA8888);
        // convertToFormat AFTER the scale, not just before it. Qt's smooth scaler has no
        // Format_RGBA8888 path: it converts to ARGB32_Premultiplied and returns it in that format,
        // undoing the line above. The byte order then flips RGBA→BGRA, so the metalness read below
        // (op[x*4+2]) picks up OCCLUSION instead — near-white on most maps, so every texel looked
        // metallic. On a leather or fabric material, which has no metal layer, that meant metalMask
        // = 0 and the whole bake silently did nothing; with a metal layer it painted brushed metal
        // over cloth. Only bit when a material's normal and ORM decoded at different sizes, which
        // is why it survived so long.
        if (orm.size() != normal.size())
            orm = orm.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                     .convertToFormat(QImage::Format_RGBA8888);
    }
    QImage mask = dyeMask.isNull() ? QImage() : dyeMask.convertToFormat(QImage::Format_RGBA8888);
    if (!mask.isNull() && mask.size() != normal.size())
        mask = mask.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                   .convertToFormat(QImage::Format_RGBA8888);   // same trap: mp[x*4] would read blue
    QImage dN[3], dR[3];
    for (int k = 0; k < 3; ++k) {
        if (!detN[k].isNull()) dN[k] = detN[k].convertToFormat(QImage::Format_RGBA8888);
        if (!detR[k].isNull()) dR[k] = detR[k].convertToFormat(QImage::Format_RGBA8888);
    }
    const float bnd[4] = { float(bands.x()), float(bands.y()), float(bands.z()), float(bands.w()) };
    const int   zmap[4] = { int(zoneMap.x()), int(zoneMap.y()), int(zoneMap.z()), int(zoneMap.w()) };
    const float sc[3] = { float(scale.x()), float(scale.y()), float(scale.z()) };
    auto wrap = [](int a, int n) { if (n <= 0) return 0; a %= n; return a < 0 ? a + n : a; };
    for (int y = 0; y < H; ++y) {
        uchar* np = normal.scanLine(y);
        uchar* op = hasOrm ? orm.scanLine(y) : nullptr;
        const uchar* mp = mask.isNull() ? nullptr : mask.constScanLine(y);
        const float vv = (y + 0.5f) / H;
        for (int x = 0; x < W; ++x) {
            const float uu = (x + 0.5f) / W;
            int layer = zmap[1];
            if (mp) { const float mv = mp[x*4] / 255.0f; int zone = 0; float best = 2.0f;
                      for (int k = 0; k < 4; ++k) { const float e = qAbs(mv - bnd[k]); if (e < best) { best = e; zone = k; } }
                      layer = zmap[zone]; if (mv <= 0.02f) layer = -1; }
            const float metalv = op ? op[x*4+2] / 255.0f : 0.0f;
            float metalMask = 1.0f;
            if (metalv > 0.5f) { if (metalLayer >= 0) layer = metalLayer; else metalMask = 0.0f; }
            else if (metalLayer >= 0 && layer == metalLayer) layer = -1;
            if (metalLayer < 0 || layer != metalLayer) metalMask *= 1.0f - qBound(0.0f, (metalv - 0.30f) / 0.30f, 1.0f);
            if (layer < 0 || layer > 2 || metalMask <= 0.001f) continue;
            const float s = sc[layer];
            if (!dN[layer].isNull()) {
                const int dw = dN[layer].width(), dh = dN[layer].height();
                const uchar* dp = dN[layer].constScanLine(wrap(int(vv*s*dh), dh)) + wrap(int(uu*s*dw), dw) * 4;
                const float dnx = (dp[0]/255.0f)*2.0f-1.0f, dny = (dp[1]/255.0f)*2.0f-1.0f;
                const float amt = qBound(0.0f, nInt, 1.0f) * metalMask;
                const float nx = (np[x*4]/255.0f)*2.0f-1.0f + dnx*amt, ny = (np[x*4+1]/255.0f)*2.0f-1.0f + dny*amt;
                np[x*4]   = uchar(qBound(0.0f, (nx*0.5f+0.5f)*255.0f, 255.0f));
                np[x*4+1] = uchar(qBound(0.0f, (ny*0.5f+0.5f)*255.0f, 255.0f));
            }
            if (hasOrm && !dR[layer].isNull()) {
                const int dw = dR[layer].width(), dh = dR[layer].height();
                const float drg = dR[layer].constScanLine(wrap(int(vv*s*dh), dh))[wrap(int(uu*s*dw), dw)*4 + 1] / 255.0f;
                float dr = ((drg - 0.5f) * qBound(0.0f, rInt, 4.0f) + rOff) * metalMask;
                if (dr > 0.0f) dr *= 1.0f - 0.85f * qBound(0.0f, (metalv - 0.35f) / 0.35f, 1.0f);
                op[x*4+1] = uchar(qBound(0.04f, op[x*4+1]/255.0f + dr, 1.0f) * 255.0f);
            }
        }
    }
}

void MaterialDecode::bakeDetailForMaterial(CascReader* reader, const QString& d4,
                                           const QString& matName, QImage& normal, QImage& orm)
{
    if (normal.isNull() || matName.isEmpty() || d4.isEmpty()) return;
    // Authored per-map intensities/offsets. 1.0/0.0 are the SHADER defaults, not "unset": a material
    // that wants a detail map off authors 0, so passing a blanket 1.0 would bake in grain the artist
    // explicitly disabled. One file read AND one JSON parse for all nine.
    const QJsonObject um = QJsonDocument::fromJson(readMat(d4, matName)).object()
                               .value(QStringLiteral("tUberMaterial")).toObject();
    const float nI[3] = { matScalar(um, "Normal Intensity - Detail Map 1", 1.0f),
                          matScalar(um, "Normal Intensity - Detail Map 2", 1.0f),
                          matScalar(um, "Normal Intensity - Detail Map 3", 1.0f) };
    const float rI[3] = { matScalar(um, "Roughness Intensity - Detail Map 1", 1.0f),
                          matScalar(um, "Roughness Intensity - Detail Map 2", 1.0f),
                          matScalar(um, "Roughness Intensity - Detail Map 3", 1.0f) };
    const float rO[3] = { matScalar(um, "Roughness Offset - Detail Map 1", 0.0f),
                          matScalar(um, "Roughness Offset - Detail Map 2", 0.0f),
                          matScalar(um, "Roughness Offset - Detail Map 3", 0.0f) };
    QVector<QImage> outN, outR;
    float sN = 1.0f, sR = 1.0f, sO = 0.0f, sc[3] = { 8, 8, 8 };
    int ml = -1;
    detailMapsSeparate(reader, d4, matName, nI, rI, rO, outN, outR, sN, sR, sO, sc, ml);
    outN.resize(3); outR.resize(3);
    // Nothing authored → no-op, so a material with no detail maps costs one JSON read and no decode.
    // Same guard the Wardrobe export path uses (all three detail normals null → skip).
    if (outN[0].isNull() && outN[1].isNull() && outN[2].isNull()) return;
    const QImage dN[3] = { outN[0], outN[1], outN[2] };
    const QImage dR[3] = { outR[0], outR[1], outR[2] };
    // Bands from THIS material's own dye mask (the artist's painted zone levels), zone→map from the
    // maps actually present — exactly what the Wardrobe decode pass derives per part.
    const QImage dyeMask = byRole(reader, d4, matName, "DYE_MASK");
    bakeDetail(normal, orm, dyeMask, dN, dR, QVector3D(sc[0], sc[1], sc[2]),
               deriveZoneMap(outN, ml), detectDyeBands(dyeMask), ml,
               qBound(0.0f, sN, 4.0f), qBound(0.0f, sR, 4.0f), sO);
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

QStringList MaterialDecode::appearanceRosterFromMeta(const QByteArray& meta, const SnoIndex* idx,
                                                     QString* why, QVector<bool>* clothOut)
{
    QStringList out;
    if (clothOut) clothOut->clear();
    const QVector<AppearanceMatBin::Entry> entries = AppearanceMatBin::read(meta, why);
    QVector<int> snos;
    snos.reserve(entries.size());
    for (const AppearanceMatBin::Entry& e : entries) {
        snos.push_back(e.sno);
        if (clothOut) clothOut->push_back(e.cloth);
    }
    if (snos.isEmpty()) return out;
    // sno -> name over both material groups (57 "Material (2)", 37 "Material") plus Cloth (11),
    // since an entry can be cloth-only. Built once per call; the caller does this at most once per
    // piece load, and caching it would outlive a d4data change.
    QHash<int, QString> byId;
    if (idx)
        for (int g : {57, 37, 11})
            for (const SnoEntry& e : idx->entries(g)) byId.insert(e.snoId, e.name);
    out.reserve(snos.size());
    for (int s : snos) {
        if (!s) { out << QString(); continue; }          // genuinely no material for this slot
        const QString nm = byId.value(s);
        // A KNOWN sno with no usable name still has to carry its sno forward. Returning "" here
        // threw away the one handle we had: the name lookup then found no .mat.json, and the
        // "~unnamed_" placeholder path never fired either, so the material resolved to nothing and
        // the part rendered untextured.
        //
        // That is not a rare case. d4data's CoreTOC.dat.json ships 1,079 group-57 material entries
        // with an EMPTY name (BarM_stor251_TRS_mat, sno 2333699, among them) — so any index built
        // from the d4data snapshot rather than from CASC loses those names, while the game itself
        // has the material perfectly well. d4analyzer reads them because it works from the sno.
        //
        // The placeholder is the existing, tested route to the meta binary; this just makes sure a
        // nameless-but-known material actually takes it.
        out << (nm.isEmpty() ? QStringLiteral("~unnamed_%1").arg(s) : nm);
    }
    return out;
}

QStringList MaterialDecode::appearanceRoster(const QString& d4, const QString& appName,
                                             QVector<bool>* clothOut)
{
    QStringList out;
    if (clothOut) clothOut->clear();
    if (d4.isEmpty() || appName.isEmpty()) return out;
    QFile f(QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4, appName));
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonObject ro = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue& mv : ro.value(QStringLiteral("ptAppearanceMaterials")).toArray()) {
        const QJsonArray soas = mv.toObject().value(QStringLiteral("ptSOAs")).toArray();
        QString name;
        bool isCloth = false;
        if (!soas.isEmpty()) {
            const QJsonObject s = soas[0].toObject();
            // Cloth is decided from the SOURCE FIELD, not from the name that came out of it, and is
            // recorded even when an override or base material won the naming — the entry is still
            // backed by a cloth sim, which is what the SIM toggle is about.
            isCloth = qint64(s.value(QStringLiteral("snoCloth")).toObject()
                              .value(QStringLiteral("__raw__")).toDouble()) > 0
                   || qint64(s.value(QStringLiteral("snoHighQualityClothOverride")).toObject()
                              .value(QStringLiteral("__raw__")).toDouble()) > 0;
            name = s.value(QStringLiteral("snoOverrideMaterial")).toObject().value(QStringLiteral("name")).toString();
            if (name.isEmpty()) name = s.value(QStringLiteral("snoMaterial")).toObject().value(QStringLiteral("name")).toString();
            if (name.isEmpty()) name = s.value(QStringLiteral("snoCloth")).toObject().value(QStringLiteral("name")).toString();
            // Last resort before giving up: a sub-object can reference ONLY the HQ cloth override
            // (spiF_stor198_TRS's chains, PalF_stor157's belt items — 14 in a 1200-piece sample).
            // Those resolved to an empty name, which is what left the part with no material.
            if (name.isEmpty()) name = s.value(QStringLiteral("snoHighQualityClothOverride"))
                                        .toObject().value(QStringLiteral("name")).toString();
            // Still nothing? Each of those sno objects also carries "__raw__", the sno itself, and
            // d4data blanks the NAME on entries it has no export for while keeping the number. So
            // fall back to the sno and hand it on as a "~unnamed_" placeholder, exactly as the
            // binary roster does — the material is then read from the game meta instead of being
            // abandoned because a JSON string happened to be empty.
            if (name.isEmpty()) {
                for (const char* k : {"snoOverrideMaterial", "snoMaterial",
                                      "snoCloth", "snoHighQualityClothOverride"}) {
                    const qint64 raw = qint64(s.value(QLatin1String(k)).toObject()
                                               .value(QStringLiteral("__raw__")).toDouble());
                    if (raw > 0) { name = QStringLiteral("~unnamed_%1").arg(raw); break; }
                }
            }
        }
        out.append(name);
        if (clothOut) clothOut->append(isCloth);
    }
    return out;
}
