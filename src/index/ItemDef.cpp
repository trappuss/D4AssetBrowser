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

int heroClassIndex(const QString& prefix)
{
    const QString p = prefix.toLower();
    if (p == QLatin1String("sor")) return Sorcerer;
    if (p == QLatin1String("dru")) return Druid;
    if (p == QLatin1String("bar")) return Barbarian;
    if (p == QLatin1String("rog")) return Rogue;
    if (p == QLatin1String("nec")) return Necromancer;
    if (p == QLatin1String("spb") || p == QLatin1String("spi")) return Spiritborn;
    if (p == QLatin1String("pal")) return Paladin;
    if (p == QLatin1String("war")) return Warlock;
    return -1;
}

}  // namespace ItemDef
