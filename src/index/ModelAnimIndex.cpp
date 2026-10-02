// The model animation + entity indexes — MOVED from tabs/ModelsTab.cpp (2026-09-23, phase 6b of
// the AssetBrowser core extraction) so D4's Models tab and the core's D4 store run ONE copy. Every
// function body below is the code that was in ModelsTab, with its member reads made parameters and
// its setScan() progress made a callback; nothing about what is scanned, matched or cached changed.
#include "index/ModelAnimIndex.h"

#include "app/AppPaths.h"
#include "app/SehGuard.h"
#include "casc/CascReader.h"
#include "index/SnoIndex.h"
#include "model/ModelGeometry.h"
#include "model/ModelParser.h"
#include "util/ParallelMap.h"

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <functional>

namespace modelanim {

namespace {
constexpr int kGroupAppearance = 9;   // ModelsTab.cpp's constant

QDataStream& operator<<(QDataStream& ds, const AnimBlob& b) {
    return ds << b.animatedSnos << b.rowsBySno << b.famPrefixes << b.famRows << b.famOwner
              << b.famBones << b.clipSet << b.setClips << b.femaleClips << b.femalePair << b.clipPower;
}
QDataStream& operator>>(QDataStream& ds, AnimBlob& b) {
    return ds >> b.animatedSnos >> b.rowsBySno >> b.famPrefixes >> b.famRows >> b.famOwner
              >> b.famBones >> b.clipSet >> b.setClips >> b.femaleClips >> b.femalePair >> b.clipPower;
}

QDataStream& operator<<(QDataStream& ds, const EntityBlob& b) {
    return ds << b.apprActors << b.apprActorN << b.apprFamily << b.apprItems << b.apprItemN
              << b.itemAppr << b.apprSets << b.apprVariants << b.apprVariantSnos << b.apprName;
}
QDataStream& operator>>(QDataStream& ds, EntityBlob& b) {
    return ds >> b.apprActors >> b.apprActorN >> b.apprFamily >> b.apprItems >> b.apprItemN
              >> b.itemAppr >> b.apprSets >> b.apprVariants >> b.apprVariantSnos >> b.apprName;
}

template <typename T>
bool readIndexCache(const QString& path, const QString& magic, const QString& sig, T& out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QDataStream ds(&f);
    ds.setVersion(QDataStream::Qt_6_0);
    QString gotMagic, gotSig;
    ds >> gotMagic >> gotSig;
    if (gotMagic != magic || gotSig != sig) return false;   // wrong version → re-scan
    ds >> out;
    return ds.status() == QDataStream::Ok;
}
template <typename T>
void writeIndexCache(const QString& path, const QString& magic, const QString& sig, const T& blob)
{
    QSaveFile f(path);   // atomic write (temp + rename) so a crash mid-write can't corrupt the cache
    if (!f.open(QIODevice::WriteOnly)) return;
    QDataStream ds(&f);
    ds.setVersion(QDataStream::Qt_6_0);
    ds << magic << sig << blob;
    f.commit();
}
}   // namespace

// ── Cache key and location ───────────────────────────────────────────────────────────────────────
// Cache key = d4data buildVersion.txt (content + mtime) AND the game build id.
//
// The comment here used to claim the game build was included; it was not — the signature was
// snapshot-only. Phase 2 of the anim scan parses rig bones through CascReader, so a game patch that
// reskeletons a rig served stale family/bone data from the cache with nothing to indicate it. The
// caller passes the build id because this helper has no reader.
QString dataSignature(const QString& d4, const QString& gameBuildId)
{
    const QString bv = d4 + QStringLiteral("/buildVersion.txt");
    QString ver;
    QFile f(bv);
    if (f.open(QIODevice::ReadOnly)) ver = QString::fromUtf8(f.readAll()).trimmed();
    const QDateTime m = QFileInfo(bv).lastModified();
    return ver + QLatin1Char('|') + (m.isValid() ? m.toString(Qt::ISODate) : QString())
               + QLatin1Char('|') + gameBuildId;
}
QString indexCachePath(const QString& name)
{
    static const QString dir = AppPaths::subDir(QStringLiteral("index_cache"));
    return dir + QLatin1Char('/') + name;
}

bool readAnim(const QString& sig, AnimBlob& out)
{ return readIndexCache(indexCachePath(QStringLiteral("anim_index.bin")), QStringLiteral("ANIMIDX2"), sig, out); }
void writeAnim(const QString& sig, const AnimBlob& blob)
{ writeIndexCache(indexCachePath(QStringLiteral("anim_index.bin")), QStringLiteral("ANIMIDX2"), sig, blob); }
bool readEntity(const QString& sig, EntityBlob& out)
{ return readIndexCache(indexCachePath(QStringLiteral("entity_index.bin")), QStringLiteral("ENTIDX3"), sig, out); }
void writeEntity(const QString& sig, const EntityBlob& blob)
{ writeIndexCache(indexCachePath(QStringLiteral("entity_index.bin")), QStringLiteral("ENTIDX3"), sig, blob); }

// ── Scans ────────────────────────────────────────────────────────────────────────────────────────
AnimScan scanAnim(const QString& d4, CascReader* reader, const Progress& progress)
{
    const QString animDir = QStringLiteral("%1/json/base/meta/Anim").arg(d4);
    QElapsedTimer idxClk; idxClk.start();   // first-run indexing timing (logged via qInfo below)
    QSet<int> found;
    QHash<int, QStringList> rowsBySno;   // authoritative anim rows per appearance SNO
    QHash<QString, QString> clipRowByName;   // clip name (lower) → its display row ("name  ·  N frames")
    // Each .ani.json may reference one or more snoAppearance blocks — capture every one, plus
    // the clip's keyframe count, matching the per-model ANIMATIONS panel's row format exactly.
    QStringList files;
    {
        QDirIterator it(animDir, QStringList{QStringLiteral("*.ani.json")}, QDir::Files);
        while (it.hasNext()) files << it.next();
    }
    clipRowByName.reserve(files.size());   // ~one clip row per .ani.json → size up-front, avoid rehashing
    found.reserve(files.size() / 4);
    // Parse every .ani.json in parallel (its snoAppearance owners + frame count), then aggregate.
    struct AnimRec { QString lower; QString row; QList<int> snos; };
    const std::vector<AnimRec> arecs = parallelMap<AnimRec>(files,
        [](const QString& path) -> AnimRec {
            static const QRegularExpression rxApp(
                QStringLiteral("\"snoAppearance\":\\s*\\{[^{}]*?\"__raw__\":\\s*(\\d+)"));
            static const QRegularExpression rxFrames(QStringLiteral("\"nKeyframeCount\":\\s*(\\d+)"));
            AnimRec r;
            QFile jf(path);
            if (!jf.open(QIODevice::ReadOnly)) return r;
            // fromLatin1 (not fromUtf8): we only match ASCII patterns (snoAppearance ids,
            // nKeyframeCount, base/meta paths) and capture ASCII — skipping UTF-8 decoding of
            // every file is faster and cannot affect the (ASCII-only) captured values.
            const QString raw = QString::fromLatin1(jf.readAll());
            QString animName = QFileInfo(path).fileName();
            if (animName.endsWith(QLatin1String(".ani.json"))) animName.chop(9);
            r.lower = animName.toLower();
            const auto fm = rxFrames.match(raw);
            r.row = fm.hasMatch()
                ? QStringLiteral("%1  ·  %2 frames").arg(animName, fm.captured(1)) : animName;
            auto mi = rxApp.globalMatch(raw);
            while (mi.hasNext()) {
                const int sno = mi.next().captured(1).toInt();
                if (sno > 0) r.snos << sno;
            }
            return r;
        },
        [&progress](int d, int t) { if (progress) progress(QStringLiteral("Scanning animations… %1%").arg(t > 0 ? int(qint64(d) * 100 / t) : 100)); }, /*installSeh=*/false, /*threadMul=*/2);   // I/O-bound loose-file scan → oversubscribe
    for (const AnimRec& r : arecs) {
        if (r.lower.isEmpty()) continue;
        clipRowByName.insert(r.lower, r.row);   // for AnimSet → clip-row resolution
        for (int sno : r.snos) {
            found.insert(sno);
            QStringList& list = rowsBySno[sno];
            if (!list.contains(r.row)) list << r.row;
        }
    }
    for (auto it = rowsBySno.begin(); it != rowsBySno.end(); ++it) it.value().sort();
    qInfo("[index] anim files: %lld parsed in %lld ms", (qint64)files.size(), idxClk.elapsed());

    // ── AnimSet index: the game's authoritative clip grouping (base/meta/AnimSet/*.ans.json).
    // Every set's ptPowerEntryList lists snoAnim (+ optional snoFemaleOverrideAnim) clips; we map
    // each clip → its set name (for provenance + display grouping) and flag female-override
    // variants. Pure game data — no name/skeleton inference.
    QHash<QString, QString> clipSet;   // clip name (lower) → AnimSet display name
    QHash<QString, QStringList> setClips;  // AnimSet name → its clip rows (for authoritative borrow)
    QSet<QString> femaleClips;         // clip names (lower) that appear as a female override
    QHash<QString, QString> femalePair;// base clip (lower) → its female-override clip (for gender swap)
    QHash<QString, QString> clipPower; // clip name (lower) → snoPower name (the action it plays)
    {
        const QString setDir = QStringLiteral("%1/json/base/meta/AnimSet").arg(d4);
        QStringList setFiles;
        { QDirIterator it(setDir, QStringList{QStringLiteral("*.ans.json")}, QDir::Files);
          while (it.hasNext()) setFiles << it.next(); }
        // Parse every .ans.json in parallel into a per-file record, then aggregate serially in
        // file order so the "first set/pair/power wins" semantics stay identical to the old loop.
        struct ClipEntry { QString clip; bool female; QString orig; };
        struct SetRec { QString setName;
                        QList<ClipEntry> clips;
                        QList<QPair<QString, QString>> pairs;    // base(lower) → female clip name
                        QList<QPair<QString, QString>> powers; };// clip(lower) → power name
        const std::vector<SetRec> srecs = parallelMap<SetRec>(setFiles,
            [](const QString& path) -> SetRec {
                // Match a snoAnim / snoFemaleOverrideAnim block and pull the referenced Anim file name.
                static const QRegularExpression rxSetAnim(
                    QStringLiteral("\"sno(FemaleOverride)?Anim\":\\s*\\{[^{}]*?base/meta/Anim/([^\"]+?)\\.ani"));
                // Pair a base clip with its female override IN THE SAME entry: the "(?!\"snoAnim\")"
                // guard stops the gap crossing into the next entry, so a null female never mis-pairs.
                static const QRegularExpression rxPair(QStringLiteral(
                    "\"snoAnim\":\\s*\\{[^{}]*?base/meta/Anim/([^\"]+?)\\.ani"
                    "(?:(?!\"snoAnim\")[\\s\\S])*?"
                    "\"snoFemaleOverrideAnim\":\\s*\\{[^{}]*?base/meta/Anim/([^\"]+?)\\.ani"));
                // Pair the entry's Power (the action) with its clip. Group1 = power, group2 = clip.
                static const QRegularExpression rxPower(QStringLiteral(
                    "\"snoPower\":\\s*\\{[^{}]*?base/meta/Power/([^\"]+?)\\.pow"
                    "(?:(?!\"snoPower\")[\\s\\S])*?"
                    "\"snoAnim\":\\s*\\{[^{}]*?base/meta/Anim/([^\"]+?)\\.ani"));
                // The female-override clip shares the entry's power → label it too.
                static const QRegularExpression rxPowerFemale(QStringLiteral(
                    "\"snoPower\":\\s*\\{[^{}]*?base/meta/Power/([^\"]+?)\\.pow"
                    "(?:(?!\"snoPower\")[\\s\\S])*?"
                    "\"snoFemaleOverrideAnim\":\\s*\\{[^{}]*?base/meta/Anim/([^\"]+?)\\.ani"));
                SetRec r;
                QFile sf(path);
                if (!sf.open(QIODevice::ReadOnly)) return r;
                // fromLatin1 (not fromUtf8): only ASCII patterns/captures here → faster, identical result.
                const QString raw = QString::fromLatin1(sf.readAll());
                r.setName = QFileInfo(path).fileName();
                if (r.setName.endsWith(QLatin1String(".ans.json"))) r.setName.chop(9);
                auto mi = rxSetAnim.globalMatch(raw);
                while (mi.hasNext()) {
                    const auto m = mi.next();
                    const QString clip = m.captured(2).toLower();
                    if (clip.isEmpty()) continue;
                    r.clips.append({clip, !m.captured(1).isEmpty(), m.captured(2)});
                }
                auto pi = rxPair.globalMatch(raw);
                while (pi.hasNext()) { const auto m = pi.next();
                    r.pairs.append({m.captured(1).toLower(), m.captured(2)}); }
                auto qi = rxPower.globalMatch(raw);        // base clips first…
                while (qi.hasNext()) { const auto m = qi.next();
                    r.powers.append({m.captured(2).toLower(), m.captured(1)}); }
                auto qf = rxPowerFemale.globalMatch(raw);  // …then female overrides (same first-wins order)
                while (qf.hasNext()) { const auto m = qf.next();
                    r.powers.append({m.captured(2).toLower(), m.captured(1)}); }
                return r;
            },
            [&progress](int d, int t) { if (progress) progress(QStringLiteral("Indexing anim sets… %1%").arg(t > 0 ? int(qint64(d) * 100 / t) : 100)); }, /*installSeh=*/false, /*threadMul=*/2);   // I/O-bound loose-file scan → oversubscribe
        for (const SetRec& rec : srecs) {
            if (rec.setName.isEmpty()) continue;
            QStringList& sc = setClips[rec.setName];       // creates the (possibly empty) set entry
            for (const ClipEntry& ce : rec.clips) {
                if (!clipSet.contains(ce.clip)) clipSet.insert(ce.clip, rec.setName);  // first set wins
                if (ce.female) femaleClips.insert(ce.clip);                            // female-override slot
                const QString row = clipRowByName.value(ce.clip, ce.orig);             // "name · N frames"
                if (!sc.contains(row)) sc << row;                                      // set → its clip rows
            }
            for (const auto& pr : rec.pairs)
                if (!pr.first.isEmpty() && !pr.second.isEmpty() && !femalePair.contains(pr.first))
                    femalePair.insert(pr.first, pr.second);   // base → female clip name
            for (const auto& pw : rec.powers)
                if (!pw.first.isEmpty() && !pw.second.isEmpty() && !clipPower.contains(pw.first))
                    clipPower.insert(pw.first, pw.second);     // clip → the action/power it plays
        }
        qInfo("[index] animset files: %lld parsed, cumulative %lld ms", (qint64)setFiles.size(), idxClk.elapsed());
    }

    // Phase 2: parse each clip-owning base rig's skeleton → its bone-name-hash set, so a model
    // with no name-family match can still be matched to its rig by bone-hash overlap. parseApp is
    // the crash-prone path, so each parse is SEH-guarded — a bad base can't kill the scan thread.
    QHash<int, QSet<quint32>> bonesBySno;
    if (reader) {
        seh::installSehTranslator();
        const QList<int> owners = rowsBySno.keys();
        const int otot = owners.size();
        int lastRp = -1;
        for (int i = 0; i < otot; ++i) {
            const int osno = owners.at(i);
            QSet<quint32> bones;
            seh::runGuarded("rigparse", [&]() {
                const QByteArray meta = reader->readMetaBySno(quint64(osno));
                const QByteArray payload = reader->readPayloadBySno(quint64(osno));
                if (!meta.isEmpty() && !payload.isEmpty()) {
                    const ModelGeometry g = ModelParser::parseApp(meta, payload);
                    for (const ModelJoint& j : g.skeleton) if (j.nameHash) bones.insert(j.nameHash);
                }
            });
            if (!bones.isEmpty()) bonesBySno.insert(osno, bones);
            const int pct = otot > 0 ? int((qint64(i + 1) * 100) / otot) : 100;
            if (pct != lastRp) {
                lastRp = pct;
                if (progress) progress(QStringLiteral("Indexing rigs… %1%").arg(pct));
            }
        }
    }

    qInfo("[index] anim TOTAL (incl. rig phase): %lld ms", idxClk.elapsed());
    AnimScan scan;
    scan.blob.animatedSnos = found;
    scan.blob.rowsBySno = rowsBySno;
    scan.blob.clipSet = clipSet;
    scan.blob.setClips = setClips;
    scan.blob.femaleClips = femaleClips;
    scan.blob.femalePair = femalePair;
    scan.blob.clipPower = clipPower;
    scan.bonesBySno = bonesBySno;
    return scan;
}

AnimBlob finishFamilies(const AnimScan& scan, const SnoIndex* index)
{
    AnimBlob a = scan.blob;
    const QHash<int, QSet<quint32>>& bonesBySno = scan.bonesBySno;
    // Build the base-family index: each clip owner's name → family prefix → its clips, so a
    // rigged piece can inherit its base body's animations (bone-hash retargeting handles the rest).
    a.famPrefixes.clear();
    a.famRows.clear();
    a.famOwner.clear();
    a.famBones.clear();
    if (index) {
        QHash<int, QString> ownerName;
        for (const SnoEntry& e : index->entries(kGroupAppearance))
            if (a.rowsBySno.contains(e.snoId)) ownerName.insert(e.snoId, e.name);
        for (auto it = a.rowsBySno.begin(); it != a.rowsBySno.end(); ++it) {
            const QString nm = ownerName.value(it.key());
            if (nm.isEmpty()) continue;
            const QString fam = animFamilyPrefix(nm.toLower());
            if (fam.isEmpty()) continue;
            a.famPrefixes.insert(fam);
            if (!a.famOwner.contains(fam)) a.famOwner.insert(fam, nm);   // base name for the tooltip
            QStringList& fr = a.famRows[fam];
            for (const QString& r : it.value()) if (!fr.contains(r)) fr << r;
        }
        for (auto it = a.famRows.begin(); it != a.famRows.end(); ++it) it.value().sort();
        // Phase 2: union each family's base-rig bone-hash sets (parsed on the scan thread),
        // keyed by the same family prefix, for skeleton-overlap fallback matching.
        for (auto it = bonesBySno.constBegin(); it != bonesBySno.constEnd(); ++it) {
            const QString fam = animFamilyPrefix(ownerName.value(it.key()).toLower());
            if (!fam.isEmpty() && !ownerName.value(it.key()).isEmpty())
                a.famBones[fam].unite(it.value());
        }
    }
    return a;
}

EntityBlob scanEntity(const QString& d4, const Progress& progress)
{
    constexpr int kCap = 50;   // keep per-appearance name lists bounded (shared base rigs are huge)
    QElapsedTimer idxClk; idxClk.start();   // first-run indexing timing (logged via qInfo below)

    QHash<int, QStringList> apprActors;   // appearance → capped actor names
    QHash<int, int>         apprActorN;   // appearance → true count
    QHash<int, QString>     apprFamily;   // appearance → family name
    QHash<int, int>         actorAppr;    // actor sno → its (base) appearance sno (to resolve items)
    QHash<int, QStringList> apprSets;     // appearance → AnimSet names its actors play (AUTHORITATIVE)
    QHash<int, QStringList> apprVariants; // appearance → sibling skin-variant names (same actor)
    QHash<int, QList<int>>  apprVariantSnos; // appearance → sibling variant SNOs (for jump)
    QHash<int, QString>     apprName;     // appearance sno → its short name (for variant display/menu)

    // ── Actors (parsed as JSON so appearance/animset scoping is exact, not guessed) ──────────
    {
        const QString dir = d4 + QStringLiteral("/json/base/meta/Actor");
        QStringList files;
        { QDirIterator it(dir, QStringList{QStringLiteral("*.acr.json")}, QDir::Files);
          while (it.hasNext()) files << it.next(); }
        actorAppr.reserve(files.size());   // ~one entry per actor → size up-front, avoid rehashing
        apprName.reserve(files.size());
        // Parse every actor JSON in parallel (the slow part), then aggregate the records serially
        // (identical logic to the old loop). A record is one actor's self-appearances + animsets.
        struct ActorRec {
            bool ok = false;
            int  selfSno = 0;
            int  base = 0;
            QList<int> apprs;              // self appearances (base + add-on skins), base first
            QHash<int, QString> apprName;  // appearance sno → short name
            QStringList sets;              // AnimSet names this actor plays
            QString fam;                   // monster family name
            QString name;                  // actor filename (no ext)
        };
        const std::vector<ActorRec> recs = parallelMap<ActorRec>(files,
            [](const QString& path) -> ActorRec {
                ActorRec r;
                QFile f(path);
                if (!f.open(QIODevice::ReadOnly)) return r;
                QJsonParseError pe;
                const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
                if (pe.error != QJsonParseError::NoError || !doc.isObject()) return r;
                const QJsonObject o = doc.object();
                auto nameOfAppr = [](const QJsonObject& ob) {
                    QString nm = ob.value(QStringLiteral("__targetFileName__")).toString()
                                     .section(QLatin1Char('/'), -1);
                    if (nm.endsWith(QLatin1String(".app"))) nm.chop(4);
                    return nm;
                };
                QSet<int> seen;
                auto addAppr = [&](int sno, const QString& anm) {
                    if (sno > 0 && !seen.contains(sno)) {
                        seen.insert(sno); r.apprs << sno;
                        if (!anm.isEmpty()) r.apprName.insert(sno, anm);
                    }
                };
                const QJsonObject baseObj = o.value(QStringLiteral("snoAppearance")).toObject();
                r.base = baseObj.value(QStringLiteral("__raw__")).toInt();
                addAppr(r.base, nameOfAppr(baseObj));
                std::function<void(const QJsonValue&)> gatherApp = [&](const QJsonValue& v) {
                    if (v.isObject()) {
                        const QJsonObject ob = v.toObject();
                        if (ob.value(QStringLiteral("__targetFileName__")).toString()
                                .contains(QLatin1String("/Appearance/")))
                            addAppr(ob.value(QStringLiteral("__raw__")).toInt(), nameOfAppr(ob));
                        for (const QString& k : ob.keys()) gatherApp(ob.value(k));
                    } else if (v.isArray()) {
                        for (const QJsonValue& e : v.toArray()) gatherApp(e);
                    }
                };
                gatherApp(o.value(QStringLiteral("arActorAppearanceAddOnEntries")));
                gatherApp(o.value(QStringLiteral("arCustomizationAppearances")));
                auto gatherSets = [&](const QJsonValue& v) {
                    if (!v.isArray()) return;
                    for (const QJsonValue& e : v.toArray()) {
                        QString tf = e.toObject().value(QStringLiteral("__targetFileName__")).toString();
                        if (!tf.contains(QLatin1String("/AnimSet/"))) continue;
                        QString nm = tf.section(QLatin1Char('/'), -1);
                        if (nm.endsWith(QLatin1String(".ans"))) nm.chop(4);
                        if (!nm.isEmpty() && !r.sets.contains(nm)) r.sets << nm;
                    }
                };
                gatherSets(o.value(QStringLiteral("arAnimSets")));
                gatherSets(o.value(QStringLiteral("arStoreAnimSets")));
                // Monster family — lives under ptMonsterData[], NOT at the actor's top level.
                // (Verified against d4data: 0/400 actors have a top-level snoMonsterFamily;
                // ~7% have ptMonsterData[0].snoMonsterFamily. The old top-level read always
                // came back empty, which left "Family" blank and made the Creature filter
                // match nothing.) `name` is the family directly — no path parsing needed.
                { const QJsonArray md = o.value(QStringLiteral("ptMonsterData")).toArray();
                  if (!md.isEmpty()) {
                      const QJsonObject fam = md.first().toObject()
                                                  .value(QStringLiteral("snoMonsterFamily")).toObject();
                      r.fam = fam.value(QStringLiteral("name")).toString();
                      if (r.fam.endsWith(QLatin1String(".mfm"))) r.fam.chop(4);
                  } }
                r.selfSno = o.value(QStringLiteral("__snoID__")).toInt();
                r.name = QFileInfo(path).fileName();
                if (r.name.endsWith(QLatin1String(".acr.json"))) r.name.chop(9);
                r.ok = true;
                return r;
            },
            [&progress](int d, int t) { if (progress) progress(QStringLiteral("Indexing actors… %1%").arg(t > 0 ? int(qint64(d) * 100 / t) : 100)); }, /*installSeh=*/false, /*threadMul=*/2);   // I/O-bound loose-file scan → oversubscribe
        qInfo("[index] actor files: %lld parsed in %lld ms", (qint64)files.size(), idxClk.elapsed());
        for (const ActorRec& r : recs) {
            if (!r.ok) continue;
            for (auto it = r.apprName.constBegin(); it != r.apprName.constEnd(); ++it)
                if (!apprName.contains(it.key())) apprName.insert(it.key(), it.value());
            if (r.base > 0 && r.selfSno > 0) actorAppr.insert(r.selfSno, r.base);   // items resolve via base
            for (int appr : r.apprs) {
                int& n = apprActorN[appr]; ++n;
                QStringList& l = apprActors[appr];
                if (l.size() < kCap && !l.contains(r.name)) l << r.name;
                if (!r.fam.isEmpty() && !apprFamily.contains(appr)) apprFamily.insert(appr, r.fam);
                if (!r.sets.isEmpty()) {
                    QStringList& as = apprSets[appr];
                    for (const QString& s : r.sets) if (!as.contains(s)) as << s;
                }
                if (r.apprs.size() > 1) {   // sibling skin variants
                    QStringList& vs = apprVariants[appr];
                    QList<int>& vsno = apprVariantSnos[appr];
                    for (int b : r.apprs) {
                        if (b == appr) continue;
                        const QString bn = r.apprName.value(b);
                        if (!bn.isEmpty() && !vs.contains(bn)) vs << bn;
                        if (b > 0 && !vsno.contains(b)) vsno << b;
                    }
                }
            }
        }
    }

    // ── Items → (via their actor) the appearances they render ────────────────
    QHash<int, QStringList> apprItems;
    QHash<int, int>         apprItemN;
    QHash<QString, int>     itemAppr;   // item name (original case) → appearance sno (forward jump)
    {
        const QString dir = d4 + QStringLiteral("/json/base/meta/Item");
        QStringList files;
        { QDirIterator it(dir, QStringList{QStringLiteral("*.itm.json")}, QDir::Files);
          while (it.hasNext()) files << it.next(); }
        struct ItemRec { int actor = 0; QString name; };
        const std::vector<ItemRec> recs = parallelMap<ItemRec>(files,
            [](const QString& path) -> ItemRec {
                static const QRegularExpression rxA(
                    QStringLiteral("\"snoActor\":\\s*\\{[^{}]*?\"__raw__\":\\s*(\\d+)"));
                ItemRec r;
                QFile f(path);
                if (!f.open(QIODevice::ReadOnly)) return r;
                const auto mo = rxA.match(QString::fromUtf8(f.readAll()));
                if (mo.hasMatch()) {
                    r.actor = mo.captured(1).toInt();
                    r.name = QFileInfo(path).fileName();
                    if (r.name.endsWith(QLatin1String(".itm.json"))) r.name.chop(9);
                }
                return r;
            },
            [&progress](int d, int t) { if (progress) progress(QStringLiteral("Indexing items… %1%").arg(t > 0 ? int(qint64(d) * 100 / t) : 100)); }, /*installSeh=*/false, /*threadMul=*/2);   // I/O-bound loose-file scan → oversubscribe
        qInfo("[index] item files: %lld parsed in %lld ms", (qint64)files.size(), idxClk.elapsed());
        for (const ItemRec& r : recs) {
            if (r.actor <= 0) continue;
            const int appr = actorAppr.value(r.actor, 0);
            if (appr <= 0) continue;
            int& n = apprItemN[appr]; ++n;
            QStringList& l = apprItems[appr];
            if (l.size() < kCap) l << r.name;
            itemAppr.insert(r.name, appr);   // forward: item → its model (original-case key)
        }
    }
    for (auto it = apprActors.begin(); it != apprActors.end(); ++it) it.value().sort();
    for (auto it = apprItems.begin();  it != apprItems.end();  ++it) it.value().sort();
    for (auto it = apprVariants.begin(); it != apprVariants.end(); ++it) it.value().sort();
    qInfo("[index] entity TOTAL (actors+items): %lld ms", idxClk.elapsed());
    EntityBlob e;
    e.apprActors = apprActors;   e.apprActorN = apprActorN;
    e.apprFamily = apprFamily;
    e.apprItems  = apprItems;    e.apprItemN  = apprItemN;
    e.itemAppr   = itemAppr;
    e.apprSets   = apprSets;
    e.apprVariants = apprVariants;
    e.apprVariantSnos = apprVariantSnos;
    e.apprName   = apprName;
    return e;
}

QSet<QString> rigFamilies(const SnoIndex& index)
{
    // ModelsTab::ensureRigIndex's loop, verbatim.
    QSet<QString> out;
    // `_base` with the digits OPTIONAL — must match animFamilyPrefix's `_base\d*$`, which is what
    // strips the token when the filter tests a name. With `\d+` here, the 84 rigs named plain
    // "<family>_base" (council_hydra_base, boss_inarius_*_base, cave_*_base…) never entered the
    // family set, so nothing on those rigs could ever match "Rigged".
    static const QRegularExpression rxBaseName(QStringLiteral("_base\\d*$"));
    for (const SnoEntry& e : index.entries(kGroupAppearance)) {
        const QString nl = e.name.toLower();
        if (rxBaseName.match(nl).hasMatch())
            out.insert(animFamilyPrefix(nl));
    }
    return out;
}

// ── Queries ──────────────────────────────────────────────────────────────────────────────────────
// A model's clip NAMES (own + inherited base-rig, deduped) from the animation index — the batch
// export's clip lister, so "include all animations" works for models that were never LOADED in
// the viewport. Requires the anim index (ensureAnimatedIndex) to have completed.
// Row list → clip names, deduped and sorted. Rows are "clip  ·  extra".
QStringList clipNamesOf(const QStringList& rows)
{
    QStringList names;
    for (const QString& r : rows) {
        const QString nm = r.section(QStringLiteral("  ·  "), 0, 0);
        if (!nm.isEmpty() && !names.contains(nm)) names << nm;
    }
    names.sort();
    return names;
}

// Does this clip name belong to one of the model's own animation families?
bool clipInFamily(const QString& clipName, const QStringList& fams)
{
    const QString c = clipName.toLower();
    for (const QString& f : fams)
        if (c == f || c.startsWith(f + QLatin1Char('_'))) return true;
    return false;
}

// The longest `_`-boundary prefix of a model name that names a clip-owning base family
// (empty if none): npcF_S14_Dannica_TRS matches base family "npcf_s14_dannica".
QString animLongestFamily(const QString& nameLower, const QSet<QString>& families)
{
    QString best;
    for (int i = nameLower.indexOf(QLatin1Char('_')); i > 0;
         i = nameLower.indexOf(QLatin1Char('_'), i + 1)) {
        const QString p = nameLower.left(i);
        if (families.contains(p) && p.size() > best.size()) best = p;
    }
    if (families.contains(nameLower) && nameLower.size() > best.size()) best = nameLower;
    return best;
}

// ── Shared-rig ("base") animation resolution helpers ─────────────────────────
// Family prefix of a clip-owning appearance name: strip a trailing _base<NN> (the body-rig
// carrier) or a slot token, so barF_base00 → "barf" and npcF_S14_Dannica_base00 → "npcf_s14_dannica".
QString animFamilyPrefix(const QString& nameLower)
{
    static const QRegularExpression rxBase(QStringLiteral("_base\\d*$"));
    const auto m = rxBase.match(nameLower);
    if (m.hasMatch()) return nameLower.left(m.capturedStart());
    static const QSet<QString> kSlots = {
        QStringLiteral("trs"), QStringLiteral("hlm"), QStringLiteral("leg"), QStringLiteral("glv"),
        QStringLiteral("bts"), QStringLiteral("sho"), QStringLiteral("cap"), QStringLiteral("blt"),
        QStringLiteral("hed"), QStringLiteral("bdy")};
    const int us = nameLower.lastIndexOf(QLatin1Char('_'));
    if (us > 0 && kSlots.contains(nameLower.mid(us + 1))) return nameLower.left(us);
    return nameLower;
}

QStringList familiesBySkeleton(const AnimBlob& a, const QVector<ModelJoint>& skel, double minScore)
{
    QStringList out;
    if (skel.isEmpty() || a.famBones.isEmpty()) return out;
    QSet<quint32> mine;
    for (const ModelJoint& j : skel) if (j.nameHash) mine.insert(j.nameHash);
    if (mine.isEmpty()) return out;
    // SCORING. This used to be inter / min(|mine|, |fb|) — the fraction of the SMALLER set covered.
    // That makes any small skeleton match everything: a leg-armour piece is skinned to ~20 bones,
    // every humanoid rig in the game contains those same leg bones, so it scored ~1.0 against
    // hundreds of families and inherited all of their clips (measured: 20,251 rows on
    // barF_stor189_LEG). Use a symmetric Jaccard score instead — a genuine same-rig match stays
    // near 1.0, while a 20-bone subset of a 300-bone rig scores ~0.07 and is correctly rejected.
    QVector<QPair<double, QString>> ranked;
    for (auto it = a.famBones.constBegin(); it != a.famBones.constEnd(); ++it) {
        const QSet<quint32>& fb = it.value();
        if (fb.isEmpty()) continue;
        int inter = 0;
        for (quint32 h : mine) if (fb.contains(h)) ++inter;
        const double uni = double(mine.size() + fb.size() - inter);
        const double score = uni > 0 ? inter / uni : 0.0;
        if (score >= minScore) ranked.append({ score, it.key() });
    }
    // Best first, and only a handful. This is a GUESS used when the game data gave us nothing;
    // it should offer the closest rigs, never a union of every rig that happens to overlap.
    std::sort(ranked.begin(), ranked.end(),
              [](const QPair<double, QString>& a, const QPair<double, QString>& b) { return a.first > b.first; });
    constexpr int kMaxFamilies = 3;
    for (int i = 0; i < ranked.size() && i < kMaxFamilies; ++i) out << ranked[i].second;
    return out;
}

QStringList clipFamiliesFor(const Lookup& L, const QString& nameLower)
{
    QStringList fams;
    auto add = [&fams](const QString& f) { if (!f.isEmpty() && !fams.contains(f)) fams << f; };
    add(animLongestFamily(nameLower, L.anim->famPrefixes));
    add(animLongestFamily(nameLower, (*L.rigFams)));
    // animFamilyPrefix strips a slot/base suffix (barF_stor189_LEG → barf_stor189); the family that
    // actually owns clips is usually the shorter body prefix, so offer the stripped form too.
    const QString stripped = animFamilyPrefix(nameLower);
    if (L.anim->famPrefixes.contains(stripped) || (*L.rigFams).contains(stripped)) add(stripped);
    // Leading token (barf_stor189_leg → barf) when it is a known clip-owning family.
    const int us = nameLower.indexOf(QLatin1Char('_'));
    if (us > 0) {
        const QString head = nameLower.left(us);
        if (L.anim->famPrefixes.contains(head) || (*L.rigFams).contains(head)) add(head);
    }
    // The skeleton fallback: only when the caller holds the model's skeleton (ModelsTab: the model
    // on screen — sno == m_curSno && m_curGeo.valid; the store: the model it is loading).
    if (fams.isEmpty() && L.skel)
        for (const QString& f : familiesBySkeleton(*L.anim, *L.skel, 0.5)) add(f);
    return fams;
}

QStringList authoredAnimClips(const Lookup& L, int sno, const QString& nameLower)
{
    const QStringList all = clipNamesOf(L.anim->rowsBySno.value(sno));
    const QStringList fams = clipFamiliesFor(L, nameLower);
    // Unresolvable family → keep the whole set. This is a REFINEMENT of an already-authoritative
    // list, not an expansion of a speculative one: failing closed here would mean exporting
    // nothing. (The fail-closed rule applies to expansions — see modelAnimRows, where an unknown
    // family must expand NOTHING.)
    if (fams.isEmpty()) return all;

    QStringList out;
    QSet<QString> have;
    auto add = [&](const QString& nm) {
        if (nm.isEmpty() || have.contains(nm)) return;
        have.insert(nm);
        out << nm;
    };
    for (const QString& nm : all)
        if (clipInFamily(nm, fams)) add(nm);

    // The clips of the AnimSets the game assigns to this appearance's ACTORS, filtered to the same
    // family. This belongs HERE, not in setAnimClips: it is how a gear piece or a prop — which owns
    // no .ani.json rows of its own — gets any animation at all. Moving it to the opt-in source made
    // every such model export with zero clips by default, and put in-family clips behind a checkbox
    // labelled "named outside this model's family". m_apprSets is usually empty for a BASE rig (the
    // player body is applied at runtime, so no Actor references it), which is why this line does
    // nothing for barM_base00 and everything for barM_stor191_LEG.
    const QStringList authSets = L.ent->apprSets.value(sno);
    if (!authSets.isEmpty()) {
        QStringList rows;
        for (const QString& sn : authSets)
            for (const QString& r : L.anim->setClips.value(sn)) {
                const QString clip = r.section(QStringLiteral("  ·  "), 0, 0).toLower();
                if (clipInFamily(clip, fams) && !rows.contains(r)) rows << r;
            }
        for (const QString& nm : clipNamesOf(rows)) add(nm);
    }
    out.sort();
    return out;
}

QStringList setAnimClips(const Lookup& L, int sno, const QString& nameLower)
{
    const QStringList fams = clipFamiliesFor(L, nameLower);
    if (fams.isEmpty()) return {};   // no family → authoredAnimClips already returned everything
    QStringList out;
    for (const QString& nm : clipNamesOf(L.anim->rowsBySno.value(sno)))
        if (!clipInFamily(nm, fams)) out << nm;
    return out;
}

QStringList ownAnimClips(const Lookup& L, int sno, const QString& nameLower)
{
    QStringList names = authoredAnimClips(L, sno, nameLower);
    for (const QString& nm : setAnimClips(L, sno, nameLower))
        if (!names.contains(nm)) names << nm;
    names.sort();
    return names;
}

QStringList baseAnimClips(const Lookup& L, int sno, const QString& nameLower)
{
    const QString fam = animLongestFamily(nameLower, L.anim->famPrefixes);
    if (fam.isEmpty()) return {};
    const QStringList own = ownAnimClips(L, sno, nameLower);
    QStringList out;
    for (const QString& nm : clipNamesOf(L.anim->famRows.value(fam)))
        if (!own.contains(nm)) out << nm;
    return out;
}

QStringList animClipsFor(const Lookup& L, int sno, const QString& nameLower,
                                    bool wantOriginal, bool wantSets, bool wantBase)
{
    QStringList names;
    if (wantOriginal) names = authoredAnimClips(L, sno, nameLower);
    if (wantSets)
        for (const QString& nm : setAnimClips(L, sno, nameLower))
            if (!names.contains(nm)) names << nm;
    if (wantBase)
        for (const QString& nm : baseAnimClips(L, sno, nameLower))
            if (!names.contains(nm)) names << nm;
    names.sort();
    return names;
}

AnimParser::DecodedAnim decodeForSkeleton(CascReader* reader, const QString& d4, const QString& animName,
                                                         const ModelGeometry& geo)
{
    AnimParser::DecodedAnim bad;
    if (animName.isEmpty() || !geo.valid || geo.skeleton.isEmpty() ||
        !reader || !reader->isReady())
        return bad;
    QFile jf(QStringLiteral("%1/json/base/meta/Anim/%2.ani.json").arg(d4, animName));
    if (!jf.open(QIODevice::ReadOnly))
        return bad;
    const QJsonObject root = QJsonDocument::fromJson(jf.readAll()).object();
    const int animSno = root.value(QStringLiteral("__snoID__")).toInt();
    const QJsonArray perms = root.value(QStringLiteral("ptPermutations")).toArray();
    if (animSno <= 0 || perms.isEmpty())
        return bad;
    const QJsonObject perm = perms.first().toObject();
    const QJsonObject pv = perm.value(QStringLiteral("ptPayloadData")).toObject()
                               .value(QStringLiteral("value")).toObject();
    const int offset = pv.value(QStringLiteral("dataOffset")).toInt();
    const int frames = perm.value(QStringLiteral("nKeyframeCount")).toInt();
    const int comp = perm.value(QStringLiteral("flCompression")).toInt();
    const float fps = float(perm.value(QStringLiteral("flFrameRate")).toDouble(30.0));
    if (frames <= 0)
        return bad;
    const QByteArray payload = reader->readPayloadBySno(quint64(animSno));
    if (payload.isEmpty())
        return bad;
    QHash<quint32, AnimParser::RestTRS> rest;
    for (const ModelJoint& j : geo.skeleton) {
        AnimParser::RestTRS t; t.q = j.restQ; t.t = j.restT; t.s = j.restS;
        rest.insert(j.nameHash, t);
    }
    // Guard the decode (arbitrary .ani payload) — a malformed clip would otherwise access-violate
    // and kill the process. Covers both the interactive "play clip" path and the export path.
    AnimParser::DecodedAnim out;
    if (!seh::runGuarded("anim", [&]() { out = AnimParser::decode(payload, offset, frames, comp, fps, rest); }))
        return bad;
    return out;
}

}  // namespace modelanim
