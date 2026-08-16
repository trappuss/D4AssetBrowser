# Context handoff — general-purpose Blender addon for prepping game models

**What this is.** A fresh session is going to build a Blender addon that prepares extracted game
models for use in *other* games — a modern, maintained answer to what **CATS Blender Plugin** was
for VRChat avatars. The **first concrete target is Diablo IV → Dragon's Dogma 2 (RE Engine)**, but
that is a *profile*, not the product. The product is the general toolkit.

**Why a new session:** the session that produced this file was ~90% C++ internals of D4AssetBrowser
(CASC parsing, Qt threading, cache invalidation). None of that helps here and all of it competes for
attention.

Facts about the D4 side below were measured against shipped code. Where something is unverified it
says so. **Read this, then treat the source tree and the actual DD2 tools as the authority.**

---

## 0. Ground truth before anything else

| | |
|---|---|
| **Exists** | `D4AssetBrowser` (C++17/Qt6) — extracts D4 assets, writes `.glb`. Root: `C:\Users\notso\Downloads\Claude Current\Diablo4AssetBrowser Native` |
| **Exists** | A "Modding / retarget" settings group already doing Blender/Unreal/Unity conventions at export time (`src/app/SettingsDialog.cpp`, **Experimental** tab, ~line 1446) |
| **DOES NOT EXIST** | Any Blender addon. Globbed the whole tree and the shared folders: only `verify-src.py`, `docs/parse_probe.py`, `.Resources/tact_scan.py` and vcpkg vendor scripts. This is a **new** addon, not an update. |

---

## 1. The CATS lesson — read this before designing anything

CATS (Cats Blender Plugin) was the standard one-click prep tool for VRChat avatars: model fix,
armature merging, bone merging/renaming, decimation, bone-name translation, visemes, eye tracking,
texture atlasing, material combining. It became the default because it turned a long manual checklist
into a few buttons.

**It went stale because of Blender API churn**, not because the idea was wrong. Anything built now
should be designed so that does not happen again:

- **Target current Blender (4.x) and pin the minimum in `bl_info`.** Avoid deprecated operator
  patterns; prefer direct data-API manipulation (`bpy.data`, `bmesh`) over `bpy.ops` wherever
  possible — `bpy.ops` is the part that breaks between versions and needs correct context overrides.
- **Separate pure logic from Blender bindings.** Rig mapping, name translation and channel repacking
  should be testable functions that take plain data, with a thin operator layer on top. That is what
  makes a version bump a small fix instead of a rewrite.
- **No network dependency for core features.** CATS' bone-name translation leaned on an online
  translate API; that is a permanent liability. Ship tables.

**First research task:** check the *current* state of CATS and its forks (there have been community
continuations). If a maintained fork already covers the generic avatar-prep half well, the right move
may be to complement it rather than duplicate it. Establish this before writing overlapping features.

---

## 2. Suggested architecture: core operations + target profiles

The thing that makes this general rather than a one-off D4→DD2 script:

```
core/           game-agnostic operations
  armature.py     merge, reparent, rename, symmetrise, bone-count reduction
  weights.py      transfer, normalise, limit influences, cleanup
  mesh.py         join, decimate, split by material, UV ops
  materials.py    channel repack (ORM ↔ per-map), atlas, dedupe
  naming.py       name-mapping tables + fuzzy match
profiles/       per-target definitions — DATA, not code paths
  dd2_re.py       skeleton map, units, axis, normal convention, material params
  <next game>.py
```

A profile should be a **declaration** (bone name map, unit scale, axis convention, normal green
direction, material parameter names, socket names), not a fork of the logic. Adding a game = adding a
profile. If a profile needs its own code path, that is a signal the core abstraction is wrong.

**This matters more than it sounds.** The C++ side of this project accumulated the same rule
implemented in four places and they drifted apart, producing four separate user-visible bugs in one
session. One rule, one place.

---

## 3. The boundary question — decide it early

The D4 exporter **already does** several things a CATS-like addon would also do: bone renaming,
`.L`/`.R` Blender-friendly naming, X-mirror symmetrisation, unit scale, normal-map green flip,
26-bone humanoid reduction, cloth-chain stripping, hardpoints as empties.

So decide explicitly: **does the exporter keep doing these, or does the addon own them?** Doing both
is the failure mode. A defensible split:

- **Exporter keeps** anything needing D4 source data the `.glb` cannot carry (hardpoint offsets,
  cloth authorship, gender pairing, base-body fit reference).
- **Addon owns** anything that is a transform of a generic scene (retarget, decimate, atlas, rename,
  repack) — because those must work on models from *any* source, not just this tool.

Under that split the D4 exporter's retarget options become "export me a clean, unopinionated `.glb`",
and the addon does the target-specific work. Worth confirming with the user.

---

## 4. What the D4 tool hands you: the `.glb`

Written by `src/model/ModelExporter.cpp` via **fastgltf**. Self-contained binary glTF: geometry,
skeleton, skin, inverse-bind matrices, animations, embedded PNG textures.

### Invariants you can rely on

- **Parents precede children in the skeleton array** — enforced by `mergeGeometries`; skinning, rest
  pose and the exporter all assume it.
- Skinned meshes emit `JOINTS_0`/`WEIGHTS_0`, a skin, bone-node hierarchy and `inverseBindMatrices`.
- **D4 is Z-up; the exporter applies the Z-up→Y-up swap** glTF requires. Anim curves are D4-native
  (pre-swap) and converted on the way out.
- Verified against the `d4extract` reference: geometry identical on 5 sample models, skeleton
  matrices to ~1e-16 on a 317-bone character, per-vertex joints/weights identical
  (`docs/MODEL_EXPORT.md`).
- Bone names default to `bone_<hash>`; two optional renamers exist (§5).
- **Only vertex buffer 0 is decoded** — sub-objects on other buffers are dropped rather than drawn
  scrambled. **~10% of wardrobe appearances render incomplete.** Documented limitation of the source
  tool; if parts are missing, that is why, and it is not the addon's bug.

### Materials (`ModelExporter::ExportMaterial`, `src/model/ModelExporter.h`)

```
QString name
bool    doubleSided
bool    alphaCutout;  float alphaCutoff   // → glTF MASK + cutoff (hair / cut-out cloth)
bool    hasMetal;     float metal
bool    hasRough;     float rough
bool    hasEmissive;  float emisR/emisG/emisB, emisMult
QImage  baseColor · normal · orm · emissive
```

**`orm` is the glTF packing: R = occlusion, G = roughness, B = metalness.** Most engines want these
split or packed differently — channel repacking belongs in `core/materials.py`.

**Materials resolve by `primitive.materialIndex`, not by position.** Any subset/merge must renumber
or parts land on the wrong material. This has already caused a real bug.

### Loose textures (2.2.6+)

`Settings ▸ Export ▸ Model export ▸ "Also write textures to a textures folder"` writes
`<model>_<material>_basecolor/_normal/_orm/_emissive.png` into `textures\` beside the `.glb`.
Additive — the `.glb` keeps its embedded copies. Likely what users should enable when a target needs
loose files (e.g. building RE Engine `.tex`).

---

## 5. Exporter settings that change what arrives in Blender

`QSettings` keys; resolved by `ModelExporter::optionsFromSettings()`.

| Key | Effect |
|---|---|
| `retarget/enginePreset` | 0 Custom · 1 Blender · 2 Unreal/Skyrim · 3 Unity. **No RE Engine/DD2 entry — adding one is an obvious deliverable, but only after §6 is known.** |
| `retarget/unitScale` | Multiplies positions / bone + IBM / anim translations. **D4 units are meters.** Custom preset only. |
| `export/reconstructNormalZ` | Rebuild normal B as √(1−x²−y²); D4's BC5 normals decode with B≈0. Default **on**. |
| `flipNormalGreen` (via preset) | OpenGL → DirectX green flip. |
| `export/blenderFriendly` | Bakes glTF-space rotY −90° into vertices, root bones, IBMs, root anim channels → faces −Y, character-left on +X. Prerequisite for X-mirror. |
| `xMirror` | Rewrites each `.R` bone's rest rotation as the exact mirror of its `.L`, rebuilds locals/IBMs, conjugates anim curves. **Verified in Blender 4.2.9** (`D4_XMirror_Spec.md`). |
| `export/boneNamesTranslated` | `bone_<hash>` → readable labels from verified hardpoint/IK data. Overridden by `blenderFriendly`. |
| `export/hardpointEmpties` | Attachment points as empties — from `ItemType.tHardpointOffsets`, **real data, not guessed**. |
| 26-bone reduction · strip cloth chains · fit-reference body | Sharp tools; all change geometry or weighting. |
| `export/exportFxSim` | FX / SIM / FORM submeshes — hidden by default. |
| `export/bothGenders` · `export/includeAnim` · `export/animScope` | Gender pairing; clip embedding. |

---

## 6. RE Engine / DD2 — WHAT IS NOT KNOWN

Do not invent these. None are verified:

- The current best Blender addon for RE Engine meshes (commonly cited: **RE Mesh Editor** by
  NSACloud) — its import/export contract, DD2 mesh versions supported, rig expectations.
- DD2 mesh format version, `.mdf2` material parameter names, `.tex` conversion route, chunk paths.
- The DD2 humanoid skeleton: bone count, names, orientation, rest pose — therefore what a retarget
  from D4's 317-bone rig actually requires.
- Whether DD2 wants DirectX or OpenGL normal green.
- Socket/attachment conventions, and whether D4 hardpoints map onto them at all.
- RE Engine `.chain` cloth vs D4's authored cloth — almost certainly the hardest single problem.

**First job: establish these from a real extracted DD2 asset and the actual tools.** The D4 side
above is solid; this side is not.

---

## 7. Suggested order

1. **Survey**: current CATS/fork status, and the RE Engine addon's contract. Decide what to build vs
   complement.
2. **Establish the DD2 target contract** by importing a real DD2 mesh and recording rig, units,
   axis, normals, materials.
3. **Core skeleton**: `core/` operations + a profile loader, with the DD2 profile as the first.
4. **Then** an RE Engine preset in the C++ exporter, if §3's boundary decision says it belongs there.
5. **Cloth last** — least understood on both sides.

### Working discipline the user expects (carried over — this is what kept the last session productive)

- **Measure, don't assume.** "These bones correspond" gets checked across the whole set with counts
  quoted ("31 of 31") before code depends on it.
- **Instrument before fixing.** Add the diagnostic that proves *where* first. Guess-fixing cost this
  project ten failed attempts on a single bug.
- **A silent zero is a bug.** Anything that can produce nothing must say so — including when the
  answer is legitimately zero.
- **One rule, one place.** Duplicated logic drifts; it produced four separate user-visible bugs in
  one session here.
- Be concise, state confidence honestly, and if a request rests on a wrong premise say so rather
  than building it.

---

## 8. Files worth reading

| File | Why |
|---|---|
| `src/model/ModelExporter.h` | `Options` + `ExportMaterial` — the exact export contract |
| `src/model/ModelExporter.cpp` | glTF writing, axis swap, X-mirror, unit scale |
| `src/model/ModelGeometry.h` | Parser↔exporter contract; skeleton ordering rule |
| `src/tabs/ModelsTab_Export.cpp` | Batch/single export, loose textures, fit reference |
| `src/app/SettingsDialog.cpp` (~1446+) | Experimental tab's retarget box — where a DD2 preset would go |
| `docs/MODEL_EXPORT.md` | Design + what was verified against the reference implementation |
| `D4_XMirror_Spec.md` | X-mirror derivation, verified in Blender 4.2.9 |
| `README.md` | Feature overview + "Honest limitations" |
| `CLAUDE.md` / `STATUS.md` | Project conventions and current state |

Build: `rebuild.bat` (incremental + launch) — **the user rebuilds; an assistant cannot.**
`verify-src.py` runs pre-build checks; `package-release.bat` blocks a release if they fail.
