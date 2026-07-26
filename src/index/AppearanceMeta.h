#pragma once
#include <QHash>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class SnoIndex;
class CascReader;

// Per-appearance metadata (class, gender, item type, weapon/armor, transmog) +
// the item's translated title, derived by crawling d4data Item → Actor →
// Appearance + ItemType. Port of the Python AppearanceMeta (Phase A + Phase B).
// Built once on a background thread and cached to disk; feeds the Models tab's
// authoritative Type/Title filters.
class AppearanceMeta : public QObject {
    Q_OBJECT
public:
    static AppearanceMeta& instance();

    bool ready() const { return m_ready; }
    bool building() const { return m_building; }

    // Build (or load from cache) on a background thread. No-op if already ready
    // or in progress. Emits readyChanged() when the data becomes available.
    // `reader` (optional) enables the CASC-item icon phase: it recovers inventory-icon
    // handles for items missing from the d4data snapshot (seasonal uniques/sets/etc.) by
    // reading their ItemDefinition binaries straight from CASC.
    void ensureBuilt(const QString& d4dataDir, const SnoIndex* index, CascReader* reader = nullptr);
    // Drop the in-memory metadata + delete the on-disk cache (stale-cache invalidation on
    // a game-build / d4data change). Next ensureBuilt rebuilds from scratch.
    void reset();

    QSet<QString> tagsFor(int sno) const { return m_tags.value(sno); }
    QString       titleFor(int sno) const { return m_titles.value(sno); }
    QString       collectionFor(int sno) const { return m_collections.value(sno); }
    quint32       iconFor(int sno) const { return m_icons.value(sno); }   // inv-icon handle (0=none)
    QString       nameForIconHandle(quint32 h) const { return m_iconNames.value(h); }   // texframe label

    // {"Class":[...], "Gender":["Female","Male"], "Type":[... >=20 uses ...]}.
    QMap<QString, QStringList> tagGroups() const;

    // ── Shared item→appearance name-derivation rules ─────────────────────────
    // Used by the crawl AND by IconAudit so the audit checks exactly the logic
    // the solver runs. All names are returned lowercased ("barf_sets53_hlm").
    //
    // Class-prefix codes in eHeroClass order (sor,dru,bar,rog,nec,spi,pal,war) —
    // index into this list == index into tInvImages / fUsableByClass.
    static const QStringList& heroClassPrefixes();
    // "bar" → "Barbarian" (falls back to the upper-cased code for a class this build doesn't
    // know — a NEW class still lists instead of vanishing). THE one place class names live:
    // when Blizzard adds a class, extend classPrefix() in AppearanceMeta.cpp and every combo,
    // filter and regex across the app follows.
    static QString classDisplayName(const QString& prefix);
    // "(bar|rog|…)" alternation for regexes, built from heroClassPrefixes() (never hardcode).
    static QString classPrefixPattern();
    // Route 1 — cosmetic item name: "Helm_Cosmetic_Barb_150_stor" →
    // {barf_stor150_hlm, barm_stor150_hlm}. Empty when the name isn't cosmetic.
    static QStringList cosmeticAppearanceNames(const QString& itemName);
    // Route 2 — actor SLOT_style: actor "HLM_sets53" + class prefixes →
    // {<cls><g>_sets53_hlm ...}. Empty when the actor isn't a SLOT_style name.
    // classPrefixes empty = expand all 8 classes.
    static QStringList styleAppearanceNames(const QString& actorName,
                                            const QStringList& classPrefixes = {});

signals:
    void readyChanged();
    void progress(int pct);   // 0..100 while crawling (cache miss only)

private:
    explicit AppearanceMeta(QObject* parent = nullptr) : QObject(parent) {}

    void install(QHash<int, QSet<QString>> tags, QHash<int, QString> titles,
                 QHash<int, QString> collections, QHash<int, quint32> icons,
                 QHash<quint32, QString> iconNames);

    QHash<int, QSet<QString>> m_tags;
    QHash<int, QString>       m_titles;
    QHash<int, QString>       m_collections;
    QHash<int, quint32>       m_icons;
    QHash<quint32, QString>   m_iconNames;
    bool m_ready = false;
    bool m_building = false;
};
