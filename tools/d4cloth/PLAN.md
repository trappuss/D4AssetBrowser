# d4cloth — Cloth Physics Diagnostic Harness: Plan

_2026-07-25. Written before any solver code, per the working method. Sources read:
`PHYSICS_AUDIT.md`, `buildSpringBones` / `springBoneStep` / `buildClothSim` / `buildPhysBones`
in `GLModelWidget.cpp`, the `ClothData` parser in `ModelParser.cpp`, `mergeGeometries`,
`ModelGeometry.h`, `CascReader.h`, `barF_base03_TRS_cape_sim.clt.json`, and — new —
`d4data/definitions.json` (the game client's own type definitions)._

---

## 0. New findings that shape the design (from this planning pass)

`definitions.json` in the d4data snapshot carries the **complete authored layout of
`ClothData` (720 B)** — the 288-byte `dmClothDataMirror` header field-by-field plus all
**27 named variable arrays**. The current parser reads 9 of them. The full list, with
parse status today:

| Off | Field | Elem type | Parsed today? |
|---|---|---|---|
| 288 | `ptBindVertices` | vec4 | yes |
| 304 | `ptBindNormals` | vec4 | **no** |
| 320 | `ptInvMasses` | f32 | yes |
| 336 | `ptBlendWeights` | f32 | **no** |
| 352 | `ptAnimBlendFractions` | f32 | **no** |
| 368 | `ptDeltaFrames` | ? | **no** |
| 384 | `ptLevels` | u16 | **no** |
| 400 | `ptAttachmentLengths` | f32 | partial (silently empty on size mismatch) |
| 416 | `ptParentIndices` | u16 | **no** |
| 432 | `ptKinematicRoots` | u16 | **no** |
| 448 | `ptTangentIndices` | u16 | **no** |
| 464 | `ptWeights` | vec4 | **no** |
| 480 | `ptDriverInfluences` | u16 | **no** |
| 496 | `ptFollowerIndices` | u16 | **no** |
| 512 | `ptTriangles` | u16 | yes |
| 528 | `ptConstraintIndices` | u16 | yes |
| 544 | `ptConstraintLengths` | f32 | yes |
| 560 | `unk_8ecbb2b` | u16 | **no** |
| 576 | `unk_9f71907` | f32 | **no** |
| 592–640 | `ptWarp/Weft/Shear/BendClusters` | {u16 start,u16 end} | **no** |
| 656 | `ptCapsuleDefs` | 80 B | yes |
| 672 | `ptPlaneDefs` | 48 B | yes |
| 688 | `ptDriverBindPose` | dmFrameMirror (q,p,s — 48 B) | **no** |
| 704 | `ptDriverMap` | u16 | **no** |

Header counts not yet read: `kinematicCount`@256, `warp/weft/shear/bendClusterCount`,
`maxLevel`@272, `boneCount`@274, `driverCount`@276, `planeCount`@280, `density`@248.
Capsule def also carries unread `scale`@32, `solver`@66, `hide`@67 (a `hide` capsule
should probably not collide). Plane def carries unread `padding` only — fine.

Two hypotheses fall straight out of the names, and both bear directly on the open defects:

**H1 — `ptAttachmentLengths` is a tether (long-range-attachment) system, together with
`ptParentIndices` / `ptKinematicRoots` / `ptLevels`.** This is the classic NvCloth/PhysX
LRA model: each particle has a kinematic anchor (its root up the `parentIndices` chain),
`levels` is chain depth, and `attachmentLengths` is the allowed distance along that tether.
If so, the value may be an **absolute length in cloth-local units to the anchor particle's
current position**, not a normalized 0..1 fraction of anything, and the current
"clamp distance from the skinned pose by `al × slider × cageSpan`" is the wrong constraint
in both reference point and scale. The audit's "normalized 0..1" note came from a header
comment, not a measurement — the harness coverage report (min/max/mean per cage, alongside
each cage's physical span) settles this in one run. This directly attacks the residual
zigzag and the parked-at-clamp behaviour.

**H2 — the `ptDriver*` / `ptFollower*` / `ptWeights` / `ptTangentIndices` arrays are the
authored driving mechanism** — how the simulated cage drives the render mesh / bones
(`driverBindPose` = per-driver bind frames, `driverMap`/`driverInfluences` = indices,
`boneCount`/`driverCount` in the header). If so, the game ships exactly the mapping the
app currently *reinvents* with the nearest-particle-within-10cm anchor search — the
mechanism that produced the shared-cage defect and the bone-327 jut. An authored mapping
is per-piece by construction, which would resolve garment provenance without any merge-time
plumbing. Decoding these arrays is the single highest-value research item.

Also confirmed while planning: `CascReader` is pure Qt (no CascLib, no WinAPI in the
interface) — portable to a Linux build. The `.clt.json` tuning block
(`dmClothTuningMirror`) carries `vGravity` (z = −22 for the barF cape), per-constraint-type
stiffnesses (`flStretching/Horizontal/Shear/BendingStiffness` — which map onto the
warp/weft/shear/bend **clusters**, hypothesis H3: the cluster arrays partition
`ptConstraintIndices` by constraint class so each class gets its own stiffness),
`nIterations`, `flDampingFactor`, `flDragFactor`, `flLiftFactor`, `eSolverOverride`,
`fUseShapeCollision`, and `flAttachmentStiffness` — none of which the solver consumes today.

---

## 1. Goals and non-goals

**Goal.** A standalone, deterministic, headless CLI (`tools/d4cloth/`) that loads a model +
its authored cloth data, simulates it with *no OpenGL and no window*, and reports what
happened in machine-readable text — precise enough that a wrong solver is visible in a diff
and a wrong hypothesis dies in one run. Then use it to rebuild the physics authentically.

**Non-goals.** No GUI. No screenshots. No dependence on the main app's build. No tuned
magic constants presented as fixes — authored data or measured justification only.

## 2. Architecture

```
tools/d4cloth/
  CMakeLists.txt          # standalone; also add_subdirectory-able from the root build
  README.md
  src/
    main.cpp              # CLI dispatch
    AssetSource.{h,cpp}   # corpus dir (extracted files) OR live CASC via CascReader
    ClothDoc.{h,cpp}      # FULL ClothData parse — all 27 arrays + full header
    Coverage.{h,cpp}      # authored-data coverage report (§6)
    Scene.{h,cpp}         # skeleton, skinning, capsule/plane posing, anim playback
    LegacySolver.{h,cpp}  # faithful replica of today's springBoneStep/cage path (§10)
    Solver.{h,cpp}        # the corrected PBD solver (written LAST, from research)
    Dump.{h,cpp}          # CSV writers (§5)
    RenderText.{h,cpp}    # SVG / ASCII orthographic projections (§7)
    Invariants.{h,cpp}    # per-step checks (§8)
    Scenario.{h,cpp}      # named scenarios (§9)
    Compare.{h,cpp}       # golden/A-B diff (§9)
  scenarios/              # scenario definitions + committed golden files
  corpus/                 # extracted test assets (meta+payload+clt.json+anim), small
```

Dependencies: Qt6 Core only (the repo already requires it; QByteArray/QFile/JSON are used
throughout the parser). Reused app sources are compiled in directly (`ModelParser.cpp`,
`AnimParser.cpp`, `RigMath.h`, `CascReader.cpp` for the Windows/extract path) — no fork of
the parser. Where the app files need de-widgeting, the split happens in the app tree
(e.g. moving pure logic out of `GLModelWidget.cpp` is *deferred to the port step*; the
harness replicates the solver logic in `LegacySolver` rather than #including a QOpenGLWidget).

**Platforms.** Builds on Windows (MSVC, same toolchain as the app — for CASC extraction and
final in-situ verification) and Linux (for the fast iteration loop in the cloud sandbox).
Pure QtCore code, so this is cheap. Floating-point note in §4.

## 3. Data access

Two asset sources behind one interface:

- `--casc "G:\G Games\Diablo IV"` — live CASC via the existing `CascReader` (Windows
  primarily; the reader is portable but the install drive lives on the Windows box).
- `--corpus tools/d4cloth/corpus/` — a directory of extracted `meta/<sno>` +
  `payload/<sno>` blobs plus the relevant `Cloth/*.clt.json` and anim SNOs.
  `d4cloth extract --casc <dir> --outfit barF_base03_TRS ...` produces it (runs once,
  on Windows). The corpus for the full test matrix is a few tens of MB and becomes the
  portable, versionable test fixture — the Linux build iterates against it exclusively.

Outfit assembly (which pieces merge onto which base body) follows the same piece lists the
app uses; the corpus stores the resolved piece set per named test case so the harness does
not depend on the Wardrobe tab's logic.

## 4. Determinism contract

- Fixed timestep `dt = 1/60 s` (configurable, logged). No wall clock anywhere; step count
  is the only time axis. No RNG in any code path (scenario impulses are closed-form
  functions of the step index; if noise is ever wanted it is seeded and the seed printed).
- Single-threaded solve. Same binary + same inputs ⇒ **byte-identical output**. This is the
  regression guarantee and it holds per platform/build.
- Across compilers (MSVC vs gcc) bit-identity is *not* promised; `compare` therefore has an
  explicit `--tol` (default 0 for same-platform golden checks, documented small epsilon for
  cross-platform checks). Goldens are committed per platform if they ever differ.
- Every run prints a header: binary build id (git hash + build time), input identity
  (SNO ids + byte sizes + FNV hash of each blob), dt, step count, all effective parameters
  after CLI overrides. A result that cannot state what produced it is not a result
  (this enforces working-method rule 3: the "is the binary newer than the source" check is
  automated — the harness embeds its own source hash).

## 5. Text-first dumps

All dumps are CSV with a `#`-comment preamble that states **units and the thresholds in
effect**. Column names carry units explicitly (`_wu` = world units, `_n` = normalized,
`_wu2` = squared). The preamble states the world-unit convention once measured (§11.3).

- `--dump particles` → per step, per cage particle:
  `step,cage,vert,px_wu,py_wu,pz_wu,tx_wu,ty_wu,tz_wu,drift_wu,attachLen_raw,attachLen_interp,md_wu,invMass,pinned,level,parentIdx,kinRoot,contacts,atLimit`
  (`attachLen_raw` = the authored number as shipped; `attachLen_interp` = the world-space
  limit the solver derived from it, so the dimensional conversion is *visible in the data*.)
- `--dump bones` → per step, per cloth bone:
  `step,bone,name,parent,px_wu,py_wu,pz_wu,animPx_wu,animPy_wu,animPz_wu,drift_wu,cage,vert,driveW,driven,pinned,hair,animMoves,chainLen_wu,restLen_wu`
- `--dump summary` → one line per step:
  `step,worstDrift_wu,worstDriftBone,worstPen_wu,contacts,kineticE,atLimitCount,clampHits,tetherViolations,nanCount`
- `--dump contacts` → per contact: particle/bone id, capsule id, penetration, normal —
  for diagnosing wrong-side pushes.
- Dumps go to stdout or `--out dir/`; every scenario run also always writes `summary`.

## 6. Authored-data coverage report (`d4cloth inspect`)

Per ClothData block found in each piece: every one of the 27 arrays + every header field,
with: present? size (bytes, elements), element count vs `vertexCount`, min/max/mean (numeric
arrays), index-range validity (index arrays), **and consumption status** — one of
`CONSUMED (where)`, `PARSED-UNUSED`, `UNPARSED`, `FALLBACK (default X used because Y)`.
Cross-referenced against the matching `.clt.json` (both `<name>.clt.json` and
`<name>_sim.clt.json` naming conventions), reporting each tuning field and whether the
runtime reads it. This report on the eight barF cages is **research deliverable #1** —
it decides H1 (attachLen scale) by inspection: if per-cage `max(attachLen) ≉ 1.0` and
instead tracks each cage's physical chain lengths, the "normalized" premise dies; if it is
1.0 on every cage, the scale search (§11.2) proceeds.

## 7. Headless rendering (SVG + ASCII)

`--render svg --every N` writes `frame_%05d.svg` with three fixed orthographic views
(front XY, side ZY, top XZ) drawn from the same arrays the dumps print: cage particles
(dots; pinned = filled square, at-motion-limit = ring, in-contact = cross), bone chains
(polylines, parent→child), capsules (outline at authored radius × active scale), planes
(clipped line + normal tick), and the animated-target ghost (faint) so drift is visible as
displacement from the ghost. Any bone with `drift > threshold` is flagged red and labeled
with its index. `--render ascii` emits an 80×40 char version of the same projections for
in-terminal reading. Colours/marks carry a legend block in the SVG itself. The known-bad
chain (326→327→328) renders legibly at default zoom on the side view — that is the
acceptance test for the renderer.

## 8. Invariants — checked every step, loud failures

Each invariant prints `INVARIANT <name> step=<n> …offender detail…` and sets a nonzero
exit code at run end (`--strict` aborts at first failure).

1. **Motion limit**: no particle/bone exceeds its authored motion constraint (as currently
   interpreted). Report offender id, distance, limit, both in wu.
2. **Energy**: total kinetic energy non-increasing across steps in force-free settle
   (`rest` scenario after gravity removal); any increase names the step and the subsystem
   that ran between the two measurements (instrumented per-phase energy bookkeeping).
3. **Symmetry**: for a garment whose bind cage is mirror-symmetric about X (detected, not
   assumed — verified against the bind verts within ε), a symmetric input keeps
   `|P(x,y,z) − mirror(P')| < ε` per step. Catches per-index bugs like the +7-stride
   assumptions.
4. **Finiteness**: no NaN/Inf in any state array; checked after each phase, not only at
   step end, so the *producing* phase is named.
5. **Hierarchy**: `world[j] == world[parent] × local[j]` for every bone after pass-2
   reconstruction (ε in the preamble).
6. **Anchor exclusivity**: no two bones share a cage particle anchor — violations are
   listed (bone pair, cage/vert, positions). If research shows sharing is authored
   (via `ptDriverMap`), the check downgrades to a report, never silence.
7. **Clamp-as-mechanism detector**: any particle/bone resting on a safety clamp
   (`kDivergeMax`, step clamp, 1.5× re-bound) for more than K consecutive steps is
   reported as a masked bug — clamps are a net, not a floor.
8. **Constraint sanity at load**: all constraint/triangle/parent/driver indices in range;
   constraint rest lengths > 0 and ≈ bind-pose distances (report the residual distribution —
   a systematic offset here means a space/units mistake in parsing).
9. **Determinism self-check** (`--selfcheck`): run the scenario twice in-process, compare
   byte-identically.

## 9. Scenarios, A/B, regression

Named scenarios (`d4cloth run <scenario> --model <case>`): `rest` (no anim, gravity only —
must reach equilibrium: summary line deltas → 0; nothing moves after N steps), `gravity-drop`
(release from bind pose), `spin` (scripted yaw-impulse profile replicating the interactive
path), `anim <name>` (e.g. `barF_2HM_nav_idle`, fixed frame stepping), `stress` (velocity
spikes + teleport frame to exercise sweep/tunnelling). Each scenario runs a fixed step
count and writes `summary` + goldens.

- `--compare baseline.csv` → per-row deltas, worst offenders first, with the same
  units/threshold discipline.
- `--param name=value` overrides any solver/tuning parameter without rebuild
  (`--params` lists them all with current values and provenance: authored / default / CLI).
- `--golden write|check` per scenario; goldens are committed. A behaviour change without a
  golden update fails CI-style.
- `--trace bone:327 particle:5/68` → per-step causality log for the watched entities:
  every mutation with the responsible phase (integrate/constraint e#/motion-limit/plane
  p#/capsule c#/velocity-pass/clamp), position before/after, applied delta. This is the
  "one measurement that discriminates several hypotheses" tool — it answers *what moved
  bone 327* in one run.

## 10. Two solvers, in order

1. **LegacySolver** — a faithful, line-for-line port of today's cage + spring-bone step
   (including its bugs). Purpose: reproduce the cape defect *inside the harness* so the
   root-cause account (deliverable 3) cites harness numbers, and so fixes are demonstrated
   as A/B diffs against a reproduced baseline rather than argued from screenshots.
   Acceptance: on `barF_base03_TRS`, bones 326/327/328 land at x ≈ −0.61/−0.96/−0.74 and
   327 sits at the divergence cap, matching the in-app measurements.
2. **Solver** (corrected) — designed only after §11 lands. Position-based dynamics with:
   authored constraint network solved per **cluster class** with per-class stiffness from
   the `.clt.json` (H3), authored `invMasses`, authored motion/tether constraints with
   *measured* semantics, authored planes + tapered capsules (respecting capsule `hide`
   and per-capsule `friction`), authored `nIterations`/damping/drag, sub-stepping with
   forces scaled once and consistently (the sub-step divisor is applied in exactly one
   place, asserted by a unit test that runs both 1- and 4-substep configs on a free-fall
   particle and demands identical displacement), collision positional inside the solve +
   exactly one velocity/friction pass after it, and clamps that assert (§8.7) instead of
   sculpting. Driving of bones/render mesh via the authored driver arrays if H2 confirms,
   else via merge-time provenance recorded in `mergeGeometries` (the audit's durable fix).

## 11. Research phase — ordered, decisive measurements first

1. **Full parse + coverage on the test matrix** (all eight test cases). Output: the §6
   report. Decides H1 partially, quantifies every unparsed array, and catches the
   `attachLen` size-mismatch silent-empty case with an explicit `FALLBACK` line.
2. **`attachLen` semantics.** With `ptParentIndices`/`ptKinematicRoots` parsed: check
   numerically whether `attachmentLengths[k]` ≈ bind-distance(k → its kinematic root) or
   ≈ bind-distance(k → parent) or ≈ per-vertex max-distance paint (correlate against all
   three candidate references; print the correlation table). NvCloth's
   `radius = scale × value + bias` model: search the 720 B header + `.clt.json`
   (`flSkinOffset/Exponent/Stiffness`, `unk_c5496ae = 0.1`, `flAttachmentStiffness`) for a
   scale/bias pair that makes the numbers land on the observed in-game drape. Report
   confidence explicitly.
3. **Unit system.** Establish what 1 wu is: capsule radii vs. authored heights vs. a known
   quantity (a ~1.8 m humanoid). Record it in the harness preamble and express every
   threshold in it. (Measured so far: `m_radius` 1.168–1.207 is *half the bbox diagonal* —
   never to be used as a body radius again.)
4. **Driver arrays (H2).** Decode `ptDriverBindPose` (48 B q/p/s frames), `ptDriverMap`,
   `ptDriverInfluences`, `ptFollowerIndices`, `ptWeights`, `ptTangentIndices` on the barF
   cape: counts vs `boneCount`/`driverCount`, whether driver frames coincide with cloth
   bone rests (≤ mm), whether the map is 1:1 bone↔particle. If it is, the 10cm-anchor
   mechanism is replaced wholesale by authored data.
5. **Cluster arrays (H3).** Check that warp/weft/shear/bend cluster ranges partition
   `ptConstraintIndices`, and that per-class rest-length statistics differ as expected
   (warp ≈ vertical chains, shear ≈ diagonals).
6. **Game install cross-check.** Spot-check a handful of garments' ClothData in the live
   CASC against the corpus (byte equality), and inspect `Global`/`CollisionSettings` SNOs
   for a global cloth config (gravity reference −22 z-up appears per-piece; confirm whether
   a global scale exists).

## 12. Root-cause account + port plan (deliverables 3–5)

The cape-defect account is written from LegacySolver traces (`--trace bone:327`), the §6
coverage report, and A/B runs toggling one mechanism at a time (`--param` switches for:
drive weighting, anchor radius, span scaling, tether interpretation). The port plan lists
every `GLModelWidget.cpp` call site that changes (`buildSpringBones`, `springBoneStep`,
`buildClothSim`, `applySkinning` ordering, overlay readers of `m_sb*` state, settings
plumbing for retired knobs) and what state each replaces — written once the corrected
solver passes the §8 invariants and §9 goldens on the full test matrix.

## 13. Test matrix

`barF_base03_TRS` (primary repro, cape) · `barF_base03_HLM` (hood) · `barF_stor161_TRS`
(two-cape/shared-cage) · `barF_stor151_TRS` (fur — regression guard for the provenance
trap) · `BarF_base15_HLM` · a skirt (`barF_base03_LEG`) · a hair asset
(`palF_stor151_HLM_hair`) · a mount from the Stable tab (`mnt_stor161`).

## 14. Milestones

1. **M1** — loader + full ClothData parse + `inspect` coverage report on the matrix.
   *(Research items 1–5 fall out of this milestone.)*
2. **M2** — Scene (skinning, capsule/plane posing, anim) + LegacySolver + dumps + SVG +
   invariants; defect reproduced with numbers.
3. **M3** — root-cause account written from M2 output.
4. **M4** — corrected Solver, validated against scenarios/invariants/goldens.
5. **M5** — port plan + (on approval) the port itself into `GLModelWidget.cpp`.

## 15. Open questions (asked separately)

1. Iteration platform: dual-build (Linux harness in the cloud sandbox for the fast loop,
   Windows build for extraction + final verify) vs Windows-only.
2. Corpus bootstrap: who runs the one-time `d4cloth extract` on the Windows side.
3. Confirm LegacySolver-first (reproduce, then fix) vs going straight to the new solver.
