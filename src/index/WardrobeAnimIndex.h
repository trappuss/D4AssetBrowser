#pragma once
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

// The animations the GAME'S OWN wardrobe plays, resolved from the shipped data rather than from
// name patterns.
//
// THE CHAIN, verified against the JSON:
//
//   ItemType/<type>.itt.json  ->  eWeaponClass          (Sword2H = 3, Axe2H = 13, Polearm = 4, …)
//   AnimSet/<cls>_<cfg>_ui_wardrobe.ans.json
//        ptWeaponClasses  = [the classes this set covers]
//        ptPowerEntryList = [{ snoPower, snoAnim, snoFemaleOverrideAnim }, …]
//
// So the game keys on a WEAPON CLASS, looks up the wardrobe AnimSet that declares it, and takes the
// anim bound to a Power — with a separate female override. Nothing is inferred from the file name.
//
// TWO THINGS THE DATA SETTLES that guessing gets wrong:
//
//  · There is NO wardrobe "sheathe". The only Powers any of the 48 *_ui_wardrobe sets bind are
//    ui_wardrobe_unSheathe, ui_wardrobe_idle and ui_loadingScreen_pose. The wardrobe DRAWS the new
//    weapon and settles into an idle; it never puts one away. The *_event_sheathe clips belong to
//    the gameplay AnimSets, not to this.
//  · A two-handed AXE is weapon class 13, the same as Mace2H — so it plays the 2HM wardrobe anim,
//    not the 2HS one its name suggests. Only Sword2H is class 3.
//
// Weapon classes are GLOBAL, not per-character-class: class 3 means Sword2H in the barbarian,
// necromancer, paladin and warrior sets alike. Dual-wield (9) and the main+off-hand combinations
// (25/26/30) are STATES rather than item types — no ItemType carries them — so they are derived
// from what is in the two hands.
class WardrobeAnimIndex : public QObject {
    Q_OBJECT
public:
    // What the wardrobe plays for one weapon configuration. Either may be empty when the set does
    // not bind that Power (bar_1hshth, for instance, binds only the loading-screen pose).
    struct Clips {
        QString unsheathe;   // ui_wardrobe_unSheathe — played once on a weapon change
        QString idle;        // ui_wardrobe_idle      — looped afterwards
    };

    static WardrobeAnimIndex& instance();
    void ensureBuilt(const QString& d4dataDir);   // background; no-op if ready/in-flight
    void reset();
    bool ready() const { return m_ready; }
    // Exposed so File ▸ Index can say "Building…" instead of only ready/not-ready.
    bool building() const { return m_building; }

    // eWeaponClass for an ItemType stem ("Sword2H", "Axe", …). -1 when the type carries none.
    int weaponClassOf(const QString& itemType) const
    { return m_classByType.value(itemType.toLower(), -1); }

    // classPrefix is the animation prefix without gender ("bar", "rog", …). female picks
    // snoFemaleOverrideAnim over snoAnim. Empty Clips when nothing covers that combination.
    Clips clipsFor(const QString& classPrefix, int weaponClass, bool female) const;

    // Every weapon class this class prefix has a wardrobe set for — lets the caller tell "we have
    // no data for this character" apart from "we have data and this configuration isn't in it".
    bool covers(const QString& classPrefix) const
    { return m_prefixes.contains(classPrefix.toLower()); }

signals:
    void readyChanged();
    void progress(int pct);   // 0..100 while crawling (this index has no disk cache \u2014 always)

private:
    explicit WardrobeAnimIndex(QObject* parent = nullptr) : QObject(parent) {}
    struct Entry { Clips male, female; };
    void install(QHash<QString, int>&& byType, QHash<QString, Entry>&& sets, QSet<QString>&& prefixes);
    static QString key(const QString& prefix, int wc)
    { return prefix.toLower() + QLatin1Char('#') + QString::number(wc); }

    QHash<QString, int>   m_classByType;   // lower-cased ItemType stem -> eWeaponClass
    QHash<QString, Entry> m_sets;          // "bar#9" -> the two clips, per gender
    QSet<QString>         m_prefixes;      // class prefixes that have any wardrobe set at all
    bool m_ready = false;
    bool m_building = false;
    int  m_generation = 0;
};
