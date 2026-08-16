#include "index/ItemDef.h"

namespace ItemDef {
namespace {

constexpr int   kInvImagesOffset = 368;   // tInvImages[] start in the ItemDefinition
constexpr int   kSelfOffset      = 16;    // the def's own sno (self-reference)
constexpr int   kActorOffset     = 24;    // snoActor in the ItemDefinition
constexpr int   kAppearanceOffset = 36;   // snoAppearance in the ActorDefinition (@16 is self-sno)
constexpr quint32 kMagic         = 0xDEADBEEFu;

inline quint32 rdU32(const QByteArray& b, int o)
{
    if (o < 0 || o + 4 > b.size()) return 0;
    const uchar* d = reinterpret_cast<const uchar*>(b.constData());
    return quint32(d[o]) | (quint32(d[o + 1]) << 8)
         | (quint32(d[o + 2]) << 16) | (quint32(d[o + 3]) << 24);
}

inline bool hasMagic(const QByteArray& b)
{
    return b.size() >= 4 && rdU32(b, 0) == kMagic;
}

}  // namespace

ItemInfo parseItem(const QByteArray& meta)
{
    ItemInfo info;
    // Need at least the header + one tInvImages entry. Read as many of the 8 inline
    // class entries as the blob actually contains (tolerates truncated/older blobs).
    if (!hasMagic(meta) || meta.size() < kInvImagesOffset + 8)
        return info;

    info.snoActor = rdU32(meta, kActorOffset);
    const int avail = int((meta.size() - kInvImagesOffset) / 8);
    const int n = qMin(avail, int(HeroClassCount));
    info.images.resize(n);
    bool anyImage = false;
    for (int c = 0; c < n; ++c) {
        const int o = kInvImagesOffset + c * 8;
        const quint32 male   = rdU32(meta, o);
        const quint32 female = rdU32(meta, o + 4);
        info.images[c] = qMakePair(male, female);
        if (male || female) anyImage = true;
    }
    // Only meaningful if the item actually carries an inventory image.
    info.valid = anyImage;
    return info;
}

// (actorAppearance removed — no callers; ItemDef::parse extracts snoActor itself.)

// The one hero-class table. Row order IS eHeroClass order — do not sort this.
static const HeroClassDef kHeroClasses[HeroClassCount] = {
    /* 0 Sorcerer    */ {"sor", "Sorcerer"},
    /* 1 Druid       */ {"dru", "Druid"},
    /* 2 Barbarian   */ {"bar", "Barbarian"},
    /* 3 Rogue       */ {"rog", "Rogue"},
    /* 4 Necromancer */ {"nec", "Necromancer"},
    /* 5 Spiritborn  */ {"spi", "Spiritborn"},
    /* 6 Paladin     */ {"pal", "Paladin"},
    /* 7 Warlock     */ {"war", "Warlock"},
};
static_assert(sizeof(kHeroClasses) / sizeof(kHeroClasses[0]) == HeroClassCount,
              "kHeroClasses must have exactly one row per HeroClass enumerator");

const HeroClassDef* heroClasses() { return kHeroClasses; }

const char* heroClassCode(int idx)
{
    return (idx >= 0 && idx < HeroClassCount) ? kHeroClasses[idx].code : "";
}

const char* heroClassName(int idx)
{
    return (idx >= 0 && idx < HeroClassCount) ? kHeroClasses[idx].name : "";
}

int heroClassIndex(const QString& prefix)
{
    const QString p = prefix.toLower();
    // "spb" is an alias the data uses for Spiritborn alongside "spi"; it is the one code that
    // is not simply the table entry, so it stays an explicit special case rather than a row.
    if (p == QLatin1String("spb")) return Spiritborn;
    for (int i = 0; i < HeroClassCount; ++i)
        if (p == QLatin1String(kHeroClasses[i].code)) return i;
    return -1;
}

}  // namespace ItemDef
