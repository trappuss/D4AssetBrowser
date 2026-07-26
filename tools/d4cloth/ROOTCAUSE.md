# Root cause of the cape defect — harness-backed account

_2026-07-25. Every number below is from d4cloth output on the extracted corpus
(game 3.1.1.72903). Reproduction commands at the bottom. This closes PHYSICS_AUDIT.md's
"Open defect" section with measured causality._

## Reproduction

`d4cloth run --case cape-outfit --scenario anim` (the `barF_2HM_nav_idle` idle, 60 Hz
stepping, app-default parameters with game tuning — the exact live configuration: the
INI matches ClothParams defaults). LegacySolver is a line-for-line port of the app's
current `springBoneStep`/`buildSpringBones`/`buildClothSim`.

Harness state matches every in-app measurement first:
cages=8, capsules=25, simBones=146, driveW=1.00 on all 144 driven bones,
anchor pairs bone 298→cage5/55, 299→62, 327→68 (the audit's verified +7 stride),
divergence cap 0.338 (radius 1.2076 × 0.28; app measured 0.327–0.338).

At steady state under the idle clip (step 590):

```
worst drifting bones [wu from animated pose]:
  bone 372  0.328    ← parked at ~the divergence cap — the audit's 0.327 signature
  bone 299  0.297    ← the audit's secondary zigzag chain (298→299→300)
  bone 326  0.274    ← the audit's known-bad chain (326→327→328)
  bone 317  0.260
chain 326→327→328 drift: 0.272 / 0.049 / 0.036  → one bone out, chain returns (the jut)
cage motion-limit hits: ~23 particles per step, every step (clamp as mechanism)
```

## The causal chain (each link measured)

**1. The cage-target skinning is borrowed across garments.** `buildSpringBones` gives
each cage particle the skinning of the nearest cloth-skinned RENDER vert within 20 cm.
On a layered outfit the garments interpenetrate in bind pose, so the search crosses
pieces. Probe of cage 5 (the cape, `--probe-cage 5`):

```
v55 bind(-0.338 0.873 -0.124) → bones 298[c](0.69) 297[c](0.27) ...   cape bones — correct
v60 bind(-0.338 0.757  0.126) → bones 437[c](0.50) 438[c](0.47)      FOREIGN piece
v61 bind(-0.338 0.757  0.000) → bones 371[c](0.59) 372[c](0.40)      FOREIGN piece (skirt)
v62 bind(-0.338 0.757 -0.124) → bones 386[c](0.50) 387[c](0.46)      FOREIGN piece
v68 bind(-0.338 0.641  0.000) → bones 326[c](0.52) 327[c](0.48)      cape bones — correct
```

**2. Under animation the foreign skinning puts the target in the wrong place.** At step
590, cage 5 targets down the mid-cape column are non-monotonic — the foreign-skinned
vert's target sits at thigh height between two correct neighbours:

```
v55 target y=0.897   (cape bones   — plausible)
v61 target y=0.597   (skirt bones  — ~0.27 wu wrong, thigh height)
v68 target y=0.703   (cape bones   — plausible)
```

**3. The particle itself stays right; the TARGET is the outlier.** The authored
constraint network holds the sheet: v61 pos (-0.597, 0.803, -0.053) — smoothly between
v55 (0.905) and v68 (0.710). drift(v61) = |pos − target| = 0.274.

**4. The bone drive converts target error into bone displacement.** The drive is
`bone = animBone + (particlePos − particleTarget) × driveW`, driveW = 1.00. With a
correct particle and a garbage target this ADDS the target error to a correct bone:

```
bone326 = animG326 + (pos61 − tgt61)
        = (-0.649, 0.802) + (-0.180, +0.206)
        = (-0.829, 1.005)      ← measured bone 326 position. The jut, to 3 decimals.
```

Neighbouring bones 327/328 anchor to correctly-skinned verts (v68/v75) and stay at
drift 0.05/0.04 — hence one bone out while parent and child track: the zigzag.

**5. Bone 372 parked at the cap is the same bug from the other side.** 372 is one of
the foreign (skirt-chain) bones; its own anchor particle carries a cross-garment target
error large enough that the drive output runs to the divergence cap and rests there
(0.328 ≈ 0.97 × cap, sustained every step) — the audit's "sat at the divergence cap
every frame for hours".

## Why earlier fixes couldn't work

- **driveW taper** (refuted in the audit): the anchor distances ARE coincident
  (driveW=1.00 everywhere, confirmed in-harness) — the anchor is fine; the *target* is
  wrong. Tapering the drive scales the error, it doesn't remove it.
- **attachLen scaling (cageSpan fix)**: the per-particle motion limit on the affected
  verts computes to md = 0.96–1.26 wu — larger than any observed drift, so it never
  engages. (And per FINDINGS.md F1 the authored value is an absolute tether length to
  the kinematic root — the current formula's reference point and scale are both wrong,
  which is why md exceeds 1 wu.)
- **Clamp adjustments**: the clamps only decide WHERE the error parks, not whether it
  exists — matching the audit's "every earlier clamp change merely relocated the
  parking spot".

## The authored fix (FINDINGS.md F2 — implemented next as the corrected solver)

1. Cage-particle targets from the authored cage skinning — `ptDriverInfluences` (4×u16)
   + `ptWeights` (row sums = 1.0) against the piece's driver frames (`ptDriverBindPose`
   posed via `ptDriverMap`) — per-piece by construction; a cape target can never come
   from a skirt bone. This removes link 1, which removes the whole chain.
2. Bone driving from `ptFollowerIndices` (rest alignment ≤ 5 µm, no duplicates, strictly
   per-piece) instead of the nearest-particle anchor search.
3. Motion limit as the authored tether: `|P_k − P_root(k)| ≤ attachmentLengths[k]`
   (absolute wu, kinematic root's current position as reference).
4. Real particle set `[0, vertexCount)`; padding self-pair constraints dropped.

## Reproduction commands

```
d4cloth run --corpus corpus --case cape-outfit --scenario anim --steps 600 \
    --out out --dump bones,particles --report-bones 326,327,328
d4cloth run --corpus corpus --case cape-outfit --scenario anim --steps 5 --probe-cage 5
d4cloth run --corpus corpus --case cape-outfit --scenario rest --steps 300      # control: quiescent
```
