#include "index/SnoIndex.h"
#include "casc/CascReader.h"
#include "app/AppPaths.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>

namespace {

const QHash<int, QString>& groupNameMap()
{
    static const QHash<int, QString> kMap = {
        {1,"Actor"}, {2,"Adventure"}, {5,"Anim2D"}, {6,"Anim"}, {7,"Unknown"},
        {8,"AnimSet"}, {9,"Appearance"}, {11,"AnimTree"}, {12,"Sound"},
        {14,"TimedEvent"}, {15,"UI"}, {17,"Conversation"}, {18,"Global"},
        {19,"LevelArea"}, {20,"GameBalance"}, {21,"Global (2)"}, {22,"Particle"},
        {23,"Scene"}, {24,"Actor (2)"}, {26,"Observer"}, {27,"Anim (2)"},
        {28,"Encounter"}, {29,"Power"}, {31,"Quest"}, {32,"RopeSim"},
        {33,"Sound (2)"}, {36,"Cloth"}, {37,"Material"}, {38,"Explosion"},
        {39,"FlagSet"}, {40,"FogVolume"}, {42,"StringList"}, {43,"Subzone"},
        {44,"Texture"}, {45,"Trail"}, {46,"UI (2)"}, {47,"VectorField"},
        {48,"Vibration"}, {49,"Weather"}, {51,"Zone"}, {57,"Material (2)"},
        {59,"Reverb"}, {60,"MarkerSet"}, {62,"Recipe"}, {63,"Reputation"},
        {67,"Crafter"}, {68,"HoudiniParticles"}, {71,"SoundBank"}, {72,"Actor (NPC)"},
        {73,"Item"}, {74,"PlayerClass"}, {76,"Font"}, {77,"Affix"}, {78,"Emblem"},
        {79,"DungeonAffix"}, {80,"MonsterAffix"}, {81,"MaterialValue"},
        {82,"MaterialValueSet"}, {85,"Jewelry"}, {86,"Condition"}, {88,"ActorService"},
        {90,"Boost"}, {92,"ItemRequirement"}, {93,"ItemType"}, {95,"Achievement"},
        {96,"Season"}, {98,"GearItem"}, {99,"WwiseSoundBank"}, {100,"MonsterFamily"},
        {101,"Physics"}, {102,"BehaviorContainer"}, {103,"Modal"}, {104,"Power (2)"},
        {105,"Surface"}, {106,"SkillKit"}, {107,"Shader"}, {108,"ShaderMap"},
        {109,"NPCComponentSet"}, {110,"Power (3)"}, {111,"Stagger"}, {112,"Wall"},
        {114,"Rope"}, {115,"Biome"}, {116,"DemonScroll"}, {117,"EyeColor"},
        {118,"FacialHair"}, {119,"HairColor"}, {120,"HairStyle"}, {121,"Makeup"},
        {122,"MarkingColor"}, {123,"MarkingShape"}, {124,"Face"}, {126,"Lore"},
        {127,"Tutorial"}, {128,"Storyboard"}, {129,"Movie"}, {130,"FogOfWar"},
        {131,"FootstepTable"}, {132,"MercenaryClass"}, {133,"ParagonBoard"},
        {134,"ParagonGlyph"}, {135,"ParagonGlyphAffix"}, {136,"ParagonNode"},
        {137,"ParagonThreshold"}, {138,"Raid"}, {139,"Territory"},
        {140,"BattlePassTier"}, {141,"CommunityModifier"}, {143,"StoreProduct"},
        {145,"TownPortalCosmetic"}, {146,"CollectiblePower"}, {149,"GenericSkillTree"},
        {150,"ABTest"}, {151,"Aspect"}, {152,"TrackedReward"}, {156,"PlayerTitle"},
        {157,"DeathKit"}, {158,"CrowdTemplates"}, {160,"SoundTable"}, {162,"Dye"},
        {165,"Emote"}, {166,"QuestChain"}, {167,"DataStore"}, {169,"AudioContext"},
        {170,"PowerModifier"}, {172,"SetItemBonus"}, {173,"WorldState"},
        {174,"MountProfile"}, {175,"CrafterTab"}, {176,"Vendor"}, {177,"TiledStyle"},
        {180,"UIDesignerNotification"},
    };
    return kMap;
}

// Features intentionally kept out of the browser (data-group level), matching the
// Python fork's EXCLUDED_GROUPS: Powers (29/104/110), Quests (31/166), Adventure (2).
const QSet<int>& excludedGroups()
{
    static const QSet<int> kExcluded = {2, 29, 31, 104, 110, 166};
    return kExcluded;
}

}  // namespace

QString SnoIndex::groupName(int group)
{
    const auto& m = groupNameMap();
    auto it = m.constFind(group);
    return it != m.constEnd() ? it.value() : QStringLiteral("Group %1").arg(group);
}

// Reverse lookup so tabs can pin themselves to a group NAME instead of a magic number —
// if a game update ever renumbers the SNO groups, only the name map needs correcting and
// every caller follows. `fallback` keeps today's behavior when the name isn't found.
int SnoIndex::groupIdByName(const QString& name, int fallback)
{
    const auto& m = groupNameMap();
    for (auto it = m.constBegin(); it != m.constEnd(); ++it)
        if (it.value().compare(name, Qt::CaseInsensitive) == 0) return it.key();
    return fallback;
}

bool SnoIndex::isExcluded(int group) { return excludedGroups().contains(group); }

void SnoIndex::clear()
{
    m_byGroup.clear();
    m_loaded = false;
    m_total  = 0;
}

bool SnoIndex::ingest(QHash<int, QVector<SnoEntry>>& parsed)
{
    m_byGroup.clear();
    m_total = 0;
    m_loaded = false;
    if (parsed.isEmpty())
        return false;

    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
        if (isExcluded(it.key()))
            continue;
        QVector<SnoEntry> entries = it.value();
        std::sort(entries.begin(), entries.end(),
                  [](const SnoEntry& a, const SnoEntry& b) {
                      return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
                  });
        m_total += entries.size();
        m_byGroup.insert(it.key(), std::move(entries));
    }
    m_loaded = !m_byGroup.isEmpty();
    return m_loaded;
}

bool SnoIndex::loadFromCasc(CascReader& casc)
{
    clear();
    const QByteArray data = casc.readFile(QStringLiteral("base/CoreTOC.dat"));
    qInfo().noquote() << "loadFromCasc: base/CoreTOC.dat ->" << data.size() << "bytes";
    if (data.isEmpty())
        return false;
    QHash<int, QVector<SnoEntry>> parsed = parseCoreToc(data);
    return ingest(parsed);
}

// ── Post-ingest disk cache — same compact-binary recipe as the CASC caches ───────────────────
// magic · sig · groupCount · per group [i32 id, u32 n, per entry i32 sno + u16 utf8-name].
// Stored AFTER exclude+sort, so a warm launch is a bulk read straight into m_byGroup.
static constexpr quint32 kTocCacheMagic = 0x544F4331;   // 'TOC1'

static QString tocCachePath() { return AppPaths::dataDir() + QStringLiteral("/coretoc_v1.bin"); }

bool SnoIndex::loadFromCache(const QString& sig)
{
    clear();
    if (sig.isEmpty()) return false;
    QFile f(tocCachePath());
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray all = f.readAll();
    const char* p = all.constData();
    const char* end = p + all.size();
    auto need = [&](qint64 n) { return end - p >= n; };
    if (!need(8)) return false;
    if (qFromLittleEndian<quint32>(p) != kTocCacheMagic) return false;
    const quint32 sigLen = qFromLittleEndian<quint32>(p + 4);
    p += 8;
    if (!need(sigLen)) return false;
    if (QString::fromUtf8(p, int(sigLen)) != sig) return false;   // different game build
    p += sigLen;
    if (!need(4)) return false;
    const quint32 groups = qFromLittleEndian<quint32>(p);
    p += 4;
    for (quint32 g = 0; g < groups; ++g) {
        if (!need(8)) { clear(); return false; }
        const qint32 id = qint32(qFromLittleEndian<quint32>(p)); p += 4;
        const quint32 n = qFromLittleEndian<quint32>(p); p += 4;
        QVector<SnoEntry> entries;
        entries.reserve(int(n));
        for (quint32 i = 0; i < n; ++i) {
            if (!need(6)) { clear(); return false; }
            SnoEntry e;
            e.snoId = qint32(qFromLittleEndian<quint32>(p)); p += 4;
            const quint16 nl = qFromLittleEndian<quint16>(p); p += 2;
            if (!need(nl)) { clear(); return false; }
            e.name = QString::fromUtf8(p, nl); p += nl;
            entries.append(std::move(e));
        }
        m_total += entries.size();
        m_byGroup.insert(id, std::move(entries));
    }
    m_loaded = !m_byGroup.isEmpty();
    return m_loaded;
}

void SnoIndex::saveToCache(const QString& sig) const
{
    if (!m_loaded || sig.isEmpty()) return;
    const QString path = tocCachePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QString part = path + QStringLiteral(".part");
    QFile f(part);
    if (!f.open(QIODevice::WriteOnly)) return;
    QByteArray out;
    out.reserve(32 * 1024 * 1024);
    auto put32 = [&](quint32 v) { quint32 le = qToLittleEndian(v); out.append(reinterpret_cast<const char*>(&le), 4); };
    auto put16 = [&](quint16 v) { quint16 le = qToLittleEndian(v); out.append(reinterpret_cast<const char*>(&le), 2); };
    put32(kTocCacheMagic);
    const QByteArray sigU = sig.toUtf8();
    put32(quint32(sigU.size()));
    out.append(sigU);
    put32(quint32(m_byGroup.size()));
    for (auto it = m_byGroup.constBegin(); it != m_byGroup.constEnd(); ++it) {
        put32(quint32(it.key()));
        put32(quint32(it.value().size()));
        for (const SnoEntry& e : it.value()) {
            const QByteArray nu = e.name.toUtf8();
            put32(quint32(e.snoId));
            put16(quint16(qMin(nu.size(), qsizetype(0xFFFF))));
            out.append(nu.left(0xFFFF));
        }
    }
    f.write(out);
    f.close();
    QFile::remove(path);
    QFile::rename(part, path);
    qInfo("SnoIndex: CoreTOC cached (%lld MB) — next launch skips the parse",
          qint64(out.size()) / (1024 * 1024));
}

// ── "Latest" snapshot/diff ───────────────────────────────────────────────────────────────────
// File: magic · snapSig · snapCount·[i32 sno] · newSig · newCount·[i32 sno].
// snap* = the full SNO set at snapSig (the baseline to diff against); new* = the additions computed
// at the last build transition (kept so re-opening at the same build shows the same "Latest" set).
static constexpr quint32 kLatestMagic = 0x4C415431;   // 'LAT1'
static QString latestPath() { return AppPaths::dataDir() + QStringLiteral("/latest_v1.bin"); }

static void readLatest(QString& snapSig, QSet<int>& snap, QString& newSig, QSet<int>& newSet)
{
    QFile f(latestPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray all = f.readAll();
    const char* p = all.constData();
    const char* end = p + all.size();
    auto need = [&](qint64 n) { return end - p >= n; };
    if (!need(4) || qFromLittleEndian<quint32>(p) != kLatestMagic) return;
    p += 4;
    auto rdStr = [&](QString& out) -> bool {
        if (!need(4)) return false;
        const quint32 n = qFromLittleEndian<quint32>(p); p += 4;
        if (!need(n)) return false;
        out = QString::fromUtf8(p, int(n)); p += n; return true;
    };
    auto rdSet = [&](QSet<int>& out) -> bool {
        if (!need(4)) return false;
        const quint32 n = qFromLittleEndian<quint32>(p); p += 4;
        if (!need(qint64(n) * 4)) return false;
        out.reserve(int(n));
        for (quint32 i = 0; i < n; ++i) { out.insert(qint32(qFromLittleEndian<quint32>(p))); p += 4; }
        return true;
    };
    if (!rdStr(snapSig) || !rdSet(snap) || !rdStr(newSig) || !rdSet(newSet)) {
        snapSig.clear(); snap.clear(); newSig.clear(); newSet.clear();   // corrupt → treat as absent
    }
}

static void writeLatest(const QString& snapSig, const QSet<int>& snap,
                        const QString& newSig, const QSet<int>& newSet)
{
    const QString path = latestPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QString part = path + QStringLiteral(".part");
    QFile f(part);
    if (!f.open(QIODevice::WriteOnly)) return;
    QByteArray out;
    out.reserve(int((snap.size() + newSet.size()) * 4 + 64));
    auto put32 = [&](quint32 v) { quint32 le = qToLittleEndian(v); out.append(reinterpret_cast<const char*>(&le), 4); };
    auto putStr = [&](const QString& s) { const QByteArray u = s.toUtf8(); put32(quint32(u.size())); out.append(u); };
    auto putSet = [&](const QSet<int>& s) { put32(quint32(s.size())); for (int v : s) put32(quint32(v)); };
    put32(kLatestMagic);
    putStr(snapSig); putSet(snap);
    putStr(newSig);  putSet(newSet);
    f.write(out);
    f.close();
    QFile::remove(path);
    QFile::rename(part, path);
}

void SnoIndex::updateLatest(const QString& sig)
{
    m_newSnos.clear();
    if (!m_loaded || sig.isEmpty()) return;   // Latest needs a build id to detect updates

    QSet<int> cur;
    for (auto it = m_byGroup.constBegin(); it != m_byGroup.constEnd(); ++it)
        for (const SnoEntry& e : it.value()) cur.insert(e.snoId);

    QString snapSig, newSig; QSet<int> snap, storedNew;
    readLatest(snapSig, snap, newSig, storedNew);

    if (snapSig.isEmpty()) {                 // first run → establish the baseline; nothing new yet
        writeLatest(sig, cur, sig, {});
        return;
    }
    if (snapSig == sig) {                     // same build as the snapshot → keep the last diff
        m_newSnos = storedNew;
        return;
    }
    for (int s : cur) if (!snap.contains(s)) m_newSnos.insert(s);   // build changed → additions
    writeLatest(sig, cur, sig, m_newSnos);
    qInfo("SnoIndex: Latest — %lld new asset(s) since the previous build", qint64(m_newSnos.size()));
}

bool SnoIndex::loadFromD4data(const QString& d4dataDir)
{
    clear();
    if (d4dataDir.isEmpty())
        return false;
    const QString path = QDir(d4dataDir).filePath(QStringLiteral("json/base/CoreTOC.dat.json"));
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning().noquote() << "loadFromD4data: cannot open" << path;
        return false;
    }
    qInfo().noquote() << "loadFromD4data: parsing" << path;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning().noquote() << "loadFromD4data: JSON parse error:" << err.errorString();
        return false;
    }

    // { "<groupId>": { "<snoId>": "<name>", … }, … }
    QHash<int, QVector<SnoEntry>> parsed;
    const QJsonObject root = doc.object();
    for (auto git = root.constBegin(); git != root.constEnd(); ++git) {
        bool ok = false;
        const int gid = git.key().toInt(&ok);
        if (!ok)
            continue;
        const QJsonObject entries = git.value().toObject();
        QVector<SnoEntry>& vec = parsed[gid];
        vec.reserve(entries.size());
        for (auto eit = entries.constBegin(); eit != entries.constEnd(); ++eit) {
            bool ok2 = false;
            const int sno = eit.key().toInt(&ok2);
            if (!ok2) continue;
            // Same treatment as the binary CoreTOC path: an encrypted record arrives with no name,
            // and keeping it under an empty one is no better than dropping it — it sorts to the top
            // of every list and matches nothing. Synthesise the same "~unnamed_<sno>" so the two
            // load routes cannot disagree about what the index contains.
            QString nm = eit.value().toString().trimmed();
            if (nm.isEmpty()) nm = QStringLiteral("~unnamed_%1").arg(sno);
            vec.append(SnoEntry{sno, nm});
        }
    }
    return ingest(parsed);
}

QList<int> SnoIndex::groups() const
{
    QList<int> keys = m_byGroup.keys();
    std::sort(keys.begin(), keys.end());
    return keys;
}

const QVector<SnoEntry>& SnoIndex::entries(int group) const
{
    static const QVector<SnoEntry> kEmpty;
    auto it = m_byGroup.constFind(group);
    return it != m_byGroup.constEnd() ? it.value() : kEmpty;
}
