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
