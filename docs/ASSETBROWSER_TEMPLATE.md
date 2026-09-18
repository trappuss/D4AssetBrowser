# The AssetBrowser Template

**What this is.** The design document for the *AssetBrowser* family of tools —
`(GameName|Engine)AssetBrowser`: D4AssetBrowser (Diablo IV, the reference implementation),
FOXAssetBrowser (Fox Engine), DIAssetBrowser (Diablo Immortal), and whatever comes next.
It records the UI, UX, features and conventions that D4AssetBrowser converged on after months
of iteration, so a new tool starts from the finished design instead of rediscovering it one
correction at a time.

**How to use it in a session.** Give this file to the session that is building or extending a
tool, together with the engine-specific facts (formats, archive layout, how assets are named).
Then ask for features *by the names this document uses* — "build the asset list per the
template", "bulk extraction per the template, minus the queue for now", "settings dialog per
the template §16". The session should treat every rule here as a decision already made:
deviate only when the target engine genuinely can't support the behavior, and say so out loud
when that happens.

**Where the reference code lives.** `D4AssetBrowser` — C++17 / Qt6 / OpenGL 4.5 / MSVC 2022 /
CMake + Ninja / vcpkg manifest. When this document says *copy the mechanism*, the file named
is in that repo. Porting by copying a named file and deleting the D4-specific parts is always
preferred over re-implementing from the description.

---

## How to use this document

The 33 sections are sorted into **four tiers** by what you lose if you stop, and every item
carries a **portability mark** saying how much of it survives a change of engine. Read the
tier you are in; ignore the ones above it.

### The four tiers

**Tier 1 · Fundamentals** — *without these it is not an AssetBrowser.* The decisions that are
load-bearing for everything above them, and expensive to retrofit. **If you stop here you have
a tool that opens the game's storage in place, lists what is inside it, draws one asset,
exports that asset, and has a README someone can find.**

**Tier 2 · Essentials** — *needed before anyone other than you can use it.* **If you stop here
you have something you could hand to a stranger with a README and expect them to succeed.**

**Tier 3 · Quality of life** — *the polish that makes it feel finished rather than functional.*
Real value, genuinely skippable, and each item skippable on its own: no tier-3 item is a
prerequisite for anything in another tier. **If you stop here you have a tool people enjoy
using rather than tolerate.**

**Tier 4 · Showpiece** — *the game-specific tab the whole tool exists to enable*, and the
reason anyone downloads it. D4's Wardrobe, Stable and Catalogue. Every one of them stands on
tiers 1–2, which is exactly why it is last. **If you stop here you have the tool you set out to
build.**

### The portability key

Every item is marked. The mark is about the *mechanism*, not the wording.

| Mark | Means | Test |
|---|---|---|
| **[U]** Universal | Copy the mechanism as-is. Nothing about it is D4. | Would this rule read identically in a tool for a game you have never heard of? |
| **[A]** Adapt | Same mechanism, engine-specific data behind it. | Is the *shape* right but the ids / paths / taxonomy different? |
| **[E]** Engine-specific | D4's answer to a problem another engine may not have. | Could a target engine reasonably have no equivalent at all? |

Every **[E]** item states the *underlying question* it answers, so you can decide whether your
engine even asks it. "D4 gates classification on an authored slot tag" is **[E]**; "classify by
authored data, never by a name substring" is **[U]**.

Sections also carry **skip this unless…** lines where an item is real bloat for a smaller tool.
This document used to imply that everything should be ported. It should not: the GIF budget
ladder is superb and irrelevant to a tool nobody will make turntables with, and Bulk's
pause/ETA/manifest machinery is weeks of work a five-hundred-asset game does not need.

### What is engine-specific, and what you copy

**Engine-specific — rewrite per tool:** the storage layer (`casc/` → QAR/FPK/…), the binary
parsers (`model/`, `tex/` decode tables), the id scheme, the tag taxonomy, and the
game-specific showpiece tab (§33).

**Generic — copy from D4, then delete the D4-isms:** the viewport with its overlays and channel
viewer **and its part-selection model** (§5, §10, §11), the panel system + PanelPersist (§12,
§23), QueryTerm and the search box (§4), the funnel+chips filter UI (§9), bulk run machinery —
queue, manifest, workers, pause/ETA, failures (§25), ExportLayout and NameTemplate (§15),
Hotkeys (§29), ExportCapture — images and the GIF ladder (§15, §26), ExportNotifier (§15),
SettingsDialog's skeleton **and ViewportSettings' reset-by-removal** (§16, §3.10),
ViewportPartMenu **and its plural vocabulary** (§17), the self-explanation reports (§18, §31),
`verify-src.py` (§7), the documentation skeleton and link checker (§8), and the `.bat` suite
including `release-notes.py` (§7, §30).

### The minimum viable build path

The shortest honest route to a working browser for a new engine. Every step is tier 1 or tier 2;
nothing here is optional, and the order is load-bearing.

1. **Storage layer + index** (§1, §2) — get a searchable list of what the game contains.
2. **Viewport with flat shading** (§5) — get one model drawn, in the one shared widget.
3. **The list and the one search matcher** (§4) — with the startup self-test from day one.
4. **Single-model export** (§6) — the first thing every user will try.
5. **Adopt §3 as you write all of the above.** It is ten conventions, not a feature; every one
   of them is cheaper now than later.
6. **`verify-src.py` and the build bats** (§7) — port the script early, it pays for itself in
   the first week.
7. **The documentation skeleton** (§8) — README index, `wiki/` in the repo, `_Sidebar`,
   `_Footer`, link checker. Four files and a script, while the tool is small.
8. **Materials/textures + channel viewer** (§10), then **the panel set** (§12), then the
   **Textures tab** (§13).
9. **Part selection as a set** (§11) and **context menus** (§17) — do these together; they are
   one rule seen twice.
10. **Settings dialog skeleton** (§16), **filters** (§9), **export options and layout** (§15).
11. **The reports you cannot debug without** (§18) and **the manual's twelve pages** (§19).

Steps 7 and 11 are the two that get deferred in practice and should not be: §18 pays for itself
the first time something renders wrong, and §8's skeleton is cheap while the tool is small.

**What it leaves out, deliberately:** every tier-3 section — the Outliner and Grid views, the
Rendered shading mode, fullscreen, the panel reorder/persist machinery, texture extras, bulk
run machinery, the GIF ladder, retarget presets, the Information tab, hotkeys, the audit and
release tooling, and the other three self-explanation reports. It also leaves out the showpiece
tab (§33). A tool built to this path is usable and honest; it is not finished.

**Resist building the showpiece tab first.** Every one of them stands on the list + viewport
foundation, and a showpiece built before tiers 1–2 gets rebuilt.

### Asking a session for a feature

Name it from this document and point at the D4 file. "Bulk extraction per template §14 — copy
the manifest/only-new mechanism from `BulkExtractorTab.cpp`" gets the finished design; "add
bulk export" gets a guess.

---

## Tier 1 · Fundamentals

*Without these it is not an AssetBrowser.* **If you stop here you have a tool that opens the
game's storage in place, lists what is inside it, draws one asset, exports that asset, and has
a README someone can find.**

Nothing in this tier is skippable, so there are no "skip unless" lines in it.

### 1. Product identity — every tool in the family

- **[U] Name:** `(GameName|Engine)AssetBrowser`. One native executable, no Python, no external
  extractor processes, no runtime downloads except game-community data the tool fetches itself.
- **[U] Portable, absolutely.** Everything the tool writes lives in `data\` beside the exe:
  settings as INI (`QSettings` pointed at `data\<Tool>.ini` — never the registry), index
  caches, thumbnails, logs. Move the folder, it keeps working; delete it, nothing is left
  behind. Superseded cache versions are pruned automatically at startup.
  The one deliberate exception: **export dialogs default to Documents/Pictures** — a `.glb`
  the user exported belongs with their files, not inside the tool.
- **[A] Reads the installed game directly.** No "extract first" step. Whatever the engine's
  storage is (CASC, QAR/FPK, …), the tool opens it in place and decodes on demand.
- **[U] Nothing proprietary in the repo.** No game assets.
- **[E] No decryption keys either.** Keys and community metadata are fetched at runtime and
  gitignored. State this in the README. *Underlying question: does your game lock content
  behind keys you have to fetch, and is there a third-party metadata dump at all?* If neither
  exists, this bullet is moot.
- **[U] Honesty is a feature.** The README carries an *Honest limitations* section with measured
  numbers ("~10% of appearances render incomplete", "93 decode perfectly but have no name").
  Errors in the tool say what failed and why; optimizers report "target not reachable" instead
  of silently shipping the wrong thing.
- **[U] Fail closed, and read the authoritative source.**
- **[E] Where a community metadata dump and the game disagree, the game wins and the tool says
  which one answered.** A dump always lags the build, and the lag is exactly where new content
  lives — so a pipeline reading only the dump cannot tell "newer than the dump" from "does not
  exist", and a *sparse checkout* of it looks identical to "the game ships nothing here". Both
  have shipped as user-visible bugs. *Underlying question: is there a second, lagging source of
  truth for names and metadata?*
- **[E] Distinguish present · unnamed (encrypted) · absent** in the UI as well as internally:
  they are different answers and deserve different words. *Underlying question: can a record
  exist in your storage but be unreadable?*

### 2. Architecture skeleton

```
src/
  app/     main window, settings dialog, log console, export capture, hotkeys, SEH guard
  <store>/ the engine's archive/storage layer (casc/ in D4)
  index/   background-built indexes: the asset list, metadata, tags, icons, links
  model/   geometry, skeleton, animation, materials, exporter
  tex/     texture definitions + block decode (BC1/3/4/5/7 …)
  gl/      ONE shared 3D viewport widget used by every model-viewing tab
  tabs/    one file per tab (+ _Panels / _Export splits when a tab grows)
  util/    header-only shared helpers (each anchored by a real #include, see §7)
```

Rules that keep this shape working:

- **[U] One viewport class.** Every tab that shows 3D uses the same `GLModelWidget`. Features
  land once (overlays, channel viewer, shading modes) and every tab gets them.
- **[U] Split, don't grow.** When a tab file gets unwieldy, split by concern into
  `Tab_Panels.cpp` and `Tab_Export.cpp` — and remember all three exist before declaring a
  function absent.
- **[A] Background indexes all follow one shape** (copy it exactly): detached thread → disk
  cache signed with the file counts of *every* directory read plus the game build id →
  `install()` posted back via `Qt::QueuedConnection` → a `readyChanged` signal the UI
  re-populates on → `reset()` wired to the data-fingerprint change → a generation counter so an
  in-flight build discards itself if the data dir switches under it. The fingerprint inputs and
  the build id are the engine's; the shape is not.
- **[A] Crash-prone paths are SEH-guarded** (GPU submissions, binary parsers) so one bad asset
  reports instead of taking the app down. The guard primitive is platform-specific; the rule is
  not.

### 3. Cross-cutting conventions (violations have caused real bugs)

This section keeps the number it has always had, because other documents cite its items
individually. Ten conventions, none of them a feature: adopt all ten while writing tier 1, when
they are free. Each one is here because it was violated in shipped code.

1. **[U] One setting, one key.** A state reachable from two places binds two widgets to ONE
   QSettings key — never a second key for the same state. A key that is only read and never
   written is a dead key and a bug.
2. **[U] Persist the stable identity, not the label or index.** Store ids/stems/stable strings;
   restore with `findData`. Display text gets reworded by patches and `findText` then silently
   resolves to "(none)"; a combo index reorders the moment the list grows. (See
   `util/ExportLayout.h` for the worked example: stable string ids, unknown values fail to the
   safe default, and the old index-keyed setting is migrated once then removed.)
3. **[U] Overlay master gate.** Each viewport tab has one `m_overlaysOn`; ALL overlay state
   flows through that tab's `reapplyOverlays()`. Settings-replay paths never call `setShow*()`
   directly — ungated replays silently re-enable overlays the user turned off. (The overlays
   themselves arrive in §10; the rule is here because retrofitting the gate means auditing every
   call site.)
4. **[U] Shared menu builders.** Context menus come from one place (`util/ViewportPartMenu.h`,
   `addRowImageActions()`, `addRowExportCopyActions()`); extend those, never fork a per-view
   copy. This is what makes §17's uniformity rule cheap.
5. **[A] Diagnostics are permanent and env-gated** (`D4_DUMP_CLOTH`-style variables), bounded
   and throttled. They stay in the code — the next regression uses them again.
6. **[U] Defaults are chosen, not inherited.** Every new option ships with the value most users
   want, ON only if it's what the in-viewport experience already shows.
7. **[A] QSettings namespaces per tab** (`models/…`, `wardrobe2/…`, `export/…`). A bad default
   written once needs a versioned migration to undo — so think before first write.
8. **[U] Classify by authored data, never by a name substring.** If the game authored a slot, a
   shader, a type reference — use it. Names are labels: they get reworded, they collide, and
   encrypted records have none at all. The canonical failure: a test for `head` in a material
   name matched `wolfHead`, an ornament on a Paladin pauldron, and the head grouping then hid
   the entire torso. The fix was not a better name test — it was the authored slot tag
   (`primSlot >= 0` means equipment, so it is not part of the body). Two more of the same
   shape: back trophies are named `trophy_*`, not `back_*`, so a `back_*` scan returns six
   placeholders and zero real trophies; and two classes name their eyeball material
   `Hero_eyes_mat` instead of `global_eyeball_mat`, so only the shader catches all of them.
   When a name test is genuinely the only option, say so in a comment and add a case to
   `verify-src.py` so it is counted rather than forgotten.
9. **[U] Batch widget writes, then recompute once.** A loop of `setCheckState` fires one
   `itemChanged` per item and runs a full visibility pass each time — sixty of them on a
   sixty-part outfit. Block signals for the batch, unblock, then call the recompute yourself.
   Where two surfaces show one selection, edit with signals blocked and call **one** named
   slot both paths share; never `emit otherWidget->someSignal()` to fake it.
10. **[U] Reset by removing keys, not by writing defaults over them.** A reset that writes
   today's defaults freezes them: a later change to a default is never picked up, because the
   key now exists. Remove the group's keys and let the read fall back. Keep an explicit
   *keep-prefixes* list for user-authored state that a reset must not touch — saved presets,
   camera bookmarks, cloth setups. (`app/ViewportSettings.h` is the worked example: three
   groups, subtree lists, `resetGroup()` / `resetAll()` by removal.)

### 4. The asset list and the one search matcher

The centerpiece, and the template for **every list-driven tab** in the tool, not just the first
one. Whatever the engine, this is what makes it a browser.

**[U] A list view** — dense flat rows over the whole index. One view is enough to be a browser;
the other two are §21.

**[U] One search box, one syntax, everywhere.** The same query language drives every searchable
tab (the asset list, Textures, bulk extraction):

| Token | Mark | Meaning |
|---|---|---|
| `word` | **[U]** | substring, case-insensitive, in name/tags |
| `123456` | **[A]** | asset id (SNO or engine equivalent) |
| `#tag` | **[A]** | tags/title/collection only, *not* the filename |
| `c:collection` | **[E]** | collection; reads to end of line, so put it last. *Underlying question: does the game author named sets an asset belongs to?* Drop the token if it does not |
| `-term` | **[U]** | exclude (works with every form above) |
| `a\|b` | **[U]** | OR within one term; combined with the outer space-AND. Needed by the factory presets in §25 |
| space | **[U]** | AND between terms |

**[U] The non-negotiable part is the one-matcher rule**: every place that filters parses through
one shared helper (`util/QueryTerm.h`), and that helper carries a **startup self-test** of ~10
canonical cases. The bug this prevents is real: three hand-rolled parsers drifted, and Bulk
Extract silently exported a different set from the list the user filtered.

### 5. The 3D viewport — one shared widget

- **[U] One `GLModelWidget`**, per §2, used by every tab that draws anything. This is the single
  most expensive thing in the document to retrofit: features added to a second viewport class
  have to be added twice forever.
- **[U] Flat shading** is enough to get a model on screen and confirm the parser. Wireframe,
  Shaded and the channel viewer are §10; Rendered is §22.
- **[U] Camera: orbit / pan / zoom.**

### 6. Single-model export

- **[U] Models → rigged, animated `.glb`.** The first thing every user tries, and the thing a
  browser is judged by.
- **[U] Exports exactly what's visible** — hidden parts stay out *unless* the user explicitly
  picked a subset, in which case the subset is written verbatim.
- **[U] When exporting a subset, renumber material indices** — exporters that resolve materials
  by index quietly drop parts off the end of the material list otherwise.
- **[U] Single-model paths deliberately ignore the output layout of §15.** `Ctrl+E` becoming
  `Barbarian\foo.glb` is a surprise the caller can't undo.

### 7. The build and verify loop

- **[U] Double-clickable `.bat` entry points, one job each, no babysitting** — each builds what
  it needs, runs, writes a report, exits.
- **[A] `build.bat`** (cold) / **`rebuild.bat`** (incremental: kill exe → snapshot `src` to
  `.Backups\` → `verify-src.py` → build → distilled `build_errors.txt` → launch) /
  **`clean-rebuild.bat`**.
- **[U] `verify-src.py` — pre-build source checks that catch what has actually broken builds.**
  Port this script early; it pays for itself in the first week. D4's runs eleven checks over
  ~140 files in seconds: zero-byte files from a botched write · unbalanced `{}` `()` `[]` ·
  header-only helpers used without a real anchored `#include` (a comment mentioning the path
  must NOT satisfy the check) · printf format-vs-arg mismatches · locals named
  `emit`/`signals`/`slots` · duplicate lambdas · duplicate map keys · a metadata-coverage
  baseline · **settings keys written and never read** · **combo boxes persisted by display
  text instead of value** · **classification decided by a name substring**.
  The last three exist because §3.1, §3.2 and §3.8 were each violated in shipped code.
- **[U] A convention you cannot check is a convention you will lose.** Every rule in §3 that can
  be grepped for should become a check, and the ones that legitimately have exceptions should
  report a reviewed count rather than failing.

### 8. The documentation skeleton

**[U] A tool with an undiscovered manual has no manual.** Ship three layers — in the app, in the
repository, in the wiki — and connect them. The *skeleton* is here, in tier 1, because it is
four files and a script while the tool is small; the pages that fill it are §19. Retrofitting a
manual onto a finished tool means writing twelve pages at once instead of one per feature.

- **[U] The README is the overview** — what the tool is, what it can do, whether to download it.
  It opens with a **documentation index** that links every wiki page and says what each one
  answers, and each major section ends with a pointer to the longer version. This is not
  decoration: before it existed, the only route to D4's wiki was the repo's own Wiki tab, which
  nobody clicks.
- **[U] `_Sidebar.md` and `_Footer.md`.** GitHub renders these two filenames on *every* wiki
  page. Without them each page is a dead end reachable only from wherever the user landed.
- **[U] The wiki folder lives in the repo** (`wiki/*.md`) and is pushed to the wiki's separate
  git repository by the release script. Documentation reviewed in the same PR as the code.
- **[U] Check the links.** A script that walks every `[text](Target)` in the docs and resolves
  both the page and the `#anchor` against the real headings. GitHub's anchor rule replaces each
  space with a hyphen and does **not** collapse runs, so `## Help ▸ Find SNO` is
  `help--find-sno` with two hyphens — worth encoding once rather than discovering per link.
  Run it before publishing.

---

## Tier 2 · Essentials

*Needed before anyone other than you can use it.* **If you stop here you have something you
could hand to a stranger with a README and expect them to succeed.**

### 9. Filters: funnel, facets, chips

**[U] A funnel-icon popup that *stays open while you tick things*.** A popup that closes on
every click makes multi-criteria filtering unusable.

- **[A] Grouped tag checkboxes** — Category / Class / Gender / Type, or the engine's equivalents.
  The groups are your taxonomy; the grouping is the mechanism.
- **[U] A Match any (OR) toggle** over the ticked set.
- **[E] Data-driven facets: only-decrypted, only-encrypted.** *Underlying question: does the
  engine encrypt records?* If nothing is ever unreadable, these two do not exist.
- **[A] Facet: hide un-renderable.**
- **[E] Facet: Latest (new this game update).** *Underlying question: can the tool observe the
  game's builds over time?* It depends on the observational record of §31.
- **[A] Facets: Animated · Rigged · Orphaned.**
- **[U] Every active filter shows as a removable chip** in the header, and active filters tint
  the funnel icon. With a live match count, this is what stops a user exporting a set they did
  not mean.

**Skip this unless** your index is big enough to need narrowing by more than a search box — a
few hundred assets do not need facets.

### 10. Shading modes, channel viewer and overlays

- **[U] The shading-ball row, top-right, Blender-style**: Wireframe · Flat · Shaded. (Rendered
  is §22.)
- **[A] Channel viewer** — Base Colour, Normal, Roughness, Metallic, AO, Emissive — cycled by
  scrolling the `⌄` next to the shading balls or picked from its menu. This is the single best
  material-debugging feature; every tool gets it. The channel set follows the engine's material
  model.
- **[U] Overlays: statistics, ground grid, skeleton.** All behind the master gate of §3.3.
- **[A] Overlay: hardpoints.**
- **[E] Overlays: collision shapes, physics bones (anchored grey / simulated orange).**
  *Underlying question: does the engine author per-asset physics and collision the user would
  want to see?*
- **[E] Submesh class toggles** (FX / SIM / GIB in D4). *Underlying question: does the engine tag
  submeshes by class?*
- **[U] Frame-selected.**
- **[E] Snap-to-slot where slots exist.** *Underlying question: does the engine define equipment
  slots a camera could frame?*

### 11. Part selection is a set, and the viewport is a first-class way to build it

- **[U] Single click** picks the part under the cursor; empty space clears. **Ctrl or Shift**
  adds one or takes one back out.
- **[U] Double-click frames** and does not touch the selection.
- **[U] The selection is mirrored both ways** with the tab's parts surface (tree or table), so
  the blue outline and the panel are one state seen twice.
- **[U] Right-clicking a part already in the selection acts on the whole selection**; right-
  clicking one outside it replaces the selection with that part *first*. The set the menu acts
  on is the same set that turns blue, so the outline can never disagree with what is about to
  happen.
- **[U] Menu labels count what they have**: *Export 3 parts (5,120 tris)…*, *Frame 3 parts*,
  *Isolate 3 parts*, *Copy 3 material names* (de-duplicated). A title that names one source
  piece drops the name when the selection spans several.

Three scars worth copying the code for rather than re-deriving:

1. **[U] Qt delivers press → release → DoubleClick → release.** A click handler on release
   therefore fires *twice* for a double-click, once either side of the double-click handler.
   With Ctrl held that reads select → clear-and-select → toggle-off, and the whole selection
   is gone. Set a swallow flag in `mouseDoubleClickEvent` and consume the second release —
   and clear that flag on every press, or a drag that fails the click threshold leaves it
   set and eats the next real click.
2. **[U] One gesture, one job.** Double-click used to select because it was the only way to
   select from the viewport. Once single click owns selection, a double-click that also
   selects is two behaviours fighting over the same event.
3. **[U] Clear the picked set when geometry changes.** A stale index that is still *in range* for
   the new model outlines the wrong part rather than being filtered out — and a picked set
   makes that a whole wrong set.

### 12. The panel set

The panels are how anyone reads an asset, which is why the set is tier 2 and the stack
machinery that arranges them is §23.

**[A] Standard panels to offer per tab** (rename per engine): PARTS (per-part triangle counts,
slot, material, visibility checkboxes) · MATERIALS · TEXTURES/TEXTURE PREVIEW (channel tiles
with a wheel-resizable hover zoom) · INFO (filename, id, tags, size, LODs, bones, counts,
what-uses-it, clickable variant links) · ANIMATIONS · **[E]** engine extras (CLOTH,
ATTACHMENTS…) — *underlying question: what else does your engine author per asset that has no
D4 equivalent?*

### 13. The Textures tab

Every texture in the game, decoded in-tool.

- **[A] Decode in-tool**, whatever the engine's block formats are.
- **[U] Channel isolation** (RGB · R · G · B · A).
- **[U] Scroll zooms, drag pans, double-click resets.**
- **[A] Filter by format, by the tags of the assets that *use* the texture, orphans-only.**

The extras — alpha checkerboard, face selector, pixel inspector, atlas frames, ASSOCIATED
MODELS, drag-out — are §24. A browser that cannot show you a texture is half a tool; one that
cannot tell you the pixel value under the cursor is merely less convenient.

### 14. Bulk extraction — the run

Filter the whole index with the same search + funnel as the asset list, watch the **match count
update live**, then export everything in one run.

- **[U] The one-matcher rule of §4 is what makes this safe.** The count you see and the set you
  export are the same parse.
- **[U] The tab holds ONLY run controls** (filters, queue, workers, only-new/overwrite). Every
  *export option* — what gets written, how it's named, how it's laid out — lives in
  Settings ▸ Export, shared with every other export path. One setting, one key, one home.

**Tier by intent.** This section is tier 2 for a tool people use to *look* at a game, and
**tier 1 for a tool whose purpose is to get assets out of one** — a ripper without bulk output
is not the tool anyone asked for. If that is your tool, read §25 as Essentials rather than
Quality of life, and move this section's two rules up into your minimum viable build path.

### 15. Export options, output layout and name templates

Everything here is shared by every export path in the tool, which is why it is one section and
not a paragraph in each tab.

- **[A] Output layout** (`util/ExportLayout.h`) is the worked example of doing this right: one
  choice — Flat · by class · by type · by model — that every *batch* path obeys (bulk, list
  multi-select, context-menu batch, export-all) and every *single-model* path deliberately
  ignores (§6). The layout picks the group folder only; inside each group the shape is identical
  in every mode, which makes Flat "one group at the root" rather than a special case. Folder
  names are sanitized (trailing dots, Windows reserved device names), unknown stored values fail
  to Flat, and un-taggable items go to `_misc` — never loose into the parent. The group
  dimensions are your taxonomy; the mechanism is not.
- **[A] Name templates** for every written file: `{{FileName}}`, `{{SNO}}`/`{{Id}}`,
  `{{FrameIdx}}`, `{{FrameName}}` — one template engine (`util/NameTemplate.h`), applied
  everywhere, with path-separator/reserved-name sanitizing.
- **[U] Drag-out**: models drag straight from the list into Blender/Explorer. Drag-out rebuilds
  its own paths — keep it exempt from output layout.
- **[A] Animation libraries** — skeleton + selected clips, no mesh, for retargeting.
- **[U] Images** — PNG/JPEG/WebP; 25–400% where **above 100% the scene is re-rendered larger**,
  never upscaled; transparent background as a native-alpha single render; crop-to-model.
- **[U] Completion notifier** (`ExportNotifier`): every export path reports what it wrote and
  where, click-to-open.

### 16. The Settings dialog

**[A] Top-level tabs, in this order**, with the ordering philosophy: *setup → presentation → the
per-area pages → what lands on disk → keys → upkeep → reference → experimental*:

**General** (Directories · Game data · Updates · Settings profile) ·
**Interface** (Startup & layout · On-hover previews & info · Icon indicators · Diagnostics) ·
**Models** (Browsing & loading · Performance) · **Wardrobe** (or the engine's dress-up
equivalent) · **Export** · **Hotkeys** · **Maintenance** (Caches & reset · Indexing
(advanced)) · **Information** · **Experimental**.

- **[U] Export gets sub-tabs**: Models · Images · per-tab options (Wardrobe/Catalogue/Bulk) ·
  File names. When a page accumulates more than ~4 groups, split it into sub-tabs too.
- Mechanics that matter (all in `SettingsDialog.cpp`):
  - **[U]** Every tab is a `QScrollArea` (`makeTab` lambda) so no page ever clips.
  - **[U] Never elide tab labels**: `setElideMode(Qt::ElideNone)`, `setExpanding(false)`,
    scroll buttons as the safety net, and `showEvent` folds the tab bar's full width into
    the requested size. ("General" once rendered as "eneral".)
  - **[U]** `&&` in `QGroupBox` titles (Qt renders one `&`; a bare `&` becomes a mnemonic and
    eats the letter).
  - **[A] Caches & reset**: one button per cache with its size shown, plus reset-all.
  - **[U] Restore Defaults is scoped and complete.** Scoped: it resets the group in front of
    you, and *every* viewport tab has a group — a reset that quietly covered one tab and not the
    others shipped in D4 and read as "the setting did not stick". Complete: every tab with
    viewport state appears in Settings at all. Implemented by removal, per §3.10.
- **[U] Tooltips on every option**, written to answer "when would I want this?", not restating
  the label.

The Information tab and the settings-profile export/import are §28.

### 17. Context menus

**Full specification: `docs/CONTEXT_MENUS.md`** — read that file before building any menu. It
is exhaustive (label grammar, the selection rule, every tab's exact menu, omit-vs-disable) and
this section is only the summary.

- **[U] Right-click works nearly everywhere, and the same object offers the same actions
  wherever it appears** — list row, grid tile, outliner node, parts panel, viewport click all
  raise the one shared menu. Uniformity is what makes the tool learnable, and §3.4's shared
  builders are what make it cheap.
- **[A] Canonical action set for an asset:** Load / preview · Copy image · Save image(s) [to
  last folder | …] · Render icon(s) · Export N model(s) [to last folder (…/path) | …] ·
  **[E]** Copy SNO · Copy file name · Copy name · **[E]** Copy collection name · Variants ▸ ·
  Show dependencies…. *Underlying question for the two marked: does your engine have a single
  stable asset id, and does it group assets into named collections?*
- **[U] For a part:** export model/part × (last folder | prompt) · the copy block · Frame part ·
  Select part · Hide/Show part · Isolate part · Show all · Hide all · Invert.
- **[U] Every detail table gets Copy / Copy all** (`Ctrl+C`) via `CsvCopy`.
- **[U] The four laws**: one `MenuText` vocabulary for every string · one builder per menu
  family · never show an action that cannot be performed · the label states subject, count and
  destination.
- **[U]** Right-clicking outside the selection acts on the clicked row; inside it, on the whole
  selection — and single-subject actions (Copy image) always take the *clicked* row.
- **[U] The same rule governs viewport parts** (§11), with one addition: the acted-on set is also
  what the viewport highlights, computed once and used for both.
- **[U]** Where a tab has two surfaces that can select parts — an outliner of nodes and a flat
  parts table — consult them most-specific first, and have "select these" route to whichever
  surface can actually show several at once.
- **[U]** Single-subject vocabulary degrades gracefully: `MenuText::parts(n)` and
  `verbParts(verb, n)` give one plural rule for every builder, and options that only make sense
  for one part (*Explain this material*) are omitted, not disabled, above `n == 1`.

### 18. The tool explains itself — the reports you cannot debug without

A browser for opaque binary data spends most of its life answering "what *is* this, and why
does it look like that". Build the answers in, as menu items rather than scripts: a user who
has to run a `.bat` and read a text file will instead file an issue that says "it's broken".

**Five reports earn their place in every tool.** Two of them are here, because you cannot debug
the engine without them; the other three — Health check, Patch contents, Diagnostic output — are
§31.

This section is **commonly deferred and should not be. It pays for itself the first time something
renders wrong** — without an explain report you are debugging opaque binary data by eye.

- **[A] Explain this \<thing\>** on a right-click — the single most useful report in the tool.
  For a material: is the record encrypted and do we hold its key · where the roster came from
  (metadata, the game's own binary, or nowhere) · what it resolved to · **which values are
  authored and which are stand-ins the tool substituted** · every texture role and whether it
  resolves · and everything else that uses the same record.

  **[U] That authored-versus-substituted line is the whole point.** A material reporting
  roughness 0.6 looks identical whether the game authored 0.6 or the tool gave up and picked it,
  and that ambiguity is what let encrypted content look merely ugly instead of unread for
  months.
- **[A] Find SNO / Find id** — paste an id or part of a name; get the group, the name, the
  collection, how many things reference it, and whether it arrived in the current game build.
  Most investigations open with this question, and the tool is already holding the table.
  **[U]** A name search is a substring match across every group, capped for display but
  **counting all matches**, so the user is told how many they did not see.
- **[U] Instrument before theorising.** When a piece renders wrong and the data is opaque, add a
  dump that prints what the tool actually resolved — `Dump - Piece Roster.bat` prints every
  equipped piece's material roster by both routes with each part's classification — and read it
  before proposing a cause. One such dump settled a fortnight-old "missing parts" question in a
  single run: the roster was complete and correct, and the primitive count was one short, which
  moved the bug from the material layer to the mesh layer.
- **[U] A log console in-app**, with Help → Export log; the tool never writes logs unasked.

### 19. The manual

The skeleton is §8; these are the pages and the in-app layer that fill it. §8's three layers,
connected.

**[U] In the app.** Tooltips answer "when would I want this?". An **Information** settings tab
(§28) explains the confusable options. A **Shortcuts** cheat sheet on `F1`, modeless so it can
sit beside the app while the user learns. All three are code, so all three go stale — treat a
behaviour change as touching them, and grep the help strings when you change an interaction.

**In the wiki.** The manual, one page per question the user actually has:

| Page | Mark | Answers |
|---|---|---|
| Install | **[U]** | Download, first run, the folders it needs, the SmartScreen warning |
| The tabs | **[U]** | What each tab is for and when to use which |
| Keyboard & mouse | **[U]** | Every binding — *generated by reading the source*, not from memory |
| Settings | **[U]** | Each settings tab, and the options that change results rather than looks |
| Exporting | **[U]** | Formats, scopes, and the conventions that make an export look wrong elsewhere |
| After a game patch | **[A]** | What to re-download, and how to tell "new" from "broken" |
| FAQ | **[U]** | Short answers, each linking to the long one |
| Troubleshooting | **[U]** | **Routed by symptom**, opening with a triage table |
| Diagnostics | **[U]** | Which report answers which question (§18, §31) |
| Asset formats | **[A]** | How the data is really put together — measured, with the method |
| Glossary | **[U]** | One line per term the rest of the documentation assumes |
| Building from source | **[U]** | Prerequisites, build, checks, CI, cutting a release |

- **[A] Say what is unverified.** Where the format documentation does not know something, it says
  so instead of filling the gap. D4's names 24 of 133 asset groups and leaves 109 unnamed, and
  a section on *how to verify a claim on this page yourself* is what makes the rest credible.

**Skip this unless** someone other than you will use the tool — but note that "someone other
than you" includes you in six months.

---

## Tier 3 · Quality of life

*The polish that makes it feel finished rather than functional.* **If you stop here you have a
tool people enjoy using rather than tolerate.**

Every section in this tier is skippable on its own, and nothing above depends on it. Two
qualifications, said plainly rather than hidden:

- **§32 Releasing is not polish** — it is distribution. It is in this tier because it is
  genuinely skippable: you can reach a finished tool without ever cutting a release.
- **§23 and §29 are cheapest on day one** even though what they carry is polish. The panel
  persist helper and the hotkey registry are each an afternoon now and a refactor later. Skipping
  them costs nothing but rework.

### 20. The quality-of-life (QoL) checklist — this tier's index, and the small things users notice

Tick these off per tool. Each exists because its absence was felt in D4. The pointer says where the rule lives; four of
them are implemented in tiers 1–2 and keep their box here so nothing is missed.

- [ ] **[U]** Working Cancel on every long operation, bound to `Esc` too → §25
- [ ] **[U]** Pause/Resume on batch runs; paused time excluded from the ETA → §25
- [ ] **[U]** Live match-count while filtering; chips for active filters → *tier 2, §9*
- [ ] **[U]** Search history (`↓`), `Ctrl+F` focus, `Esc` clear — in every search box → §21
- [ ] **[U]** Failures written to a file with reasons; runs survive individual bad assets → §25
- [ ] **[U]** Only-new / overwrite semantics on anything re-runnable, ledger-tracked → §25
- [ ] **[U]** Every destructive or surprising action states what it will do *before* doing it →
      *tier 2, §17's fourth law*
- [ ] **[U]** Copy-to-clipboard on every id, name, path and table the user can see → *tier 2, §17*
- [ ] **[A]** Thumbnails/icons rendered by the tool, cached, with a re-render action → §21
- [ ] **[A]** Undo where state is user-authored (D4: wardrobe, 30 deep) → §33
- [ ] **[U]** Remember window/panel/splitter layout behind one toggle → §23
- [ ] **[U]** No elided labels anywhere; scroll instead → *tier 2, §16*
- [ ] **[U]** Tooltips answer "when do I want this?"; Information tab for anything longer →
      *tier 2, §16* and §28
- [ ] **[U]** Startup self-tests for invariants that would otherwise fail silently (QueryTerm) →
      *tier 1, §4*
- [ ] **[U]** Exports notify with a click-to-open path; dialogs default to user folders →
      *tier 2, §15* and *tier 1, §1*
- [ ] **[A]** Version-stamped caches keyed to the game build, pruned automatically →
      *tier 1, §1 and §2*
- [ ] **[A]** Env-gated diagnostic dumps that stay in the code → *tier 1, §3.5*
- [ ] **[A]** An asset-health audit that diffs runs, so patches report their own damage → §30
- [ ] **[U]** Viewport part selection is a set: click, Ctrl-click, right-click scoped to it →
      *tier 2, §11*
- [ ] **[U]** Every in-app cheat sheet and Information page re-checked when the behaviour it
      describes changes — a stale help screen is a bug with a long half-life → *tier 2, §19*
- [ ] **[U]** `_Sidebar.md` / `_Footer.md` in the wiki so no page is a dead end →
      *tier 1, §8*
- [ ] **[U]** A link checker over the docs, run before publishing → *tier 1, §8*

### 21. The other two list views, and hover

- **[U] Outliner** — a scene tree: the loaded model's row grows child nodes for parts, looks,
  animations, bones. Selecting a part node selects it in the viewport and vice versa (§11).
- **[U] Grid** — thumbnail tiles, rendered by the tool itself and cached in `data\`, with a
  re-render action.
- **[U] A display dropdown in the header** switches the three views. It only earns its place once
  there is more than one.
- **[U]** `Ctrl+F` focuses the search box, `Esc` clears it, `↓` recalls the last ten searches.
- **[A] Hover previews and icon indicators** are settings-gated (Settings ▸ Interface) so heavy
  metadata lookups can be turned off on slow machines.

**Skip this unless** the list is long enough to browse visually, or the assets have internal
structure worth a tree. A flat list plus search answers most questions.

### 22. Viewport polish

- **[U] Rendered** shading mode — IBL, shadows, SSAO, tonemap — the fourth shading ball.
- **[U] Popovers, not dialogs**, for Graphics / Camera / Lighting / **[E]** engine-specific extras
  (Pigment in D4): small floating panels positioned next to the button that opened them.
  *Underlying question for the extras: does the engine have a per-asset appearance system with no
  D4 equivalent?*
- **[U] Fullscreen** with a floating exit button (`✕ Exit fullscreen`) — never trap the user
  behind a hotkey they didn't read a tooltip for.
- **[U] Overlays: axis gizmo and bone names.**

**Skip Rendered unless** the tool is expected to produce presentable images. Flat and Shaded
answer "is the geometry right"; Rendered answers "does it look like the game", which only matters
if someone is going to show the output to someone else.

### 23. The panel machinery

A stack of collapsible panels in a splitter, Blender-style:

- **[U]** A vertical **icon strip** toggles each panel; each panel header has **▲ ▼** to reorder
  and **✕** to hide. The whole column collapses to just the strip via `»`.
- **[U]** A newly opened panel takes its height **from the slack of the panels already up** rather
  than forcing an equal split, and a drag can never fully erase a panel.
- **[U]** Layout is remembered per tab behind one global setting (*Remember the right-hand panel
  layout*) via `util/PanelPersist.h`: `bind()` is called AFTER the code-set default `setSizes()`,
  so the default stands until the user has actually dragged something, and nothing is written
  when the setting is off.

**Skip this unless** there are enough panels to want rearranging — but adopt `PanelPersist` early
anyway; retrofitting persistence means auditing every `setSizes()` call.

### 24. Textures tab extras

- **[U] Alpha checkerboard.**
- **[E] Cubemap / array face selector.** *Underlying question: does the engine ship cubemaps or
  texture arrays a browser has to page through?*
- **[U] A pixel inspector** reporting `(x,y) RGBA` under the cursor.
- **[E] Atlas/sprite support**: list the packed frames, export each with optional
  trim-to-bounds. *Underlying question: does the engine pack small images into atlases rather
  than shipping them individually?*
- **[A] ASSOCIATED MODELS** walks texture → material → model and jumps to the asset list.
- **[U]** Drag the image straight out into another application.

**Skip the atlas machinery unless** the engine actually packs sprites; skip the face selector
unless it ships cubemaps. Both are dead code otherwise.

### 25. Bulk run machinery

Weeks of work, and the reason bulk extraction is pleasant instead of merely possible. Read this
as Essentials if your tool exists to rip rather than to look (§14).

- **[U] Pick items manually** moves matches into a persistent **Queue** that survives filter
  changes, mode switches and restarts.
- **[U] Only new** skips anything already exported, tracked in a `_bulk_manifest.json` ledger in
  the output folder; or **Overwrite**.
- **[U] Parallel** workers (auto = core count) with a live console, a working **Cancel** (and
  `Esc`), and **Pause/Resume that excludes paused time from the ETA**.
- **[U] Failures** go to `_bulk_failed.txt` with a reason each; one bad asset never takes the run
  down.
- **[E] Factory presets** where the engine has natural families ("all customization for a
  class") — which is why the query language has `|` OR. *Underlying question: does the game group
  assets into families a user would want wholesale?*
- **[A] Run-scoped decode cache.** Batch runs share detail/common textures heavily; a bounded,
  mutex-guarded cache keyed on `id|name|variant` makes repeat decodes memory hits. RAII scope
  (`TextureCacheScope`) so it exists only for the run, opt-in per thread, zero disk footprint.

**Skip this unless** a run is long enough that a user would want to abandon, resume or repeat it.
A five-hundred-asset game does not need a queue, a manifest or an ETA.

### 26. The GIF budget ladder

**GIFs** — turntable and animation-loop, and copy the optimizer verbatim
(`app/ExportCapture.cpp`), because the naive version was wrong three separate ways:

1. **[U]** Capture frames ONCE; retries only re-encode.
2. **[U]** Budget ladder in this order: **palette** (×0.75 steps to a 32-colour floor) → **dither
   off** (at a coarse palette, killing the Bayer pattern is often the biggest single saving —
   dither amplitude scales with palette coarseness, so cutting colours with dither on can make
   files *bigger*) → **aimed downscale**: one pass at `sqrt(target/actual) × 0.93`, floored
   at 96px, ≤5 passes — never nibble in 15% steps.
3. **[U] Ship the smallest attempt, not the last one**, and when the target is unreachable say
   so in the log — "TARGET NOT REACHABLE — shipping the smallest encode".
4. **[U]** Turntables snap to whole animation loops so orbit and pose wrap together, and run a
   warm-up lap so physics settles before the first captured frame.

**Skip this entirely unless** somebody will make turntables with your tool. It is superb work and
completely irrelevant to a browser nobody will use for showcase images — but if you do build
GIFs, copy the ladder rather than re-deriving it, because all four of these were learned the
expensive way.

### 27. Retarget and modding presets

**[A] Modding/retarget presets** (Settings ▸ Export ▸ Advanced): engine presets
(Blender/Unreal/Unity — unit scale + normal convention), readable bone names, `.L`/`.R`
mirror names, humanoid rig reduction, hardpoints as empties.

**Skip this unless** people will take the exports into a DCC app or another engine. A browser
whose exports are only ever looked at does not need a unit-scale matrix.

### 28. Settings extras

- **[A] Information is a real tab**, also in sub-tabs (Reading the game · Models · Materials &
  textures · Icons · Names & files): plain-language explanations of how the tool works and
  **side-by-side tables for options that look interchangeable but aren't** (the two
  loose-texture options in D4). Explaining in-tool beats a wiki nobody opens.
- **[U] Settings profile** group: export/import the INI.

**Skip the Information tab until** the option set has actually become confusable — that is the
problem it solves. Two options nobody mixes up do not need a comparison table.

### 29. Hotkeys

**[U] One central registry** (`app/Hotkeys.h`): a `{key, label, default}` table shared by the
Settings ▸ Hotkeys editor (writes) and MainWindow (reads/applies). All rebindable. Adding a
shortcut = adding one row.

**[A] Shipping defaults** `Ctrl+E` export selection · `Ctrl+Shift+E` export to last dir ·
`Ctrl+Shift+A` animations only · `Ctrl+Shift+I` save image; unbound slots for the GIF exports.

**Skip the bindings, not the registry.** The keys are polish; the table is an afternoon that
makes every later shortcut one line.

### 30. Audit, release and housekeeping tooling

- **[A] `Audit - Asset Health.bat`** — walks every asset, classifies renderability
  (OK / no-textures / no-geometry / locked / no-data), and **diffs against the previous
  run**: after a game patch it reports "newly working / newly broken" instead of letting
  regressions age into bug reports.
- **[U] `github.bat`** — menu-driven commit/push/release front end (repo root). Never `git init`;
  refuse to run if `.git` is absent. See §32 for the release failures worth encoding.
- **[U] `release-notes.py`** — extracts one version's section from `CHANGELOG.md` into the release
  body, with real exit codes. A shell one-liner here published an empty release (§32).

**Skip the health audit unless** the game patches often enough for regressions to appear on their
own. Skip the release tooling until you are actually publishing.

### 31. The remaining self-explanation reports

Three more of the five from §18. Each is genuinely skippable; Health check is the one to pull
forward first, because it pairs with §1's fail-closed rule.

- **[E] Health check** — storage, keys, snapshot freshness and live format probes on one screen,
  including the *coverage gap* between what the game ships and what any community metadata
  describes. This is the honest answer to "why is this missing". *Underlying question: is there a
  second metadata source whose coverage can lag the game's?*
- **[E] Patch contents** — what each observed game build *added*, newest first, grouped and
  named. Nothing in the data stamps an asset with the build that introduced it, so this can only
  be captured observationally. Say so: a build the tool was never opened on cannot be
  reconstructed, and when two observed builds are not consecutive the entry must state what it
  was actually diffed against rather than implying one patch. *Underlying question: can you tell
  when an asset appeared?* The **Latest** facet of §9 depends on this record existing.
- **[U] Diagnostic output** — the probe files the env-gated dumps (§3.5) write, listed newest
  first with size and age and readable in place. **List the folder, not a table of known probe
  names**, so a new probe appears without anything being taught about it.

### 32. Releasing

Distribution, not polish — see this tier's opener. `github.bat` is the front end, but the
failures worth encoding are these:

- **[U] Match the tag spelling the repo already uses**, and let the release workflow accept both
  (`tags: ['v*', '[0-9]*']`). D4's script *forced* a `v` prefix while every published tag was
  a bare number, so the two disagreed for eleven releases.
- **[U] Size-check generated files, never existence-check them.** A notes extractor wrote a
  one-byte release body, `if exist` waved it through, and the release published empty — the
  only symptom was on the website. Anything below a floor is a failure, not a file.
- **[U] Extraction belongs in a real script, not a shell one-liner.** The PowerShell one-liner
  that produced that empty body had no exit code and no message. A `release-notes.py` with
  distinct exit codes for *no such section* and *section too short* fails loudly instead.
- **[U] Assume CI got there first.** The release workflow publishes the moment the tag lands, so a
  hand-run usually finds the release already created and must **set the body as well as upload
  the asset** — `gh release create` is not the only path.
- **[U] Read back what you published.** "The command returned success" is not "the body is on the
  page"; query the release and check the length.
- **[U] The wiki is a separate git repository** and needs its own push. Its first page must exist
  before it can be cloned at all.

**Skip this until** you ship to someone who is not you.

---

## Tier 4 · Showpiece

*The game-specific tab the whole tool exists to enable.* **If you stop here you have the tool you
set out to build.**

### 33. The game-specific showpiece tab

**[E] This is the one section of this document that is not written yet, and saying so is more
useful than filling it in.** D4's three showpiece tabs — **Wardrobe** (dress a character from
every owned appearance), **Stable** (mounts and their armour), **Catalogue** (the shop's bundles)
— are the reason anyone downloads D4AssetBrowser, and the template records everything *under*
them without ever specifying them. What follows is every commitment the rest of this document
makes about a showpiece tab. It is not a specification.

*Underlying question for the whole tier: what is the one thing your game lets players assemble
that no generic browser could show them?* For D4 it is an outfit; for a Fox Engine tool it is an
avatar; for some engine it may be nothing at all, and that tool stops at tier 3.

What this document already commits to:

- **[U] It is engine-specific and gets rewritten per tool** — it is named in the front matter's
  engine-specific list alongside the storage layer and the binary parsers. Nothing about D4's
  Wardrobe ports.
- **[U] It stands on tiers 1–2, which is why it is last.** *Resist building the game-specific
  showpiece tab first.* Every one of D4's three stands on the list + viewport foundation, and one
  built before that foundation gets rebuilt on top of it.
- **[U] It is a viewport tab like any other**, so it inherits: the one shared `GLModelWidget`
  (§2, §5), the shading modes and channel viewer (§10), overlays behind its own master gate
  (§3.3), part selection as a set (§11), the panel set (§12), and the one shared menu builders
  (§3.4, §17).
- **[A] It gets its own Settings page** — D4's **Wardrobe** page sits between Models and Export in
  §16's tab order, and its viewport state has a group in Restore Defaults like every other
  viewport tab. A showpiece tab missing from Settings is the bug §16 describes.
- **[A] It gets its own Export sub-tab** under Settings ▸ Export's per-tab options, beside the
  bulk one (§16).
- **[A] Undo, where its state is user-authored** — D4's wardrobe is 30 deep (§20). This is the
  only tier-3 item that is really a tier-4 obligation: a dress-up tab without undo punishes
  experimenting, which is the entire activity.
- **[A] Its caches and indexes follow §2's one shape** and are keyed to the game build like
  everything else.

**Where D4's own showpiece work is written down**, since it is not here: `docs/CONTEXT_MENUS.md`
specifies every one of their menus exactly; `docs/STABLE_PARITY_AUDIT.md` audits Stable against
Wardrobe across four dimensions, including nine bugs the reverse pass found in Wardrobe; and
`docs/notes/BUNDLES-TAB-RESEARCH.md` holds the StoreProduct/bundle structure behind Catalogue.
Read those three before building a showpiece tab for another game — they are the closest thing to
a specification that exists, and the parity audit in particular is a list of the mistakes a
second showpiece tab makes.

**Skip this tier entirely if** your game has no assemblable thing. A tool that stops at tier 3 is
a complete asset browser; it is just not a showpiece.
