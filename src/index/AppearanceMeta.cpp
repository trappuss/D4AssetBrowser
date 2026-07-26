#include "index/AppearanceMeta.h"

#include "app/AppPaths.h"

#include "casc/CascReader.h"
#include "index/DadOverride.h"
#include "index/IconIndex.h"
#include "index/ItemDef.h"
#include "index/SnoIndex.h"
#include "text/StringTable.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVector>

#include <atomic>
#include <functional>
#include <chrono>
#include <thread>
#include <vector>

namespace {

constexpr int kGroupAppearance = 9;
constexpr int kCacheVersion = 21;  // v21: bind mount/pet/barding/trophy inventory icons (snoMount/
                                   //      snoCompanion appearance + direct-name route + unk_75d565b)
                                   // v20: d4dad pass collects all valid handles per appearance and
                                   //      prefers one with a local atlas sprite (kills blank icons)

const QHash<QString, QString>& classPrefix()
{
    static const QHash<QString, QString> m = {
        {"bar", "Barbarian"}, {"rog", "Rogue"}, {"sor", "Sorcerer"}, {"nec", "Necromancer"},
        {"dru", "Druid"}, {"spi", "Spiritborn"}, {"pal", "Paladin"}, {"war", "Warlock"}};
    return m;
}
const QHash<QString, QString>& slot2code()
{
    static const QHash<QString, QString> m = {
        {"helm", "hlm"}, {"chest", "trs"}, {"gloves", "glv"}, {"pants", "leg"}, {"boots", "bts"}};
    return m;
}
bool isSlot(const QString& s)
{
    static const QSet<QString> k = {"bts", "trs", "glv", "hlm", "leg"};
    return k.contains(s);
}

QString classPrefixForWord(const QString& word)
{
    const QString w = word.toLower();
    if (w.isEmpty() || w == "generic" || w == "any" || w == "all")
        return {};
    for (auto it = classPrefix().constBegin(); it != classPrefix().constEnd(); ++it) {
        const QString fl = it.value().toLower();
        if (w == fl || w == it.key() || fl.startsWith(w) || w.startsWith(it.key()))
            return it.key();
    }
    return {};
}

QStringList classesFromItem(const QString& iname)
{
    const QStringList toks = iname.toLower().split(QRegularExpression("[^a-z]+"), Qt::SkipEmptyParts);
    for (const QString& t : toks) {
        const QString p = classPrefixForWord(t);
        if (!p.isEmpty()) return {p};
    }
    return classPrefix().keys();
}

// fUsableByClass in the item JSON is an 8-element 0/1 mask in eHeroClass order —
// the ground truth for which classes an item serves (all-1 = all classes). Returns
// the matching class prefixes, or empty when the field is absent (→ callers fall
// back to the classesFromItem name heuristic).
QStringList classesFromMask(const QJsonObject& obj)
{
    const QJsonArray m = obj.value(QStringLiteral("fUsableByClass")).toArray();
    if (m.isEmpty()) return {};
    QStringList out;
    const int n = qMin(int(m.size()), int(AppearanceMeta::heroClassPrefixes().size()));
    for (int i = 0; i < n; ++i)
        if (m.at(i).toInt() == 1) out << AppearanceMeta::heroClassPrefixes().at(i);
    return out;
}

// Shared route regexes (also backing the public AppearanceMeta:: statics).
const QRegularExpression& cosmeticRex()
{
    static const QRegularExpression re("^(helm|chest|gloves|pants|boots)_cosmetic_(.+)$",
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}
const QRegularExpression& actorStyleRex()
{
    static const QRegularExpression re("^([A-Za-z]{3})_([a-z]+\\d+)$");
    return re;
}
const QRegularExpression& classGenderRex()
{
    static const QRegularExpression re("^([a-z]{3})([fm])_");
    return re;
}

QJsonObject loadJson(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

int rawOf(const QJsonObject& o, const QString& key)
{
    const QJsonObject sub = o.value(key).toObject();
    return sub.value("__raw__").toInt();
}

QString stringLabel(const QString& sldir, const QString& stem, const QString& label)
{
    QFile f(QStringLiteral("%1/%2.stl.json").arg(sldir, stem));
    if (!f.open(QIODevice::ReadOnly)) return {};
    for (const StringRow& r : parseStringTableJson(f.readAll()))
        if (r.label == label)
            return r.text;
    return {};
}
QString stringName(const QString& sldir, const QString& stem)
{
    return stringLabel(sldir, stem, QStringLiteral("Name"));
}

struct BuildResult {
    QHash<int, QSet<QString>> tags;
    QHash<int, QString>       titles;
    QHash<int, QString>       collections;
    QHash<int, quint32>       icons;       // appearance sno → inventory-icon handle
    QHash<quint32, QString>   iconNames;   // icon handle → item name (texframe labels)
    int                       cascFilled = 0;   // icons recovered by the CASC-item phase
};

// tInvImages is an array indexed by the eHeroClass enum (all 8 classes, incl. the
// datamined Paladin/Warlock); each entry holds that class's icon (hDefaultImage =
// male, hFemaleImage = female). ItemDef::heroClassIndex maps an appearance's
// 3-letter class code to that index so each class/gender gets ITS icon.
//
// Picks the handle for one class/gender from a per-class image table, with the
// standard fallbacks: male↔female within the class entry, then any populated
// entry (single-entry items store their icon in their own class slot only).
quint32 pickHandle(const QVector<QPair<quint32, quint32>>& inv, int classIdx, bool female)
{
    quint32 m = 0, f = 0;
    if (classIdx >= 0 && classIdx < inv.size()) {
        m = inv[classIdx].first;
        f = inv[classIdx].second;
    }
    if (!m && !f)
        for (const auto& e : inv)
            if (e.first || e.second) { m = e.first; f = e.second; break; }
    return female ? (f ? f : m) : (m ? m : f);
}

// The full Phase A + Phase B crawl (runs on a worker thread). `prog` (0..100) is
// called periodically so the UI can show indexing progress.
BuildResult crawl(const QString& d4, const SnoIndex* index, CascReader* reader,
                  const std::function<void(int)>& prog)
{
    BuildResult R;
    const QString meta = d4 + "/json/base/meta";
    const QString sldir = d4 + "/json/enUS_Text/meta/StringList";
    auto add = [&](int sno, const QString& tag) {
        if (sno > 0 && !tag.isEmpty()) R.tags[sno].insert(tag);
    };
    // diablo4.dad DB (may be absent) — patch-current per-class handles keyed by
    // item sno. Used in Phase B when the local snapshot's tInvImages lacks a
    // class entry (stale after a new-class patch) and in Phase D for delta items.
    DadOverride& dad = DadOverride::instance();
    dad.ensureLoaded();
    // diablo4.dad's per-class icon table keyed by item NAME (robust vs. a stale/missing
    // __snoID__ in the local JSON). This is the authoritative per-class source — the local
    // d4data snapshot lags patches and can carry class-0's icon in the newer spi/pal/war slots.
    QHash<QString, QVector<QPair<quint32, quint32>>> dadInvByName;
    for (auto it = dad.items().constBegin(); it != dad.items().constEnd(); ++it)
        if (!it.value().inv.isEmpty())
            dadInvByName.insert(it.value().stem.toLower(), it.value().inv);

    const QRegularExpression& classRe = classGenderRex();
    static const QRegularExpression devActorRe("(^|_)(test|dev|debug|placeholder)(_|$)",
                                               QRegularExpression::CaseInsensitiveOption);
    // Base/starter armour appearances (e.g. barM_base14_TRS) are class-specific but
    // only generic all-class loot references them, so a generic item's icon would
    // not depict this class's piece — those are skipped (→ 3D-render fallback).
    static const QRegularExpression baseStyleRe("_base\\d+_",
                                               QRegularExpression::CaseInsensitiveOption);
    // Bespoke/promo/quest/test items reuse base-armour meshes but carry their own
    // distinctive icon (BlizzConline promo, BarbarianGhost, rarespawn rewards, quest
    // pieces, QA/VFX tests). Their icon must never represent a base appearance.
    static const QRegularExpression bespokeRe(
        "blizzconline|rarespawn|ghost|corrupted|skeleton|(^|_)capped(_|$)|"
        "(^|_)qst(_|$)|(^|_)qa(_|$)|(^|_)test|_test|vfx|debug|placeholder",
        QRegularExpression::CaseInsensitiveOption);

    // ── Phase A: class + gender from the appearance naming convention ──
    QHash<QString, int> name2sno;
    QHash<int, QString> sno2name;
    if (index) {
        for (const SnoEntry& e : index->entries(kGroupAppearance)) {
            const QString low = e.name.toLower();
            name2sno.insert(low, e.snoId);
            sno2name.insert(e.snoId, low);
            const auto m = classRe.match(low);
            if (!m.hasMatch()) continue;
            const QString code = m.captured(1), g = m.captured(2);
            if (classPrefix().contains(code)) add(e.snoId, classPrefix().value(code));
            else if (code == "npc")           add(e.snoId, "npc");
            add(e.snoId, g == "f" ? "Female" : "Male");
        }
    }

    // ── ItemType → weapon/armor ──
    QHash<int, QString> ittCat;
    QDirIterator iit(meta + "/ItemType", QStringList{"*.itt.json"}, QDir::Files);
    while (iit.hasNext()) {
        const QJsonObject d = loadJson(iit.next());
        const int sno = d.value("__snoID__").toInt();
        const int wc = d.value("eWeaponClass").toInt(-1);
        const bool hasSlots = !d.value("arBodySlots").toArray().isEmpty();
        ittCat.insert(sno, wc >= 0 ? "weapon" : (hasSlots ? "armor" : QString()));
    }

    // ── Phase B: items → actor → appearance, tags + titles ──
    QHash<QString, QVector<int>> itemApp;   // item name → appearance snos (for StoreProduct)
    QHash<int, int>              iconSpec;   // app sno → icon specificity (class-specific wins)
    QHash<QString, quint32>      itemIcon;   // item name → handle (StoreProduct fallback)
    QSet<QString>                d4dataItems; // lowercased names present in d4data (delta detection)
    const QStringList itemFiles = QDir(meta + "/Item").entryList(QStringList{"*.itm.json"}, QDir::Files);
    const int itemTotal = qMax(1, itemFiles.size());
    const QString itemDir = meta + QStringLiteral("/Item");
    int itemSeen = 0;
    // Parse item JSONs in parallel in bounded chunks (the JSON parse is the dominant cost), then
    // process each in ORIGINAL order so the order-dependent icon-specificity / title rules stay
    // byte-identical to the old sequential crawl.
    constexpr int kItemChunk = 2048;
    for (int cs = 0; cs < itemFiles.size(); cs += kItemChunk) {
        const int ce = qMin(cs + kItemChunk, itemFiles.size());
        std::vector<QJsonObject> parsed(size_t(ce - cs));
        {
            const unsigned hw = std::thread::hardware_concurrency();
            const int nT = int(qBound(1u, hw ? hw : 4u, 8u));
            std::vector<std::thread> pool; pool.reserve(size_t(nT));
            for (int th = 0; th < nT; ++th)
                pool.emplace_back([&, th]() {
                    for (int i = cs + th; i < ce; i += nT)
                        parsed[size_t(i - cs)] = loadJson(itemDir + QLatin1Char('/') + itemFiles[i]);
                });
            for (auto& t : pool) t.join();
        }
        for (int fi = cs; fi < ce; ++fi) {
        if (prog && (++itemSeen % 256) == 0) prog(itemSeen * 88 / itemTotal);
        const QString iname = itemFiles[fi].chopped(9);   // strip ".itm.json"
        const QJsonObject obj = parsed[size_t(fi - cs)];
        if (obj.isEmpty()) continue;
        d4dataItems.insert(iname.toLower());
        const QString actor = obj.value("snoActor").toObject().value("name").toString();
        // Which classes does this item serve? fUsableByClass (8-element eHeroClass
        // mask) is authoritative; the name-token heuristic is the fallback.
        QStringList prefs = classesFromMask(obj);
        if (prefs.isEmpty()) prefs = classesFromItem(iname);
        QVector<int> apps;

        // Route 0 — mounts & pets: the visible MESH is the ridden/companion actor's appearance,
        // NOT the reins/item actor (snoActor). Resolve it directly so the item's inventory icon
        // (unk_75d565b) binds to the correct appearance sno — Models/Textures were otherwise blank
        // for mounts, pets, and their gear.
        {
            const QString itTypeName = obj.value("snoItemType").toObject().value("name").toString().toLower();
            if (itTypeName == QLatin1String("mountitem") || itTypeName.contains(QLatin1String("companion"))) {
                for (const char* key : { "snoMount", "snoCompanion" }) {
                    const QString mActor = obj.value(QLatin1String(key)).toObject().value("name").toString();
                    if (mActor.isEmpty() || devActorRe.match(mActor).hasMatch()) continue;
                    const int s = rawOf(loadJson(meta + "/Actor/" + mActor + ".acr.json"), "snoAppearance");
                    if (s > 0) { apps.append(s); break; }
                }
            }
        }

        // Route 1 — cosmetic item name → appearance name.
        for (const QString& nm : AppearanceMeta::cosmeticAppearanceNames(iname)) {
            const int s = name2sno.value(nm, 0);
            if (s) apps.append(s);
        }
        // Route 2 — the item's ACTOR is a SLOT_style name (HLM_sets53, TRS_uniq12 …);
        // the item's own name is often arbitrary, so the actor name is what encodes
        // the transmog style. Expand over the item's class set × both genders.
        if (apps.isEmpty()) {
            for (const QString& nm : AppearanceMeta::styleAppearanceNames(actor, prefs)) {
                const int s = name2sno.value(nm, 0);
                if (s) apps.append(s);
            }
        }
        if (apps.isEmpty() && !actor.isEmpty() && !devActorRe.match(actor).hasMatch()) {
            const QJsonObject a = loadJson(meta + "/Actor/" + actor + ".acr.json");
            const int s = rawOf(a, "snoAppearance");
            if (s > 0) apps.append(s);
        }
        // Route 4 — direct name match: barding/trophies (mnt_amor…, mnt_stor…_trophy) name their
        // appearance the same as the item stem, so bind the icon there when nothing else resolved.
        if (apps.isEmpty()) { const int s = name2sno.value(iname.toLower(), 0); if (s > 0) apps.append(s); }
        // Inventory icon handle: item.tInvImages.hDefaultImage / hFemaleImage, or
        // the unnamed field many store cosmetics/pets carry it in (Alkor case).
        // Per-class icon entries (index = eHeroClass). hd/hf keep the first non-empty
        // pair as a fallback for items whose appearance class isn't resolvable.
        quint32 hd = 0, hf = 0;
        QVector<QPair<quint32, quint32>> imgs;   // [class] → (male/default, female)
        for (const QJsonValue& iv : obj.value("tInvImages").toArray()) {
            const QJsonObject gi = iv.toObject();
            const quint32 a = quint32(gi.value("hDefaultImage").toDouble());
            const quint32 b = quint32(gi.value("hFemaleImage").toDouble());
            imgs.append({a, b});
            if (!hd && !hf && (a || b)) { hd = a ? a : b; hf = b ? b : a; }
        }
        if (!hd && !hf) hd = hf = quint32(obj.value("unk_75d565b").toDouble());
        // Every icon handle → this item's name, for texframe labels in the Textures tab.
        for (const auto& e : imgs) {
            if (e.first)  R.iconNames.insert(e.first, iname);
            if (e.second) R.iconNames.insert(e.second, iname);
        }
        if (hd || hf) itemIcon.insert(iname.toLower(), hd ? hd : hf);

        if (apps.isEmpty()) continue;

        const QJsonObject itt = obj.value("snoItemType").toObject();
        const QString ittname = itt.value("name").toString().toLower();
        const QString cat = ittCat.value(itt.value("__raw__").toInt());
        const bool transmog = obj.value("bIsTransmog").toBool();
        QString title = stringName(sldir, "Item_" + iname);
        if (title.isEmpty()) title = stringName(sldir, iname);
        if (title.startsWith('[')) title.clear();
        // class-specific items (resolve to ONE class) carry that class's real icon;
        // generic all-class items only fill gaps.
        const int spec = (prefs.size() == 1) ? 2 : 1;

        itemApp.insert(iname, apps);
        for (int s : apps) {
            if (!ittname.isEmpty()) add(s, ittname);
            if (!cat.isEmpty())     add(s, cat);
            if (transmog)           add(s, "transmog");
            if (!title.isEmpty() && !R.titles.contains(s)) R.titles.insert(s, title);
            // Icon specificity. Normally a class-specific item (one class) beats a
            // generic one. But base-armour appearances (barF_base02_HLM …) ARE the
            // generic loot look, so the "*_Generic_*" item depicts them — prefer it
            // over class-specific rewards/variants (Ghost, set pieces) that merely
            // reuse the base mesh with a bespoke icon. (eff: generic-base 3 wins,
            // non-generic-base 1 only fills a gap.)
            int eff = spec;
            if (baseStyleRe.match(sno2name.value(s)).hasMatch())
                eff = iname.contains(QLatin1String("generic"), Qt::CaseInsensitive) ? 3 : 1;
            // Bespoke/promo/quest/test items normally shouldn't bind their (distinctive) icon to a
            // shared base appearance. EXCEPTION: a "*_Generic_*" base item genuinely IS the generic
            // base look, so its icon is that appearance's icon — even though a QST/quest tag also
            // trips bespokeRe (e.g. X1_QST_Helm_Legendary_Generic_HatredSet → barF_base12_HLM).
            // NON-generic bespoke items get the weakest score (0): fill-if-empty ONLY. A rule must
            // never zero out an icon when the bespoke item is that appearance's only source — a
            // slightly-off icon beats a silently blank one (the old refuse-outright behaviour was
            // the recurring "icons vanish" bug pattern).
            if (baseStyleRe.match(sno2name.value(s)).hasMatch()
                && bespokeRe.match(iname).hasMatch()
                && !iname.contains(QLatin1String("generic"), Qt::CaseInsensitive))
                eff = 0;
            // Fill-if-empty semantics: an appearance with NO icon accepts any score
            // (cur = -1); an existing icon is only replaced by a strictly higher score.
            const int cur = R.icons.contains(s) ? iconSpec.value(s, 0) : -1;
            if ((hd || hf) && eff > cur) {
                const auto gm = classRe.match(sno2name.value(s));
                const bool female = gm.hasMatch() && gm.captured(2) == QLatin1String("f");
                const int ci = gm.hasMatch() ? ItemDef::heroClassIndex(gm.captured(1)) : -1;
                // diablo4.dad's authoritative per-class entry FIRST. The local d4data snapshot
                // lags patches and can duplicate class-0's icon into the newer spi/pal/war slots
                // — so trusting the local slot showed e.g. the Sorcerer art for every class of a
                // generic set. d4dad has the real per-class handle; using it here matches exactly
                // what the icon audit expects (pickHandle over the same d4dad table).
                quint32 handle = 0;
                if (ci >= 0) {
                    const auto dib = dadInvByName.constFind(iname.toLower());
                    if (dib != dadInvByName.constEnd())
                        handle = pickHandle(dib.value(), ci, female);
                }
                // …then this class's own entry in the LOCAL snapshot…
                if (!handle && ci >= 0 && ci < imgs.size()) {
                    const auto& e = imgs[ci];
                    handle = female ? (e.second ? e.second : e.first)
                                    : (e.first  ? e.first  : e.second);
                }
                // …then any populated class entry, then the unnamed-field (Alkor case).
                if (!handle) handle = pickHandle(imgs, ci, female);
                if (!handle) handle = female ? hf : hd;
                if (handle) { R.icons.insert(s, handle); iconSpec.insert(s, eff); }
            }
        }
        }   // for fi (process one parsed item, in order)
    }       // for cs (parallel-parsed chunk)

    // ── Phase C: Actor → entity Category (Player/Monster/NPC/Boss/Prop/…) ──
    // Each appearance is owned by the actor(s) that reference it, so we read the
    // actor's populated component block (pt*Data) straight from the game JSON and
    // tag the appearance it points at. Lightweight text scan (no full JSON parse):
    // the d4data files are pretty-printed, so "ptFooData": [  vs  [] distinguishes a
    // populated block, and the flags below live only inside ptMonsterData.
    auto popBlock = [](const QByteArray& t, const char* key) -> bool {
        const QByteArray k = QByteArray("\"") + key + "\": [";
        const int i = t.indexOf(k);
        if (i < 0) return false;
        int j = i + k.size();
        while (j < t.size() && (t[j] == ' ' || t[j] == '\t' || t[j] == '\n' || t[j] == '\r'))
            ++j;
        return j < t.size() && t[j] != ']';
    };
    auto appRawFromText = [](const QByteArray& t) -> int {
        const int i = t.indexOf("\"snoAppearance\"");
        if (i < 0) return 0;
        const int c = t.indexOf(':', i + 15);
        if (c < 0) return 0;
        int j = c + 1;
        while (j < t.size() && (t[j] == ' ' || t[j] == '\t' || t[j] == '\n' || t[j] == '\r'))
            ++j;
        if (j >= t.size() || t[j] != '{') return 0;   // null snoAppearance → no model
        int r = t.indexOf("\"__raw__\":", j);
        const int brace = t.indexOf('}', j);
        if (r < 0 || (brace >= 0 && r > brace)) return 0;
        r += 10;
        while (r < t.size() && t[r] == ' ') ++r;
        int e = r;
        while (e < t.size() && t[e] >= '0' && t[e] <= '9') ++e;
        return (e > r) ? t.mid(r, e - r).toInt() : 0;
    };
    // Phase C runs after the item loop (why the bar sat at ~88-98% before). The Actor folder
    // is the largest scan in the crawl, so run it in parallel across cores: each worker reads a
    // stripe of files into a private (appearance→categories) map, then we merge (categories are
    // purely additive, so there's no ordering/conflict issue). One directory scan, reused.
    const QString actorDir = meta + QStringLiteral("/Actor");
    const QStringList actorFiles = QDir(actorDir).entryList(QStringList{"*.acr.json"}, QDir::Files);
    const int actorTotal = qMax(1, actorFiles.size());
    {
        auto classify = [&](const QByteArray& t) -> QString {
            if (popBlock(t, "ptPlayerData"))       return QStringLiteral("Player");
            if (popBlock(t, "ptItemData"))         return QStringLiteral("Item");
            if (popBlock(t, "ptMountData"))        return QStringLiteral("Mount");
            if (popBlock(t, "ptCritterData"))      return QStringLiteral("Critter");
            if (popBlock(t, "ptMonsterData")) {
                if (t.contains("\"fIsNPC\": true"))            return QStringLiteral("NPC");
                if (t.contains("\"bIsWorldBoss\": true"))      return QStringLiteral("Boss");
                return QStringLiteral("Monster");
            }
            if (popBlock(t, "ptNPCData") && t.contains("\"snoNPCComponentSet\": {"))
                return QStringLiteral("NPC");
            if (popBlock(t, "ptPropData"))         return QStringLiteral("Prop");
            if (popBlock(t, "ptGizmoData"))        return QStringLiteral("Gizmo");
            return {};
        };
        const unsigned hw = std::thread::hardware_concurrency();
        const int nT = int(qBound(1u, hw ? hw : 4u, 8u));
        std::vector<QHash<int, QSet<QString>>> partials(static_cast<size_t>(nT));
        std::atomic<int> seen{0};
        std::vector<std::thread> pool;
        pool.reserve(size_t(nT));
        for (int th = 0; th < nT; ++th)
            pool.emplace_back([&, th]() {
                QHash<int, QSet<QString>>& part = partials[size_t(th)];
                for (int i = th; i < actorFiles.size(); i += nT) {
                    if (prog) { const int s = seen.fetch_add(1, std::memory_order_relaxed) + 1;
                        if ((s % 1024) == 0) prog(88 + s * 12 / actorTotal); }
                    QFile af(actorDir + QLatin1Char('/') + actorFiles[i]);
                    if (!af.open(QIODevice::ReadOnly)) continue;
                    const QByteArray t = af.readAll();
                    const int app = appRawFromText(t);
                    if (app <= 0) continue;
                    const QString c = classify(t);
                    if (!c.isEmpty()) part[app].insert(c);
                }
            });
        for (auto& t : pool) t.join();
        for (const auto& part : partials)
            for (auto it = part.constBegin(); it != part.constEnd(); ++it)
                for (const QString& c : it.value()) add(it.key(), c);
    }

    // ── StoreProduct → collection (Series) + product-name fallback (transmog path) ──
    QDirIterator pit(meta + "/StoreProduct", QStringList{"*.prd.json"}, QDir::Files);
    while (pit.hasNext()) {
        const QString fp = pit.next();
        const QString pname = pit.fileName().chopped(9);   // strip ".prd.json"
        const QJsonObject p = loadJson(fp);
        const QString tm = p.value("snoItemTransmog").toObject().value("name").toString();
        if (tm.isEmpty() || !itemApp.contains(tm)) continue;
        const QString stem = QStringLiteral("StoreProduct_%1").arg(pname);
        QString series = stringLabel(sldir, stem, QStringLiteral("Series"));
        series.remove('"');
        const QString pn = stringLabel(sldir, stem, QStringLiteral("Name"));
        const quint32 tmIcon = itemIcon.value(tm.toLower(), 0);   // item's first/default icon
        const auto dibIt = dadInvByName.constFind(tm.toLower());  // its authoritative per-class table
        for (int app : itemApp.value(tm)) {
            if (!series.isEmpty()) R.collections.insert(app, series);
            if (!pn.isEmpty() && !R.titles.contains(app)) R.titles.insert(app, pn);
            if (R.icons.contains(app)) continue;
            // Per-class icon for THIS appearance's class (spi/pal/war get their own art, not the
            // generic set's class-0/Sorcerer icon). Falls back to the single item icon.
            quint32 h = 0;
            const auto gm = classRe.match(sno2name.value(app));
            if (gm.hasMatch() && dibIt != dadInvByName.constEnd()) {
                const bool female = gm.captured(2) == QLatin1String("f");
                const int ci = ItemDef::heroClassIndex(gm.captured(1));
                if (ci >= 0) h = pickHandle(dibIt.value(), ci, female);
            }
            if (!h) h = tmIcon;
            if (h) R.icons.insert(app, h);
        }
    }

    // ── Phase D: delta recovery — items ABSENT from the d4data snapshot ──────
    // d4data lags the live game, so items added since the snapshot never enter
    // Phase B and their appearances get no icon — the exact "icons silently vanish
    // after an update" failure. CoreTOC (group 73) is always current; walk the delta:
    //   appearances ← cosmetic name rule, else ItemDefinition binary → snoActor →
    //                 actor SLOT_style rule expanded over all 8 classes;
    //   handles     ← diablo4.dad per-class handles (patch-current, keyed by item
    //                 sno; see DadOverride) when cached, else the binary tInvImages.
    // Fill-if-empty only: a delta item can never overwrite a Phase B icon.
    if (reader && reader->isReady() && index) {
        QHash<quint32, QString> actorNames;
        for (const SnoEntry& a : index->entries(1))   // group 1 = Actor
            actorNames.insert(quint32(a.snoId), a.name);
        int deltaSeen = 0, deltaFilled = 0;
        for (const SnoEntry& it : index->entries(73)) {
            const QString lname = it.name.toLower();
            if (d4dataItems.contains(lname))
                continue;   // d4data has this item; Phase B already did its best
            ++deltaSeen;
            const auto dit = dad.items().constFind(it.snoId);
            const DadItem* di = (dit != dad.items().constEnd()) ? &dit.value() : nullptr;

            // Route 1 (cosmetic name) needs no CASC read; route 2 (actor style) does.
            QStringList candNames = AppearanceMeta::cosmeticAppearanceNames(lname);
            ItemDef::ItemInfo info;
            bool haveInfo = false;
            if (candNames.isEmpty()) {
                info = ItemDef::parseItem(reader->readMetaBySno(quint64(it.snoId)));
                haveInfo = true;
                if (info.snoActor)
                    candNames = AppearanceMeta::styleAppearanceNames(
                        actorNames.value(info.snoActor));
            }
            for (const QString& nm : candNames) {
                const int s = name2sno.value(nm, 0);
                if (!s || R.icons.contains(s))
                    continue;
                if (!haveInfo && (!di || di->inv.isEmpty())) {
                    info = ItemDef::parseItem(reader->readMetaBySno(quint64(it.snoId)));
                    haveInfo = true;
                }
                const auto gm = classGenderRex().match(nm);
                const bool female = gm.hasMatch() && gm.captured(2) == QLatin1String("f");
                const int ci = gm.hasMatch() ? ItemDef::heroClassIndex(gm.captured(1)) : -1;
                quint32 h = (di && !di->inv.isEmpty()) ? pickHandle(di->inv, ci, female) : 0;
                if (!h && haveInfo && info.valid) h = pickHandle(info.images, ci, female);
                if (!h && di) h = di->icon;   // diablo4.dad's "best" single handle
                if (h) {
                    R.icons.insert(s, h);
                    if (!R.iconNames.contains(h)) R.iconNames.insert(h, it.name);
                    ++deltaFilled;
                    ++R.cascFilled;
                }
            }
        }
        qInfo("AppearanceMeta: delta phase — %d items absent from d4data, %d icons filled (d4dad db: %s)",
              deltaSeen, deltaFilled, dad.items().isEmpty() ? "absent" : "loaded");
    }

    // ── Phase D2: name-link mop-up (unique/cosmetic items) ────────────────────
    // For items whose NAME encodes the transmog appearance (Helm_Unique_Barbarian_95
    // → barF/barM_uniq95_HLM), read the ItemDefinition binary from CASC to recover the
    // inventory-icon handle — this also covers items IN d4data whose JSON lost its
    // tInvImages, and items whose actor route failed above. Safety: only fills
    // appearances that (a) exist in the index and (b) have no icon yet — a wrong
    // name-guess can never overwrite a correct icon.
    if (reader && reader->isReady()) {
        static const QHash<QString, QString> slotWord = {
            {QStringLiteral("helm"), QStringLiteral("hlm")}, {QStringLiteral("chest"), QStringLiteral("trs")},
            {QStringLiteral("gloves"), QStringLiteral("glv")}, {QStringLiteral("pants"), QStringLiteral("leg")},
            {QStringLiteral("boots"), QStringLiteral("bts")}};
        // Recovered categories are those with a granting item whose NAME encodes the
        // transmog appearance: Unique ("Helm_Unique_Barbarian_95" → uniq95) and Cosmetic
        // ("Helm_Cosmetic_Barb_150_stor" → stor150). Set/generic/pvp looks have no such
        // item (they're granted via actors/StoreProduct) so aren't reachable this way.
        const QVector<SnoEntry>& items = index->entries(73);   // Item group
        for (const SnoEntry& it : items) {
            const QString lname = it.name.toLower();
            if (!lname.contains(QStringLiteral("unique")) &&
                !lname.contains(QStringLiteral("cosmetic")) && !lname.contains(QStringLiteral("stor")))
                continue;   // only categories a name link can resolve
            const QStringList toks = lname.split(QLatin1Char('_'), Qt::SkipEmptyParts);
            if (toks.size() < 3) continue;

            QString slot;
            for (const QString& t : toks) { const auto s = slotWord.constFind(t);
                if (s != slotWord.constEnd()) { slot = s.value(); break; } }
            if (slot.isEmpty()) continue;
            QString cls;
            for (const QString& t : toks) { const QString p = classPrefixForWord(t);
                if (!p.isEmpty()) { cls = p; break; } }
            const int ci = ItemDef::heroClassIndex(cls);
            if (ci < 0) continue;
            QString numRaw;
            for (const QString& t : toks) { bool ok = false; t.toInt(&ok); if (ok) { numRaw = t; break; } }
            if (numRaw.isEmpty()) continue;

            const QString token = lname.contains(QStringLiteral("unique"))
                                      ? QStringLiteral("uniq") : QStringLiteral("stor");
            const QString numStripped = QString::number(numRaw.toInt());

            // Read the item's per-class handle only when a derived appearance is actually missing.
            bool read = false; quint32 hM = 0, hF = 0;
            auto ensureHandle = [&]() {
                if (read) return;
                read = true;
                const ItemDef::ItemInfo info = ItemDef::parseItem(reader->readMetaBySno(quint64(it.snoId)));
                if (!info.valid || info.images.size() <= ci) return;
                quint32 m = info.images[ci].first, f = info.images[ci].second;
                if (!m && !f)
                    for (const auto& e : info.images)
                        if (e.first || e.second) { m = e.first ? e.first : e.second;
                                                   f = e.second ? e.second : e.first; break; }
                hM = m ? m : f;
                hF = f ? f : m;
            };

            QStringList nums{ numStripped };
            if (numRaw != numStripped) nums << numRaw;
            for (const QString& num : nums)
                for (const QString& g : { QStringLiteral("f"), QStringLiteral("m") }) {
                    const QString appNm = cls + g + QLatin1Char('_') + token + num + QLatin1Char('_') + slot;
                    const int appSno = name2sno.value(appNm, 0);
                    if (!appSno || R.icons.contains(appSno)) continue;
                    ensureHandle();
                    const quint32 h = (g == QLatin1String("f")) ? hF : hM;
                    if (h) { R.icons.insert(appSno, h); ++R.cascFilled; }
                }
        }
    }

    // ── Final authoritative pass: diablo4.dad per-class handles WIN ───────────────────────
    // Derive appearances DIRECTLY from every diablo4.dad item (same rules as the Icon audit:
    // cosmetic-name route, else the item's ACTOR → style names, reading the actor from the local
    // JSON or — for items the local d4data snapshot lacks entirely, e.g. new-patch generic sets —
    // from the CASC ItemDefinition). For each derived appearance, force d4dad's per-class handle.
    // d4dad is the reference the audit checks against and is refreshed promptly after patches, so
    // this makes the tool match it. Runs LAST + unconditionally (overrides StoreProduct etc.);
    // never blanks an icon (0 handles are skipped).
    {
        // The sprite index is kicked off alongside this crawl (the tab calls both ensureBuilt).
        // It's fast, but may still be finishing — wait briefly (bounded) so the sprite preference
        // below is meaningful. Runs on the crawl's background thread, so it never blocks the UI.
        for (int w = 0; w < 200 && !IconIndex::instance().ready(); ++w)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));

        // Collect ALL valid d4dad handles per appearance (exactly the audit's expected set),
        // then choose one — PREFERRING a handle that has a local atlas sprite so the icon isn't
        // blank. Every choice is still a valid d4dad handle, so this keeps the audit at 0 diffs.
        QHash<int, QVector<quint32>> validByApp;
        const QStringList& allPrefs = AppearanceMeta::heroClassPrefixes();
        QHash<quint32, QString> actorNames;   // actor sno → name (for the CASC-actor fallback)
        if (index)
            for (const SnoEntry& a : index->entries(1))   // group 1 = Actor
                actorNames.insert(quint32(a.snoId), a.name);
        for (auto dit = dad.items().constBegin(); dit != dad.items().constEnd(); ++dit) {
            const DadItem& di = dit.value();
            if (di.inv.isEmpty() && !di.icon) continue;
            QStringList classPrefs;                       // usable-class restriction (empty = all)
            if (!di.usable.isEmpty()) {
                const int n = qMin(di.usable.size(), allPrefs.size());
                for (int i = 0; i < n; ++i) if (di.usable.at(i)) classPrefs << allPrefs.at(i);
            }
            QStringList candNames = AppearanceMeta::cosmeticAppearanceNames(di.stem.toLower());
            if (candNames.isEmpty()) {
                QString actor;
                QFile jf(meta + QStringLiteral("/Item/") + di.stem + QStringLiteral(".itm.json"));
                if (jf.open(QIODevice::ReadOnly))
                    actor = QJsonDocument::fromJson(jf.readAll()).object()
                                .value(QStringLiteral("snoActor")).toObject()
                                .value(QStringLiteral("name")).toString();
                if (actor.isEmpty() && reader && reader->isReady()) {
                    const ItemDef::ItemInfo info = ItemDef::parseItem(reader->readMetaBySno(quint64(dit.key())));
                    if (info.snoActor) actor = actorNames.value(info.snoActor);
                }
                candNames = AppearanceMeta::styleAppearanceNames(actor, classPrefs);
            }
            for (const QString& nm : candNames) {
                const int app = name2sno.value(nm, 0);
                if (!app) continue;
                const auto gm = classRe.match(nm);
                const bool female = gm.hasMatch() && gm.captured(2) == QLatin1String("f");
                const int ci = gm.hasMatch() ? ItemDef::heroClassIndex(gm.captured(1)) : -1;
                quint32 h = di.inv.isEmpty() ? 0 : pickHandle(di.inv, ci, female);
                if (!h) h = di.icon;
                if (h && !validByApp[app].contains(h)) validByApp[app].append(h);
            }
        }
        int forced = 0, spritePref = 0;
        const IconIndex& II = IconIndex::instance();
        const bool spriteReady = II.ready();
        for (auto it = validByApp.constBegin(); it != validByApp.constEnd(); ++it) {
            const QVector<quint32>& cands = it.value();
            if (cands.isEmpty()) continue;
            quint32 chosen = cands.first();
            if (spriteReady && !II.has(chosen))
                for (quint32 c : cands) if (II.has(c)) { chosen = c; ++spritePref; break; }
            if (R.icons.value(it.key()) != chosen) { R.icons.insert(it.key(), chosen); ++forced; }
        }
        qInfo("AppearanceMeta: d4dad authoritative pass — %d icon(s) forced, %d sprite-preferred (%ssprite-aware)",
              forced, spritePref, spriteReady ? "" : "NOT ");
    }

    return R;
}

}  // namespace

AppearanceMeta& AppearanceMeta::instance()
{
    static AppearanceMeta inst;
    return inst;
}

void AppearanceMeta::install(QHash<int, QSet<QString>> tags, QHash<int, QString> titles,
                             QHash<int, QString> collections, QHash<int, quint32> icons,
                             QHash<quint32, QString> iconNames)
{
    m_tags = std::move(tags);
    m_titles = std::move(titles);
    m_collections = std::move(collections);
    m_icons = std::move(icons);
    m_iconNames = std::move(iconNames);
    m_building = false;
    m_ready = true;
    emit readyChanged();
}

void AppearanceMeta::reset()
{
    m_ready = false;
    m_building = false;   // clear any in-flight build flag, else ensureBuilt() never rebuilds
    m_tags.clear();
    m_titles.clear();
    m_collections.clear();
    m_icons.clear();
    m_iconNames.clear();
    const QString cachePath = AppPaths::dataDir()
                              + QStringLiteral("/appearance_meta_v%1.json").arg(kCacheVersion);
    QFile::remove(cachePath);
    emit readyChanged();
}

void AppearanceMeta::ensureBuilt(const QString& d4dataDir, const SnoIndex* index, CascReader* reader)
{
    if (m_ready || m_building || d4dataDir.isEmpty() || !index)
        return;
    m_building = true;

    const QString cacheBase = AppPaths::dataDir();
    const QString cachePath = cacheBase + QStringLiteral("/appearance_meta_v%1.json").arg(kCacheVersion);
    const int appCount = index->entries(kGroupAppearance).size();
    // The diablo4.dad DB feeds the delta phase, so a refreshed d4dad.json must
    // invalidate this cache even when the game/d4data fingerprint didn't change.
    const QFileInfo dadFi(DadOverride::defaultPath());
    const QString dadSig = dadFi.exists()
        ? QStringLiteral("%1:%2").arg(dadFi.size()).arg(dadFi.lastModified().toSecsSinceEpoch())
        : QStringLiteral("none");

    const SnoIndex* idx = index;
    CascReader* rdr = reader;
    const QString d4 = d4dataDir;
    const QString cb = cacheBase, cp = cachePath, ds = dadSig;
    const int ac = appCount;
    // EVERYTHING on a detached worker thread — including the disk-cache try. The cache is a
    // multi-megabyte JSON over ~67k appearances; parsing it inline (the old "instant" path)
    // stalled the first tab refresh for the whole parse. Results marshal back queued.
    std::thread([this, d4, idx, rdr, cb, cp, ac, ds]() {
        if (QFile::exists(cp)) {
            QFile f(cp);
            if (f.open(QIODevice::ReadOnly)) {
                const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
                if (root.value("appCount").toInt() == ac
                    && root.value("dadSig").toString() == ds) {
                    QHash<int, QSet<QString>> tags;
                    QHash<int, QString> titles;
                    const QJsonObject jt = root.value("tags").toObject();
                    for (auto i = jt.constBegin(); i != jt.constEnd(); ++i) {
                        QSet<QString> s;
                        const QJsonArray arr = i.value().toArray();
                        for (int k = 0; k < arr.size(); ++k) s.insert(arr.at(k).toString());
                        tags.insert(i.key().toInt(), s);
                    }
                    const QJsonObject jn = root.value("titles").toObject();
                    for (auto i = jn.constBegin(); i != jn.constEnd(); ++i)
                        titles.insert(i.key().toInt(), i.value().toString());
                    QHash<int, QString> collections;
                    const QJsonObject jc = root.value("collections").toObject();
                    for (auto i = jc.constBegin(); i != jc.constEnd(); ++i)
                        collections.insert(i.key().toInt(), i.value().toString());
                    QHash<int, quint32> icons;
                    const QJsonObject ji = root.value("icons").toObject();
                    for (auto i = ji.constBegin(); i != ji.constEnd(); ++i)
                        icons.insert(i.key().toInt(), quint32(i.value().toDouble()));
                    QHash<quint32, QString> iconNames;
                    const QJsonObject jin = root.value("iconNames").toObject();
                    for (auto i = jin.constBegin(); i != jin.constEnd(); ++i)
                        iconNames.insert(i.key().toUInt(), i.value().toString());
                    QMetaObject::invokeMethod(this,
                        [this, tags, titles, collections, icons, iconNames]() mutable {
                            install(std::move(tags), std::move(titles), std::move(collections),
                                    std::move(icons), std::move(iconNames));
                        }, Qt::QueuedConnection);
                    return;
                }
            }
        }
        auto prog = [this](int pct) {
            QMetaObject::invokeMethod(this, [this, pct]() { emit progress(pct); },
                                      Qt::QueuedConnection);
        };
        BuildResult r = crawl(d4, idx, rdr, prog);
        QMetaObject::invokeMethod(this, [this, r, cb, cp, ac, ds]() {
            qInfo("AppearanceMeta: CASC-item phase recovered %d icons missing from d4data", r.cascFilled);
            QDir().mkpath(cb);
            QJsonObject root, jt, jn, jc, ji;
            for (auto i = r.tags.constBegin(); i != r.tags.constEnd(); ++i) {
                QJsonArray a; for (const QString& t : i.value()) a.append(t);
                jt.insert(QString::number(i.key()), a);
            }
            for (auto i = r.titles.constBegin(); i != r.titles.constEnd(); ++i)
                jn.insert(QString::number(i.key()), i.value());
            for (auto i = r.collections.constBegin(); i != r.collections.constEnd(); ++i)
                jc.insert(QString::number(i.key()), i.value());
            for (auto i = r.icons.constBegin(); i != r.icons.constEnd(); ++i)
                ji.insert(QString::number(i.key()), double(i.value()));
            QJsonObject jin;
            for (auto i = r.iconNames.constBegin(); i != r.iconNames.constEnd(); ++i)
                jin.insert(QString::number(i.key()), i.value());
            root.insert("appCount", ac);
            root.insert("dadSig", ds);
            root.insert("tags", jt);
            root.insert("titles", jn);
            root.insert("collections", jc);
            root.insert("icons", ji);
            root.insert("iconNames", jin);
            QFile f(cp);
            if (f.open(QIODevice::WriteOnly))
                f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
            install(r.tags, r.titles, r.collections, r.icons, r.iconNames);
        }, Qt::QueuedConnection);
    }).detach();
}

const QStringList& AppearanceMeta::heroClassPrefixes()
{
    // eHeroClass order — index into this list == index into tInvImages/fUsableByClass.
    static const QStringList o{
        QStringLiteral("sor"), QStringLiteral("dru"), QStringLiteral("bar"),
        QStringLiteral("rog"), QStringLiteral("nec"), QStringLiteral("spi"),
        QStringLiteral("pal"), QStringLiteral("war")};
    return o;
}

QString AppearanceMeta::classDisplayName(const QString& prefix)
{
    // Unknown prefix → upper-cased code, so a class this build doesn't know still LISTS
    // (graceful-unknown) instead of vanishing from filters until the tool is updated.
    return classPrefix().value(prefix.toLower(), prefix.toUpper());
}

QString AppearanceMeta::classPrefixPattern()
{
    return QStringLiteral("(%1)").arg(heroClassPrefixes().join(QLatin1Char('|')));
}

QStringList AppearanceMeta::cosmeticAppearanceNames(const QString& itemName)
{
    QStringList out;
    const auto cm = cosmeticRex().match(itemName);
    if (!cm.hasMatch())
        return out;
    const QString slot = slot2code().value(cm.captured(1).toLower());
    const QStringList parts = cm.captured(2).toLower().split('_');
    if (slot.isEmpty() || parts.size() < 3)
        return out;
    // "Helm_Cosmetic_Barb_150_stor": class word, numeric id, suffix → "stor150".
    const QString suffix = parts.last();
    const QString x = QStringList(parts.mid(1, parts.size() - 2)).join('_');
    bool isNum = false;
    const int xn = x.toInt(&isNum);
    const QString token = isNum ? (suffix + QString::number(xn)) : x;
    const QString pc = classPrefixForWord(parts.first());
    const QStringList prefs = pc.isEmpty() ? heroClassPrefixes() : QStringList{pc};
    for (const QString& pref : prefs)
        for (const QString& g : {QStringLiteral("f"), QStringLiteral("m")})
            out << QStringLiteral("%1%2_%3_%4").arg(pref, g, token, slot);
    return out;
}

QStringList AppearanceMeta::styleAppearanceNames(const QString& actorName,
                                                 const QStringList& classPrefixes)
{
    QStringList out;
    const auto sm = actorStyleRex().match(actorName);
    if (!sm.hasMatch() || !isSlot(sm.captured(1).toLower()))
        return out;
    const QString slot = sm.captured(1).toLower(), styletok = sm.captured(2).toLower();
    const QStringList prefs = classPrefixes.isEmpty() ? heroClassPrefixes() : classPrefixes;
    for (const QString& pref : prefs)
        for (const QString& g : {QStringLiteral("f"), QStringLiteral("m")})
            out << QStringLiteral("%1%2_%3_%4").arg(pref, g, styletok, slot);
    return out;
}

QMap<QString, QStringList> AppearanceMeta::tagGroups() const
{
    // Actor-derived entity categories (Phase C). Surfaced as their own dropdown and
    // ordered most-useful-first rather than alphabetically.
    static const QStringList kCategoryOrder = {
        "Player", "Monster", "Boss", "NPC", "Critter",
        "Mount", "Prop", "Item", "Gizmo"};
    const QSet<QString> kCategory(kCategoryOrder.begin(), kCategoryOrder.end());

    QSet<QString> cls, catSeen;
    QHash<QString, int> typeCount;
    for (const QSet<QString>& s : m_tags) {
        for (const QString& t : s) {
            if (kCategory.contains(t))                    catSeen.insert(t);
            else if (t == "npc")                          { /* entity kind — already covered by Category "NPC"; not a class */ }
            else if (classPrefix().values().contains(t))  cls.insert(t);
            else if (t != "Female" && t != "Male")        typeCount[t] += 1;
        }
    }
    QStringList categories;
    for (const QString& c : kCategoryOrder)
        if (catSeen.contains(c)) categories.append(c);
    QStringList classes;
    for (const QString& c : cls) classes.append(c);
    classes.sort();
    QStringList types;
    for (auto i = typeCount.constBegin(); i != typeCount.constEnd(); ++i)
        if (i.value() >= 20) types.append(i.key());
    types.sort();
    QMap<QString, QStringList> g;
    g.insert("Category", categories);
    g.insert("Class", classes);
    g.insert("Gender", {"Female", "Male"});
    g.insert("Type", types);
    return g;
}
