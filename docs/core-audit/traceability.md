# Traceability verdict — `CORE_EXTRACTION_PLAN.md` against the five audit reports and the source trees

Reviewer stance: read the plan cold, then all five reports in full, then recomputed every number and
spot-checked report citations against `d4/src`, `fox/src`, `poe2/src` (plus each project's
`CMakeLists.txt`, `vcpkg.json`, `verify-src.py`, `docs/`, `CLAUDE.md`, `NEW_SESSION_PROMPT.md`).

Verdict codes:

- **TRACED** — the claim is stated in a named report section; where I also opened the report's own
  citation the row says `[spot-checked]` and the log in §A records the result.
- **CODE** — not stated in any report, but verified directly in the trees (file:line given).
- **UNTRACED** — in neither. Quoted verbatim.
- **CONTRADICTED** — a report or the source says otherwise. Both sides quoted.

Totals (one row = one claim; a claim repeated in two plan sections is counted where it appears):

| Verdict | Count |
|---|---|
| TRACED (incl. rows also confirmed in code, marked "+ CODE" / "[spot-checked]") | 213 |
| CODE (in no report; verified only in the trees) | 4 |
| UNTRACED | 6 |
| CONTRADICTED | 10 |
| **Total claims examined** | **233** |

(§F additionally lists two borderline items, C-10 and C-11, that are tallied above as CODE and TRACED respectively: one is a report-internal error the plan happens to get right, the other an overstated "zero coupling" the plan's own Graft column already qualifies.)

---

## A. Citation spot-check log (report citation → source)

40 report citations were opened against the source. Result column: ✔ = line(s) say what the report says.

| # | Report / section | Citation | Result |
|---|---|---|---|
| 1 | viewport §0 | sizes 856/6727, 1138/3677, 201/1010 | ✔ `wc -l` exact |
| 2 | viewport §1.4 | `d4/src/gl/GLModelWidget.h:853` `m_swallowLeftClick`; `.cpp:5652` cleared on press, `:5684` consumed, `:5693` set | ✔ |
| 3 | viewport §1.4 | `poe2/src/gl/GLModelWidget.h:187` `m_swallowNextRelease`; `.cpp:879/905/935` | ✔ |
| 4 | viewport §1.2 | `fox/src/gl/GLModelWidget.h:360` `enum class ShadingMode`, `:390` setter | ✔ |
| 5 | viewport §1.3 | `poe2/src/gl/GLModelWidget.h:61` `setOverlaysOn`; `.cpp:648,656` gate in paintGL | ✔ |
| 6 | viewport §3.1 | `d4/src/gl/GLModelWidget.cpp:1187` `Config::d4dataDir()` | ✔ |
| 7 | viewport §3.1 | `d4/src/gl/GLModelWidget.cpp:1324-1351` writes `detail_mask_probe.txt` | ✔ (`:1348-1350`) |
| 8 | viewport §3.3 | `poe2/src/gl/GLModelWidget.cpp:15` `toYUp`; `:297` `AstSkeleton::skinMatrices` in widget | ✔ |
| 9 | viewport §3.2 | `fox/src/gl/GLModelWidget.cpp:8` `#include "app/Hotkeys.h"`; `:3164` `Hotkeys::seq` | ✔ |
| 10 | viewport §1.6 | `poe2/src/gl/GLModelWidget.cpp:765-783` two-pass opaque+mask / blend+additive | ✔ |
| 11 | viewport §1.4 (HUNCH) | FOX Ctrl+double-click: `fox.cpp:3212-3220` press → `applyPickGesture`; `:3105-3113` dblclick → `applyPickGesture`; `:3090-3103` Ctrl = `toggleInSelection` | ✔ — the HUNCH is confirmed by code (toggle on press, toggle again on dbl-click) |
| 12 | viewport §1.3 | `fox/src/view/ViewportBar.h:40-64` `ViewportOverlays{master…}` | ✔ |
| 13 | viewport §3.1 / geo appendix | `d4/src/gl/GLModelWidget.h:403-405` `translateBoneName/translateSkeletonNames/blenderizeSkeletonNames` | ✔ |
| 14 | viewport §5 | cloth `d4.cpp:1589-3130, 3273-3636` | ✔ `buildSpringBones` at 1589, `buildClothSim` at 3273; 1,906 lines |
| 15 | viewport §3.1 | 120 `^uniform` declarations | ✔ `grep -c` = 120 |
| 16 | geo §1.2 / appendix | `poe2/src/model/ModelGeometry.h:21` `uint8_t joints[4]`; `GlbExporter.cpp:117` widens to u16 | ✔ |
| 17 | geo §5.2 | `poe2/src/model/GlbExporter.cpp:284-296` IBM row-major scar with "2.6 m → 0" | ✔ |
| 18 | geo §5.2 | `poe2 GlbExporter.cpp:333-336` accessors assigned after animation block | ✔ |
| 19 | geo §5.2 | `poe2 GlbExporter.cpp:250-256` always TRS, Blender 4.4 vs 5.0 | ✔ |
| 20 | geo §3.1 | `d4/src/model/ModelExporter.cpp:717-739` time accessor cached, "954" story | ✔ |
| 21 | geo §5.3 | two container tails `d4 ModelExporter.cpp:809-843` vs `:846-879` | ✔ two `QJsonObject root;` at 809 and 846 |
| 22 | geo §5.3 | `fox/src/model/GlbExporter.cpp:715-736` `usedMaterials` | ✔ (`QSet<int> usedMaterials` at 723) |
| 23 | geo §3.3 | `fox GlbExporter.cpp:1112-1115` constant tracks dropped | ✔ |
| 24 | geo §6.4 | `poe2/src/bulk/BulkExtractor.cpp:168` sole `NameTemplate::apply("{{FileName}}"…)`; no `export/nameModel` readers elsewhere | ✔ |
| 25 | geo §6.2 | ladder constants: d4 `ExportCapture.cpp:218-238`, poe2 `:145-165`, fox `ViewCapture.cpp:310-330` | ✔ all three: `colors*3/4` floor 32, `sqrt(ratio)*0.93` clamped `[0.35,0.92]`, 96 px floor |
| 26 | geo §6.2 | turntable clip-snap d4 `:364`, poe2 `:223`; FOX none | ✔ (`grep -i snap fox/src/export/ViewCapture.cpp` empty) |
| 27 | geo §2.4 | `d4 RigMath.h:31-41` `mat4mul` column-major; `poe2 RigMath.h:20-30` `mul` row-major; `fox AnimMath.h:104-112` | ✔ |
| 28 | geo §2.2 | `poe2 AstSkeleton.cpp:241-253` "bones need not be pre-sorted"; `:214` "keyframe times are in frame units" | ✔ |
| 29 | geo §2.2 | POE2 `RigMath.h` has `inverse` (:33) and `decomposeTRS` (:109) | ✔ (relevant to plan §3.1 Mat4 row — see C-6) |
| 30 | app-shell §1.1 | `fox/src/app/Hotkeys.h:25-30` `Def{key,label,def,hint}`; `:197` `sheetRows`; `:260` `Role` | ✔ — but **row count is 32, not 36** (see C-8) |
| 31 | app-shell §1.1/§8.7 | `d4/src/app/MainWindow.cpp:509-517` positional `acts[i]` ↔ `d[i]` | ✔ |
| 32 | app-shell §2.1 | `d4 MainWindow.cpp:465-497` hand-written HTML sheet | ✔ |
| 33 | app-shell §2.3 | `poe2/src/app/MainWindow.cpp:131-151` hotkeys created once in ctor; `SettingsDialog.cpp:190` "apply after the next launch" | ✔ |
| 34 | app-shell §1.1 | `fox/src/util/PanelPersist.h:103-133` pane-count guard | ✘ **line ref impossible — file is 82 lines**; the guard exists at `:37` (`countKey = key + "/panes"`). Fact holds, citation wrong |
| 35 | app-shell §4.2 | `poe2/src/app/SettingsDialog.cpp:323-336` `restoreExportDefaults()` removes `export/` + `image/` groups | ✔ |
| 36 | app-shell §1.1/§5 | `poe2/src/main.cpp:56-61` ten self-tests to stderr; `fox/src/main.cpp:121-144` | ✔ POE2 = 10; **FOX = 5** (bc, encodeSelfTest, QueryTerm, searchq, animpose) — plugin-build §9 says four (see C-10) |
| 37 | app-shell §1.1 | `fox/src/app/Config.cpp:30-38` session overrides | ✔ |
| 38 | app-shell §1.1 | `poe2/src/util/HintBar.h:21-22` `setSizePolicy(…Fixed)` | ✔ |
| 39 | app-shell §1.1 | `d4/src/main.cpp:137-150` `AppLog::setFileLogging`; `d4 SettingsDialog.cpp:2802` `showEvent`, `:2537` `snapshotLiveSettings` | ✔ |
| 40 | app-shell §1.2 | POE2: no `runGuarded`/SEH install outside `app/SehGuard.*`, no `qInstallMessageHandler`, no `AppLog::` definition, `ConsoleWindow`/`LogBuffer`/`PanelPersist` referenced only by their own files | ✔ |
| 41 | tabs-store §9.1 | `d4/src/tabs/TexturesTab.cpp:2029-2035` `b.contains(t)` / `meta.contains(t)` | ✔ |
| 42 | tabs-store §9.3 | `fox/src/tabs/ModelsTab.cpp:2474-2480` linear `for (c : index.files())`; `:2561` `fileIndexForPath` | ✔ |
| 43 | tabs-store §2.1 | `d4/src/tabs/ModelsTab.cpp:8127-8182` `std::thread` + token + `invokeMethod` | ✔ (`:8154` thread, `:8167` token check) |
| 44 | tabs-store §3 note | `d4 MainWindow.cpp:1029-1031` "generation counters exist"; `grep generation` in AppearanceMeta/AssetLinks/IconIndex `.cpp` | ✔ comment present; grep = 0/0/0 |
| 45 | tabs-store §3/§9.7 | `poe2/src/store/IndexLoader.cpp:15-31` store mutated on worker, no install() | ✔ |
| 46 | tabs-store §7.1/§9.2 | `d4 ModelsTab.h:243-246` `ensureAnimatedIndex/ensureEntityIndex/ensureRigIndex`; `:362-402` blocklist / sets | ✔ |
| 47 | tabs-store §9.8 | `d4 ModelsTab_Export.cpp:1117-1121` `par = 1` when anim / base-body | ✔ |
| 48 | tabs-store §5 | `poe2 BulkExtractor.cpp:45-68` "four AssetStore methods and nothing else" | ✘ **seven** distinct `m_store->` methods in the file: `loadModel`, `loadSkeletonFor`, `resolveMaterials`, `loadTexture`, `collectAssetFiles`, `readFile`, `index` (see C-3) |
| 49 | tabs-store §9.2 | `d4 BulkExtractorTab.h:33` ctor takes `ModelsTab*`, `:143` `m_models`; `.cpp:1536-1537` delegation | ✔ |
| 50 | plugin-build §2/§6 | `d4/CMakeLists.txt:28-31,194` fastgltf/tinygltf; `grep` of `d4/src` for `#include` of either = none (only comments/About text) | ✔ |
| 51 | plugin-build §6.1 | Svg in `d4/CMakeLists.txt:14`, `fox:25`, not `poe2:16`; no `QSvg`/`QtSvg` include in any `src` | ✔ |
| 52 | plugin-build §7 | vcpkg baselines `a1cae005…` (d4, poe2) vs `ea1a7396…` (fox) | ✔ |
| 53 | plugin-build §8 | `poe2/verify-src.py:103-106` "No-op body kept as the documented anchor"; `:115-127` dead-key regex without concatenation | ✔ |
| 54 | plugin-build §8 | `fox/verify-src.py:38-43` `HEADER_ONLY` two entries; `:362` `check_qprintable` | ✔ |
| 55 | plugin-build §6 | `fox/CMakeLists.txt:266-269` `/Z7`; POE2 has no `target_precompile_headers`; `fox:343-366` `foxab_probe` re-lists sources | ✔ |
| 56 | plugin-build §9 | `poe2/tools/render_offscreen.cpp:24` `#include "/root/work/scratch/shaders.inc"`; `verify_container.sh:24-35` hand-listed `SRCS` | ✔ |
| 57 | plugin-build §10 | `fox/NEW_SESSION_PROMPT.md` 1,279 lines; `poe2/CLAUDE.md` 292; `d4/CLAUDE.md` 39; templates 373 / 910 | ✔ exact |
| 58 | plugin-build §9 | `Verify - Regression.bat`: "189 `:check` + 2 `:checkno` + 1 `:checkre` + 2 `:rc`" | ~ file has **186** `call :check` (185 at line start + 1 guarded by `if exist` at `:413`), 2, 1, 2 → **191** total; the report's 189 is slightly off, "~190" in the plan holds |
| 59 | plugin-build §1.1/§3 | "All 11 background indexes … each … with its own `m_ready/m_building/m_generation`" | ✘ see §D.5 — 7 classes have thread+`readyChanged`+`m_building`; only 3 have `m_generation`; 11 = singleton `instance()` classes under `index/`+`tex/`; the roster (`MainWindow.cpp:1670+`) has 9 rows and `MainWindow.h:93` says "the nine indexes" |
| 60 | app-shell §7 vs plugin-build §8 | D4 check count: app-shell "D4 has 11 checks"; plugin-build table gives D4 12 rows | ✘ report-internal: `grep '^def check_' d4/verify-src.py` = **12** |
| 61 | app-shell §6 / CONTEXT_MENUS | `d4/docs/CONTEXT_MENUS.md` §6 "a reordering that desyncs them binds the wrong keys silently" | ✔ `:584-585`, inside §6 (`:426-640`) |

---

## B. Per-section claim tables

### B.0 — Preamble

| # | Claim (verbatim or condensed) | Verdict | Where / evidence |
|---|---|---|---|
| 0.1 | "513 files, ~198k lines" | TRACED + CODE | plugin-build §1 preamble: 58+82 / 130+152 / 39+52 = 513 `.cpp/.h`; `find … -type f` under the three `src` trees = 513, `wc -l` = 197,814 |
| 0.2 | "Every classification below is grounded in a file:line citation in the five audit reports" | see §C pointer check | 5 of 26 pointers point at the wrong report for part of what they attribute |

### B.1 — §1 What the audit measured

| # | Claim | Verdict | Where / evidence |
|---|---|---|---|
| 1.1 | "exactly one file byte-identical in all three — `GifEncoder.cpp` — plus `SehGuard`" | **CONTRADICTED** | Code: the only file byte-identical in all three trees is `SehGuard.cpp`. `GifEncoder.cpp` differs in all three by its first line (`#include "GifEncoder.h"` / `"export/GifEncoder.h"` / `"gl/GifEncoder.h"`); `SehGuard.h` differs in FOX by a comment typo (`fox/src/app/SehGuard.h:8`). The reports agree with the code, not the plan: geometry-export §6.1 "the `.cpp` differs only in include path"; app-shell §1.2 "FOX's `SehGuard.cpp` is byte-identical to D4's; the header differs only by a comment typo" |
| 1.2 | "D4 and POE2 share eight more small helpers verbatim (`AppLog.h`, `CsvCopy`, `Hotkeys.h`, `LogConsole`, `NameTemplate.h`, `PanelPersist.h`, `TextReportDialog.h`, `GifEncoder.h`)" | TRACED [spot-checked] | app-shell §1.2; `cmp` confirms all eight (11 files) identical d4↔poe2 |
| 1.3 | "Every other file that carries the same name in two projects is a rewrite: line similarity below 0.30 for `MainWindow`, `SettingsDialog`, `ModelsTab`, `TexturesTab`, `GLModelWidget`, `BulkExtractorTab`, `ExportLayout`, `QueryTerm`, `Config`, `ExportNotifier`" | **CONTRADICTED** (partly CODE) | No report gives a similarity figure. `difflib.SequenceMatcher` line ratio over every pair: MainWindow ≤0.15, SettingsDialog ≤0.27, ModelsTab ≤0.07, TexturesTab ≤0.15, GLModelWidget ≤0.08, BulkExtractorTab ≤0.11, ExportLayout ≤0.28, Config ≤0.22 — hold. **`QueryTerm.h` d4/poe2 = 0.39** and **`ExportNotifier.h` d4/poe2 = 0.40, d4/fox = 0.31** — above 0.30. "Every other file … is a rewrite" also fails for `HintBar.h` d4/poe2 = **0.96** (app-shell §1.1: POE2's is "D4's + `setSizePolicy(Fixed)` fix") and `AppPaths.h` 0.44–0.53 |
| 1.4 | "FOX diverged even on the helpers D4 and POE2 kept identical (its `Hotkeys.h` is 289 lines to their 38)" | TRACED + CODE | app-shell §1.1; `wc -l` 289 / 38 / 38 |
| 1.5 | FOX: `ShadingMode` enum | TRACED [spot-checked] | viewport §1.2 (`fox.h:360`) |
| 1.6 | FOX: an overlay gate | TRACED | viewport §1.3 (in `ViewportBar`, not the widget) |
| 1.7 | FOX: a selection set living in the widget | TRACED | viewport §1.4 |
| 1.8 | FOX: hide/isolate/fullscreen/help driven by a hotkey registry | TRACED [spot-checked] | viewport §1.4, §1.6, §3.2 (`fox.cpp:8,3164`) |
| 1.9 | FOX: camera presets | TRACED | viewport §1.1 |
| 1.10 | FOX: first-run page, crash handler, startup profiler, density and font scale, cache-maintenance table | TRACED | app-shell §1.1 |
| 1.11 | FOX: the three-layer search split | TRACED | app-shell §1.1 |
| 1.12 | FOX: the menu vocabulary/rule/dump split | TRACED | app-shell §1.1 |
| 1.13 | FOX: `NPanel`+`PanelBox` | TRACED | app-shell §1.1 |
| 1.14 | FOX: "a ~190-check regression suite" | TRACED + CODE | plugin-build §9 (not app-shell §1 / viewport §5 as the pointer says); `Verify - Regression.bat` = 191 check invocations |
| 1.15 | POE2: neutral `ModelGeometry` (one shared buffer, parts as index ranges) | TRACED | geometry-export §1.2, §1.4 |
| 1.16 | POE2: `GlbExporter` takes geometry + skeleton + materials + options and builds in memory | TRACED | geometry-export §5.3 |
| 1.17 | POE2: "an `AssetStore` whose four methods are all `BulkExtractor` needs" | **CONTRADICTED** | tabs-store §5 makes the same claim ("call four `AssetStore` methods and nothing else", `BulkExtractor.cpp:45-68`); code: `poe2/src/bulk/BulkExtractor.cpp` calls **seven** distinct store methods — `loadModel`, `loadSkeletonFor`, `resolveMaterials`, `loadTexture`, `collectAssetFiles`, `readFile`, `index` (the last three in `exportRaw`) |
| 1.18 | POE2: one material resolver feeding viewport and export | TRACED | tabs-store §2.1, §7.1, §9.5 |
| 1.19 | POE2: `.gltf+.bin`, three KHR extensions, MASK and BLEND | TRACED | geometry-export §4.2, §5.1 |
| 1.20 | POE2: a scoped reset-by-removal | TRACED [spot-checked] | app-shell §4.2 (`poe2 SettingsDialog.cpp:323-336`) — not tabs-store §5/§7 as the pointer says |
| 1.21 | POE2: a ten-entry self-test table | TRACED [spot-checked] | app-shell §1.1/§5, plugin-build §2/§9; `poe2/src/main.cpp:56-59` = 10 |
| 1.22 | D4: Rendered pipeline (shadows, SSAO, cubemap IBL, ACES) | TRACED | viewport §1.6 |
| 1.23 | D4: click-on-release selection with the double-click swallow flag | TRACED [spot-checked] | viewport §1.4 (`d4.h:853`, `.cpp:5652/5684/5693`) |
| 1.24 | D4: worker-thread model loading with token and SEH guard | TRACED [spot-checked] | tabs-store §2.1, §9.4 (`d4 ModelsTab.cpp:8154-8180`) — not in the cited viewport §1 / geometry-export §5 / app-shell §1 |
| 1.25 | D4: `BrowserTab` export-hook interface and `LazyTab` | TRACED [spot-checked] | app-shell §1.1, §2.1 (`BrowserTab.h:50-99`, `MainWindow.cpp:525-584`) |
| 1.26 | D4: `IndexDesc` roster | TRACED [spot-checked] | app-shell §1.1 (`MainWindow.h:96-118`) |
| 1.27 | D4: retarget presets and X-mirror | TRACED | geometry-export §5.1 |
| 1.28 | D4: hardpoint empties | TRACED | geometry-export §5.1 |
| 1.29 | D4: animation-library export | TRACED | geometry-export §5.1 |
| 1.30 | D4: the time-accessor cache | TRACED [spot-checked] | geometry-export §5.2 (`ModelExporter.cpp:717-739`) |
| 1.31 | D4: "the most complete `verify-src.py` (12 check functions)" | CODE (report-internal conflict) | `grep -c '^def check_' d4/verify-src.py` = **12**; plugin-build §8 table has 12 D4 rows; **app-shell §7 says "D4 has 11 checks"** — app-shell is wrong, the plan is right |
| 1.32 | "POE2 compiles `SehGuard`, `AppLog.h`, `LogConsole` and `PanelPersist` and calls none of them" | TRACED [spot-checked] | app-shell §1.1, §1.2, §8.7; grep confirms. Nit: `AppLog.h` is a header with *no definition* rather than something compiled |
| 1.33 | "D4 links `fastgltf` and `tinygltf` and includes neither" | TRACED [spot-checked] | plugin-build §2 (glTF row), §6.2, §7 — not in the four reports the pointer names |
| 1.34 | "D4's Textures tab bypasses `QueryTerm` so `a\|b` silently returns nothing there" | TRACED [spot-checked] | tabs-store §4, §9.1 (`TexturesTab.cpp:2029-2035`) |
| 1.35 | "FOX's `.fcnp` lookup is a linear scan over every file on each model load" | TRACED [spot-checked] | tabs-store §9.3 (`fox ModelsTab.cpp:2474-2480`) |
| 1.36 | "POE2 embeds textures for hidden parts' materials" | TRACED | geometry-export §5.1 (material renumbering row), appendix |
| 1.37 | "FOX's Ctrl+double-click is the exact toggle-twice shape that template §11 scar 1 describes" | TRACED (report HUNCH) + CODE | viewport §1.4 marks this **HUNCH**; the plan states it as fact. Code confirms: `fox.cpp:3215-3218` press with Ctrl → `applyPickGesture`, `:3111` dbl-click → `applyPickGesture`, `:3093-3094` Ctrl = `toggleInSelection` |
| 1.38 | "FOX has no engine-neutral geometry type. Its parser output (`fox::FmdlFile`) goes straight to GPU-shaped uploads and its exporter consumes `FmdlFile` directly" | TRACED | tabs-store §7.2, plugin-build §1.2, geometry-export §1.3 |
| 1.39 | "An `FmdlFile → core::Geometry` adapter is the single largest piece of genuinely new code" | TRACED | tabs-store §7.2 (judgement, as marked there) |

### B.2 — §2 Target layout (factual assertions only)

| # | Claim | Verdict | Where / evidence |
|---|---|---|---|
| 2.1 | vcpkg union `qtbase{widgets,opengl,gui,png,jpeg}, qtsvg?, zlib, lz4` | TRACED [spot-checked] | plugin-build §7 |
| 2.2 | "the generic rules ONCE (today they are restated in three files)" | TRACED | plugin-build §10 |
| 2.3 | `games/d4` … "the eleven indexes" | **CONTRADICTED** (count) | plugin-build §1.1 "All 11 background indexes" / §3 "all eleven D4 indexes". Code: 7 classes implement the §2 shape (detached thread + `readyChanged` + `m_building`: AppearanceMeta, AssetLinks, BackTrophyIndex, IconIndex, ItemHoverIndex, StoreProductIndex, WardrobeAnimIndex); 11 only if every `instance()` singleton under `index/` and `tex/` is counted (adds AnimActionIndex, DadOverride, FrameTable, TextureDefTable — none threaded, none with `readyChanged`); the File ▸ Index roster has **9** rows and `d4/src/app/MainWindow.h:93` says "the nine indexes" |
| 2.4 | FOX `foxab_probe` re-lists sources by hand | TRACED [spot-checked] | plugin-build §6 (`fox/CMakeLists.txt:343-366`) |
| 2.5 | "FOX already has treeV/instS; POE2 staged three data bundles" | TRACED | plugin-build §9 (`NEW_SESSION_PROMPT.md:218-221`, not staged; `poe2/CLAUDE.md:189`) |
| 2.6 | "DIAssetBrowser (Python/PySide6 — a different line…)" | **UNTRACED** | No report says DIAssetBrowser is Python/PySide6. plugin-build §10 only notes the FOX template "names DIAssetBrowser as a sibling"; `d4/docs/ASSETBROWSER_TEMPLATE.md:5` and `NEXT_SESSION_TEMPLATE_TIERS.md:14` name it as a consumer of the same native template, and template `:138` says the family is "One native executable, no Python". Nothing in the trees describes DI's implementation language |
| 2.7 | `qt_standard_project_setup`, one `assetbrowser_configure_target()` | TRACED | plugin-build §6.3, §6.4 |
| 2.8 | `third_party\ooz` under POE2, lz4 under D4 | TRACED | plugin-build §6.5 |

### B.3.1 — Data types

| # | Claim | Verdict | Where / evidence |
|---|---|---|---|
| 3.1.1 | Geometry base = POE2 `ModelGeometry.h` (shared buffer, index-range parts, bbox, tangent, provenance) | TRACED | geometry-export §1.2, §1.4 verdict, §7 |
| 3.1.2 | Graft D4 `hasColor/hasUv1`, per-part `doubleSided`, `slotHash`; FOX `groupParent` | TRACED | geometry-export §1.1, §1.4, §7 (`Part`/`Geometry` sketch) |
| 3.1.3 | "widen joints to `uint16` (POE2's `uint8_t` caps rigs at 256 bones)" | TRACED [spot-checked] | geometry-export §1.4, appendix (`poe2 ModelGeometry.h:21`) |
| 3.1.4 | Strip D4 cloth block, `vertexBuffers`; FOX SoA layout | TRACED | geometry-export §1.1, §1.3, §7 |
| 3.1.5 | Skeleton base = POE2 shape with D4 per-joint content (name, hash, parent, rest TRS, IBM) | TRACED | geometry-export §2.5, §7 `Joint` |
| 3.1.6 | D4 `cloth/chain` + `nBaseBones` optional | TRACED | geometry-export §2.5 |
| 3.1.7 | "sort parents-before-children at ingest and assert (D4 and FOX already guarantee it; POE2 resolves recursively)" | TRACED [spot-checked] | geometry-export §2.1–2.3, §2.5 (`poe2 AstSkeleton.cpp:241-253`) |
| 3.1.8 | Strip D4 mixed-space contract (Y-up verts, Z-up rest TRS) | TRACED | geometry-export §1.4 end, appendix |
| 3.1.9 | Clip base = "POE2 sparse per-channel keys, **seconds**, bound by joint index" | **CONTRADICTED** (minor) | geometry-export §3.2: POE2 `KeySet` times are "**in frame units**" (`AstSkeleton.cpp:214`), divided by fps only at write (`GlbExporter.cpp:360`); "seconds" is the *core* choice from §3.4, not POE2's shape. Sparse + by-index are TRACED §3.2 |
| 3.1.10 | D4 dense → sparse (times = i/fps), hash → index at ingest; FOX baked-dense from the pose solver | TRACED | geometry-export §3.4 |
| 3.1.11 | Strip FOX `.frig`/IK solver, D4 AnimSet taxonomy | TRACED | geometry-export §3.3, §7 not-core list |
| 3.1.12 | Material base = POE2 `ExportMaterial` (alphaMode enum, specular colour, transmission, emissive strength) | TRACED | geometry-export §4.2 |
| 3.1.13 | D4 `hasX` presence flags; FOX `lossyOk`/`srgb` | TRACED | geometry-export §4.4, §7 `TextureImage` |
| 3.1.14 | one `NormalConvention` replacing D4 `flipNormalGreen`, FOX `normalsGreenDown`, POE2 `reconstructNormalZ` | TRACED | geometry-export §4.4 (which also lists D4 `reconstructNormalZ`) |
| 3.1.15 | Strip D4 dye/detail bakes, FOX colour-layer bake, FOX SRM→ORM easing; viewport-only fields → extension bag | TRACED | geometry-export §4.4, §7 not-core list; tabs-store §7.2 |
| 3.1.16 | Hardpoint = D4 `ModelHardpoint` + FOX `ConnectPoint` merged, `modelSpace` flag; POE2 none | TRACED | geometry-export §7 `Hardpoint`, §1.4 table |
| 3.1.17 | Mat4/Quat base = POE2 `RigMath.h`; graft "D4's `invertRigid/invert/decomposeTRS`" | **CONTRADICTED** (partly) | geometry-export §2.2 lists POE2 `RigMath.h` as already having `inverse` (Gauss-Jordan, `:33-55`) and `decomposeTRS` (`:109-142`); code confirms (`poe2/src/model/RigMath.h:33,109`). Only `invertRigid` is a D4-only graft (`d4 RigMath.h:66-78`) |
| 3.1.18 | FOX `quatFromMatrix`, `rotate`, `fromTo` | TRACED | geometry-export §2.4 |
| 3.1.19 | "all three already share one 16-float memory layout; only the multiply argument order differs" | TRACED [spot-checked] | geometry-export §2.4 |
| 3.1.20 | `AssetId` = `{quint8 scheme; quint64 value}` + plugin `toString/fromString` | TRACED (HUNCH) | plugin-build §4 |
| 3.1.21 | "D4 packs `(group<<32)\|sno`" | TRACED (wording) | plugin-build §4 says D4 "**fits** `(group << 32) \| sno`" — a proposal; D4 today passes `(group, sno)` pairs (plugin-build §4 table) |
| 3.1.22 | FOX carries GZ-vs-TPP scheme bit; POE2 is the MurmurHash | TRACED | plugin-build §4 |
| 3.1.23 | `AssetRow` from union in tabs-store §1.4; flags Encrypted/Unrenderable/Animated/Rigged/Orphaned/New (D4), Shadowed/Unnamed (FOX) | TRACED | tabs-store §1.4, §7 sketch |
| 3.1.24 | `ExportOptions` base FOX → `sceneOptionsFrom` + text presets | TRACED | geometry-export §5.1, §6.5 |
| 3.1.25 | POE2 fields (`gltf`, `looseTextures`, `onlyClip`) | TRACED + CODE | geometry-export §5.1 (`looseTextures .h:59`, `onlyClip .h:50-53`); `gltf` is not an `Options` field — it is `Config::exportGltf()` (`poe2/src/app/Config.h:45`) / `ExportOptionsDialog::wantGltf()` (`.h:25`) |
| 3.1.26 | D4 `animationLibrary`, `xMirror`, `hardpointEmpties`, retarget preset | TRACED | geometry-export §5.1 |
| 3.1.27 | "D4's ~20 loose QSettings keys read in `ModelsTab_Export.cpp`" | TRACED | geometry-export §6.5 ("~20"); grep of `QStringLiteral("<ns>/…")` in that file finds 15 distinct keys — order of magnitude holds |

### B.3.2 — Store and index

| # | Claim | Verdict | Where / evidence |
|---|---|---|---|
| 3.2.1 | `IGameStore` base = POE2 `AssetStore` API shape (`open/readFile/loadModel/loadSkeletonFor/resolveMaterials/loadTexture/attachmentsForModel/collectAssetFiles`) | TRACED | plugin-build §1.3 (`AssetStore.h:33,52-53,58,61,66,69,82,102`), tabs-store §7.1 |
| 3.2.2 | D4 `encryptedSnos`, `payloadSize`, two-blob reads → "part" selector | TRACED | tabs-store §1.1, §7.1 (`open(bytes)` row) |
| 3.2.3 | FOX `readContainer` (read once, pull children), install scope | TRACED (HUNCH) | plugin-build §3 (HUNCH paragraph; `InstallScope :184-193`) |
| 3.2.4 | Full sketch with per-method map in tabs-store §7 | TRACED | tabs-store §7, §7.1 |
| 3.2.5 | Threading contract: `open/loadModel/loadTexture/resolveMaterials` thread-safe; `row/tagGroups/searchBlob` GUI-thread | TRACED | tabs-store §7.3 |
| 3.2.6 | `BackgroundIndex` base FOX `ArchiveIndex` (detached thread → fingerprint-signed cache → `install()` queued → `readyChanged` → `reset()` → generation counter, all present) | TRACED + CODE | tabs-store §3 table; `fox/src/index/ArchiveIndex.h:17,236,242,266` |
| 3.2.7 | D4 `IndexDesc` roster for File ▸ Index | TRACED | app-shell §1.1 |
| 3.2.8 | "D4's older singletons (AppearanceMeta, AssetLinks, IconIndex) lack the generation counter the MainWindow comment claims they have" | TRACED [spot-checked] | tabs-store §3 note, §9.6 (`MainWindow.cpp:1029-1031`; grep = 0) |
| 3.2.9 | "POE2 mutates the store on the worker with no `install()` swap" | TRACED [spot-checked] | tabs-store §3, §9.7 (`IndexLoader.cpp:15-31`) |
| 3.2.10 | "Eleven D4 indexes re-implement this shape by hand — they become subclasses" | **CONTRADICTED** (count) | see 2.3 — seven implement the shape; eleven is the singleton count |
| 3.2.11 | Model load path base D4 `ModelsTab::loadGeometry` (worker thread, token check, `seh::runGuarded`, geometry cache) | TRACED [spot-checked] | tabs-store §2.1 |
| 3.2.12 | "FOX and POE2 parse synchronously on the GUI thread" | TRACED | tabs-store headline 2, §2.1, §9.4 |
| 3.2.13 | `RunCache` base D4 `MaterialDecode::TextureCacheScope`; FOX `blobcache::Scope`; POE2 no-op | TRACED + CODE | tabs-store §5, §7.1 (`MaterialDecode.h:86`, `Extract.h:99`) |

### B.3.3 — Viewport

| # | Claim | Verdict | Where / evidence |
|---|---|---|---|
| 3.3.1 | Base FOX: neutral `GLMeshUpload/GLSkeletonUpload/GLConnectPoint` inputs | TRACED | viewport §3.2, §4, §5 |
| 3.3.2 | Base FOX: `ShadingMode` enum, selection+context sets, hide/isolate/frame/fullscreen/help, `renderAtSize`, turntable, camera presets, RAII scope guards, harness hooks | TRACED | viewport §1.2, §1.4, §1.5, §1.6, §2.2, §5 |
| 3.3.3 | Graft POE2 `AlphaMode` two-pass (opaque+mask, then blend/additive) | TRACED [spot-checked] | viewport §1.6 (`poe2.cpp:765-783`) |
| 3.3.4 | Graft POE2 `renderToImage(scale, transparentBg, cropToModel)` | TRACED | viewport §1.5, §2.3 |
| 3.3.5 | Graft POE2 `skeletonMatchesMesh` fail-closed gate | TRACED + CODE | viewport §2.4 (unique to POE2); `poe2/src/gl/GLModelWidget.h:104` |
| 3.3.6 | Graft D4 click-on-release with `m_swallowLeftClick` cleared on every press, Ctrl/Shift toggle, right-click scoping | TRACED [spot-checked] | viewport §1.4 |
| 3.3.7 | Graft D4 stencil outline + `grabSupersampled` + `setCoverageAlpha` | TRACED | viewport §1.4, §1.5 |
| 3.3.8 | Graft D4 live-pose `partsBounds` framing, camera glide | TRACED | viewport §1.1 |
| 3.3.9 | Graft D4 Rendered pipeline (shadow map, SSAO, cubemap IBL, A2C, ACES) behind feature toggles | TRACED | viewport §1.6 |
| 3.3.10 | "overlay master gate moves into the widget (POE2's placement) so no tab can bypass it" | TRACED [spot-checked] | viewport §5 concrete plan; §1.3 placement table |
| 3.3.11 | Strip FOX `GLPbrMaterial` vocabulary, `DebugView`, `GameId`, `Hotkeys.h` include, QSettings camera presets | TRACED [spot-checked] | viewport §3.2 |
| 3.3.12 | Strip D4 `Config::d4dataDir()` read + cloth tuning JSON in `setGeometry` | TRACED [spot-checked] | viewport §3.1 (`d4.cpp:1187`) |
| 3.3.13 | Strip "the `detail_mask_probe.txt` write on every load" | TRACED [spot-checked] | viewport §3.1 (`d4.cpp:1324-1351`) |
| 3.3.14 | Strip bone-hash dictionaries | TRACED | viewport §3.1 |
| 3.3.15 | "~100 D4 material setters" | TRACED (report-internal drift) | viewport §5 says "~100 D4-material setters"; viewport §4 says "~40 per-part setters"; `grep -c 'set[A-Z]' d4.h:79-166` = 60 |
| 3.3.16 | "the cloth solver (~1,900 lines…)" | TRACED [spot-checked] | viewport §5 (~1 900; §1.6 says ~2 000); the two cited ranges sum to 1,906 |
| 3.3.17 | Strip POE2 hard-coded `toYUp`, in-widget engine pose evaluation | TRACED [spot-checked] | viewport §3.3 (`poe2.cpp:15,297`) |
| 3.3.18 | Furniture: FOX `ViewportBar`, `ViewportGizmo`, `ViewportHud`, `RenderPanel`; D4 `CameraOrbitRow`, `ViewportSettings` reset-by-removal groups | TRACED | viewport scope line, §1.1, §1.3; app-shell §4.2 |
| 3.3.19 | Part menu: D4 `ViewportPartMenu::{Info,Actions}` (labels count sets, plural vocabulary); FOX `partmenu::Context` (viewport + tree share one builder); strip D4 `Info::sno/collection/isSim/isFx` | TRACED + CODE | viewport §1.4, §3.1; `fox/src/util/SceneTree.cpp:217`, `ViewCapture.cpp:842` |
| 3.3.20 | `GLTextureWidget` POE2 `setImage(QImage)`; D4 `eTexFormat` parameter | TRACED | viewport §3.3, §3.1 |
| 3.3.21 | Thumbnails FOX `ThumbnailRenderer` (own thread + context + disk cache); POE2 `render(Geometry, baseColors)`; strip FOX `ArchiveIndex`/`ModelLoader` addressing | TRACED + CODE | viewport §1.5, §3.2, §3.3; `fox ThumbnailRenderer.h:28,161`, `poe2 ModelThumbnailRenderer.h:28` |
| 3.3.22 | Skinning: keep CPU (all three); FOX `applyPose(palette, skeletonLines)` | TRACED + CODE | viewport §1.6, §2.2; `fox.h:745-746` |

### B.3.4 — Export

| # | Claim | Verdict | Where / evidence |
|---|---|---|---|
| 3.4.1 | `GlbExporter` base POE2 (neutral inputs, in-memory `build()`, `.gltf+.bin`, loose textures, three KHR, MASK+BLEND, IBM-order and accessor-order scars documented, always-TRS) | TRACED [spot-checked] | geometry-export §5.3 item 1, §5.2 |
| 3.4.2 | D4 grafts: animation-library, time-accessor cache per (count,fps), hardpoint empties with `IBM·socket`, retarget layer (presets, `.L/.R`, X-mirror, `flipNormalGreen` self-test), `AnimExportScope`/`AnimClipFilter` | TRACED | geometry-export §5.3 "From D4" |
| 3.4.3 | FOX grafts: `usedMaterials` pruning, image de-dup + JPEG + size cap, wrapper-node scale/up-axis, constant-track elision, per-clip shared time accessor, `prepareMesh` shared with OBJ, `reduceRig`, u8/u16 joint width, `SceneAccum` keyed by name, `qInfo` summary | TRACED [spot-checked] | geometry-export §5.3 "From FOX" |
| 3.4.4 | Strip D4 `swapTRS`/`SymBone`/`DecodedAnim` coupling and "its two duplicated container tails"; FOX `FmdlFile` input | TRACED [spot-checked] | geometry-export §5.3 items 2–3 (`cpp:809-843` vs `:846-879`) |
| 3.4.5 | `ObjExporter` = FOX | TRACED | geometry-export §5.1 OBJ row |
| 3.4.6 | `ExportCapture` base FOX `ViewCapture` (options struct, interactive wrappers, memory guard, clipboard, one `encodeGif`) | TRACED | geometry-export §6.2 superset paragraph |
| 3.4.7 | Graft POE2's viewport interface (`renderToImage/orbitYaw/setAnimTime/currentClipFps`) | TRACED + CODE | geometry-export §6.2 (first three); `currentClipFps` at viewport §2.3 / `poe2.h:118` |
| 3.4.8 | Turntable clip-snap (D4/POE2 — FOX lacks it) | TRACED [spot-checked] | geometry-export §6.2 (FOX side is HUNCH there; grep confirms no `snap` in `ViewCapture.cpp`) |
| 3.4.9 | Strip D4 `settleCloth`/`CaptureScope` → pre-capture hook | TRACED | geometry-export §6.2 game-specific bits |
| 3.4.10 | "The ladder constants (¾, 32, 0.93, 96 px) are identical in all three copies" | TRACED [spot-checked] | geometry-export §6.2, appendix |
| 3.4.11 | `GifEncoder` — "any (byte-identical ×3)" | **CONTRADICTED** | geometry-export §6.1/appendix: "byte-identical apart from includes and a FOX provenance comment"; code: `.cpp` first line differs ×3, FOX `.h` carries a 12-line note (`fox/src/export/GifEncoder.h:2-13`). Only `GifEncoder.h` d4↔poe2 is identical |
| 3.4.12 | `ExportLayout` base FOX (universal sanitizer, `modes()` table with labels/hints, group + per-file APIs) | TRACED | geometry-export §6.3 |
| 3.4.13 | Graft POE2 self-test; D4 legacy `bulk/organize` migration | TRACED | geometry-export §6.3 |
| 3.4.14 | Three taxonomies → `folderKeyFor(item)` callback | TRACED | geometry-export §6.3 |
| 3.4.15 | `NameTemplate` base FOX `applyNameTemplate` (`{{Frame}}` auto-padded, `{{Part}}`, `{{Clip}}`, `{{Date}}`, tidy pass, " CON " story) | TRACED | geometry-export §6.4 |
| 3.4.16 | `{{Id}}` generic, `{{SNO}}` = D4 alias | TRACED | geometry-export §6.4 |
| 3.4.17 | "POE2's copy of D4's file has no UI and one literal call — dead" | TRACED [spot-checked] (HUNCH there) | geometry-export §6.4, appendix (`BulkExtractor.cpp:168`) |
| 3.4.18 | `Retarget` = FOX rules (readable, mirror, `reduceRig`) + D4 (`collapseClothChains`, `remapToAnchors`, X-mirror) | TRACED + CODE | geometry-export §5.1 retarget row; `d4 Retarget.h:17,28`, `fox Retarget.h:106` |
| 3.4.19 | "D4's bone renaming currently lives in `GLModelWidget` (`blenderizeSkeletonNames`) — it moves here" | TRACED [spot-checked] | geometry-export appendix (`d4.h:404-405`) |
| 3.4.20 | "D4's 26 curated anchor hashes stay in `games/d4`" | TRACED + CODE | geometry-export §7 not-core list; `d4/src/model/Retarget.h:19` "the 26 identified player-rig anchor bones" |

### B.3.5 — Application shell

| # | Claim | Verdict | Where / evidence |
|---|---|---|---|
| 3.5.1 | `AppShell` base FOX (first-run stack, status bar with `StatusLine` source filter, `applyHotkeys` by registry key, `populateExportMenu(QMenu*)` delegation, `restoreWindowLayout`, DevShot scheduling skeleton) | TRACED [spot-checked] | app-shell §2.2, §2.5 "Core keeps", §8.1 (`fox MainWindow.cpp:1304-1317`) |
| 3.5.2 | Graft D4 `BrowserTab` hooks + `LazyTab`, `IndexDesc` → submenu + indicator + toast, export toast/tray, Ctrl+K + Alt-nav with `jumpTo`, Help ▸ Copy diagnostic info | TRACED | app-shell §2.1, §8.2 |
| 3.5.3 | Strip hardcoded tab lists (all three) → `AppPlugin::tabs()` | TRACED | app-shell §2.4 |
| 3.5.4 | Strip D4 positional hotkey binding — "CONTEXT_MENUS §6 warns this 'binds the wrong keys silently'" | TRACED [spot-checked] | app-shell §1.1, §8.7; `d4/docs/CONTEXT_MENUS.md:584-585` (in §6) |
| 3.5.5 | Strip D4 hand-written F1 sheet | TRACED [spot-checked] | app-shell §2.1, §8.7 |
| 3.5.6 | Strip POE2 hotkeys bound once at construction | TRACED [spot-checked] | app-shell §2.3, §8.7 |
| 3.5.7 | `AppPlugin` sketch: `productName, tabs(), indexRoster(), reload(), helpEntries(), diagnosticInfo(), firstRunPage(), isConfigured(), settingsPages(), wireCrossTab()` | TRACED | app-shell §2.5 — matches method-for-method |
| 3.5.8 | `SettingsDialog` base FOX (`addPage`, `showTab("Export/Images")` by name, hotkey-clash detector) | TRACED | app-shell §1.1, §3.3 |
| 3.5.9 | Graft D4 `showEvent` screen clamp and Cancel snapshot/revert "(the only 'Cancel truly cancels' for live-written keys)" | TRACED [spot-checked] | app-shell §1.1 |
| 3.5.10 | Graft POE2 `restoreExportDefaults()` as reset-by-removal model | TRACED [spot-checked] | app-shell §4.2, §8.3 |
| 3.5.11 | Graft D4 `ViewportSettings` scoped groups + keep-prefixes | TRACED + CODE | app-shell §4.2, §6; `ViewportSettings.h:90,100` |
| 3.5.12 | `SettingsPage{title, order, build, apply, revert, resetGroups, keepPrefixes}` | TRACED | app-shell §3.3 (identical struct) |
| 3.5.13 | Core-owned pages list | TRACED | app-shell §3.3 |
| 3.5.14 | `Hotkeys` base FOX "(36 rows with hints, `seq(key)`, cheat-sheet text/HTML generators, `Role` lookup)" | **CONTRADICTED** (count) | app-shell §1.1 says "36 rows"; code: `fox/src/app/Hotkeys.h` `defs()` contains **32** `"hotkeys/…"` rows. Struct/`seq`/sheet/`Role` are TRACED (`:25-30,197-247,260-287`) |
| 3.5.15 | `AppLog`+`CrashHandler`+`LogConsole` FOX; graft D4 `setFileLogging` live toggle wired to Settings ▸ Diagnostics | TRACED [spot-checked] | app-shell §1.1 |
| 3.5.16 | "product name in the crash-file name → parameter" | TRACED + CODE | app-shell §6; `fox/src/app/CrashHandler.cpp:256` |
| 3.5.17 | `Config` base FOX pattern (accessor pair, named constants, session overrides — "what makes headless tests safe") | TRACED [spot-checked] | app-shell §1.1 |
| 3.5.18 | POE2 `exportOptionsFromConfig()` one-liner | TRACED + CODE | app-shell §8.3; geometry-export §5.1; `poe2/src/app/ExportConfig.h:9` |
| 3.5.19 | `ExportNotifier` base POE2; graft FOX `file` arg + after-export action; strip D4/FOX `glbOptionsLine` | TRACED | app-shell §1.1 |
| 3.5.20 | `StatusLine, StartupProfile, Density, FontScale, CheckStyle, RowShading, FirstRunPage, CacheMaint, NaturalOrder` — "FOX (zero coupling)" | TRACED (overstated) | app-shell §6: the first six and `NaturalOrder` are "none"; **`FirstRunPage` → `app/Config.h`** and **`CacheMaint` → `app/AppPaths.h`, `gl/ThumbnailRenderer.h`** are coupled (the plan's Graft column concedes both) |
| 3.5.21 | `main()` generic sequence | TRACED | app-shell §5 "Generic lines" |
| 3.5.22 | "POE2 prints to stderr which a Windows GUI build discards" | TRACED [spot-checked] | app-shell §1.1 (`poe2/src/main.cpp:61`) |
| 3.5.23 | Search FOX three-layer; POE2 `Query{and,not,meta,id}` + `isHexId`; `isIdTerm` policy injected | TRACED + CODE | app-shell §1.1, §8.4; `poe2 QueryTerm.h:29-33,41`, `fox QueryTerm.h:47` |
| 3.5.24 | Funnel: sticky popup + live counts (POE2), Match-any, chip flow (FOX `FilterChips`), tinted button | TRACED | app-shell §1.1, §8.5 |
| 3.5.25 | "POE2 `Facets.h` pulls `MaterialFamilyIndex`; FOX `TagFunnel` pulls `ModelTags`" | TRACED + CODE | app-shell §6, §8.5; `poe2 Facets.h:2`, `fox TagFunnel.cpp:8` |
| 3.5.26 | Menus: FOX `MenuText`+`MenuContext`+`MenuDump`; D4 `LookIcon::addActions` (disabled-not-hidden); `MenuText::Nouns` | TRACED + CODE | app-shell §6, §8.6; `d4 LookIcon.h:49` |
| 3.5.27 | Panels FOX `PanelBox`+`NPanel`+`PanelPersist` "(pane-count guard fixes a measured Qt defect)" | TRACED (bad line ref in report) | app-shell §1.1; guard at `fox/src/util/PanelPersist.h:37`, report cites `:103-133` in an 82-line file |
| 3.5.28 | glyphs from `ViewGlyphs` | TRACED | app-shell §6 |
| 3.5.29 | `AssetListModel` POE2 (`std::function` icon provider, "`applyFilters` once per keystroke", facet counts) | TRACED + CODE | tabs-store §1.3; "once per keystroke" not in a report — `poe2/src/index/AssetListModel.h:54-56` ("rebuild ONCE … on every keystroke") |
| 3.5.30 | D4 `SnoListModel` hooks (`setSearchBlob/setPredicate/setFailedPredicate/setPresence`) | TRACED | tabs-store §1.1, §1.4 |
| 3.5.31 | `CsvCopy`/`TableCopy` merge | TRACED | app-shell §1.1 |
| 3.5.32 | `HoverPreview` FOX + D4 `HoverInfo` | TRACED | app-shell §1.1 |
| 3.5.33 | `ThumbnailCache` POE2 (zero coupling) + FOX disk tier | TRACED + CODE | app-shell §1.1; `poe2 ThumbnailCache.cpp:1` only self-include |
| 3.5.34 | `HintBar` FOX + POE2 `setSizePolicy(Fixed)` fix "(the 'huge band' bug)" | TRACED + CODE | app-shell §1.1; the phrase in code is "tall band" (`poe2/src/util/HintBar.h:21`) |
| 3.5.35 | `TextReportDialog` D4/POE2 identical; FOX's inside `MgsvMetaDialog` | TRACED [spot-checked] | app-shell §1.1 (`cmp` identical) |

### B.3.6 — Tabs and bulk

| # | Claim | Verdict | Where / evidence |
|---|---|---|---|
| 3.6.1 | `ModelsTab` base POE2 (store-driven, tabbed panels Parts/Animations/Attachments/Info, one resolver) | TRACED + CODE | tabs-store §2.1; `poe2/src/tabs/ModelsTab.cpp:207-210` |
| 3.6.2 | Graft D4 worker-thread load; panel data set from tabs-store §6 | TRACED | tabs-store §2.1, §6 common denominator |
| 3.6.3 | Strip game-specific panels (D4 CLOTH/Looks/Dye; FOX RENDER/help bones; POE2 alpha modes) | TRACED | tabs-store §6 "engine extras" |
| 3.6.4 | `TexturesTab` POE2 + D4's funnel (formats, tags of using assets, orphans) | TRACED | tabs-store §4 D4 Textures row |
| 3.6.5 | "D4's tab must route through `QueryTerm` (today it uses bare `contains()`)" | TRACED [spot-checked] | tabs-store §4, §9.1 |
| 3.6.6 | atlas frames (D4-only) behind `IGameStore::atlasFrames` | TRACED | tabs-store §7.1 |
| 3.6.7 | `BulkTab` base POE2 `BulkExtractor` (standalone QObject taking store + items + options; coordinator loop) | TRACED [spot-checked] | tabs-store §5, §2.3 (`BulkExtractor.h:44`) |
| 3.6.8 | "TSan-clean" | CODE | not in any report; `poe2/CLAUDE.md:94` "TSan clean (0 races)" |
| 3.6.9 | FOX grafts (hash-keyed queue, path-aware manifest, dry run, saved queries, degraded-write rule, headless harness) | TRACED | tabs-store §5 |
| 3.6.10 | D4 grafts (Both mode, factory presets, co-textures, deps, buffers, per-folder passes, shared decode cache, per-item `seh::runGuarded`) | TRACED | tabs-store §5 |
| 3.6.11 | Strip D4 delegation to `ModelsTab::bulkExport` — "parallelism is disabled when animations are on because those paths read GUI state" | TRACED [spot-checked] | tabs-store §5, §9.8 |
| 3.6.12 | Strip FOX extension-name dispatch inside the worker | TRACED | tabs-store §5 |
| 3.6.13 | `BrowserTab` base D4 virtuals (`refresh/reset/onSettingsChanged/persistView` + export hooks CONTEXT_MENUS §6 lists); FOX `populateExportMenu`; D4 setters → `setStore` | TRACED [spot-checked] | app-shell §1.1, §2.5 HUNCH (`BrowserTab.h:50-70`) |

### B.3.7 — Engine-neutral code in game layers

| # | Claim | Verdict | Where / evidence |
|---|---|---|---|
| 3.7.1 | `bcdec` superset; D4 row-pitch alignment and fmt-46/47 rule sit above it | TRACED | plugin-build §2 BC row |
| 3.7.2 | DDS container probe (POE2 DX10 + FOX slice/mip walker) | TRACED | plugin-build §2 |
| 3.7.3 | FOX `BcEncode` the only encoder, round-trip self-test | TRACED | plugin-build §2 |
| 3.7.4 | `FoxZlib`, `ZipReader/ZipWriter`, `FoxCity` (Qt-free), MurmurHash64A and DJB2, `NaturalOrder` | TRACED | plugin-build §2 |
| 3.7.5 | hash schemes (51+13-bit, lowercase-then-hash) stay in plugins | TRACED | plugin-build §2, §4 |

### B.3.8 — Build and verification

| # | Claim | Verdict | Where / evidence |
|---|---|---|---|
| 3.8.1 | One CMake function replacing three copies of `/EHa /MP /bigobj NOMINMAX`, PCH, `.rc` OBJECT_DEPENDS, packaging; drift FOX `/Z7`, POE2 no PCH, no static branch | TRACED [spot-checked] | plugin-build §6 table, §6.3 |
| 3.8.2 | Drop `fastgltf`/`tinygltf` (unused) | TRACED [spot-checked] | plugin-build §6.2, §7 |
| 3.8.3 | Svg linked by D4/FOX, included by nothing, only for static plugin baking | TRACED [spot-checked] | plugin-build §6.1 |
| 3.8.4 | "Core headers get a `core/` prefix so the seven same-named headers stop shadowing each other" | TRACED (loose count) | plugin-build §6.7 lists 7; code: **15** headers share the same `src`-relative path in all three trees; the report's `util/NameTemplate.h` exists only in D4/POE2 |
| 3.8.5 | Root vcpkg manifest; FOX's baseline moves to match | TRACED [spot-checked] | plugin-build §7 |
| 3.8.6 | `verify-src.py` D4 base (checks 0–7 + dead-key + text-persisted-combo + name-substring with inventory/baseline), plus FOX `check_qprintable`, minus d4data JSON | TRACED | plugin-build §8 superset paragraph |
| 3.8.7 | Tables from `games/<x>/verify.toml` | TRACED | plugin-build §8 (proposal) |
| 3.8.8 | Retire POE2's port (no-op format check, dead-key check that cannot see concatenated keys, substring count that never fails) | TRACED [spot-checked] | plugin-build §8 |
| 3.8.9 | Self-test registry: "POE2 ten, FOX four, D4 two" | **CONTRADICTED** (FOX) | plugin-build §9 item 1 says four; code `fox/src/main.cpp:121-143` runs **five** (`bc::selfTest`, `bc::encodeSelfTest`, `QueryTerm`, `searchq`, `animpose`); app-shell §1.1 and §5 also say five. POE2 = 10, D4 = 2 (QueryTerm at startup + BcDecode in Wardrobe) hold |
| 3.8.10 | Linux compile replacing POE2's hand-listed `verify_container.sh` "which has already drifted and cannot link the current tree" | TRACED [spot-checked] | plugin-build §9 |
| 3.8.11 | POE2 `render_offscreen.cpp` includes a shader copy from `/root/work/scratch/` | TRACED [spot-checked] | plugin-build §9 (`:24`) |
| 3.8.12 | export twice + md5 | TRACED | plugin-build §9 item 5 |
| 3.8.13 | FOX stdout-needle harness (`--tab/--shot/--export`) under `xvfb-run` | TRACED | plugin-build §9 |

### B.4 — §4 Plugin interface

| # | Claim | Verdict | Where / evidence |
|---|---|---|---|
| 4.1 | Interface with per-method map in tabs-store §7; struct sketches in geometry-export §7; `AppPlugin` in app-shell §2.5 | TRACED | all three pointers correct |
| 4.2 | `IGameStore` method list | TRACED | tabs-store §7 (plan adds a `part` selector per §7.1 `open(bytes)` row) |
| 4.3 | `AppPlugin` extras (`isIdTerm` policy, `MenuText::Nouns`, cachemaint defs, self-test list, verify.toml) | TRACED | app-shell §8.4, §8.6, §8.1; plugin-build §8, §9 |
| 4.4 | "What each plugin implements today" (D4 `CascReader::open/readPayloadBySno/readMetaBySno`, `ModelParser::parseApp`, `buildExportMats`, `AppearanceMeta::tagsFor/titleFor`, `AssetLinks`, `IconIndex`; FOX `ArchiveIndex::rebuild/readFile`, `FmdlFile::parse`, `modelload::load*`, `ModelTags`, `TextureUsers/RefIndex`, `IconCatalog`; POE2 `AssetStore::*`) | TRACED | tabs-store §7.1 |
| 4.5 | "`buildExportMats` (must merge with `applyPartMaterials` — D4 has two material resolvers, POE2 one)" | TRACED | tabs-store §7.1, §9.5 |
| 4.6 | "D4's tab-owned indexes (`ensureEntityIndex`, `ensureAnimatedIndex`, blocklist, animated/rigged/orphaned sets — `ModelsTab.h:243-246, 362-402`) move to the D4 store" | TRACED [spot-checked] | tabs-store §7.1, §9.2 |
| 4.7 | "that is what lets `BulkExtractorTab` stop holding a `ModelsTab*`" | TRACED [spot-checked] | tabs-store §9.2 (`BulkExtractorTab.h:33,143`) |

### B.5 — §5 Migration order and gates (premises)

| # | Claim | Verdict | Where / evidence |
|---|---|---|---|
| 5.1 | Phase 0: "FOX treeV/instS" | TRACED | plugin-build §9 (not staged) |
| 5.2 | Phase 0: "POE2's three data bundles + a coat" | TRACED / **UNTRACED** | "3 data-table bundles" TRACED plugin-build §9 (`poe2/CLAUDE.md:189`); "+ a coat" is in no report — `poe2/CLAUDE.md:214,219` mention a "treasurehunter coat" as a *verification* asset (user screenshot), not a staged fixture |
| 5.3 | Phase 0: "D4 the five repro assets in `CLAUDE.md`" | TRACED + CODE | plugin-build §10 item 4 ("Repro assets (D4 `CLAUDE.md` last line)"); `d4/CLAUDE.md:39` lists five items (six asset names) |
| 5.4 | Phase 1: `ModelGeometry→core::Geometry` for D4 with the single Z-up swap; `FmdlFile→core::Geometry` for FOX — "the biggest new piece" | TRACED | geometry-export §1.4/appendix; tabs-store §7.2 |
| 5.5 | Phase 2 gate: "Blender import check (`blender_import_check.py`, pip bpy) and `gltf_skin_check.py`" | **UNTRACED** | Neither script is named in any report, and neither exists anywhere under `d4/`, `fox/`, `poe2/` (grep for `blender_import`, `skin_check`, `bpy` = only the plan itself) |
| 5.6 | Phase 3 gate: "part isolate test (POE2's `part_isolate_test` generalised)" | CODE | not in any report; `poe2/docs/D4_REUSE_AUDIT.md:48` names `tools/part_isolate_test.cpp` (not staged) |
| 5.7 | Phase 4 gate: POE2 self-tests and `bulk_verify` | TRACED | tabs-store §5 (headless harness row), plugin-build §9 |
| 5.8 | Phase 5: "The ~190-check `Verify - Regression.bat` and `regress.sh`" | TRACED + CODE | plugin-build §9; 191 invocations; `regress.sh` not staged ("147 checks", `NEW_SESSION_PROMPT.md:217`) |
| 5.9 | Phase 6: "`Audit - Asset Health.bat` diff shows no newly-broken" | TRACED | plugin-build §9 (not staged; template `:803-806`) |
| 5.10 | Phase 6: "the five `Dump -` / `Test -` bats still pass" | **UNTRACED** | No report names them. The trees name only `Dump - Piece Roster.bat` (template `:571`), `Dump - MarkingShape Layout.bat` and `Measure - Everything Owed.bat` (D4 docs); no `Test - *.bat` is mentioned anywhere and none is staged |
| 5.11 | "phase 1's FOX adapter and phase 6 are the two large items" | TRACED | tabs-store §7.2 (judgement) |

### B.6 — §6 Rules (premises)

| # | Claim | Verdict | Where / evidence |
|---|---|---|---|
| 6.1 | "The GIF ladder exists as three hand-copies with identical constants" | TRACED [spot-checked] | geometry-export §6.2 |
| 6.2 | "`ExportLayout` as three ports" | TRACED | geometry-export §6.3 |
| 6.3 | "the §2 index shape as thirteen hand-rolled singletons" | **UNTRACED** (derivable, and the derivation is unsound) | No report says thirteen. plugin-build §3 item 2 ("all eleven D4 indexes, FOX `ArchiveIndex`, and POE2 `IndexLoader`") sums to 13, but POE2 `IndexLoader` is owned by `MainWindow`, not a singleton (tabs-store §3; plugin-build §3 table), and the D4 eleven is over-counted (see 2.3). By code the family has 7 (D4) + 3 (FOX `ArchiveIndex`, `TextureUsers`, `RefIndex`) + 1 (POE2 `IndexLoader`) implementations of the §2 shape. Coincidentally `d4/src` has exactly 13 `instance()` singletons of any kind |
| 6.4 | "D4's showpiece tabs reach around `ModelsTab` into `ModelParser`/`MaterialDecode`/`CascReader` directly; POE2's Customize does the same against `AssetStore`" | TRACED | tabs-store §8 observation |
| 6.5 | Scars → tests: double-click swallow flag, IBM order, accessor-after-animation rule, pane-count guard, "Cancel truly cancels" snapshot | TRACED | viewport §1.4; geometry-export §5.2 (×2); app-shell §1.1 (×2) |

### B.7 — §7 What a new game costs

| # | Claim | Verdict | Where / evidence |
|---|---|---|---|
| 7.1 | Phase 0 "done best in POE2: research → `FORMATS.md` with the method stated → a Python oracle → the C++ parser verified against the oracle" | TRACED / CODE | `FORMATS.md` as ground truth TRACED plugin-build §10; "Python oracle" in no report — `poe2/CLAUDE.md:17` ("The Python reference parsers in `tools/formats/` are the oracle the C++…"), `poe2/docs/FORMATS.md:10` |
| 7.2 | "distinguishing present · unnamed · absent" | TRACED | plugin-build §4 table row |
| 7.3 | "the seven items `plugin-build.md` §10 found to be the only genuinely per-game content" | TRACED | plugin-build §10 lists exactly seven |
| 7.4 | "so it does not grow to 1,279 lines as FOX's has" | TRACED [spot-checked] | plugin-build §10; `wc -l` = 1279 |

### B.8 — §8 Decisions (premises)

| # | Claim | Verdict | Where / evidence |
|---|---|---|---|
| 8.1 | "All three projects already use one 16-float memory layout (translation at [12..14]); D4 is column-vector `mat4mul(a,b)`, POE2 and FOX row-vector `mul(a,b)` = 'a then b'" | TRACED [spot-checked] | geometry-export §2.4 |
| 8.2 | "two of three and the CPU-skinning code use it" | TRACED (HUNCH) | geometry-export §2.5 |
| 8.3 | "D4's function is the same bytes with swapped arguments" | TRACED | geometry-export §2.4 |
| 8.4 | "FOX has the structure the template asks for and neutral inputs; D4 has more rendering features but its 6.7k-line widget reads the game-data directory and writes a probe file on every load" | TRACED [spot-checked] | viewport §5 |
| 8.5 | "Alternative is D4 as base with the cloth/dye/fur/FX systems extracted out — larger surgery" | TRACED | viewport §5 honest trade-off |
| 8.6 | "FOX and POE2 load on the GUI thread today" | TRACED | tabs-store headline 2, §9.4 |
| 8.7 | `AssetId` struct vs plain `quint64` with D4 giving up group scoping | TRACED | plugin-build §4 |
| 8.8 | "Svg module: linked by D4 and FOX, included by nothing" | TRACED [spot-checked] | plugin-build §6.1 |
| 8.9 | "DIAssetBrowser: outside this extraction (Python)" | **UNTRACED** | see 2.6 |
| 8.10 | Defect lists in tabs-store §9, app-shell §8.7, geometry-export appendix | TRACED | pointers correct |

---

## C. Pointer check — every "(`report.md` §n)" in the plan

| Plan location | Pointer | Contains what is attributed? |
|---|---|---|
| §0 table of reports | five one-line summaries | ✔ |
| §1 FOX bullet | `app-shell.md §1, viewport.md §5` | **partial** — the "~190-check regression suite" is in `plugin-build.md §9`, not in either |
| §1 POE2 bullet | `geometry-export.md §1.4, §5.3; tabs-store.md §5, §7` | **partial** — "scoped reset-by-removal" is `app-shell.md §4.2`; "ten-entry self-test table" is `app-shell.md §1.1/§5` / `plugin-build.md §2` |
| §1 D4 bullet | `viewport.md §1, geometry-export.md §5, app-shell.md §1` | **partial** — "worker-thread model loading with token and SEH guard" is `tabs-store.md §2.1`; "`verify-src.py` (12 check functions)" is `app-shell.md §7` / `plugin-build.md §8` |
| §1 defects | `tabs-store.md §9, app-shell.md §8.7, geometry-export.md appendix, viewport.md §1.4` | **partial** — "D4 links `fastgltf` and `tinygltf`" is `plugin-build.md §2/§6` |
| §1 FOX geometry | `tabs-store.md §7.2, plugin-build.md §1.2` | ✔ |
| §1 D4 hotkeys | `CONTEXT_MENUS §6` | ✔ (`d4/docs/CONTEXT_MENUS.md:584-585`) |
| §3.1 heading | `geometry-export.md §1–4, §7` | ✔ |
| §3.1 AssetId | `plugin-build.md §4` | ✔ |
| §3.1 AssetRow | `tabs-store.md §1.4` | ✔ |
| §3.2 heading | `tabs-store.md §3, §7; plugin-build.md §3` | ✔ |
| §3.2 IGameStore notes | `tabs-store.md §7` | ✔ |
| §3.3 heading | `viewport.md §1–6` | ✔ |
| §3.4 heading | `geometry-export.md §5–6` | ✔ |
| §3.5 heading | `app-shell.md §1–5, §8` | ✔ |
| §3.5 AppPlugin | `app-shell.md §2.5` | ✔ |
| §3.5 SettingsPage | `app-shell.md §3.3` | ✔ |
| §3.5 main() | `app-shell.md §5` | ✔ |
| §3.6 heading | `tabs-store.md §2, §5, §6` | ✔ |
| §3.6 ModelsTab panels | `tabs-store.md §6` | ✔ |
| §3.7 heading | `plugin-build.md §2` | ✔ |
| §3.8 heading | `plugin-build.md §6–9` | ✔ |
| §4 | `tabs-store.md §7; geometry-export.md §7; app-shell.md §2.5` | ✔ |
| §7 | `plugin-build.md §10` | ✔ |
| §8.1 | `geometry-export.md §2.4` | ✔ |
| §8.2 | `viewport.md §5` | ✔ |
| §8.8 | `tabs-store.md §9, app-shell.md §8.7, geometry-export.md appendix` | ✔ |

Four of the §1 pointers each omit the report that actually holds one of the attributed facts; no pointer names a section that does not exist.

---

## D. Base / Graft / Strip vs the reports' own recommendations

| Report recommendation | Plan table | Agreement |
|---|---|---|
| viewport §5: FOX structural base; port in POE2 pass structure + D4 outline/`grabSupersampled`/`setCoverageAlpha`; neutral material struct with engine-extension pointer; overlay gate into widget; widget-owned selection with a mirror signal (D4's `partClicked` shape) | §3.3 | ✔ item-for-item |
| geometry-export §5.3: POE2 exporter skeleton + the enumerated D4 and FOX grafts | §3.4 `GlbExporter` | ✔ every listed graft appears |
| geometry-export §6.1–6.5: one `GifEncoder` home; one ladder; ExportLayout = FOX mechanism + POE2 self-test + D4 migration; NameTemplate = FOX engine with `{{Id}}`; ExportCapture = FOX structure + POE2 API + D4 hook | §3.4 rows | ✔ (the "byte-identical ×3" label is the only mismatch — C-9) |
| app-shell §8.1–8.6 | §3.5 | ✔ every module's Base/Graft/Strip matches; app-shell §8.2's `ProcQuiet` is simply omitted (not contradicted) |
| app-shell §8.6: D4 part-menu builder goes to the *viewport* core | §3.3 Part menu row | ✔ |
| tabs-store §5: FOX run machinery, D4 run semantics, POE2 cleanest separation | §3.6 `BulkTab` | ✔ |
| tabs-store §7 / §7.1: `IGameStore` shape and per-method map | §3.2, §4 | ✔ |
| tabs-store §9.4: lift D4's worker/token/SEH load pattern | §3.2 Model load path | ✔ |
| plugin-build §2: bcdec + DDS probe + FoxZlib + Zip + FoxCity + NaturalOrder to core | §3.7 | ✔ |
| plugin-build §6, §8, §9: one CMake function, D4 verify-src base + FOX qprintable, conformance registry | §3.8 | ✔ (except the FOX self-test count — C-10) |

No Base/Graft/Strip assignment contradicts a report recommendation.

---

## E. The plan's numbers, recomputed

| Number in plan | Recomputed | Holds? |
|---|---|---|
| 513 files | `find d4/src fox/src poe2/src -type f` = 140 + 282 + 91 = **513** (all `.cpp`/`.h`); = plugin-build §1's 58+82+130+152+39+52 | ✔ |
| ~198k lines | **197,814** lines in those 513 files (206,112 counting docs/CMake/verify scripts) | ✔ |
| Hotkeys.h 289 vs 38 | fox 289; d4 38; poe2 38 (d4 = poe2 byte-identical) | ✔ |
| 12 verify-src check functions (D4) | `grep -c '^def check_'` d4 = **12**, fox = 8, poe2 = 0 (inline blocks) | ✔ (app-shell §7's "11" is the odd one out) |
| ~190 regression checks | `Verify - Regression.bat`: 186 `:check` + 2 `:checkno` + 1 `:checkre` + 2 `:rc` = **191** (plugin-build §9 says 189+2+1+2 = 194) | ✔ as "~190" |
| "eleven indexes" in D4 | 7 classes with detached thread + `readyChanged` + `m_building`; 3 of those with `m_generation`; 11 `instance()` singletons under `index/`+`tex/`; roster = 9 rows; `MainWindow.h:93` "the nine indexes" | ✘ under the report's own definition ("§2 shape … `m_ready/m_building/m_generation`"); 11 only as "index-ish singletons" |
| "thirteen hand-rolled singletons" | not stated anywhere; 11 (D4, over-counted) + FOX `ArchiveIndex` + POE2 `IndexLoader` (not a singleton) = 13 by plugin-build §3 arithmetic; by code 7 + 3 + 1 = 11 §2-shape implementations family-wide; `d4/src` alone has 13 `instance()` singletons of any kind | ✘ |
| FOX Hotkeys "36 rows" | **32** `"hotkeys/…"` rows in `defs()` | ✘ |
| "POE2 ten, FOX four, D4 two" self-tests | 10 / **5** / 2 | ✘ (FOX) |
| "~1,900 lines" cloth | 1,906 in the two cited ranges | ✔ |
| "6.7k-line widget" | 6,727 | ✔ |
| "1,279 lines" FOX kickoff | 1,279 | ✔ |
| "five repro assets" | 5 items in `d4/CLAUDE.md:39` | ✔ |
| "seven same-named headers" | plugin-build lists 7; 15 headers share a path across all three trees | ~ (undercount, harmless) |
| "~20 loose QSettings keys" in `ModelsTab_Export.cpp` | 15 distinct literal keys | ~ |
| "26 curated anchor hashes" | `Retarget.h:19` "26 identified player-rig anchor bones" | ✔ |

---

## F. Every UNTRACED and CONTRADICTED item, with a suggested correction

### CONTRADICTED

**C-1** (§1) — *"A same-name comparison across the three `src` trees found exactly one file byte-identical in all three — `GifEncoder.cpp` — plus `SehGuard`."*
Code: the only byte-identical-×3 file is `SehGuard.cpp`; `GifEncoder.cpp` differs in all three by its `#include` line; `SehGuard.h` differs in FOX by a typo. Reports: geometry-export §6.1 "the `.cpp` differs only in include path"; app-shell §1.2 "FOX's `SehGuard.cpp` is byte-identical to D4's; the header differs only by a comment typo".
→ *"found exactly one file byte-identical in all three — `SehGuard.cpp` — plus `GifEncoder.{h,cpp}`, which differ only by an include path and a FOX provenance comment."*

**C-2** (§1) — *"Every other file that carries the same name in two projects is a rewrite: line similarity below 0.30 for … `QueryTerm`, `Config`, `ExportNotifier`."*
No report gives a similarity figure. difflib line ratio: `QueryTerm.h` d4/poe2 = 0.39; `ExportNotifier.h` d4/poe2 = 0.40, d4/fox = 0.31; `HintBar.h` d4/poe2 = 0.96; `AppPaths.h` 0.44–0.53.
→ Drop `QueryTerm` and `ExportNotifier` from the "below 0.30" list (or say "below 0.40"); replace "Every other file … is a rewrite" with "Apart from `HintBar.h` (a near-copy) and `AppPaths.h`, every other …", and state the metric.

**C-3** (§1, §3.6) — *"an `AssetStore` whose four methods are all `BulkExtractor` needs"*
tabs-store §5 says the same (`BulkExtractor.cpp:45-68`); code: `BulkExtractor.cpp` calls seven store methods (`loadModel`, `loadSkeletonFor`, `resolveMaterials`, `loadTexture`, `collectAssetFiles`, `readFile`, `index`).
→ *"an `AssetStore` of which `BulkExtractor` needs only seven methods (four for models/textures, three more for raw export)"*; fix tabs-store §5 too.

**C-4** (§2 "the eleven indexes"; §3.2 "Eleven D4 indexes re-implement this shape by hand") — plugin-build §1.1/§3 say eleven; code: 7 classes implement the §2 shape (thread + `readyChanged` + `m_building`), only 3 carry `m_generation`; the roster has 9 rows and `MainWindow.h:93` says "the nine indexes"; 11 counts every `instance()` singleton under `index/` and `tex/` including four synchronous loaders.
→ *"the seven background indexes (nine roster rows; eleven index-like singletons)"*; fix plugin-build §1.1 line 52 ("All 11 … each … with its own `m_ready/m_building/m_generation`") to name the seven and note that only BackTrophy/StoreProduct/WardrobeAnim have a generation counter.

**C-5** (§3.1 `Clip` row) — *"Base: POE2 sparse per-channel keys, seconds, bound by joint index"*
geometry-export §3.2 / `AstSkeleton.cpp:214`: POE2 key times are in **frame units**, divided by fps at write.
→ *"POE2 sparse per-channel keys (frame units today → seconds in core), bound by joint index"*.

**C-6** (§3.1 `Mat4/Quat` row) — *"Graft: D4's `invertRigid/invert/decomposeTRS`"*
geometry-export §2.2 and `poe2/src/model/RigMath.h:33,109`: POE2 already has `inverse` and `decomposeTRS`.
→ *"Graft: D4's `invertRigid`"* (and, if wanted, D4's `kSwapZtoY/kSwapYtoZ` basis constants).

**C-7** (§3.4 `GifEncoder` row) — *"any (byte-identical ×3)"*
geometry-export §6.1/appendix: identical "apart from includes and a FOX provenance comment".
→ *"any (identical ×3 up to the include path and FOX's provenance note)"*.

**C-8** (§3.5 `Hotkeys` row) — *"FOX (36 rows with hints…)"*
app-shell §1.1 says 36; `fox/src/app/Hotkeys.h` `defs()` has 32 rows.
→ *"FOX (32 rows with hints…)"*; fix app-shell §1.1.

**C-9** (§3.8 conformance) — *"a registry of every `QString selfTest()` (POE2 ten, FOX four, D4 two…)"*
plugin-build §9 says four; `fox/src/main.cpp:121-143` runs five (`bc::selfTest`, `bc::encodeSelfTest`, `QueryTerm`, `searchq`, `animpose`); app-shell §1.1/§5 say five.
→ *"POE2 ten, FOX five, D4 two"*; fix plugin-build §9.

**C-10** (§1 D4 bullet, indirectly) — *"the most complete `verify-src.py` (12 check functions)"* is right by code and by plugin-build §8, but **app-shell §7 says "D4 has 11 checks"**. The plan is not wrong; the report is.
→ Fix app-shell §7 to 12 (its own table lists twelve D4 rows).

**C-11** (§3.5 `StatusLine…` row) — *"FOX (zero coupling)"* for a list that includes `FirstRunPage` and `CacheMaint`; app-shell §6 shows `FirstRunPage` → `app/Config.h` and `CacheMaint` → `app/AppPaths.h` + `gl/ThumbnailRenderer.h`.
→ *"FOX (zero coupling except `FirstRunPage`→`Config` and `CacheMaint`→`ThumbnailRenderer`, both injected per the Graft column)"*.

### UNTRACED

**U-1** (§2, §8.7) — *"DIAssetBrowser (Python/PySide6 — a different line; a rewrite as a `games/di` plugin is possible later but is not part of this extraction)"* and *"DIAssetBrowser: outside this extraction (Python)."*
No report and no file in the trees says what DIAssetBrowser is written in; the template names it as a sibling consumer of the same *native* template (`d4/docs/ASSETBROWSER_TEMPLATE.md:5`, `:138` "no Python").
→ Either cite the source of the Python/PySide6 fact (outside these trees) or soften to *"DIAssetBrowser (not audited; not in these trees)"*.

**U-2** (§5 Phase 2 gate) — *"Blender import check (`blender_import_check.py`, pip bpy) and `gltf_skin_check.py` on both"*
Neither script is named in any report nor present anywhere under `d4/`, `fox/`, `poe2/`.
→ Say these are **to be written** in phase 2, or cite where they exist; as written the gate depends on tooling the audit never saw.

**U-3** (§5 Phase 6 gate) — *"the five `Dump -` / `Test -` bats still pass"*
No report names them; the trees name only `Dump - Piece Roster.bat`, `Dump - MarkingShape Layout.bat`, `Measure - Everything Owed.bat`; no `Test - *.bat` is mentioned anywhere and none is staged (plugin-build §9: "no container path and no regression driver staged").
→ Name the bats explicitly and mark them "not staged — run on the user's machine", or drop the count.

**U-4** (§6 rule 1) — *"the §2 index shape as thirteen hand-rolled singletons"*
No report states thirteen. It is derivable from plugin-build §3 item 2 (eleven D4 + FOX `ArchiveIndex` + POE2 `IndexLoader`), but POE2 `IndexLoader` is owned by `MainWindow`, not a singleton, and the eleven is over-counted (C-4). By code: 7 (D4) + 3 (FOX `ArchiveIndex`, `TextureUsers`, `RefIndex`) + 1 (POE2 `IndexLoader`) = 11 implementations of the §2 shape.
→ *"the §2 index shape as roughly a dozen hand-rolled implementations (seven D4 singletons, three FOX, POE2's `IndexLoader`)"*.

**U-5** (§5 Phase 0) — *"POE2's three data bundles + a coat"*
"+ a coat" is in no report; `poe2/CLAUDE.md:214,219` mention a "treasurehunter coat" as the asset a fix was verified on (user screenshot), not as a staged fixture.
→ *"POE2's three staged data-table bundles (plus a coat mesh to be pulled — the 'vertex explosion' repro)"*.

**U-6** (§7) — *"a Python oracle"* (and §3.6 *"TSan-clean"*, §5 Phase 3 *"`part_isolate_test`"*, §3.5 *"once per keystroke"*, *"huge band"*) — none of these appear in the five reports; all are supported by files in the trees (`poe2/CLAUDE.md:17,94`, `poe2/docs/D4_REUSE_AUDIT.md:48`, `poe2/src/index/AssetListModel.h:54-56`, `poe2/src/util/HintBar.h:21`) and are therefore tallied as **CODE**, not UNTRACED — listed here only because the plan's preamble promises every classification is "grounded in … the five audit reports".
→ Add a footnote pointing at `poe2/CLAUDE.md` / `D4_REUSE_AUDIT.md` for these, or fold them into plugin-build §9/§10.

### Report-side citation errors found during spot-checks (not plan defects, but worth fixing before the plan cites them)

- app-shell §1.1 cites `fox/src/util/PanelPersist.h:103-133` for the pane-count guard; the file is 82 lines, the guard is at `:37`.
- plugin-build §9 counts 189 `:check` invocations; the file has 186 (one behind an `if exist` guard at `:413`).
- viewport §4 ("~40 per-part setters") vs viewport §5 ("~100 D4-material setters"); the header block `d4.h:79-166` has 60 `set…` declarations.
- viewport §1.6 ("~2 000 lines" cloth) vs viewport §5 ("~1 900"); the cited ranges total 1,906.
