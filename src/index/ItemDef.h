#pragma once
#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>

// Minimal parser for the game's ItemDefinition / ActorDefinition snapshot binaries
// (base/meta/<sno>), used to recover inventory-icon handles for items that are NOT
// present in the (community, patch-lagging) d4data snapshot — e.g. seasonal uniques,
// sets and generics. This is the CASC-native counterpart to the d4data JSON crawl.
//
// Layout verified against real base/meta/Item/*.itm and base/meta/Actor/*.acr:
//   ItemDefinition:
//     [0..15]  header (0xDEADBEEF magic + 12 bytes)
//     [16]     u32 self-sno            (the definition's own sno)
//     [24]     u32 snoActor            (the actor that owns this item's look)
//     [368]    tInvImages: 8 × { u32 hDefaultImage(male), u32 hFemaleImage(female) }
//              INLINE array indexed by eHeroClass (Sorc=0, Druid=1, Barb=2, Rogue=3,
//              Necro=4, Spiritborn=5, Paladin=6, Warlock=7 — the last two are datamined
//              future classes; d4data's tInvImages JSON confirms 8 entries). Class-specific
//              items populate only their own slot(s).
//   ActorDefinition:
//     [16]     u32 self-sno
//     [36]     u32 snoAppearance       (the PROXY/base-mesh appearance — not the transmog)
//
// The offsets are fixed fields in the flat struct; they hold across the armour item
// types we target. Callers must sanity-check the returned snos against the index.
namespace ItemDef {

// eHeroClass order used by tInvImages / fUsableByClass (matches the game enum).
enum HeroClass { Sorcerer = 0, Druid = 1, Barbarian = 2, Rogue = 3,
                 Necromancer = 4, Spiritborn = 5, Paladin = 6, Warlock = 7,
                 HeroClassCount = 8 };

struct ItemInfo {
    bool     valid   = false;
    quint32  snoActor = 0;
    // Per-class {male/default, female}. Up to HeroClassCount entries; may be shorter
    // if the blob ends early (older builds) — callers must bounds-check the index.
    QVector<QPair<quint32, quint32>> images;
};

// Parse an ItemDefinition meta blob. Returns valid=false on a too-short/!magic buffer
// or when no class entry carries an image.
ItemInfo parseItem(const QByteArray& meta);

// Parse an ActorDefinition meta blob → snoAppearance (0 if unavailable).

// Map a hero-class 3-letter prefix (from an appearance name, e.g. "bar", "sor")
// to its eHeroClass index, or -1 if unknown.
int heroClassIndex(const QString& prefix);

}  // namespace ItemDef
