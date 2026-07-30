#pragma once
#include <QByteArray>
#include <QVector>

// ── ptAppearanceMaterials, read from the CASC meta binary ───────────────────────────────────────
//
// WHY. Every material path in this tool is name-and-JSON keyed:
//   appearanceRoster(Appearance/<name>.app.json) -> material NAME
//     -> Material/<name>.mat.json -> sno + texture names
//       -> Texture/<name>.tex.json -> width/height/format
// Encrypted appearances (Doom collab, seasonal store sets) ship none of those files, so they render
// white with sub-objects labelled "part 7", "part 8", ... This reads the same list straight out of
// the binary, so no name is required.
//
// LAYOUT — derived from raw meta blobs and VERIFIED against two independent ground truths:
// the d4analyzer GLB export (necF_stor245_TRS 10 entries, necF_stor245_LEG 4) for ENCRYPTED
// appearances, and d4data JSON for eight named ones. 10 of 10 reproduce exactly, hashes and snos,
// in order.
//
//   meta +0xC8 : u32 dataOffset, u32 byteSize     <- fixed header slot
//   records    : first record at dataOffset + 16, 32 bytes each, count = byteSize / 32
//                  +0  u32 per-entry hash   (distinguishes two entries sharing one material —
//                                            necF_stor245_LEG has sno 2335347 twice)
//                  +24 u32 soaOffset        (ptSOAs; its own array descriptor)
//   SOA        : at soaOffset
//                  +20 snoMaterial          +24 snoOverrideMaterial
//                  +28 snoCloth             +32 snoHighQualityClothOverride
//
// The +16 bias on the record base is real and measured, not a fudge: the descriptor's dataOffset
// points 16 bytes ahead of the first record in every sample. The same bias does NOT apply to
// soaOffset, which is why the two were derived separately rather than assumed to match.
//
// An entry can be cloth-only (snoMaterial == 0xFFFFFFFF) — three of the fourteen verified entries
// are. Preference order matches MaterialDecode::appearanceRoster so the binary route and the JSON
// route cannot disagree about which sno represents an entry.
namespace AppearanceMatBin {

struct Entry {
    quint32 hash = 0;   // per-entry hash; two entries may share one material sno
    int     sno  = 0;   // override / material / cloth / HQ cloth, first non-empty
};

// Empty when the blob carries no material array (or is not an appearance meta). Index i corresponds
// to MeshPrimitive::materialIndex == i — the same ordinal ModelParser reads at meta.i32(so + 0x60).
QVector<Entry> read(const QByteArray& meta);

// Just the snos, positionally aligned with materialIndex. Convenience for the roster path.
QVector<int> snos(const QByteArray& meta);

}  // namespace AppearanceMatBin
