# Changelog

Notable changes per release. The published GitHub releases are the canonical source;
this file is the same content in one place.

## 2.4.0

### Fixed

- **The bulk queue could confuse a model with a texture.** It was keyed on the SNO alone, and a SNO is unique only within its group — so queueing appearance 12345 marked texture 12345 queued, and removing one removed the other. Keyed on group and SNO now, including the double-click selection snapshot, which was the one membership test that kept a bare number.
- **"All Warlock Textures" was deleting genuine Warlock assets.** The fix for the `Swarm`/`warm_` name collision excluded the word outright, and ShadowDemonSwarm is a Warlock ability — so six real records went with it. The colliding owners are named instead: the Vampire corpse piles, the Spiritborn bat swarm and the Druid storm cloud go, every Warlock one stays.
- **"All Paladin Textures" still returned palm trees.** The exclusion caught one spelling and left 43 Kehjistan appearances and a Tora set behind; 60 foliage records now drop, and no genuine Paladin record does.
- **A preset saved in Both mode loaded back as Textures**, silently, and swapped to the wrong queue with it.
- **The ✓ "already in folder" marker confused a model with a same-named texture** once both were in one run, which also made the run report's status column wrong. Never affected what was extracted.
- **"Also write raw game buffers" wrote no `.tex` files** for the texture half of a Both run.
- **Help ▸ Audit bulk presets reported only the model half** of a Both preset, which made a healthy preset look half-broken in the one place preset coverage is ever checked. It now sums both and prints the split.
- **Three of every four Catalogue rows had no picture.** Shop art was found only by guessing a filename from the product's name, which fits the classic store bundles and almost nothing since; rows now fall back to the product's own authored art, and then to the icon of what it contains. Season 15 goes from 12 rows with a picture to 83 of 87.
- **A collection pack listed eight helmets instead of eight armour sets.** These packs hold per-class bundles rather than items, and only the first level was ever read — so nothing resolved, nothing but the shop art exported, and every row borrowed the slot of the first piece underneath it. 311 bundles were affected; the Warcraft pack goes from 8 unresolved rows to 50 named pieces.
- **Icons stopped loading until you toggled grid view.** A thumbnail pass that ran before the index had loaded, or before the tab had ever been shown, ended for the rest of the session instead of trying again.
- **Scrolling the Catalogue grid was extremely slow.** Every pass walked all 3,837 rows to find the first visible one, several times per scroll; it now finds it directly.
- **Every pack tile read "…tion Pack".** Long names were shortened from the middle, which keeps the half they all share and discards the half that tells them apart.
- **A pack with no card art of its own showed one arbitrary piece from its contents sheet**, chosen by whichever image on the sheet happened to be largest.
- **Companions and mount trophies had no picture anywhere in the Catalogue.** Around 300 loose products author no shop imagery at all; their payload actor carries a portrait, which the item strip has used for years and the grid never asked for.
- **A collection pack’s item strip showed its class packs rather than its items**, so double-clicking one opened nothing and the per-gender split went silent. The strip, the contents tree, export and the manifest now all describe the same set of pieces.
- **New seasons no longer need a tool update to show their icons.** The filename patterns, the record fields and the binary offsets the Catalogue relied on were each a list written down once against an old game build — and the game has added a new shop-art prefix in most recent seasons. All three are now measured from the installed build every launch.
- **Exporting one item never wrote the raw source files it promised.** The menu counted them — *1 model + 37 raw files* — and the export produced the model alone. They are written now, into a `deps` folder beside the model, and the result line says how many. The list is also drawn from the same material reader the viewport uses, so it includes the encrypted materials a direct read of the game's JSON cannot open.
- **"Bake detail maps" was ignored when exporting a single item**, though a whole-outfit export honoured it — so the same piece came out with different surface detail depending on which menu was used.
- **Cancel in Settings could not undo a change to the weapon panel.** Its six options write the moment they are changed, which is what makes the viewport update live, but they were not on the list Cancel restores.
- **Three Wardrobe settings were written or read by nothing.** Two recorded a weapon-type dropdown that the icon browser replaced, and were saved into every ensemble you kept; the third restored a dye picker that has not existed since pigments went per-slot. An existing profile still gets its stale values cleared.
- **Right-click did nothing on the Stable tab's mount cards until the roster finished scanning.** On a cold cache that is the first minute of the tab, and it is exactly when Equip, Export and the Copy actions are most wanted. The cards answer a right-click from the moment they appear now; the theme actions still wait for the scan, because they need the item data it produces.
- **After a game update, the Stable picker briefly kept the old build's cards.** They looked live and could be clicked, which added an undo step and re-ran the assembly for a mount that no longer existed. The grid is taken down with the rest of the stale data and says it is reloading.
- **The icon audit reported "0 rows filled by the portrait" for a route that visibly fills rows.** It asked each row about its own payload, and a bundle has none — its contents do. It walks the same contents the tab walks now, reports all four picture routes in the order the tab tries them, and says which rows it still cannot check and why.
- **The Catalogue grid could not find a picture for weapons, mounts, trophies or companions through its contents.** It asked for appearances by the armour naming convention alone, which matches helm, chest, gloves, pants and boots and nothing else — while the item strip beside it, one function away, had already been moved onto the resolver that handles both conventions.
- **A collection pack's row said "8 items" above a pane listing 50.** The strip, the contents tree, the export and the manifest were all moved onto the pack's real pieces; the row in the list was not. Filtering by contents and searching missed them for the same reason — a search for a piece could not find the pack that sold it.

### Added

- **Filter the Catalogue by class and by slot.** Both are the game's own authored fields, asked of what a bundle contains rather than of the bundle, and both dropdowns are built from the data — so a class or slot a later patch adds appears without a tool update, each labelled with how many rows it returns.
- **"Reward only" finds the cosmetics that were never sold.** The shop's own *requires* relationship, which is how a mount trophy turns out to have been a Season 3 pass reward rather than a purchase.
- **"Also sold in", on any piece.** Right-click an item in the strip or the contents tree for every other product that carries it, with its season, and one click to go there. The tool has been building that map for releases; only the Models tab ever asked it.
- **Ctrl+F focuses the Catalogue search, Esc clears it** — the same keys the Models tab has always had.
- **Bulk Extract has a third mode, "Both".** One NAME query, run over models and textures in a single pass: models into the folder layout, textures into a `textures\` subfolder beside them. It is the route to loose maps no material binds — fur masks, dye masks and ramps, atlas sheets, recolour variants — which a models-only run never produces. The tag filters reach the model half only, because textures carry no tags, so the count reports the two halves separately rather than one total, rows carry a `[model]` / `[tex]` tag, and each mode keeps its own queue.
- **Eleven "— Everything" presets**: one per class, plus Global & Base, Mounts and Back Trophies. 34 built-ins in total.
- **A blank Catalogue row now says why it is blank** — the record is locked, no art was ever authored for it, or its only images belong to a whole family of products rather than to it.
- **"View in Models" from the Catalogue list.** It existed only after opening a bundle and right-clicking its contents, which is the step the tab is meant to remove.
- **The Catalogue's art pane is a viewer now.** Scroll to zoom about the cursor, drag to pan, double-click to fit; right-click for the same export and copy actions the lists offer. It was a fixed, pre-scaled picture, which meant a 5120x2160 shop banner was shown at 400px and could not be read.
- **Any of a bundle’s images can be shown there.** Picking a row under *Bundle images* loads it at full resolution — the tree has always listed them by name, and the only way to see one was to leave for the Textures tab. A corner strip names what is on screen with its pixel size and zoom.

### Build tooling

- `Probe - Catalogue Icons.bat` — reports Catalogue icon coverage from the app's own index files, with no rebuild and without launching the app.
- `Test - Icon Audit Compare.bat` — runs the icon audit twice, once with the diablo4.dad copy forced and once without, and prints the two summaries side by side. It is what settled whether that step is carrying the icons or hiding a broken route: without it, 311 appearances resolve no icon at all and 30 wear another class's.
- `verify-src.py` rejects a file with MIXED line endings — always a half-finished edit, whichever direction it went, and the one ending fault nothing else catches. Whole-file CRLF/LF is deliberately *not* policed: git normalises that on commit, so a baseline of the working tree would fail the build on a state git itself produced.

## 2.3.0

### Fixed

- **The Paladin wolf set's chest disappeared when HED was switched off.** A pauldron ornament material named `wolfHead` matched the head test, and the head grouping then took every part of the torso with it.
- **A back trophy that crashed on load crashed again on every launch.** Crash recovery cleared a setting the Wardrobe never wrote, so the trophy was restored untouched each time.
- **Loading a saved look could leave the previous skin tone on screen.** Skin tone and skin detail were stored by their menu label rather than their value, so a look saved with neither left the old one selected — and the next save recorded it as absent.
- **The Subsurface slider read 15 on a fresh profile while the viewport rendered at 24.** Two defaults for one setting; the first nudge produced a jump nothing on screen accounted for.
- **"Export model to last folder" on a look card named a folder it did not use**, and opened a file dialog anyway when that folder was unset.
- **Hiding the collision model left the other copy of the checkbox showing the opposite.** Both panels drive one setting and were never mirrored; the Physics copy also ignored the master overlay toggle.
- **Textures column layout was saved on every change and never read back.** Un-hiding NAME or COLLECTION lasted until you closed the app.
- **Restore Defaults reset the Wardrobe viewport only.** Models and Stable kept whatever they had, and the Stable tab appeared nowhere in Settings at all.
- **Basilisks and several mounts listed no animations.** Their clips are owned by a storefront appearance, not the base one the lookup assumed exists.
- **13 of 48 pets saw none of their animation clips and 15 more saw only some.** The game names pet clips two ways — per appearance and per species — and only one of the two was ever matched.
- **Help ▸ Shortcuts still described the old viewport.** It told you to double-click to select a part, which stopped being true the moment single-click selection landed, and it listed the viewport as Models and Wardrobe only when Stable has had one for releases.

### Added

- **Select parts directly in the 3D viewport, several at a time.** A left click picks the part under the cursor; Ctrl or Shift adds to the selection or takes a part back out; clicking empty space clears it. Right-clicking a part that is already selected acts on the whole selection rather than replacing it, and the outline turns blue over all of it, so what is highlighted and what the menu is about to do can never disagree. The menu counts what it has — *Export 3 parts (5,120 tris)…*, *Frame 3 parts*, *Isolate 3 parts*, *Copy 3 material names* — and drops the piece name from its title when the selection spans more than one piece. Works the same in Models, Wardrobe and Stable; double-click is now framing only in all three.
- **Help ▸ Find SNO** — an id or part of a name in; group, name, collection, how many appearances use a material, and whether it arrived in this game build.
- **Help ▸ Patch contents** — what each game build added since the tool first opened on it, grouped by asset type and named.
- **Help ▸ Diagnostic output** — the probe files written beside the exe, newest first with size and age, readable without leaving the app.
- **"Explain this material" now lists every appearance that uses it**, which is how you tell a piece's own material from one shared across a set or across both genders.
- **Clear Stable memory** in Settings, and a Stable animations panel with its own toggle.
- **Sort by SNO** in the Catalogue.

### Documentation

- **The wiki is now reachable from the README**, which it was not — the only route to it was the repo's own Wiki tab. The README opens with a documentation index, every tab in its table links to the page that covers it, and each major section ends with a pointer to the longer version.
- **Six new wiki pages.** *Keyboard & mouse* — every binding, including the viewport ones written on no button, taken from the source rather than from memory. *Settings* — what each of the nine tabs holds, and the three options that change exported results rather than presentation. *Glossary* — the vocabulary the rest of the documentation assumes, from SNO and CoreTOC to dye zones and the implied Z. *FAQ*. *Building from source*, which the wiki's own landing page had been linking to without it existing. Plus a sidebar and footer, so no page is a dead end.
- **The existing pages were rewritten to cross-reference each other.** Troubleshooting now routes by symptom, Diagnostics opens with a table of which report answers which question, and the six-tab page carries the detail you need while using the app rather than a summary of it.
- All 129 internal links are checked against the headings they point at.

### Build tooling

- `verify-src.py` — three checks added: settings keys written and never read, combo boxes saved by their display label, and character-versus-equipment decided by a material name.
- `Dump - Piece Roster.bat` — writes the material roster every equipped piece resolves, by both routes, with each part's classification.

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
