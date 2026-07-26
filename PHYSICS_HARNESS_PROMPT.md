# Prompt — Build a Cloth Physics Diagnostic Harness (`d4cloth`)

> Paste this as the opening message of a fresh session. It is self-contained: it carries the
> paths, the measured data, the confirmed bugs, and the failure modes already ruled out, so
> none of it has to be rediscovered.

---

## Task

Build a **standalone, text-first diagnostic harness** for the cloth/physics system of
**Diablo4AssetBrowser Native**, then use it to rebuild that physics system so it is
(a) authentic to Diablo IV, (b) backed by parsed game data rather than tuned constants, and
(c) visually correct in the tool's preview.

The harness is the deliverable *first*. Fixes come from it, not before it.

**Do not** iterate on the main application to diagnose physics. That approach failed: it costs
a full rebuild per hypothesis, and the only feedback channel is a screenshot, which cannot
distinguish "the solver is wrong" from "the overlay is drawing something misleading."

---

## Paths

| What | Where |
|---|---|
| Application source | `C:\Users\notso\Downloads\Claude Current\Diablo4AssetBrowser Native` |
| Cloth/physics code | `src/gl/GLModelWidget.cpp` (`buildSpringBones`, `springBoneStep`, `buildClothSim`, `buildPhysBones`) |
| Parser | `src/model/ModelParser.cpp` (cloth payload at the `ClothData` block) |
| d4data JSON snapshot | `C:\Users\notso\AppData\Roaming\Diablo4AssetBrowser\Diablo4AssetBrowserNative\d4data` |
| Game install | `G:\G Games\Diablo IV` |
| Prior audit (read first) | `PHYSICS_AUDIT.md` in the app root |
| Backups (pre-cage-driving) | `.Backups/src_20260716_200352_pre-npanel`, `_backups/src_20260712_*` |

---

## Read this before designing anything

`PHYSICS_AUDIT.md` records what is fixed, what was tried and reverted, and — importantly —
**what was refuted by measurement**. Do not re-litigate those. Summary of the traps:

1. **`capsuleRadius` 0.52 is correct.** 1.0 was tried and visually splays garments open. The
   authored capsule radii are larger than the visible body; that is expected.
2. **Provenance inferred from skin indices does not work.** Restricting a bone to cages whose
   particle→bone span covers it broke fur on `barF_stor151_TRS`, because a cage skinned only
   to body bones excludes every cloth bone. Real per-piece provenance must be recorded at
   merge time in `ModelParser::mergeGeometries`.
3. **The 10cm anchor radius is not the problem.** Measured `driveW=1.00` on every bone — all
   anchor matches are coincident (≤3cm). Distance-weighting the drive changes nothing.
4. **`attachLen` is normalized 0..1 but was consumed as a world distance.** This is a
   confirmed dimensional bug; a partial fix (scale by cage span) is in place and unverified.

---

## Confirmed data (do not re-derive)

**Cloth payload** (`ModelParser.cpp`, `ClothData` block):

| Field | Offset / source | Semantics |
|---|---|---|
| `bindVerts` | cage vertex positions, bind space | particle rest positions |
| `invMasses` | per vertex | `0.0` = pinned/kinematic |
| `constraintIdx` / `constraintLen` | index pairs + rest lengths | distance constraint network |
| `attachLen` | array @ `+400` (`ptAttachmentLengths`) | **normalized 0..1**; 0 = locked to skinned pose, 1 = free. Only parsed when `arraySize/4 == vertCount` — otherwise silently empty |
| `planes` | `ptPlaneDefs` | plane colliders, with `stiffness`, `friction`, `boneIndex` |
| `triangles` | `ptTriangles` | cage topology |
| `name` | 32-byte field @ `+216` | keys `Cloth/<name>.clt.json` **or** `Cloth/<name>_sim.clt.json` — both conventions ship |

**Collision:** bone-bound tapered capsules (`m_colBoneA/B`, `m_colP0/P1Bind`, `m_colR0/R1`),
posed each frame from animated body bones.

**Measured on a Barbarian female (`barF_base03_TRS`, Fur-Lined Robe):**

```
nb=439..585   baseBones=293   cages=8   authoredCaps=24..25
simulated bones 146 (one cape) / 261..292 (two capes)
model radius 1.168..1.207        # NOTE: half the BOUNDING-BOX DIAGONAL, not a body radius
collision: ~1800-2200 contacts/frame, worst penetration 0.054-0.065, margin 0.020,
           capsule scale 0.52, substeps 2
```

**Cage structure:** particles form a grid. Consecutive bones along a chain map to cage verts
**+7 apart** (row stride). Verified: bones 298→55, 299→62; 327→68, 328→75. The bone→particle
mapping is orderly and correct — do not suspect it without evidence.

**Known-bad chain** (the reproduction case): bones 326 → 327 → 328 at
x = −0.61, **−0.96**, −0.74. The middle bone juts out and the chain returns. Bone 327 is
cage 5 vert 68, `driven=1`, and sat at the divergence cap (0.327) every frame for hours.

**History:** `m_sbDriven` / `m_sbAnchorPiece` / `m_cages` — the entire cage-driven bone
mechanism — exist in **no backup**. Before 24 July, cloth bones were pure spring-bone
simulation. That is the "it used to work" boundary. The mechanism was added so cage collision
would reach the fabric *between* bones (the leg-clipping fix).

**Open defect:** two garments can share one cage. Bones 329 and 475 — different rigs,
different name hashes — both resolved to `cage=5 vert=16` with byte-identical positions.
`drawn=292` was exactly 2× the single-cape 146. Also doubles simulation cost.

---

## Harness requirements

Build `tools/d4cloth/` as a **separate CLI binary**. Qt is available (the app already uses
Qt6); a GUI is not wanted. Optimise every decision for *machine-readable diagnosis*.

### 1. Deterministic and headless
- Fixed timestep, no wall-clock, no frame-rate dependence, no RNG (or seeded and logged).
- Same input ⇒ byte-identical output. This is what makes regression comparison possible.
- Loads a model + its cloth data directly from d4data/CASC. No OpenGL, no window.

### 2. Text-first output, designed to be read
- `--dump particles` → CSV per step: `step,cage,vert,px,py,pz,tx,ty,tz,drift,attachLen,md,invMass,pinned,contacts`
- `--dump bones` → CSV: `step,bone,name,parent,px,py,pz,animPx,animPy,animPz,drift,cage,vert,driveW,driven,pinned`
- `--dump summary` → one line per step: worst drift, worst penetration, contact count, total
  kinetic energy, count of particles at their motion limit.
- Every quantity that participates in a comparison must be printed **with its units** and
  alongside the threshold it is compared against. The single most expensive bug in this
  codebase was a normalized value silently compared to a world-space distance.

### 3. ASCII / SVG rendering
Emit an SVG or ASCII orthographic projection (front/side/top) of cage particles, bone chains
and capsules, per step or per N steps. **You must be able to see the failure without asking
the user for a screenshot.** Colour/mark: pinned particles, particles at their motion limit,
particles in contact, and any bone whose drift exceeds a threshold.

### 4. Invariants, checked every step, failing loudly
- No particle exceeds its authored motion constraint (report the offender, not just a count).
- Total energy is non-increasing in the absence of input forces.
- A symmetric garment under symmetric input stays symmetric (catches per-index bugs).
- No NaN/Inf anywhere; a bone's world transform equals `parent × local` for every bone.
- **No two bones share an anchor particle** — or if that is legitimate, it is reported, not silent.

### 5. Authored-data coverage report
For each cage, print every authored field, whether it parsed, its size vs `vertCount`, its
min/max/mean, **and whether the runtime actually reads it**. Fields that parse but are never
consumed, or that fall back to a default, must be listed explicitly.
*This alone would have surfaced the `attachLen` bug immediately.*

### 6. Scenarios
Reproducible named cases, runnable by name:
- `rest` — no animation, gravity only, settle to equilibrium; nothing should move after N steps.
- `gravity-drop` — release from bind pose.
- `spin` — user rotation impulse (the interactive-physics path).
- `anim` — playback of a named animation (e.g. `barF_2HM_nav_idle`).
- `stress` — high velocity, to exercise tunnelling and the sweep path.

### 7. A/B and regression
- `--compare baseline.csv` → table of per-bone/per-particle deltas, worst offenders first.
- `--param cloth.maxDistance=0.5` → override any parameter from the CLI without a rebuild.
- Golden files committed per scenario, so a change that alters behaviour says exactly where.

---

## Research phase (do this before writing solver code)

1. **Parse the real cloth data.** Enumerate every field in the `ClothData` block, not just
   the ones currently read. Compare against d4data JSON (`.app.json`, `Cloth/*.clt.json`) to
   recover names and intent. Report anything unparsed.
2. **Determine the true semantics of `attachLen`.** It is normalized. Find what the game
   scales it by — look for a motion-constraint scale/bias in the payload or the `.clt.json`.
   NvCloth's model is `radius = scale × value + bias`; establish whether D4 ships those.
3. **Establish the unit system.** Confirm what one world unit is in the model space, and
   record it. Every threshold in the solver must then be expressed in those units explicitly.
4. **Inspect the game install** (`G:\G Games\Diablo IV`) for shipped cloth config, and
   cross-check a handful of garments against their authored values.

Only after this, design the solver.

---

## Solver goals

Authentic first, then stable, then pretty:
- Position-based dynamics with the authored constraint network, authored `invMasses`,
  authored motion constraints correctly scaled, authored plane colliders, and the tapered
  capsule set.
- Sub-stepping, with forces divided consistently (a prior bug applied a sub-step divisor on a
  path that did not sub-step).
- Collision resolved positionally inside the solve, with exactly **one** velocity/friction
  pass afterwards (running it inside the iteration loop mistakes constraint displacement for
  velocity — this bug already occurred).
- Clamps are a **safety net, not a mechanism.** If a particle is resting on a clamp every
  frame, the clamp is masking a bug. Assert on it.

---

## Working method — non-negotiable

1. **Instrument before hypothesising.** Every claim about behaviour must cite a number the
   harness printed. In the prior session four consecutive fixes were built on inference from
   screenshots; all four were wrong.
2. **Treat "nothing changed" as decisive information.** If a change that *must* alter the
   output produces no change, the thing you changed is not in the code path being exercised.
   Stop and locate the actual path. This signal was given repeatedly and discounted.
3. **Verify the binary is newer than the source** before interpreting any result.
4. **State confidence explicitly**, and say plainly when a hypothesis has been refuted rather
   than quietly keeping the change.
5. Prefer one measurement that discriminates between several hypotheses over several
   speculative fixes.

---

## Deliverables

1. `tools/d4cloth/` — the harness, building standalone, documented in its own README.
2. An authored-data coverage report for a representative set of garments.
3. A written root-cause account of the cape defect, backed by harness output.
4. A corrected physics implementation, validated in the harness against the scenarios and
   invariants above.
5. A port plan for folding the corrected solver back into `GLModelWidget.cpp`, listing every
   call site that changes.

## Test cases

`barF_base03_TRS` (Fur-Lined Robe, cape — primary reproduction) · `barF_base03_HLM` (hood) ·
`barF_stor161_TRS` (second cape; two-cape/shared-cage case) · `barF_stor151_TRS` (fur;
regressed by a previous fix) · `BarF_base15_HLM` · a skirt · a hair asset · a mount from the
Stable tab.

---

## First step

Read `PHYSICS_AUDIT.md`, then the four cloth functions in `GLModelWidget.cpp` and the
`ClothData` parser. Produce a written plan for the harness — its CLI surface, its output
formats, and the invariant list — **before** writing solver code. Ask about anything in the
scope that is ambiguous.
