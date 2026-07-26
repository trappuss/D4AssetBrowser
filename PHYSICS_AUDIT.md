# Cloth Physics — Audit & State

_2026-07-24. Backup of the tree at audit time: `.Backups/src_PHYSICS_FINAL_AUDIT_20260724_233754`_

## Status

Cloth simulation is **stable but not correct**. Nothing diverges, NaNs, or explodes; a
subset of cape bones still rest in visibly wrong positions on many outfits. The remaining
defect is diagnosed but unfixed — see *Open defect*.

---

## Verified fixed (evidence, not assumption)

| Area | Defect | Evidence it is fixed |
|---|---|---|
| Merge | Bones whose parent was authored *after* them were orphaned to `parent = -1`, collapsing world transform to local and dumping the bone at the model origin | Parents now resolved after all bones are mapped; new bones appended parents-first |
| Skinning | `global[j]` treated any bone with `parent > index` as a root | Resolves by readiness; cycles broken explicitly |
| Divergence | Emergency clamp was `m_radius * 1.5` ≈ 80% of body height, and never reset velocity, so bones parked *on* the clamp permanently | `cloth-diverge` went from firing every frame on bone 299/327 to **zero** occurrences |
| Collision | Velocity correction ran inside the solver loop (~60×/frame), reading constraint displacement as velocity | Split via `fixVel`; one velocity pass per path |
| Sweep | Resting contacts read as "inside" and were clamped to 25% motion every frame | Guarded on start-point-outside |
| Overlay | Chain roots drew full-length lines from one shared body bone — 7 cape roots on bone 5, 7 skirt roots on bone 164 — producing two fans that read as "bones flying off" | Drawn as short stubs; user-confirmed improvement |

## Present but never triggered

- **`attachLen` synthesis.** Rebuilds the per-vertex motion constraint when the authored
  array is absent. Logged 0 synthesized on every model tested — all cages were authored.
  Harmless safety net for future/other assets; not a fix for anything observed.
- **Parent-cage preference** in anchoring (`kForeignPenalty`). Cage mismatches measured 0
  both before and after, so its effect is unproven either way. Low risk, retained.

## Tried and reverted

- **Cage provenance filter.** Restricted each bone to cages whose particle→bone span covered
  it. Correct in principle — two garments' cape chains provably shared one cage (bones 329
  and 475, different rigs, both `cage=5 vert=16`, identical positions). **Broke fur on
  `barF_stor151_TRS`**: a cage skinned only to body bones yields a span excluding every
  cloth bone, so legitimate chains failed the filter. Reverted. A retry needs real per-piece
  provenance recorded at merge time, not inferred from skin indices.
- **`capsuleRadius` 0.52 → 1.0.** Wrong; 1.0 splays the chiton open. Reverted, v3 migration
  undoes it.

---

## Root cause, found by backup archaeology

`m_sbDriven`, `m_sbAnchorPiece` and `m_cages` appear in **no backup** — not
`_backups/src_20260712_*`, not `src_20260715_pre-outliner`, not `src_20260716_pre-npanel`.
The entire **cage-driven bone mechanism was introduced on 24 July**. Before that, cape bones
were pure spring-bone simulation. That is the "they were fine before" boundary.

The mechanism anchors each cloth bone to its nearest cage particle and copies that particle's
displacement onto the bone. The tight 3cm "same point" bound was **deliberately widened to
10cm** so cage collision would reach the fabric between bones (the leg-clipping fix). But
driving at 10cm makes a bone inherit motion belonging to a point it is not at.

Correlation across every log captured: **409 of 427** long overlay lines (`len >= 0.30`) are
`driven=1`; 18 are not. It also explains the duplicates — two bones nearest the same particle
receive identical displacement and land at byte-identical positions.

**Fix applied:** the drive is now weighted by match quality (`m_sbAnchorW`). Coincident
matches (≤3cm) drive at full strength; matches at the 10cm edge taper smoothly to 0.25 and
the bone keeps more of its animated pose. Cage influence — and therefore the leg-clipping
fix — is preserved, while distant particles can no longer dominate a bone. The overlay dump
now prints `driveW=` per bone.

**REFUTED by measurement.** Every bone reported `driveW=1.00` — all anchor matches were
coincident (≤3cm), so the taper changes nothing. The widened search radius was not the cause.
The weighting is retained because it is correct in principle and costs nothing, but it is not
a fix. The `driven=1` correlation is real yet explained by the item below, not by match quality.

## Root cause (dimensional) — attachLen used as a world distance

`attachLen` is authored **normalized 0..1** (`ModelGeometry.h`: "0 = locked to skinned pose,
1 = free to swing"). It was consumed directly as a world-space length:

```cpp
const float mdScale = m_cloth.maxDistance * qMax(0.05f, tnorm*tnorm);  // slider, default 1.0
const float md      = al * mdScale;                                     // al is 0..1
```

A fully-free particle was therefore allowed to drift **1.0 world unit** on a character whose
entire radius is 1.168. Particles ran outward until an unrelated clamp caught them — which is
exactly why the measured worst drift (0.330–0.347) equalled `kDivergeMax` (0.327) rather than
any value the cloth data implies, and why every earlier clamp change merely relocated the
parking spot.

**Fix applied:** `mdScale` is now multiplied by `m_cageSpan[sim]` — the cage's world-space
reach from its pinned edge, i.e. the length a normalized attachLen of 1.0 refers to. A
normalized fraction now yields a real length, and a small collar is constrained like a collar
rather than like a full-length cloak.

_Confidence: the units error is objectively a bug and the arithmetic matches the observed
numbers. Whether it is the whole of the visible defect is unverified._

## Open defect

**Two garments can share one cloth cage.** The anchor search picks the nearest cage particle
within 10cm across *all* cages on the character, with nothing checking the cage belongs to
that bone's garment. When two pieces each carry a cape rig they occupy the same space, both
chains latch onto whichever cage is nearer, and the losing chain is dragged by cloth it is
not part of.

Measured on Fur-Lined Hood + `stor161` torso:

```
bone 329 'bone_99a08eea'  pos(-0.324 1.350 0.243)  cage=5 vert=16
bone 475 'bone_80436d2c'  pos(-0.324 1.350 0.243)  cage=5 vert=16
```

`drawn=292` — exactly 2× the 146 of a single-cape outfit. Also doubles simulation cost.

**Why the obvious fix failed:** provenance inferred from skin indices is unreliable (see
above). The durable fix is to record the source piece index per cage and per bone during
`ModelParser::mergeGeometries`, where that information exists and is exact, then require a
bone to anchor only within its own piece's cages. That is a data-plumbing change through
`ModelGeometry`, not a solver change.

**Secondary:** a residual zigzag on some chains — bone out, child back (e.g. 298 → 299 → 300
at x = -0.55, -0.75, -0.57). May be the same root cause; unconfirmed.

---

## Diagnostics

`Debug Cloth Overlay.bat` sets `D4_DUMP_CLOTH=1`, runs the tool, and extracts the report.
All logging is env-gated and costs nothing in normal use.

| Line | Reports |
|---|---|
| `cloth-build` | bone/cage/capsule counts, simulated + pinned totals |
| `cloth-overlay` | 10 longest overlay lines: both endpoints, cage + vert, `CAGE MISMATCH` flag |
| `cloth-diverge` | bones hitting the divergence cap, worst offender, cage-driven vs spring |
| `cloth-collide` | contact count, worst penetration vs margin |

**Reading it:** identical positions on two different bone indices ⇒ shared-cage bug.
`inSim=0` on a long line ⇒ bone not simulated, look at the rig not the solver.
`cloth-diverge` firing ⇒ solver instability. Currently: none.

---

## Process note

This took far more iterations than it should have. Four rounds were spent adjusting solver
clamps while the reported symptom was immune to them — the user repeatedly said "nothing
changed", which was accurate and disconfirming, and should have redirected the search after
the first occurrence rather than the fourth. Progress only began once the overlay was
instrumented to print what it was actually drawing. **For any future work here: instrument
first, hypothesise second.**

---

## 2026-07-25 — Root cause closed via the d4cloth harness (see tools/d4cloth/)

The diagnostic harness (tools/d4cloth: PLAN/FINDINGS/ROOTCAUSE/VALIDATION/PORTPLAN.md)
reproduced the cape defect deterministically and closed it with authored data:

- **attachLen is NOT normalized.** It is the authored ABSOLUTE tether length (wu) to the
  particle's kinematic-root particle (r=1.0000 on 12/12 ClothData blocks). The
  "normalized 0..1" premise above is refuted; the cageSpan fix double-scaled it.
- **Root cause of the jut/zigzag:** the cage-target skinning borrowed from the nearest
  render vert (20cm) crossed garment layers (cape verts skinned to skirt bones), and the
  drive `bone = anim + (pos − target)` added the target error to a correct bone —
  bone 326's jut position predicted to 3 decimals from the measured target error.
- **The game ships the whole driving system** the anchor mechanism reinvented:
  ptDriverInfluences/ptWeights (cage skinning, per piece), ptFollowerIndices
  (particle→bone, ≤5µm rest alignment, never duplicated), ptKinematicRoots +
  ptAttachmentLengths (tethers), ptDriverMap. The shared-cage defect (bones 329/475)
  cannot occur under followers: harness A/B measured 111 shared anchors → 0.
- **Ported 2026-07-25** (backup: .Backups/src_20260725_pre-d4cloth-port): authored cage
  skinning + follower driving + absolute tethers + real particle set (vertexCount vs
  capacity; SIMD padding excluded). All fallback-gated: assets without the arrays keep
  the old paths. Rebuild with build.bat and verify per PORTPLAN.md's expectations list.
