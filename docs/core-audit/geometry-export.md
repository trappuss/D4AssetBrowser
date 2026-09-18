# Geometry & export audit — D4 / FOX / POE2 asset browsers

Scope: the engine-neutral data model (geometry, rig, clip, material) and the export pipeline
(glb/gltf/obj writers, retarget, capture/GIF, layout/name templates). All paths are relative to
`/home/claude/audit/`. Every claim cites `file:line`; inferences are marked **HUNCH**.

Family authority consulted: `d4/docs/ASSETBROWSER_TEMPLATE.md` §6 (285–294), §15 (456–478),
§26 (748–767), §27 (769–776) and `d4/docs/MODEL_EXPORT.md` (whole file).

---

## 0. One-paragraph orientation

Three exporters, three geometry shapes, **two-and-a-half math conventions that share one memory
layout**. D4 (`ModelGeometry` + `ModelExporter`) is the most featured and most Blender-verified;
POE2 (`ModelGeometry` + `GlbExporter`) is the cleanest engine-neutral contract (one shared vertex
buffer, parts as index ranges, explicit `Options`, `.gltf+.bin`, `KHR_materials_specular`/
`transmission`); FOX has **no neutral geometry type at all** — its exporter consumes the raw parsed
`fox::FmdlFile` plus GL-side structs (`GLPbrMaterial`) and a pose callback, but it has the richest
*scene* model (multi-part, rigid seats, hidden groups, connect points, image de-dup, JPEG,
texture caps, rig reduction, OBJ). The shared-verbatim pieces are `GifEncoder` (byte-identical in
all three) and the GIF budget ladder (three hand-copies of one algorithm). `ExportLayout` and
`NameTemplate` are "mechanism copied, taxonomy re-authored" ports.

---

## 1. The geometry type

### 1.1 D4 — `d4/src/model/ModelGeometry.h`

Consumed by `GLModelWidget::setGeometry(const ModelGeometry&)` (`d4/src/gl/GLModelWidget.h:40`)
and `ModelExporter::exportGlb(const ModelGeometry&…)` (`d4/src/model/ModelExporter.h:86`).

**Vertex** (`MeshVertex`, `ModelGeometry.h:22–38`) — array-of-structs, one struct per vertex:

```
float px,py,pz              // POSITION                       :23
float nx,ny,nz              // NORMAL (unit)                  :24
float u,v                   // TEXCOORD_0                     :25
float cr,cg,cb,ca; bool hasColor      // COLOR_0 = detail-map blend weights (D4 uber shader)  :29-30
float c2r..c2a; bool hasColor1        // COLOR_1 (diagnostic)                                 :31-32
float u1,v1; bool hasUv1              // TEXCOORD_1                                           :33-34
quint16 joints[4]; float weights[4]   // JOINTS_0 / WEIGHTS_0, all-zero ⇒ static              :36-37
```
No tangent. Positions/normals are **already Y-up** — the parser swaps at
`d4/src/model/ModelParser.cpp:372,935,942` (`zUpToYUp`), per MODEL_EXPORT.md §1.5 (73–76).

**Part** (`MeshPrimitive`, `:40–48`): its own `vertices` + `indices` (u32 tri list), `materialName`,
`materialIndex` (appearance roster slot), `subObjectHash`, `slotHash` (bdy/trs/leg/hlm/glv/bts),
`doubleSided`. Each primitive owns its own vertex array (no shared buffer).

**Container** (`ModelGeometry`, `:189–217`): `valid`, `droppedSubObjects/droppedVerts` (audit counters,
`:194–195`), `primitives`, `skeleton` (`QVector<ModelJoint>`, empty ⇒ static, `:197`),
`vertexBuffers` (raw VB layout info, `:198`), `hardpoints` (`:201`), `nBaseBones` (cloth boundary,
`:205`), `clothCapsules`, `clothSims`, `pinnedBones` (`:208–216`). **No bounds field, no LODs**
(LOD0 only — `MODEL_EXPORT.md:5`). D4-only payload: the entire NvCloth block `ClothCapsule` /
`ClothPlane` / `ClothSim` (`:86–174`) — ~90 lines of physics authoring data that no other project
has and that the exporter never reads.

### 1.2 POE2 — `poe2/src/model/ModelGeometry.h`

Consumed by `GLModelWidget::setModel(const ModelGeometry&, const AstSkeleton::Skeleton&)`
(`poe2/src/gl/GLModelWidget.h:34`) and `GlbExporter::write/build` (`poe2/src/model/GlbExporter.h:74,84`).

**Vertex** (`MeshVertex`, `:14–24`):
```
float px,py,pz              // POSITION (NATIVE frame, Z-down)  :15
float nx,ny,nz              // NORMAL                            :16
float tx,ty,tz,tw           // TANGENT xyz + handedness w        :17-18
float u,v; float u2,v2      // TEXCOORD_0 / TEXCOORD_1           :19-20
uint8_t joints[4]; float weights[4]   // JOINTS_0 / WEIGHTS_0    :21-22
uint8_t color[4]            // COLOR_0                           :23
```
**Part** (`MeshPart`, `:28–35`): `name` (Maya DAG leaf), `material` (path), `indexStart/indexCount`
into the **shared** `ModelGeometry::indices`, `materialIndex` into `materialPaths`, `visible`.

**Container** (`:44–70`): ONE `vertices`, ONE `indices`, `parts`, `joints` (a *second* skeleton
representation — see §2), `materialPaths` (dedup'd roster), `bboxMin/bboxMax` (`:50–51`),
provenance (`sourcePath`, `formatVersion`, `vertexFormat`, `skinned`, `:54–57`),
`jointPaletteSize()` (`:63–68`). No LODs, no hardpoints, no cloth.

Coordinates stay native; the single shared rotation `kNativeToYUp` is applied by the viewport and
exporter (`:11–12`; exporter `GlbExporter.cpp:26–30`).

### 1.3 FOX — no neutral type; `fox::FmdlFile` + GL uploads

`modelload::LoadedModel` (`fox/src/preview/ModelLoader.h:68–82`) is the "loaded model":
```
fox::FmdlFile model; QVector<QImage> textures, normalMaps; QVector<GLPbrMaterial> pbr;
QVector<GLMeshUpload> uploads; GLSkeletonUpload skeleton; int texturesFound…
```
The exporter takes `const fox::FmdlFile&` directly (`fox/src/model/GlbExporter.h:131,140–141`).

**Vertex** — structure-of-arrays per mesh (`fox/src/fox/FmdlFile.h:67–85`, `FmdlMesh`):
`positions` (xyz), `normals` (xyz, from half4), `uv0`, `tangents` (xyzw, w = bitangent sign),
`boneIndices` (u8 ×4, **palette-relative**), `boneWeights` (f32 ×4), `triangles` (u16 tri list),
`palette` (u16 → skeleton index), `materialInstanceIndex`, `meshGroupIndex`, `vertexUsages`
(format record). No COLOR, no UV1 (the parser skips usages other than 1/2/7/8/14, `:79–81`).

The **viewport** re-flattens to interleaved `[pos3 normal3 uv2 tangent4]` (`kVertexFloats = 12`,
`fox/src/gl/GLModelWidget.h:34–55`, `GLMeshUpload` with `joints`/`weights`/`materialSlot`/
`groupId`/`meshId`).

**Part** — `FmdlMeshGroup` (`FmdlFile.h:87–93`): `name`, `parentIndex` (groups form a tree),
`visible` (shipped-hidden flag), `nameHash32`. Meshes point at groups; groups do not own vertex
ranges.

**Container** — `FmdlFile` (`:95–134`): `bones()`, `meshes()`, `meshGroups()`, `materials()`,
`version()`, `shippedHiddenGroups()`, `boneSubtreeMask()`. No bounds field (the viewport computes
its own; `GLModelWidget.h:32–33`), no LOD.

Native frame: Y-up. Evidence — the exporter writes positions verbatim (`GlbExporter.cpp:949,961`),
and `zUp` is an opt-in wrapper transform (`:1426–1436`). **HUNCH**: Fox is right-handed Y-up
(nothing swaps or mirrors anywhere in the export path).

### 1.4 Unifiable? Yes — POE2's shape is the base, plus fields from the other two

| Feature | D4 | POE2 | FOX |
|---|---|---|---|
| Vertex layout | AoS per primitive | AoS, one shared buffer | SoA per mesh |
| Position / normal | ✔ / ✔ | ✔ / ✔ | ✔ / ✔ |
| Tangent (xyzw) | ✗ | ✔ `:17–18` | ✔ `FmdlFile.h:71` |
| UV0 / UV1 | ✔ / ✔(+flag) | ✔ / ✔ | ✔ / ✗ |
| Color0 / Color1 | f32 ×4 (+flags) / ✔ | u8 ×4 / ✗ | ✗ / ✗ |
| Joints type | u16 global | u8 global | u8 palette + u16 palette table |
| Index type | u32 | u32 | u16 |
| Parts | own vertex arrays | index ranges into shared buffer | mesh → group tree |
| Material ref | name + roster index + hashes | path + roster index | material-instance index |
| Part visibility | external (viewport/outliner) | `MeshPart::visible` | `FmdlMeshGroup::visible` + hidden sets at export |
| Bounds | ✗ | `bboxMin/Max` | ✗ |
| LODs | ✗ (LOD0) | ✗ | ✗ |
| Double-sided per part | ✔ `:47` | ✗ (per material, default true) | ✗ (always true `GlbExporter.cpp:739`) |
| Hardpoints/sockets | `ModelHardpoint` `:181–187` | ✗ | `fox::ConnectPoint` (separate `.fcnp`, `FcnpFile.h:14–20`) |
| Cloth/physics | ✔ (huge) | ✗ | ✗ |
| Provenance | `droppedSubObjects`, `vertexBuffers` | `sourcePath/formatVersion/vertexFormat` | `version()` |

**Verdict.** A single `core::Geometry` is straightforward: POE2's shared-buffer + index-range parts
is the superset structurally (D4's per-primitive arrays are a degenerate case: concatenate and
record ranges; FOX's SoA flattens to it at load — its own viewport already does this at
`ModelLoader.h:298`). Fields to carry that exist in only one project: TANGENT (POE2/FOX — D4 would
leave it empty; the FOX exporter needs it for `normalTexture`, `GlbExporter.cpp:989–1008`);
COLOR_0/COLOR_1 (D4 — only D4's viewport reads them; **no exporter writes COLOR_0 or TEXCOORD_1**:
grep over the three writers finds no `COLOR_0`/`TEXCOORD_1` emission); UV1 (D4/POE2); per-part
`doubleSided` (D4); `visible` (POE2); group tree (FOX). Joints should be `uint16` global in the core
(D4 already is; POE2's `uint8_t` caps rigs at 256 bones, though its exporter widens to u16 at
`GlbExporter.cpp:117`; FOX's palette indirection is resolved at export at `:1028–1037`). Indices
`uint32` (FOX widens u16 → u32 for free). Cloth stays a D4 side-car keyed by joint index, not a
core field. Hardpoints belong in the core (D4 and FOX both need them; see §5).

One space-convention problem must be resolved at unification (see §2.5): D4's `ModelGeometry` is
**mixed-space** — vertices/IBM/localMatrix are Y-up, `restQ/T/S` and hardpoint `q/t` are
D4-native Z-up (`ModelGeometry.h:66–70,179–186`; parser `ModelParser.cpp:472`).

---

## 2. The skeleton / rig type and math convention

### 2.1 D4 — `ModelJoint` (`d4/src/model/ModelGeometry.h:50–71`)

```
QString name; quint32 nameHash (DJB2, maps anim curves → joints)   :51-52
int parent (-1 root)                                                :53
bool cloth, chain                                                   :59,63
std::array<float,16> inverseBind   // column-major, Y-UP            :64
std::array<float,16> localMatrix   // column-major, Y-UP            :65
restQ (x,y,z,w) / restT / restS    // D4-NATIVE Z-up, pre-swap      :68-70
```
Both bind representations are carried (IBM + local matrix + raw rest TRS). Parent order: **parents
precede children is guaranteed** by `mergeGeometries` (`d4/CLAUDE.md` "Hard code conventions";
`jointWorldMat` walks the parent chain explicitly anyway, `RigMath.h:45–56`).

Math: `RigMath.h` is **column-major, column-vector (M·v)**: `composeTRS` puts translation at
indices 12–14 (`:25–28`), `mat4mul(a,b)` = a·b column-major (`:31–41`), `invertRigid` (`:66–78`),
general `invert` (`:83–109`), `decomposeTRS` (`:116–155`, verified round-trip 3e-15), and the
Z-up→Y-up basis change `kSwapZtoY`/`kSwapYtoZ` (`:159–160`). Quaternion order **x,y,z,w**
(`:21`). Units: metres (`ModelExporter.h:51–53`, unit scale 1.0 default `:71`).

### 2.2 POE2 — two rig types

- `ModelGeometry::joints` → `ModelJoint {name, parent, bindMatrix (row-major, translation row 3),
  inverseBind}` (`poe2/src/model/ModelGeometry.h:37–42`) — the mesh-side palette from `.smd`.
- `AstSkeleton::Bone {name, parent, bind, inverseBind, rawSibling, rawChild}` (`poe2/src/model/AstSkeleton.h:14–20`)
  — the `.ast` rig the exporter and animation actually use (`GlbExporter.h:74`).

Parent order: **not guaranteed** — `skinMatrices` resolves parents recursively with cycle guard
("bones need not be pre-sorted", `AstSkeleton.cpp:241–253`); the exporter wires children by scan
(`GlbExporter.cpp:275–281`). Math: `RigMath.h` is **row-major, row-vector (v·M)**: `mul(a,b)`
`:20–30`, `inverse` (Gauss-Jordan, `:33–55`), `transformPoint` (`:58–63`), `quatToMat` (`:79–92`),
`composeTRS` `v' = v·(S·R)+T` (`:96–104`), `decomposeTRS` (`:109–142`), `quatNlerp` (`:145–152`).
Quaternion xyzw (`:79`). Native frame Z-down, `kNativeToYUp` = (x,y,z)→(x,−z,y) (`:8–9`). Units:
~0.5 cm per unit, default `unitScale = 0.005` (`GlbExporter.h:61–65`).

### 2.3 FOX — `FmdlBone` + `.frig` + `AnimMath`

`FmdlBone {name, qint16 parentIndex, float localPos[4], float worldPos[4], quint32 hash32}`
(`fox/src/fox/FmdlFile.h:27–34`): **translation-only bind pose** — no rotation, no matrix. Parents
precede children (`:29`, `:17–18`; `FmdlFile.cpp:709`). The rig *rules* (rig units → bones, IK) live
in a separate `fox::FrigFile` resolved by `rigbind::loadFrigFor` (`fox/src/anim/RigBind.h:21`).

Math: `animmath` (`fox/src/anim/AnimMath.h`) is **row-major, row-vector, System.Numerics
semantics** (`:1–9`): `Mat4 { float m[4][4] }` translation in row 3 (`:47–80`), `transform(v,M)` =
v·M (`:83–90`), `mul(A,B)` applies A then B (`:104–112`), `invertRigid` (`:118–128`), `quatMul` =
System.Numerics operator* (`:131–143`), `rotate(v,q)` (`:158–164`), `Quat {x,y,z,w}`
(`fox/src/fox/GaniAnim.h:24–26`). The header forbids swapping in `QMatrix4x4` (`:8–9`).
Inverse bind for export is derived as `translate(-worldPos)` (`GlbExporter.cpp:581–597`). Units:
metres (scale 1.0 default, `GlbExporter.h:47`).

### 2.4 Are D4 `RigMath.h` and POE2 `RigMath.h` the same convention?

**No — but their memory layouts are identical, which is the fact that matters for a core.**

- Both store 16 floats with translation at `[12],[13],[14]` (D4 `RigMath.h:28`; POE2 `:102`;
  FOX `m[3][0..2]` `AnimMath.h:53–56`).
- Rotation element placement is identical: D4 `composeTRS` puts `r10` at index 1 (`:25` — column 0
  = r00,r10,r20); POE2 `quatToMat` puts `2(xy+wz)` = r10 at index 1 (`:87`); FOX `fromQuat` puts
  `2(xy+wz)` at `m[0][1]` = index 1 (`:68`).
- Therefore D4's column-major `M` and POE2/FOX's row-major `Mᵀ` are the **same bytes**. The only
  difference is the multiply: D4 `mat4mul(a,b)` (`:37`, `o[c*4+r] = Σ a[k*4+r]·b[c*4+k]`) equals
  POE2 `mul(b,a)` (`:26`) byte-for-byte; FOX `mul(A,B)` equals POE2 `mul(a,b)`.
- Consequence: glTF's column-major IBM/`matrix` arrays can be written straight from either
  storage — D4 does so (`ModelExporter.cpp:677`), and the POE2 scar comment explains exactly this
  (`GlbExporter.cpp:284–291`: "storing it in ROW-MAJOR element order… glTF then reads it
  column-major as M_rowᵀ"). FOX states the same for its static connect-point matrix
  (`GlbExporter.cpp:919–926`).

What FOX's `AnimMath` does *differently*: it is a **C# port** (System.Numerics semantics), uses a
`float m[4][4]` struct instead of `std::array<float,16>`, has no general inverse, no
compose/decompose TRS (bones are translation-only so it never needed them), adds `rotate(v,q)`,
`fromTo`, `perpendicularTo`, and `quatFromMatrix` (`AnimMath.h:167–172`) which the exporter uses to
recover local rotations from world matrices (`GlbExporter.cpp:1172`).

### 2.5 Consequences for a core `Skeleton`

- One `Mat4 = std::array<float,16>` with translation at 12–14 works for all three today; the core
  must pick ONE multiply convention and document it (**HUNCH**: keep POE2/FOX row-vector `mul(a,b)`
  = "a then b", since two of three projects and the CPU-skinning code use it; D4's `mat4mul` is the
  same function with swapped arguments).
- Store per joint: `name`, optional `nameHash` (D4 DJB2 / FOX StrCode32; POE2 none), `parent`,
  **bind-local TRS in the core's canonical Y-up frame** plus the payload `inverseBind`. D4 must
  convert `restQ/T/S` from Z-up at ingest (today the exporter swaps per bone with `swapTRS`,
  `ModelExporter.cpp:28–38`); FOX fills `restQ = identity`, `restT = localPos`; POE2 derives local
  = `bind·inverse(parentBind)` then `decomposeTRS` (exactly what its exporter does at
  `GlbExporter.cpp:261–266`).
- The core must either **require parents-precede-children** (sort at ingest — cheap, and D4's
  skinning, overlays and exporter already assume it) or resolve recursively like POE2. Recommend
  sorting at ingest and asserting.
- D4-only per-joint flags `cloth`/`chain` and `nBaseBones` stay as a D4 side-car or as optional
  core flags (they gate D4's `Retarget::collapseClothChains`).

---

## 3. The animation clip type and how each exporter writes it

### 3.1 D4 — `AnimParser::DecodedAnim` (`d4/src/model/AnimParser.h:16–29`)

`DecodedBone {boneHash; per-frame translations (x,y,z) "absolute local"; rotations (x,y,z,w)
D4-native; scales}` + `DecodedAnim {frameRate=30, frameCount, compression, bones, valid}`.
Uniformly sampled, one key per frame, curves bound to joints **by name hash**. Empty channels fall
back to the rig's rest pose via `RestTRS` (`:31–42`).

Writer (`ModelExporter.cpp:691–807`): one glTF animation per clip; per bone up to three channels
(rotation/translation/scale) sampled at `1/fps` (`:714,731`); time accessor **cached per
(count,fps) per clip** (`:717–739` — "954 byte-identical float arrays" scar); LINEAR
interpolation (`:743`); every key goes through `swapTRS` (Z-up→Y-up), optional Blender yaw on root
bones (`:753–756,766,782`), X-mirror conjugation `q' = C_p⁻¹⊗q⊗C` (`:756–758,767–772,783`), unit
scale on translations (`:784`). Clip names come from a parallel `QStringList` (`:802–804`).
Which clips: `AnimExportScope` five sources (`d4/src/util/AnimExportScope.h:43–49`) +
`AnimClipFilter` (max frames / exclude substrings, `AnimClipFilter.h:22–24`), gathered by
`ModelsTab::collectExportAnims` (`d4/src/tabs/ModelsTab.cpp:7992–8015`). Animation-library export
(rig + clips, no mesh) is the `animLibrary` branch (`ModelExporter.cpp:256,281,814`).

### 3.2 POE2 — `AstSkeleton::Clip` (`poe2/src/model/AstSkeleton.h:22–34`)

`KeySet {nodeId; scaleTimes/scale; rotTimes/rot (xyzw); posTimes/pos}` — **sparse, per-channel
key times in frame units** (`AstSkeleton.cpp:214` "keyframe times are in frame units"), bound by
**bone index**; `Clip {name, parentName, fps=30, offset, size, keys}`; `Skeleton::clipsDecoded`
gate (`:41`).

Writer (`GlbExporter.cpp:346–396`): one glTF animation per clip, `onlyClip` filter (`:350`);
per KeySet a sampler per non-empty channel, **times ÷ fps** (`:360`), min/max on the time accessor
(`:364–366`), values rotated by `R` (`:378,382–384`), scale axis-swapped (`:388`), translation
× unitScale (`:378`), LINEAR (`:374`). No time-accessor sharing (each channel writes its own).

### 3.3 FOX — `glb::GlbAnimation` (`fox/src/model/GlbExporter.h:83–108`)

Not a clip type at all: `{name, fps=30, sampleCount, std::function<void(int part,int sample,
QVector<Mat4>* out)> pose}` — the exporter **bakes** by calling the pose solver
(`animpose::buildWorld`, `fox/src/anim/AnimPose.h:134–138`) once per sample, sample-major
across parts (`:98–107`). The underlying decoded clip is `fox::GaniAnim {frameCount, tracks}` with
per-track channels (`fox/src/fox/GaniAnim.h:61–103`) that drive *rig units*, not bones — which is
why baking is the only faithful export (`GlbExporter.h:86–91`).

Writer (`GlbExporter.cpp:1131–1271`): world → local via `world[b]·invertRigid(world[parent])`
(`:1168–1171`), `quatFromMatrix` (`:1172`), **one shared time accessor per clip** (`:1187–1191`),
rotation + translation only — **no scale channel** (`:1108–1110`), **constant tracks dropped**
(tracks equal to rest within 1e-6 are not written, `:1112–1115,1222–1246`), clips keyed by NAME so
one clip driving several parts is one glTF animation (`:1193–1202`; assembled at `:1443–1463`),
channels re-targeted through the rig reduction (`:1247–1255`), and a clip whose pose is sized for a
different skeleton is refused (`:1162–1165,1179–1184`). Root-motion in-place is a process-wide
switch consulted by viewport and export alike (`AnimPose.h:106–116`).

### 3.4 Unifiable core `Clip`

All three end up as **per-joint TRS key arrays in seconds** at the glTF boundary. A core clip of
`{name, fps, duration, tracks[joint] {times[], t[], q[], s[]}}` with sparse per-channel times covers
POE2 natively, D4 as dense (times = i/fps), and FOX as baked-dense. Two core policies to lift from
the projects: **time-accessor sharing** (D4 per (count,fps), FOX per clip) and **constant-track
elision** (FOX). Binding: D4 by hash, POE2 by index, FOX by index-after-reduction — the core should
bind by joint index and let the D4 adapter resolve hashes at ingest.

---

## 4. The material type per project, and glTF extensions

### 4.1 D4 — `ModelExporter::ExportMaterial` (`d4/src/model/ModelExporter.h:23–36`)

`name, doubleSided, alphaCutout+alphaCutoff(0.35), hasMetal/metal, hasRough/rough, hasEmissive +
emisR/G/B + emisMult, QImage baseColor / normal / orm (R=AO,G=rough,B=metal) / emissive`.
Filled by `buildExportMats` (`d4/src/tabs/ModelsTab.cpp:800–929`) from `MaterialValues`
(`d4/src/model/Material.h:17–23`) + roles `BASE_COLOR/NORMAL/ROUGHNESS/METALLIC/AO/EMISSIVE/
DYE_MASK/DYE_RAMP` (`ModelsTab.cpp:870–885`), with dye and detail-grain baking (`:906–923`).
No SSS, no specular colour, no transmission. **HUNCH**: the Models-tab path never sets
`alphaCutout` (only `StableTab2.cpp:2393` and `WardrobeTab2.cpp:9640` do), so Models-tab hair
exports OPAQUE.

Emission: written only when an emissive texture exists (`ModelExporter.cpp:384–403`), factor =
authored colour else white, HDR multiplier → `KHR_materials_emissive_strength` (`:399–401`);
`extensionsUsed` declared conditionally (`:537–540,822`). ORM feeds both `metallicRoughnessTexture`
and `occlusionTexture` with factors forced to 1 (`:336–346`). Normal map: Z reconstruction
(`:349–364`) and DirectX→OpenGL green flip **default ON** (`:365–371`, `ModelExporter.h:55–67`),
guarded by `normalConventionSelfTest` (`ModelExporter.cpp:908–971`). alphaMode MASK only
(`:380–383`); no BLEND. Only one KHR extension.

### 4.2 POE2 — `GlbExporter::ExportMaterial` (`poe2/src/model/GlbExporter.h:19–46`)

`name, baseColor, normal, metallicRoughness (ORM), emissive, specularColor, doubleSided=true,
alphaCutout, alphaBlend, alphaMode (0 Opaque/1 Mask/2 Blend/3 Additive), alphaCutoff=0.5,
metalFactor, roughFactor, emissiveStrength, hasOcclusion/occlusionStrength, dielectricSpec,
transmissionFactor, subsurface[3] (viewport-only), isFur/furNoise/furMask/furDepth (viewport-only)`.
Filled by `AssetStore::resolveMaterials` (`poe2/src/store/AssetStore.cpp:385–428`) from the
authored `.mat` workflow (MetalRough / DielectricSpecGloss / SpecGloss) and `Force*` alpha graph.

Writer (`GlbExporter.cpp:158–239`): PNG embed or loose sibling files (`:164–185`);
`KHR_materials_emissive_strength` (`:214`), `KHR_materials_specular` with optional
`specularColorTexture` (`:217–226`), `KHR_materials_transmission` (`:229–232`); `extensionsUsed`
collected in a set (`:159,338–341`); MASK **and** BLEND (`:235–236`; Additive → BLEND). This is the
only exporter writing three KHR extensions and the only one carrying an explicit alpha-mode enum.

### 4.3 FOX — `GLPbrMaterial` (`fox/src/gl/GLModelWidget.h:75–320`) + `FmdlMaterialInstance`

The exporter consumes the **viewport's** PBR struct: `material` (SRM: R spec mask, G roughness,
B reflection mask, `:59–66`), `translucent` (TRM), `layer/layerMask` (runtime colour), `subNormal`,
eye maps, glass params, `presetF0[4]/presetSpec/presetTrans/presetAniso` from `.fmtt`
(`:177–189`), layer/sub-normal tiling, incidence rim, hair params, `noMetal`, `unlit` — some 40
fields, most viewport-only. Authored source: `FmdlMaterialInstance {name, nameHash32, shader,
textures[] {role, roleHash32, pathHash, path}, params[] {name, value[4]}}`
(`fox/src/fox/FmdlFile.h:36–65`).

Writer (`fox/src/model/GlbExporter.cpp:734–821`): always `doubleSided` (`:739`); base map with the
colour layer **baked through its mask** (`:750–757`, `bakeLayerColour` `:227`); MASK at 0.35 when the
base has alpha (`:758–762`); SRM → ORM (`buildOrm` `:304–320`: G=roughness, R and B neutral 255 —
no occlusion slot, `:778–785`); metalness as a scalar eased from the FMTT F0 (`:787–798`); normal map
green-flipped to +Y unless `normalsGreenDown` (`:799–817`). **No KHR extensions at all**, no
emissive, no BLEND. Image de-dup by content hash (`:610–699`), JPEG for opaque base maps
(`:672–691`), size cap (`:652–656`).

### 4.4 Unifiable core `Material`

The three agree on the glTF-facing shape: name, doubleSided, alphaMode(+cutoff), metal/rough
factors, baseColor/normal/ORM/emissive images, emissive factor+strength. POE2 adds
specularColor/dielectricSpec/transmission/subsurface; D4 adds `hasX` presence flags and
dye/detail bakes (adapter-side); FOX adds occlusion-less ORM, F0→metal easing, colour-layer bake
(adapter-side) and image post-processing (JPEG/cap/de-dup — **core**-side, they are engine-neutral).
Normal-map convention handling exists in D4 (`flipNormalGreen`, `reconstructNormalZ`) and FOX
(`normalsGreenDown`) and POE2 (`reconstructNormalZ`) under three names — the core needs one
`NormalConvention {sourceGreenDown, reconstructZ}` pair.

---

## 5. Exporter comparison

### 5.1 Feature matrix (✔ with citation; ✗ absent)

| Feature | D4 `ModelExporter` | POE2 `GlbExporter` | FOX `glb::exportGlbScene` |
|---|---|---|---|
| Self-written GLB (no lib) | ✔ `ModelExporter.h:10–12`, container `cpp:829–842` | ✔ `GlbExporter.h:9–10`, `cpp:409–418` | ✔ `GlbExporter.h:7–8`, `cpp:1480–1495` |
| `.gltf` + `.bin` | ✗ | ✔ split from built GLB `cpp:441–467` | ✗ |
| Embed textures | ✔ PNG only `cpp:289–310` | ✔ PNG, or loose siblings for .gltf `cpp:164–185` | ✔ PNG/JPEG, size cap, content de-dup `cpp:610–713` |
| Loose textures beside file | ✔ `textures\` folder, `ModelsTab_Export.cpp:212,685` | ✔ `looseTextures` `.h:59` | ✗ (OBJ writes PNGs beside `.mtl`) |
| TANGENT attribute | ✗ | ✔ `cpp:88–98` | ✔ only when normal maps on `cpp:996–1008` |
| Skeleton + skin + IBM | ✔ `cpp:543–689` | ✔ `cpp:248–308` | ✔ `cpp:530–608` |
| Skeleton off | ✗ (always if skinned) | ✔ `includeSkeleton` `.h:49` | ✔ `SceneOptions::skeleton` `.h:51` |
| Joints component | u16 `cpp:487` | u16 `cpp:126` | u8 or u16 by kept bone count `cpp:1020` |
| Animation | ✔ per clip, dense `cpp:691–807` | ✔ per clip, sparse `cpp:346–396` | ✔ baked via pose callback `cpp:1131–1271` |
| Animation selection | 5-source scope + filter + list multi-select (`AnimExportScope.h`, `AnimClipFilter.h`, `ModelsTab.cpp:7992`) | all / current-clip / none (`onlyClip`, `.h:50–53`; dialog `ExportOptionsDialog.h:36`) | caller passes the clip vector; per-part dropping `cpp:1370–1382` |
| Animation library (rig + clips, no mesh) | ✔ `cpp:254–258,281,814`; UI `ModelsTab_Export.cpp:1296` | ✗ | ✗ |
| Attachments merge | ✔ `ModelAttach::seat` (bake, 100 % weight) / `attachSubRigAt` (keep rig, salted hashes) / `seatMount` (`Attachments.h:60–136`) | ✔ `AssetStore::assembleForExport` bakes at bone bind, 100 % weight (`ModelsTab.cpp:691`; `poe2/CLAUDE.md` "INCLUDE ATTACHMENTS") | ✔ multi-part scene: `ScenePart::rigid` seat, `restOffset` wrapper, fragment rigs borrow host palette (`GlbExporter.h:140–190`, `cpp:1330–1354`) |
| Part subsets | ✔ visible ∧ camera-toggle, or explicit keep list (`ModelsTab_Export.cpp:601–617`) | ✔ `MeshPart::visible` from viewport subset (`ModelsTab.cpp:693–695`; writer skips `cpp:143`) | ✔ hidden groups + hidden meshes + bone-subtree cut (`prepareMesh` `cpp:327–435`) |
| Material renumbering on subset (§6) | ✔ implicit — materials created on first use, keyed `idx:N` (`cpp:314–323,410–412`) | ✗ **writes the whole roster** incl. unused materials and embeds their images (`cpp:160–238`) | ✔ `usedMaterials` set then `materialGlbIdx` remap (`cpp:723–736,819,1078–1081`) |
| Unit scale | ✔ multiplies positions/bone T/IBM T/anim T (`cpp:263–265,440,565,577,668,784`) | ✔ same approach (`cpp:59,65,264,295,378`), default 0.005 (`.h:65`) | ✔ **wrapper root node matrix** (`cpp:1414–1437`) |
| Yaw / orientation | ✔ Blender yaw −90° baked into verts/roots/IBM/anim (`cpp:40–88,439,563,673`) | ✔ `yaw180` wrapper node (`cpp:316–324`) | ✔ `zUp` wrapper (same node as scale, `cpp:1432–1434`) |
| Hardpoints / sockets as empties | ✔ `ModelHardpoint` → child nodes; authored = model-space socket → `IBM·socket` local (`cpp:590–643`) | ✗ | ✔ `.fcnp` connect points as empties, bone-local pass-through; static branch folds pose (`cpp:838–933`) |
| Retarget presets | ✔ Blender / Unreal-Skyrim / Unity / Custom (`optionsFromSettings` `cpp:973–1001`) + `.L/.R` naming, X-mirror symmetrization (`cpp:90–211`), `Retarget::collapseClothChains / remapToAnchors` (`Retarget.h:17,28`) | ✗ (scale + yaw + normalZ only) | ✔ `readableBoneNames`, `mirrorBoneNames`, `reduceRig`, `normalsGreenDown` (`GlbExporter.h:71–80`; rules in `Retarget.h`); user presets `ExportOptions.h:264–281` |
| Posed static snapshot | ✔ `GLModelWidget::snapshotPose` (`d4/src/gl/GLModelWidget.h:241`) | ✗ | ✔ `pose` bakes into vertices, no skeleton (`cpp:99–152,472–475`) |
| OBJ | ✗ | ✗ | ✔ `obj::exportObjScene` sharing `prepareMesh` (`ObjExporter.h:35`) |
| Options resolved from settings | `optionsFromSettings()` (`.h:84`) | `exportOptionsFromConfig()` (`ExportConfig.h:9–20`) + at-export dialog | `sceneOptionsFrom(ExportOptions)` (`ExportOptions.h:138`), one converter for all call sites |
| Self-test | normal-convention (`cpp:908–971`) | one-triangle GLB round trip (`cpp:475–493`) | ✗ in exporter (harness flags elsewhere) |
| Export log line | via `ExportNotifier::glbOptionsLine` (`ModelsTab_Export.cpp:704`) | status signal | `qInfo` summary with counts/ms (`cpp:1511–1518`) |

### 5.2 The hard-won scars — who encodes which

| Scar | D4 | POE2 | FOX |
|---|---|---|---|
| **IBM storage order** (row-vector matrix must be written row-major so glTF reads Mᵀ) | Not needed: IBM already column-major in memory, written raw (`ModelGeometry.h:64`, `cpp:677`). Non-issue by construction. | **Encoded explicitly with the war story** (`cpp:284–296`: "bind deviation 2.6 m → 0"). | Translation-only IBM, order-agnostic (`cpp:590–596`); the same insight is stated for the static connect-point matrix (`cpp:919–926`). |
| **TRS vs `matrix` on animated nodes** | Encoded conditionally: `matrix` when no clips, TRS when animated (`cpp:546–581`). Animation-free exports still ship `matrix` nodes. | **Always TRS**, with the Blender 4.4-vs-5.0 story (`cpp:250–256`). | Always TRS (`translation` only, `cpp:549–560`); never writes a bone `matrix`. |
| **Accessor/bufferView assignment after the animation block** (QJsonArray is a value type) | Correct by ordering (`cpp:812–813` after the anim block at `:694–807`) — **no comment**; the static branch re-assembles separately (`:846–858`). | **Encoded explicitly** (`cpp:333–336, 398–400`). | Correct by structure: `BinBuilder` arrays assigned at the very end (`cpp:1470–1471`, after `appendAnimations` at `:1380`) — no comment. |
| **Time accessor sharing** | per (count,fps) per clip, with the 954-array story (`cpp:717–739`) | ✗ one per channel (`cpp:359–366`) | one per clip (`cpp:1186–1191`) |
| **Blender-verified** | X-mirror in Blender 4.2.9 (`cpp:90–96`), hardpoint bind pose (`:593`), yaw derivation from hardpoint data (`:41–44`), `MODEL_EXPORT.md:184–191,270–272` | real Blender import + strict solver (`cpp:291`); `.gltf`/assembled/single-clip imports (`poe2/CLAUDE.md` "EXPORT OPTIONS") | Retarget.cpp mentions Blender's X-mirror naming (`Retarget.cpp:94`); **no Blender-verification claim** found in the exporter itself. |
| Normal-map green convention measured | ✔ (`ModelExporter.h:55–67`, self-test) | reconstructZ only | ✔ measured (`cpp:799–811`; `fox/NormalMap.h`) |
| Empty animation is invalid glTF | not guarded (a clip with no matching hashes is skipped: `:800`) | skipped (`:392`) | guarded + logged (`cpp:1446–1454`) |
| Joint used with zero weight | zeroed (`cpp:477–484`) | index clamped, weights renormalised (`cpp:117–120`) | palette resolve + renormalise, reduction mismatch counted (`cpp:1024–1055,1502–1507`) |
| "Everything hidden ⇒ nothing to write" | ✔ (`ModelsTab_Export.cpp:618–623`) | ✔ (`cpp:156`) | ✔ (`cpp:1384–1393`) |

### 5.3 Which exporter should be the core base?

**Recommendation: POE2's `GlbExporter` as the skeleton, with D4's rig transforms and FOX's scene
and image machinery grafted on.** Reasons:

1. POE2 already consumes a neutral `ModelGeometry` + separate `Skeleton` + `ExportMaterial` +
   `Options` — the exact shape a core needs — and is the only one that builds in memory
   (`build()` `GlbExporter.h:84`), splits `.gltf+.bin`, writes loose textures, emits three KHR
   extensions and BLEND, and documents both the IBM-order and accessor-ordering scars in code.
2. D4's writer is entangled with D4 semantics (`swapTRS` Z-up bones, `AnimParser::DecodedAnim`,
   `SymBone` X-mirror, hardpoint "authored means model-space" rule) and has two duplicated container
   tails (`cpp:809–843` vs `:846–879`).
3. FOX's writer consumes `fox::FmdlFile` directly (`cpp:437–451`) and would need the geometry
   adapter written first, but its `SceneAccum` design (`cpp:154–190`) is the right *shape* for
   multi-part export.

What must be added to POE2's base from the others (each a self-contained port):

- From **D4**: animation-library mode (`cpp:254–258`); per-primitive `doubleSided`
  (`ModelGeometry.h:47`); time-accessor cache (`cpp:717–739`); hardpoint empties with the
  `IBM·socket` rule (`cpp:590–643`); unit-scale conjugation of anim translations (already in POE2)
  and IBM translations; the retarget layer (`Retarget.h`, X-mirror `cpp:90–211`, yaw `:40–88`,
  `optionsFromSettings` presets `:973–1001`, `flipNormalGreen` + self-test `:898–971`);
  `AnimExportScope`/`AnimClipFilter` as generic "which clips" policy objects.
- From **FOX**: `usedMaterials` pruning (`cpp:715–736`) — POE2 currently embeds images for
  invisible parts' materials; image de-dup + JPEG + size cap (`cpp:610–713`); wrapper-node
  scale/up-axis (`cpp:1414–1437`) as an alternative to baked scale; constant-track elision and
  per-clip shared time accessor (`cpp:1112–1115,1186`); `prepareMesh` as the single
  "what survives" function shared with OBJ (`cpp:322–435`); the OBJ writer; `reduceRig`
  (weight-based, `Retarget.h:67–79`); connect-point "seed parents into keep set" rule
  (`cpp:484–502`); u8/u16 joint width choice (`cpp:1020`); the multi-part `SceneAccum` with
  animations keyed by name (`cpp:154–190,1193–1202`); the end-of-run `qInfo` summary (`cpp:1511`).

---

## 6. ExportLayout / NameTemplate / ExportCapture / GifEncoder / ViewCapture

### 6.1 `GifEncoder` — byte-identical ×3

`d4/src/gl/GifEncoder.{h,cpp}` = `poe2/src/gl/GifEncoder.{h,cpp}` exactly (diff: only the
`#include` path on the .cpp). `fox/src/export/GifEncoder.h` adds a 12-line provenance note
(`:2–13`: "LIFTED, UNCHANGED… If a bug is found here, fix it in BOTH trees") and is otherwise
identical; the `.cpp` differs only in include path. Pure C++ (`std::vector`, `std::string`), no Qt
(`GifEncoder.h:2–4`). **This is already a core library file; it just needs one home.**

### 6.2 GIF budget ladder — three hand-copies of one algorithm (template §26)

| | D4 `app/ExportCapture.cpp` | POE2 `app/ExportCapture.cpp` | FOX `export/ViewCapture.cpp` |
|---|---|---|---|
| ladder: palette ×¾ to 32 → dither off → aimed downscale √(target/actual)·0.93, ≤5 passes, 96 px floor | `:177–250` (`:218,224–227,231–238`) | `:122–176` (`:149,162`) | `:263–370` (`:314,318,328`) |
| ship smallest, "TARGET NOT REACHABLE" | `:244–249` | `:173` | `:367` |
| options source | QSettings read inside (`:180–189`) | explicit `GifOptions` struct (`ExportCapture.h:20–30`) | explicit `CaptureOptions` (`ExportOptions.h:159–205`) |
| frames captured once, retries re-encode | ✔ `encodeToBuffer` | ✔ | ✔ |
| turntable snap to whole clip loops | ✔ `:364` | ✔ `:223` | **HUNCH** ✗ (no `snap` in `ViewCapture.cpp`; turntable is a camera revolution only) |
| cloth warm-up lap / settle steps | ✔ `:380–399,468–477` (D4-only: physics) | ✗ (time-based `setAnimTime`, `:252–258,293–297`) | ✗ |
| transparent capture | native-alpha single render (`:32–41`) + coverage alpha for crop (`:51–66`) | `renderToImage(scale, transparentBg, crop)` (`:182–184`) + `compositeOverBg` (`:104`) | 1-bit via `CaptureOptions::transparent` (`ExportOptions.h:179`) |
| still image 25–400 % re-render, png/jpg/webp | `saveImage` `:261–333` | `ImageExportDialog` + `renderToImage` (POE2 `app/ImageExportDialog.*`) | `captureStill` + `imageScale/imageFormat` (`ExportOptions.h:196–204`) |
| refuses when frames won't fit memory | ✗ | ✗ | ✔ (`ViewCapture.h:51–54`) |
| interactive dialogs / context-menu / clipboard | ✗ (tabs own it) | dialogs in `app/GifExportDialog` | ✔ `captureStillInteractive`, `captureTurntableInteractive`, `captureAnimationInteractive`, `copyViewportToClipboard`, `installViewportContextMenu` (`ViewCapture.h:65–135`) |

**Superset:** FOX `ViewCapture` for structure (explicit options struct, interactive wrappers,
memory guard, clipboard, one `encodeGif` shared by turntable and animation `ViewCapture.h:81–85`);
POE2 for the cleanest engine-neutral capture API (a `GifOptions` value + a viewport interface of
`renderToImage`/`orbitYaw`/`setAnimTime`); D4 for the physics-aware frame stepping which is
game-specific and should stay behind a hook. The core should own **one** `encodeWithBudget`
(three copies today, all behaviourally identical) plus `pushFrame`/`downscaleFrames`/
`cropFramesToModel` (D4 `:76–170`, POE2 `:16–101` "ported verbatim").

Game-specific bits: D4 `settleCloth`/`setCaptureMode`, `grabCoverage`/`setCoverageAlpha`;
POE2 `compositeOverBg` (its transparent capture clears to black, `:102–104`); FOX `partmenu`
integration and `setExportSettingsOpener` (`ViewCapture.h:31–42`).

### 6.3 `ExportLayout.h` — mechanism ported, taxonomy re-authored (template §15)

| | D4 `util/ExportLayout.h` (161 l) | POE2 (145 l) | FOX (243 l) |
|---|---|---|---|
| key | `export/folderLayout` stable string (`:31–41`) | same (`:35–40`) | same (`:45–57`) |
| modes | Flat / `Class` / `Type` / `_model` — AppearanceMeta tag groups by SNO (`:38–41,111–116`) | Flat / `Type` (ext bucket) / `Folder` (mirror game dir) / `_model` (`:37–40,82–109`) | Flat / `game` / `category` / `family` (ModelTags ids) / `_model` (`:53–57`), with a `modes()` table carrying labels+hints (`:62–81`) |
| fail-to-Flat on unknown id | ✔ `:56–59,80` | ✔ `:44–53` | ✔ `:98–112` |
| legacy `bulk/organize` index migration | ✔ `:61–78` | ✗ | ✗ (never shipped one, `:24–27`) |
| sanitizer (trailing dots, reserved names) | in `modelFolder` only (`:86–103`) | `sanitizeSegment` applied to every segment (`:64–79`) | `sanitizeFolder` applied to EVERY folder incl. tags (`:114–144`, with the "aux/con tag" bug story) |
| `_misc` bucket, never loose | ✔ `:115` | ✔ `:97,102,108` | ✔ `:185,213` |
| API | `group(mode, items(sno,name))` → `Group{folder, items}`, `folderFor` (`:125–159`) | per-item `subfolderFor` / `dirFor` (`:91–116`) | `group()` + per-file `folderForFile` (`:197–241`) |
| self-test | ✗ | ✔ `selfTest()` `:120–143` | ✗ |
| entry-point rule stated | ✔ `:14–19` | ✔ `:17–18` | ✔ `:11–18` |

Superset of the *mechanism*: FOX (universal sanitizer, modes table with labels, both group and
per-file APIs) + POE2's self-test + D4's migration branch. The core should carry `sanitizeSegment`,
`isKnown/mode/setMode`, `_misc` rule, `Group` splitting over an abstract `folderKeyFor(item)`
callback; the taxonomy (AppearanceMeta / path tree / ModelTags) stays per-project.

### 6.4 `NameTemplate` — D4 ≡ POE2 (identical file); FOX is the superset

`d4/src/util/NameTemplate.h` and `poe2/src/util/NameTemplate.h` are **byte-identical**, including
the D4-isms `{{SNO}}`, `{{FrameIdx}}`, `{{FrameName}}` and the three QSettings keys
`export/nameTexture|nameModel|nameFrame` (`:28–44`). POE2 has **no Settings UI and no caller**
for those keys — its only use is `NameTemplate::apply("{{FileName}}", stem, 0)` in
`poe2/src/bulk/BulkExtractor.cpp:168` (grep finds no `export/nameModel` anywhere else in
`poe2/src`). **HUNCH**: dead template machinery in POE2, copied for §15 conformance.

FOX (`fox/src/export/ExportOptions.h:210–262`, impl `ExportOptions.cpp:171–235`) has `{{Name}}`,
`{{Game}}`, `{{Hash}}` (16-hex), `{{Frame}}` (auto-padded to the run's width, `:194–201`),
`{{Part}}`, `{{Clip}}`, `{{Date}}`, unfilled-placeholder removal with separator-run tidy
(`:208–216`), `simplified()` before the trailing-dot / reserved-name checks with the " CON " story
(`:217–235`), `nameTemplatePlaceholders()` for docs/harness (`.h:249`), and `templatedStem`
so every path names files the same way (`.h:251–262`). Deliberately no `{{FrameName}}`
(`.h:226–229`).

Core: FOX's engine with placeholder names generalised (`{{Name}}`/`{{Id}}` per template §15;
`{{SNO}}` is the D4 alias for `{{Id}}`).

### 6.5 Export options objects

Three different homes for the same decisions: D4 `ModelExporter::Options` (5 fields) + ~20 loose
QSettings keys read in `ModelsTab_Export.cpp` (`export/includeBaseBody`, `export/withDeps`,
`export/hardpointEmpties`, `export/bothGenders`, `retarget/*`, …); POE2 `GlbExporter::Options`
(9 fields) + `Config` accessors + at-export dialog; FOX `fox::ExportOptions` (19 fields incl. bulk
conversion choices) → `glb::SceneOptions` via one converter (`ExportOptions.h:134–138`), plus
named presets serialisable as `k=v|k=v` text (`.h:264–281`). FOX's "one converter, one describe()
line in the log" is the pattern to keep.

---

## 7. Minimal core interfaces implied

Legend: **[D4] [FOX] [POE2]** = which project(s) populate/need the field. Types are sketches derived
from the real fields; Qt containers kept for continuity with all three code bases.

```cpp
namespace core {

using Mat4 = std::array<float,16>;   // 16 floats, translation at [12..14] — the layout all three
                                     // already use (d4 RigMath.h:25-28, poe2 RigMath.h:102,
                                     // fox AnimMath.h:53-56). Multiply convention: row-vector
                                     // v·M, mul(a,b) = "a then b" (poe2/fox); d4's mat4mul(a,b)
                                     // == mul(b,a).   HUNCH: choose row-vector.
using Quat = std::array<float,4>;    // x,y,z,w in every project

// ── Vertex + geometry ────────────────────────────────────────────────────────
struct Vertex {
    float px, py, pz;                // [D4][FOX][POE2]  canonical Y-up, right-handed, metres?
                                     //   NOTE units: D4 m, FOX m, POE2 ~0.5 cm (scale at export)
    float nx, ny, nz;                // [D4][FOX][POE2]
    float tx, ty, tz, tw = 1;        // [FOX][POE2]  D4 leaves default; exporter emits only if hasTangent
    float u, v;                      // [D4][FOX][POE2]
    float u2, v2;                    // [D4 hasUv1][POE2]   (no exporter writes it today)
    uint8_t color[4] = {255,255,255,255}; // [D4 f32→u8][POE2]  (no exporter writes it today)
    uint16_t joints[4] = {};         // [D4 u16][FOX palette→global][POE2 u8→u16]
    float    weights[4] = {};        // all three; all-zero ⇒ static
};

struct Part {
    QString  name;                   // [FOX group name][POE2 DAG leaf][D4 subObjectHash→name]
    uint32_t indexStart, indexCount; // [POE2] native; [D4] concatenated; [FOX] flattened
    int      materialIndex = -1;     // all three (roster index)
    bool     visible = true;         // [POE2][FOX shipped-hidden]; D4 applies externally
    bool     doubleSided = false;    // [D4]; FOX always true; POE2 per material
    int      groupParent = -1;       // [FOX] mesh-group tree (D4/POE2: -1)
    uint32_t nameHash = 0;           // [D4 subObjectHash][FOX nameHash32]
    uint32_t slotHash = 0;           // [D4] wardrobe slot (bdy/trs/…)
};

struct Geometry {
    QVector<Vertex>   vertices;      // shared buffer  (POE2 shape)
    QVector<uint32_t> indices;       // u32 tri list (FOX widens u16)
    QVector<Part>     parts;
    QStringList       materialNames; // roster: [D4 palette names][POE2 materialPaths][FOX instance names]
    float bboxMin[3], bboxMax[3];    // [POE2] stored; D4/FOX compute (core computes at ingest)
    bool  hasTangent, hasUv1, hasColor;  // attribute presence (D4's hasColor/hasUv1 flags)
    // provenance / audit (optional but all three have some):
    QString sourcePath; int formatVersion; // [POE2]
    int droppedParts; unsigned droppedVerts; // [D4]
    // NOT core: D4 cloth (ClothSim/ClothCapsule/ClothPlane/pinnedBones), D4 vertexBuffers.
};

// ── Skeleton ─────────────────────────────────────────────────────────────────
struct Joint {
    QString  name;                   // all three
    uint32_t nameHash = 0;           // [D4 DJB2][FOX StrCode32]; POE2 0 — clips bind by index
    int      parent = -1;            // all three; core guarantees parents precede children
    Quat  restQ; float restT[3]; float restS[3];   // bind-LOCAL TRS in canonical frame
                                     //   [D4 restQ/T/S swapped from Z-up at ingest]
                                     //   [FOX q=identity, t=localPos, s=1]
                                     //   [POE2 decomposeTRS(bind·inv(parentBind))]
    Mat4  inverseBind;               // payload IBM in canonical frame [D4 y-up col-major][POE2 R·inv·R⁻¹][FOX translate(-world)]
    bool  cloth = false, chain = false;  // [D4] physics markers (drive collapseClothChains)
};
struct Skeleton {
    QVector<Joint> joints;
    int  nBaseBones = 0;             // [D4] cloth boundary; 0 = unknown
    QString note;                    // [POE2] "bones only (version 9 clips not decoded)"
};

// ── Hardpoint / socket (empties) ─────────────────────────────────────────────
struct Hardpoint {
    QString  name;                   // [D4 "HP_*"][FOX "CNP_*"]
    int      joint = -1; uint32_t jointHash = 0;   // [D4 hash survives reorder][FOX parentBone hash]
    Quat q; float t[3]; float s[3] = {1,1,1};     // [D4 q/t][FOX pos/quat/scale]
    bool modelSpace = false;         // [D4] authored ≠ identity ⇒ socket is MODEL-space (IBM·socket);
                                     // [FOX] always bone-local (bind is translation-only)
};

// ── Animation clip ───────────────────────────────────────────────────────────
struct Track {                       // one per animated joint, sparse per channel (POE2 shape)
    int joint;                       // [POE2 nodeId][D4 hash→index at ingest][FOX bone index]
    QVector<float> tTimes; QVector<std::array<float,3>> t;   // seconds
    QVector<float> qTimes; QVector<Quat>                q;
    QVector<float> sTimes; QVector<std::array<float,3>> s;   // [D4][POE2]; FOX never writes scale
};
struct Clip {
    QString name;                    // all three
    float   fps = 30;                // [D4 frameRate][POE2 fps][FOX 30]
    float   duration = 0;            // derived
    QVector<Track> tracks;
    bool    baked = false;           // [FOX] sampled from a solver, dense
};
// Policies the core owns: shared time accessor per (count) [D4][FOX]; drop constant tracks [FOX];
// clip selection expressed as a filter over Clip names/lengths [D4 AnimExportScope/AnimClipFilter],
// "current clip only" [POE2 onlyClip].

// ── Texture image + material ─────────────────────────────────────────────────
struct TextureImage {
    QImage  image;                   // decoded RGBA (all three)
    QString sourcePath;              // [FOX *_Source strings][POE2 .mat role path][D4 texName/SNO]
    bool    srgb = true;             // [FOX baseSrgb]
    bool    lossyOk = false;         // [FOX] JPEG allowed (opaque base maps only)
};
struct Material {
    QString name;                    // all three
    bool    doubleSided = false;     // [D4 per part][POE2 default true][FOX always true]
    enum AlphaMode { Opaque, Mask, Blend } alphaMode = Opaque;  // [D4 Mask][POE2 Mask/Blend/(Additive→Blend)][FOX Mask if alpha]
    float   alphaCutoff = 0.5f;      // [D4 0.35][POE2 0.5][FOX 0.35]
    float   metalFactor = 0, roughFactor = 1;       // [D4 hasMetal/hasRough][POE2][FOX eased F0]
    TextureImage baseColor, normal, orm, emissive;  // all three (FOX orm = SRM-derived, no AO)
    bool    ormHasOcclusion = false; // [D4 true][POE2 hasOcclusion][FOX false]  → occlusionTexture
    float   occlusionStrength = 1;   // [POE2]
    float   emissiveFactor[3] = {1,1,1}; float emissiveStrength = 1;  // [D4 emisRGB/emisMult][POE2]
    // KHR extensions (POE2 today; core emits when set):
    TextureImage specularColor; bool dielectricSpec = false;   // [POE2] KHR_materials_specular
    float   transmission = 0;        // [POE2] KHR_materials_transmission
    // normal-map convention of the SOURCE (so the core can flip/reconstruct once):
    bool    normalGreenDown = false; // [D4 DirectX measured][FOX green-down measured][POE2 ?]
    bool    normalNeedsZ = false;    // [D4 BC5][POE2 RG]
    // viewport-only (NOT emitted): [POE2 subsurface/fur], [FOX layer/SRM/eye/glass/hair/…]
};

// ── Export options (what every writer honours) ───────────────────────────────
struct ExportOptions {
    bool includeSkeleton = true;     // [POE2][FOX]; D4 always
    bool includeAnimations = true;   // [POE2][D4 Anim tick]; FOX by clip vector
    bool animationLibrary = false;   // [D4] rig + clips, no mesh
    bool embedTextures = true; bool looseTextures = false; // [POE2]; [D4 textures\ folder]
    int  jpegQuality = 0; int maxTextureSize = 0;          // [FOX]
    float unitScale = 1;             // [D4 1/100][POE2 0.005][FOX scale]
    enum ScaleMode { BakeIntoData, WrapperNode } scaleMode; // [D4/POE2] vs [FOX]
    enum UpAxis { YUp, ZUp } upAxis = YUp;                  // [FOX zUp]
    float yawDegrees = 0;            // [D4 −90 Blender yaw][POE2 180]
    bool flipNormalGreen = true; bool reconstructNormalZ = true; // [D4][FOX normalsGreenDown][POE2]
    bool hardpointEmpties = false;   // [D4][FOX connectPoints]
    bool readableBoneNames, mirrorBoneNames, reduceRig, xMirrorSymmetrize; // [FOX][FOX][FOX][D4]
    bool tangents = true;            // [FOX only-with-normal-maps]
    QString generator;               // "D4AssetBrowser" / "POE2AssetBrowser" / "FOXAssetBrowser"
};

}  // namespace core
```

Explicitly **not** core (stay in adapters): D4 cloth sim/capsules/planes, D4 dye/detail bakes,
D4 attachment seating (`ModelAttach`), D4 anchor remap (26 curated hashes), FOX colour-layer bake,
FOX FMTT/F0 easing, FOX `.frig`/IK pose solver, POE2 `.ao` assembly, all three taxonomies for
`ExportLayout`, and each project's QSettings key names (the core takes value structs).

---

## Appendix — things worth flagging to the other audit areas

- D4 `ModelGeometry` is a mixed-space contract (Y-up vertices/IBM, Z-up rest TRS and hardpoints,
  `ModelGeometry.h:66–70,179–186`). Any core ingest must swap once and assert one frame.
- POE2 embeds textures for every material in the roster even when the part using it is hidden
  (`GlbExporter.cpp:160–238`) — FOX's `usedMaterials` (`:723–736`) is the fix.
- POE2 `MeshVertex::joints` is `uint8_t` (`ModelGeometry.h:21`) — a >256-bone rig cannot be
  represented before export widens it (`GlbExporter.cpp:117`).
- D4 Models-tab export never sets `alphaCutout` (§4.1 HUNCH); Stable/Wardrobe tabs do.
- POE2 ships D4's `NameTemplate.h` verbatim with no settings UI and one literal-template call
  (`BulkExtractor.cpp:168`).
- `GifEncoder` ×3 is byte-identical apart from includes and a FOX provenance comment; the budget
  ladder ×3 is a hand-copied algorithm with identical constants (¾, 32, 0.93, 0.35–0.92, 96 px).
- D4's bone-renaming lives in the GL widget (`GLModelWidget::blenderizeSkeletonNames /
  translateSkeletonNames`, `d4/src/gl/GLModelWidget.h:404–405`) — a rig operation misplaced in
  the viewport; FOX's equivalent is in `model/Retarget.h:106`.
- Only D4 writes `matrix` on bone nodes (non-animated exports, `ModelExporter.cpp:569–581`);
  POE2's scar says some importers mishandle it when clips are added later — moot for D4 today
  because the choice is made per export, but a core writer should follow POE2/FOX and always TRS.
