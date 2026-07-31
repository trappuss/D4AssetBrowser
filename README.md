# D4AssetBrowser

*(formerly Diablo4AssetBrowser Native)*

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

## Quick start

1. Download the release `.zip` and unzip anywhere. Fully portable — everything the tool
   writes lives in `data\` next to the exe.
2. Run **`D4AssetBrowser.exe`**.
3. **File → Settings** — set your **Diablo IV game folder**.
4. **File → Dependencies…** — download **d4data** (community metadata snapshot)(don't panic when extracting folders takes a long time). One click.
5. **File → Update TACT Keys** — fetches the community decryption keys.

No Python. No `pip`. No d4extract.

**Requirements:** Windows 10/11 x64 · a Diablo IV install (Battle.net Tested Only) ·
GPU with OpenGL 4.5 · internet on first run.

---

## Tabs

| Tab | What it does |
|---|---|
| **Models** | All 67k+ appearances in three views (list / outliner / icon grid). Live PBR viewport, parts, looks, materials, textures, LODs, bones, animations, dependency graph. Smart search: `c:` collection, `#tag`, digits = SNO. |
| **Wardrobe** | Build a character — class, gender, face, hair, facial hair, armour per slot, weapons, back trophy. Dyes, skin/hair tint, eye colour, makeup, markings. Cloth physics, auto-animations, saved ensembles. |
| **Stable** | The same for mounts and pets — bodies, barding, trophies. |
| **Textures** | Browse and decode every texture (BC1/3/4/5/7); channel split, frame galleries, model associations. |
| **Bulk Extract** | Queue and export many assets at once. |

**Export:** rigged animated `.glb` · PNG / JPEG / WebP stills · animated GIF with
palette + inter-frame optimisation and a size budget · turntables.

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

### Original's known issues — status here

| Original known issue | Status |
|---|---|
| Warlock models bugged / missing (encrypted) | **Fixed** — encrypted content resolves from CASC |
| Paladin animations missing (encrypted) | **Fixed** — same route |
| Horse armour isn't rigged to the skeleton | **Fixed** |
| Stable models don't have proper transparency | **Fixed** |
| Facial hair is buggy | **Fixed** |
| Wardrobe tab is slow | **Rewritten** — sync rebuild ~150 ms for 6 pieces |
| D4Extract as a dependency | **Removed** |

### Honest limitations (7/31/2026)

- **~10% of wardrobe appearances render incomplete.** The model parser decodes only vertex
  buffer 0; sub-objects on other buffers are dropped rather than drawn scrambled. Measured,
  documented, not yet fixed.
- **93 appearances decode perfectly but have no name**, so they cannot appear in
  name-keyed rosters. Cloth-bearing pieces recover names; plain helms/gloves/boots do not.
- **~11,500 SNOs stay locked** behind 189 TACT keys nobody has harvested. Not fixable here.
- Windows only.

---

## Data and keys

Nothing proprietary ships in this repository. On first run the tool downloads **d4data**
(community metadata) and fetches **TACT keys** from the public community list. Both live
next to the exe and are gitignored; delete the folder to remove everything.

TACT keys decrypt content Blizzard has already shipped to your client. This repo does not
distribute them.

---

## Credits

Original tool and design: **[trappuss](https://github.com/trappuss/Diablo4AssetBrowser)**.
Community tooling: **d4data** (DiabloTools) · **d4extract** (narascode, no longer
required) · **[rustydemon](https://github.com/HoldMyBeer-gg/rustydemon)** (TACT keys) ·
**d4analyzer** (reference extractions used to verify binary format derivations).

## License

MIT — see [LICENSE](LICENSE).
