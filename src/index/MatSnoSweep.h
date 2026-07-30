#pragma once
#include <QString>

class SnoIndex;
class CascReader;

// ── D4_MATSNO_SWEEP=1 — derive the appearance material-sno table from the corpus ────────────────
//
// THE PROBLEM. Encrypted appearances (Doom collab, seasonal store sets) ship no
// json/base/meta/Appearance/<name>.app.json, and the tool's entire material chain is keyed by NAME
// through that file — appearanceRoster -> Material/<name>.mat.json -> Texture/<name>.tex.json. So
// they render white with sub-objects labelled "part 7", "part 8", ... The binary already supplies
// the per-sub-object material ORDER (ModelParser.cpp:356, meta.i32(so + 0x60)); only the sno LIST
// that order indexes into is missing.
//
// WHAT IS ALREADY MEASURED (from D4_DUMP_MATSNO over ~32 hand-clicked appearances):
//   · the material sno sits at record offset +20        (held in every sample)
//   · records are 72 bytes apart                        (measured twice — from sno-hit spacing,
//                                                        and from the descriptor's size field)
//   · the descriptor reads  ... 0 0 0 0 0 [dataOffset] [72] ...
//     which is the (offset,size) array convention ModelParser::arr() already uses
//
// WHAT THIS SWEEP SETTLES. Where the array ENDS, and whether the three facts above actually
// reproduce the JSON. For every NAMED appearance the JSON states the true material set, so the walk
// is scored against it: a run of hits followed by misses gives the length rule; hits interleaved
// with misses means the 72-byte record holds more than one sno field. Hand-clicking produced 32
// samples biased toward names I happened to think of; this visits the whole corpus.
//
// Self-validating by construction: if the walk cannot reproduce the JSON on named appearances, the
// offsets are wrong and no reader gets written on top of them.
//
// Writes next to the exe:
//   matsno_sweep.csv      one row per appearance — descriptor, walk result, agreement
//   matsno_sweep.txt      the verdict: does a single length rule explain the corpus, and which
// Returns a one-line summary.
QString runMatSnoSweep(const QString& d4, SnoIndex* idx, CascReader* rd, QWidget* parent);
