#pragma once
// ── Cache-version discipline ────────────────────────────────────────────────────────────────────
//
// THE RULE
//   When the MEANING of anything a cache stores changes, bump its version in the FILENAME.
//   Not the writer, not a header field — the filename, so an old file can never be opened at all.
//
// WHY IT IS WRITTEN DOWN
//   A stale cache is the worst failure mode this project has: it loads cleanly, reports success,
//   and the only symptom is the absence of something you went looking for. It cost two false
//   diagnoses in one session.
//
//   Concretely: SnoIndex gained a pass that recovers names for encrypted appearances. The pass runs
//   on a cache MISS. coretoc_v1.bin was still valid, so every launch took the cache path, skipped
//   the pass, and the recovered assets silently never appeared — while the code that recovered them
//   was demonstrably correct. Renaming the file to coretoc_v2.bin fixed it instantly.
//
// THE TEST
//   Ask: "could a file written by the PREVIOUS build be read by this one and produce a different
//   answer than a fresh computation would?" If yes, bump. Adding a field, changing what a field
//   means, changing what gets INCLUDED, or adding a pass that fills the cache — all qualify. Only
//   a pure performance change to how the same bytes are produced does not.
//
// CACHES IN THIS PROJECT (keep this list current, AND the matching pruneOldCaches numbers in
// main.cpp — a stale number there only under-prunes, but a stale entry HERE misleads the next
// person into thinking a bump already happened)
//   coretoc_v3.bin              SnoIndex        entry NAMES — v2 encrypted-name recovery,
//                                               v3 EncryptedNameDict pass
//   casc_index_v1.bin           CascReader      archive index
//   tvfs_paths_v1.bin           CascReader      TVFS path table
//   appearance_meta_v<N>.json   AppearanceMeta  kCacheVersion, currently 23 — v23 binds icons for
//                                               weapons/mounts/trophies (route 3). A worked example
//                                               of the rule below: the CODE changed what the crawl
//                                               includes while every signature input stayed
//                                               identical, so the old cache stayed valid and the
//                                               new bindings silently never ran.
//   asset_links_v<N>.bin        AssetLinks      kCacheVersion, currently 1
//   item_hover_v<N>.json        ItemHoverIndex  currently 2 — v2 added weapons (route 3). Signature
//                                               is Item + StringList + APPEARANCE counts + the
//                                               build stamp; the Appearance count is there because
//                                               the route-3 test reads that folder.
//   wardrobe_anims_v<N>.json    WardrobeTab2    currently 1 — the ui_wardrobe clip-name index
//   latest_v<N>.bin             SnoIndex        currently 2 — per-build "what is new" baseline
//   build_history_v<N>.bin      SnoIndex        currently 1 — which build each asset first appeared in
//   back_trophy_v<N>.json       BackTrophyIndex kCacheVersion, currently 4
//   icon_index_v<N>.json        IconIndex       kCacheVersion, currently 4 — v4 added the CASC pass
//   tex_info_v<N>.bin           TexturesTab     format/dimension scan, currently 2 — v2 added the
//                                               CASC texture-def fallback
//   stable_index_v<N>.bin       StableTab2      currently 6
//   store_products_v<N>.json    StoreProductIndex  currently 4 — the shop catalogue. v3 added the
//                                               reference-graph slot + soldIn map, season name and
//                                               relations; v4 added the products recovered from
//                                               the CASC binary that d4data never described.
//                                               Signature is the .prd.json file count PLUS an
//                                               `inputs` string naming whether the reference graph,
//                                               the SnoIndex and a CASC reader were available, and
//                                               how many StoreProduct snos the game has (see the
//                                               signature rule below).
//
// NOT in this list, deliberately: data\index_cache\anim_index.bin and entity_index.bin version
// themselves with a magic STRING ("ANIMIDX2" / "ENTIDX3"), so pruneOldCaches — which matches a
// numeric suffix — can never apply to them. Bump the magic to invalidate those.
//
// TWO caches carry no signature of their own (magic number only) and are therefore DELETED by the
// fingerprint guard in MainWindow::finishReload instead: stable_index_v*.bin and tex_info_v*.bin.
// Left to themselves they were frozen for the life of the install — a mount added by a patch never
// appeared in the Stable tab. If you give either one a real signature, drop it from that block.
//
// Also invalidate on a game-build or d4data fingerprint change where the cache depends on either —
// several already do this via a sig/appCount/dadSig guard, which is complementary, not a substitute:
// the fingerprint catches "the DATA changed", the version catches "our INTERPRETATION changed".
//
// ── THE SIGNATURE RULE ──────────────────────────────────────────────────────────────────────────
//
//   A cache signature must be able to SEE every input its build reads. If the build opens a
//   directory, reads a file, or behaves differently because some optional source was present, that
//   fact belongs in the signature.
//
// This is a separate rule from the version bump above and it has failed twice, both times the same
// way and both times invisibly:
//
//   · AppearanceMeta counted APPEARANCES. A d4data commit that RENAMED encrypted appearances left
//     the count identical, so the cache stayed valid and the name-keyed icon join kept matching
//     against names that no longer existed. Encrypted icons sat at 1. Counting NAMES as well
//     (v22) took it to 106.
//   · StoreProductIndex counted .prd.json files. Whether the reference graph and the SnoIndex were
//     available decides whether slots and provenance get built at all — build once without them
//     and you get a cache that is byte-indistinguishable from a good one, loads clean forever, and
//     silently has neither feature. Fixed with an explicit `inputs` string.
//
// Both produce the project's worst failure mode: a cache that reports success and is quietly
// wrong. A file count is a proxy for "did the data change"; it is not the thing itself.
//
// PRACTICAL TEST when adding an input to any index build:
//   "If this input changed, or vanished, or arrived late — would the signature change?"
//   If no, add it in the SAME edit that reads it. ItemHoverIndex::ensureBuilt shows the shape:
//   one count per directory read, and a comment saying so next to the signature.
