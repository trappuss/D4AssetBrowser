# Model `.glb` export — design & implementation plan

> **Status (current):** Mesh export (static **and skinned**) is **implemented,
> wired, and verified**, plus a native 3D viewport.
> `src/model/ModelParser.cpp` natively parses the `.app` LOD0 geometry, skeleton
> (BoneData/BoneStructure), per-vertex blend indices/weights and per-segment bone
> palettes (port of d4extract's `app_parser.py`). `src/model/ModelExporter.cpp`
> writes a self-contained `.glb` — for skinned models it emits JOINTS_0/WEIGHTS_0,
> a skin, bone-node hierarchy and inverseBindMatrices. `src/gl/GLModelWidget.cpp`
> shows the mesh in-app (orbit camera). The **"Export .glb"** button reads
> meta+payload from CASC and runs the pipeline.
>
> Verified against the d4extract reference: geometry identical across 5 sample
> models (static stride-36 + skinned stride-44), bounding-box identical to a
> shipped reference `.glb`, skeleton matrices identical to ~1e-16 (317-bone
> character), per-vertex global joints/weights identical, and the exported skinned
> glb loads with the same 317-joint skin as the reference. The Python `d4extract`
> dependency is **no longer needed** for mesh export.
>
> **Base-color textures are now embedded too**: `src/tex/BcDecode.cpp` CPU-decodes
> BC1/BC3/BC4/BC5 (bit-exact vs Pillow, verified on synthetic blocks + a real
> 1024² payload); the exporter PNG-encodes a material's `BASE_COLOR` map and writes
> a glTF image/texture/sampler + `baseColorTexture`. ModelsTab reads each
> material's base-color payload from CASC and decodes it at export time.
>
> Remaining follow-ups: BC7 decode (format 50 — gradient ramps/body markings),
> normal/roughness/metal/AO/emissive texture channels (only base color is embedded
> so far), alpha-mode detection, and animations. Sections below are the original
> plan, kept for those.

This is the one substantial d4analyzer feature not yet in the native tool. It is
split into two independent halves with a stable contract between them
(`src/model/ModelGeometry.h`):

```
base/payload/<sno>  ──[ ModelParser  (TODO) ]──►  ModelGeometry  ──[ ModelExporter
                                                                     (fastgltf) ]──► .glb
```

Everything except the binary **ModelParser** is already designed or reusable. The
parser is the genuinely hard part, because it requires the D4 binary model format
(vertex/index/skeleton layout), which is **not** present in d4data's JSON.

---

## 1. ModelExporter (fastgltf) — writable now

`fastgltf` (already linked) can both parse and **write** glTF via `fastgltf::Exporter`.
Build a `fastgltf::Asset` from a `ModelGeometry` and serialize to `.glb`:

1. **Buffer**: concatenate, per primitive, the interleaved vertex bytes then the
   index bytes into one `std::vector<std::byte>`; add one `fastgltf::Buffer`.
2. **BufferViews / Accessors** per primitive:
   - POSITION → `vec3`/float, with `min`/`max` (required by the spec).
   - NORMAL → `vec3`/float.
   - TEXCOORD_0 → `vec2`/float.
   - JOINTS_0 → `vec4`/unsigned short, WEIGHTS_0 → `vec4`/float (skinned only).
   - indices → scalar/unsigned int.
3. **Materials**: one `fastgltf::Material` per distinct `MeshPrimitive::materialName`.
   Resolve textures + MaterialValues with the existing parsers
   (`model/Material.h` → `parseMaterialJson` + `parseMaterialValues`) and apply the
   **same accuracy rules already implemented** for textures in `glb_merge` (Python
   fork) / the texture passes here:
   - metalness/roughness factors (when no MR texture), AO strength, emissive colour
     × multiplier via `KHR_materials_emissive_strength`;
   - hide shadow/collision proxy sub-objects; make FX/particle materials BLEND;
   - `doubleSided` from `MeshPrimitive::doubleSided`.
   Embed each texture as a PNG bufferView+image+texture (decode via the existing
   GPU `GLTextureWidget::grabImage()` path, or a CPU BC decoder if added).
4. **Nodes / Skin**: one node per primitive (static) or a `fastgltf::Skin` built
   from `ModelGeometry::skeleton` (joints + `inverseBindMatrices` accessor) for
   skinned meshes.
5. **Coordinate system**: D4 is left-handed Z-up; glTF is right-handed Y-up. Apply
   the `(x, y, z) → (x, z, -y)` transform **in the parser** when filling
   `ModelGeometry` (matches d4extract's default `z_up_to_y_up`), so the exporter
   stays coordinate-agnostic. Do it in exactly one place to avoid double-rotation.
6. **Serialize**: `fastgltf::Exporter` → `exportAsBinary` → write the `.glb`.

This half can be implemented and unit-tested with a synthetic `ModelGeometry`
(e.g. a single triangle) before any real parsing exists.

---

## 2. ModelParser — the blocker (needs the D4 binary model format)

A `ModelGeometry parseModel(const QByteArray& payload, const QByteArray& meta)` must
read the appearance/model payload (`base/payload/<sno>`, with descriptor in
`base/meta/<sno>`) and produce vertices, indices, sub-object→material bindings,
and (for characters) the skeleton + skin weights.

Why it isn't done here:
- The vertex/index/bone layout is a **binary** format, not in d4data's JSON.
- The Python fork never parsed it — it shelled out to **d4extract** (narascode).
- d4analyzer parses it natively in C++, but the layout isn't publicly specified in
  a form that can be transcribed and verified blind.

The authoritative reference is **d4extract's** parser
(`d4extract-original/src/d4extract/…`, present in the workspace): its
`app_parser` / model reader shows the exact buffer layout, the sub-object→material
mapping, the bone/skin extraction, and the `z_up_to_y_up` conversion. Port that to
C++ filling `ModelGeometry`. This is a real, multi-day reverse-engineering/porting
task and must be validated against known-good exports (the workspace has reference
`.glb`s under `.backups/.../d4extract model example/`).

---

## 3. Interim: wrap d4extract (working export today)

For a working export before the native parser exists, invoke d4extract via
`QProcess`, exactly as the Python fork's `ModelExportWorker` does:

1. Read the appearance `.app` meta + payload (and each material's textures, shared-
   alias aware) from CASC into a temp dir, laid out as loose `.tex` for
   `--with-textures`.
2. Run (d4extract's CLI has no `__main__` guard, so use `-c`, not `-m`):
   ```
   python -c "from d4extract.cli import cli; cli()" export \
       --with-textures --texture-dir <tmp/tex> --include-cloth \
       <tmp/meta> <tmp/payload> -o <out>/<name>.glb
   ```
3. Optionally pass animation meta/payload pairs; pre-filter long-form (>255 keyframe)
   clips that d4extract can't decode, and retry without animations on failure.

Trade-off: reintroduces a Python + d4extract dependency the native tool exists to
remove — so it's an **interim**, gated behind a configured d4extract path (add a
row in Settings ▸ Directories), not the end state.

---

## 4. Recommended sequence

1. Implement **ModelExporter** against the `ModelGeometry` contract + a synthetic
   triangle test (no real data needed). Low risk, fully testable.
2. Ship the **d4extract-wrap interim** behind a Settings path → working `.glb`
   export immediately.
3. Port **d4extract's model parser** to C++ (`parseModel` → `ModelGeometry`),
   validating against the reference `.glb`s, then drop the d4extract dependency.
4. Add the `QOpenGLWidget` 3D viewport last — it consumes the same `ModelGeometry`,
   so it falls out almost for free once the parser exists.

The `ModelGeometry` contract (`src/model/ModelGeometry.h`) is the fixed point: build
steps 1, 3, 4 against it independently.

---

## 5. Blender-friendly export (`export/blenderFriendly`)

Settings ▸ Export ▸ **"Blender-friendly rig (.L/.R names + X-mirror orientation)"**.
Two coordinated transforms, both derived from real game data (see
`D4_BoneHash_Research_Report.md`):

**Orientation.** D4 characters face D4 **+X** with the character's left along
D4 **+Y** (verified: `HP_chestFront` sits at +X of the chest bone; the
`HP_leftHand` bone rests at +0.65 on Y in barM_base00). The standard export
lands in Blender facing +X with left = +Y, so Blender's X-Mirror pairs nothing.
Blender mode bakes one extra proper rotation (glTF-space rotY −90°,
`(x,y,z) → (−z,y,x)`) into vertices, normals, root-bone TRS/matrix, inverse
bind matrices (`IBM·R⁻¹`) and root-bone animation channels. Result in Blender:
facing **−Y**, left = **+X** — the Blender character convention. Child bones
and child anim channels are parent-relative and untouched.

**Naming** (`GLModelWidget::blenderizeSkeletonNames`). Blender pairs bones whose
names differ only by a `.L`/`.R` suffix:
- The 26 identified player-rig bones get curated names (`hand.L`, `shin.R`,
  `pelvis`, `head`…) keyed by bone hash — valid for every player class (identical
  190-bone core rig).
- Every other bone goes through geometric mirror detection: reciprocal
  nearest-neighbour of rest-pose bone heads across the D4 Y plane (6 mm
  tolerance). Pairs get `m<idx>.L` / `m<idx>.R` (idx = skeleton index of the left
  member). This covers cloth/hair chains, monsters and mounts — measured: 158 of
  318 bones paired on barM, 58 of 82 on the goatman.
- Center/unpaired bones keep `bone_<hash>`. All names deduplicated.

**Pose X-Axis Mirror symmetrization** (`export/xMirror`, default ON, requires the
Blender-friendly rig): naming + orientation alone are NOT enough — D4 authors
left/right bone orientations ~180° apart as mirrors, so Blender's pose mirroring
produced wrong results (measured 0.27 m at the hand). With this option the
exporter rewrites each paired `.R` bone's world rest rotation to `Mx·R(.L)·Mx`
(translation kept), rebuilds every local TRS against the new hierarchy, corrects
inverse binds as `IBM' = C⁻¹·IBM` (`C = R⁻¹R'`, exact for the payload-authored
binds), and conjugates embedded anim curves per bone
(`q' = C_p⁻¹⊗q⊗C`, `t' = C_p⁻¹·t`). Pairing is `Retarget::mirrorPairs` — curated
hash pairs first, geometric reciprocal nearest-mirror second — the same table
the `.L/.R` naming uses. Blender-verified (4.2.9, Bone Dir Blender & Temperance):
paste-flipped/X-mirror error 1e-6 m, bind identical (2.4e-7), animation world
trajectories unchanged. Full derivation + evidence: `D4_XMirror_Spec.md`.

Verification recipe: export barM with the toggle on, import into Blender
(default glTF settings), enter Pose Mode, enable Pose ▸ X-Mirror, rotate
`hand.L` — `hand.R` must mirror. `Symmetrize` on the armature should report
matching bone counts for all `.L/.R` pairs.

---

## 6. Retarget & modding (Settings ▸ Export ▸ "Retarget & modding")

Options for porting extracted models onto OTHER games' rigs. All operate on the
export copy only (never the live preview), in this order:
fit-reference body → cloth collapse → anchor remap → bone naming → exportGlb.

**Target engine preset** (`retarget/enginePreset`) resolves the final
`ModelExporter::Options`: Blender = Blender-friendly rig, meters; Unreal/Skyrim =
Blender-friendly rig, ×100 unit scale + DirectX normals (G channel flipped) for
Blender→FBX round-trips (beware: glTF importers that auto-convert m→cm will
double-scale); Unity = plain glTF. Custom respects the individual toggles plus
`retarget/unitScale`. Unit scaling multiplies vertex positions, ALL local bone
translations, inverse-bind translations and anim translation channels (uniform-
scale conjugation — rotations/scales invariant).

**Remap weights to standard humanoid bones** (`retarget/remapWeights`,
`Retarget::remapToAnchors`): merges every bone's skin weights into its nearest
ancestor among the 26 identified player-rig anchors and exports ONLY that reduced
skeleton (anchor world bind transforms — and therefore inverse bind matrices —
are preserved; locals recomputed against the anchor-only hierarchy). Result: ~20
vertex groups that line up with typical humanoid game rigs. Skipped (untouched)
for rigs carrying fewer than 16 anchors — monsters, mounts, props.

**Strip cloth / physics bone chains** (`retarget/collapseCloth`,
`Retarget::collapseClothChains`): removes bones at index ≥ `nBaseBones` and folds
their weights into the nearest kept ancestor. No-op when the boundary is unknown.

**Fit-reference body** (`retarget/fitReference`, ModelsTab): resolves the piece's
name prefix to `<prefix>_base00`, parses that body from CASC, hash-remaps its
joints onto the piece's skeleton (requires ≥50 shared bones) and appends its mesh
as a `__fitReference` material. Use in Blender for clipping checks and as a
weight-transfer source; delete before final export.

**Set-aware batch export** (`retarget/setManifest`, ModelsTab batch): expands the
selection to `_HLM/_TRS/_GLV/_LEG/_BTS` siblings by name and writes
`manifest.json` (file / SNO / name / slot + export date) for reproducible
re-exports after game patches.

**Companion Blender add-on** (`d4_blender_companion.py`, repo root next to the
hash tables): install via Edit ▸ Preferences ▸ Add-ons ▸ Install…; panel in the
3D View N-sidebar, "D4" tab. Three operators, headless-tested on Blender 4.2.9
against real exports:
- *Import D4 .glb* — glTF import + enables Pose X-Axis Mirror, parks the
  auto-playing clip in a muted NLA strip (rest pose shows, clip kept), splits a
  `__fitReference`-material body into its own wire-display, non-rendering
  collection.
- *Transfer Weights from Body* — fit-reference body → selected armor meshes
  (nearest-face interpolated), then Limit Total 4 + Normalize All.
- *Rename Groups to Target Rig* — renames/merges vertex groups (optionally
  bones) to UE5 mannequin or Skyrim NIF names; groups mapping to one target are
  weight-merged (weapon/handEnd fold into hand). Profiles are plain dicts at the
  top of the file — edit or add games freely.
- (v1.1+) *Import D4 Set (manifest)*, *Raise Arms / Apply Pose as Rest*,
  *Toggle Clip Playback*, *QA Report*, *Setup Body Fit*, *Split ORM Textures*,
  *Prep & Export FBX*, and (v1.2) *Import Anim Library*.

## 7. Hardpoint empties, animation library, format probe

**Hardpoints as empties** (`export/hardpointEmpties`): attaches the model's rig
hardpoints (weapon grips, sheaths, trail emitters, look-at sockets…) as named
empties parented under their bone, using the exact game transforms. Read from the
`.app.json` `ptHardpoints`, keyed to the parent bone by hash so they survive
retarget reorder/reduction (`Hardpoints::readInto` → `resolveBoneIndices`). In the
exporter each empty's local = `swapTRS(bone-local)` with the same X-mirror `C⁻¹`
conjugation the bone gets + unit scale — Blender-verified: bind-pose world identical
with symmetrize off/on. Names come from the verified 118-entry `HP_*` table; unknown
hashes export as `HP_<hex>`. Complements weapon-in-hand export (weapons are already
seated on the hand hardpoint and 100%-weighted to the hand bone by `seatWeapon`, so
they follow animation) by also exposing `HP_rightWeapon`/`HP_leftWeapon` for snapping
*other* games' weapons onto the character.

**Animation library export** (Wardrobe ▸ "Export animation library (.glb)"):
exports the RIG + selected clips only, no mesh — a small `.glb` of actions to append
onto an already-imported character in Blender. Multi-select in the ANIMATIONS list
(ctrl/shift) picks exactly which clips; otherwise the "Animations to embed" scope
applies. The exporter's `animLibrary` path emits bone nodes + skin + animations with
no meshes; Blender adds a placeholder mesh for the mesh-less skin, which the
companion's *Import Anim Library* strips while appending the actions as muted NLA
tracks onto the active armature.

**Format probe** (`FormatProbe::run`, startup): parses a known player body
(`barM_base00`, else any `*_base00`) from CASC and checks the skeleton has a
plausible bone count (≥80). A game patch that changes the model/skeleton format
trips this and warns the user up front, instead of exports silently coming out
wrong. Mirrors the icon-audit startup pattern; skipped when CASC/model aren't
available.
