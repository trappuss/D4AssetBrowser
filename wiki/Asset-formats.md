# Diablo IV asset formats — how the data is actually put together

A working reference for anyone reading, ripping or re-implementing Diablo IV assets, and
for anyone doing the same on another game built on the same storage stack.

**Provenance rule for this page.** Every figure here was measured, and it says where. Counts
marked *(measured)* were counted directly from the `CoreTOC.dat.json` of a current retail
build while writing this page — you can reproduce each one in a few lines of Python. Facts
marked *(project measurement)* come from this tool's own verification notes, which record the
sample size that established them. Nothing here is inferred from a wiki, a forum post or
another extractor's source, and where something is genuinely unknown it says so rather than
guessing. That distinction matters more than it sounds: several confident-looking claims in
circulation about this game's data are wrong, and two of them are called out below.

---

## 1. The shape of the whole thing

Diablo IV does not ship asset files. It ships a **content-addressed archive** with a virtual
filesystem laid over it, and every asset is identified by a number.

### CASC, TVFS and BLTE

- **CASC** is Blizzard's content-addressed storage. Data lives in `.data` archives; `.idx`
  files map content keys to an archive and an offset. Nothing is stored under a human path.
- **TVFS** is the virtual filesystem manifest that gives those blobs paths. It is a tree of
  nodes with a `0x80000000` folder bit and a 31-bit size mask on each node.
- **BLTE** is the per-file container inside the archive: a `BLTE` magic followed by frames,
  each independently compressed and optionally **encrypted**. A file can be partly readable —
  frames decode independently, so a truncated or key-less archive fails frame by frame rather
  than all at once.

You need all three to get bytes. There is no step where a normal file appears on disk.

### The SNO model

Every asset is a **SNO** — a numeric id, unique game-wide, belonging to a **group** that says
what kind of thing it is. An appearance, a texture, a material, an animation and an item are
all just SNOs in different groups.

Each SNO has up to two files in the virtual filesystem:

```
base/meta/<sno>       the record: fields, references to other SNOs
base/payload/<sno>    the bulk data: vertices, pixels, animation curves
```

Some assets also use `base/paylow/<sno>` for a low-detail payload, and `base/child/<sno>-<idx>`
for split payloads.

**The meta/payload split is the single most useful thing to understand.** The meta blob is
small and structured and tells you what an asset references. The payload is large and opaque
and holds the actual content. An asset can have a perfectly readable meta and an unreadable
payload — that is exactly what an encrypted texture looks like, and it is why "the record
exists" and "I can see the pixels" are different questions.

### CoreTOC — the name table

`base/CoreTOC.dat` maps every SNO to a name. Parsed, it is:

```json
{ "<groupId>": { "<sno>": "<name>" } }
```

**Measured on a current build: 133 groups, 862,731 records.** This is the index everything
else hangs off — but read section 11 before trusting a name.

---

## 2. Groups — what the numbers mean

CoreTOC carries group **ids**, never group **names**. The mapping below was established by the
project the only honest way available: for each `json/base/meta/<Folder>` in the community
snapshot, look every file stem up in CoreTOC and accept the owning group only where every
unambiguous stem agrees. All 23 folders the snapshot carries come back unanimous.

Record counts are *(measured)* from a current retail build, with the number of records whose
name is blank (encrypted — see section 11) beside it:

| id | group | records | unnamed | id | group | records | unnamed |
|---|---|---|---|---|---|---|---|
| 1 | Actor | 61,499 | 1,173 | 115 | MarkingShape | 374 | 70 |
| 6 | Anim | 45,681 | 89 | 118 | Emote | 171 | 10 |
| 8 | AnimSet | 11,915 | 10 | 119 | Jewelry | 33 | 0 |
| 9 | Appearance | 67,733 | 570 | 121 | Emblem | 117 | 24 |
| 11 | Cloth | 15,481 | 330 | 122 | Dye | 91 | 2 |
| 44 | Texture | 145,563 | 3,660 | 131 | EyeColor | 18 | 0 |
| 57 | Material | 102,751 | 1,048 | 132 | Makeup | 23 | 0 |
| 73 | Item | 12,859 | 1,130 | 133 | MarkingColor | 98 | 5 |
| 98 | ItemType | 154 | 0 | 134 | HairColor | 32 | 0 |
| 37 | ShaderMap | 3,633 | 2 | | | | |
| 110 | StoreProduct | 9,310 | 1,746 | 138 | HairStyle | 28 | 0 |
| | | | | 139 | FacialHair | 18 | 0 |
| | | | | 140 | Face | 5 | 0 |
| | | | | 152 | AppearanceSet | 20 | 0 |

The other 109 groups hold world geometry, effects, UI, quests, audio and so on. Several are
very large — group 24 has 68,323 records and group 40 has 61,555 — and none of them are named
here because **this project has not verified them, and unverified is not the same as wrong.**
Inventing names for the rest is exactly the mistake that produced the trap below.

### Group 37 is ShaderMap — identified here, and it corrects a live assumption

*(measured while writing this page)* Group **37** has been treated by at least one tool (this
one) as a possible second Material group, on the grounds that it is not empty and nothing had
established what it holds. It is not materials. It is the **shader map** group — the thing a
material's `tUberMaterial.snoShaderMap` points at.

The test is decisive. Every shader name this project classifies submeshes by is present in
group 37 and absent from group 57:

| shader | in group 37 | in group 57 |
|---|---|---|
| `hero_hair` | yes | no |
| `hero_opaque_skin` | yes | no |
| `hero_opaque_alphatest_skin` | yes | no |
| `Hero_Eye` | yes | no |
| `hair_pbr_igc` | yes | no |
| `hero_opaque_hollow` | yes | no |

Group 37 holds **3,633** records, **zero** of which end in `_mat`, including 75 names starting
`hero_` and 377 starting `vfx`. Group 57 holds 102,751 records, 21,202 ending in `_mat`.

So the group table gains a verified row — **37 = ShaderMap** — and any material-name lookup
built over both groups is mixing 3,631 shader names into a material table. Harmless most of
the time, wrong on a name collision, and now unnecessary.

This is included as a worked example of the method in section 13: the previous position was
the *correct* one to hold — "not empty, nothing established" is honest — and it took four
lines of Python against real data to replace it with an answer.

### The group-name trap *(project measurement)*

If you build a name→id lookup table, **the table can be wrong and will override a correct
hard-coded value.** Labels that looked plausible and were not: id 36 (actually holds
`minimap_marker`) labelled Cloth; id 37 (`2D_prims_transparent`) labelled Material; id 93
(`Scosglen_Corbach`) labelled ItemType; id 78 (`Scos_Ruin_Wall_B`) labelled Emblem; id 85
(`DrownRocks`) labelled Jewelry.

One of those cost a full debugging session: a lookup returned 123 for "MarkingShape" instead
of 115, so a scan walked 468 territory records instead of 374 body markings and found nothing.
The symptom was an empty list, which is indistinguishable from "this class has no markings".

The lesson generalises past this game: **a data-driven lookup is only as good as its table.**
When a lookup replaces a constant, the table behind it needs its own measurement.

---

## 3. Directories and file types

Two places hold this data, and they are not equivalent.

### The game install — authoritative, numeric, partly encrypted

CASC storage. Paths are numeric (`base/meta/<sno>`). Always current with the installed build.
Some content is encrypted and needs TACT keys.

### The community JSON snapshot — readable, named, always behind

`DiabloTools/d4data` is a decoded dump of the meta records as JSON, laid out by group name:

```
json/base/meta/<Group>/<name>.<ext>.json     the records
json/enUS_Text/meta/StringList/              localised strings
json/base/CoreTOC.dat.json                   sno -> name, all groups
json/base/EncryptedSNOs.dat.json             sno -> group + TACT key
json/base/CoreTOCSharedPayloadsMapping.dat.json   payload deduplication
```

Extensions by type:

| ext | group | ext | group | ext | group |
|---|---|---|---|---|---|
| `.app.json` | Appearance | `.acr.json` | Actor | `.msh.json` | MarkingShape |
| `.mat.json` | Material | `.ani.json` | Anim | `.mcl.json` | MarkingColor |
| `.tex.json` | Texture | `.prd.json` | StoreProduct | `.hcl.json` | HairColor |
| `.clt.json` | Cloth | `.itm.json` | Item | `.har.json` | HairStyle |
| `.stl.json` | StringList | `.itt.json` | ItemType | `.fhr.json` | FacialHair |
| `.fac.json` | Face | `.jwl.json` | Jewelry | `.mak.json` | Makeup |
| `.eye.json` | EyeColor | | | | |

**The snapshot lags the game, and the gap is where new content lives.** Measured on
MarkingShape: the game ships 374, the snapshot describes 304. Of the 70 missing, 13 are named
(the Berserk *Brand of Sacrifice* set, one per class) and 57 are encrypted.

The practical consequence is a rule: **prefer failing closed over substituting.** If your
pipeline reads only the JSON snapshot, an asset that is merely newer than the snapshot is
indistinguishable from an asset that does not exist. Read the game's own binary tables as a
fallback and the difference becomes visible.

There is a second version of the same trap. If you use a sparse checkout of the snapshot, a
group you never downloaded looks **exactly** like "the game ships nothing here". Three
categories in this tool were empty for three releases for that reason, and it read as "this
class has no options".

---

## 4. Naming grammar

Character equipment follows `<class><gender>_<variant>_<SLOT>`:

```
barF_stor189_LEG      Barbarian female, store variant 189, legs
palF_sets50_TRS       Paladin female, set 50, torso
```

*(measured)* Over 67,163 named appearance records, the trailing token on names matching that
shape resolves almost entirely to five armour slots:

| slot | records | | slot | records |
|---|---|---|---|---|
| `HLM` helm | 1,890 | | `BTS` boots | 1,820 |
| `TRS` torso | 1,830 | | `LEG` legs | 1,796 |
| `GLV` gloves | 1,824 | | `HED` head | 11 |

Everything else in that position is a handful of one-off tokens (`BLACKSMITH`, `INVISIBLE`,
`SMOKESOURCE`). Five slots carry essentially all of it.

*(measured)* Class codes, with their appearance counts: `bar` 1,460 · `sor` 1,421 · `rog`
1,404 · `nec` 1,389 · `dru` 1,243 · `spi` 924 · `pal` 682 · `war` 614. `npc` also appears.
The canonical order used by the game's own class enum is `sor dru bar rog nec spi pal war`,
which matters because several records carry per-class arrays indexed in exactly that order.

Other patterns *(measured)*:

| pattern | meaning | count |
|---|---|---|
| `<family>_base<NN>` | base rig / default body | 334 (314 with digits, 20 without) |
| `<class><gender>_P<NN>` | face mesh | 64 |
| `<class><gender>_H<NN>` | hair mesh | 309 |
| `<class><gender>_B<NN>` | beard mesh | 74 |
| `jwl<NN>_<class><gender>` | jewelry mesh | 512 |

**Zero-padding is not consistent across groups** *(project measurement)*. Animation stem
`trophy_glo12_stor_idle` belongs to appearance `trophy_glo012_stor`. Any cross-group join on
names must normalise digit runs or it silently drops rows.

### Names are a convenience, not an identity

This is the most important thing on this page and the hardest habit to break.

Name-based selection fails in this data in three distinct ways, each of which has produced a
real bug:

1. **A prefix that looks right selects the wrong things.** Back trophies are named
   `trophy_<class><NN>_stor`, *not* `back_*`. Scanning for `back_*` returns six placeholder
   proxies and zero real trophies *(project measurement)*.
2. **A substring matches an unrelated word.** A test for `head` in a material name matches
   `wolfHead`, an ornament on a Paladin pauldron. In this tool that classified a chest piece
   as the character's face and hid the whole torso. `brow` is inside `browplate`; `lash` is
   inside `backlash`.
3. **There is no name at all.** Encrypted records reach CoreTOC blanked.

**Select by reference or by shader, never by filename.** Where a record points at another
record — an item's `snoItemType`, a material's shader — that pointer is authored data and
survives renames. A name is a label.

---

## 5. The reference graph

This is what actually connects things. Every arrow is a SNO reference in a meta record.

```
Item ──snoItemType──> ItemType
  │
  └──snoActor──> Actor ──snoAppearance──> Appearance
                                              │
                          ptAppearanceMaterials[].ptSOAs[]
                                              │
                               ┌──────────────┴──────────────┐
                          snoMaterial                    snoCloth
                               │                             │
                               v                             v
                           Material                        Cloth
                               │
                    tUberMaterial.ptMatTexList[]
                               │
                        tMatTex.snoTex
                               │
                               v
                           Texture
```

An **Appearance** is the renderable unit: mesh payload plus an ordered material roster. Each
submesh carries a material *index* into that roster, so the roster's **order is load-bearing**
— get it wrong and every submesh gets someone else's material, with no error anywhere.

Each roster entry is a `ptSOAs[]` list from which you take, in preference order, an override
material, then the base material, then the cloth reference. An entry that resolves only to
cloth is a simulation cage, not a visible surface.

### Two traps in this graph *(project measurement)*

**`Actor.snoAppearance` is a drop proxy about half the time.** Measured over 400 items, 219
resolve to a `*_flippy` ground-drop model (`Armor_flippy`, `Item_Saddle_Drop_flippy`) rather
than the transmog you wanted. Guard the Item→Actor→Appearance route with a check that the
appearance's name relates to the item's own.

**Appearance→AnimSet is many-to-many and explodes.** The mapping accumulates the sets of every
actor referencing that appearance, and a shared armour piece is worn by actors game-wide.
Expanding it raw lists tens of thousands of foreign clips.

---

## 6. Materials and shaders

A material record's `tUberMaterial.snoShaderMap.name` is **the reliable way to ask what a
submesh is.** Names are per-class and drift; shaders do not.

| shader | means |
|---|---|
| `hero_opaque_skin`, `hero_opaque_alphatest_skin` | head / body skin |
| `hero_hair` | all real hair *and* facial hair, including class-signature beards |
| `hair_pbr_igc` | eyelashes — deliberately **not** hair; do not fold these together |
| `Hero_Eye` | eyes |
| `hero_opaque_hollow` | one specific placeholder (see section 10) |
| `vfx*` | effect submeshes |

Two things this buys you that a name test cannot:

- **Rogue and Sorcerer name their eyeball material `Hero_eyes_mat`** instead of the usual
  `global_eyeball_mat`, so a name test misses them. The shader catches all of them.
- `global_eyeball_mat` ships with an **empty texture roster** — the shader supplies the maps.

Which leads to a trap worth stating on its own: **"empty texture roster" is not a placeholder
test.** Paladin's face alone carries five empty-roster materials — eyeball, eyelashes,
eyeshadow, facial hair and mouth — all of them legitimate *(project measurement)*.

*(measured)* 101,703 named material records; 21,202 end in `_mat`. The convention is common,
not universal — another reason not to key on it.

---

## 7. Textures

### Codecs

D4 ships block-compressed textures. This tool decodes **BC1, BC3, BC4, BC5 and BC7**.

### BC5 normal maps have an implied Z

BC5 stores **two** channels. A normal map in BC5 therefore has no blue channel at all — the
game reconstructs it at render time as `nz = sqrt(1 - x² - y²)`.

This is why an exported PNG can light wrong in every DCC while the in-game and in-tool preview
look correct. If you export the decoded bytes as-is, B is zero.

**Filling B with white is not equivalent** *(project measurement)*. With X/Y unchanged and the
DCC renormalising, Z = 1 flattens the relief — measured at 4.2% of the tilt lost on average
and 23.6% on the steepest 1% of texels. Compute the real value.

Gate this on the **codec**, never the file name. A BC5 texture that is a packed two-channel
mask has no implied Z and must not be touched.

### Normal maps are DirectX convention

Green points toward the **bottom** of the texture.

This is *(project measurement)* and the method is worth copying, because it is the part people
get wrong. It was measured on `barM_P00_BOD_normal` from three features with known anatomy,
straight off the channel with no integration: on a convex bump the upper half carries the
higher G under OpenGL and the lower G under DirectX. Both nipples read −15.3 / −13.4, both
pectoral mounds −5.3 / −4.0, and the navel read as a pit at +5.6. The same statistic run on a
synthetic bump of each convention returns +14.4 / −14.4 — so the sign is established, not
assumed.

Consequences:

- glTF mandates OpenGL-convention normal maps, and Blender is OpenGL. **Exporting to glTF
  means flipping green.** Only a DirectX target wants the channel as decoded.
- **Do not judge this by eye.** Perceptual bump/hollow flip is unreliable and reversed the
  answer twice during the session that measured it. Use the above-minus-below statistic on a
  feature you know is convex.
- **A mirrored-UV seam fix cannot validate the global sign.** Flipping the bitangent on
  mirrored islands makes both sides of a symmetry line agree, whichever one is right, so a
  global inversion survives it untouched.

There is **no per-material "flip green" parameter** — the convention is global.

### Texture roles

A material's texture list is `tUberMaterial.ptMatTexList[]`, each entry carrying an
`eShaderTex` slot number and a `tMatTex.snoTex` reference. The slot number is the role:

| slot | role | slot | role |
|---|---|---|---|
| 1, 11, 13, 19 | BASE_COLOR | 97 | NOISE_PROCEDURAL |
| 3, 47, 48 | NORMAL | 104 | TRANSLUCENCY |
| 54 | DYE_MASK | 108 | DYE_MASK_2 |
| 56 | DYE_RAMP | 145 | SKIN_MASK |
| 62, 112, 113 | ROUGHNESS | 212, 213, 214 | DETAIL_NORMAL |
| 63 | METALLIC | 218, 219, 220 | DETAIL_ROUGHNESS |
| 81 | AO | 234, 235, 236 | secondary colour / freckle / vitiligo |
| 86 | EMISSIVE | 96 | MASK_PRIMARY |

Texture names follow the same roles *(measured, most common suffixes over 141,903 named
textures)*: `_Normal` 13,898 · `_Color` 11,406 · `_color` 11,143 · `_Rough` 10,385 ·
`_normal` 9,727 · `_AO` 9,694 · `_Metal` 5,586 · `_Mask` 5,346 · `_DyeMask` 3,205 ·
`_DyeRamp` 3,132 · `_Emissive` 3,324 · `_FurMask` 2,397.

Note the **casing is inconsistent** — `_Color` and `_color` are both common. Any name match
here must be case-insensitive, which is a good argument for using the slot number instead.

### Detail maps are where the surface actually comes from

The game layers a tiling detail normal and roughness over the base maps **per dye zone**.
Fabric weave, leather grain and scale texture live there, not in the base maps. A pipeline
that exports only the base maps produces something that looks flat next to the same asset
in-game, and the cause is not obvious from looking at either map alone.

### Shared payloads — a texture that "resolves" may be a placeholder

*(measured)* `CoreTOCSharedPayloadsMapping.dat.json` holds **34,919** entries mapping one
asset's payload to another's. The most-reused targets are revealing:

| reuses | target |
|---|---|
| 3,064 | `Texture/RenderTest_Black_Metal.tex` |
| 2,077 | `Texture/roughness_default.tex` |
| 1,909 | `Texture/zmap_Sanctuary_Eastern_Continent_00_00.tex` |
| 1,887 | `Texture/OpaqueWhite.tex` |

So thousands of textures with distinct names and SNOs are deduplicated onto a handful of flat
defaults. **A texture reference resolving successfully does not mean the asset authored a
texture** — it may be pointing at shared black, shared white or a default roughness. If you
are auditing what content actually exists, resolve through this table first.

---

## 8. Icons and atlases

Inventory and shop icons are packed into atlas textures. Two pieces are needed to get one out:

- **`base/Misc/2D_table.dat`** — a 16-byte header then 12-byte records
  `{u32 hImageHandle, u32 atlasSno, u32 frameIndex}`. This ships in CASC and needs no
  snapshot, which makes it the only source for atlases newer than the snapshot.
- **The texture's own `ptFrame[]`** — the per-frame UV rectangles. `2D_table.dat` does **not**
  carry rectangles.

*(project measurement, over the 2,635 multi-frame atlases both sources describe)*

| claim | result |
|---|---|
| `2D_table` frameIndex == position in `ptFrame[]` | **2,635 / 2,635 — unanimous** |
| row-major spatial sort == `ptFrame[]` order | 740 / 2,635 — 28.1% |
| … restricted to `2DUI_Bundle_*` | 162 / 1,125 — **14.4%** |

The ordinal is completely reliable. What is **not** reliable is recovering a rectangle for an
undescribed atlas by segmenting the image and pairing rectangles to handles by position:
row-bucketing with tolerance recovers exactly zero additional atlases, so it is not float
drift — the packer's order simply is not spatial.

**Do not build that heuristic.** On `2DUI_Bundle_*`, where the newest store and collaboration
art lands, it would put the wrong icon on the card six times in seven. A card showing the
wrong item is worse than a card showing nothing.

---

## 9. Cloth and simulation

Garment simulation data lives in a ClothData block inside the appearance payload:

| field | semantics |
|---|---|
| `bindVerts` | sim-cage particle rest positions, bind space |
| `invMasses` | per particle; **0.0 = pinned/kinematic** |
| `constraintIdx` / `constraintLen` | distance constraints: index pairs plus rest lengths |
| `ptAttachmentLengths` | **normalized 0..1** (0 = locked to the skinned pose, 1 = free) |
| `ptPlaneDefs` | plane colliders: stiffness, friction, bone index |
| `ptTriangles` | cage topology |
| `name` (32 bytes) | keys `Cloth/<name>.clt.json` **or** `Cloth/<name>_sim.clt.json` |

That attachment-length field is worth dwelling on: it is **normalized, not a world distance**
*(project measurement)*. Comparing it directly to world-space drift is meaningless — multiply
by the garment's own reach first. It is also only present when its size matches the vertex
count; otherwise it is empty, and a consumer must not default it to "free".

*(measured)* 15,151 named Cloth records; **13,047 end in `_sim`** and 2,889 contain
`override` — the game ships per-situation overrides (high quality, sitting, evading) as
separate cloth definitions.

Sim cages are low-poly driver proxies of roughly 120 triangles, have no material or shader,
are authored deliberately looser and larger than the visible garment, and **must never render
by default**. Authored body-collision capsules are likewise larger than the visible body; this
tool found 0.52 to be the verified visual scale, and 1.0 splays garments open.

---

## 10. Skeletons, hardpoints and animation

### Merging

Multi-piece outfits merge skeletons keyed by **bone-name hash**. Armour appends its own cloth
bones past the base rig; `nBaseBones` marks the boundary, and player gear leaves it unset, so
the first piece is the base rig.

**D4 authors cloth chains in chain order, not hierarchy order** *(project measurement)* — a
bone's parent index can point *forward*. Any consumer that assumes parent < child needs a
parents-first normalisation pass first.

### Hardpoints

Attachment sockets live at `tStructure.ptBoneData[0].ptHardpoints`, each entry carrying a name
hash, a bone index, an ignore-parent-orientation flag, and a transform (quaternion plus
position). Verified hashes:

| hash | socket | hash | socket |
|---|---|---|---|
| 3636304447 | `HP_rightWeapon` | 585752080 | `HP_leftBackSheath` |
| 4036545548 | `HP_leftWeapon` | 274763203 | `HP_rightBackSheath` |
| 899481535 | `HP_chestBack` | 982636814 | `HP_trophy1` (mounts) |
| 2366464462 | `HP_chest` | | |

Per-class orientation offsets live on the **ItemType** record as
`tHardpointOffsets.arMaleOffsets` / `arFemaleOffsets`. This is why weapons that seat correctly
on a Barbarian sit wrong on every other class if you ignore them.

**Back trophies author no attach hardpoint at all** *(project measurement)* — the link hash is
0 on every one, and the CosmeticBack ItemType's offsets are empty for all eight classes. The
socket has to come from the body rig instead.

### Animation — four traps, each of which produced a 20,000-row bug

1. **Filter set-expanded clips to the model's own family.** See section 5.
2. **An unresolvable family must expand to nothing, never to everything.** A guard that
   disables itself when it cannot decide fails exactly when it is needed.
3. **Skeleton-overlap matching must be symmetric** — Jaccard `intersection/union`, never
   `intersection/min(|A|,|B|)`. A 20-bone armour skeleton is a subset of every humanoid rig
   and scores ~1.0 against all of them under min-normalisation.
4. **A clip's own bone list is authoritative.** Decoding a clip against an arbitrary skeleton
   is a valid way to ask "whose bones does this drive?", because the decoded target list is
   the clip's own regardless of what you decoded it against.

### Mount and pet clips do not follow the appearance name

*(project measurement, from 67,733 Appearance and 45,681 Anim records)*

- Only **8** Anim records start with `mnt_`, and all eight are FX. Real mount clips are
  prefixed by the **rider**: `MERC_Shieldbearer_HTH_mount_horse_nav_gallop`,
  `NPC_mount_chimera_nav_idle`. A filter keyed on `mnt` finds almost nothing.
- Companions are named **two ways at once**: stem-named `cmp_stor100_murloc_idle` (28 of 48
  companion appearances) and species-named `CMP_dogLarge_nav_idle`, shared by every
  `*_dogLarge` variant. The species family carries no variant token at all.
- **The right key for both is the species** — the trailing name token with digits stripped.
- **Which appearance owns the clips differs by family, and the two are opposites.** A pet's own
  appearance is the right first answer. A mount's is not: a mount skin owns only FX extras
  under its own name and plays the base rig's set, so "prefer its own" hands you a wing-flap
  instead of thirty gaits.
- A species token is short, so **match it on `_` segment boundaries, never as a bare
  substring**: unanchored, `deer` takes every reindeer clip (40 vs 34), `crab` 91 vs 21, `cat`
  365 vs 236.

---

## 11. Encryption — what "missing" usually means

Some content ships encrypted under **TACT keys**, and the keys are not distributed with the
game. This is the single most common reason an asset appears absent.

*(project measurement)* **14,101 encrypted SNOs across 197 TACT keys.** Community key
collections cover a fraction — this tool holds 9, covering 2,593.

Three things follow, and they compound:

1. **Encrypted records reach CoreTOC with the name blanked.** The record is there; the name is
   not. *(measured)* 3,660 Texture, 1,746 StoreProduct, 1,173 Actor, 1,130 Item, 1,048
   Material and 570 Appearance records are nameless on a current build.
2. **Every name-shaped roster therefore cannot see them.** If your slot filter is
   `startsWith("barf") && endsWith("_trs")`, a nameless appearance cannot appear anywhere,
   even with the key.
3. **A name lookup that returns empty is not the same as an asset that does not exist** — and
   if a downstream consumer re-derives the SNO from that empty name, it gets 0 and silently
   does nothing. Carry the SNO alongside the name and fall back to it.

### Recovering names

Two routes, both verified:

**The game's own name dictionaries — authoritative.** The game ships **189**
`base/EncryptedNameDict-0x<keyName>.dat` files, one per TACT key, each encrypted with the key
it is named after, plus `base/EncryptedSNOs.dat` mapping sno → group + key. Holding a key
therefore means holding the authored names of everything that key covers. Format:

```
u32  magic = 0xABCD4567
u32  count
count x { i32 snoGroup, i32 snoId }
packed NUL-terminated names, same order as the table
```

> **A correction worth recording.** The snapshot ships a
> `json/base/EncryptedNameDict.dat.json` with **four** entries, which reads like the file is
> useless. It is not — the snapshot's own parser writes that JSON from *inside* its per-file
> loop, so each dict overwrites the last and the artefact only ever contains whichever it read
> last. The dicts themselves are complete. This was written off once on that basis.

**The payload heuristic — partial fallback.** For pieces under keys you do not hold, the
authored name is sometimes still present in the appearance payload's ClothData name field:
`necM_stor245_TRS_cape`, `palF_stor171_LEG_hipPlate`. Only cloth-bearing pieces carry one, so
this recovers capes, skirts and chests and not plain boots — which is exactly why
`barF_stor251_HLM/BTS/GLV` stayed invisible while `_TRS` and `_LEG` did not.

Anything recovered this way should be shape-checked before use. A wrong name seats a piece on
the wrong class, which is worse than no name.

---

## 12. Cosmetics — the parts with surprising semantics

### Character creator filtering

Two independent locks, both of which must be honoured:

- **`fUsableByClass`** — an 8-element 0/1 mask in class-enum order. A 0 at the class's index
  excludes it. An absent array means usable by all.
- **`eClassRestriction`** — a single class index (used by MarkingShape and Jewelry). Out of
  range means global.

*(project measurement)* Per-class counts after both locks: Face 4 · HairStyle 19 (Rogue 20) ·
Jewelry 32 · HairColor 31 · EyeColor 14 · Makeup 22 · MarkingColor 92. MarkingShape genuinely
varies by class: Barbarian 75, Rogue 71, Sorcerer/Necromancer 70, Druid 55, Spiritborn 54,
Paladin 43, Warlock 40.

A category that returns **0 for every class is a missing download, not a data fact.**

Every folder contains one `… Bad Data …` stem that must be skipped.

### Hair colour is not in ramp order

`rgbaColors` is **`[shadow, HIGHLIGHT, mid]`** — the brightest stop sits in the *middle* of the
array. Render order is `[0, 2, 1]`.

*(project measurement)* Over every shipped HairColor definition, `[0,2,1]` is
luminance-monotonic for **31 of 31** real definitions; the stored order for **1**. Walking it
as authored hands mid-luminance strands the most saturated stop and the brightest strands a
duller one — it reads as flat neon plastic, which looks like a shader bug and is not.

`rgbaColors2`, a second 3-stop set present on all 32 definitions, is **undecoded**. Nothing
known selects it. It is listed here as an open question, not an answer.

### Markings are not recolourable

Each MarkingShape authors `snoDefaultColor` — 290 of 304 described shapes do — and that is the
real pairing. In the mask, **red is coverage and green is ramp position**. The colour
definition's `arPaintColorSamples` are **linear** floats and must be sRGB-encoded before use.

The `.msh` binary record is 56 bytes, and every offset below was established by searching each
record for the values its own JSON states, unanimous across ~300 records:

| offset | field |
|---|---|
| 0x00 | u32 magic `0xDEADBEEF` |
| 0x10 | u32 self sno |
| 0x18 | i32 `eClassRestriction` (−1 = any) |
| 0x24 | u32 `hIconImage` (0 = none) |
| 0x28 | u32 `snoMaskFace` |
| 0x2c | u32 `snoMaskBody` |
| 0x30 | u32 `snoDefaultColor` |

SNO fields use `0xFFFFFFFF` for "none"; `hIconImage` uses 0.

### Facial hair

The style index lives in a field named `unk_2ab2122` — there is no `dwSubObjectStyle` on these
records. 0 and 1 are Clean and Stubble and are **texture only, with no mesh**; 2–9 are real
beard meshes, male only.

Two findings worth having *(project measurement)*:

- **No FacialHair stem contains "male" or "female"** — the names are `Clean`,
  `Bushy_LongBeard`, `Rogue`. Any gender filter keyed on the filename is a silent no-op. Use
  `snoShellMaterialM` / `snoShellMaterialF` instead.
- **The face piece's facial-hair submesh is named `Global_<gender>_Facialhair_NN_*` on seven of
  eight classes — but Rogue male names it `lambert1_skin`**, a Maya default, with shader
  `hero_opaque_hollow` and zero textures. A substitution keyed on the name "facialhair" misses
  it entirely, so beard, stubble and brow coverage were all absent — and "skin" in the name
  additionally earned it a pointless skin-tone recolour. That shader occurs **exactly once**
  across all 16 face pieces, which is what makes it a reliable identifier.

Also worth knowing: Paladin and Warlock author style 0 and ship no `B09` mesh, but their shell
material still points at `palM_B09_mat` / `warM_B09_mat` — a texture-only shell. Not a bug.

---

## 13. How to verify a claim about this data

The method matters more than any individual fact here, and it transfers to any game.

**Score a hypothesis against ground truth you did not choose.** The material-SNO derivation in
this tool was hand-checked against about 32 appearances and looked solid. Run over the whole
corpus it reproduced the known-good JSON for only 22.7% of cases, and was rejected. Thirty-two
hand-picked samples are a hypothesis; 63,000 is a test.

**Report search coverage separately from model accuracy.** The first version of that sweep
divided by *all* appearances and read 41% as "the model is wrong", when the model held in 99%
of located cases and the *search* was what failed. Those are different bugs with different
fixes.

**Prefer a statistic to an eye.** The normal-map convention reversed twice under visual
inspection before a measurable above-minus-below statistic settled it.

**Check the absence, not just the presence.** "This class has no markings", "this bundle is
empty" and "the folder was never downloaded" look identical. Before concluding an absence,
confirm the source exists at all.

**Write the number down with the claim.** "299 of 299 records", "28.1% across 2,635 atlases",
"measured, reverted". A plausible-sounding claim with no measurement behind it has cost this
project real debugging time more than once.

---

## 14. Applying this to other games on the same stack

CASC, TVFS, BLTE and TACT are **Blizzard-wide**, not Diablo IV specific. World of Warcraft,
Overwatch, StarCraft II, Heroes of the Storm and Diablo IV all use the same storage and
encryption layers, so the tooling that gets you bytes transfers directly.

What does **not** transfer is everything from section 2 onward. The SNO group model, the
`base/meta/<sno>` + `base/payload/<sno>` split, the naming grammar, the material and shader
vocabulary and every offset on this page are Diablo IV's. Another Blizzard title on the same
storage will have a different record model above it.

The *habits* transfer better than the facts:

- Find the reference graph before writing a parser. What points at what is the whole map.
- Identify records by their references, not their names.
- Establish every offset against a known-good decode of the same record, and record the sample
  size.
- Treat a missing name, a missing file and a missing key as three different states, because
  they need three different responses.

---

## Sources

- **Measured while writing this page**, from `CoreTOC.dat.json` and
  `CoreTOCSharedPayloadsMapping.dat.json` of a current retail build: all group record and
  unnamed counts, slot-token and class-prefix distributions, naming-pattern counts, texture
  suffix frequencies, cloth naming, shared-payload totals and targets.
- **This project's verification notes** — `docs/notes/STATUS.md`,
  `docs/notes/ENCRYPTED-CONTENT-HANDOFF.md`, `docs/MODEL_EXPORT.md`,
  `docs/PET_CLIP_NAMING.md`, `docs/PALADIN_MATERIAL_SHARING.md` — for the normal-map
  convention measurement, the icon-atlas study, the cloth semantics, the facial-hair and
  hair-colour findings, the `.msh` layout and the encryption work. Each records the sample
  size behind its claim.
- **The decoder source** for texture slot roles, supported codecs, hardpoint hashes and the
  CASC path scheme.

No claim on this page comes from a third-party wiki, a forum post or another extractor's
source. Where this project has not verified something — the remaining 110 SNO groups,
`rgbaColors2` — it is named as unverified rather than filled in.
