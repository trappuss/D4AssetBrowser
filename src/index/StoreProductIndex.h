#pragma once

class SnoIndex;
class CascReader;   // CASC fallback for products d4data has no .prd.json for
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

// ── The Cosmetics Shop catalogue ────────────────────────────────────────────────────────────────
//
// SNO group 110 is StoreProductDefinition (definitions.json: snoGroup 110, 73 fields, 592 bytes) —
// every product the in-game shop has ever listed. d4data exports them as
// json/base/meta/StoreProduct/<name>.prd.json (7,496 files against 9,308 CoreTOC records; the
// 1,813-record gap is encrypted content, see BUNDLES-TAB-RESEARCH.md).
//
// THERE IS NO SEPARATE BUNDLE TYPE. A bundle is a product whose arBundledProducts is non-empty,
// and its children are ordinary products. Same struct, one level of nesting, so this index stores
// one record shape and answers "is this a bundle" with children.isEmpty().
//
// Each LEAF carries exactly one payload pointer out of thirteen. Only handling snoItemTransmog
// would cover ~95% of products by count and still silently drop every mount, marking, emote,
// emblem, headstone, portal and companion a bundle contains — which is the whole point of viewing
// a bundle rather than its armour.
//
// Display names are NOT in json/base: they live in group 42 StringLists under
// json/enUS_Text/meta/StringList/StoreProduct_<name>.stl.json as {szLabel, szText} rows ("Name",
// "Description"). Bundle_HArmor_rog_stor251 is "The Lost Zealot" — the SNO name is not what the
// shop, or the player, calls it.
class StoreProductIndex : public QObject {
    Q_OBJECT
public:
    // Which of the thirteen payload fields a leaf product resolved through. Ordered so the UI can
    // group children by kind without a second lookup.
    enum Kind {
        None = 0, Transmog, Mount, Emote, Marking, Jewelry, Emblem,
        Headstone, TownPortal, HairStyle, FacialHair, Companion, Power, DyeArmor
    };
    static QString kindLabel(Kind k);

    // ── Supported classes ───────────────────────────────────────────────────────────────────────
    // fPreviewOnClasses is an 8-element 0/1 array — the shop's "Supported Classes" line. The order
    // was MEASURED, not assumed: over 1,600 products, every single-class entry lined up as
    // 0 Sorcerer · 1 Druid · 2 Barbarian · 3 Rogue · 4 Necromancer, which is the same order
    // ENCRYPTED-CONTENT-HANDOFF.md recorded for tInvImages (5 Spiritborn, 6 Paladin). Index 7 had
    // no single-class sample in that run, so it is deliberately left unnamed rather than guessed —
    // a wrong class glyph is worse than none.
    static QString classLabel(int idx);
    // "Barbarian · Rogue", or "All classes" when every bit is set, or empty when none are.
    static QString classSummary(quint32 mask);

    struct Product {
        int      sno = 0;
        QString  name;          // SNO name, e.g. "Bundle_HArmor_rog_stor251"
        QString  title;         // StringList "Name", e.g. "The Lost Zealot"
        QString  description;   // StringList "Description" (shop lore text)
        int      eType = -1;    // raw; the enum's labels are not in the data, so it is not decoded
        QString  branch;        // szProductReleaseBranch, e.g. "2_3_0" — the patch it shipped in
        int      season = 0;    // snoAssociatedSeason
        bool     hasVfx = false;
        quint32  classMask = 0;   // bit i = fPreviewOnClasses[i]; see classLabel
        QString  seasonName;    // "Season 8" — the ref object carries it; we used to discard it
        QVector<int>     children;   // arBundledProducts (empty = leaf)
        Kind     kind = None;
        int      payloadSno = 0;     // what `kind` points at
        // The SNO group payloadSno belongs to (Transmog→Item, Headstone→Actor …). Learned from
        // d4data's ref objects, never hardcoded. Needed to turn a CASC-recovered payload sno back
        // into a name, since the binary carries no names at all.
        int      payloadGroup = 0;
        // Recovered from the game's binary because d4data has no .prd.json for it. Such a product
        // has children, a payload and a name, but no shop title, lore, art handles or season —
        // those live only in the JSON and its string tables.
        bool     fromCasc = false;
        QString  payloadName;
        QVector<quint32> art;        // UI image handles (IconIndex handle space)

        // ── Relationships between products, straight out of the .prd ────────────────────────────
        // Measured on a 1,500-product sample: requires 6%, requiresNot 1.6%, addOns 1%. Small, but
        // they answer a question nothing else can — mnt_stor158_trophy REQUIRES
        // Battlepass_Season3_Premium, i.e. it was never sold, it was a Season 3 pass reward.
        QVector<int>     requires_;      // arRequiresOwning     (trailing _ : `requires` is a C++20 keyword)
        QVector<int>     requiresNot;    // arRequiresNotOwning  (mutually exclusive products)
        QVector<int>     addOns;         // arAddOnBundles

        // arCardArtVariants — {hCardImage, hCardHoverImage} pairs in the same handle space as `art`.
        // Kept separate because the hover art is a distinct asset, not another tile of the same one.
        QVector<quint32> cardArt;

        // Resolved from the SNO reference graph, not from the name: Item -> GearItem gives the real
        // equipment slot ("Helm", "ChestArmor", "Polearm"). Empty when the graph has no answer.
        QString  slot;

        bool isBundle() const { return !children.isEmpty(); }
    };

    static StoreProductIndex& instance();

    bool ready() const { return m_ready; }
    bool building() const { return m_building; }
    int  count() const { return m_byId.size(); }

    // Parse (or load from cache) on a background thread. No-op if ready/in-progress.
    // `index` is optional and only used to resolve GearItem slot names out of the SNO reference
    // graph. Without it the products still build; they just carry no slot.
    // `reader` enables the CASC fallback for the ~1,800 products d4data does not describe. Without
    // it the index is JSON-only and those products are simply absent (the cache signature records
    // which of the two you got, so a build made without CASC is not mistaken for a complete one).
    void ensureBuilt(const QString& d4dataDir, const SnoIndex* index = nullptr,
                     CascReader* reader = nullptr);
    void reset();

    const Product* product(int sno) const;
    const Product* byName(const QString& name) const;
    // Bundle SNOs, sorted by display title. Bundles only — leaves are reachable via children.
    const QVector<int>& bundles() const { return m_bundles; }

    // ── "What sold this?" ───────────────────────────────────────────────────────────────────────
    // Any asset sno (Item, Actor, Appearance, …) -> the store products that reach it. Built by
    // walking each product outward through d4data's SNO reference graph and inverting the result.
    //
    // This is the one question the tool could never answer: pick a cosmetic in Models or Wardrobe
    // and find out which bundle sold it, and in which season. Empty for anything the graph does not
    // reach — including every asset added after the d4data snapshot's game build.
    QVector<int> soldIn(int assetSno) const { return m_soldIn.value(assetSno); }
    bool hasProvenance() const { return !m_soldIn.isEmpty(); }

signals:
    void readyChanged();
    void progress(int pct);

private:
    explicit StoreProductIndex(QObject* parent = nullptr) : QObject(parent) {}
    void install(QHash<int, Product> byId, QVector<int> bundles,
                 QHash<int, QVector<int>> soldIn);

    QHash<int, Product> m_byId;
    QHash<QString, int> m_byName;   // lowercased name -> sno
    QVector<int>        m_bundles;
    QHash<int, QVector<int>> m_soldIn;   // asset sno -> products that reach it
    bool m_ready = false;
    bool m_building = false;
    // Bumped by reset(). A worker captures the value it started with and its install() is dropped
    // if the generation moved on — so a d4data switch mid-build cannot publish the old data.
    int  m_generation = 0;
};
