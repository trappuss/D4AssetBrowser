#pragma once
#include <QObject>
#include <QString>
#include <QVector>

// Back trophies (the player's back cosmetic), resolved from the game data rather than guessed
// from appearance names.
//
// WHY THIS EXISTS. The Wardrobe used to list back trophies by scanning Appearance names for a
// "back_" prefix. In the shipped data exactly SIX appearances match that, and every one of them is
// a placeholder or per-class proxy (back_dru00, back_bar_proxy001, …) — so the picker offered six
// stand-in meshes and none of the real trophies. The real ones are named "trophy_<class><NN>_stor"
// and were never reachable.
//
// The authoritative chain, verified against the shipped JSON:
//
//     Item (snoItemType -> ItemType/CosmeticBack)
//       -> snoActor  -> Actor
//         -> snoAppearance -> Appearance          <- the model to load
//
// e.g. Item/trophy_sor029_stor (sno 1573163, "Vexation Star") -> Actor/trophy_sor029_stor
//      -> Appearance/trophy_sor029_stor. Spot-checked against a community SNO dump: Uchi's Orbit
//      1598379, Urn of Fiends 1593127, Wings of the Damned 1971329 and Vexation Star 1573163 all
//      resolve exactly.
//
// Update-proofing, deliberately:
//   · the ItemType REFERENCE is what selects an item, never its filename — a new naming
//     convention for trophies cannot break this the way the "back_" prefix did;
//   · sno references are followed by "__targetFileName__" basename, so no id assumptions;
//   · tolerant reads — an item that cannot be resolved is skipped, never guessed at;
//   · the disk cache is signed with the Item file count + buildVersion.txt, so a game patch or a
//     new d4data commit rebuilds it automatically and can never serve stale entries.
class BackTrophyIndex : public QObject {
    Q_OBJECT
public:
    struct Entry {
        QString appearance;    // Appearance stem — what the Wardrobe actually loads
        QString itemStem;      // source Item stem (shown as the file, and used for hover joins)
        QString displayName;   // localized item name; empty ⇒ caller falls back to the stem
    };

    static BackTrophyIndex& instance();
    void ensureBuilt(const QString& d4dataDir);   // background; no-op if ready/in-flight
    void reset();                                 // drop memory + disk cache
    bool ready() const { return m_ready; }
    // Sorted by display name. Empty until ready() — callers keep their previous list until then.
    const QVector<Entry>& entries() const { return m_entries; }

signals:
    void readyChanged();

private:
    explicit BackTrophyIndex(QObject* parent = nullptr) : QObject(parent) {}
    void install(QVector<Entry>&& e);
    int  generation() const { return m_generation; }

    QVector<Entry> m_entries;
    bool m_ready    = false;
    bool m_building = false;
    // Bumped by reset(). An in-flight build captures the value it started with and discards its
    // result if it no longer matches, so switching d4data mid-build cannot install entries from
    // the old snapshot or rewrite the cache file reset() just deleted.
    int  m_generation = 0;
};
