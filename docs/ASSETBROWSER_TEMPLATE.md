# The AssetBrowser Template

**What this is.** The design document for the *AssetBrowser* family of tools —
`(GameName|Engine)AssetBrowser`: D4AssetBrowser (Diablo IV, the reference implementation),
FOXAssetBrowser (Fox Engine), DIAssetBrowser (Diablo Immortal), and whatever comes next.
It records the UI, UX, features and conventions that D4AssetBrowser converged on after months
of iteration, so a new tool starts from the finished design instead of rediscovering it one
correction at a time.

**How to use it in a session.** Give this file to the session that is building or extending a
tool, together with the engine-specific facts (formats, archive layout, how assets are named).
Then ask for features *by the names this document uses* — "build the Browse tab per the
template", "Bulk Extract per the template, minus the queue for now", "settings dialog per the
template §10". The session should treat every rule here as a decision already made: deviate
only when the target engine genuinely can't support the behavior, and say so out loud when
that happens.

**Where the reference code lives.** `D4AssetBrowser` — C++17 / Qt6 / OpenGL 4.5 / MSVC 2022 /
CMake + Ninja / vcpkg manifest. When this document says *copy the mechanism*, the file named
is in that repo. Porting by copying a named file and deleting the D4-specific parts is always
preferred over re-implementing from the description.

---

## 1. Product identity — every tool in the family

- **Name:** `(GameName|Engine)AssetBrowser`. One native executable, no Python, no external
  extractor processes, no runtime downloads except game-community data the tool fetches itself.
- **Portable, absolutely.** Everything the tool writes lives in `data\` beside the exe:
  settings as INI (`QSettings` pointed at `data\<Tool>.ini` — never the registry), index
  caches, thumbnails, logs. Move the folder, it keeps working; delete it, nothing is left
  behind. Superseded cache versions are pruned automatically at startup.
  The one deliberate exception: **export dialogs default to Documents/Pictures** — a `.glb`
  the user exported belongs with their files, not inside the tool.
- **Reads the installed game directly.** No "extract first" step. Whatever the engine's
  storage is (CASC, QAR/FPK, …), the tool opens it in place and decodes on demand.
- **Nothing proprietary in the repo.** No game assets, no decryption keys. Keys and community
  metadata are fetched at runtime and gitignored. State this in the README.
- **Honesty is a feature.** The README carries an *Honest limitations* section with measured
  numbers ("~10% of appearances render incomplete", "93 decode perfectly but have no name").
  Errors in the tool say what failed and why; optimizers report "target not reachable" instead
  of silently shipping the wrong thing.

## 2. Architecture skeleton

```
src/
  app/     main window, settings dialog, log console, export capture, hotkeys, SEH guard
  <store>/ the engine's archive/storage layer (casc/ in D4)
  index/   background-built indexes: the asset list, metadata, tags, icons, links
  model/   geometry, skeleton, animation, materials, exporter
  tex/     texture definitions + block decode (BC1/3/4/5/7 …)
  gl/      ONE shared 3D viewport widget used by every model-viewing tab
  tabs/    one file per tab (+ _Panels / _Export splits when a tab grows)
  util/    header-only shared helpers (each anchored by a real #include, see §14)
```

Rules that keep this shape working:

- **One viewport class.** Every tab that shows 3D uses the same `GLModelWidget`. Features land
  once (overlays, channel viewer, shading modes) and every tab gets them.
- **Split, don't grow.** When a tab file gets unwieldy, split by concern into `Tab_Panels.cpp`
  and `Tab_Export.cpp` — and remember all three exist before declaring a function absent.
- **Background indexes all follow one shape** (copy it exactly): detached thread → disk cache
  signed with the file counts of *every* directory read plus the game build id → `install()`
  posted back via `Qt::QueuedConnection` → a `readyChanged` signal the UI re-populates on →
  `reset()` wired to the data-fingerprint change → a generation counter so an in-flight build
  discards itself if the data dir switches under it.
- **Crash-prone paths are SEH-guarded** (GPU submissions, binary parsers) so one bad asset
  reports instead of taking the app down.

## 3. Cross-cutting conventions (violations have caused real bugs)

1. **One setting, one key.** A state reachable from two places binds two widgets to ONE
   QSettings key — never a second key for the same state. A key that is only read and never
   written is a dead key and a bug.
2. **Persist the stable identity, not the label or index.** Store ids/stems/stable strings;
   restore with `findData`. Display text gets reworded by patches and `findText` then silently
   resolves to "(none)"; a combo index reorders the moment the list grows. (See
   `util/ExportLayout.h` for the worked example: stable string ids, unknown values fail to the
   safe default, and the old index-keyed setting is migrated once then removed.)
3. **Overlay master gate.** Each viewport tab has one `m_overlaysOn`; ALL overlay state flows
   through that tab's `reapplyOverlays()`. Settings-replay paths never call `setShow*()`
   directly — ungated replays silently re-enable overlays the user turned off.
4. **Shared menu builders.** Context menus come from one place (`util/ViewportPartMenu.h`,
   `addRowImageActions()`, `addRowExportCopyActions()`); extend those, never fork a per-view
   copy. This is what makes §12's uniformity rule cheap.
5. **Diagnostics are permanent and env-gated** (`D4_DUMP_CLOTH`-style variables), bounded and
   throttled. They stay in the code — the next regression uses them again.
6. **Defaults are chosen, not inherited.** Every new option ships with the value most users
   want, ON only if it's what the in-viewport experience already shows.
7. **QSettings namespaces per tab** (`models/…`, `wardrobe2/…`, `export/…`). A bad default
   written once needs a versioned migration to undo — so think before first write.

## 4. The Browse tab template (Models and every list-driven tab)

The centerpiece. Whatever the engine, browsing assets works like this:

**Three views of one list**, switched from a display dropdown in the header:
- **List** — dense flat rows.
- **Outliner** — a scene tree: the loaded model's row grows child nodes for parts, looks,
  animations, bones. Selecting a part node selects it in the viewport and vice versa.
- **Grid** — thumbnail tiles, rendered by the tool itself and cached in `data\`.

**One search box, one syntax, everywhere.** The same query language drives every searchable
tab (Models, Textures, Bulk Extract):

| Token | Meaning |
|---|---|
| `word` | substring, case-insensitive, in name/tags |
| `123456` | asset id (SNO or engine equivalent) |
| `#tag` | tags/title/collection only, *not* the filename |
| `c:collection` | collection; reads to end of line, so put it last |
| `-term` | exclude (works with every form above) |
| `a\|b` | OR within one term; combined with the outer space-AND |
| space | AND between terms |

`Ctrl+F` focuses, `Esc` clears, `↓` recalls the last ten searches.

The non-negotiable part is the **one-matcher rule**: every place that filters parses through
one shared helper (`util/QueryTerm.h`), and that helper carries a **startup self-test** of
~10 canonical cases. The bug this prevents is real: three hand-rolled parsers drifted, and
Bulk Extract silently exported a different set from the list the user filtered.

**Funnel filters.** A funnel-icon popup that *stays open while you tick things*: grouped tag
checkboxes (Category/Class/Gender/Type or the engine's equivalents) with a **Match any (OR)**
toggle, plus data-driven facets — only-decrypted, only-encrypted, hide un-renderable,
**Latest** (new this game update), Animated, Rigged, Orphaned. Every active filter shows as a
**removable chip** in the header, and active filters tint the funnel icon.

**Hover previews and icon indicators** are settings-gated (Settings ▸ Interface) so heavy
metadata lookups can be turned off on slow machines.

## 5. The 3D viewport

- **Four shading modes** — Wireframe · Flat · Shaded · Rendered (IBL, shadows, SSAO, tonemap)
  — as the row of "shading balls" top-right, Blender-style.
- **Channel viewer** — Base Colour, Normal, Roughness, Metallic, AO, Emissive — cycled by
  scrolling the `⌄` next to the shading balls or picked from its menu. This is the single
  best material-debugging feature; every tool gets it.
- **Overlays** (all behind the master gate, §3.3): statistics, ground grid, axis gizmo,
  skeleton, hardpoints, collision shapes, physics bones (anchored grey / simulated orange),
  bone names. Submesh class toggles (FX / SIM / GIB in D4) where the engine has them.
- **Popovers, not dialogs**, for Graphics / Camera / Lighting / engine-specific extras
  (Pigment in D4): small floating panels positioned next to the button that opened them.
- **Fullscreen** with a floating exit button (`✕ Exit fullscreen`) — never trap the user
  behind a hotkey they didn't read a tooltip for.
- Camera: orbit/pan/zoom, frame-selected, snap-to-slot where slots exist.

## 6. The right-hand panel system

A stack of collapsible panels in a splitter, Blender-style:

- A vertical **icon strip** toggles each panel; each panel header has **▲ ▼** to reorder and
  **✕** to hide. The whole column collapses to just the strip via `»`.
- A newly opened panel takes its height **from the slack of the panels already up** rather
  than forcing an equal split, and a drag can never fully erase a panel.
- Layout is remembered per tab behind one global setting (*Remember the right-hand panel
  layout*) via `util/PanelPersist.h`: `bind()` is called AFTER the code-set default
  `setSizes()`, so the default stands until the user has actually dragged something, and
  nothing is written when the setting is off.

Standard panels to offer per tab (rename per engine): PARTS (per-part triangle counts, slot,
material, visibility checkboxes) · MATERIALS · TEXTURES/TEXTURE PREVIEW (channel tiles with a
wheel-resizable hover zoom) · INFO (filename, id, tags, size, LODs, bones, counts, what-uses-it,
clickable variant links) · ANIMATIONS · engine extras (CLOTH, ATTACHMENTS…).

## 7. The Textures tab template

Every texture in the game, decoded in-tool. **Channel isolation** (RGB·R·G·B·A), **alpha
checkerboard**, cubemap/array face selector, a **pixel inspector** reporting `(x,y) RGBA`
under the cursor. Scroll zooms, drag pans, double-click resets. Filter by format, by the
tags of the assets that *use* the texture, orphans-only. Atlas/sprite support: list the
packed frames, export each with optional trim-to-bounds. **ASSOCIATED MODELS** walks
texture → material → model and jumps to the Browse tab. Drag the image straight out into
another application.

## 8. The Bulk Extract template

Filter the whole index with the same search + funnel as Browse, watch the **match count
update live**, then export everything in one run.

- **Pick items manually** moves matches into a persistent **Queue** that survives filter
  changes, mode switches and restarts.
- **Only new** skips anything already exported, tracked in a `_bulk_manifest.json` ledger in
  the output folder; or **Overwrite**.
- **Parallel** workers (auto = core count) with a live console, a working **Cancel** (and
  `Esc`), and **Pause/Resume that excludes paused time from the ETA**.
- Failures go to `_bulk_failed.txt` with a reason each; one bad asset never takes the run
  down.
- **Factory presets** where the engine has natural families ("all customization for a class")
  — which is why the query language has `|` OR.
- The tab holds ONLY run controls (filters, queue, workers, only-new/overwrite). Every
  *export option* — what gets written, how it's named, how it's laid out — lives in
  Settings ▸ Export, shared with every other export path. One setting, one key, one home.

**Output layout** is the worked example of doing this right (`util/ExportLayout.h`): one
choice — Flat · by class · by type · by model — that every *batch* path obeys (Bulk, Models
multi-select, context-menu batch, export-all) and every *single-model* path deliberately
ignores (`Ctrl+E` becoming `Barbarian\foo.glb` is a surprise the caller can't undo). The
layout picks the group folder only; inside each group the shape is identical in every mode,
which makes Flat "one group at the root" rather than a special case. Folder names are
sanitized (trailing dots, Windows reserved device names), unknown stored values fail to
Flat, and un-taggable items go to `_misc` — never loose into the parent.

**Run-scoped decode cache.** Batch runs share detail/common textures heavily; a bounded,
mutex-guarded cache keyed on `id|name|variant` makes repeat decodes memory hits. RAII scope
(`TextureCacheScope`) so it exists only for the run, opt-in per thread, zero disk footprint.

## 9. Exporting

- **Models** → rigged, animated `.glb`. **Exports exactly what's visible** — hidden parts
  stay out *unless* the user explicitly picked a subset, in which case the subset is written
  verbatim. When exporting a subset, **renumber material indices** — exporters that resolve
  materials by index quietly drop parts off the end of the material list otherwise.
- **Drag-out**: models drag straight from the list into Blender/Explorer. (Drag-out rebuilds
  its own paths — keep it exempt from output layout.)
- **Name templates** for every written file: `{{FileName}}`, `{{SNO}}`/`{{Id}}`,
  `{{FrameIdx}}`, `{{FrameName}}` — one template engine (`util/NameTemplate.h`), applied
  everywhere, with path-separator/reserved-name sanitizing.
- **Animation libraries** — skeleton + selected clips, no mesh, for retargeting.
- **Modding/retarget presets** (Settings ▸ Export ▸ Advanced): engine presets
  (Blender/Unreal/Unity — unit scale + normal convention), readable bone names, `.L`/`.R`
  mirror names, humanoid rig reduction, hardpoints as empties.
- **Images** — PNG/JPEG/WebP; 25–400% where **above 100% the scene is re-rendered larger**,
  never upscaled; transparent background as a native-alpha single render; crop-to-model.
- **Completion notifier** (`ExportNotifier`): every export path reports what it wrote and
  where, click-to-open.

**GIFs** — turntable and animation-loop, and copy the optimizer verbatim
(`app/ExportCapture.cpp`), because the naive version was wrong three separate ways:

1. Capture frames ONCE; retries only re-encode.
2. Budget ladder in this order: **palette** (×0.75 steps to a 32-colour floor) → **dither
   off** (at a coarse palette, killing the Bayer pattern is often the biggest single saving —
   dither amplitude scales with palette coarseness, so cutting colours with dither on can make
   files *bigger*) → **aimed downscale**: one pass at `sqrt(target/actual) × 0.93`, floored
   at 96px, ≤5 passes — never nibble in 15% steps.
3. **Ship the smallest attempt, not the last one**, and when the target is unreachable say
   so in the log — "TARGET NOT REACHABLE — shipping the smallest encode".
4. Turntables snap to whole animation loops so orbit and pose wrap together, and run a
   warm-up lap so physics settles before the first captured frame.

## 10. The Settings dialog

Top-level tabs, in this order, with the ordering philosophy: *setup → presentation → the
per-area pages → what lands on disk → keys → upkeep → reference → experimental*:

**General** (Directories · Game data · Updates · Settings profile) ·
**Interface** (Startup & layout · On-hover previews & info · Icon indicators · Diagnostics) ·
**Models** (Browsing & loading · Performance) · **Wardrobe** (or the engine's dress-up
equivalent) · **Export** · **Hotkeys** · **Maintenance** (Caches & reset · Indexing
(advanced)) · **Information** · **Experimental**.

- **Export gets sub-tabs**: Models · Images · per-tab options (Wardrobe/Catalogue/Bulk) ·
  File names. When a page accumulates more than ~4 groups, split it into sub-tabs too.
- **Information is a real tab**, also in sub-tabs (Reading the game · Models · Materials &
  textures · Icons · Names & files): plain-language explanations of how the tool works and
  **side-by-side tables for options that look interchangeable but aren't** (the two
  loose-texture options in D4). Explaining in-tool beats a wiki nobody opens.
- Mechanics that matter (all in `SettingsDialog.cpp`):
  - Every tab is a `QScrollArea` (`makeTab` lambda) so no page ever clips.
  - **Never elide tab labels**: `setElideMode(Qt::ElideNone)`, `setExpanding(false)`,
    scroll buttons as the safety net, and `showEvent` folds the tab bar's full width into
    the requested size. ("General" once rendered as "eneral".)
  - `&&` in `QGroupBox` titles (Qt renders one `&`; a bare `&` becomes a mnemonic and eats
    the letter).
  - **Settings profile** group: export/import the INI.
  - **Caches & reset**: one button per cache with its size shown, plus reset-all.
- **Tooltips on every option**, written to answer "when would I want this?", not restating
  the label.

## 11. Hotkeys

One central registry (`app/Hotkeys.h`): a `{key, label, default}` table shared by the
Settings ▸ Hotkeys editor (writes) and MainWindow (reads/applies). All rebindable; shipping
defaults `Ctrl+E` export selection · `Ctrl+Shift+E` export to last dir · `Ctrl+Shift+A`
animations only · `Ctrl+Shift+I` save image; unbound slots for the GIF exports. Adding a
shortcut = adding one row.

## 12. Context menus

**Full specification: `docs/CONTEXT_MENUS.md`** — read that file before building any menu. It
is exhaustive (label grammar, the selection rule, every tab's exact menu, omit-vs-disable) and
this section is only the summary.

Right-click works nearly everywhere, and **the same object offers the same actions wherever
it appears** — list row, grid tile, outliner node, parts panel, viewport click all raise the
one shared menu. Canonical action set for an asset: Load / preview · Copy image · Save
image(s) [to last folder | …] · Render icon(s) · Export N model(s) [to last folder (…/path) | …] ·
Copy SNO · Copy file name · Copy name · Copy collection name · Variants ▸ · Show dependencies….
For a part: export model/part × (last folder | prompt) · the copy block · Frame part · Select
part · Hide/Show part · Isolate part · Show all · Hide all · Invert. Every detail table gets
Copy / Copy all (`Ctrl+C`) via `CsvCopy`.

The four laws, in short: one `MenuText` vocabulary for every string · one builder per menu
family · never show an action that cannot be performed · the label states subject, count and
destination. Right-clicking outside the selection acts on the clicked row; inside it, on the
whole selection — and single-subject actions (Copy image) always take the *clicked* row.

## 13. Tooling around the tool

Double-clickable `.bat` entry points, one job each, no babysitting — each builds what it
needs, runs, writes a report, exits:

- `build.bat` (cold) / `rebuild.bat` (incremental: kill exe → snapshot `src` to `.Backups\` →
  `verify-src.py` → build → distilled `build_errors.txt` → launch) / `clean-rebuild.bat`.
- `verify-src.py` — pre-build source checks that catch what has actually broken builds:
  zero-byte files, unbalanced delimiters, header-only helpers used without a real anchored
  `#include` directive (a comment mentioning the path must NOT satisfy the check), printf
  format-vs-arg mismatches, locals named `emit`/`signals`/`slots`. Port this script early;
  it pays for itself in the first week.
- `Audit - Asset Health.bat` — walks every asset, classifies renderability
  (OK / no-textures / no-geometry / locked / no-data), and **diffs against the previous
  run**: after a game patch it reports "newly working / newly broken" instead of letting
  regressions age into bug reports.
- `github.bat` — menu-driven commit/push/release front end (repo root).
- A log console in-app with Help → Export log; the tool never writes logs unasked.

## 14. Porting guide — retrofitting a tab to a new engine

**What is engine-specific** (rewrite per tool): the storage layer (`casc/` → QAR/FPK/…),
binary parsers (`model/`, `tex/` decode tables), the id scheme, tag taxonomy, and any
game-specific tab (Wardrobe, Catalogue, Stable).

**What is generic** (copy, then delete D4-isms): the viewport and its overlays/channel
viewer, the panel system + PanelPersist, QueryTerm and the search box, the funnel+chips
filter UI, Bulk Extract's run machinery (queue, manifest, workers, pause/ETA, failures),
ExportLayout, NameTemplate, Hotkeys, ExportCapture (images + the GIF ladder),
ExportNotifier, SettingsDialog's skeleton, ViewportPartMenu, verify-src.py, the .bat suite.

**Porting order that works:** storage layer + index (get a searchable list on screen) →
viewport with flat shading (get one model drawn) → Browse tab views + search + filters →
materials/textures + channel viewer → panels → export single model → Textures tab → Bulk
Extract → settings polish, hotkeys, Information tab. Resist building the game-specific
showpiece tab first; every one of them stands on the Browse+viewport foundation.

**When asking a session for a feature**, name it from this document and point at the D4
file. "Bulk Extract per template §8 — copy the manifest/only-new mechanism from
BulkExtractorTab.cpp" gets the finished design; "add bulk export" gets a guess.

## 15. QoL checklist (the small things users notice)

Tick these off per tool; each exists because its absence was felt in D4:

- [ ] Working Cancel on every long operation, bound to `Esc` too
- [ ] Pause/Resume on batch runs; paused time excluded from the ETA
- [ ] Live match-count while filtering; chips for active filters
- [ ] Search history (`↓`), `Ctrl+F` focus, `Esc` clear — in every search box
- [ ] Failures written to a file with reasons; runs survive individual bad assets
- [ ] Only-new / overwrite semantics on anything re-runnable, ledger-tracked
- [ ] Every destructive or surprising action states what it will do *before* doing it
- [ ] Copy-to-clipboard on every id, name, path and table the user can see
- [ ] Thumbnails/icons rendered by the tool, cached, with a re-render action
- [ ] Undo where state is user-authored (D4: wardrobe, 30 deep)
- [ ] Remember window/panel/splitter layout behind one toggle
- [ ] No elided labels anywhere; scroll instead
- [ ] Tooltips answer "when do I want this?"; Information tab for anything longer
- [ ] Startup self-tests for invariants that would otherwise fail silently (QueryTerm)
- [ ] Exports notify with a click-to-open path; dialogs default to user folders
- [ ] Version-stamped caches keyed to the game build, pruned automatically
- [ ] Env-gated diagnostic dumps that stay in the code
- [ ] An asset-health audit that diffs runs, so patches report their own damage
