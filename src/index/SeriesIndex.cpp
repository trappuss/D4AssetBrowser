#include "index/SeriesIndex.h"

#include "app/AppPaths.h"
#include "index/AppearanceMeta.h"
#include "index/ProductContents.h"
#include "index/StoreProductIndex.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <thread>

namespace {
// v3: the whole derivation changed. v1/v2 looked for a Series row on each ownable thing's own
// string table; measurement said only StoreProduct tables carry one (7,017 of 61,330, and nothing
// else at all), so the set name now comes from the product and its contents inherit it.
// v5: a set's sub-bundles ("Ash Knight Accessories", "… Mount Armor", "… Weapons" — 167 of them)
// carry their own label while the pieces inside carry the set's, so the Catalogue offered both as
// separate filters and picking the set hid its own accessories bundle. A container now follows its
// pieces where they agree. Version bumped because the fix is invisible to the signature.
// v4: v3 recorded a set name only for the PIECES. The Catalogue filters bundle ROWS and a bundle
// is a container, so every row resolved to nothing and the Collection combo came up empty. The fix
// was in the code, but the signature is a product count plus a build stamp — neither of which a
// code change touches — so a v3 cache written before it would have been served forever. Bumping
// the version is the only thing that invalidates it; pruneOldCaches removes the v3 file.
constexpr int kCacheVersion = 5;

// Display class names, built from AppearanceMeta's prefix list so a class added there is
// recognised here with no edit. Never a literal list.
const QSet<QString>& classDisplayNames()
{
    static const QSet<QString> s = [] {
        QSet<QString> out;
        for (const QString& p : AppearanceMeta::heroClassPrefixes())
            out.insert(AppearanceMeta::classDisplayName(p));
        return out;
    }();
    return s;
}

// The authored string is quote-wrapped ("Ash Knight"), though some rows ship without them — so
// this is a strip, not an unwrap, matching what AppearanceMeta has always done.
QString stripQuotes(QString s) { return s.remove(QLatin1Char('"')).trimmed(); }

// «"Beauty in Sin" Barbarian Equipment» -> {"Beauty in Sin", "Barbarian"}. The suffix is split off
// only when the middle word is a class this build knows; anything else stays part of the name
// rather than being silently truncated.
void splitClassSuffix(const QString& in, QString* base, QString* cls)
{
    *base = in;
    cls->clear();
    if (!in.endsWith(QLatin1String(" Equipment"))) return;
    const QString head = in.left(in.size() - 10).trimmed();   // strlen(" Equipment")
    const int sp = head.lastIndexOf(QLatin1Char(' '));
    if (sp <= 0) return;
    const QString word = head.mid(sp + 1);
    if (!classDisplayNames().contains(word)) return;
    *base = head.left(sp).trimmed();
    *cls  = word;
}

void sortUnique(QStringList* l)
{
    std::sort(l->begin(), l->end());
    l->erase(std::unique(l->begin(), l->end()), l->end());
}

// A product, copied off the index on the GUI thread. StoreProductIndex::reset() can free its
// Products while a build is in flight, and the worker has no way to notice; carrying our own copy
// removes the hazard entirely rather than racing it.
struct Snap {
    int     sno = 0;
    QString name, title, payloadName, kind, seasonName, branch;
    int     payloadSno = 0;
    int     etype = -1;
    bool    encrypted = false;
    bool    passLocked = false;   // arRequiresOwning is non-empty
    bool    hasSeason = false;
    QVector<int> children;
};

void logResult(const QVector<SeriesIndex::Entry>& e, const char* how, int seriesRows, int products)
{
    int members = 0, season = 0, shop = 0, unc = 0;
    for (const SeriesIndex::Entry& s : e) {
        members += s.memberCount();
        switch (s.category) {
            case SeriesIndex::Season:        ++season; break;
            case SeriesIndex::Shop:          ++shop;   break;
            case SeriesIndex::Uncategorised: ++unc;    break;
        }
    }
    qInfo("series index (%s): %d collections, %d members — %d season, %d shop, %d uncategorised",
          how, int(e.size()), members, season, shop, unc);
    // The two numbers that explain an empty result without a re-crawl or an external probe:
    // how many products were read at all, and how many of them named a set.
    qInfo("series index: %d products read, %d carried a Series row", products, seriesRows);
}
}   // namespace

QString SeriesIndex::categoryLabel(Category c)
{
    switch (c) {
        case Season:        return QStringLiteral("Seasonal");
        case Shop:          return QStringLiteral("Shop");
        case Uncategorised: break;
    }
    return QStringLiteral("Uncategorised");
}

QString SeriesIndex::categoryWhy(Category c)
{
    switch (c) {
        case Season:
            return QStringLiteral("A product selling part of this set names a season, or is gated "
                                  "behind arRequiresOwning — it came with a pass and was never "
                                  "sold on its own.");
        case Shop:
            return QStringLiteral("The products selling this set name no season and sit behind no "
                                  "ownership gate, so it was sold outright.");
        case Uncategorised: break;
    }
    return QStringLiteral("Every product for this set is TACT-encrypted, so how it was obtained "
                          "cannot be read. This says the data is unreadable — not that the set was "
                          "left unclassified.");
}

SeriesIndex& SeriesIndex::instance()
{
    static SeriesIndex inst;
    return inst;
}

QStringList SeriesIndex::kinds() const
{
    QSet<QString> s;
    for (const Entry& e : m_entries)
        for (auto it = e.kinds.constBegin(); it != e.kinds.constEnd(); ++it)
            s.insert(it.key());
    QStringList out;
    out.reserve(s.size());
    for (const QString& k : s) out << k;
    out.sort();
    return out;
}

void SeriesIndex::install(QVector<Entry>&& e, QHash<int, QString>&& byProduct)
{
    m_entries   = std::move(e);
    m_byProduct = std::move(byProduct);
    m_ready     = true;
    m_building  = false;
    m_waiting   = false;
    emit readyChanged();
}

void SeriesIndex::reset()
{
    ++m_generation;      // orphan any in-flight build before clearing
    m_ready = false;
    m_building = false;
    m_waiting = false;
    m_entries.clear();
    m_byProduct.clear();
    QFile::remove(AppPaths::dataDir()
                  + QStringLiteral("/series_index_v%1.json").arg(kCacheVersion));
    emit readyChanged();
}

void SeriesIndex::ensureBuilt(const QString& d4dataDir)
{
    if (m_ready || m_building || d4dataDir.isEmpty()) return;
    const QString stlDir = d4dataDir + QStringLiteral("/json/enUS_Text/meta/StringList");
    if (!QDir(stlDir).exists()) return;

    // The set name is a product property, so there is nothing to do until the products exist.
    // Arm once and come back on its signal rather than polling or racing it.
    StoreProductIndex& prd = StoreProductIndex::instance();
    if (!prd.ready()) {
        m_waiting = true;
        if (!m_armed) {
            m_armed = true;
            connect(&prd, &StoreProductIndex::readyChanged, this, [this, d4dataDir] {
                if (StoreProductIndex::instance().ready()) ensureBuilt(d4dataDir);
            });
        }
        return;
    }
    m_waiting = false;
    m_building = true;

    // ── Snapshot the product graph on THIS thread ───────────────────────────────────────────────
    // Roots are bundles + loose + locked; everything else is somebody's child, so descending from
    // those three reaches every product exactly once.
    QVector<Snap> snaps;
    QHash<int, int> indexOf;          // product sno → index into snaps
    {
        QVector<int> stack;
        QSet<int> seen;
        for (const QVector<int>* src : {&prd.bundles(), &prd.loose(), &prd.locked()})
            for (int s : *src) stack.push_back(s);
        while (!stack.isEmpty()) {
            const int sno = stack.takeLast();
            if (seen.contains(sno)) continue;
            seen.insert(sno);
            const StoreProductIndex::Product* p = prd.product(sno);
            if (!p) continue;
            Snap s;
            s.sno         = p->sno;
            s.name        = p->name;
            s.title       = p->title;
            s.payloadName = p->payloadName;
            s.kind        = StoreProductIndex::kindLabel(p->kind);
            s.seasonName  = p->seasonName;
            s.branch      = p->branch;
            s.payloadSno  = p->payloadSno;
            s.etype       = p->eType;
            s.encrypted   = p->encrypted;
            s.passLocked  = !p->requires_.isEmpty();
            s.hasSeason   = p->season != 0 || !p->seasonName.isEmpty();
            s.children    = p->children;
            indexOf.insert(s.sno, int(snaps.size()));
            snaps.push_back(s);
            for (int k : p->children) stack.push_back(k);
        }
    }

    const QString cachePath = AppPaths::dataDir()
                              + QStringLiteral("/series_index_v%1.json").arg(kCacheVersion);
    AppPaths::pruneOldCaches(QStringLiteral("series_index_v"), kCacheVersion, QStringLiteral(".json"));

    const int gen = m_generation;
    std::thread([this, gen, stlDir, cachePath, d4dataDir, snaps, indexOf]() {
        // ── Signature ───────────────────────────────────────────────────────────────────────────
        // The product count this build actually saw, plus the snapshot's build stamp. A patch or a
        // new d4data commit changes one of them and the cache rebuilds; nothing else can go stale,
        // because nothing else is read.
        QString sig;
        {
            QString bv;
            QFile f(d4dataDir + QStringLiteral("/buildVersion.txt"));
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) bv = QString::fromUtf8(f.readAll()).trimmed();
            sig = QStringLiteral("%1|%2").arg(snaps.size()).arg(bv);
        }

        QVector<Entry> out;
        QHash<int, QString> byProduct;
        int seriesRows = 0;

        {   // ── Cache hit? ──────────────────────────────────────────────────────────────────────
            QFile f(cachePath);
            if (f.open(QIODevice::ReadOnly)) {
                const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
                if (root.value(QStringLiteral("sig")).toString() == sig) {
                    seriesRows = root.value(QStringLiteral("rows")).toInt();
                    for (const QJsonValue& v : root.value(QStringLiteral("series")).toArray()) {
                        const QJsonObject o = v.toObject();
                        Entry e;
                        e.name = o.value(QStringLiteral("n")).toString();
                        e.raw  = o.value(QStringLiteral("r")).toString();
                        e.products     = o.value(QStringLiteral("p")).toInt();
                        e.seasonLinked = o.value(QStringLiteral("se")).toInt();
                        e.passLocked   = o.value(QStringLiteral("pl")).toInt();
                        e.lockedCount  = o.value(QStringLiteral("lk")).toInt();
                        e.category = Category(o.value(QStringLiteral("c")).toInt());
                        for (const QJsonValue& cv : o.value(QStringLiteral("cl")).toArray()) e.classes  << cv.toString();
                        for (const QJsonValue& cv : o.value(QStringLiteral("ss")).toArray()) e.seasons  << cv.toString();
                        for (const QJsonValue& cv : o.value(QStringLiteral("br")).toArray()) e.branches << cv.toString();
                        const QJsonObject et = o.value(QStringLiteral("et")).toObject();
                        for (auto i = et.constBegin(); i != et.constEnd(); ++i)
                            e.etypes.insert(i.key().toInt(), i.value().toInt());
                        for (const QJsonValue& mv : o.value(QStringLiteral("m")).toArray()) {
                            const QJsonObject mo = mv.toObject();
                            Member m;
                            m.sno     = mo.value(QStringLiteral("s")).toInt();
                            m.kind    = mo.value(QStringLiteral("k")).toString();
                            m.stem    = mo.value(QStringLiteral("f")).toString();
                            m.name    = mo.value(QStringLiteral("d")).toString();
                            m.cls     = mo.value(QStringLiteral("c")).toString();
                            m.product = mo.value(QStringLiteral("g")).toString();
                            m.locked  = mo.value(QStringLiteral("l")).toBool();
                            if (!m.sno) continue;
                            e.kinds[m.kind] = e.kinds.value(m.kind) + 1;
                            byProduct.insert(m.sno, e.name);
                            e.members.push_back(m);
                        }
                        // Containers resolve to a name but are not members, so they are stored
                        // separately — without them a warm start could not filter a bundle row.
                        for (const QJsonValue& cv : o.value(QStringLiteral("ct")).toArray())
                            byProduct.insert(cv.toInt(), e.name);
                        if (!e.name.isEmpty()) out.push_back(e);
                    }
                    logResult(out, "cached", seriesRows, int(snaps.size()));
                    QMetaObject::invokeMethod(this, [this, gen, out, byProduct]() mutable {
                        if (gen != generation()) return;      // d4data switched mid-build
                        install(std::move(out), std::move(byProduct));
                    }, Qt::QueuedConnection);
                    return;
                }
            }
        }

        // ── Pass 1 — each product's own Series row ──────────────────────────────────────────────
        // One targeted read per product (≈7,800), not a sweep of all 61,330 string tables: the
        // label only ever lives on StoreProduct_<name>.stl.json, which is a name we already have.
        QVector<QString> ownSeries(snaps.size());
        for (int i = 0; i < int(snaps.size()); ++i) {
            if ((i % 256) == 0 && !snaps.isEmpty()) {
                const int pct = int(qint64(i) * 100 / snaps.size());
                QMetaObject::invokeMethod(this, [this, pct]() { emit progress(pct); },
                                          Qt::QueuedConnection);
            }
            QFile f(QStringLiteral("%1/StoreProduct_%2.stl.json").arg(stlDir, snaps[i].name));
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QByteArray raw = f.readAll();
            if (!raw.contains("\"Series\"")) continue;   // cheap reject before parsing
            for (const QJsonValue& v : QJsonDocument::fromJson(raw).object()
                                           .value(QStringLiteral("arStrings")).toArray()) {
                const QJsonObject o = v.toObject();
                if (o.value(QStringLiteral("szLabel")).toString() == QLatin1String("Series")) {
                    ownSeries[i] = o.value(QStringLiteral("szText")).toString();
                    break;
                }
            }
            if (!ownSeries[i].isEmpty()) ++seriesRows;
        }

        // ── Pass 2 — inherit down the bundle graph ──────────────────────────────────────────────
        // A per-class bundle carries «"X" Barbarian Equipment» and the pieces inside it carry
        // nothing, so a piece takes the NEAREST named ancestor. Walking up a parent map rather than
        // descending keeps ProductContents as the single owner of the descent rules.
        QHash<int, int> parentOf;
        for (const Snap& s : snaps)
            for (int k : s.children)
                if (!parentOf.contains(k)) parentOf.insert(k, s.sno);

        // Resolve the set name for EVERY product first — containers included. A bundle is a
        // container, and the Catalogue filters bundle ROWS: resolving only the pieces would leave
        // every bundle nameless, so "show me this collection" would hide the rows it should keep.
        QVector<QString> resolved(snaps.size());
        for (int i = 0; i < int(snaps.size()); ++i) {
            QString rawSeries = ownSeries[i];
            int hops = 0;
            for (int cur = snaps[i].sno; rawSeries.isEmpty() && hops < 8; ++hops) {
                const auto pit = parentOf.constFind(cur);
                if (pit == parentOf.constEnd()) break;
                cur = pit.value();
                const auto iit = indexOf.constFind(cur);
                if (iit == indexOf.constEnd()) break;
                rawSeries = ownSeries[iit.value()];
            }
            resolved[i] = rawSeries;
        }

        QHash<QString, Entry> acc;
        QHash<int, QString> memberOf;   // ownable product sno → the set it landed in
        for (int i = 0; i < int(snaps.size()); ++i) {
            const Snap& s = snaps[i];
            const QString rawSeries = resolved[i];
            if (rawSeries.isEmpty()) continue;   // in no named set — nothing to invent

            QString base, cls;
            splitClassSuffix(stripQuotes(rawSeries), &base, &cls);
            if (base.isEmpty()) continue;

            // Every product that resolves is answerable by name, so the Catalogue can filter and
            // label a container row. Only things you can OWN become MEMBERS, though: counting a
            // container would list "The Lost Zealot" as one of its own pieces.
            byProduct.insert(s.sno, base);
            if (s.payloadSno <= 0 && !s.encrypted) continue;

            Entry& e = acc[base];
            if (e.name.isEmpty()) e.name = base;
            // Smallest, not first: hash and directory order are not stable, and "whichever we saw
            // first" made two builds of the same snapshot cache different bytes.
            if (e.raw.isEmpty() || rawSeries < e.raw) e.raw = rawSeries;

            Member m;
            m.sno     = s.sno;
            m.kind    = s.kind;
            m.stem    = s.payloadName.isEmpty() ? s.name : s.payloadName;
            m.name    = s.title;
            m.cls     = cls;
            m.product = s.name;
            m.locked  = s.encrypted;

            ++e.products;
            if (!cls.isEmpty())        e.classes << cls;
            if (s.encrypted)           ++e.lockedCount;
            if (s.hasSeason)         { ++e.seasonLinked; if (!s.seasonName.isEmpty()) e.seasons << s.seasonName; }
            if (s.passLocked)          ++e.passLocked;
            if (!s.branch.isEmpty())   e.branches << s.branch;
            e.etypes[s.etype] = e.etypes.value(s.etype) + 1;
            e.kinds[m.kind]   = e.kinds.value(m.kind) + 1;
            memberOf.insert(s.sno, base);
            e.members.push_back(m);
        }

        // ── A container follows its own pieces ──────────────────────────────────────────────────
        // The shop labels a set's sub-bundles with their own strings: the pieces inside
        // Bundle_..._stor268 carry "Ash Knight", but the bundle carries "Ash Knight Accessories",
        // and so do "… Mount Armor", "… Weapons", "… Pet". 167 such labels exist. Left alone, the
        // Catalogue would offer "Ash Knight" and "Ash Knight Accessories" as separate filters and
        // picking the set would hide the set's own accessories bundle — the exact opposite of what
        // the filter is for.
        //
        // So a container is placed where its OWN pieces went, not where its label points. Only when
        // every piece under it agrees: a wrapper like "Prestige Mega Bundle" holds several
        // different sets, and its pieces disagree, so it keeps its own label rather than being
        // filed under whichever set happens to be largest.
        for (int i = 0; i < int(snaps.size()); ++i) {
            const Snap& c = snaps[i];
            if (c.payloadSno > 0 || c.encrypted || c.children.isEmpty()) continue;
            QString only;
            bool agreed = true;
            QVector<int> stack = c.children;
            QSet<int> seen;
            while (!stack.isEmpty() && agreed) {
                const int k = stack.takeLast();
                if (seen.contains(k)) continue;
                seen.insert(k);
                const auto mit = memberOf.constFind(k);
                if (mit != memberOf.constEnd()) {
                    if (only.isEmpty())        only = mit.value();
                    else if (only != *mit)     agreed = false;
                }
                const auto iit = indexOf.constFind(k);
                if (iit != indexOf.constEnd())
                    for (int gk : snaps[iit.value()].children) stack.push_back(gk);
            }
            if (agreed && !only.isEmpty()) byProduct.insert(c.sno, only);
        }

        // ── Pass 3 — categorise, tidy, order ────────────────────────────────────────────────────
        out.reserve(acc.size());
        for (auto it = acc.begin(); it != acc.end(); ++it) {
            Entry e = std::move(it.value());
            if (e.members.isEmpty()) continue;
            sortUnique(&e.classes);
            sortUnique(&e.seasons);
            sortUnique(&e.branches);
            // Season wins over Shop: a battle-pass set is usually also sold, and "this came with
            // the pass" is the more specific and more useful of the two answers.
            e.category = (e.seasonLinked > 0 || e.passLocked > 0)   ? Season
                       : (e.lockedCount == e.memberCount())        ? Uncategorised
                                                                   : Shop;
            std::sort(e.members.begin(), e.members.end(), [](const Member& a, const Member& b) {
                const int c = a.kind.compare(b.kind, Qt::CaseInsensitive);
                if (c != 0) return c < 0;
                const QString an = a.name.isEmpty() ? a.stem : a.name;
                const QString bn = b.name.isEmpty() ? b.stem : b.name;
                const int d = an.compare(bn, Qt::CaseInsensitive);
                return d != 0 ? d < 0 : a.sno < b.sno;
            });
            out.push_back(std::move(e));
        }
        std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });

        QMetaObject::invokeMethod(this, [this, gen, out, byProduct, sig, cachePath, seriesRows,
                                         n = int(snaps.size())]() mutable {
            if (gen != generation()) return;                  // d4data switched mid-build
            QJsonArray arr;
            QSet<int> memberSnos;
            for (const Entry& e : out)
                for (const Member& m : e.members) memberSnos.insert(m.sno);
            for (const Entry& e : out) {
                QJsonArray ma;
                for (const Member& m : e.members) {
                    QJsonObject mo{{QStringLiteral("s"), m.sno}, {QStringLiteral("k"), m.kind},
                                   {QStringLiteral("f"), m.stem}};
                    if (!m.name.isEmpty())    mo.insert(QStringLiteral("d"), m.name);
                    if (!m.cls.isEmpty())     mo.insert(QStringLiteral("c"), m.cls);
                    if (!m.product.isEmpty()) mo.insert(QStringLiteral("g"), m.product);
                    if (m.locked)             mo.insert(QStringLiteral("l"), true);
                    ma.append(mo);
                }
                QJsonObject eo{{QStringLiteral("n"),  e.name}, {QStringLiteral("r"), e.raw},
                               {QStringLiteral("p"),  e.products},
                               {QStringLiteral("se"), e.seasonLinked},
                               {QStringLiteral("pl"), e.passLocked},
                               {QStringLiteral("lk"), e.lockedCount},
                               {QStringLiteral("c"),  int(e.category)},
                               {QStringLiteral("m"),  ma}};
                if (!e.classes.isEmpty())  eo.insert(QStringLiteral("cl"), QJsonArray::fromStringList(e.classes));
                if (!e.seasons.isEmpty())  eo.insert(QStringLiteral("ss"), QJsonArray::fromStringList(e.seasons));
                if (!e.branches.isEmpty()) eo.insert(QStringLiteral("br"), QJsonArray::fromStringList(e.branches));
                {   // container snos: resolved to this set, but not pieces of it
                    QJsonArray ct;
                    for (auto i = byProduct.constBegin(); i != byProduct.constEnd(); ++i)
                        if (i.value() == e.name && !memberSnos.contains(i.key()))
                            ct.append(i.key());
                    if (!ct.isEmpty()) eo.insert(QStringLiteral("ct"), ct);
                }
                if (!e.etypes.isEmpty()) {
                    QJsonObject et;
                    for (auto i = e.etypes.constBegin(); i != e.etypes.constEnd(); ++i)
                        et.insert(QString::number(i.key()), i.value());
                    eo.insert(QStringLiteral("et"), et);
                }
                arr.append(eo);
            }
            // An EMPTY result is never cached: the signature is a product count plus a build stamp,
            // so a cached nothing would outlive every code fix and keep answering "no collections"
            // until the game patched. Re-deriving on each launch is the cheaper mistake.
            if (!out.isEmpty()) {
                QDir().mkpath(QFileInfo(cachePath).absolutePath());
                QFile f(cachePath);
                if (f.open(QIODevice::WriteOnly))
                    f.write(QJsonDocument(QJsonObject{{QStringLiteral("sig"), sig},
                                                      {QStringLiteral("rows"), seriesRows},
                                                      {QStringLiteral("series"), arr}})
                                .toJson(QJsonDocument::Compact));
            }
            logResult(out, "built", seriesRows, n);
            install(std::move(out), std::move(byProduct));
        }, Qt::QueuedConnection);
    }).detach();
}
