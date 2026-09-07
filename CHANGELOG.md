# Changelog

Notable changes per release. The published GitHub releases are the canonical source;
this file is the same content in one place.

## 2.2.9

Rolls up 2.2.3 – 2.2.8, which were tagged but never published.

### Fixed

- **Icon, image and texture previews faded out of grid view as you moved the mouse.** Every repaint rebuilt the icon from scratch into a shared 10 MB pool, so each tab kept evicting the others'.
- **Wardrobe was missing Head, Hair style and Jewelry options for every class and gender.** Three asset groups were never downloaded — the list of what to fetch is maintained by scanning the source for literal folder names, and those three are built from a table, so nothing ever saw them.
- **Rogue male had no facial hair, stubble or eyebrows.** Its facial-hair slot is named `lambert1_skin`, a Maya default, so every substitution keyed on the name missed it.
- **Collab and store content that the community data snapshot has never described now resolves from the game itself** — appearances, materials, texture definitions and body markings. Previously these were absent with nothing on screen to distinguish that from "this does not exist".
- **Body markings the snapshot does not describe were missing from the Wardrobe.** The game ships 374 and the snapshot describes 304; the gap is where new content lands, including every Berserk Brand of Sacrifice marking.
- **Normal maps exported for Blender and Unity had an inverted green channel.** Diablo IV authors DirectX-style and both of those expect OpenGL; the flip was applied to the one preset that did not need it and skipped on the two that did.
- **Exported normal maps had an empty blue channel** and lit wrong in every DCC. BC5 carries two channels and the third is rebuilt at render time, which the exporter now does.
- **Encrypted armour pieces rendered with no roughness, metalness or AO** in the Wardrobe and Stable viewports while looking correct in Models.
- **Holes in opaque armour.** Punch-through alpha was being honoured for a BC1 format that has no cutout to express.
- **Weapons sat wrong on every class but Barbarian** — per-class hardpoint offsets were ignored, held weapons were seated at their sheath sockets, and flails lost their chain rig.
- **Hair read as flat neon plastic.** The colour array is `[shadow, highlight, mid]`, not a ramp, so walking it in order handed the brightest strands the dullest stop.
- **Paladin (female) and Spiritborn (male) had a near-empty animation list.**
- **The Catalogue missed part of a bundle's contents**, and shop products whose slot is not an item resolved to nothing.
- **Context menus drifted between tabs** — the same row could offer different actions in Models, Wardrobe and Stable.
- **Cloth**: chain links behaved like fabric instead of a pendulum, rigid garments were treated as hair, and collision capsules ignored body region.

### Added

- **Auto Animate**, driven by the game's own wardrobe AnimSets, with emotes named the way the game names them.
- **An ATTACHED animations panel** — back trophies and weapons animate alongside the body, each on its own timeline, all at once.
- **Back trophies** resolved through the item chain and seated on the body's own chest socket, with their idle clips.
- **GIF export**: turntable or clip loop, crop to model, inter-frame differencing, parallel encode, and an optimise-to-target-size mode that ships the smallest result rather than the last one it tried.
- **Still-image resolution and format**, with genuine re-rendering above 100% rather than upscaling.
- **A stencil silhouette** for viewport selection, and one shared part context menu across Models, Wardrobe and Stable.
- **Corpus-wide asset health audit** with a run-to-run diff, plus a Diagnostics menu.
- **"Only encrypted (TACT)" filter** in Models, and encrypted appearance names recovered from their cloth data.
- **Base-colour-only texture mode**, and the Marking colour each marking is authored to use is now shown.
- The d4data download fetches only the asset groups the tool reads and NTFS-compresses as it writes (~20 GB down to ~4–6 GB).

### Build tooling

- `verify-src.py` — pre-build checker, run automatically by `rebuild.bat`. Catches unbalanced delimiters, missing includes, printf argument mismatches, Qt macro collisions, duplicate lambdas and duplicate keys in a `QHash`/`QMap` initializer.
- `github.bat` — menu-driven git front end for this repo.
- `Release - Set Version.bat`, `Test - Release Smoke.bat`, `package-release.bat` — version bump across all five files, then test the zip rather than the build tree.
- `Dump - *.bat` — one-step build-run-report probes used to derive binary layouts.
- `CMakePresets.json` reads `VCPKG_ROOT` instead of a hard-coded path.

## 2.2.2

### Fixed

- **Auto Animate was not remembered.** The setting saved correctly but was never read back.
- **Switching gender cleared equipment slots.** Armour re-equipped but the cells were not refreshed.
- **Wardrobe still paused ~2 seconds on first open.** The clip-name index was built on the UI thread; it now builds in the background during startup.

### Added

- HED toggle in the Wardrobe viewport to hide the head and its attachments.
- Export scope: everything shown, items only, or items with an untextured character.
- Export the matching opposite-gender item, across multiple tools.
- Outfit filename templates using variables like Class and Collection.
- Desktop notifications when an export finishes.
- Equipping a theme pins the matching pieces to the top of each list.
- Experimental settings tab for modding and retarget options.
- FX / SIM / FORM submeshes hidden by default, with export controls.

## 2.2.1

### Fixed

- **`#tag` search matched nothing** — the `#` was never stripped.
- The log was written to the wrong directory.
- **The window froze on every launch.** The icon audit ran on the UI thread; it now runs in the background.
- **The Wardrobe took seconds to open, sometimes ~90.** It rescanned all 45,549 animation files.
- The "latest" feature conflicted across multiple game installations.
- Settings' "Game build" dropdown did nothing.
- Stale metadata after a patch — the cache is now tied to the build version.

### Added

- The executable reports its version under Properties.
- Asset update tracking per game patch.

## 2.2.0

First release under the name D4AssetBrowser.
