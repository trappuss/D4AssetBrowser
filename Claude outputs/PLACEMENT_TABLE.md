# Placement table — tiering `ASSETBROWSER_TEMPLATE.md`

Every discrete rule in the 567-line document, with its proposed tier, its portability mark,
and whether it splits out of its current section. **Rows marked ❓ are contentious and are
being asked about separately — nothing is written until they are settled.**

Baseline inventory of the current document, measured: **131 discrete items** — 71 bullets,
17 numbered items, 22 checkboxes, 21 table rows — across 18 sections, plus prose-only rules in
§7, §11, §12 and §14 that are not bullets today and become items here (so the new document's
item count will be *higher*, never lower).

Portability key: **U** copy as-is · **A** same mechanism, engine-specific data · **E** D4's
answer to a question another engine may not ask.

---

## Proposed section map

| New § | Section | Tier | From |
|---|---|---|---|
| — | Front matter: what this is · how to use it · the four tiers · the portability key · engine-specific vs generic · the minimum viable build path | — | intro + §14 |
| 1 | Product identity | 1 Fundamentals | §1 |
| 2 | Architecture skeleton | 1 | §2 |
| **3** | **Cross-cutting conventions (all ten, unsplit)** | 1 | §3 — *keeps the number 3 deliberately, so all six `§3.x` sub-references survive unchanged* |
| 4 | The asset list and the one search matcher | 1 | §4 part |
| 5 | The 3D viewport — one shared widget | 1 | §5 part |
| 6 | Single-model export | 1 | §9 part |
| 7 | The build and verify loop | 1 | §13 part |
| 8 | Filters: funnel, facets, chips | 2 Essentials | §4 part |
| 9 | Shading modes, channel viewer, overlays | 2 | §5 part |
| 10 | Part selection is a set | 2 | §5 part |
| 11 | The panel set | 2 | §6 part |
| 12 | The Textures tab | 2 | §7 part |
| 13 | Bulk extraction — the run ❓ | 2 | §8 part |
| 14 | Export options, output layout and name templates | 2 | §9 part + §8's ExportLayout block |
| 15 | The Settings dialog | 2 | §10 part |
| 16 | Context menus | 2 | §12 |
| 17 | The tool explains itself — the reports you cannot debug without ❓ | 2 | §16 part |
| 18 | Documentation is part of the product ❓ | 2 | §17 + §13's link checker + log console |
| 19 | QoL checklist — the tier-3 index | 3 Quality of life | §15 |
| 20 | Browse views: Outliner and Grid | 3 | §4 part |
| 21 | Viewport polish: Rendered mode, popovers, fullscreen | 3 | §5 part |
| 22 | The panel machinery | 3 | §6 part |
| 23 | Textures tab extras | 3 | §7 part |
| 24 | Bulk run machinery ❓ | 3 | §8 part |
| 25 | The GIF budget ladder | 3 | §9 part |
| 26 | Retarget and modding presets | 3 | §9 part |
| 27 | Settings extras: Information tab, profiles | 3 | §10 part |
| 28 | Hotkeys | 3 | §11 |
| 29 | Audit, release and housekeeping tooling | 3 | §13 part |
| 30 | The remaining self-explanation reports ❓ | 3 | §16 part |
| 31 | Releasing | 3 | §18 — *see the note at the bottom: this one resists the scheme* |
| 32 | The game-specific showpiece tab ❓ | 4 Showpiece | **no existing section** |

---

## §1 Product identity → new §1, Fundamentals

| Item | Tier | Mark | Note |
|---|---|---|---|
| Name `(GameName\|Engine)AssetBrowser`; one native exe, no Python, no external extractor processes, no runtime downloads except community data | 1 | U | |
| Portable absolutely: everything in `data\` beside the exe, INI via QSettings never the registry, caches/thumbnails/logs there, superseded cache versions pruned at startup | 1 | U | Expensive to retrofit — the reason it is tier 1 |
| The deliberate exception: export dialogs default to Documents/Pictures | 1 | U | |
| Reads the installed game directly, no "extract first" step | 1 | A | The storage is the engine's (CASC, QAR/FPK, …) |
| Nothing proprietary in the repo: no game assets | 1 | U | |
| Keys and community metadata fetched at runtime and gitignored; state it in the README | 1 | E | Underlying question: *does your game lock content behind keys, and is there a third-party metadata dump at all?* |
| Honesty is a feature: README *Honest limitations* with measured numbers; errors say what failed and why; optimizers report "target not reachable" | 1 | U | The two measured numbers ("~10% render incomplete", "93 decode perfectly but have no name") stay verbatim |
| Fail closed and read the authoritative source | 1 | U | |
| Where a community dump and the game disagree the game wins, and the tool says which answered; dump lag is exactly where new content lives; a sparse checkout looks identical to "ships nothing here"; both shipped as user-visible bugs | 1 | E | Underlying question: *is there a second, lagging source of truth for names and metadata?* If not, this whole rule is moot |
| Distinguish **present · unnamed (encrypted) · absent** in the UI, not just internally | 1 | E | Underlying question: *can a record exist in your storage but be unreadable?* |

## §2 Architecture skeleton → new §2, Fundamentals

| Item | Tier | Mark | Note |
|---|---|---|---|
| The `src/` tree (app · \<store\> · index · model · tex · gl · tabs · util) | 1 | A | `casc/` is D4's store directory name |
| **One viewport class** — every 3D tab uses the same `GLModelWidget`; features land once | 1 | U | Retrofitting this is a rewrite |
| Split, don't grow: `Tab_Panels.cpp` / `Tab_Export.cpp`, and remember all three exist before declaring a function absent | 1 | U | |
| One background-index shape: detached thread → disk cache signed with the file counts of every directory read **plus the game build id** → `install()` posted back via `Qt::QueuedConnection` → a `readyChanged` signal → `reset()` wired to the data-fingerprint change → a generation counter so an in-flight build discards itself | 1 | A | The fingerprint inputs and build id are the engine's |
| Crash-prone paths SEH-guarded (GPU submissions, binary parsers) so one bad asset reports instead of killing the app | 1 | A | The guard primitive is platform-specific; the rule is not |
| `see §14` in the `util/` line | — | — | **Content error:** §14 (porting guide) says nothing about anchored includes; the anchored-`#include` rule is `verify-src.py`'s, in §13. Repointed at the new §7 |

## §3 Cross-cutting conventions → new §3, Fundamentals, **all ten, unsplit**

Kept whole and kept at number 3, so `§3.1`, `§3.2`, `§3.3`, `§3.5`, `§3.8`, `§3.10` resolve
unchanged. Every one is a convention: cheap to adopt on day one, expensive to retrofit, which
is the tier-1 test.

| Item | Tier | Mark | Note |
|---|---|---|---|
| 3.1 One setting, one key; a key only read and never written is a dead key and a bug | 1 | U | |
| 3.2 Persist the stable identity, not the label or index; restore with `findData`; `ExportLayout.h` as the worked example, unknown values fail safe, index-keyed setting migrated once then removed | 1 | U | |
| 3.3 Overlay master gate: one `m_overlaysOn` per tab, all overlay state through `reapplyOverlays()`, settings replays never call `setShow*()` | 1 | U | Applies as soon as overlays exist (new §9) |
| 3.4 Shared menu builders (`ViewportPartMenu.h`, `addRowImageActions()`, `addRowExportCopyActions()`); extend, never fork | 1 | U | What makes new §16's uniformity cheap |
| 3.5 Diagnostics permanent and env-gated (`D4_DUMP_CLOTH`-style), bounded and throttled, left in the code | 1 | A | The variable names are the tool's |
| 3.6 Defaults are chosen, not inherited; ON only if it is what the viewport already shows | 1 | U | |
| 3.7 QSettings namespaces per tab; a bad default written once needs a versioned migration | 1 | A | The namespace names are the tool's tabs |
| 3.8 **Classify by authored data, never by a name substring** — with all three scars verbatim: `head` matching `wolfHead` and hiding a whole torso, the `primSlot >= 0` fix, `trophy_*` vs `back_*` returning six placeholders and zero real trophies, `Hero_eyes_mat` vs `global_eyeball_mat`; unavoidable name tests get a comment and a `verify-src.py` case | 1 | U | The rule is universal; the three examples are D4's and stay as prose |
| 3.9 Batch widget writes with signals blocked then recompute once — sixty visibility passes on a sixty-part outfit; two surfaces, one named slot, never `emit otherWidget->someSignal()` | 1 | U | |
| 3.10 Reset by removing keys, not by writing defaults over them; keep-prefixes list for user-authored state (presets, camera bookmarks, cloth setups); `ViewportSettings.h` worked example | 1 | U | |

## §4 Browse tab → splits three ways

| Item | Tier | Mark | New § | Note |
|---|---|---|---|---|
| **List** view — dense flat rows | 1 | U | 4 | The minimum: you cannot browse without a list |
| One search box, one syntax, driving every searchable tab | 1 | U | 4 | |
| Syntax row: `word` substring, case-insensitive, name/tags | 1 | U | 4 | |
| Syntax row: `123456` asset id | 1 | A | 4 | SNO or the engine's equivalent |
| Syntax row: `#tag` tags/title/collection only, not the filename | 1 | A | 4 | Needs a tag taxonomy |
| Syntax row: `c:collection` reads to end of line, put it last | 1 | E | 4 | Underlying question: *does the game author named sets an asset belongs to?* Drop the token if not |
| Syntax row: `-term` exclude, works with every form | 1 | U | 4 | |
| Syntax row: `a\|b` OR within one term | 1 | U | 4 | Needed by the factory presets in new §24 |
| Syntax row: space = AND | 1 | U | 4 | |
| **The one-matcher rule**: every filtering site parses through one shared helper (`QueryTerm.h`) with a startup self-test of ~10 canonical cases — three hand-rolled parsers drifted and Bulk Extract exported a different set from the list the user filtered | 1 | U | 4 | The scar stays verbatim |
| `Ctrl+F` focuses, `Esc` clears, `↓` recalls the last ten searches | 3 | U | 20 | Also a checklist line in new §19 |
| **Outliner** view — scene tree; part nodes select in the viewport and vice versa | 3 | U | 20 | Mirroring rule cross-refs new §10 |
| **Grid** view — thumbnail tiles rendered by the tool and cached in `data\` | 3 | U | 20 | |
| Display dropdown in the header switching the three views | 3 | U | 20 | Only needed once there is more than one view |
| Funnel popup that stays open while you tick; grouped tag checkboxes (Category/Class/Gender/Type) | 2 | A | 8 | The groups are the engine's taxonomy |
| **Match any (OR)** toggle | 2 | U | 8 | |
| Facet: only-decrypted / only-encrypted | 2 | E | 8 | Underlying question: *does the engine encrypt records?* |
| Facet: hide un-renderable | 2 | A | 8 | |
| Facet: **Latest** (new this game update) | 2 | E | 8 | Underlying question: *can the tool observe game builds over time?* Depends on new §30's patch-contents observation |
| Facets: Animated · Rigged · Orphaned | 2 | A | 8 | |
| Every active filter shows as a **removable chip**; active filters tint the funnel icon | 2 | U | 8 | |
| Hover previews and icon indicators, settings-gated (Interface) for slow machines | 3 | A | 20 | |

## §5 The 3D viewport → splits three ways

| Item | Tier | Mark | New § | Note |
|---|---|---|---|---|
| One shared widget, orbit/pan/zoom camera, flat shading — *get one model drawn* | 1 | U | 5 | |
| Shading-ball row top-right, Blender-style: Wireframe · Flat · Shaded | 2 | U | 9 | |
| **Rendered** mode (IBL, shadows, SSAO, tonemap) | 3 | U | 21 | Skip unless the tool is expected to make presentable images |
| **Channel viewer** — Base Colour, Normal, Roughness, Metallic, AO, Emissive, cycled by scrolling the `⌄` or picked from its menu; "the single best material-debugging feature; every tool gets it" | 2 | A | 9 | The channel set follows the engine's material model |
| Overlays: statistics · ground grid · skeleton | 2 | U | 9 | All behind the master gate, §3.3 |
| Overlays: hardpoints | 2 | A | 9 | |
| Overlays: collision shapes · physics bones (anchored grey / simulated orange) | 2 | E | 9 | Underlying question: *does the engine author per-asset physics and collision?* |
| Overlays: axis gizmo · bone names | 3 | U | 21 | |
| Submesh class toggles (FX / SIM / GIB in D4) | 2 | E | 9 | Underlying question: *does the engine tag submeshes by class?* |
| Popovers, not dialogs, for Graphics / Camera / Lighting / engine extras — small floating panels beside the button that opened them | 3 | U | 21 | Pigment is D4's extra — E |
| Fullscreen with a floating `✕ Exit fullscreen` button; never trap the user behind an unread hotkey | 3 | U | 21 | |
| Frame-selected | 2 | U | 9 | |
| Snap-to-slot where slots exist | 2 | E | 9 | Underlying question: *does the engine define equipment slots?* |
| Single click picks the part under the cursor; empty space clears; **Ctrl or Shift** adds or removes | 2 | U | 10 | |
| **Double-click frames** and does not touch the selection | 2 | U | 10 | |
| Selection mirrored both ways with the tab's parts surface — one state seen twice | 2 | U | 10 | |
| Right-click inside the selection acts on the whole selection; outside it replaces the selection with that part first; the set the menu acts on is the set that turns blue | 2 | U | 10 | |
| Menu labels count what they have (*Export 3 parts (5,120 tris)…*, *Frame 3 parts*, *Isolate 3 parts*, *Copy 3 material names* de-duplicated); a title naming one source piece drops the name when the selection spans several | 2 | U | 10 | |
| Scar 1: Qt delivers press → release → DoubleClick → release, so a release-handler click fires twice; with Ctrl held that reads select → clear-and-select → toggle-off and the selection is gone. Swallow flag in `mouseDoubleClickEvent`, consume the second release, **clear the flag on every press** or a sub-threshold drag eats the next real click | 2 | U | 10 | Moves whole |
| Scar 2: one gesture, one job — once single click owns selection, a double-click that also selects is two behaviours fighting over one event | 2 | U | 10 | Moves whole |
| Scar 3: clear the picked set when geometry changes — a stale but in-range index outlines the wrong part, and a picked set makes it a wrong *set* | 2 | U | 10 | Moves whole |

## §6 Panel system → splits two ways ❓*(was flagged contentious; resolved as the handoff proposed)*

| Item | Tier | Mark | New § | Note |
|---|---|---|---|---|
| Standard panels to offer per tab: PARTS (per-part triangle counts, slot, material, visibility checkboxes) · MATERIALS · TEXTURES/TEXTURE PREVIEW (channel tiles with wheel-resizable hover zoom) · INFO (filename, id, tags, size, LODs, bones, counts, what-uses-it, clickable variant links) · ANIMATIONS | 2 | A | 11 | The panel *contents* are how anyone reads an asset — tier 2. Engine extras (CLOTH, ATTACHMENTS) are **E** |
| Collapsible panels in a splitter; vertical icon strip toggles each; `▲ ▼` reorder, `✕` hide, `»` collapses the column to the strip | 3 | U | 22 | |
| A newly opened panel takes its height from the slack of the panels already up, not an equal split; a drag can never fully erase a panel | 3 | U | 22 | |
| Layout remembered per tab behind one global setting via `PanelPersist.h`: `bind()` called AFTER the code-set default `setSizes()`, nothing written when the setting is off | 3 | U | 22 | |

## §7 Textures tab → splits two ways

| Item | Tier | Mark | New § | Note |
|---|---|---|---|---|
| Every texture in the game, decoded in-tool | 2 | A | 12 | Block formats are the engine's |
| **Channel isolation** (RGB · R · G · B · A) | 2 | U | 12 | |
| Scroll zooms, drag pans, double-click resets | 2 | U | 12 | |
| Filter by format, by the tags of the assets that *use* the texture, orphans-only | 2 | A | 12 | |
| **Alpha checkerboard** | 3 | U | 23 | |
| Cubemap / array face selector | 3 | E | 23 | Underlying question: *does the engine ship cubemaps or texture arrays the browser must page through?* |
| **Pixel inspector** reporting `(x,y) RGBA` under the cursor | 3 | U | 23 | |
| Atlas/sprite support: list the packed frames, export each with optional trim-to-bounds | 3 | E | 23 | Underlying question: *are small images packed into atlases rather than shipped individually?* |
| **ASSOCIATED MODELS** walks texture → material → model and jumps to the Browse tab | 3 | A | 23 | |
| Drag the image straight out into another application | 3 | U | 23 | |

## §8 Bulk Extract → ❓ splits two ways, pending the tier-by-intent question

| Item | Tier | Mark | New § | Note |
|---|---|---|---|---|
| Filter the whole index with the same search + funnel as Browse, watch the **match count update live**, export everything in one run | 2 ❓ | U | 13 | Tier 1 if the tool's purpose is ripping — see the question |
| The tab holds ONLY run controls; every *export option* lives in Settings ▸ Export, shared with every other export path. One setting, one key, one home | 2 | U | 13 | |
| **Pick items manually** → a persistent **Queue** surviving filter changes, mode switches and restarts | 3 | U | 24 | |
| **Only new** via a `_bulk_manifest.json` ledger in the output folder; or **Overwrite** | 3 | U | 24 | |
| **Parallel** workers (auto = core count) with a live console, working **Cancel** (and `Esc`), **Pause/Resume that excludes paused time from the ETA** | 3 | U | 24 | Weeks of work; skip unless runs are long enough to abandon |
| Failures to `_bulk_failed.txt` with a reason each; one bad asset never takes the run down | 3 | U | 24 | |
| **Factory presets** where the engine has natural families ("all customization for a class") | 3 | E | 24 | Underlying question: *does the game group assets into families a user would want wholesale?* |
| **Run-scoped decode cache**: bounded, mutex-guarded, keyed on `id\|name\|variant`, RAII `TextureCacheScope`, opt-in per thread, zero disk footprint | 3 | A | 24 | |
| **Output layout** (`ExportLayout.h`): Flat · by class · by type · by model; every *batch* path obeys it, every *single-model* path deliberately ignores it (`Ctrl+E` becoming `Barbarian\foo.glb` is a surprise the caller can't undo); the layout picks the group folder only so Flat is "one group at the root", names sanitized for trailing dots and Windows reserved device names, unknown stored values fail to Flat, un-taggable items go to `_misc` never loose into the parent | 2 | A | **14** | **Moves** out of Bulk and into the export section, where every path that obeys it lives |

## §9 Exporting → splits four ways

| Item | Tier | Mark | New § | Note |
|---|---|---|---|---|
| Models → rigged, animated `.glb` | 1 | U | 6 | |
| **Exports exactly what's visible** — hidden parts stay out unless the user explicitly picked a subset, which is then written verbatim | 1 | U | 6 | |
| When exporting a subset, **renumber material indices** — exporters that resolve materials by index quietly drop parts off the end otherwise | 1 | U | 6 | |
| **Name templates**: `{{FileName}}`, `{{SNO}}`/`{{Id}}`, `{{FrameIdx}}`, `{{FrameName}}`; one engine (`NameTemplate.h`) applied everywhere, with path-separator and reserved-name sanitizing | 2 | A | 14 | The token set names the engine's ids |
| **Drag-out**: models drag from the list into Blender/Explorer; drag-out rebuilds its own paths and stays exempt from output layout | 2 | U | 14 | |
| **Animation libraries** — skeleton + selected clips, no mesh, for retargeting | 2 | A | 14 | |
| **Images** — PNG/JPEG/WebP, 25–400% where above 100% the scene is **re-rendered larger, never upscaled**; transparent background as a native-alpha single render; crop-to-model | 2 | U | 14 | |
| **Completion notifier** (`ExportNotifier`): every export path reports what it wrote and where, click-to-open | 2 | U | 14 | |
| **Modding/retarget presets** (Settings ▸ Export ▸ Advanced): engine presets (Blender/Unreal/Unity — unit scale + normal convention), readable bone names, `.L`/`.R` mirror names, humanoid rig reduction, hardpoints as empties | 3 | A | 26 | Skip unless people will take the exports into a DCC app |
| **GIFs** — turntable and animation-loop; copy the optimizer verbatim from `ExportCapture.cpp`, "because the naive version was wrong three separate ways" | 3 | U | 25 | Skip unless anyone will make turntables |
| GIF 1: capture frames ONCE; retries only re-encode | 3 | U | 25 | |
| GIF 2: budget ladder in order — palette (×0.75 steps to a 32-colour floor) → dither off (at a coarse palette, killing the Bayer pattern is often the biggest single saving; dither amplitude scales with palette coarseness, so cutting colours with dither on can make files *bigger*) → aimed downscale, one pass at `sqrt(target/actual) × 0.93`, floored at 96px, ≤5 passes — never nibble in 15% steps | 3 | U | 25 | Moves whole |
| GIF 3: ship the smallest attempt, not the last one, and say so in the log — "TARGET NOT REACHABLE — shipping the smallest encode" | 3 | U | 25 | |
| GIF 4: turntables snap to whole animation loops so orbit and pose wrap together, and run a warm-up lap so physics settles before the first captured frame | 3 | U | 25 | |

## §10 Settings dialog → splits two ways

| Item | Tier | Mark | New § | Note |
|---|---|---|---|---|
| Top-level tab order and its philosophy: *setup → presentation → per-area pages → what lands on disk → keys → upkeep → reference → experimental* (General · Interface · Models · Wardrobe · Export · Hotkeys · Maintenance · Information · Experimental, with their groups) | 2 | A | 15 | The per-area pages are the tool's tabs |
| **Export gets sub-tabs** (Models · Images · per-tab options · File names); a page past ~4 groups splits into sub-tabs too | 2 | U | 15 | |
| Every tab is a `QScrollArea` (`makeTab` lambda) so no page clips | 2 | U | 15 | |
| **Never elide tab labels**: `setElideMode(Qt::ElideNone)`, `setExpanding(false)`, scroll buttons as the safety net, `showEvent` folds the tab bar's full width into the requested size — "General" once rendered as "eneral" | 2 | U | 15 | Scar stays |
| `&&` in `QGroupBox` titles (a bare `&` becomes a mnemonic and eats the letter) | 2 | U | 15 | |
| **Caches & reset**: one button per cache with its size shown, plus reset-all | 2 | A | 15 | |
| **Restore Defaults is scoped and complete** — resets the group in front of you, *every* viewport tab has a group, every tab with viewport state appears in Settings at all; a reset that covered one tab and not the others shipped in D4 and read as "the setting did not stick"; implemented by removal per §3.10 | 2 | U | 15 | |
| **Tooltips on every option**, answering "when would I want this?", not restating the label | 2 | U | 15 | |
| **Information is a real tab**, in sub-tabs (Reading the game · Models · Materials & textures · Icons · Names & files): plain-language explanation plus **side-by-side tables for options that look interchangeable but aren't** (D4's two loose-texture options). Explaining in-tool beats a wiki nobody opens | 3 | A | 27 | Skip unless the option set has become confusable — that is what it is for |
| **Settings profile** group: export/import the INI | 3 | U | 27 | |

## §11 Hotkeys → new §28, Quality of life

| Item | Tier | Mark | Note |
|---|---|---|---|
| One central registry (`Hotkeys.h`): a `{key, label, default}` table shared by the Settings editor (writes) and MainWindow (reads/applies); all rebindable; adding a shortcut = adding one row | 3 | U | The registry is cheap and worth copying on day one even though the keys are polish |
| Shipping defaults: `Ctrl+E` export selection · `Ctrl+Shift+E` export to last dir · `Ctrl+Shift+A` animations only · `Ctrl+Shift+I` save image; unbound slots for the GIF exports | 3 | A | |

## §12 Context menus → new §16, Essentials (whole)

| Item | Tier | Mark | Note |
|---|---|---|---|
| Full specification is `docs/CONTEXT_MENUS.md` — read that file before building any menu; this section is the summary | 2 | U | |
| The same object offers the same actions wherever it appears — list row, grid tile, outliner node, parts panel, viewport click all raise the one shared menu | 2 | U | Uniformity is what makes the tool learnable |
| Canonical asset action set: Load/preview · Copy image · Save image(s) [to last folder \| …] · Render icon(s) · Export N model(s) [to last folder (…/path) \| …] · Copy SNO · Copy file name · Copy name · Copy collection name · Variants ▸ · Show dependencies… | 2 | A | "Copy SNO" and "Copy collection name" name D4's ids — **E** individually |
| Canonical part action set: export model/part × (last folder \| prompt) · the copy block · Frame part · Select part · Hide/Show part · Isolate part · Show all · Hide all · Invert | 2 | U | |
| Every detail table gets Copy / Copy all (`Ctrl+C`) via `CsvCopy` | 2 | U | |
| The four laws: one `MenuText` vocabulary · one builder per menu family · never show an action that cannot be performed · the label states subject, count and destination | 2 | U | |
| Right-clicking outside the selection acts on the clicked row, inside it on the whole selection; single-subject actions (Copy image) always take the *clicked* row | 2 | U | |
| The same rule governs viewport parts, plus: the acted-on set is what the viewport highlights, computed once and used for both | 2 | U | Cross-refs new §10 |
| Two part-selecting surfaces: consult most-specific first; "select these" routes to whichever surface can show several at once | 2 | U | |
| `MenuText::parts(n)` / `verbParts(verb, n)` give one plural rule for every builder; options that only make sense for one part (*Explain this material*) are omitted, not disabled, above `n == 1` | 2 | U | |

## §13 Tooling → splits two ways

| Item | Tier | Mark | New § | Note |
|---|---|---|---|---|
| Double-clickable `.bat` entry points, one job each, no babysitting — each builds what it needs, runs, writes a report, exits | 1 | U | 7 | |
| `build.bat` (cold) / `rebuild.bat` (kill exe → snapshot `src` to `.Backups\` → `verify-src.py` → build → distilled `build_errors.txt` → launch) / `clean-rebuild.bat` | 1 | A | 7 | |
| **`verify-src.py`** — port it early, it pays for itself in the first week; eleven checks over ~140 files in seconds: zero-byte files from a botched write · unbalanced `{} () []` · header-only helpers used without a real anchored `#include` (a comment mentioning the path must NOT satisfy the check) · printf format-vs-arg mismatches · locals named `emit`/`signals`/`slots` · duplicate lambdas · duplicate map keys · metadata-coverage baseline · settings keys written and never read · combos persisted by display text · classification by name substring | 1 | U | 7 | The last three exist because §3.1, §3.2 and §3.8 were violated in shipped code |
| "A convention you cannot check is a convention you will lose" — every §3 rule that can be grepped becomes a check; rules with legitimate exceptions report a reviewed count rather than failing | 1 | U | 7 | |
| A log console in-app with Help → Export log; the tool never writes logs unasked | 2 | U | 18 | Moves to the documentation/explain-yourself side, where the user-facing half of diagnostics lives |
| A documentation link checker over the README and `wiki/*.md`, run before publishing | 2 | U | 18 | Moves into the documentation section it serves |
| `Audit - Asset Health.bat` — walks every asset, classifies renderability (OK / no-textures / no-geometry / locked / no-data) and **diffs against the previous run**, so a patch reports "newly working / newly broken" instead of letting regressions age into bug reports | 3 | A | 29 | |
| `github.bat` — menu-driven commit/push/release front end at the repo root; never `git init`; refuse to run if `.git` is absent | 3 | U | 29 | |
| `release-notes.py` — extracts one version's section from `CHANGELOG.md` into the release body, with real exit codes; a shell one-liner here published an empty release | 3 | U | 29 | |

## §14 Porting guide → front matter (not a tier)

| Item | Goes to | Mark | Note |
|---|---|---|---|
| What is engine-specific (storage layer, binary parsers, id scheme, tag taxonomy, the game-specific tab) | Front matter | — | |
| What is generic, the copy-then-delete-D4-isms list (viewport + overlays + channel viewer + part-selection model, panels + PanelPersist, QueryTerm + search box, funnel+chips, Bulk run machinery, ExportLayout, NameTemplate, Hotkeys, ExportCapture, ExportNotifier, SettingsDialog skeleton + ViewportSettings reset-by-removal, ViewportPartMenu + plural vocabulary, the self-explanation menu items, verify-src.py, the documentation skeleton + link checker, the .bat suite including release-notes.py) | Front matter | — | Every cross-reference in it is renumbered |
| Porting order that works (storage+index → viewport flat → Browse views+search+filters → materials/textures+channel viewer → panels → single-model export → Textures → Bulk → settings polish/hotkeys/Information → self-explanation → documentation) | Front matter | — | Becomes the spine of the **minimum viable build path**, which stops at the tier-2 line and says what it leaves out |
| "Resist building the game-specific showpiece tab first; every one of them stands on the Browse+viewport foundation" | Front matter + tier 4 opener | — | |
| The two commonly-deferred-but-shouldn't-be arguments (§16 pays for itself the first time something renders wrong; §17's skeleton is cheap while the tool is small, retrofitting means twelve pages at once) | Front matter | — | These are the arguments behind the ❓ questions on new §17 and §18 |
| "When asking a session for a feature, name it from this document and point at the D4 file" — *Bulk Extract per template §8 — copy the manifest/only-new mechanism from BulkExtractorTab.cpp* gets the finished design; "add bulk export" gets a guess | Front matter | — | The example's § is renumbered |

## §15 QoL checklist → new §19, the tier-3 index

All 22 checkboxes survive verbatim as the tier-3 index. Four of them describe rules that live
in tiers 1–2; they keep their box and gain a pointer, so nothing is lost and nothing is
mis-tiered:

| Checkbox | Rule lives in |
|---|---|
| Live match-count while filtering; chips for active filters | tier 2, new §8 |
| Viewport part selection is a set (click, Ctrl-click, right-click scoped to it) | tier 2, new §10 |
| Startup self-tests for invariants that would otherwise fail silently (QueryTerm) | tier 1, new §4 |
| Every in-app cheat sheet and Information page re-checked when the behaviour changes — a stale help screen is a bug with a long half-life | tier 2, new §18 |

The other 18 are genuine tier-3 items and point at new §§20–31. Marks: all **U** except
*Undo where state is user-authored (D4: wardrobe, 30 deep)* (**A**), *Thumbnails/icons rendered
by the tool, cached, with a re-render action* (**A**), *Version-stamped caches keyed to the game
build, pruned automatically* (**A**), and *An asset-health audit that diffs runs* (**A**).

## §16 Tool explains itself → ❓ splits two ways, pending the question

| Item | Tier | Mark | New § | Note |
|---|---|---|---|---|
| The premise: a browser for opaque binary data spends its life answering "what *is* this and why does it look like that"; build the answers in as menu items, because a user who must run a `.bat` will instead file "it's broken" | 2 | U | 17 | |
| **Explain this \<thing\>** on right-click — is the record encrypted and do we hold its key · where the roster came from (metadata, the game's own binary, or nowhere) · what it resolved to · **which values are authored and which are stand-ins** · every texture role and whether it resolves · everything else using the same record | 2 ❓ | A | 17 | |
| "That authored-versus-substituted line is the whole point. A material reporting roughness 0.6 looks identical whether the game authored 0.6 or the tool gave up and picked it, and that ambiguity is what let encrypted content look merely ugly instead of unread for months" | 2 | U | 17 | Stays whole |
| **Instrument before theorising** — add a dump and read it; `Dump - Piece Roster.bat` prints every equipped piece's material roster by both routes with each part's classification; one run settled a fortnight-old "missing parts" question — the roster was complete and the primitive count was one short, moving the bug from the material layer to the mesh layer | 2 | U | 17 | Stays whole |
| **Find SNO / Find id** — paste an id or part of a name, get group, name, collection, reference count, and whether it arrived in the current build; a name search is a substring match across every group, capped for display but **counting all matches** so the user is told how many they did not see | 2 ❓ | A | 17 | Most investigations open with this question |
| **Health check** — storage, keys, snapshot freshness, live format probes on one screen, including the *coverage gap* between what the game ships and what community metadata describes; the honest answer to "why is this missing" | 3 ❓ | E | 30 | Underlying question: *is there a second metadata source whose coverage can lag?* |
| **Patch contents** — what each observed build added, newest first, grouped and named; nothing stamps an asset with its introducing build so this is observational only; a build the tool was never opened on cannot be reconstructed, and non-consecutive observed builds must state what they were actually diffed against | 3 | E | 30 | Underlying question: *can you tell when an asset appeared?* |
| **Diagnostic output** — the env-gated probe files (§3.5) listed newest first with size and age, readable in place; **list the folder, not a table of known probe names**, so a new probe appears without being taught | 3 | U | 30 | |

## §17 Documentation → ❓ new §18, tier pending (Essentials vs Fundamentals)

| Item | Tier | Mark | Note |
|---|---|---|---|
| "A tool with an undiscovered manual has no manual." Ship three layers and connect them | 2 ❓ | U | |
| In the app: tooltips answer "when would I want this?" · an Information settings tab · a **Shortcuts cheat sheet on `F1`, modeless** so it can sit beside the app; all three are code, so all three go stale — treat a behaviour change as touching them and grep the help strings | 2 | U | |
| In the repository: the README is the *overview* and opens with a **documentation index** linking every wiki page and saying what each answers; each major section ends with a pointer to the longer version. Before it existed the only route to D4's wiki was the repo's Wiki tab, which nobody clicks | 2 | U | |
| The wiki page table — Install · The tabs · Keyboard & mouse (*generated by reading the source*, not from memory) · Settings · Exporting · After a game patch · FAQ · Troubleshooting (**routed by symptom**, opening with a triage table) · Diagnostics · Asset formats (measured, with the method) · Glossary · Building from source | 2 | U | "After a game patch" and "Asset formats" are **A** |
| `_Sidebar.md` and `_Footer.md` — GitHub renders them on every wiki page; without them each page is a dead end | 2 | U | |
| The wiki folder lives in the repo (`wiki/*.md`) and is pushed to the wiki's separate git repository by the release script; documentation reviewed in the same PR as the code | 2 | U | |
| **Check the links** — walk every `[text](Target)` and resolve both page and `#anchor` against the real headings; GitHub replaces each space with a hyphen and does **not** collapse runs, so `## Help ▸ Find SNO` is `help--find-sno` with two hyphens | 2 | U | The link checker from §13 lands here |
| **Say what is unverified** — D4's format page names 24 of 133 asset groups and leaves 109 unnamed, and a section on *how to verify a claim on this page yourself* is what makes the rest credible | 2 | A | The counts are D4's |

## §18 Releasing → new §31, tier 3

| Item | Tier | Mark | Note |
|---|---|---|---|
| **Match the tag spelling the repo already uses**, and let the workflow accept both (`tags: ['v*', '[0-9]*']`) — D4's script forced a `v` prefix while every published tag was a bare number, so the two disagreed for eleven releases | 3 | U | |
| **Size-check generated files, never existence-check them** — a notes extractor wrote a one-byte release body, `if exist` waved it through, and the release published empty; the only symptom was on the website | 3 | U | |
| **Extraction belongs in a real script, not a shell one-liner** — the PowerShell one-liner had no exit code and no message; `release-notes.py` with distinct exit codes for *no such section* and *section too short* fails loudly | 3 | U | |
| **Assume CI got there first** — the workflow publishes the moment the tag lands, so a hand-run must set the body as well as upload the asset; `gh release create` is not the only path | 3 | U | |
| **Read back what you published** — "the command returned success" is not "the body is on the page" | 3 | U | |
| **The wiki is a separate git repository** and needs its own push; its first page must exist before it can be cloned at all | 3 | U | |

---

## Where the content resists the scheme — stated, not forced

1. **Releasing is not polish.** Tier 3 is defined as *polish that makes it feel finished rather
   than functional*, and shipping is neither. It lands in tier 3 because it is genuinely
   skippable — you can reach a finished tool without ever cutting a release — and the new
   section says exactly that in its opener rather than pretending it is polish.
2. **Tier 4 has almost no source material.** See the question below.
3. **Two tier-3 items are prerequisites for other tiers' comfort, not their function.** The
   Hotkeys *registry* (new §28) and the panel *machinery* (new §22) are cheapest to adopt on
   day one even though the features they carry are polish. The tier-3 rule "each item
   independently skippable" still holds — skipping them costs nothing but later rework, which
   is said in each opener.
4. **`§3.4` (shared menu builders) and `§3.3` (overlay master gate) are tier-1 conventions for
   tier-2 features.** That is the point of a conventions section and why §3 stays unsplit, but
   it is worth saying out loud: a reader stopping at tier 1 adopts two rules whose payoff
   arrives in tier 2.
