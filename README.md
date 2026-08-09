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





<img width="1080" height="720" alt="anim_20260809_180840-ezgif com-optimize" src="https://github.com/user-attachments/assets/2d3f6cc1-aeed-4e84-ab55-47c52ff569cf" />



<img width="2560" height="1440" alt="D4AssetBrowser_uyVybhfc02" src="https://github.com/user-attachments/assets/31178753-07f5-44db-895a-83e40711f383" />
<img width="2560" height="1440" alt="D4AssetBrowser_UHzm9rGzEQ" src="https://github.com/user-attachments/assets/561be044-63c3-47a9-8289-0b2d21810fa2" />
<img width="2560" height="1440" alt="D4AssetBrowser_5I2ww9Q7qQ-ezgif com-optimize" src="https://github.com/user-attachments/assets/30c566a0-f817-4ffb-a946-ec5638ddfb53" />
<img width="2560" height="1440" alt="D4AssetBrowser_Tt26Xz2lEq" src="https://github.com/user-attachments/assets/7d032717-77ef-4837-b68e-8a16416f946b" />
<img width="2048" height="1152" alt="1d0c449d-0529-4640-98e4-955b84bd3013" src="https://github.com/user-attachments/assets/368024e1-b7b2-4d6b-909d-bb4b0ad24075" />
<img width="2560" height="1440" alt="D4AssetBrowser_c0aFS6riGg" src="https://github.com/user-attachments/assets/e1797e72-3b62-4bc4-bee0-07d2b8030d92" />
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

**Requirements:** Windows 10/11 x64 · a Diablo IV install (Battle.net Tested Only) ·
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
| **Catalogue** | The Cosmetics Shop — every bundle, what was in it, and export the lot. |
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

**ANIMATIONS.** Clips are grouped under the game's own AnimSet headers, and each set carries its
own colour — header saturated, its rows tinted the same hue — so a run of rows stays attached to its
set once the header has scrolled away. Gold still means *pulled from another model* and muted cyan
*matches this skeleton but has no explicit in-game assignment*; those win over the set tint, because
they say something about the row rather than which block it is in. The header counts both what the
model can reach and what an export would actually embed (`ANIMATIONS · 758 (378 export)`).

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

### Bulk Extract

Filter the whole index, watch the match count update live, then export everything at once.
Same funnel filters as the Models tab. **Pick items manually** moves matches into a persistent
**Queue** that survives filter changes, mode switches and restarts.

Options are split by lifetime: an **Export settings…** button opens the shared export settings
(textures, animations, raw sources), and a **This run:** group holds the per-run switches — loose
textures · raw buffers · write report · **Parallel** workers (auto = core count). Output can be flat or
organised into subfolders by class or type. **Only new** skips anything already exported —
tracked in a `_bulk_manifest.json` ledger — or **Overwrite**. Live console with a working
Cancel (or `Esc`) and Pause/Resume that excludes paused time from the ETA. Failures are
written to `_bulk_failed.txt` with a reason each, and one bad model can't take the run down.

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
exports the mount.

**Animation libraries** — skeleton plus selected clips, no mesh, for retargeting in Blender. The
Export menu names the count it is about to write (*Export animations only — 378 clips…*), so a scope
or filter change is visible before you commit to the file rather than after.

**Which animations get embedded** *(Settings ▸ Export ▸ Models)* is two independent questions.
*Which clips belong to this model* — **Original** (named in the model's own family: gameplay,
emotes, wardrobe and UI poses), **Cutscene & conversation** (the `IGC_` / `Conv_` performances the
same body appears in), **Previewed**, **Pulled**, **Base** (the base-rig clips a gear piece
inherits). Then *which of those you want* — a **length cap** and **exclude-by-name** patterns.
`barM_base00` declares 758 clips: 378 in its own family and 380 cutscene, and eight long clips carry
about a quarter of the animation payload against a 75-frame mean — so the cap is usually the biggest
single lever on file size. Filters apply only to the data-derived sources; a clip you are previewing
or have pulled is an explicit choice and is never filtered out from under you.

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

**Modding / retarget options** *(Settings ▸ Experimental)* — engine presets for Blender,
Unreal/Skyrim and Unity (unit scale and normal-map convention), rebuild normal-map blue
channel, readable bone names, hardpoints as empties, Blender-friendly `.L`/`.R` rig names,
symmetrise for X-Axis Mirror, reduce to a 26-bone humanoid rig, strip cloth chains, include the
base body as a fit reference, and batch the whole armour set with a `manifest.json`.

**Shortcuts** — `Ctrl+E` export selection · `Ctrl+Shift+E` export to last dir ·
`Ctrl+Shift+A` animations only · `Ctrl+Shift+I` save preview image. All rebindable in
*Settings ▸ Hotkeys*, along with unbound slots for the two GIF exports.

---

## Building from source

```bat
build.bat          :: first build — vcpkg fetches Qt6, slow (~30-60 min)
rebuild.bat        :: incremental, then launch
Diagnostics.bat    :: menu of audits and self-tests
```

Visual Studio 2022 with "Desktop development with C++". Qt6, fastgltf, lz4 and zlib come
from vcpkg automatically.

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
| `coretoc_v3.bin` · `casc_index_v1.bin` | asset index caches, keyed to the game build |
| `appearance_meta_v23.json` · `icon_index_v4.json` | metadata and icon caches |
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
  be told about new builds.

**After a game patch:** run **File → Update TACT Keys**, then re-run **File → Dependencies…**
to refresh d4data. New seasonal and collab content usually needs both. **File ▸ Index** then lists
every background index with its state — *not started* / a live percentage / *done* — with **Index
all** to start whatever is outstanding and **Re-index everything** to drop the caches and rebuild;
clicking a single row rebuilds just that one. The status bar carries the same summary (`Indexes
6/9`), so a half-built index is visible rather than something you discover from a worse result. If something still
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
