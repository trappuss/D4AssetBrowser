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
// CACHES IN THIS PROJECT (keep this list current)
//   coretoc_v2.bin              SnoIndex        entry NAMES — bumped for encrypted-name recovery
//   casc_index_v1.bin           CascReader      archive index
//   tvfs_paths_v1.bin           CascReader      TVFS path table
//   appearance_meta_v<N>.json   AppearanceMeta  kCacheVersion, currently 21
//   asset_links_v<N>.bin        AssetLinks      kCacheVersion
//   backtrophy_v<N>.bin         BackTrophyIndex kCacheVersion
//
// Also invalidate on a game-build or d4data fingerprint change where the cache depends on either —
// several already do this via a sig/appCount/dadSig guard, which is complementary, not a substitute:
// the fingerprint catches "the DATA changed", the version catches "our INTERPRETATION changed".
