#include "index/ItemHoverIndex.h"

#include "app/AppPaths.h"
#include "index/AppearanceMeta.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <thread>

namespace {
// v2: route 3 — weapons, mounts, trophies and headstones now get hover entries, so a v1 file is
// missing whole categories rather than merely being older. The 3→4 field signature change would
// have forced a rebuild anyway, but "it happens to invalidate" is not the rule; the rule is that
// a change in WHAT IS INCLUDED bumps the filename.
constexpr int kCacheVersion = 2;

// Basename (no path, no extension) of a d4data sno reference's "__targetFileName__" —
// e.g. {"__targetFileName__": "base/meta/Season/Season 1.sea"} → "Season 1". Tolerates a
// missing/reshaped ref by returning empty (the caller omits that field).
QString refBaseName(const QJsonValue& v)
{
    if (!v.isObject()) return {};
    const QString t = v.toObject().value(QStringLiteral("__targetFileName__")).toString();
    if (t.isEmpty()) return {};
    return QFileInfo(t).completeBaseName();
}

// Read a StringList (.stl.json) into label → text. Tolerant: a missing file or reshaped array
// yields an empty map, so every caller degrades to "no line" rather than breaking.
QHash<QString, QString> readStrings(const QString& path)
{
    QHash<QString, QString> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).object()
                               .value(QStringLiteral("arStrings")).toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        const QString l = o.value(QStringLiteral("szLabel")).toString();
        if (!l.isEmpty()) out.insert(l, o.value(QStringLiteral("szText")).toString());
    }
    return out;
}
}  // namespace

ItemHoverIndex& ItemHoverIndex::instance()
{
    static ItemHoverIndex inst;
    return inst;
}

QString ItemHoverIndex::rarityLabel(int r)
{
    switch (r) {
        case 0: return QStringLiteral("Normal");
        case 1: return QStringLiteral("Magic");
        case 2: return QStringLiteral("Rare");
        case 3: return QStringLiteral("Legendary");
        case 4: return QStringLiteral("Unique");
        case 5: return QStringLiteral("Set");
        case 6: return QStringLiteral("Mythic");
        default: return r >= 0 ? QStringLiteral("quality %1").arg(r) : QString();
    }
}

const char* ItemHoverIndex::rarityColor(int r)
{
    switch (r) {
        case 0: return "#c8c8c8";   // Normal — bone white
        case 1: return "#6d8ce0";   // Magic — blue
        case 2: return "#e8d24a";   // Rare — yellow
        case 3: return "#b07b3e";   // Legendary — orange-brown
        case 4: return "#8a6bb5";   // Unique — purple-ish
        case 5: return "#3fa34d";   // Set — green
        case 6: return "#d94f4f";   // Mythic — red
        default: return "#b0b0b0";  // unknown/new quality — neutral
    }
}

void ItemHoverIndex::install(QHash<QString, Info> map)
{
    m_byAppearance = std::move(map);
    m_building = false;
    m_ready = true;
    emit readyChanged();
}

void ItemHoverIndex::reset()
{
    m_ready = false;
    m_building = false;
    m_byAppearance.clear();
    QFile::remove(AppPaths::dataDir()
                  + QStringLiteral("/item_hover_v%1.json").arg(kCacheVersion));
    emit readyChanged();
}

void ItemHoverIndex::ensureBuilt(const QString& d4dataDir)
{
    if (m_ready || m_building || d4dataDir.isEmpty()) return;
    const QString itemDir = d4dataDir + QStringLiteral("/json/base/meta/Item");
    if (!QDir(itemDir).exists()) return;
    m_building = true;

    const QString stlDir  = d4dataDir + QStringLiteral("/json/enUS_Text/meta/StringList");
    const QString prdDir  = d4dataDir + QStringLiteral("/json/base/meta/StoreProduct");
    // Read by the route-3 existence test below, so it is an INPUT to this index and therefore
    // belongs in the signature — see the note there.
    const QString apprDir = d4dataDir + QStringLiteral("/json/base/meta/Appearance");
    const QString cachePath = AppPaths::dataDir()
                              + QStringLiteral("/item_hover_v%1.json").arg(kCacheVersion);

    std::thread([this, itemDir, stlDir, prdDir, apprDir, cachePath, d4dataDir]() {
        // Signature: the file count of EVERY directory this build reads, plus the snapshot's build
        // stamp. Any patch / d4data commit changes at least one of these → automatic rebuild.
        //
        // The rule that matters is "every directory this build reads": twice now an index has
        // shipped a signature that could not see one of its own inputs, and a stale cache then
        // survived a change it should have been invalidated by. When you add a directory to this
        // build, add it here in the same edit.
        // nItm is hoisted out of the signature block because pass 1 walks exactly that file set —
        // so the count the signature already pays for doubles as the progress denominator, with
        // no second directory walk.
        int nItm = 0;
        QString sig;
        {
            int nStl = 0, nApp = 0;
            { QDirIterator c(itemDir, {QStringLiteral("*.itm.json")}, QDir::Files); while (c.hasNext()) { c.next(); ++nItm; } }
            { QDirIterator c(stlDir,  {QStringLiteral("*.stl.json")}, QDir::Files); while (c.hasNext()) { c.next(); ++nStl; } }
            { QDirIterator c(apprDir, {QStringLiteral("*.app.json")}, QDir::Files); while (c.hasNext()) { c.next(); ++nApp; } }
            QString bv;
            QFile f(d4dataDir + QStringLiteral("/buildVersion.txt"));
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) bv = QString::fromUtf8(f.readAll()).trimmed();
            sig = QStringLiteral("%1|%2|%3|%4").arg(nItm).arg(nStl).arg(nApp).arg(bv);
        }

        // Cache hit?
        {
            QFile f(cachePath);
            if (f.open(QIODevice::ReadOnly)) {
                const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
                if (root.value(QStringLiteral("sig")).toString() == sig) {
                    QHash<QString, Info> map;
                    const QJsonObject e = root.value(QStringLiteral("appearances")).toObject();
                    for (auto it = e.constBegin(); it != e.constEnd(); ++it) {
                        const QJsonObject o = it.value().toObject();
                        Info inf;
                        inf.rarity     = o.value(QStringLiteral("r")).toInt(-1);
                        inf.seasonItem = o.value(QStringLiteral("s")).toBool();
                        inf.itemName   = o.value(QStringLiteral("n")).toString();
                        inf.desc       = o.value(QStringLiteral("d")).toString();
                        inf.flavor     = o.value(QStringLiteral("f")).toString();
                        inf.transmogName = o.value(QStringLiteral("t")).toString();
                        inf.introducedIn = o.value(QStringLiteral("i")).toString();
                        inf.collDesc   = o.value(QStringLiteral("cd")).toString();
                        inf.collQuote  = o.value(QStringLiteral("cq")).toString();
                        map.insert(it.key(), inf);
                    }
                    QMetaObject::invokeMethod(this, [this, map]() mutable { install(std::move(map)); },
                                              Qt::QueuedConnection);
                    return;
                }
            }
        }

        // Pass 1 — cosmetic Items. Name pre-filter via the SHARED appearance-derivation rules
        // (no file open unless the item can map to appearances at all).
        QHash<QString, Info> map;                    // lower appearance → info
        QHash<QString, QStringList> itemToAppear;    // lower item stem → its appearance names
        QDirIterator it(itemDir, {QStringLiteral("*.itm.json")}, QDir::Files);
        int seen = 0, lastPct = -1;
        while (it.hasNext()) {
            const QString path = it.next();
            // Throttled to whole-percent changes: this loop runs tens of thousands of times and a
            // queued emit per iteration would cost more than the work being measured.
            if (nItm > 0) {
                const int pct = int(qint64(++seen) * 100 / nItm);
                if (pct != lastPct) {
                    lastPct = pct;
                    QMetaObject::invokeMethod(this, [this, pct]() { emit progress(pct); },
                                              Qt::QueuedConnection);
                }
            }
            const QString stem = QFileInfo(path).fileName().section(QStringLiteral(".itm.json"), 0, 0);
            QStringList apps = AppearanceMeta::cosmeticAppearanceNames(stem);
            // Route 3 — but TESTED, not assumed. Appending the item's own name unconditionally
            // would defeat the pre-filter this loop depends on and open every item file in the
            // snapshot instead of only the cosmetic ones. A stat is orders of magnitude cheaper
            // than the parse it avoids, and it is exact rather than a name heuristic.
            //
            // Without this, weapons had no hover entry anywhere in the tool, and pass 2 below —
            // which joins store products through itemToAppear — could not attach a collection or
            // an "introduced in" line to a single weapon either.
            // LOWERCASED. m_byAppearance is looked up as appearanceName.toLower() (see the header),
            // and cosmeticAppearanceNames already returns lowercase, so passing the raw file stem
            // here would insert a mixed-case key — twoHandPolearm_stor059 — that infoFor() can
            // never match. The entry would exist, look correct in the cache file, and be dead.
            if (apps.isEmpty()
                && QFile::exists(apprDir + QLatin1Char('/') + stem + QStringLiteral(".app.json")))
                apps = AppearanceMeta::withSelfName(apps, stem.toLower());
            if (apps.isEmpty()) continue;

            Info inf;
            inf.itemName = stem;
            {
                QFile f(path);
                if (f.open(QIODevice::ReadOnly)) {
                    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
                    // Tolerant reads — absent/renamed keys just leave defaults (line omitted).
                    const int disp = o.value(QStringLiteral("eDisplayedQualityLevel")).toInt(-1);
                    inf.rarity     = disp >= 0 ? disp : o.value(QStringLiteral("eMagicType")).toInt(-1);
                    inf.seasonItem = o.value(QStringLiteral("bSeasonItem")).toBool();
                }
            }
            {   // Localized text (enUS StringList "Item_<stem>"): Description = mechanical text,
                // Flavor = the lore line cosmetics actually carry, TransmogName = wardrobe display
                // name when it differs. Labels read by NAME, so extra/renamed ones are harmless.
                const QHash<QString, QString> s = readStrings(
                    stlDir + QStringLiteral("/Item_%1.stl.json").arg(stem));
                inf.desc   = s.value(QStringLiteral("Description"));
                inf.flavor = s.value(QStringLiteral("Flavor"));
                const QString tn = s.value(QStringLiteral("TransmogName"));
                if (!tn.isEmpty() && tn != s.value(QStringLiteral("Name"))) inf.transmogName = tn;
            }
            itemToAppear.insert(stem.toLower(), apps);
            for (const QString& a : apps)
                if (!map.contains(a)) map.insert(a, inf);
        }

        // Pass 2 — StoreProducts: the season/expansion that introduced each transmog item, plus the
        // product's own Description/Quote (what the bundle contains + its flavour line) which is the
        // closest thing the data has to a COLLECTION description.
        {
            QDirIterator pit(prdDir, {QStringLiteral("*.prd.json")}, QDir::Files);
            while (pit.hasNext()) {
                const QString ppath = pit.next();
                QFile f(ppath);
                if (!f.open(QIODevice::ReadOnly)) continue;
                const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
                const QString item = refBaseName(o.value(QStringLiteral("snoItemTransmog"))).toLower();
                if (item.isEmpty()) continue;
                QString intro = refBaseName(o.value(QStringLiteral("snoAssociatedSeason")));
                if (intro.isEmpty())
                    intro = refBaseName(o.value(QStringLiteral("snoAssociatedExpansion")));
                const QString pstem = QFileInfo(ppath).fileName().section(QStringLiteral(".prd.json"), 0, 0);
                const QHash<QString, QString> ps = readStrings(
                    stlDir + QStringLiteral("/StoreProduct_%1.stl.json").arg(pstem));
                const QString cd = ps.value(QStringLiteral("Description"));
                const QString cq = ps.value(QStringLiteral("Quote"));
                if (intro.isEmpty() && cd.isEmpty() && cq.isEmpty()) continue;
                for (const QString& a : itemToAppear.value(item)) {
                    auto mi = map.find(a);
                    if (mi == map.end()) continue;
                    if (mi->introducedIn.isEmpty()) mi->introducedIn = intro;
                    if (mi->collDesc.isEmpty())     mi->collDesc     = cd;
                    if (mi->collQuote.isEmpty())    mi->collQuote    = cq;
                }
            }
        }

        // Persist + install on the GUI thread.
        QMetaObject::invokeMethod(this, [this, map, sig, cachePath]() mutable {
            QJsonObject e;
            for (auto i = map.constBegin(); i != map.constEnd(); ++i) {
                const Info& n = i.value();
                QJsonObject o{{QStringLiteral("n"), n.itemName}};
                if (n.rarity >= 0)              o.insert(QStringLiteral("r"), n.rarity);
                if (n.seasonItem)               o.insert(QStringLiteral("s"), true);
                if (!n.desc.isEmpty())          o.insert(QStringLiteral("d"), n.desc);
                if (!n.flavor.isEmpty())        o.insert(QStringLiteral("f"), n.flavor);
                if (!n.transmogName.isEmpty())  o.insert(QStringLiteral("t"), n.transmogName);
                if (!n.introducedIn.isEmpty())  o.insert(QStringLiteral("i"), n.introducedIn);
                if (!n.collDesc.isEmpty())      o.insert(QStringLiteral("cd"), n.collDesc);
                if (!n.collQuote.isEmpty())     o.insert(QStringLiteral("cq"), n.collQuote);
                e.insert(i.key(), o);
            }
            QDir().mkpath(QFileInfo(cachePath).absolutePath());
            QFile f(cachePath);
            if (f.open(QIODevice::WriteOnly))
                f.write(QJsonDocument(QJsonObject{{QStringLiteral("sig"), sig},
                                                  {QStringLiteral("appearances"), e}})
                            .toJson(QJsonDocument::Compact));
            qInfo("item-hover index: %d appearance entries", int(map.size()));
            install(std::move(map));
        }, Qt::QueuedConnection);
    }).detach();
}
