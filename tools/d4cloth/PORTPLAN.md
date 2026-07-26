# M5 — Port plan: corrected solver into the app

_2026-07-25. Port v1 is the CORRECTNESS fix set only — the three mechanisms proven in
VALIDATION.md, each behind a data-presence check that falls back to today's behaviour on
assets that lack the arrays. Feel-changing refinements (authored gravity, per-class
cluster stiffness, driving the body-range Wrath chains) are deliberately deferred to v2
so this diff stays reviewable and testable. Backup: `.Backups/src_20260725_pre-d4cloth-port`._

## Files touched

### 1. `src/model/ModelGeometry.h` — ClothSim gains authored fields
`nRealVerts` (authored vertexCount@252 — existing `vertCount` is CAPACITY@sized-arrays),
`drvInf` (4×u16/vert), `drvW` (4×float/vert), `drvBone` (per driver → piece bone, from
ptDriverMap), `followerBone` (per real vert → piece bone, -1 none), `kinRoots`
(u16/vert). Comments cite tools/d4cloth/FINDINGS.md.

### 2. `src/model/ModelParser.cpp`
- `parseClothCapsules` sim block: parse ptWeights@464, ptDriverInfluences@480,
  ptKinematicRoots@432, ptFollowerIndices@496 (real-vert count!), ptDriverMap@704 +
  header boneCount@274/driverCount@276/vertexCount@252. Size-gated exactly; a mismatch
  leaves the field empty (runtime falls back — and `inspect` reports it loudly).
- `mergeGeometries`: remap `followerBone` and `drvBone` through the piece→unified bone
  remap table, exactly like plane `boneIndex`.

### 3. `src/gl/GLModelWidget.cpp` — four call sites
- **buildSpringBones, cage-runtime skinning (~line 1680):** per cage vert, use the
  authored skinning (drvInf→drvBone bones + drvW weights) when fully resolvable;
  else the existing 20 cm render-vert borrow. This kills ROOTCAUSE link 1.
- **buildSpringBones, anchor search (~line 1796):** when any cage carries a follower
  table, drive bones from `followerBone` (driveW=1, per-piece, exact) and skip the 10 cm
  nearest-particle search entirely; the search survives only as the no-follower-data
  fallback. Kills the shared-cage defect and the cross-garment latching.
- **springBoneStep, cage sim:** dynamics run over `[0, nRealVerts)`; constraint pairs
  with an endpoint ≥ nRealVerts (SIMD padding, incl. the (77,77) self-pairs) are skipped.
  The motion limit becomes the authored tether — `|P − pos(kinRoot)| ≤ attachLen ×
  maxDistance-slider` (attachLen is ABSOLUTE wu; slider default 1.0 = authored) — with
  the old target-relative clamp only for verts with no root. `mdScale`/`cageSpan`/
  `tnorm²` go away on this path; the post-substep safety bound becomes a pure
  kDivergeMax net.
- **springBoneStep, bone drive:** a followed bone's head IS its particle
  (`S = rt.pos[kv]`), replacing `S = anim + (pos − target) × aw`. Kills ROOTCAUSE link 4.
- **springBoneStep, bone-path motion limit:** `m_sbAttach × maxDistance` absolute (the
  tnorm² scaling drops; m_sbAttach is an authored absolute length per FINDINGS F1).

## Unchanged (v1)
Collision (capsules/planes/sweep/velocity pass), sub-stepping, spin forces, hair-class
handling, computeAnimMoves, pass-2 swing, all sliders/QSettings keys, the overlay
(`driveW` now reads 1.00 on followed bones), skin-fit capsule fallback, `m_cageSpan`
(still computed; only its consumer changed), attachLen synthesis fallback.

## Behavioural expectations after port (verify in-app)
1. Fur-Lined Robe outfit + idle: no jutting bone on chains 298-300 / 326-328; no bone
   parked at a fixed radius. `cloth-overlay` longest lines drop from ~0.30+ to ≤ ~0.15.
2. Fur-Lined Hood + stor161: `drawn` returns to ~146 (not 292); no byte-identical
   duplicate bone positions; hood bones no longer follow the cape.
3. barF_stor151 fur unchanged-or-better (its own followers drive it).
4. `cloth-build` gains a line: authored-skinned cage verts + followed bone counts.
5. Models-tab standalone pieces: unchanged path (no split → cage-claimed), now with
   followers when authored.

## Risks & rollback
- Assets without follower tables (older/odd cloth): per-cage fallback keeps today's
  behaviour; `D4_DUMP_CLOTH=1` prints which path each cage took.
- Rollback = restore `.Backups/src_20260725_pre-d4cloth-port`.
