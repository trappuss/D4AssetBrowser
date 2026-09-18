#pragma once

// ── What a product CONTAINS, answered once ──────────────────────────────────────────────────────
// These four are the Catalogue's descent over StoreProductIndex, and they used to live in
// CatalogueTab.cpp's anonymous namespace. IconAudit needs the SAME answer: its fourth-route
// measurement (the payload actor's portrait) read the ROW's own payloadName, while the tab asks
// each of the row's leaf CONTENTS — so the audit reported "0 rows filled by the portrait" for a
// route that demonstrably fills rows on screen. A second copy would have drifted the same way
// again, so the descent moved here and both callers include it.
//
// Pure functions over an already-built StoreProductIndex: no widgets, no tab state, no lazy
// caches, nothing mutable. That is what makes them safe from IconAudit's worker thread, which is
// the reason the audit could not simply call SnoIndex::nameForSno instead.

#include "index/StoreProductIndex.h"

#include <QSet>
#include <QVector>

namespace ProductContents {

// ── What a product's contents ARE ───────────────────────────────────────────────────────────────
// A product with no children is not an empty bundle. It is a single item, and the shop sells 337
// of them that way - Scythe_stor007, mnt_amor103_horse_stor - none of which any amount of drilling
// into bundles will ever reach. Answering "its contents are itself" in ONE place makes the strip,
// the contents tree, the hover card and the exporter all handle it unchanged, instead of each
// growing its own leaf special case.
//
// Locked products are deliberately excluded: their children are unreadable, and saying the product
// is its own content would be inventing one rather than reporting one.
inline QVector<int> contentSnos(const StoreProductIndex::Product& b)
{
    if (!b.children.isEmpty() || b.encrypted) return b.children;
    return QVector<int>{ b.sno };
}

// ── A pack's contents are not always one level down ─────────────────────────────────────────────
// AddOn_CollectionPack_WOW has 8 children, and not one of them is an item: each is a per-class
// BUNDLE (Bundle_HArmor_bar_stor268) holding the 5-7 pieces of that class's set. contentSnos stops
// at those 8, so everything downstream saw eight products with no payload, no kind and no
// appearance - which is exactly what the tab showed: eight rows filed under "Other", RESOLVES TO
// blank, and "8 unresolved" in the status line. The 45 actual pieces were unreachable, so the pack
// exported its shop art and nothing else.
//
// The SLOT column made it look like a different bug than it was. StoreProductIndex resolves `slot`
// off the SNO reference graph, which for a container returns the first GearItem it reaches - and
// Helm_Cosmetic_* is authored as child [0] of every one of these sets. So all eight rows read
// "Helm", and a pack of eight full armour sets looked like a pack of eight helmets. Position
// mistaken for meaning, the same shape as the first-non-zero art handle.
//
// Descends to the products that actually carry something. A container that ALSO has a payload is
// kept as well as descended into, rather than one or the other being guessed at.
inline void collectLeafContents(const StoreProductIndex& idx, int sno, QVector<int>& out,
                                QSet<int>& seen, int depth)
{
    // Depth cap and visited set are belt and braces: the observed nesting is two deep, but this
    // walks authored data, and a cycle here would hang the UI thread rather than misdraw a row.
    if (depth > 8 || seen.contains(sno)) return;
    seen.insert(sno);
    const auto* p = idx.product(sno);
    // A locked product's children are unreadable, so it is a leaf whatever it may really contain -
    // the same reason contentSnos refuses to invent contents for one.
    if (!p || p->encrypted) { out.append(sno); return; }
    if (p->payloadSno > 0 || p->children.isEmpty()) out.append(sno);
    for (int k : p->children) collectLeafContents(idx, k, out, seen, depth + 1);
}

// contentSnos flattened. Identical to contentSnos for every ordinary bundle - its children are
// already leaves - so only the nested packs change.
inline QVector<int> leafContentSnos(const StoreProductIndex& idx,
                                    const StoreProductIndex::Product& b)
{
    QVector<int> out;
    QSet<int> seen;
    for (int cs : contentSnos(b)) collectLeafContents(idx, cs, out, seen, 0);
    return out;
}

// Is this product a GROUP rather than a thing - children, but nothing of its own to resolve?
inline bool isContainer(const StoreProductIndex::Product& p)
{
    return !p.children.isEmpty() && !p.encrypted && p.payloadSno <= 0;
}

}  // namespace ProductContents
