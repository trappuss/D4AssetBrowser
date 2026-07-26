# d4cloth — M1 research findings

_2026-07-25. Produced by `d4cloth inspect` (build 2026-07-25) over the extracted corpus:
game 3.1.1.72903, CASC build bb86be950d3b, 10 pieces, 12 ClothData blocks. Every number
below is printed in `reports/<piece>.txt`; nothing here is inferred from a screenshot._

---

## F1 — `ptAttachmentLengths` semantics: SETTLED (the "normalized 0..1" premise is refuted)

Correlating the authored values against four candidate references on **all 12 blocks**:

```
vs chainSumToRoot        r = 1.0000   mean(attachLen/candidate) = 1.000   (12/12 blocks)
vs distToKinematicRoot   r = 0.9999–1.0000                    = 1.000–1.003
```

**`attachmentLengths[k]` is the summed bind-space length of the particle's chain — along
the authored `ptParentIndices` — from particle k to its `ptKinematicRoots[k]` anchor. It
is an absolute distance in world units, not a normalized fraction.** `ptLevels[k]` is the
chain depth (verified equal to parent-chain depth; header `maxLevel` matches). This is the
classic long-range-attachment / tether model (NvCloth tethers).

It *looked* normalized because a cape is ~1 wu long, so hem tethers land near 1.0.

Consequences for the current app code:
- The consumption `md = attachLen × maxDistance × tnorm² × cageSpan` is wrong twice over:
  the `cageSpan` multiplication double-scales an already-absolute length, and the clamp is
  applied **around the particle's skinned pose**, while the authored constraint is a rope
  limit **around the kinematic root particle's current position**. Under animation those
  references diverge substantially — the correct constraint follows the collar, not the
  per-particle skinned ghost.
- The correct solve step: `|P_k − P_root(k)| ≤ attachLen[k]` (project back along the
  vector to the root when exceeded), optionally scaled by NvCloth-style
  tetherScale (nothing in the .clt.json overrides it; default 1.0).

## F2 — the authored driving system exists and is complete (H2 confirmed)

The mechanism the app reinvented with a 10 cm nearest-particle anchor search ships in the
data, per piece:

- **`ptFollowerIndices`** (`vertexCount` entries): for each real cage particle, the
  garment-skeleton bone that FOLLOWS it, or 0xFFFF. Measured across the matrix:
  follower-bone rest position ↔ cage vert bind distance **mean 0.6–2.5 µm (capes/hair/
  skirts) and ≤1.7 mm (body chains)**, `>3cm = 0` everywhere, and **no bone is followed by
  two particles** in any block. Pinned particles have no follower; some free fabric verts
  (fur: 31 of 56) have none — they exist for collision/shape only.
  - This kills both open defects at the root: per-piece provenance is *authored* (a bone
    can only be driven by its own piece's ClothData block), and the bone↔particle map is
    exact — no search radius, no shared anchors, no `driveW` heuristics.
  - Indices are into the **piece's own skeleton** — the merge must remap them by bone hash
    exactly like vertex joints (`mergeGeometries` already has the remap table).
- **`ptDriverInfluences`** (4×u16 per vert) + **`ptWeights`** (vec4 per vert, row sums
  exactly 1.0): authored skinning of cage particles to `driverCount` driver frames —
  replacing the "borrow skinning from nearest render vert within 20 cm" heuristic.
  Influence indices stay < driverCount everywhere.
- **`ptDriverBindPose`** (48 B q/p/s frames × driverCount) + **`ptDriverMap`**
  (`boneCount` entries, ~driverCount distinct values, rest 0xFFFF): binds each driver
  frame to a specific bone of the piece rig (bone → driver index). Drivers are frames
  *offset from* bones (only ~1 of 10 sits on a bone origin), posed each frame as
  `animatedBone × (restBone⁻¹ × driverBindPose)`. One decode pass remains: confirming
  which bone table the 178 entries index (likely the piece skeleton prefix) — flagged for
  M2 with a concrete test (pose drivers from candidate tables, check which reproduces the
  kinematic particles' skinned drape).

## F3 — vertexCount vs vertexCapacity, and SIMD padding (a live app bug)

Per-vert arrays are sized by **`vertexCapacity`**, not `vertexCount` (cape: 77 real, 80
allocated). The extra particles sit at bind (0,0,0) with invMass 1. Padding constraints
are **self-pairs** on the first extra particle (cape: 55 pairs (77,77), rest length 1.0)
so cluster ranges stay multiples of 8 — SIMD batch padding.

The app derives `vertCount = bindVerts.size()/16 = 80` and simulates the phantom
particles: harmless in the constraint loop (self-pairs hit the `len < 1e-6` guard) but
live in every other pass — they collide, they are skinned by "nearest render vert to the
origin", and **they are legal targets of the 10 cm bone-anchor search** for any cloth bone
whose rest sits within 10 cm of the model origin. The fix: real particle set is
`[0, vertexCount)`; drop constraints touching `≥ vertexCount`.

Also: the app's `attachLen` size gate `alS/4 == nCage` only ever *worked by accident* —
both sides are capacity-sized. The report prints both counts explicitly now.

## F4 — constraint classes: cluster partition CONFIRMED (H3)

`ptWarp/Weft/Shear/BendClusters` partition `[0..constraintCount)` exactly on 12/12 blocks
(`sum(cluster spans) = constraintCount`), with SIMD-friendly range sizes. Rest-length
statistics differ per class as fabric geometry predicts. The `.clt.json` ships per-class
stiffnesses the solver must apply per range: `flStretchingStiffness` /
`flHorizontalStiffness` / `flShearStiffness` / `flBendingStiffness` (warp/weft assignment
to stretching/horizontal to be pinned down in M2 by edge-direction statistics), plus
`nIterations`, damping/drag/lift, `vGravity` (z = −22 on the barF cape), and
`flAttachmentStiffness` (likely the tether-solve stiffness).

## F5 — corpus facts that reframe the defects

- **The base00 body itself carries 5 ClothData blocks** (`BarF_base00_LEG`,
  `barF_Wrath_chainFront/Back`, `barF_Wrath_side_LeftA/RightA`). 5 (body) + 1 (cape) +
  2 (skirt) = **8 cages — exactly the in-app measurement** on the Fur-Lined Robe outfit.
- **`barF_base03_HLM` (Fur-Lined Hood), GLV and BTS contain NO ClothData.** The hood's
  extra bones have no cage and no followers — the game does not cage-drive them. Under
  the app's current mechanism they can *only* latch onto other garments' cages, which is
  precisely the measured two-garment defect (bones 329/475 on cage 5 vert 16). Authored
  behaviour: pieces without a ClothData block are never cage-driven.
- Unit system: consistent with 1 wu ≈ 1 m (constraint rests 0.09–0.35 wu on a cape,
  hem tethers ≈ 1.0 wu, capsule radii 0.03–0.15 wu).

## F6 — `ptDeltaFrames` row layout: DECODED (M1.1); frame roll still open

Brief (DELTAFRAMES.md) says "add as F4", but F4/F5 are taken — filed as F6.
Measured on `barF_base03_TRS_cape` (vertexCount 77, capacity 80), raw bytes, no harness rebuild.

**Settled — each row is a pure rotation, one per CAGE vert:**

| Property | Measurement |
|---|---|
| Size | capacity x 64 B (80 rows); rows `[vertexCount, capacity)` are **identity** |
| Translation | none — col 3 of rows 0-2 is exactly 0, row 3 is `(0,0,0,1)` |
| Orthonormal | worst \|dot-delta\| = **3.04e-07** across all 77 |
| Determinant | **+1.000000** for all 77 (proper rotation, no reflection) |
| Handedness | **Y = Z x X**, worst component error **1.78e-07** (only X and Z are independent) |
| **X axis** | **direction parent -> vertex**: mean `1-\|cos\|` = **0.00156**, worst 0.0274, n=63 (verts with a valid parent; `sign(X . (parent-vert)) = -0.998`) |

**Refuted by measurement (do not retry):**

- `ptBindNormals` as any axis — mean `1-\|cos\|` 0.306 (Z), 0.316 (Y).
- Cage **triangle normal** (area-weighted, from `ptTriangles`) — 0.306/0.315, i.e. identical to
  bindNormals, which it should be; both are simply not the frame's second axis.
- **Tangent-partner direction** (`ptTangentIndices`) — 0.269 (Z) / 0.341 (Y), before and after
  Gram-Schmidt against X.
- **World-axis up-reference** (Z = normalise(up - (up.X)X)). The apparent 0.081 fit against worldZ
  is an ARTEFACT: `\|X . worldZ\| >= 0.9` for **all 77** verts (the cape hangs vertically, so the
  chain direction is near-parallel to world Z everywhere) — the projection is degenerate for every
  vertex, so the statistic is meaningless. worldX/worldY: 0.295-0.327.

**Open — the roll about X.** No cage-geometric quantity tested accounts for it. The array name
("delta") suggests the rotation is relative to another frame rather than absolute, which is
consistent with X being absolute-looking while the roll is not.

**`ptDriverBindPose` — DECODED** (was the blocker): 48 B per driver = three float4s,
`position (x,y,z,0) | quaternion (x,y,z,w) | scale (1,1,1,1)`. All 10 quats unit to 1e-7, every
`pos.w` exactly 0, every scale exactly 1. Driver 0 is the null/root driver (zero pos, identity
quat). Positions read as z-up bind locations (driver 1 z=1.17 = chest height).

**Roll about X — five hypothesis families refuted, all with measured residuals:**

| Candidate | mean `1-\|cos\|` | verdict |
|---|---|---|
| `ptBindNormals` | 0.306 | refuted |
| Cage triangle normal (area-weighted) | 0.306 | refuted (== bindNormals, as expected) |
| Tangent partner (`ptTangentIndices`) | 0.269 | refuted |
| World-axis up-reference | degenerate | refuted — `\|X . worldZ\| >= 0.9` for ALL 77 verts |
| Warp / Weft / Shear / Bend constraint directions | 0.282-0.343 | refuted (no class better than chance) |
| Driver bind rotation (`Q`, `Q*R`, `R*Q`, `Qt*R`, `R*Qt`) | >= 77 deg on every composition | refuted (correct layout) |

**Conclusion: the roll is not derivable from the cage data.** X is fixed by the chain
(parent -> vertex, residual 0.0016) and `Y = Z x X`, but the roll about X matches no cage-geometric
quantity available in the ClothData block. The most consistent reading is that it is **authored**
— carried over from the source garment's tangent space (UV-derived) in the DCC export, which the
cage does not retain.

**Design consequence (M2):** do not attempt to reconstruct these frames — *use them*. They are
per-cage-vert authored rotations to be posed by the runtime and applied to render verts. That is
also why the app can ignore them today without visible error in the cage sim itself: they carry
render-binding information, not simulation state.

**M1.2 is blocked on inputs, not analysis.** Reconstructing SIM-submesh render verts needs the
RENDER mesh (positions + skin weights), which the harness does not parse — `tools/d4cloth` reads
ClothData only; render geometry lives in the appearance payload and is parsed app-side
(`ModelGeometry`/`MeshPrimitive`). Options: (a) teach the harness to read render submeshes, or
(b) dump the cape's `_sim` submesh from the app once and validate against that dump offline.

## F7 — the `_sim` submesh IS the cage; render/cage share one space (M1.2)

Measured on `barF_base03_TRS` against the corpus bytes (appearance meta+payload), mirroring
`ModelParser`'s vertex-buffer scan.

- The appearance has **2 vertex buffers**, both stride 44 (skinned): VB@6976 (4675 verts) and
  VB@7056 (10124 verts). Submeshes index ranges inside them; there is no per-submesh VB.
- **VB@6976 contains a render vertex coincident with every one of the 77 cage verts — at
  0.000 mm (exact, all 77/77).** VB@7056 does not (21/77 within 20 mm, median 16.7 mm) — it is a
  different part/LOD.
- Cage `triangleCount` = **120** = the tri count the app's Parts panel reports for
  `barF_base03_TRS_cape_sim`. Same vert set, same tri count: **the `_sim` submesh is the cage.**
- **Coordinate space: identical.** Cage `ptBindVertices` are byte-identical to render positions —
  both **z-up, no axis conversion, no scale** at rest. (`zUpToYUp` is a display-time transform,
  not a data-space difference.)

**M1.2 validation target is met, and is degenerate:** reconstructing `_sim` render verts at rest
from the cage is exact (0.000 mm) because they *are* the cage verts. The `_sim` submesh needs no
binding — it is the simulation mesh drawn directly. This also explains why hiding `[SIM]` parts
matters visually and why they were never the source of the cape defects.

**What actually needs binding (M2):** the VISIBLE garment submeshes (`barF_base03_TRS_mat` 2615
tris, `barM_base03_TRS_fur_mat` 2924), which are *not* coincident with the cage. No stored
render->cage binding array exists — all 27 ClothData slots are accounted for — so the binding must
be **computed at load** (nearest cage triangle + barycentric coords + offset along the normal),
with `ptDeltaFrames` supplying the per-particle rotation for transporting normals/tangents.

## F8 — `definitions.json` is the authoritative schema (tooling unlock + 2 self-corrections)

`d4data/definitions.json` (mounted locally; github.com/DiabloTools/d4data) contains the FULL type
graph: every field carries `[arrayType, elemType, tail]` hashes, and each hash resolves to a named
struct with field names, offsets and sizes. Reverse-engineering layouts by inspection is no longer
necessary — look them up.

**`ClothData` (hash 2666466548, size 720)** — all 27 slots confirmed, element types named:

| slot | element type |
|---|---|
| ptBindVertices / ptBindNormals / ptWeights | `DT_VECTOR4D` |
| ptDeltaFrames | **`dmMtxMirror`** (64 B) |
| ptDriverBindPose | **`dmFrameMirror`** (48 B) |
| ptWarp/Weft/Shear/BendClusters | **`dmConstraintClusterMirror`** (4 B) |
| ptCapsuleDefs | **`dmClothCapsuleDefMirror`** (80 B) |
| ptPlaneDefs | **`dmClothPlaneDefMirror`** (48 B) |

**`dmMtxMirror` = `rx`@0, `ry`@16, `rz`@32, `p`@48.** Confirms F6's measured layout and names it:
rows 0-2 ARE the basis vectors, and the 4th vec4 is a **position** (not a homogeneous row). On the
barF cape `p` is `(0,0,0,1)` — zero translation — but it is a real field and may be non-zero
elsewhere.

### Correction 1 — cluster fields are `(startIndex, endIndex)`, NOT `(offset, count)`

`dmConstraintClusterMirror = {startIndex@0, endIndex@2}`. F6's constraint-direction test indexed
them as `(offset, count)` and therefore scanned garbage ranges; that result was **INVALID, not a
refutation**. Re-run with correct semantics: ranges are contiguous and partition `[0,320)` exactly
(Shear 0-72, Bend 72-176, Weft 176-248, Warp 248-320), independently re-confirming F4's partition
claim. The roll test still fails (0.276-0.369 across all four classes) — so the refutation stands,
now on valid data.

### Correction 2 — serialized order is `position, quaternion`, NOT the declared `q, p`

`dmFrameMirror` declares `q`@0, `p`@16, `s`@32. **The data says otherwise**, consistently across
three independent structs:

- `ptDriverBindPose`: vec4#1 has `w == 0` exactly on all 10 drivers and reads as a bind location
  (z = 1.17 = chest height); vec4#2 is **unit to 1e-7 on all 10**. Position first.
- `ptCapsuleDefs.localTransform`: vec4#1 `w == 0`; vec4#2 contains 0.707 / 0.5 components
  (half-angle quaternions). Position first.
- `ptPlaneDefs.localTransform`: vec4#1 `w == 0`; vec4#2 is exactly `(0,0,0,1)` = identity quat.

Three structs agree, so the measurement wins over the declared field order. Treat `definitions.json`
as authoritative for **names, sizes and offsets**, but verify component order against data.

### Capsules — the app ignores authored fields and substitutes a constant

`dmClothCapsuleDefMirror = localTransform@0(32) | scale@32(vec4) | radius1@48 | radius2@52 |
height@56 | friction@60 | boneIndex@64 | solver@66 | hide@67`.

On the barF cape (7 capsules): **`scale` = (1,1,1,1)** on every one, **`radius1 == radius2`** (no
taper: 0.160/0.220/0.198/0.270), `friction` = 0.10 authored, `solver` = 2, **`hide` = 0**.

The app multiplies every radius by a hardcoded **0.52**. The authored data carries a real `scale`
field (unused) and a `hide` flag (unused, and the obvious mechanism for capsules that should not
collide at all). A global constant standing in for per-capsule authored data contradicts the
"no per-model constants" guardrail and is the leading suspect for residual clipping. **Not changed
here** — 0.52 is visually verified and reverting it blind previously caused a regression.

### Planes — struct decoded; every corpus plane is identical in form

`dmClothPlaneDefMirror = localTransform@0(32) | stiffness@32 | friction@36 | boneIndex@40 |
padding@42`. Measured over every plane in the corpus (druF_stor249 cloth+tabard, spiF_stor210 b+c,
spiF_stor211 loin+skirt+cape): **rotation is the identity quaternion `(0,0,0,1)` in all cases**,
position is `(0, 0, d, 0)` with d = 0.034-0.070, `stiffness` = 1.000, `friction` = 0.100,
`boneIndex` = 0 — uniformly. So a plane is a fixed offset along one local axis of bone 0 with no
rotation; the remaining unknown is only WHICH axis is the normal and what bone 0 is in that space.
This is strictly more than was known when planes were disabled.

## F9 — M2's premise is UNFOUNDED: render verts are bone-skinned, not cage-skinned

Measured on `barF_base03_TRS` with a full LOD0 submesh parse (mirrors `ModelParser`'s
VB/segment/sub-object scan). LOD0 submeshes match the app's Parts panel exactly:
mat0 = 77 verts/120 tris (the cage), mat1 = 1860 tris (armor_skin), mat2 = 2924 (fur),
mat3 = 2615 (torso).

**Distance from each visible submesh to the cage surface (point-to-triangle, mm):**

| submesh | verts | min | p5 | p25 | median | max | <1 mm |
|---|---|---|---|---|---|---|---|
| mat0 (cage/`_sim`) | 77 | 0.00 | 0.00 | 0.00 | **0.00** | 0.00 | 77/77 |
| mat1 (armor_skin) | 1079 | 77.5 | 90.3 | 135.4 | 175.7 | 312.1 | 0 |
| mat2 (fur) | 1797 | **0.26** | 8.8 | 43.0 | 79.1 | 319.4 | 11 |
| mat3 (torso) | 1722 | 54.3 | 85.2 | 168.6 | 265.3 | 436.9 | 0 |

Only the **fur** (mat2) is cage-adjacent, and only partly: 225/1797 verts within 20 mm, 529 within
50 mm — a continuum with no natural cutoff. mat1 and mat3 never come within 54 mm.

**Decisive: the near-cage render verts carry BONE skin weights.** Reading `BLENDINDICES`/
`BLENDWEIGHTS` through the segment bone palette (66 entries, globals 4-250), verts within 50 mm of
the cage reference global bones 5, 104, 134 and a dense run of **191-250** (the high-index cloth
bones). There is **no bone used exclusively by near-cage verts** — the same bones drive near and
far geometry.

**Therefore the game deforms this cloth the way the app already does: cage sim -> cloth bones ->
ordinary skinning.** Supporting evidence, all from M1:

1. No authored render->cage binding exists — all 27 `ClothData` slots are named via
   `definitions.json` (F8) and none maps render verts.
2. `ptDeltaFrames` is per-CAGE-vert and a pure rotation (F6) — consistent with normal/tangent
   transport for the cage/`_sim` mesh, not a render binding.
3. The `_sim` submesh IS the cage (F7), so the only geometry with an exact cage relationship is
   the simulation mesh itself.

**Recommendation: do not build M2 as briefed.** Any cage->render binding would have to be a
proximity heuristic over that 0.26-319 mm continuum — precisely the class of distance-threshold
heuristic that produced the shared-cage and cross-garment defects already documented. The authored
mechanism (driver skinning + follower bone drive) is already ported per PORTPLAN.md and is present
in the app (`followerBone`/`drvInf`/`drvBone`/`nRealVerts`/`kinRoots` in ModelGeometry.h,
ModelParser.cpp, GLModelWidget.cpp).

**Where the remaining quality gap actually is:** collision, not skinning topology. The leading
concrete lead is the capsule one from F8 — authored `scale`, `hide`, per-capsule `radius1/2`,
`height` and `friction` exist and the app substitutes a hardcoded 0.52 radius multiplier for all
of it.

## F10 — capsule lead: mostly REFUTED, but `m_colAuthored` is dead code (a real defect)

Corpus sweep, **143 authored capsules** across all 21 pieces:

| authored field | measured | verdict |
|---|---|---|
| `hide` | **0 of 143 set** | ignoring it costs nothing — refuted |
| `scale` | **0 of 143 non-unit** (all 1,1,1,1) | ignoring it costs nothing — refuted |
| `solver` | **2 on all 143** (constant) | nothing to honour — refuted |
| `friction` | **0.10 on all 143**; app defaults to 0.1f | already matches — refuted |
| `radius1 != radius2` | **39 of 143 tapered (27%)** | app DOES interpolate r0->r1 along the axis — already correct |
| `radius1` | 70-331 mm (median 137) | **see below** |

So my F8 framing ("the app ignores authored capsule fields") is **wrong** for everything except
radius — the other fields carry no information in this corpus. Radius is the only authored quantity
that varies, which narrows the lead rather than killing it.

**The real defect: `m_colAuthored` is set and logged, but never read.** It exists specifically to
stop the Capsule-size slider scaling the game's exact radii — its own comment says *"a 0.55x
default was silently shrinking the thigh capsules to half size, letting skirts clip into the
legs"*, and `GLModelWidget.h:495` repeats the intent (*"capsuleRadius slider only scales SKIN-FIT
ones"*). But `springBoneStep` computed `rScale = m_cloth.capsuleRadius` unconditionally, so with
the 0.52 default **every authored capsule has been colliding at 52% of its authored size** — 70-331
mm applied as 36-172 mm. The documented fix was never wired.

**Change made (opt-in, default unchanged):** `rScale` (and the Collision-model overlay, which must
mirror it) now honour the flag when `D4_CAPS_FULL=1`. Default behaviour is byte-identical to
before, because 0.52 is visually verified on the current build and a blind flip regressed
previously. `D4_DUMP_CLOTH=1` prints `cloth-caps:` with authored vs APPLIED radii so the
discrepancy is visible rather than inferred.

**To evaluate:** run with `D4_CAPS_FULL=1` on a skirt/leg repro (spiF_stor210/211_LEG,
DruF_stor249_LEG) and compare clipping against the default. Note this interacts with the
Capsule-size slider's meaning: with the flag on, that slider governs only skin-fit capsules, as
originally intended.

## F11 — capsule ORIENTATION is the leading defect (**premise CORRECTED — see warning**)

> **WARNING — this section's original framing was INVALID.** It was written as "the result of
> enabling `D4_CAPS_FULL=1`", but `D4_CAPS_FULL` is an ENVIRONMENT VARIABLE and there is no
> evidence it was set when the observations were made. With it unset, the F10 change is a no-op
> (`rScale` is byte-identical to before), so the observations below are the **BASELINE** symptoms
> of the shipping default (authored capsules at 0.52x, capsule axis = bone-dir), **not** the effect
> of full-size capsules. Re-test with `Test Capsules Full.bat` (which sets the variable and prints
> `cloth-caps: ... APPLIED x1.00` so the setting can be confirmed live) before drawing size
> conclusions. The orientation argument below stands on the CODE (`m_capAxis` default), which is
> independent of the test.

Observed on two skirt repros (baseline build, default settings):

- `spiF_stor211_LEG`: **completely rigid** — "as if it has no physics bones". Consistent with
  particles being engulfed by oversized capsules: once every particle is inside a collider, the
  positional solve pins them all to the surface and nothing can move.
- `spiF_stor210_LEG`: **still clipping badly**, physics otherwise fine.

**What can and cannot be concluded.** If the observations are baseline (variable unset), they say
only that the shipping default clips on 210 and is over-constrained on 211 — a useful baseline, but
NOT a refutation of the size hypothesis. If the variable WAS set, then correctly placed capsules
getting bigger must reduce clipping, and the observed rigid/still-clipping pair would refute
size-alone and implicate orientation. **Determine which by re-running with the .bat and checking
the `cloth-caps:` line.**

**Root cause candidate (code, not inference):** `m_capAxis` defaults to **3 = "bone-dir"**
(`GLModelWidget.h:418`; the tabs default `cloth/capAxis` to 3). The authored quaternion IS parsed
and axis-swapped, then **overridden** — the capsule's long axis is taken from the bone direction
instead of from the authored rotation. Axis 0/1/2 (X/Y/Z of the authored quat) exist but are not
the default, i.e. the convention was never pinned down. That is exactly the kind of standing guess
the 0.52 multiplier would have been tuned to compensate for.

**Recommendation:** leave `D4_CAPS_FULL` OFF (it is off by default) until the axis is resolved —
full-size radii only help once the orientation is right. Resolve the axis FIRST, then re-test size.

**Cheapest decisive experiment (no code change):** the Physics panel already has a capsule-axis
cycle button. With `D4_CAPS_FULL=1`, try axis X / Y / Z / bone-dir on both assets and record which
combination stops 210 clipping WITHOUT making 211 rigid. Four combinations, two assets, existing UI.

**Offline alternative (stronger, needs work):** at the bind pose the authored cloth should not be
inside the authored capsules. Transform each capsule into cage space for each candidate axis and
count particle interpenetrations — the correct axis minimises them. Blocked on skeleton parsing in
the harness (bone rest transforms), which `tools/d4cloth` does not yet do.

## F12 — ROOT CAUSE of "rigid, not flowing": cage-less cloth bones got hair treatment

User report: most skirts deform around colliders instead of flowing. Diagnosed from the shipping
build's own logs — no new instrumentation needed.

**Measured** (`cloth_cage_diag.txt`, barF_base08_LEG + barF_base00 body cloth):

```
nb=519  baseBones=190  physBones=329  cages=7  cageDriven=207
```

**122 of 329 cloth bones (37%) are cage-less.** `authoredNone` across the seven FOLLOW lines totals
88 — the authored follower tables themselves assign no bone for those verts. The `cloth-orphan`
lines show their simulated position sitting **d = 2-45 mm** from the animated pose: frozen, with
`moves=0` (so the animation is not what holds them).

**Mechanism (three compounding clamps, all keyed on `si < 0` = "no cage"):**

| site | effect on a cage-less bone |
|---|---|
| `hairBone` | `stiff >= 0.55` (snap-back), `keep <= 0.45` (heavy damping), `gravity x 0.35` |
| `hairTight` | tether x **0.10** |
| `noCage` | tether x `tnorm^2` (a further ~0.07-1.0) |

Net leash ~7-100 mm around the skinned pose. That is the "rigid but deforming around colliders"
look exactly: no chain dynamics, only collision displacement.

**Why it was there:** a cage-less bone keeps the synthetic `m_sbAttach` default of **1.0**, which
under absolute-tether semantics (F1) is a ~1 wu leash — they dangled limply through walking legs
(spiF_stor214). The clamp was the fix for that. Both ends are wrong: 1 wu = loose, 0.007 wu = rigid.

**Fix:** a tether is a LENGTH, so derive it from the rig — the rest-pose distance from the bone to
its chain root (first non-cloth ancestor). A hem bone 40 cm down its chain gets a 40 cm tether; a
bone 2 cm from its anchor gets 2 cm. Measured per bone, no per-model constants. The three clamps
are then legacy-only (`D4_CLOTH_LEGACY_ORPHANS=1` restores them). **Genuine hair (`m_sbHair`) keeps
its tight treatment** — it is a separate flag and is untouched.

**Verify:** `cloth-orphan` d values should rise from 2-45 mm to tens of mm on hem bones (proportional
to chain depth), and skirts should swing. `cloth-diverge` should NOT increase materially; if it
does, the geometric tether is too generous and should be scaled by the max-distance slider.

## Remaining unknowns (decoded shape, purpose pending — none block the solver)

- `ptDeltaFrames`: one 4×4 matrix per vert (pure rotation + identity row) — per-particle
  bind frame, presumably for driving render-mesh normals/tangents (with
  `ptTangentIndices`: per-vert partner index in `[0..vertexCount)`).
- `unk_8ecbb2b` (u16 triples) + `unk_9f71907` (f32 triples), count = header
  `unk_9460e91` (108 on the cape): candidate authored bending elements (`eBendModel`).
- `ptBindNormals`, `ptBlendWeights` (0..1), `ptAnimBlendFractions` (0..0.8): per-vert;
  the latter two pair with `flAnimBlendFraction`/blend machinery in the tuning.

## Design consequences for the corrected solver (M2+)

1. Real particles = `[0, vertexCount)`; exclude padding constraints and phantom verts.
2. Kinematic targets from authored cage skinning (driver frames via `driverMap` +
   `driverBindPose`, weights from `ptWeights`/`ptDriverInfluences`) — no render-vert
   borrowing.
3. Distance constraints solved per cluster class with the class's authored stiffness,
   authored `nIterations`.
4. Motion limit = tether: `|P_k − P_root(k)| ≤ attachLen[k]`, root = the kinematic root
   particle's CURRENT position. No cageSpan, no tnorm², no skinned-pose reference.
5. Bone driving via `ptFollowerIndices` (hash-remapped at merge). Bones without a
   follower entry are never cage-driven; pieces without ClothData are never cage-driven.
6. Collision: authored capsules with per-capsule `friction`, honouring `hide`; planes
   from `ptPlaneDefs` where present.
7. `m_sbDriven`/`m_sbAnchorPiece`/anchor-search/`driveW` machinery becomes dead code at
   port time.
