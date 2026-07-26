#pragma once
#include <QHash>
#include <QObject>
#include <QString>

// Item-level hover metadata (rarity, season-item flag, required level, description, the
// season/expansion a cosmetic was introduced in), keyed by APPEARANCE name so the Wardrobe /
// Models hover popups can join it against what they already have.
//
// Update-proofing, deliberately:
//   · name-based join via AppearanceMeta::cosmeticAppearanceNames — the ONE shared rule set,
//     so a naming-convention change is fixed in one place;
//   · tolerant field reads — a missing/renamed JSON key just omits that hover line;
//   · sno references resolved through "__targetFileName__" basenames (no id assumptions);
//   · unknown rarity enums render numerically instead of vanishing;
//   · the disk cache is signed with the Item/StringList file counts + buildVersion.txt, so a
//     game patch or a new d4data commit rebuilds it automatically (never serves stale data).
class ItemHoverIndex : public QObject {
    Q_OBJECT
public:
    struct Info {
        int     rarity   = -1;    // eDisplayedQualityLevel override, else eMagicType (-1 = unknown)
        bool    seasonItem = false;
        QString itemName;         // the source Item file stem
        QString desc;             // item "Description" (mechanical text), enUS StringList
        QString flavor;           // item "Flavor" (lore/quote text) — the good one for cosmetics
        QString transmogName;     // "TransmogName" when it differs from the plain Name
        QString introducedIn;     // "Season 1" / expansion stem from the StoreProduct link
        QString collDesc;         // StoreProduct "Description" — what the bundle/collection contains
        QString collQuote;        // StoreProduct "Quote" — collection flavour line
    };

    static ItemHoverIndex& instance();
    void ensureBuilt(const QString& d4dataDir);   // background; no-op if ready/in-flight
    void reset();                                  // drop memory + disk cache (build change)
    bool ready() const { return m_ready; }
    // Lookup by appearance name (any case). Returns a default Info when unknown.
    Info infoFor(const QString& appearanceName) const
    {
        return m_byAppearance.value(appearanceName.toLower());
    }
    // "Normal/Magic/Rare/…" for known enums; "quality N" for anything a future patch adds.
    static QString rarityLabel(int r);
    // D4's own item-quality colours (hex). Unknown enums fall back to neutral grey.
    static const char* rarityColor(int r);

signals:
    void readyChanged();

private:
    explicit ItemHoverIndex(QObject* parent = nullptr) : QObject(parent) {}
    void install(QHash<QString, Info> map);

    QHash<QString, Info> m_byAppearance;   // lower appearance name → info
    bool m_ready = false;
    bool m_building = false;
};
