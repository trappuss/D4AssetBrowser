# D4AssetBrowser

A Diablo IV asset browser and 3D wardrobe / mount studio. Reads your installed game
directly (CASC), decodes textures, and previews or exports appearances, armour sets,
weapons, mounts and pets as animated `.glb` — with cloth physics, dyes, markings,
hair, makeup and animations.

**A full C++17 / Qt6 / OpenGL rewrite** of
[trappuss/Diablo4AssetBrowser](https://github.com/trappuss/Diablo4AssetBrowser)
(Python / PySide6). Single native executable — no Python, no external extractor.

> Not affiliated with or endorsed by Blizzard. For personal use with a copy of Diablo IV
> that you own. **No game assets and no decryption keys are included in this repository.**

---
## Preview






<img width="2560" height="1440" alt="D4AssetBrowser_uyVybhfc02" src="https://github.com/user-attachments/assets/31178753-07f5-44db-895a-83e40711f383" />
<img width="2560" height="1440" alt="D4AssetBrowser_UHzm9rGzEQ" src="https://github.com/user-attachments/assets/561be044-63c3-47a9-8289-0b2d21810fa2" />
<img width="2560" height="1440" alt="D4AssetBrowser_5I2ww9Q7qQ-ezgif com-optimize" src="https://github.com/user-attachments/assets/30c566a0-f817-4ffb-a946-ec5638ddfb53" />
<img width="2560" height="1440" alt="D4AssetBrowser_Tt26Xz2lEq" src="https://github.com/user-attachments/assets/7d032717-77ef-4837-b68e-8a16416f946b" />
<img width="2048" height="1152" alt="1d0c449d-0529-4640-98e4-955b84bd3013" src="https://github.com/user-attachments/assets/368024e1-b7b2-4d6b-909d-bb4b0ad24075" />
<img width="2560" height="1440" alt="D4AssetBrowser_gCZWlx9gDc" src="https://github.com/user-attachments/assets/6075e323-42e8-49ed-9f4b-a19c7ab8c38f" />

---
## Quick start

1. Download the release `.zip` and unzip anywhere. Fully portable — everything the tool
   writes lives in `data\` next to the exe.
2. Run **`D4AssetBrowser.exe`**.
3. **File → Settings** — set your **Diablo IV game folder**.
4. **File → Dependencies…** — download **d4data** (community metadata snapshot). One click.
   Budget **~4–6 GB** and 10–20 minutes: it writes ~460,000 small JSON files, so the
   *Extracting* step is limited by your drive, not your connection. Only the 20 asset
   groups the tool reads are fetched (of 133), and the folder is NTFS-compressed.
5. **File → Update TACT Keys** — fetches the community decryption keys.

No Python. No `pip`. No d4extract.

**Requirements:** Windows 10/11 x64 · a Diablo IV install (Battle.net or Steam) ·
GPU with OpenGL 4.5 · internet on first run.

**False Positive:** I guess because it's an .exe file it gets flagged at a virus, it's not.
<img width="522" height="440" alt="ApplicationFrameHost_pqj4REMhxN" src="https://github.com/user-attachments/assets/85705de9-b710-4896-beee-02c69ef684b4" />

---

## Tabs

| Tab | What it does |
|---|---|
| **Models** | Browse and inspect all 67k+ appearances in a live PBR viewport. |
| **Wardrobe** | Dress a character — armour, weapons, dyes, hair, markings, animations. |
| **Stable** | The same for mounts and pets. |
| **Textures** | Browse and decode every texture in the game. |
| **Catalogue** | The Cosmetics Shop — every bundle the game has, what was in it, and export the lot. |
| **Bulk Extract** | Filter the index and export in one run. |

Each is covered in detail below.

---

## Features by tab

### Wardrobe

**Pigments — the game's dye system, rebuilt.** Dyes are read from the game's own
`Dye` records, taking each swatch as **four colours, one per DyeMask zone**, and rendered
the way the game shader does it: the mask's red channel picks the zone, a ramp texture
supplies the value multiplier, and unmasked texels stay undyed. NPC, debug and
hidden-from-UI dyes are filtered out.

- **Set Look / Set Pigment** toggle per slot, mirroring the in-game flow.
- **Per-slot pigments**, or an **Apply to all slots** checkbox that hits the five armour slots.
- **Custom pigments** — a colour wheel, four numbered zone buttons, hex entry, and
  **Save Pigment** to name and store your own. Customs appear in the library with a gold border.
- **8 memory swatches** — drag a colour in to store it, click to apply, right-click to clear.
  Colours can also be dragged from one zone onto another.
- **Copy / Paste / Clear pigment** between slots from the slot right-click menu.
- Weapons are correctly **not dyeable** (greyed with a reason), matching the game.

**Character.** 8 classes including Paladin and Warlock · both genders · nine creator
categories drawn from real game data — Face, Hair style, Hair colour, Eye colour, Facial hair,
Makeup, Marking, Marking colour, Jewelry — each filtered to what your class and gender can
actually use. Plus skin tone and a skin-detail overlay (freckles / vitiligo). Eye colour is
composited from the game's own base/normal/ORM/emissive maps.

**Equipment — 10 slots.** Helm · Torso · Gloves · Legs · Boots · Main · Off · Sheath ·
Sheath 2 · Back trophy. Weapon slots your class can't use are greyed out. Each slot has its
own search box and collection filter.

**Ensembles.** Save the whole look — class, gender, skin, all nine creator picks, all ten
slots, every per-slot pigment, and the animation. The tile art is **a real viewport snapshot
taken at save time**, not a generic icon. Save · Overwrite · Delete · Rename; double-click to load.

**Equip Theme.** Right-click any look card to equip its whole matching set, with four scopes —
everything, armour only, markings only, or weapons only — and the camera frames what changed.

**Auto Animate** *(Settings ▸ Wardrobe)*. When your **weapon class** changes, plays the game's
own wardrobe unsheathe clip once then settles into that loadout's idle, resolved from the
shipped `ui_wardrobe` AnimSets. Armour changes don't trigger it.

**Also:** Ctrl+Z undo (30 deep) · attached models (back trophy, weapons) keep their own rigs and
play their own clips from a pinned ATTACHED list · camera snap-to-slot · cloth physics.

### Models

**Three views of the same list** — **List** (dense flat rows), **Outliner** (a scene tree where
the loaded model's parts, looks, animations and bones hang off its row), and **Grid**
(thumbnails). Switch from the display dropdown in the header.

**Search.** One box: `text` for name/tags, `123456` for a SNO, `#tag` to match tags, title and
collection but *not* the filename, `c:some collection` for collection (reads to end of line, so
put it last). Space-separated terms all have to match, and a leading `-` excludes —
`pandem -destroyed -pillar`, `-#cape`. `Ctrl+F` focuses, `Esc` clears, `↓` recalls your last
ten searches. The same syntax drives Bulk Extract and the Textures tab.

**Filters** live in a funnel popup that stays open while you tick things: grouped tag
checkboxes (Category, Class, Gender, Type) with **Match any (OR)**, plus **Only decrypted**,
**Only encrypted (TACT)**, **Hide un-renderable**, and usage facets — **Latest** (new this
update), **Animated**, **Rigged**, **Orphaned**. Every active filter shows as a removable chip.

**Viewport.** Four shading modes (Wireframe · Flat · Shaded · Rendered with IBL, shadows, SSAO,
tonemap). A **channel viewer** — Base Colour, Normal, Roughness, Metallic, AO, Emissive —
which you can cycle by scrolling the `⌄` next to the shading balls. **Overlays**: statistics,
ground grid, axis gizmo, skeleton, hardpoints, collision capsules, physics bones (anchored grey
/ simulated orange), bone names and translated bone names. **FX / SIM / GIB** submesh toggles.
Popovers for Graphics, Pigment, Camera, Lighting.

### Stable

Mounts and pets across three slots — **Mount** (labelled *Pet* for companions) · **Mount Armor**
· **Trophy**. Species-aware (horse / cat / basilisk). **Equip matching set** builds the themed
trio from the game's own bundle data, with individual *Equip Armor* / *Equip Trophy* entries.
Trophies seat onto the correct mount bone. Mounts aren't dyeable in Diablo IV, so there's
deliberately no dye control here.

### Textures

Every texture in the game, decoded (BC1/3/4/5/7). **Channel isolation** (RGB · R · G · B · A),
**alpha checkerboard**, cubemap/array face selector, and a **pixel inspector** that reports
`(x, y) RGBA` under the cursor. Scroll to zoom, drag to pan, double-click to reset.

Filter by **format**, by **gear tags** (the class/type/gender of appearances that use the
texture), orphans-only, or decrypted-only. Search supports `#tag`, SNO digits, and `-exclude`.

**TEXFRAMES** lists the sprite frames packed into an atlas, with an optional **Trim** that crops
each export to its tight bounds. **ASSOCIATED MODELS** walks texture → material → appearance and
lets you jump straight to the model in the Models tab. Images can be dragged out of the preview
straight into another application.

### Catalogue

The Cosmetics Shop, browsable. Every bundle it has sold — hero art, card, lore text — with each
item inside resolved to the appearance or texture it actually is. Search matches the shop title,
the SNO name and the lore.

**Filters** live behind the same funnel the other tabs use: contents kind, patch, season, and
**Latest** (new in this game update), plus a sort by name, season or patch. Active filters show as
removable chips and tint the funnel.

**Two views of the contents.** The shop's own *INCLUDES 8 ITEMS* strip, showing each piece's real
inventory icon — one row per gender, because armour resolves to a female and a male appearance and
both are openable — and a tree beneath it carrying SNOs, the **SLOT** each piece occupies (read
from the item → gear reference graph, not guessed from the name), and a plain *no appearance found*
wherever a product could not be resolved. Selection is mirrored between the two panes. The bundle's
own shop art is listed as its own branch.

**Double-click** any item to open it in Models — textured, with its parts tree and animations.
Bundle art opens in Textures.

**Provenance.** Supported classes, whether it shipped with VFX, its associated season, and the
shop's own *requires* / *add-on to* / *excludes* relationships — which is how you discover a mount
trophy was never sold at all, but was a Season 3 premium pass reward.

**Bundles the metadata snapshot has never heard of are read from the game itself.** d4data
describes about 7,500 shop products; the game has 9,300. The ~1,800 in the gap are the encrypted
and newly-patched ones — the Doom collab armour was missing for every class except Druid and Rogue
purely because only those two shipped a description file. Those records are now parsed straight
from the game's binary, so their contents, artwork and models are all present and export normally.
What the game files do *not* carry is the shop's display text, so such a bundle shows its asset
name instead of a title and says so plainly: *"read from game files — no shop text in this
snapshot"*. Re-running **File → Dependencies…** once d4data catches up fills the text back in.

### Bulk Extract

Filter the whole index, watch the match count update live, then export everything at once.
Same funnel filters as the Models tab. **Pick items manually** moves matches into a persistent
**Queue** that survives filter changes, mode switches and restarts.

Options: include textures · all animations · pulled animations · raw sources. **Parallel**
workers (auto = core count). **Only new** skips anything already exported — tracked in a
`_bulk_manifest.json` ledger — or **Overwrite**. Live console with a working Cancel (or `Esc`)
and Pause/Resume that excludes paused time from the ETA. Failures are written to
`_bulk_failed.txt` with a reason each, and one bad model can't take the run down.

Everything else this tab used to keep its own copy of — raw buffers, loose textures, the CSV
report — now lives in *Settings ▸ Export* alongside the rest, so each setting has one home and
one value regardless of which tab starts the run.

**Output layout** is one choice — **Flat**, or a subfolder per **class**, per **type**, or per
**model** — and it is a property of exporting models rather than of this tab. Every batch path
obeys it: Bulk Extract, a multi-selection from the Models tab, the context-menu batch, *export
all*. Single-model exports deliberately don't, because `Ctrl+E` quietly becoming
`Barbarian\foo.glb` is a surprise the caller can't undo. Inside a group the shape is identical
in every mode — the models, plus `deps\`, `textures\` and `buffers\` for whichever options are
on — which is what makes Flat simply *one group, at the root* rather than a special case.

**Repeated textures decode once per run.** A set of appearances typically shares the same
detail and dye maps; a run-scoped cache means the second and later uses are a memory hit rather
than a fresh BC decode. It is bounded, lives only for the duration of the run, and writes
nothing to disk.

---

## Panels

The right-hand column is a stack of panels, Blender-style. A vertical **icon strip** toggles
them; each panel header carries **▲ ▼** to reorder and **✕** to hide. They live in a splitter,
so drag the handles to resize — a drag can never fully erase one, and a newly-opened panel
takes a sensible height from the slack of the panels already up rather than forcing an equal
split. The whole column collapses to just the strip via the `»` arrow. Layout is remembered
between sessions (*Settings ▸ Remember the right-hand panel layout*).

| Tab | Panels |
|---|---|
| **Models** | `LOOKS` · `MATERIALS` · `SHADING` · `INFO` · `PARTS` · `CLOTH` · `ANIMATIONS` · `ATTACHMENTS` |
| **Wardrobe** | `PARTS` · `MATERIALS` · `MATERIAL TEXTURES` · `TEXTURE PREVIEW` · `ANIMATIONS` |
| **Stable** | `PARTS` · `MATERIALS` · `TEXTURES` · `INFO` |

- **INFO** — filename, title, SNO, collection, tags, sets, filesize, format, bounds, LODs, bones,
  material/texture/animation counts, actor, physics, what uses it, and clickable **Variants** links.
- **PARTS** — per-part triangle counts, slot and material, with visibility checkboxes.
- **MATERIALS** — App Materials / SubObject Apps / Materials / Vertex Buffers.
- **CLOTH** — the authored physics tuning per piece, showing the game's value beside the live one.
- **TEXTURE PREVIEW** (Wardrobe) — six channel tiles (colour, roughness, metal, normal, alpha,
  emissive) with a hover zoom you can resize with the wheel.

---

## Context menus

Right-click works nearly everywhere, and the same object offers the same actions wherever you
find it — the list, the grid and the outliner all raise one menu, as do the Parts panel, the
outliner's part nodes and the 3D viewport.

**An asset row / grid tile / outliner row**
Load / preview · Copy image · Save image(s) · Save image(s) as… · Render icon(s) ·
Export to last dir · Export to… · Copy SNO id · Copy file name · Copy name ·
Copy collection name · Variants ▸ · Show dependencies…

**A part** (Parts panel · outliner part node · clicking a part in the 3D view)
Export Model / Export Part (to last dir or chosen) · Copy part file name · Copy source file name,
SNO, name and collection · Frame Part · Select Part · Hide/Show Part · Isolate Part ·
Show All · Hide All · Invert.

**A wardrobe slot** — Clear · Copy/Paste/Clear pigment · Copy image · Save image… ·
Export Model · Copy SNO / file name / name / collection.
**A look card** — Equip · Equip Theme (all / armour / markings / weapons) · the same
export and copy block.
**A pigment card** — Apply to this slot · Apply to all slots · Copy name · Copy colours ·
Delete custom pigment.
**A texture** — Export to… · Copy image · Save image as… · Copy SNO / file name / name.
**Any detail table** — Copy · Copy all (also `Ctrl+C`).

---

## Exporting

**Models** — rigged, animated `.glb`. Exports exactly what's visible: hidden parts stay out
unless you explicitly picked them. Batch-export a multi-selection, or **drag models straight
out of the list** into Blender or Explorer. Wardrobe exports the assembled outfit; Stable
exports the mount. Batch exports follow the output layout described under
[Bulk Extract](#bulk-extract).

**Detail maps are baked in** *(Settings ▸ Export ▸ Models)*. Diablo IV layers a tiling detail
normal and roughness over the base maps per dye zone — that's where fabric weave, leather grain
and scale texture actually come from, and a plain material dump leaves it all behind, which is
why an exported piece can look flat next to the same piece in the viewport. With this on, the
detail layers are composited into the exported normal and ORM maps, zone by zone, exactly as
the viewport composites them. It applies on every path that writes a model — Models, Wardrobe,
Stable, Catalogue and Bulk Extract — rather than only in the wardrobe preview.

**Animation libraries** — skeleton plus selected clips, no mesh, for retargeting in Blender.

**Images** — PNG (lossless, alpha), JPEG (smallest, no alpha) or WebP (small, keeps alpha).
Resolution 25–400%; **above 100% the scene is genuinely re-rendered larger** rather than
upscaling a screenshot. Optional **transparent background** (native alpha, single render) and
**crop to model**, which applies to stills as well as GIFs.

**GIFs** — turntable or animation loop. Median-cut palette, optional dithering, inter-frame
differencing, and an **optimise-to-target-size** mode that tries palette reduction, then
dithering off, then aimed downscales, and **ships the smallest result rather than the last
attempt** — telling you plainly when a target isn't reachable. Turntables snap to whole
animation loops so orbit and pose wrap together, and run a warm-up lap so cloth settles before
the first captured frame. A turntable follows the animation only if it's actually playing.

**Textures** — single, batch, or every frame of an atlas, PNG or JPEG, with optional trimming
to non-transparent bounds. Filenames follow templates using `{{FileName}}`, `{{SNO}}`,
`{{FrameIdx}}` and `{{FrameName}}`.

**Catalogue** — a whole bundle into its own folder (`models\`, `art\`, `icons\` and a
`manifest.json` naming everything that resolved *and* everything that did not), several bundles in
one run with **Multi select**, or just the rows you highlighted in the includes strip or the
contents list. The Export menu names what it will act on before you commit — *Export 2 models…*,
*Export 1 image…*, *Export 3 bundles…*. Everything routes through the Models and Textures
pipelines, so every option here applies. *Settings ▸ Export ▸ Catalogue export* adds every frame of
each shop atlas as its own PNG.

**Modding / retarget options** *(Settings ▸ Export ▸ Advanced)* — engine presets for Blender,
Unreal/Skyrim and Unity (unit scale and normal-map convention), rebuild normal-map blue
channel, readable bone names, hardpoints as empties, Blender-friendly `.L`/`.R` rig names,
symmetrise for X-Axis Mirror, reduce to a 26-bone humanoid rig, strip cloth chains, include the
base body as a fit reference, and batch the whole armour set with a `manifest.json`.

**Shortcuts** — `Ctrl+E` export selection · `Ctrl+Shift+E` export to last dir ·
`Ctrl+Shift+A` animations only · `Ctrl+Shift+I` save preview image. All rebindable in
*Settings ▸ Hotkeys*, along with unbound slots for the two GIF exports.

**Which option do I actually want?** *Settings ▸ Information* answers that in the tool itself,
in sub-tabs — including a side-by-side of the two ways to get loose texture files, which look
interchangeable and are not: one copies the maps a model already decoded, the other decodes
every map in the material whether the model uses it or not.

---

## Building from source

**You need:** Windows 10/11 x64 · Visual Studio 2022 with the *Desktop development with C++*
workload · [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set · CMake 3.21+ and
Ninja (both ship with the VS workload) · Python 3 (for the pre-build source checks).

```bat
build.bat          :: first build. Finds vcvars64 itself, then lets vcpkg fetch every
                   :: dependency. Qt6 is built FROM SOURCE here, so budget 30-60 min.
rebuild.bat        :: incremental build, then launch. The one you use day to day.
clean-rebuild.bat  :: wipes build\ and configures again, without touching vcpkg.
Diagnostics.bat    :: menu of audits and self-tests
```

Or drive CMake directly — `CMakePresets.json` carries three configurations:

```bat
cmake --preset windows-msvc-release   &&  cmake --build --preset release   :: build\release
cmake --preset windows-msvc-debug     &&  cmake --build --preset debug     :: build\debug
cmake --preset windows-static-release &&  cmake --build --preset static    :: one static exe
```

Dependencies come from the `vcpkg.json` manifest, pinned to a baseline commit so a build today
resolves the same versions as a build six months from now: **qtbase** (widgets, opengl, gui,
png, jpeg) · **qtsvg** · **fastgltf** · **tinygltf** · **zlib** · **lz4**.

**`verify-src.py`** runs before the compiler and catches the mistakes that have actually broken
this build, in seconds rather than after a multi-minute MSVC cycle: zero-byte files from a
botched write, unbalanced `{}` `()` `[]`, a header-only helper used without its `#include`,
printf-style format/argument mismatches, and locals named `emit` / `signals` / `slots` that Qt's
macros silently delete.

```bat
python verify-src.py            :: check src\
python verify-src.py --quiet    :: only print problems
```

### Continuous integration

`.github/workflows/release.yml` builds the portable Windows folder on GitHub's runners. Push a
tag matching `v*` and it compiles, runs `windeployqt`, zips the result and publishes it as a
GitHub Release; run it by hand from **Actions ▸ Release ▸ Run workflow** to get the same zip as
a plain artifact without cutting a release.

The CI build installs a **prebuilt Qt 6.7.3** rather than letting vcpkg compile Qt, and uses
vcpkg only for the four small dependencies. That is the single reason a cold CI build takes
minutes while a cold local `build.bat` takes closer to an hour — it is the same source and the
same compiler either way.

---

## Repository layout

| | |
|---|---|
| `src\` | all C++ — see the table below |
| `res\` | application icon and Qt resource script |
| `tools\d4cloth\` | standalone cloth-format probe used to derive the physics parsing |
| `docs\` | format notes and investigation write-ups |
| `.github\workflows\` | the release build |
| `*.bat` · `*.ps1` · `verify-src.py` | build, audit, dump and test entry points — all double-clickable |

`src\` is split by what the code talks to, not by Qt class:

| | |
|---|---|
| `casc\` | Blizzard's CASC storage — archives, indices, BLTE frames, TACT decryption |
| `index\` | the asset index and everything derived from it: appearance metadata, tags, icons, shop products |
| `model\` | geometry, skeletons, animation, materials, cloth physics, glTF export |
| `tex\` | texture definitions and BC1/3/4/5/7 decode |
| `gl\` | the OpenGL 4.5 renderer — PBR, IBL, shadows, SSAO |
| `tabs\` | the six tabs and their panels |
| `app\` | main window, settings, dialogs |
| `util\` · `text\` · `deps\` | shared helpers, string handling, the dependency downloader |

**Not in the repository, by design:** no game assets, no TACT decryption keys, no d4data
snapshot. All three are fetched or read at runtime from a copy of the game you own — see
[Data and keys](#data-and-keys). `build\`, `dist\` and the local backup tree are gitignored.

---

## What's different from the original

The original is Python/PySide, shells out to **d4extract** for geometry, and reads
**d4data** JSON for all metadata. This is native C++, parses the game's binary formats
itself, and uses d4data only as a fallback.

### Architecture

| | Original v1.0 | Native |
|---|---|---|
| Language | Python 3.11 + PySide6 | C++17 + Qt6 |
| Distribution | repo + `pip install` into `deps\py` | single portable `.exe` |
| Geometry | **d4extract** subprocess per model | built-in binary parser |
| Renderer | Qt3D / offscreen | direct OpenGL 4.5, PBR + IBL |
| Metadata | d4data JSON only | **CASC binary first**, d4data as fallback |
| Startup | Python import + dependency check | native, per-build binary caches |

Dropping the d4extract subprocess is the biggest structural change: geometry, materials
and textures decode in-process, so there is no per-model process spawn, no temp files, and
no external tool that can drift from your installed patch.

### Capabilities the original does not have

**Encrypted and brand-new content.** The original's known issues list *"Warlock models are
bugged and or missing, probably encrypted"* and *"Paladin animations are missing, probably
encrypted"*. This version resolves encrypted assets end to end from CASC:

- Nameless (encrypted) records are **indexed instead of dropped** — 14,100 of them.
- Appearance names recovered from ClothData embedded in the payload.
- `ptAppearanceMaterials`, material→texture lists, and texture dimensions/formats read
  from the **binary**, so assets missing from the d4data snapshot still render fully.
- Texture definitions read from CASC's bulk tables (`texture-base-global.dat` plus per-key
  overlays) — textures have no per-sno meta entry, which is why this was the last gap.
- **Shop products read from the binary** when the metadata snapshot has no record of them —
  ~1,800 of 9,300, which is where every collab and seasonal bundle lands until d4data catches up.
- **"Only encrypted (TACT)"** filter to browse exactly that content.

**A multi-frame BLTE decode bug is fixed.** Any encrypted asset stored in more than one
BLTE frame silently lost every frame after the first — a Salsa20 nonce block-index
problem, invisible on single-frame assets (which is nearly all of them). It truncated
large payloads and surfaced as unrelated "missing texture" and "incomplete mesh" bugs.

**Cloth physics.** Authored ClothData drives a real solver — capsule colliders, driver
skinning, tethers, per-piece tuning resolved through `snoCloth`. Chain weapons (flails)
simulate. The original has none of this.

**Rendering.** PBR with image-based lighting, detail maps, per-part material overrides,
proper transparency (the original lists *"Stable models don't have proper transparency"*
as a known issue), hair and skin shading, marking and dye layers, shell fur.

**Wardrobe depth.** Hardpoint-correct weapon placement per class and weapon type, derived
from `ItemType.tHardpointOffsets` rather than guessed. Auto-animations driven by the
game's own `ui_wardrobe` AnimSets across all classes and genders. Per-class defaults and
saved ensembles.

**Diagnostics.** `Audit Asset Health.bat` walks every appearance, classifies what can and
cannot be shown (OK / no-textures / no-materials / no-geometry / locked / no-data), and
**diffs against the previous run** — so a game patch or a new TACT key reports "newly
working / newly broken" rather than being discovered months later.
`Test Encrypted Chain.bat` verifies the appearance → material → texture chain in seconds.
The health audit also reports **iconless but renderable** assets — pieces that decode perfectly and
show as a blank row because no inventory icon binds to them — so a name-rule regression that costs
a whole category its icons shows up as a number rather than as a bug report months later.

**Help → Health check** adds a *snapshot coverage* row: how many shop products and items the game
has versus how many the metadata snapshot describes. That gap is the honest answer to "why is this
bundle missing", and it is the first thing to read after a patch.

`Dump StoreProduct Layout.bat` and `Dump Marking Model.bat` each build, run, write a report and
exit on their own — one double-click, no babysitting.

### Honest limitations

- **~10% of wardrobe appearances render incomplete.** The model parser decodes only vertex
  buffer 0; sub-objects on other buffers are dropped rather than drawn scrambled. Measured,
  documented, not yet fixed.
- **93 appearances decode perfectly but have no name**, so they cannot appear in
  name-keyed rosters. Cloth-bearing pieces recover names; plain helms/gloves/boots do not.
- **~11,500 SNOs stay locked** behind 189 TACT keys nobody has harvested. Not fixable here.
- Windows only.

---

## Portable — everything lives in `data\`

No registry keys, no `%APPDATA%`, no user-profile files. Everything the tool writes goes in
`data\` beside the exe:

| | |
|---|---|
| `D4AssetBrowser\*.ini` | settings (INI, not the registry) |
| `coretoc_v2.bin` · `casc_index_v1.bin` | asset index caches, keyed to the game build |
| `appearance_meta_v22.json` · `icon_index_v3.json` | metadata and icon caches |
| `model_thumbs\` · `stable_thumbs\` · `icon_overrides\` | rendered thumbnails |
| `ensembles\` | saved wardrobe outfits |
| `d4data\` | the metadata checkout |
| `D4AssetBrowser.log` | attach this to bug reports |

Move the folder to another drive or a USB stick and it keeps working. Delete it and nothing
is left behind. Superseded cache versions are pruned automatically at startup.

The only exception, deliberately: **export** dialogs default to Documents/Pictures, because a
`.glb` you exported belongs with your files, not inside the tool.

---

## Data and keys

Nothing proprietary ships in this repository. On first run the tool downloads **d4data**
(community metadata) and fetches **TACT keys** from the public community list. Both live
next to the exe and are gitignored; delete the folder to remove everything.

TACT keys decrypt content Blizzard has already shipped to your client. This repo does not
distribute them.

---

## Keeping up with the project

Roadmap, what's in progress and what's planned live on the project board:

### **[D4AssetBrowser project board](https://github.com/users/trappuss/projects/3)**

- **[Issues](https://github.com/trappuss/D4AssetBrowser/issues)** — bugs and requests.
  A missing or broken model is worth reporting: attach `data\D4AssetBrowser.log`, which names
  the exact asset and, where relevant, the TACT key it needs.
- **[Releases](https://github.com/trappuss/D4AssetBrowser/releases)** — watch the repo to
  be told about new builds. Each one is the portable zip, built by
  [the release workflow](https://github.com/trappuss/D4AssetBrowser/actions) from the tagged
  commit, so what you download is what the source at that tag produces.

**Pull requests are welcome.** Run `python verify-src.py` and make sure `rebuild.bat` completes
before opening one — between them they catch most of what a review would otherwise be spent on.
If a change touches parsing, say which assets you tested it against; a format derivation that
works on the ten models you tried and fails on the eleventh is the usual failure here, and
`Audit Asset Health.bat` diffs the whole index against your previous run for exactly that reason.

**After a game patch:** run **File → Update TACT Keys**, then re-run **File → Dependencies…**
to refresh d4data. New seasonal and collab content usually needs both. If something still
won't load, `Audit Asset Health.bat` diffs against your last run and reports exactly what
changed — attach that output to an issue and it saves a lot of back-and-forth.

---

## Credits

Original tool and design: **[trappuss](https://github.com/trappuss/Diablo4AssetBrowser)**.
Community tooling: **d4data** (DiabloTools) · **d4extract** (narascode, no longer
required) · **[rustydemon](https://github.com/HoldMyBeer-gg/rustydemon)** (TACT keys) ·
**d4analyzer** (reference extractions used to verify binary format derivations).

## License

MIT — see [LICENSE](LICENSE).
