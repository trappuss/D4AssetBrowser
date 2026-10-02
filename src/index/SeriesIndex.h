#pragma once
#include <QHash>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

// ── Collections: the named sets, assembled from the shop graph ──────────────────────────────────
//
// WHERE THE NAME LIVES — MEASURED, NOT ASSUMED. Of the 61,330 string tables in the snapshot,
// exactly 7,017 carry a "Series" row, and every single one of them is a StoreProduct_*.stl.json.
// No Item, Emblem, MarkingShape, Emote or Actor table carries one. The first version of this index
// crawled every table looking for the label on the things themselves and found nothing, because
// there is nothing there: the set name is a property of the PRODUCT, and the product's contents
// inherit it. (diablo4.dad's export appears to carry `series` on items because their exporter
// pushes it down the same way. It is not a second source, and it is not read here.)
//
// SO A COLLECTION IS BUILT, NOT FOUND. Every product is read for its own Series; a thing you can
// own takes the nearest Series going up the arRequiresOwning/arBundledProducts parent chain. Then
// every product sharing a name merges into one set. That merge is the whole point: "Beauty in Sin"
// is six per-class bundles plus a separately-sold mount and emote — eight unrelated rows in the
// Catalogue, one set here, with every piece under it.
//
// WHAT THIS IS NOT. It is not a superset of the Catalogue. Anything the shop never listed carries
// no Series row anywhere in the data, so it cannot appear here — there is nothing to read. This is
// a regrouping of shop data by the game's own set names, which is a different and useful view, and
// claiming more than that would be claiming something the snapshot does not contain.
//
// SOURCE is derived from the product, never curated:
//   Season         a contributing product names a season, or is gated behind arRequiresOwning —
//                  which StoreProductIndex.cpp documents as the only marker for "never sold
//                  separately, it came with the pass".
//   Shop           contributing products carry neither.
//   Uncategorised  every contributing product is TACT-locked, so its facts are unreadable. An
//                  honest "cannot tell", not a bucket for things we did not classify.
//
// There is deliberately no Promotional category. Twitch drops, pre-orders and crossovers still
// have StoreProduct records, and nothing in those records distinguishes them from anything else.
// eType is the only candidate lever and its labels are not in the data, so its distribution is
// carried raw per collection (see `etypes`) to be measured before it is ever trusted.
//
// UPDATE-PROOFING: the Series STRING selects a collection, never a filename pattern; the per-class
// suffix splits using AppearanceMeta's own class list, so a new playable class needs no edit here;
// contents come from ProductContents, the same descent the Catalogue draws with, so the two cannot
// drift; and the disk cache is signed with the product count + buildVersion.txt.
class SeriesIndex : public QObject {
    Q_OBJECT
public:
    enum Category { Season = 0, Shop, Uncategorised };
    static QString categoryLabel(Category c);
    // One line saying what was PROVEN, for the details pane — so a derived label never reads like
    // an editorial one.
    static QString categoryWhy(Category c);

    // One thing you can own that belongs to this set. Always backed by a product, because that is
    // the only place the set name exists.
    struct Member {
        int     sno = 0;      // the PRODUCT's sno — what the Catalogue jumps to
        QString kind;         // StoreProductIndex::kindLabel: "Mounts", "Emotes", "Armour & weapons"
        QString stem;         // payloadName — the asset it resolves to
        QString name;         // shop title ("Skullcleave"); empty ⇒ fall back to stem
        QString cls;          // class from a "<Series>" <Class> Equipment product; else empty
        QString product;      // the product's SNO name
        bool    locked = false;   // TACT-encrypted: it exists, its contents do not decrypt
    };

    struct Entry {
        QString  name;              // normalised set name — quotes stripped, class suffix split off
        QString  raw;               // one authored string verbatim, so normalisation stays checkable
        QVector<Member> members;
        QStringList classes;        // classes seen on "<X> Equipment" products, sorted
        QMap<QString, int> kinds;   // kind label → member count (the UI's Kind filter, from data)
        QStringList seasons;        // distinct season names, sorted
        QStringList branches;       // distinct szProductReleaseBranch values, sorted
        int products     = 0;       // products that contributed to this set
        int seasonLinked = 0;       // …naming a season
        int passLocked   = 0;       // …gated behind arRequiresOwning
        int lockedCount  = 0;       // …TACT-encrypted
        QMap<int, int> etypes;      // eType → count. Raw. Labels are not in the data.
        Category category = Uncategorised;

        int memberCount() const { return int(members.size()); }
    };

    static SeriesIndex& instance();
    // Needs StoreProductIndex to be ready; if it is not, this arms itself and returns, and builds
    // as soon as that index lands. Safe to call repeatedly.
    void ensureBuilt(const QString& d4dataDir);
    void reset();
    bool ready() const    { return m_ready; }
    bool building() const { return m_building; }
    // True when the only thing standing between us and a build is the product index.
    bool waitingOnProducts() const { return m_waiting; }

    const QVector<Entry>& entries() const { return m_entries; }
    // Set name for a product sno, or empty. Lets any tab's hover card name a collection.
    QString seriesForProduct(int productSno) const { return m_byProduct.value(productSno); }
    // Every distinct kind present, sorted. The UI builds its filter from this, never from a list.
    QStringList kinds() const;

signals:
    void readyChanged();
    void progress(int pct);

private:
    explicit SeriesIndex(QObject* parent = nullptr) : QObject(parent) {}
    void install(QVector<Entry>&& e, QHash<int, QString>&& byProduct);
    int  generation() const { return m_generation; }

    QVector<Entry>      m_entries;
    QHash<int, QString> m_byProduct;   // product sno → set name
    bool m_ready    = false;
    bool m_building = false;
    bool m_waiting  = false;   // armed, waiting for StoreProductIndex::readyChanged
    bool m_armed    = false;   // that connection is made once, not once per call
    // Bumped by reset(): an in-flight build discards its result if this changed, so a d4data swap
    // cannot install entries from the old snapshot or rewrite the cache reset() just deleted.
    int  m_generation = 0;
};
