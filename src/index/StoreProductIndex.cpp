#include "index/StoreProductIndex.h"

#include "casc/CascReader.h"   // CASC fallback — products with no .prd.json
#include "index/SnoIndex.h"
#include "index/ItemDef.h"      // the canonical hero-class table (classLabel derives from it)

#include "app/AppPaths.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <thread>

namespace {

// v2: records classMask (fPreviewOnClasses). A v1 file loads clean and would leave every product
// claiming no supported classes, with no symptom.
// v3: seasonName, requires/requiresNot/addOns, cardArt, graph-derived slot, and the soldIn
// reverse map. A v2 file loads clean and would leave every one of those empty with no symptom —
// the exact failure mode the AppearanceMeta signature had.
// v4: products recovered from the CASC binary for the ~1,800 the d4data snapshot never described
// (the Doom collab bundles among them). A v3 file loads clean and simply lacks them — which is the
// failure this file's header warns about, so the filename changes rather than the contents.
constexpr int kCacheVersion = 4;

// The thirteen payload fields, in the order Kind declares them. Table-driven so adding a payload
// type is one row here and one label below, not a new branch in the parser.
struct PayloadField { const char* key; StoreProductIndex::Kind kind; };
const PayloadField kPayloads[] = {
    {"snoItemTransmog",  StoreProductIndex::Transmog},
    {"snoMount",         StoreProductIndex::Mount},
    {"snoEmote",         StoreProductIndex::Emote},
    {"snoMarkingShape",  StoreProductIndex::Marking},
    {"snoJewelry",       StoreProductIndex::Jewelry},
    {"snoEmblem",        StoreProductIndex::Emblem},
    {"snoHeadstone",     StoreProductIndex::Headstone},
    {"snoTownPortal",    StoreProductIndex::TownPortal},
    {"snoHairStyle",     StoreProductIndex::HairStyle},
    {"snoFacialHair",    StoreProductIndex::FacialHair},
    {"snoCompanion",     StoreProductIndex::Companion},
    {"snoPower",         StoreProductIndex::Power},
    {"snoDyeArmor",      StoreProductIndex::DyeArmor},
};

// Every UI image handle a product can carry. All live in the same handle space IconIndex maps
// (handle -> atlas sno + frame + UV), so the tab decodes them with IconIndex::iconImage.
const char* const kArtFields[] = {
    "hSplashImage", "hConfirmImage", "hCategoryIcon", "hTileImage", "hSmallTileImage",
    "hOwnedTileImage", "hSmallOwnedTileImage", "hPurchaseCompleteImage", "hDetailsDisplayImage",
    "hIconRepresentation", "hAddOnDetailsScreenImage", "hStoreIconOverride",
};

// d4data wraps every SNO reference as { "__raw__": <id>, "name": "...", ... }.
inline int rawSno(const QJsonValue& v) { return v.toObject().value(QStringLiteral("__raw__")).toInt(); }
inline QString refName(const QJsonValue& v) { return v.toObject().value(QStringLiteral("name")).toString(); }

}  // namespace

QString StoreProductIndex::kindLabel(Kind k)
{
    switch (k) {
        case Transmog:   return QStringLiteral("Armour & weapons");
        case Mount:      return QStringLiteral("Mounts");
        case Emote:      return QStringLiteral("Emotes");
        case Marking:    return QStringLiteral("Body markings");
        case Jewelry:    return QStringLiteral("Jewelry");
        case Emblem:     return QStringLiteral("Emblems");
        case Headstone:  return QStringLiteral("Headstones");
        case TownPortal: return QStringLiteral("Town portals");
        case HairStyle:  return QStringLiteral("Hair styles");
        case FacialHair: return QStringLiteral("Facial hair");
        case Companion:  return QStringLiteral("Companions");
        case Power:      return QStringLiteral("Powers");
        case DyeArmor:   return QStringLiteral("Dyes");
        case None:       break;
    }
    return QStringLiteral("Other");
}

QString StoreProductIndex::classLabel(int idx)
{
    // This index IS eHeroClass: 0-4 were measured directly (see the header) and 5-6 came from the
    // tInvImages mapping in ENCRYPTED-CONTENT-HANDOFF.md — tInvImages is eHeroClass-indexed, so
    // agreeing at 0-6 means agreeing at 7. This used to be a hand-written switch that stopped at
    // 6 ("no sample ever pinned 7 down"), which meant a Warlock product showed a BLANK class while
    // every other table in the tool named index 7 Warlock. Delegating removes the disagreement.
    return QString::fromLatin1(ItemDef::heroClassName(idx));   // "" when out of range, as before
}

QString StoreProductIndex::classSummary(quint32 mask)
{
    if (!mask) return {};
    QStringList out;
    int set = 0;
    for (int i = 0; i < ItemDef::HeroClassCount; ++i) {
        if (!(mask & (1u << i))) continue;
        ++set;
        const QString l = classLabel(i);
        if (!l.isEmpty()) out << l;
    }
    // Every class on → "All classes". This deliberately requires ALL of them: the old test
    // accepted 7-of-8 because index 7 had no name and could never be counted, which stopped
    // being true the moment classLabel started naming Warlock. Left alone it would have
    // relabelled genuine 7-class products as "All classes".
    if (set == ItemDef::HeroClassCount) return QStringLiteral("All classes");
    return out.join(QStringLiteral(" \u00B7 "));
}

StoreProductIndex& StoreProductIndex::instance()
{
    static StoreProductIndex inst;
    return inst;
}

const StoreProductIndex::Product* StoreProductIndex::product(int sno) const
{
    const auto it = m_byId.constFind(sno);
    return it == m_byId.constEnd() ? nullptr : &it.value();
}

const StoreProductIndex::Product* StoreProductIndex::byName(const QString& name) const
{
    const auto it = m_byName.constFind(name.toLower());
    return it == m_byName.constEnd() ? nullptr : product(it.value());
}

void StoreProductIndex::install(QHash<int, Product> byId, QVector<int> bundles,
                                QHash<int, QVector<int>> soldIn)
{
    m_byId = std::move(byId);
    m_bundles = std::move(bundles);
    m_soldIn = std::move(soldIn);
    m_byName.clear();
    m_byName.reserve(m_byId.size());
    for (auto it = m_byId.constBegin(); it != m_byId.constEnd(); ++it)
        m_byName.insert(it.value().name.toLower(), it.key());
    m_building = false;
    m_ready = true;
    emit readyChanged();
}

void StoreProductIndex::reset()
{
    m_ready = false;
    // Generation counter, NOT `m_building = false`. reset() runs from MainWindow's d4data
    // fingerprint guard, immediately before every tab's refresh() — and CatalogueTab::refresh()
    // calls ensureBuilt(). Clearing m_building while a worker was still live let that refresh
    // start a SECOND detached thread on the same singleton: two of them writing one cache file,
    // and whichever install() landed last winning regardless of which data was current.
    //
    // Bumping the generation instead means an in-flight worker still finishes, but its install()
    // sees a stale generation and discards itself — the same shape the other background indexes use.
    ++m_generation;
    m_building = false;
    m_byId.clear();
    m_byName.clear();
    m_bundles.clear();
    m_soldIn.clear();
    QFile::remove(AppPaths::dataDir()
                  + QStringLiteral("/store_products_v%1.json").arg(kCacheVersion));
    emit readyChanged();
}

void StoreProductIndex::ensureBuilt(const QString& d4dataDir, const SnoIndex* index,
                                    CascReader* reader)
{
    if (m_ready || m_building || d4dataDir.isEmpty()) return;
    const QString prdDir = d4dataDir + QStringLiteral("/json/base/meta/StoreProduct");
    if (!QDir(prdDir).exists()) return;
    m_building = true;

    const QString strDir = d4dataDir + QStringLiteral("/json/enUS_Text/meta/StringList");
    const QString cache  = AppPaths::dataDir()
                           + QStringLiteral("/store_products_v%1.json").arg(kCacheVersion);

    // All of it on a worker: 7,496 JSON files plus a StringList probe each is seconds of disk work,
    // and this runs during startup pre-warm when the GUI must stay responsive.
    const QString d4data = d4dataDir;
    // SNAPSHOT the GearItem names here, on the CALLING thread, instead of handing the worker a
    // SnoIndex pointer.
    //
    // SnoIndex::nameForSno is const but WRITES a mutable lazy cache, so a `const SnoIndex*` used
    // from this worker would race the GUI thread, which calls the same method while filling texture
    // panels — same outer QHash, so a rehash on one side while the other holds an iterator is
    // undefined behaviour. Worse, MainWindow::reload() rebuilds the index (clear() + reparse) on
    // its own thread, and this build runs for seconds; reload() already defers for the icon audit
    // for exactly this reason, and there is no equivalent guard here.
    //
    // A snapshot removes the hazard rather than trying to synchronise around it: ~a few thousand
    // GearItem names copied once, and the worker then owns everything it reads.
    QHash<int, QString> gearNames;
    if (index)
        for (const SnoEntry& e : index->entries(98))   // 98 GearItem — the equipment slot
            if (!e.name.startsWith(QLatin1String("~unnamed_"))) gearNames.insert(e.snoId, e.name);
    // Every StoreProduct the GAME has, snapshotted for the same reason: the CASC fallback in the
    // worker needs to know which products exist in order to spot the ones d4data never described,
    // and it cannot touch the live index to find out. Names included so a recovered product has
    // something to display — encrypted ones arrive as ~unnamed_<sno> and are still listable.
    QHash<int, QString> prdEntries;
    if (index)
        for (const SnoEntry& e : index->entries(110))   // 110 StoreProduct
            prdEntries.insert(e.snoId, e.name);
    const int gen = m_generation;
    std::thread([this, prdDir, strDir, cache, d4data, gearNames, prdEntries, reader, gen]() {
        int sig = 0;
        {
            QDirIterator c(prdDir, QStringList{QStringLiteral("*.prd.json")}, QDir::Files);
            while (c.hasNext()) { c.next(); ++sig; }
        }
        // ── What the cache actually DEPENDS on, beyond the product count ─────────────────────
        // The file count alone cannot see the two inputs that decide whether slots and the
        // "sold in" map get filled at all:
        //
        //   · the SNO reference graph — absent in an older d4data checkout
        //   · a loaded SnoIndex — CatalogueTab::refresh() does not wait for it
        //
        // Built without either, the result is a cache with every slot empty and no provenance,
        // BYTE-INDISTINGUISHABLE from a good one. It then loads clean forever and the features
        // silently never work. That exact failure — a signature that could not see the thing it
        // depended on — is what pinned encrypted icons at 1 until the AppearanceMeta signature
        // learned to count NAMES, and the header of this file warns about it in the same words.
        //   · a CASC reader — without it the ~1,800 products d4data never described are absent,
        //     and that cache is again byte-indistinguishable from a complete one. The product
        //     COUNT from the index goes in too: a game patch that adds shop products changes what
        //     the fallback can recover while leaving the .prd.json count untouched.
        const bool haveGraph = QFile::exists(d4data
                                   + QStringLiteral("/json/outgoingSnoReferences.json"));
        const bool haveGear  = !gearNames.isEmpty();
        const bool haveCasc  = reader && reader->isReady();
        const QString inputs = QStringLiteral("%1|%2|%3|%4").arg(int(haveGraph)).arg(int(haveGear))
                                   .arg(int(haveCasc)).arg(prdEntries.size());

        // ── Cache hit ────────────────────────────────────────────────────────────────────────
        if (QFile::exists(cache)) {
            QFile f(cache);
            if (f.open(QIODevice::ReadOnly)) {
                const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
                if (root.value(QStringLiteral("sig")).toInt() == sig
                    && root.value(QStringLiteral("inputs")).toString() == inputs) {
                    QHash<int, Product> byId;
                    QHash<int, QVector<int>> soldIn;
                    {
                        const QJsonObject so = root.value(QStringLiteral("soldIn")).toObject();
                        for (auto i = so.constBegin(); i != so.constEnd(); ++i) {
                            QVector<int> v;
                            for (const QJsonValue& x : i.value().toArray()) v.append(x.toInt());
                            soldIn.insert(i.key().toInt(), v);
                        }
                    }
                    QVector<int> bundles;
                    const QJsonArray arr = root.value(QStringLiteral("products")).toArray();
                    byId.reserve(arr.size());
                    for (const QJsonValue& pv : arr) {
                        const QJsonObject o = pv.toObject();
                        Product p;
                        p.sno         = o.value(QStringLiteral("sno")).toInt();
                        p.name        = o.value(QStringLiteral("name")).toString();
                        p.title       = o.value(QStringLiteral("title")).toString();
                        p.description = o.value(QStringLiteral("desc")).toString();
                        p.eType       = o.value(QStringLiteral("etype")).toInt(-1);
                        p.branch      = o.value(QStringLiteral("branch")).toString();
                        p.season      = o.value(QStringLiteral("season")).toInt();
                        p.seasonName  = o.value(QStringLiteral("sname")).toString();
                        p.slot        = o.value(QStringLiteral("slot")).toString();
                        p.payloadGroup = o.value(QStringLiteral("pgrp")).toInt();
                        p.fromCasc     = o.value(QStringLiteral("casc")).toBool();
                        {
                            auto getIds = [&o](const char* k, QVector<int>& v) {
                                for (const QJsonValue& x : o.value(QLatin1String(k)).toArray())
                                    v.append(x.toInt());
                            };
                            getIds("req", p.requires_);
                            getIds("reqn", p.requiresNot);
                            getIds("addon", p.addOns);
                            for (const QJsonValue& x : o.value(QStringLiteral("cart")).toArray())
                                p.cardArt.append(quint32(x.toDouble()));
                        }
                        p.hasVfx      = o.value(QStringLiteral("vfx")).toBool();
                        p.classMask   = quint32(o.value(QStringLiteral("cls")).toDouble());
                        p.kind        = Kind(o.value(QStringLiteral("kind")).toInt());
                        p.payloadSno  = o.value(QStringLiteral("psno")).toInt();
                        p.payloadName = o.value(QStringLiteral("pname")).toString();
                        for (const QJsonValue& cv : o.value(QStringLiteral("kids")).toArray())
                            p.children.append(cv.toInt());
                        for (const QJsonValue& av : o.value(QStringLiteral("art")).toArray())
                            p.art.append(quint32(av.toDouble()));
                        if (p.sno > 0) byId.insert(p.sno, p);
                    }
                    for (const QJsonValue& bv : root.value(QStringLiteral("bundles")).toArray())
                        bundles.append(bv.toInt());
                    QMetaObject::invokeMethod(this, [this, byId, bundles, soldIn, gen]() mutable {
                        if (gen != m_generation) return;   // a reset() overtook this build
                        install(std::move(byId), std::move(bundles), std::move(soldIn));
                    }, Qt::QueuedConnection);
                    return;
                }
            }
        }

        // ── Cold build ───────────────────────────────────────────────────────────────────────
        QHash<int, Product> byId;
        QHash<int, int> kindGroup;   // Kind -> SNO group its payload lives in; learned below
        byId.reserve(sig + 64);
        const int total = qMax(1, sig);
        int seen = 0;
        QDirIterator it(prdDir, QStringList{QStringLiteral("*.prd.json")}, QDir::Files);
        while (it.hasNext()) {
            const QString path = it.next();
            if ((++seen % 256) == 0) {
                const int pct = seen * 100 / total;
                QMetaObject::invokeMethod(this, [this, pct]() { emit progress(pct); },
                                          Qt::QueuedConnection);
            }
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QJsonObject d = QJsonDocument::fromJson(f.readAll()).object();
            f.close();

            Product p;
            p.sno  = d.value(QStringLiteral("__snoID__")).toInt();
            if (p.sno <= 0) continue;
            p.name = QFileInfo(path).fileName();
            p.name.chop(int(qstrlen(".prd.json")));
            p.eType  = d.value(QStringLiteral("eType")).toInt(-1);
            p.branch = d.value(QStringLiteral("szProductReleaseBranch")).toString();
            {
                const QJsonValue sv = d.value(QStringLiteral("snoAssociatedSeason"));
                p.season     = rawSno(sv);
                p.seasonName = refName(sv);   // "Season 8" — it was in the ref object all along
            }
            p.hasVfx = d.value(QStringLiteral("bHasVFX")).toBool();
            // Product relationships. arRequiresOwning is the only source for "this was never sold,
            // it came with the Season 3 premium pass".
            for (const auto& fld : {std::make_pair("arRequiresOwning",    &Product::requires_),
                                    std::make_pair("arRequiresNotOwning", &Product::requiresNot),
                                    std::make_pair("arAddOnBundles",      &Product::addOns)})
                for (const QJsonValue& rv : d.value(QLatin1String(fld.first)).toArray())
                    if (const int rs = rawSno(rv)) (p.*(fld.second)).append(rs);
            // Alternate card art: {hCardImage, hCardHoverImage}. The hover image in particular is
            // art that never appears anywhere else.
            for (const QJsonValue& cv : d.value(QStringLiteral("arCardArtVariants")).toArray()) {
                const QJsonObject co = cv.toObject();
                for (const char* k : {"hCardImage", "hCardHoverImage"}) {
                    const QJsonValue hv = co.value(QLatin1String(k));
                    const quint32 h = quint32(hv.toDouble());
                    if (hv.isDouble() && h) p.cardArt.append(h);
                }
            }
            {
                const QJsonArray fc = d.value(QStringLiteral("fPreviewOnClasses")).toArray();
                for (int i = 0; i < fc.size() && i < 8; ++i)
                    if (fc.at(i).toInt() != 0) p.classMask |= (1u << i);
            }

            for (const QJsonValue& cv : d.value(QStringLiteral("arBundledProducts")).toArray()) {
                const int cs = rawSno(cv);
                if (cs > 0) p.children.append(cs);
            }
            for (const PayloadField& pf : kPayloads) {
                const QJsonValue v = d.value(QLatin1String(pf.key));
                const int s = rawSno(v);
                if (s > 0) {
                    p.kind = pf.kind; p.payloadSno = s; p.payloadName = refName(v);
                    // Which SNO GROUP this kind's payload lives in — Transmog→Item, Headstone→
                    // Actor, and so on. LEARNED from the ref object the JSON already carries rather
                    // than hardcoded, so a renumbered group needs no code change and a kind nobody
                    // anticipated still resolves. The CASC fallback below has only a raw sno and
                    // needs exactly this to turn it back into a name.
                    p.payloadGroup = v.toObject().value(QStringLiteral("__group__")).toInt();
                    if (p.payloadGroup > 0) kindGroup.insert(int(p.kind), p.payloadGroup);
                    break;
                }
            }
            for (const char* af : kArtFields) {
                const QJsonValue v = d.value(QLatin1String(af));
                // Handles are u32; QJsonValue stores them as double, so read as double then narrow.
                const quint32 h = quint32(v.toDouble());
                if (v.isDouble() && h) p.art.append(h);
            }

            // Shop-facing name + lore. Absent for plenty of products (internal/test entries), which
            // is why the UI falls back to the SNO name rather than showing a blank row.
            QFile sf(strDir + QStringLiteral("/StoreProduct_%1.stl.json").arg(p.name));
            if (sf.open(QIODevice::ReadOnly)) {
                const QJsonObject so = QJsonDocument::fromJson(sf.readAll()).object();
                for (const QJsonValue& sv : so.value(QStringLiteral("arStrings")).toArray()) {
                    const QJsonObject e = sv.toObject();
                    const QString lbl = e.value(QStringLiteral("szLabel")).toString();
                    if (lbl == QLatin1String("Name"))
                        p.title = e.value(QStringLiteral("szText")).toString();
                    else if (lbl == QLatin1String("Description"))
                        p.description = e.value(QStringLiteral("szText")).toString();
                }
            }
            byId.insert(p.sno, p);
        }

        // ── CASC fallback: the products d4data does not describe ────────────────────────────────
        // The snapshot ships ~7,500 .prd.json against 9,308 StoreProduct records in the game. The
        // gap is encrypted and newly-patched content — the Doom collab armour is absent for every
        // class except Druid and Rogue purely because only those two shipped a JSON file.
        //
        // The layout below was MEASURED, not guessed, by hunting values we already knew from JSON
        // inside the binary and keeping only what was unanimous:
        //
        //   0x00        magic 0xDEADBEEF, then a 16-byte file header; the record starts at 0x10,
        //               and every offset stored INSIDE the record is relative to that.
        //   0x28 / 0x2c array descriptor for arBundledProducts: {u32 relOffset, u32 byteSize}.
        //               Absolute position = 0x10 + relOffset; count = byteSize / 4.
        //   0x74..0xa4  thirteen contiguous u32 payload slots, in the order Kind declares them —
        //               snoItemTransmog, snoMount, snoEmote, … , snoPower, snoDyeArmor.
        //               Nine of the thirteen were confirmed unanimous across 5,245 products
        //               (armour+weapons alone: 4,447 of 4,447); the remaining four sit between
        //               confirmed neighbours on the same 4-byte stride.
        //   EMPTY slot  is 0xFFFFFFFF, NOT 0 — a bundle has all thirteen set to ~0u.
        //
        // The descriptor at 0x28 is read rather than assuming the array is always at 0x260, which
        // is what the samples happened to show: a record whose header differs would then be parsed
        // from the wrong place, silently. Reading the offset the file gives us costs nothing and
        // cannot drift.
        //
        // EVERY field is validated. A record that fails any check is skipped entirely rather than
        // contributing a half-built product — a bundle listing children that resolve to nothing
        // looks broken, which is worse than the honest absence we have today.
        if (reader && reader->isReady() && !prdEntries.isEmpty()) {
            constexpr int  kRecBase   = 0x10;    // the record proper, after the file header
            constexpr int  kChildDesc = 0x28;    // {relOffset, byteSize}
            constexpr int  kPayload0  = 0x74;    // first of thirteen u32 slots, 4-byte stride
            constexpr quint32 kEmpty  = 0xFFFFFFFFu;
            auto u32 = [](const QByteArray& b, int off) -> quint32 {
                return quint32(uchar(b[off])) | quint32(uchar(b[off + 1])) << 8
                     | quint32(uchar(b[off + 2])) << 16 | quint32(uchar(b[off + 3])) << 24;
            };
            int recovered = 0, rejected = 0, childless = 0;
            for (auto pe = prdEntries.constBegin(); pe != prdEntries.constEnd(); ++pe) {
                if (byId.contains(pe.key())) continue;        // json already described this one
                const QByteArray m = reader->readMetaBySno(quint64(pe.key()));
                if (m.size() < kPayload0 + 13 * 4) { ++rejected; continue; }

                Product p;
                p.sno  = pe.key();
                p.name = pe.value();      // may be ~unnamed_… for an encrypted record; still listable
                p.fromCasc = true;

                // Children, via the descriptor.
                if (m.size() >= kChildDesc + 8) {
                    // qint64 throughout: these are attacker-shaped values from a file, and
                    // kRecBase + rel in int is signed overflow (UB) for a huge rel. The guards
                    // below would reject the wrapped result on MSVC, but "wrong answer that
                    // happens to fail closed" is not the same as "cannot go wrong".
                    const qint64 rel   = u32(m, kChildDesc);
                    const qint64 bytes = u32(m, kChildDesc + 4);
                    const qint64 at    = qint64(kRecBase) + rel;
                    if (bytes > 0 && (bytes % 4) == 0 && bytes <= 4096
                        && at >= kPayload0 && at + bytes <= qint64(m.size())) {
                        for (int i = 0; i < int(bytes / 4); ++i) {
                            const quint32 c = u32(m, int(at) + i * 4);
                            if (c && c != kEmpty) p.children.append(int(c));
                        }
                    }
                }

                // Payload: the first of the thirteen slots that holds a real sno decides the kind.
                for (int k = 0; k < 13; ++k) {
                    const quint32 v = u32(m, kPayload0 + k * 4);
                    if (!v || v == kEmpty) continue;
                    p.kind       = Kind(int(Transmog) + k);
                    p.payloadSno = int(v);
                    // The NAME is what every resolver downstream joins on, and the binary does not
                    // carry it. It is NOT looked up here: SnoIndex::nameForSno writes a mutable
                    // lazy cache and this is a worker thread — the same race the gearNames snapshot
                    // above exists to avoid. Instead record which GROUP to look in, learned from
                    // the JSON products rather than hardcoded, and let the GUI thread resolve it on
                    // demand (CatalogueTab::payloadNameOf).
                    p.payloadGroup = kindGroup.value(int(p.kind), 0);
                    break;
                }

                // Art handles. WEAKER EVIDENCE than everything above, and treated as such: these
                // offsets came from a histogram over 401 products, not from a unanimity test,
                // because the index skips zero handles when it builds `art` and so cannot say
                // which field a given value came from. What the histogram does show is six
                // offsets that recur far above noise (357, 217, 182, 128, 58, 25 hits).
                //
                // Safe to read on that basis because the failure mode is bounded: a handle that is
                // wrong simply fails to resolve in IconIndex and the UI falls through to the
                // name-based shop-art lookup it already uses. Nothing is displayed incorrectly,
                // and a bundle with no art at all is the status quo.
                for (int aOff : {0x114, 0x110, 0x10c, 0x15c, 0x148, 0x140}) {
                    if (aOff + 4 > m.size()) continue;
                    const quint32 h = u32(m, aOff);
                    if (h && h != kEmpty && !p.art.contains(h)) p.art.append(h);
                }

                // A record with neither children nor a payload tells us nothing a user could act
                // on. Counted so the log can say how much of the gap is genuinely empty.
                if (p.children.isEmpty() && p.payloadSno == 0) { ++childless; continue; }
                byId.insert(p.sno, p);
                ++recovered;
            }
            qInfo("StoreProductIndex: CASC fallback — %d product(s) recovered that d4data does not "
                  "describe, %d unreadable, %d with no children and no payload",
                  recovered, rejected, childless);
        } else {
            // NOT `index` here: it is deliberately uncaptured, because touching a live SnoIndex
            // from this worker is the race the gearNames/prdEntries snapshots exist to avoid.
            // prdEntries IS the captured evidence of whether the index had anything to give.
            qInfo("StoreProductIndex: CASC fallback skipped (reader %s, %d product sno(s) known) — "
                  "products absent from d4data will not appear",
                  (reader && reader->isReady()) ? "ready" : "not ready", int(prdEntries.size()));
        }

        // ── SNO reference graph: real slots, and where each asset was sold ──────────────────────
        // d4data ships json/outgoingSnoReferences.json — 554,741 SNOs with their outgoing edges.
        // Walking a product outward gives the chain the store itself uses:
        //     Bundle -> child StoreProducts -> Item -> GearItem (the SLOT) + Actor
        //
        // Two things come out of it that nothing else provides:
        //   · the real equipment slot, instead of guessing from a name token. Measured on
        //     Bundle_HArmor_bar_stor235: Helm / ChestArmor / Gloves / Legs / Boots / Polearm.
        //   · the inverse map — asset -> the products that reach it — which answers "which bundle
        //     sold this?" from any tab.
        //
        // It also catches contents a name would miss: that Barbarian bundle contains
        // twoHandPolearm_stor059, a weapon from an entirely different set.
        //
        // Depth 4 is measured, not arbitrary: product->product->item->{gearitem,actor} is the whole
        // chain, and the graph stops at Actor because Actor->Appearance is not in it.
        QHash<int, QVector<int>> soldIn;
        {
            QFile gf(d4data + QStringLiteral("/json/outgoingSnoReferences.json"));
            if (gf.open(QIODevice::ReadOnly)) {
                const QJsonObject g = QJsonDocument::fromJson(gf.readAll()).object();
                gf.close();
                auto edges = [&g](int sno) {
                    QVector<int> out;
                    for (const QJsonValue& v : g.value(QString::number(sno)).toArray())
                        out.append(v.toInt());
                    return out;
                };
                // Group ids: 98 GearItem (the slot), 73 Item, 110 StoreProduct.
                for (auto i = byId.begin(); i != byId.end(); ++i) {
                    Product& prod = i.value();
                    QSet<int> seen{prod.sno};
                    QVector<int> frontier{prod.sno};
                    // THREE expansions, not four. The measured chain is
                    //     Bundle -> child StoreProduct -> Item -> {GearItem, Actor}
                    // and the frontier starts at the product itself, so `depth < 3` walks exactly
                    // that. A fourth hop expanded every ACTOR's own references into soldIn, which
                    // attributes shared assets — base materials, shared anims, physics records — to
                    // every bundle that happens to reach them. The Models tab probes provenance BY
                    // ACTOR, so that turned a shared actor into a list of unrelated bundles.
                    for (int depth = 0; depth < 3 && !frontier.isEmpty(); ++depth) {
                        QVector<int> next;
                        for (int s : frontier)
                            for (int r : edges(s)) {
                                if (seen.contains(r)) continue;
                                seen.insert(r);
                                next.append(r);
                                // Reverse map: this product reaches r. Products themselves are
                                // navigation, not contents, so they are not recorded as "sold".
                                if (!byId.contains(r)) soldIn[r].append(prod.sno);
                                // Membership in the snapshot IS the group test — it only holds
                                // GearItem (98) names, so a hit means "this is the slot".
                                if (prod.slot.isEmpty())
                                    prod.slot = gearNames.value(r);
                            }
                        frontier = next;
                    }
                }
                qInfo("StoreProductIndex: reference graph — provenance for %d asset(s)",
                      int(soldIn.size()));
            } else {
                qInfo("StoreProductIndex: no outgoingSnoReferences.json — slots and \"sold in\" "
                      "provenance unavailable (older d4data checkout)");
            }
        }

        QVector<int> bundles;
        for (auto i = byId.constBegin(); i != byId.constEnd(); ++i)
            if (i.value().isBundle()) bundles.append(i.key());
        std::sort(bundles.begin(), bundles.end(), [&byId](int a, int b) {
            const Product& pa = byId[a];
            const Product& pb = byId[b];
            const QString ka = pa.title.isEmpty() ? pa.name : pa.title;
            const QString kb = pb.title.isEmpty() ? pb.name : pb.title;
            const int c = ka.compare(kb, Qt::CaseInsensitive);
            return c != 0 ? c < 0 : a < b;
        });

        // Persist. ~7.5k small records; a few MB of JSON against seconds of disk crawling.
        QJsonObject root;
        QJsonArray arr;
        for (auto i = byId.constBegin(); i != byId.constEnd(); ++i) {
            const Product& p = i.value();
            QJsonObject o;
            o.insert(QStringLiteral("sno"), p.sno);
            o.insert(QStringLiteral("name"), p.name);
            if (!p.title.isEmpty())       o.insert(QStringLiteral("title"), p.title);
            if (!p.description.isEmpty()) o.insert(QStringLiteral("desc"), p.description);
            o.insert(QStringLiteral("etype"), p.eType);
            if (!p.branch.isEmpty()) o.insert(QStringLiteral("branch"), p.branch);
            if (p.season) o.insert(QStringLiteral("season"), p.season);
            if (!p.seasonName.isEmpty()) o.insert(QStringLiteral("sname"), p.seasonName);
            if (!p.slot.isEmpty()) o.insert(QStringLiteral("slot"), p.slot);
            if (p.payloadGroup) o.insert(QStringLiteral("pgrp"), p.payloadGroup);
            if (p.fromCasc)     o.insert(QStringLiteral("casc"), true);
            {
                auto putIds = [&o](const char* k, const QVector<int>& v) {
                    if (v.isEmpty()) return;
                    QJsonArray a; for (int x : v) a.append(x);
                    o.insert(QLatin1String(k), a);
                };
                putIds("req", p.requires_);
                putIds("reqn", p.requiresNot);
                putIds("addon", p.addOns);
                if (!p.cardArt.isEmpty()) {
                    QJsonArray a; for (quint32 h : p.cardArt) a.append(double(h));
                    o.insert(QStringLiteral("cart"), a);
                }
            }
            if (p.hasVfx) o.insert(QStringLiteral("vfx"), true);
            if (p.classMask) o.insert(QStringLiteral("cls"), double(p.classMask));
            if (p.kind != None) {
                o.insert(QStringLiteral("kind"), int(p.kind));
                o.insert(QStringLiteral("psno"), p.payloadSno);
                o.insert(QStringLiteral("pname"), p.payloadName);
            }
            if (!p.children.isEmpty()) {
                QJsonArray k; for (int c : p.children) k.append(c);
                o.insert(QStringLiteral("kids"), k);
            }
            if (!p.art.isEmpty()) {
                QJsonArray a; for (quint32 h : p.art) a.append(double(h));
                o.insert(QStringLiteral("art"), a);
            }
            arr.append(o);
        }
        QJsonArray bj; for (int b : bundles) bj.append(b);
        root.insert(QStringLiteral("sig"), sig);
        // Recorded so the next launch can tell a cache built WITH the graph and index from one
        // built without them. Absent on a v3 file written before this check existed, which reads
        // as a mismatch and forces exactly the one rebuild we want.
        root.insert(QStringLiteral("inputs"), inputs);
        {
            // The reverse map, flattened. Rebuilding it means parsing a 47 MB JSON graph and
            // walking every product — seconds of work that must not happen on every launch.
            QJsonObject so;
            for (auto i = soldIn.constBegin(); i != soldIn.constEnd(); ++i) {
                QJsonArray a; for (int v : i.value()) a.append(v);
                so.insert(QString::number(i.key()), a);
            }
            root.insert(QStringLiteral("soldIn"), so);
        }
        root.insert(QStringLiteral("products"), arr);
        root.insert(QStringLiteral("bundles"), bj);
        QDir().mkpath(QFileInfo(cache).absolutePath());
        // Write to .part and rename, the way SnoIndex::saveToCache does. A direct write to the
        // live path leaves a TRUNCATED cache if the process dies mid-write (or if two builds ever
        // overlap), and a short-but-valid-looking JSON is worse than no cache: it loads clean and
        // silently serves a partial catalogue.
        const QString part = cache + QStringLiteral(".part");
        QFile out(part);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
            out.close();
            QFile::remove(cache);
            QFile::rename(part, cache);
        }

        QMetaObject::invokeMethod(this, [this, byId, bundles, soldIn, gen]() mutable {
            if (gen != m_generation) return;   // a reset() overtook this build
            install(std::move(byId), std::move(bundles), std::move(soldIn));
        }, Qt::QueuedConnection);
    }).detach();
}
