# M4 — corrected solver: validation status

_2026-07-25. A/B runs: `--solver legacy` vs `--solver authored`, identical scenarios,
identical inputs (FNV-verified). Commands as in ROOTCAUSE.md with `--solver authored`._

## Fixed, with numbers

**The cape jut/zigzag (primary defect).** cape-outfit, `barF_2HM_nav_idle`, step 590:

| chain 326→327→328 drift [wu] | legacy | authored |
|---|---|---|
| bone 326 | 0.272 | 0.076 |
| bone 327 | 0.049 | 0.077 |
| bone 328 | 0.036 | 0.080 |

Legacy: one bone out, chain returns (the jut). Authored: uniform coherent drape.
divergeClamped: legacy 7, authored 0. Worst parked bone: legacy 0.328 (at the cap,
sustained); authored transient 0.326 at step 2 only, settling.

**The two-garment shared-cage defect.** two-capes (base00 + stor161_TRS + base03_HLM):

| | legacy | authored |
|---|---|---|
| shared anchors | **111** (incl. the audit's exact `cage=5 vert=16 bones 329/475`) | **0** |
| driven bones | 244 (146 of them the hood's phantom rig latched onto the stor161 cage) | 63 (the cape's own authored followers) |

The hood ships no ClothData → its bones are never cage-driven (authentic; they fall back
to the pre-cage-era spring path).

**Driver-table interpretation validated.** Pinned-particle targets, authored vs legacy
borrowed skinning, per cage (cape-outfit step 590): cages 0, 6, 7 agree to **0.0000 wu**
— exact to float precision, confirming `ptDriverMap[b]` indexes piece-skeleton bone `b`.
Cages 1/2 (body Wrath chains) differ by mean 0.065–0.078 — inspection shows the LEGACY
borrowed skinning is cross-garment there too (same ROOTCAUSE mechanism); the authored
side is per-piece by construction.

**Fur regression guard.** fur-regression runs clean: 25 authored followers drive the fur
cape from its own cage; no provenance filter, nothing inferred from skin indices, so the
failure mode that reverted the 2026-07-24 fix cannot occur.

**Invariants.** nan=0 everywhere; energy bounded; no NaN nets triggered; padding
particles/constraints excluded (real set `[0, vertexCount)`).

## Open items (honest list — none are the fixed defect class)

1. **Skirt drape needs a visual pass.** skirt case: follower-driven hem bones settle
   0.31–0.37 wu from their skinned targets (parked steady, within tether budget of their
   ROOTS — not a tether violation; the clamp-residency invariant still measures the
   legacy quantity |pos − target| and flags it). Whether this drape matches the game
   needs the SVG render compared against in-game reference — likely a tuning-fidelity
   question (collision scale 0.52 on authored radii, damping mapping) rather than a
   mechanism bug. Follow-up: re-express the residency invariant in tether terms and
   eyeball `--render-every` output.
2. **Damping/drag mapping is a documented approximation** (Solver.h header): Verlet
   `keep = 0.93 × (1 − drag·0.1)`; NvCloth's exact damping semantics remain undecoded.
   Candidate refinement: derive from `flDampingFactor`/`flDragFactor` once a reference
   capture of in-game motion exists to fit against.
3. **Body-range chain bones (Wrath chains) are not yet driven** — their followers point
   at bones below `nBaseBones`, which the render path treats as body. Driving them is an
   improvement over the app (the game simulates them) but changes the body palette;
   deferred to the port (needs `sbIsCloth` to include them).
4. **tether-taut is a state, not a bug**: ~100 particles/step ride their tether under
   drape — that is the LRA model working (taut rope = hanging cloth). The summary column
   documents it; it must not be read as the legacy "clamp as mechanism" smell.
5. `worstPinnedTargetErr` in the summary is a *delta vs legacy borrowed skinning*, not an
   error bound — large values indict the legacy side wherever spot checks show
   cross-garment borrowing (as measured on cages 1/2 and the fur piece).

## What the corrected solver consumes that the app never did

`ptDriverInfluences` + `ptWeights` (cage skinning), `ptDriverMap` (driver→bone),
`ptFollowerIndices` (bone driving), `ptKinematicRoots` + `ptAttachmentLengths` (tethers),
`ptParentIndices` (validation), warp/weft/shear/bend clusters + per-class .clt stiffness,
authored `vGravity`, real `vertexCount`. The 10 cm anchor search, `driveW`, `cageSpan`,
`tnorm²`, the borrowed render-vert skinning and the (pos − target) drive are all gone.

---

## 2026-07-25 — In-app confirmation (post-port)

User-verified fixed in the rebuilt app. `cloth_cage_diag.txt` from the live build
(dlux100 cape outfit — a case OUTSIDE the test matrix): all 8 cages report
`authoredSkin=nv, matchedRenderVert=0, boneFallback=0` — the authored skinning path
took over completely; the cross-garment borrow never ran. `cageDriven=116` bones via
followers. Regression goldens for all 7 harness cases committed under
`scenarios/golden/` (compare with: `d4cloth run ... --solver authored`, diff the
summary CSV — byte-identical on an unchanged solver, same-platform build).
