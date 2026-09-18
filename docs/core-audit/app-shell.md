# Application shell and shared UI helpers — audit across d4 / fox / poe2

Scope: `src/main.cpp`, `src/app/*`, and the header-only UI helpers under `src/util/`, `src/tabs/` (d4) and `src/view/` (fox) in the three sibling browsers, read against `d4/docs/ASSETBROWSER_TEMPLATE.md` (§1, §3, §4, §7, §9, §12, §16–18, §21, §23, §28, §29) and `d4/docs/CONTEXT_MENUS.md` §6–§8. Every claim cites `project/path:line`; inferences are marked **HUNCH**. Paths are relative to `/home/claude/audit/`.

Conventions used below: **D4** = `d4/src`, **FOX** = `fox/src`, **POE2** = `poe2/src`. "Identical" means byte-identical per `diff`.

---

## 1. Shared helper concepts — who has what, and which version is the superset

### 1.1 Summary table

| Concept | D4 | FOX | POE2 | Superset / core candidate | Why (evidence) |
|---|---|---|---|---|---|
| Hotkey registry | `app/Hotkeys.h` (38 lines, 6 rows: `key,label,def`) | `app/Hotkeys.h` (289 lines, 32 rows: `key,label,def,hint`; `seq(key)` overload; cheat-sheet text/HTML generators; `Role` namespace for finding actions by stamped property) | identical to D4 (`diff` clean) | **FOX** | FOX is a strict superset of the struct (`fox/src/app/Hotkeys.h:25-30` adds `hint`) and adds `sheetRows/cheatSheetText/cheatSheetHtml` (`:197-247`) and role-based lookup (`:260-287`). Applies by key lookup, not array position (`fox/src/app/MainWindow.cpp:1304-1317`), unlike D4's positional walk (`d4/src/app/MainWindow.cpp:509-517`) which CONTEXT_MENUS §6 itself warns "a reordering that desyncs them binds the wrong keys silently". Coupling: none (Qt only). |
| Log buffer + console + AppLog | `app/AppLog.h` (toggle + path; definitions live in `main.cpp:126-150`), `app/LogConsole.*` (`LogBuffer` 5000-line cap, `ConsoleWindow` with substring filter/Copy/Clear) | `app/AppLog.{h,cpp}` (`install()`, `tail()` 4000 lines, `Notifier` QObject for queued delivery, feeds crash note), `app/LogConsole.*` (per-level toggles, follow, debounced filter, header count, opens log file/folder) | `AppLog.h` identical to D4 but **no definition anywhere** (`grep AppLog:: poe2/src --include=*.cpp` → none); `LogConsole.*` identical to D4 and compiled (`poe2/CMakeLists.txt:49`) but **never instantiated** (no `ConsoleWindow`/`LogBuffer` reference outside its own files; no `qInstallMessageHandler` in `poe2/src/main.cpp`) | **FOX** for the logger (`AppLog::install/tail/Notifier`), **D4** for the live file toggle (`AppLog::setFileLogging`, `d4/src/main.cpp:137-150`, wired to Settings ▸ Interface ▸ Diagnostics `d4/src/app/SettingsDialog.cpp:749-791`) | FOX always writes the file (`fox/src/app/AppLog.cpp:80-88`); D4 gates it on `log/autoFile` (`d4/src/main.cpp:273`). Core should take FOX's structure plus D4's toggle. FOX's console reads level from the formatted line (`fox/src/app/LogConsole.cpp:28`) — the format string is the contract. **HUNCH:** POE2's AppLog.h/LogConsole are dead scaffolding copied for parity; nothing would break by deleting them. |
| Settings dialog skeleton | `makeTab` lambda → `QScrollArea` per page (`d4/src/app/SettingsDialog.cpp:134-144`); no-elide (`:131-133`); `showEvent` folds tab-bar width + clamps to screen (`:2802-2828`); `showExportTab()` by index (`:2581-2584`); Cancel reverts live keys via snapshot (`:2537-2579`) | `addPage()` → `QScrollArea` (`fox/src/app/SettingsDialog.cpp:929-939`); no-elide (`:66-68`); `showEvent` folds bar width (`:973-983`); `showTab("Export/Images")` by **name** with sub-tab (`:941-971`); hotkey-clash detector (`:898-927`); Cancel reverts live density/font (`:886-892`) | `wrapScroll()` (`poe2/src/app/SettingsDialog.cpp:65-72`); no-elide (`:37-39`); `showEvent` width fold (`:74-80`); pages as `make*Tab()` methods (`:41-45`) | **FOX skeleton** (`addPage`, `showTab` by name, clash check) + **D4's** screen clamp (`d4:2802-2828`) and Cancel-snapshot (`d4:2537-2579`) | POE2 is the cleanest small shape (named page factories) but has no screen clamp, no Cancel-revert, no open-on-named-tab. D4's snapshot/derived-namespace revert is the only implementation of "Cancel truly cancels" for live-written keys. |
| Config / QSettings access | `app/Config.{h,cpp}` — 4 accessors, keys `paths/*`, `casc/product` | `app/Config.{h,cpp}` — ~25 accessors, namespaces `paths/`, `index/`, `render/`, `view/`, `session/`, `interface/`, `textures/`; **session overrides** for the harness (`fox/src/app/Config.cpp:30-38`, `Config.h:127-140`) | `app/Config.{h,cpp}` — ~20 accessors, `paths/`, `export/`, `image/` | **FOX** pattern (accessor pair per key, named constants, session overrides) | All three follow "one accessor pair = one key" (`fox/src/app/Config.cpp:8-9` comment; `poe2/src/app/Config.h:5`). FOX alone has the non-persisted session override which is what makes headless tests safe. |
| ExportNotifier | `app/ExportNotifier.h` — singleton, `notify(text, folder)`, `glbOptionsLine(ModelExporter::Options)` | `app/ExportNotifier.{h,cpp}` — `notify(text, folder, file)`, `glbOptionsLine(fox::ExportOptions)`, **after-export action** (none/reveal/command with `{{File}}`/`{{Folder}}`) + session override (`fox/src/app/ExportNotifier.cpp:24-74`) | `app/ExportNotifier.h` — `notify(text, folder)` only; deliberately drops `glbOptionsLine` (`poe2/src/app/ExportNotifier.h:11-13`) | **FOX** shape; `glbOptionsLine` must NOT go in core (game-typed) | The signal/singleton is generic; both D4 and FOX bolt a game-typed options-line onto it (`d4/src/app/ExportNotifier.h:6,35`; `fox/src/app/ExportNotifier.h:19,69`). POE2 shows the core-sized version. Core = POE2's class + FOX's `file` argument + FOX's after-export hook. |
| HintBar / TipBar | `tabs/HintBar.h` — `makeHintBar(parent,text,settingsKey)` free function, key is caller-supplied full key | `view/TipBar.{h,cpp}` — class, key under `tips/`, painted ✕ glyph, `resetAll()` (wired in Settings ▸ Interface `fox/src/app/SettingsDialog.cpp:322-336`) | `util/HintBar.h` — D4's + `setSizePolicy(Fixed)` fix (`poe2/src/util/HintBar.h:21-22`) | **FOX** (namespaced key, reset-all) with POE2's size-policy fix | D4/POE2 callers pass ad-hoc keys (`d4/src/tabs/ModelsTab.cpp:983`, `poe2/src/tabs/ModelsTab.cpp:218`); FOX centralises the namespace and gives the user a way back. TipBar depends on `view/ViewGlyphs.h` for the ✕ (`fox/src/view/TipBar.cpp:3`). |
| Panel stack (PanelBox / NPanel) | `tabs/PanelBox.h` — `PanelBox`, `panelBoxFloor`, `panelBoxArrive`; typed ▲▼✕ glyphs; includes `BrowserTab.h` for `kHdrQss` | `view/PanelBox.h` — same API + `key` member, greedy-detection by `findChild<QAbstractScrollArea*>` (`:125-126`), floor = declared min only (`:193-196`), clamp 260; **`view/NPanel.{h,cpp}`** — the whole column: icon strip, `addPanel(key,title,glyph,content)`, `restoreState`, persisted `<prefix>/open`, `/order`, `/stack` as stable keys (`fox/src/view/NPanel.h:17-29`), `ShotGuard` | none in scope (POE2 uses a `QTabWidget` of panels per `poe2/CLAUDE.md`) | **FOX** (`PanelBox` + `NPanel`) | D4 has only the box; the strip/order/open persistence is re-implemented per tab (`ModelsTab_Panels.cpp`, `WardrobeTab2_Panels.cpp` — out of scope, **HUNCH** based on `d4/src/app/SettingsDialog.cpp:970,1041` having per-tab `rememberPanels` keys). FOX's PanelBox depends on `view/ViewGlyphs.h` (`fox/src/view/PanelBox.h:27`) for painted glyphs. |
| PanelPersist | `util/PanelPersist.h` (31 lines; key `view/rememberPanels`) | `util/PanelPersist.h` (82 lines; key `interface/rememberPanels`; stores `<key>/panes` count and refuses a blob whose pane count differs `fox/src/util/PanelPersist.h:37-60`; logs each outcome; connects the save even when disabled) | identical to D4, **no callers** (`grep PanelPersist poe2/src` → header only) | **FOX** | FOX fixes a measured Qt defect (2-pane blob into 3-pane splitter returns true, leaves 0px pane). Only the setting key differs; core needs the key injected. |
| QueryTerm / search language / search box / history | `util/QueryTerm.h` — `matches()` + 10-case self-test (per-term only; outer AND/`-`/`#` parsed in three tabs per `d4/src/util/QueryTerm.h:8-11`) ; search history is per-tab (`d4/src/tabs/ModelsTab.cpp:1819-1848`, key `models/searchHistory`) | `util/QueryTerm.h` — `matches()` + `isIdTerm()` (0x-hex / ≥6 decimal / ≥8 bare-hex) + `matchesWithId()` + self-test; `util/SearchQuery.h` — full query class (`+`/`-`/quotes/`#tag`/`-#tag`, `tooltip()`, `selfTest()`); `util/SearchBox.h` — `attach(box,key)` Esc/↓-history/remember-on-commit, sets `objectName("foxabSearchBox")` for Ctrl+F (`fox/src/util/SearchBox.h:112-146`) | `util/QueryTerm.h` — `matches()` + `Query{and,not,meta,id}` + `parse()`/`test()` + `isHexId()` + self-test; no search-box helper, no history | **FOX** (three-layer split: term matcher / query language / box behaviour) | FOX is the only one that keeps the outer language in one class (`searchq::Query`) AND puts the Esc/↓/history behaviour in one call. POE2's `parse/test` is the middle layer in miniature. ID-term rule is engine-specific in all three (`SNO` digits vs MurmurHash hex vs PathFileNameCode) → core must take an `isIdTerm` policy. |
| Funnel + facets + chips | inline per tab (`d4/src/tabs/BulkExtractorTab.h:79-83` "parity with Models") — **HUNCH:** no shared control | `util/TagFunnel.{h,cpp}` (button+popup+chips as one object; state lives in the search box as `#tag` terms), `util/TagFilterPopup.*`, `view/FilterChips.h` (flow layout), `view/FilterPopup.h` (host-reparenting popup for non-tag filters) | `index/FunnelFilter.{h,cpp}` (QToolButton popup + chip bar + counts + Match-any), `index/Facets.h` (path-derived facets + self-test) | **Two designs, not one superset.** FOX = query-string-as-state (`fox/src/util/TagFunnel.h:12-15`); POE2 = separate facet ids persisted per tab (`poe2/src/index/FunnelFilter.h:14-17`). | Core should ship the *widgets* (funnel button tinting, sticky popup with grouped checkboxes, chip flow layout, Match-any) with an abstract "facet vocabulary" and let the app choose whether ticks rewrite the search text. FOX's chip flow layout (`fox/src/view/FilterChips.h:32`) is the better chip widget; POE2's popup carries live counts (`poe2/src/index/FunnelFilter.h:30`). |
| Context-menu builders + MenuText | `util/ViewportPartMenu.h` — `MenuText` namespace (`:45-126`: kCopySno, kSaveImage… `parts(n)`, `verbParts`, `exportSetPrompt/Last`) + `ViewportPartMenu::{Info,Actions}` part-menu builder; `util/LookIcon.h` `addActions` (Copy/Save image pair) | `util/MenuText.h` — vocabulary + `exportLabel/exportLastLabel/exportSubject/plural/withValue/withRows/withCount/prompts` (`:16-115` of the tail); `util/MenuContext.h` — the §2 selection rule as a template (`contextSet`, `contextFiles`, `clickedFile`); `util/MenuDump.h` — menu census hook at every `exec()` | none (part menu inline in `poe2/src/tabs/ModelsTab.h:66`) | **FOX** for vocabulary + selection rule + dump; **D4** for the part-menu `Info/Actions` builder shape | FOX separates the three concerns into three headers with no game includes; D4 bundles vocabulary and builder in one 5-TU-wide header (`d4/src/util/TextReportDialog.h:20-22` complains about its width). Vocabulary constants are engine-specific (`kCopySno` vs `kCopyHash`, `fox/src/util/MenuText.h:50-56`) → core carries the formatting helpers + rule, app supplies the nouns. |
| CSV / table copy | `util/CsvCopy.{h,cpp}` — `install(QAbstractItemView*)`, RFC-4180 CSV with header | `util/TableCopy.h` — `QTreeWidget` only, TAB-separated, indented children, `install/addMenuActions/installWithMenu`, menudump-aware | identical to D4 | **Merge:** D4's view-generic `install` + FOX's `addMenuActions`/`installWithMenu` | Neither is a superset: D4 handles any model/view (list, table, tree) but only Ctrl+C; FOX adds the two menu entries and the census hook but only for `QTreeWidget`. D4's `verify-src.py:148-190` has a check specifically for `CsvCopy::install` ordering vs `setContextMenuPolicy`, which FOX's `installWithMenu` doc mirrors (`fox/src/util/TableCopy.h:99-103`). |
| Hover previews | `util/HoverInfo.h` — settings read-through only (`hover/delaySec`, `imagePreview`, `scrollZoom`, `previewPx`, per-line `on(key)`, colour palette); popups built per tab | `util/HoverPreview.{h,cpp}` — `hover::Preview` singleton with dwell timer, lazy `Content` builder, wheel resize (remembered), screen clamping, `refresh()`; gated by `Config::hoverPreviews()` (`fox/src/util/HoverPreview.cpp:46,64`) | `util/HoverPreview.{h,cpp}` — `HoverPreview` QFrame card (`showFor/stepSize/hidePreview`), one per tab, no settings gate, no dwell timer (tab arms it) | **FOX** widget + **D4** settings surface | FOX has the complete behaviour; D4 has the complete *settings* (delay, size, per-line toggles) which FOX hard-codes (`fox/src/util/HoverPreview.h:30-35`). Core = FOX `hover::Preview` reading D4 `HoverInfo` keys. |
| Thumbnail caches | `index/SnoListModel::iconData` + `QPixmapCache` (`d4/src/index/SnoListModel.h:43-47,84,115`) — model-embedded | `gl/ThumbnailRenderer` (GL, disk `data/cache/thumbs_v<N>/`), `index/TexThumbCache` | `util/ThumbnailCache.{h,cpp}` — generic `std::function<QImage(quint32)>` render, background pool or GUI-tick mode, `QCache` budget, `ready(fileIndex)` signal; no game includes | **POE2** (in-memory tier) + **FOX** disk tier (**HUNCH** — ThumbnailRenderer out of scope) | POE2's is the only one with zero coupling (`poe2/src/util/ThumbnailCache.cpp:1-7` Qt-only includes). |
| Text report dialog | `util/TextReportDialog.h` `TextReport::show(parent,title,text)` | `MgsvMetaDialog::showReport` static (`fox/src/app/MgsvMetaDialog.h:42`) — lives inside a game-specific dialog | identical to D4; used for Health check (`poe2/src/app/MainWindow.cpp:220`) | **D4/POE2** | FOX's is misplaced (a generic pane owned by the SnakeBite metadata form). |
| First-run page | `QMessageBox::question` once per session in `finishReload` (`d4/src/app/MainWindow.cpp:1463-1481`) + amber-border cue on required path fields (`SettingsDialog.cpp:2588-2597`) | `app/FirstRunPage.{h,cpp}` — a real page in a `QStackedWidget` in front of the tabs (`fox/src/app/MainWindow.cpp:177-188,443-450`), add/remove folders, Start | status-bar text only (`poe2/src/app/MainWindow.cpp:154-155`) + `QMessageBox::warning` when index cannot start (`:179-182`) | **FOX** | FOX's is the only non-modal, harness-photographable one (`fox/src/app/FirstRunPage.h:3-13`). Its coupling is `Config::gameDirs()` (`FirstRunPage.cpp:78-117`) → core version needs a folder-list provider. |
| Crash handler / SEH | `app/SehGuard.*` (translator + `runGuarded`); installed `d4/src/main.cpp:158`; used in 6 tabs; plus a **crash-recovery breadcrumb** (`wardrobe2/_loading`) that clears the remembered outfit (`MainWindow.cpp:129-167`) | `app/SehGuard.*` (identical .cpp; .h differs by one typo `fox/src/app/SehGuard.h:8`); installed `fox/src/main.cpp:77`; **`app/CrashHandler.{h,cpp}`** — writes `data\FOXAssetBrowser-crash.txt` with build, fault, last 20 log lines, stack (Win SEH filter + invalid-param + pure-call; Linux signals + `backtrace`), `--crashtest` | `app/SehGuard.*` identical, compiled (`poe2/CMakeLists.txt:50`) but **never installed or called** | **FOX** (SehGuard + CrashHandler) plus D4's per-tab crash-loop breadcrumb as a pattern | `crashguard::noteLogLine` is fed from AppLog (`fox/src/app/AppLog.cpp:47`) — the two ship together. |
| Startup self-tests | `QueryTerm::selfTest()` only (`d4/src/main.cpp:232-233`) | `bc::selfTest`, `bc::encodeSelfTest`, `QueryTerm`, `searchq`, `animpose` (`fox/src/main.cpp:121-144`) | 10 tests in a table (`poe2/src/main.cpp:56-61`) | **POE2's table shape**, FOX's log-not-stderr reporting | POE2 prints to stderr (`:61`) which on a Windows GUI build goes nowhere; FOX's `qWarning` reaches the log (`fox/src/main.cpp:111-117` explains the same trap for prune). Core = a `SelfTests::run({...})` that logs. |
| Cache maintenance / pruning | `AppPaths::pruneOldCaches(stem,ver,ext)` regex prune (`d4/src/app/AppPaths.h:41-54`); **hand-copied version numbers** in `main.cpp:239-256`; `util/CacheVersioning.h` is a doc; Settings clears by glob (`SettingsDialog.cpp:2133-2153`); fingerprint guard wipes magic-only caches (`MainWindow.cpp:1502-1551`) | `app/CacheMaint.h` — ONE table `defs()` with label/subdir/glob/tooltip, `bytes/count/newest/totalBytes/report/clear/find`; caches in `data/cache/` (`AppPaths.h:30-53`) with `migrateCaches()`; prune reads the owners' constants (`fox/src/index/ArchiveIndex.cpp:120-134`) | `pruneOldCaches` identical mechanism with owners' constants (`poe2/src/main.cpp:50-52`); Maintenance lists `*.bin` dynamically with size + per-file Clear (`SettingsDialog.cpp:221-239`) | **FOX** `CacheMaint` table + `data/cache/` split; POE2's use of `kCacheVersion` constants | D4's prune numbers are a maintenance trap it documents itself (`d4/src/util/CacheVersioning.h:24-26`). FOX's table is coupled to `gl/ThumbnailRenderer.h` for `kThumbCacheVersion` (`CacheMaint.h:18,65`) — core takes the table type + operations, app registers defs. |
| Font scale / density | none (a fixed dark stylesheet + PNG checkmark, `d4/src/main.cpp:40-90`) | `app/Density.h` (3 stable keys, app-wide stylesheet), `app/FontScale.h` (percent 90–150, `QApplication::setFont`), both with session overrides; applied before show (`fox/src/main.cpp:726-727`); live in Settings with Cancel revert (`SettingsDialog.cpp:278-320,886-892`); `util/CheckStyle.h` `XCheckStyle` proxy + `util/RowShading.h` from `polish()` | none | **FOX** | Zero coupling (Qt + QSettings). |
| Startup profiling | `qInfo("startup: …")` lines only (`d4/src/app/MainWindow.cpp:118,1423,1571`) | `app/StartupProfile.h` — one clock, `mark/add`, `text()` report, rolling 5-launch history in `data/cache/startup_history.tsv`, Help ▸ Startup profile (`MainWindow.cpp:1101-1107`), `--startupprofile` | none | **FOX** | Zero coupling. |
| Status line (per-tab) | `MainWindow::setStatus` + tabs' own `scanStatus` signals (`d4/src/app/MainWindow.cpp:602-607`) | `app/StatusLine.h` singleton with source-widget filtering (`fox/src/app/MainWindow.cpp:200-218,1434-1442`) | tabs emit `status` signals connected directly (`poe2/src/app/MainWindow.cpp:67-70`) | **FOX** | Same singleton pattern as ExportNotifier; the source-filter is what stops a stale message surviving a tab switch. |
| Index roster / File ▸ Index menu | `IndexDesc{name,what,ready,building,start,reset,hasPct}` roster (`d4/src/app/MainWindow.h:96-118`, `.cpp:1670-1762`) drives submenu + status-bar indicator + floating toast | hand-built rows in `populateIndexMenu` (`fox/src/app/MainWindow.cpp:1182-1252`) | none (single `IndexLoader`) | **D4** roster struct | D4's roster is the generic shape §6 of CONTEXT_MENUS asks for ("one row per index, relabelled live"); FOX re-derives three rows inline. Coupling: the lambdas capture game singletons, but the struct is engine-free. |
| Jump palette / nav history | `Ctrl+K` palette + `Alt+Left/Right` history (`d4/src/app/MainWindow.cpp:3313-3471`) | none | none | D4 (generic shell feature, **HUNCH**: worth core once a `jumpTo(kind,id)` hook exists) | Scans `m_index.entries(9/44)` directly (`:3440-3441`). |
| BrowserTab base / export hooks | `tabs/BrowserTab.h` — virtual export hooks exactly as CONTEXT_MENUS §6 lists (`:65-95`), plus `refresh/reset/onSettingsChanged/onSettingsLiveChanged/persistView`; `LazyTab` proxy (`MainWindow.cpp:525-584`) | no base class; each tab has `populateExportMenu(QMenu*)` (`fox/src/tabs/*.h`) and the window switches on concrete pointers (`MainWindow.cpp:1459-1465`) | no base class; menu invokes `ModelsTab` slots by name (`poe2/src/app/MainWindow.cpp:113-121`) | **D4** for the hook interface; **FOX** `populateExportMenu(QMenu*)` + role stamping for tabs that build their own menus | D4 header couples to `CascReader`/`SnoIndex`/`GLModelWidget` (`d4/src/tabs/BrowserTab.h:5-7,53-54,67`). |

### 1.2 Notes on the identical files the mechanical pass found

- `GifEncoder.h/.cpp`, `CsvCopy.*`, `NameTemplate.h`, `PanelPersist.h`, `TextReportDialog.h`, `SehGuard.*`, `AppLog.h`, `Hotkeys.h`, `LogConsole.*` are copies in POE2; of those, **`AppLog.h` has no implementation and `LogConsole`, `SehGuard`, `PanelPersist` have no callers in POE2** (see table). POE2's shell is smaller than its file list suggests.
- FOX's `SehGuard.cpp` is byte-identical to D4's; the header differs only by a comment typo (`fox/src/app/SehGuard.h:8` "CMakeLists.txt.." vs `d4/src/app/SehGuard.h:8`).

---

## 2. MainWindow — what each owns, what is shell vs game

### 2.1 D4 (`d4/src/app/MainWindow.{h,cpp}`, 204 + 3739 lines)

| Responsibility | Where | Shell or game? |
|---|---|---|
| Owns `CascReader` + `SnoIndex` (`MainWindow.h:157-158`) | ctor `:109` | game |
| Menus: File (Settings Ctrl+, · Reload F5 · Index ▸ · Icon audit · Toggle console Ctrl+` · Exit), E&xport, Help | `buildMenu` `:292-448` | shell shape (matches CONTEXT_MENUS §6 exactly) with game rows: "Icon audit" `:308`, Help ▸ Patch contents `:359`, Find SNO `:363`, Diagnostic output submenu `:366-376`, Audit bulk presets `:383`, About text `:428-447` |
| Export menu: 12 actions, relabelled on `aboutToShow` from `BrowserTab` hooks | `:316-345`, `updateExportMenu` `:698-739` | shell (TexFrame/Catalogue rows are tab-specific but hidden via hooks) |
| Capture (image / turntable / anim-loop) with cloth-physics warning and RAII `ClothOff` | `:785-944` | shell mechanism; `ClothOff`/`confirmGifPhysics` are game (`GLModelWidget::clothEnabled` `:838`) |
| Tab list: **hardcoded** — Textures, Models eager; Wardrobe, Stable, Catalogue, Bulk Extract lazy via `LazyTab` | `buildTabs` `:586-660` (`:599-600,617-649`) | game list; `LazyTab` proxy (`:525-584`) is shell |
| Ctrl+1..9, Ctrl+K palette, Alt+Left/Right | `buildShortcuts` `:662-677`, `:3313-3471` | shell (palette scans groups 9/44 `:3440-3441` = game) |
| Status bar: status label (Ignored size policy), index indicator, staleness warning | `:178-188`, `buildIndexIndicator` `:3542+` | shell |
| Export toast + tray notification (`export/osNotify`) | `showExportToast` `:3476-3530` | shell |
| Floating indexing toast aggregating roster + per-tab scan messages | `:166-176`, `updateToast` `:3698` | shell |
| Index roster / File ▸ Index submenu / Index all / Re-index | `:1670-1938` | shell struct, game lambdas |
| Async reload → `finishReload` (fingerprint guard, first-run prompt, prewarm, index-all-on-startup) | `:1072-1624` | game (CASC/TACT/d4data), with shell fragments: first-run prompt `:1463-1481`, prewarm `:1597-1617`, `ui/indexAllOnStartup` `:1593` |
| Crash-recovery breadcrumb (clears `wardrobe*/…` keys) | `:129-167` | game keys, shell idea |
| Update check (throttled, notify-once per id) | `:205-250` | game (`deps/UpdateCheck`) |
| Health check (grid dialog, 12+ rows) | `showHealthCheck` `:3155+` | game content; `addRow` helper `:3163-3178` is shell |
| Help ▸ Shortcuts (F1 + Shift+/), modeless, **static HTML** not from the registry | `:451-504` | shell, but violates §29 "one registry" — sheet text is hand-written (`:465-497`) |
| Copy log / Export log / Copy diagnostic info / Open data folder / Open log folder | `:386-426` | shell (diag block cites CASC `:414-423`) |
| `closeEvent`: geometry/state, `view/lastTab`, `persistView()` on every tab | `:255-277` | shell |
| Settings dialog wiring: `settingsChanged` → every tab `onSettingsChanged` + `applyHotkeys`; `wardrobeLiveChanged` | `openExportSettings` `:950-966` | shell (signal name is game-flavoured) |
| Env-gated probes `D4_DUMP_PRD/MSH`, `D4_VERIFY_KEYS` | `:1199`, `:2600-3154` | game |

### 2.2 FOX (`fox/src/app/MainWindow.{h,cpp}`, 616 + 6765 lines)

| Responsibility | Where | Shell or game? |
|---|---|---|
| Tabs: **hardcoded** Files, Textures, Models, Customize, Bulk Extract, all eager | ctor `fox/src/app/MainWindow.cpp:120-176` | game list |
| `QStackedWidget` first-run page (0) / tabs (1) | `:180-188`, `:443-450` | shell |
| `session/tab` saved by **title** on change; `restoreSession()` by title + model path | `:189-195`, `:453-473` | shell (model restore is game) |
| Status bar: status label, per-tab `StatusLine` label with source filter, mod chip, revert button, Log chip, Show-in-folder button | `:197-305` | shell except mod chip/revert (`:236-264` = game) |
| Export report via `ExportNotifier` + `runAfter` | `:287-297` | shell |
| `ArchiveIndex::readyChanged` → catalogue builds, status counts, `TextureUsers`/`RefIndex` reset, startup-profile mark + history, session restore, dev-shot | `:307-429` | game body; profile/session/devshot fragments are shell |
| Menus: File (Set game folder · Settings [key from registry] · Index ▸ · Toggle console Ctrl+L · Exit), Export (rebuilt per tab), Help (no mnemonic: Handbook · Shortcuts F1/? · Health check · Startup profile · Copy log · Export log · Copy diagnostic · Open data folder · Open log file · About) | `buildMenus` `:602-737` | shell shape; Alt+H rationale `:652-659` |
| Export menu: delegates to `<tab>->populateExportMenu(m_exportMenu)` by concrete pointer, then Bulk presets submenu, mod-folder packaging (omitted when no mod dir), "Export settings…" last | `populateExportMenu` `:1444-1536` | delegation is shell; bulk presets/mod packaging game |
| `applyHotkeys()` — deletes/recreates QActions per registry row; viewport rows re-installed on each `GLModelWidget`/`AnimTransport`; `triggerExportAction(role)` builds a throwaway menu and finds by role | `:1290-1428` | shell |
| `focusCurrentSearch()` by `objectName` | `:1373-1387` | shell |
| Index submenu: "Index all"/"Re-index everything…" guarded on `haveGame`; three hand-written rows | `:1182-1252` | shell shape, game rows |
| Health check → `writeHealthAudit` TSV in `data/audit/`, opens folder | `:1062-1078`, `:857-1021` | game |
| Handbook (HTML from `handbook::html()`), Startup profile | `:1080-1107` | shell |
| `restoreWindowLayout()` / `closeEvent` behind `Config::rememberLayout()` (same key as panels) | `:1672-1693` | shell |
| `startRebuild()` refuses while bulk extraction runs | `:1649-1670` | game |
| `DevShot` harness — ~250 fields (`MainWindow.h:31-486`) and `takeDevShot()` (`:1726-6765`, ~5000 lines) | | dev tooling; the *mechanism* (schedule → ready → settle → grab → quit, `m_harness` suppressing layout writes `:502,1674,1690`) is shell; every flag is game |
| Mod folder dialog / preview capture+restore around rescans / packaging | `:475-600`, `:1538-1637` | game |

### 2.3 POE2 (`poe2/src/app/MainWindow.{h,cpp}`, 44 + 285 lines)

| Responsibility | Where | Shell or game? |
|---|---|---|
| Owns `AssetStore` + `IndexLoader` | `poe2/src/app/MainWindow.cpp:37-51` | game |
| Tabs: **hardcoded** Textures, Models, Customize, Bulk (eager, concrete types) | `:53-63` | game list |
| Status label + `status` signals from each tab; `ExportNotifier` → status text + permanent "Show in folder" button | `:65-89` | shell |
| File menu: Set folder · Reload index · Health check… · Settings… · Quit (no shortcuts, no Index submenu, no console) | `:91-102` | partial §6 |
| Export menu: **static** labels with the bound key appended; invokes `ModelsTab` slots by name via `QMetaObject::invokeMethod` | `:104-121` | shell idea, but no per-tab relabel/enable |
| Help: "Controls & shortcuts (F1)" → HTML cheat sheet reading two registry rows | `:123-129`, `:223-285` | shell |
| Hotkeys: created **once** in ctor, gated on `currentWidget()==m_models`; no re-apply after Settings (dialog says "Some shortcuts apply after the next launch" `poe2/src/app/SettingsDialog.cpp:190`) | `:131-151` | shell, incomplete |
| Health check → `TextReport::show` | `:203-221` | game content |
| No geometry persistence, no remembered tab, no console, no log, no update check | — | gaps vs §16/§18 |

### 2.4 Where the tab list comes from (all three: hardcoded)

- D4: `d4/src/app/MainWindow.cpp:599-649` (six `add(...)` calls; two eager `new TexturesTab`/`new ModelsTab`, four `LazyTab` factories capturing `models`/`textures`).
- FOX: `fox/src/app/MainWindow.cpp:121-125` (five `new` calls) and `:126-176` (`addTab` in a different order from construction, with a comment justifying placement).
- POE2: `poe2/src/app/MainWindow.cpp:54-62`.

No project has a tab registry; every cross-tab wire (`revealModelRequested`, `openModelRequested`, `gameFilterChanged`) is a direct `connect` between concrete types (`d4:602-656`, `fox:137-173`, `poe2:41-50`).

### 2.5 Sketch: what a core `MainWindow` needs as plugin hooks

Derived from what is duplicated above and what differs only by game:

```cpp
// core/AppShell.h — the window owns none of the game; the game plugs into these.
struct TabSpec {
    QString title;                 // tab bar label (FOX persists this string: fox MainWindow.cpp:194)
    QString stableId;              // "textures" — for session/tab and Ctrl+N (never the index)
    bool lazy;                     // D4 LazyTab (d4 MainWindow.cpp:525-584)
    std::function<BrowserTab*(QWidget* parent)> make;
};
struct IndexSpec { /* == D4 IndexDesc, d4 MainWindow.h:96-104 */ };
struct HelpEntry { QString label; QString tooltip; std::function<void()> run; };

class AppPlugin {
public:
    virtual QString productName() const = 0;                   // window title, About, log file stem
    virtual QVector<TabSpec> tabs() = 0;                        // replaces the three hardcoded lists
    virtual QVector<IndexSpec> indexRoster() = 0;               // File ▸ Index submenu + indicator (D4 shape)
    virtual void reload(std::function<void(bool ok, QString status)> done) = 0;  // D4 async reload / FOX startRebuild / POE2 startIndex
    virtual QVector<HelpEntry> helpEntries() = 0;               // Patch contents, Find SNO, Handbook, Startup profile…
    virtual QString diagnosticInfo() = 0;                       // the game half of "Copy diagnostic info"
    virtual QWidget* firstRunPage(QWidget* parent) { return nullptr; }   // FOX FirstRunPage or a core default
    virtual bool isConfigured() = 0;                            // Config::gameDir()/gameDirs() non-empty
    virtual QVector<SettingsPage> settingsPages() = 0;          // §3 below
    virtual void wireCrossTab(QHash<QString, BrowserTab*>) {}   // revealModel/openModel connects
};
```

Core keeps: menu bar shape (§6), export menu driven by `BrowserTab` hooks **or** a per-tab `populateExportMenu(QMenu*)` with role stamps (FOX), `applyHotkeys()` by registry key, status bar (status label + `StatusLine` + export Show-in-folder), export toast/tray, log console toggle, Copy/Export log, Open data folder, geometry/tab persistence behind one key, `LazyTab`, index indicator + submenu, first-run stack, Ctrl+K palette given a `jumpTo(kind,id)` hook, `DevShot` scheduling skeleton (`scheduleDevShot`/settle/quit; the flag struct stays per app).

**HUNCH:** the `BrowserTab` interface should be D4's virtuals (`d4/src/tabs/BrowserTab.h:50-99`) minus the `CascReader*/SnoIndex*` setters (replace with `void setStore(QObject*)` or template the base), plus FOX's `populateExportMenu(QMenu*)` so a tab can add engine-only export rows without `MainWindow` growing a member per row (D4's `m_actExportFrames*`/`m_actExportAllFiltered*` at `MainWindow.h:198-203` are the cost of not having it).

---

## 3. SettingsDialog — page lists and the per-game split

### 3.1 Page / tab lists (cited)

| | D4 (`d4/src/app/SettingsDialog.cpp`) | FOX (`fox/src/app/SettingsDialog.cpp`) | POE2 (`poe2/src/app/SettingsDialog.cpp`) |
|---|---|---|---|
| Top-level order | General · Interface · Models · Wardrobe · Export · Hotkeys · Maintenance · Information · Experimental (`:147-161`) | General · Interface · Viewport · Export · Hotkeys · Maintenance · Information (`:225,391,518,535,619,742,880`) | General · Export · Hotkeys · Maintenance · Information (`:41-45`) |
| General | Directories `:429` · Game data `:496` · Updates `:521` · Settings profile `:801` | folders list + dictionary + mod folder `:75-174` · Settings profile `:175-224` | Directories (game + Bundles2 override) `:88-104` |
| Interface | Startup && layout `:550` · On-hover previews && info `:607` · Icon indicators `:716` | Startup && layout (remember panels/session/viewport, density, text size, show tips again) `:232-337` · On-hover previews `:341` · Diagnostics (note only) `:356` · Textures (alpha bg) `:376` | — |
| Per-area | Models: Browsing && loading `:947`; Wardrobe: Outfit && preview `:1020`, Weapons `:1059`, Performance `:1097` | Viewport: PBR ×3 + starting environment/exposure/panel `:394-518` | — |
| Export | sub-tabs Models · Images · "Wardrobe, Catalogue && Bulk" · File names `:1158-1161` (+ "All exports" box `:1993-2001`) | sub-tabs Models · Advanced · Images && GIFs · Files && names `:530-533` (pages from `fox::ExportPages`, `export/ExportOptions.h:309-345`) | sub-tabs Models · Image · Bulk `:147,160,175` |
| Hotkeys | one `QKeySequenceEdit` + ✕ per registry row, writes live, `settingsChanged` `:2269-2306` | edits + tooltips, "Put every shortcut back", Copy/Save cheat sheet, clash warning `:538-620` | edits, written on OK; empty → `remove(key)` `:181-194,315-320` |
| Maintenance | Caches && reset (glob clears, blocklist, Wardrobe/Stable memory, footprint) `:2127-2262` · Diagnostics (log file toggle) `:746-777` · Indexing (advanced) `:2311-2320` | Caches && reset from `cachemaint::defs()` with size/count/age + total + "Reset every setting…" (`s.clear()`) `:622-743` | dynamic `*.bin` list with per-file Clear + Clear all `:197-239` |
| Information | 5 `QTextBrowser` sub-tabs `:218,239,260,270,316` | 4 sub-tabs `:766,789,824,849` | one rich-text label `:242-259` |
| Experimental | retarget box `:2117` | absent by design (`SettingsDialog.h:24`) | — |
| Buttons | OK · Cancel · Restore Defaults (`:2340-2397`) | OK · Cancel (`:883-893`) | Restore export defaults · OK · Cancel (`:47-56`) |

### 3.2 Generic vs game-specific pages

Generic (identical intent in all three, differs only in copy): **General ▸ Settings profile** (D4 JSON export `:847+`, FOX INI copy `:191-215`), **Interface ▸ Startup && layout / On-hover / Diagnostics**, **Export ▸ Images (& GIFs)** and **File names**, **Hotkeys** (whole page derives from the registry in all three), **Maintenance ▸ Caches && reset** (the mechanism; the def list is per game), **Information** (the frame; the prose is per game).

Game-specific: D4 Directories (TACT keys, d4data download, CASC product, Updates `:429-546`); D4 Models/Wardrobe pages; FOX folders list + dictionary + mod folder + Viewport PBR page; POE2 Bundles2 override; every Export ▸ Models page (unit scale, axes, attachments) — the *options* are per engine even though the sub-tab is generic.

### 3.3 How a core dialog would accept per-game pages

FOX's shape is already the API: `addPage(title, QWidget*)` (`fox/src/app/SettingsDialog.cpp:929-939`) + `showTab("Export/Images")` by name (`:941-971`). Core would expose:

```cpp
struct SettingsPage {
    QString title;               // tab label (never elided)
    int order;                   // template §16 ordering: setup=0, presentation=1, per-area=2, disk=3, keys=4, upkeep=5, reference=6, experimental=7
    std::function<QWidget*(QWidget* parent)> build;
    std::function<void()> apply;         // on OK (FOX/POE2 style) — or nothing if the page writes live (D4 style)
    std::function<void()> revert;        // on Cancel, for live pages (FOX density/font :886-892; D4 snapshot :2552-2579)
    QStringList resetGroups;             // QSettings groups cleared by removal on "Restore defaults" (§3.10)
    QStringList keepPrefixes;            // user-authored subtrees a reset must skip (D4 ViewportSettings::keepPrefixes)
};
```

Core-owned pages: General ▸ Settings profile, Interface ▸ {Startup && layout, On-hover (D4 keys), Diagnostics (log file toggle)}, Export ▸ {Images && GIFs, File names}, Hotkeys, Maintenance ▸ Caches && reset (from a `cachemaint::defs()` the app registers), Information (frame). The app contributes Directories, per-area pages, Export ▸ Models, Information sub-tabs. The dialog itself keeps: scroll-per-page, no-elide, `showEvent` width fold + **screen clamp** (only D4 has it, `d4/src/app/SettingsDialog.cpp:2802-2828`), Cancel-revert, `&&` in titles, tooltips required.

---

## 4. Config / QSettings — namespacing and convention enforcement

### 4.1 Namespaces in use

| Project | Namespaces (cited) | Notes |
|---|---|---|
| D4 | `paths/`, `casc/` (`d4/src/app/Config.cpp:5-8`); `models/`, `wardrobe2/`, `stable2/`, `tex/`, `bulk/`, `export/`, `retarget/`, `hotkeys/`, `hover/`, `icons/<tab>/`, `view/`, `ui/`, `updates/`, `log/`, `index/`, `window/` (`SettingsDialog.cpp:2482-2534`; `ViewportSettings.h:70-83`; `HoverInfo.h`; `IconBadge.h:66,71`; `MainWindow.cpp:124-125,173-174,210-214,1506,1593-1598`; `main.cpp:139,273`) | Per-tab namespaces as §3.7 asks, with three spellings of the same render keys documented at `ViewportSettings.h:9-23`. |
| FOX | `paths/`, `index/`, `render/` (`fox/src/app/Config.cpp:11-18`); `view/`, `session/`, `interface/`, `textures/` (`:65-205`); `hotkeys/` (`Hotkeys.h`); `tips/` (`TipBar.h:24`); `ui/density`, `ui/fontScale` (`Density.h:63`, `FontScale.h:65`); `export/after*` (`ExportNotifier.cpp:14-15`); `export/presets/` (`export/ExportOptions.cpp:367`); `<tab>/splitter`, `<tab>/npanel/{open,order,stack}` (`NPanel.h:17-27`); `window/geometry` (`MainWindow.cpp:1676,1691`) | One key shared deliberately: `interface/rememberPanels` gates panels, splitters AND window geometry (`Config.h:83-87`, `MainWindow.cpp:1686-1691`). |
| POE2 | `paths/`, `export/`, `image/` (`poe2/src/app/Config.cpp:7-26`); `hotkeys/`; `facets/models`, `facets/bulk` (per `poe2/CLAUDE.md`); `view/rememberPanels` read by a dead header | Smallest surface; no `window/`, `session/`, `interface/`. |

### 4.2 Convention enforcement

| Convention (§3) | D4 | FOX | POE2 |
|---|---|---|---|
| 3.1 one setting, one key | `Config.cpp` accessor pairs; `verify-src.py:695-747` **dead-key check** (written-never-read) with a reviewed baseline; the one shared key `view/rememberPanels` read live by `PanelPersist.h:16` and written by Settings `:596` | Accessor pairs with the rule stated at `Config.cpp:8-9`; `PbrView` enum so three viewports cannot drift onto two keys (`Config.h:36-40`); `interface/rememberPanels` shared on purpose; **no verify-src check** | Accessor pairs (`Config.h:5`); `verify-src.py:115-127` dead-key check (notes, non-failing) |
| 3.2 persist stable id, restore with `findData` | `verify-src.py:748-779` **text-persisted-combo check**; `ExportLayout.h` worked example | `Density.h:15` and `FontScale.h:13-16` store keys/percent not index; `NPanel.h:18-21` stable panel keys; `session/tab` stores the tab *title* (`MainWindow.cpp:191-194`) — a label, not an id (**HUNCH:** a renamed tab silently drops the restore; §3.2 would want a stable id); no verify-src check | `SettingsDialog.cpp:169-172,285` `findData` for layout; `Facets.h:31` "stable id … never a display index"; no verify-src check |
| 3.7 namespaces per tab | yes (`ViewportSettings.h`) | yes (`PanelPersist.h:81-83`) | partial |
| 3.10 reset by removal | **Mixed.** `ViewportSettings::resetGroup/resetAll` remove keys with keep-prefixes (`ViewportSettings.h:100-129`) and are called from Restore Defaults (`SettingsDialog.cpp:2385`); but the same handler **writes defaults** for view/models/wardrobe/perf toggles (`:2356-2370`) and runs `m_exportResetActions` which set widgets that live-write keys (`:2372`, e.g. `:1177,2031,2284-2286`) — export/hotkeys are reset by writing, contrary to §3.10. "Reset all to defaults…" uses `s.clear()` keeping `paths/`+`casc/` (`:829-836`). Stable memory cleared as a subtree (`:2204-2209`). | "Reset every setting…" = `s.clear()` (`SettingsDialog.cpp:725-727`) then `reject()` so OK cannot rewrite (`:731-736`); Hotkeys "Put every shortcut back" sets widgets to defaults then OK writes them (`:561-571,1011-1014`) — by writing; **no scoped Restore Defaults for Export or Viewport** (`:883-885` has only OK/Cancel). `export/presets/<name>` removed on delete (`ExportOptions.cpp:367`). | **Cleanest:** `restoreExportDefaults()` removes the `export/` and `image/` groups then `load()` (`SettingsDialog.cpp:323-336`); hotkey unbind = `remove(key)` (`:319`). |
| Dead-key / write-only detection | script (fail) | none | script (note) |

**Net:** POE2 is the reference for §3.10 mechanics; D4 for the *scoping* rule (per-viewport groups + keep-prefixes) and for tooling; FOX for accessor discipline and the session-override pattern. None enforces §3.2 mechanically except D4's script.

---

## 5. main.cpp — startup sequences

| Step | D4 (`d4/src/main.cpp`) | FOX (`fox/src/main.cpp`) | POE2 (`poe2/src/main.cpp`) | Generic? |
|---|---|---|---|---|
| Startup clock | — (`qInfo` timings inside MainWindow) | `StartupProfile::begin()` first line `:43` | — | yes (FOX) |
| GL default format | 4.5 core, 4× MSAA, stencil 8 `:162-170`; `AA_ShareOpenGLContexts` `:175` | depth 24, stencil 8, **alpha 8** on the default format `:56-74` | 3.3 core, depth 24, 4× MSAA `:32-38` | version/profile per app; stencil+alpha needs are shell (selection outline, transparent capture) |
| SEH translator | `:158` (before QApplication) | `:77` | **absent** | yes |
| QApplication + org/app/version | `:177-181` | `:79-82` (`FOXAB_VERSION` macro) | `:40-43` | yes |
| Style | `installCheckmarkStyle` stylesheet + PNG tick in `data/` `:40-90,278` | `QApplication::setStyle(new fox::XCheckStyle)` `:91`; `fontscale::apply(); density::apply()` `:726-727` | none | yes (FOX's proxy style; D4's dark palette is a theme choice) |
| QSettings → `data/<App>.ini` | `:186-187` | `:95-96` | `:46-47` | yes (identical three lines) |
| Settings migration | old INI copy `:195-205`; renamed keys table `:214-227` | — | — | mechanism yes; table per app |
| Log install | `qInstallMessageHandler` + gated file `:272-273` | `AppLog::install()` `:98` then `crashguard::install()` `:103` | **none** | yes |
| Cache prune | 14 hand-numbered `pruneOldCaches` calls `:239-256` | `fox::pruneOldCaches()` (owners' constants + migrate + thumb dirs) `:116-117` — after log so the line reaches the log (`:111-115`) | 3 calls with owners' `kCacheVersion` `:50-52` | yes given a registry |
| Self-tests | QueryTerm `:232-233` | 5 tests `:121-144`, mark `:145` | 10 tests to stderr `:56-61` | yes |
| Window icon | `:288-292` (with warning if missing) | `:147-150` | — | yes |
| CLI | none | `QCommandLineParser` with ~200 options, unknown-option = exit 2 (`:634-639`), session overrides for game/dict/mod/export dirs/density/fontscale/games (`:644-721`), `DevShot` build (`:730-1237`) | none | the *parser + unknown-flag rule + session-override pattern* are shell; every flag is app |
| Window | `MainWindow w; show; exec` `:294-296` | `restoreWindowLayout()` before show `:1238`, `shutdown()` of detached sweeps after exec `:1245-1246` | `:63-65` | yes |

Generic lines a core `main()` could own verbatim: SEH install, org/app/version, INI path, log install + crash handler, cache prune (from the app's registry), self-test runner, app icon, style, `restoreWindowLayout`, `exec` + shutdown hooks. App-supplied: GL version, migrations table, cache registry, self-test list, CLI flag set.

---

## 6. Coupling list — game includes/types touched by each core candidate

| Candidate (best version) | Game-specific include / type | Where |
|---|---|---|
| `fox/src/app/Hotkeys.h` | none (Qt only) | — |
| `fox/src/app/AppLog.{h,cpp}` | `app/CrashHandler.h` (`crashguard::noteLogLine`) | `fox/src/app/AppLog.cpp:11,47` |
| `fox/src/app/CrashHandler.cpp` | `app/AppPaths.h` (note path), product name in file name | `:15`, `CrashHandler.h:10` |
| `fox/src/app/LogConsole.cpp` | `app/AppLog.h` only | `:21,106,130,136,239` |
| `d4/src/app/AppLog.h` toggle | definitions in `main.cpp` reference `AppPaths::file("D4AssetBrowser.log")` | `d4/src/main.cpp:128,143` |
| `fox/src/app/SettingsDialog.cpp` skeleton | `Config` (game dirs, dict, mod dir, PBR, view env), `fox::ViewEnvironment::presets()` `:461-482`, `fox::ExportPages` (`export/ExportOptions.h`), `cachemaint::defs()` → `gl/ThumbnailRenderer.h`, `fox::TipBar::resetAll()` `:331` | pages are game; `addPage/showTab/showEvent/checkHotkeyClashes` are clean |
| `d4/src/app/SettingsDialog.cpp` Cancel-snapshot | `liveSettingKeys()` is a hard-coded D4 key list `:2482-2534`; `isDerivedLiveKey` prefixes `:2453` | mechanism generic if the lists are injected |
| `d4/src/app/ViewportSettings.h` | D4 group prefixes `models/wardrobe2/stable2` `:70-83` | `Group`/`resetGroup`/`keepPrefixes` shape is generic |
| `fox/src/app/Config.{h,cpp}` | entire content is game keys; the *pattern* (named constants, session overrides `Config.cpp:30-38`) is what core takes | — |
| `poe2/src/app/ExportNotifier.h` | none | — |
| `fox/src/app/ExportNotifier.h` | `export/ExportOptions.h` (`fox::ExportOptions` for `glbOptionsLine`) | `:19,69` |
| `d4/src/app/ExportNotifier.h` | `model/ModelExporter.h` | `:6,35` |
| `fox/src/view/TipBar.{h,cpp}` | `view/ViewGlyphs.h` (painted ✕) | `TipBar.cpp:3` |
| `fox/src/view/PanelBox.h` | `view/ViewGlyphs.h` (`foxglyph::toolIconTinted`, kinds 39/41/42) | `:27,80,94-96` |
| `d4/src/tabs/PanelBox.h` | `BrowserTab.h` (for `kHdrQss`), which itself forward-declares `CascReader`/`SnoIndex`/`GLModelWidget` | `PanelBox.h:28`; `BrowserTab.h:5-7` |
| `fox/src/view/NPanel.{h,cpp}` | `view/PanelBox.h`, `util/PanelPersist.h` (`NPanel.cpp:74`); glyph kinds are ints from `ViewGlyphs.h` | `NPanel.h:58` |
| `fox/src/util/PanelPersist.h` | key `interface/rememberPanels` literal | `:96` |
| `fox/src/util/QueryTerm.h` | none; but `isIdTerm` thresholds encode Fox hash semantics | `:42-78` |
| `fox/src/util/SearchQuery.h` | `util/QueryTerm.h`; tag semantics deferred to `ModelTags.h` (comment `:19-23`) | `:38` |
| `fox/src/util/SearchBox.h` | none; `objectName("foxabSearchBox")` literal shared with `MainWindow::focusCurrentSearch` | `:118`; `MainWindow.cpp:1381` |
| `fox/src/util/TagFunnel.*`, `TagFilterPopup.*` | `util/SearchQuery.h`; vocabulary from `index/ModelTags` (**HUNCH** via .cpp, not read) | `TagFilterPopup.h:22,41` |
| `fox/src/view/FilterChips.h`, `FilterPopup.h` | none | — |
| `poe2/src/index/FunnelFilter.{h,cpp}` | `index/Facets.h` → `store/MaterialFamilyIndex.h` | `FunnelFilter.cpp:2`; `Facets.h:2` |
| `fox/src/util/MenuText.h` | none (vocabulary is Fox-worded: `kCopyHash`, `.gani`) | `:56,81-82` |
| `d4/src/util/ViewportPartMenu.h` | none by include; vocabulary D4-worded (`kCopySno`, `kCopyCollection`) | `:47-50` |
| `fox/src/util/MenuContext.h` | none | — |
| `fox/src/util/MenuDump.h` | none | — |
| `d4/src/util/LookIcon.h` | `casc/CascReader.h`, `index/AppearanceMeta.h`, `index/IconIndex.h`, `util/ViewportPartMenu.h` | `:13-18` — **not** a core candidate; only its `addActions` (Copy/Save image, disabled-not-hidden) is |
| `d4/src/util/CsvCopy.{h,cpp}` | none | — |
| `fox/src/util/TableCopy.h` | `util/MenuDump.h` | `:15,117` |
| `fox/src/util/HoverPreview.{h,cpp}` | `app/Config.h` (`Config::hoverPreviews()`) | `HoverPreview.cpp:4,46,64` |
| `d4/src/util/HoverInfo.h` | none; palette colours are D4 theme | `:30-39` |
| `poe2/src/util/HoverPreview.*`, `ThumbnailCache.*` | none | — |
| `d4/src/util/TextReportDialog.h` (= poe2) | none | — |
| `fox/src/app/FirstRunPage.{h,cpp}` | `app/Config.h` (`gameDirs/setGameDirs`) | `FirstRunPage.cpp:3,78-117` |
| `fox/src/app/CacheMaint.h` | `app/AppPaths.h`, `gl/ThumbnailRenderer.h` (`fox::kThumbCacheVersion`) | `:17-18,65` |
| `fox/src/app/AppPaths.h` | none; `dictDir()` is Fox-only | `:81-88` |
| `d4/src/app/AppPaths.h` | none | — |
| `fox/src/app/Density.h`, `FontScale.h`, `StartupProfile.h`, `StatusLine.h` | none | — |
| `fox/src/util/CheckStyle.{h,cpp}`, `RowShading.*`, `NaturalOrder.h` | none | — |
| `fox/src/app/Handbook.h` | `app/Hotkeys.h`; prose is Fox | `:19,106` |
| `d4/src/tabs/BrowserTab.h` | `CascReader`, `SnoIndex`, `GLModelWidget` forward decls + setters | `:5-7,53-54,67,97-98` |
| `d4/src/app/MainWindow.cpp` `LazyTab` | `BrowserTab` (and through it the three game types) | `:526-583` |
| `d4/src/app/MainWindow.h` `IndexDesc` | none in the struct; roster lambdas capture `AppearanceMeta`, `IconIndex`, … | `.h:96-104`; `.cpp:1677-1759` |
| `d4/src/tabs/IconBadge.h` | none (settings keys `icons/<tab>/…`) | `:66,71` |
| `d4/src/tabs/ViewGlyphs.h` | `ModelOutliner.h` (d4 tabs) for `kindIcon` | `:13` |
| `fox/src/view/ViewGlyphs.h` | none | — |
| `d4/src/util/DyeColorWheel.h` | none | — |
| `d4/src/util/ProcQuiet.h` | none (Win32 `CREATE_NO_WINDOW`) | — |
| `d4/src/app/ExportCapture.h` | `GLModelWidget` | `:136,155-162` |
| `fox/src/util/SearchableCombo.h`, `poe2/src/app/SearchableCombo.h` | none (two independent implementations of one idea) | — |

---

## 7. verify-src.py — checks per project, superset

| Check | D4 (`d4/verify-src.py`) | FOX (`fox/verify-src.py`) | POE2 (`poe2/verify-src.py`) |
|---|---|---|---|
| Truncated / empty file (run first, short-circuits) | `check_truncation` `:513`, `:839-846` | `:422-443`, `:468-475` | zero-byte only `:53-56` |
| `{}()[]` balance after comment/string strip | `:122`, single-pass `strip_code` `:58` | `:110`, `:46` | `:58-65` (regex strip `:25-51`) |
| Header-only helper used without a real `#include` directive | `:131-146`; table of 8 namespaces `:42-53` | `:119`; table of **2** (`AppPaths`, `AppLog`) `:38-41` | `:67-86`; table of 5 (`QueryTerm`, `NameTemplate`, `PanelPersist`, `AppPaths`, `RigMath`) `:70-76` |
| printf-style format vs args | `check_format_args` `:273` | `:261` | stub — **no-op** (`:88-106`, "No-op body kept as the documented anchor") |
| Locals named `emit/signals/slots/foreach` | `:340` | `:334` | `:108-113` (regex on declarations) |
| `qPrintable` (mangles non-ASCII on Windows) | — | `check_qprintable` `:362-382` | — |
| Duplicate `auto x = [` lambdas in one function | `:367` | `:389` | — |
| Duplicate map keys in initializer | `check_duplicate_map_keys` `:436` | — | — |
| `CsvCopy::install` / `installCopyMenu` before `setContextMenuPolicy` | `check_ctx_menu_order` `:156-190` | `:144-178` (same code) | — |
| Direct d4data JSON reads vs baseline | `:537-662` | — (game) | — |
| Dead QSettings keys (written, never read) | `:695-747` (fail above baseline) | — | `:115-127` (note only) |
| Combos persisted by display text | `:748-779` | — | — |
| Classification by name substring | `:780-816` (character tokens, counted) | — | `:129-140` (generic `.contains/startsWith/endsWith("literal")` count, note) |
| `--quiet` | yes | yes | yes |
| Explicit file args | yes `:818-821` | yes | no |

**Superset:** D4 has 12 check functions; FOX drops 4 D4 checks (dup-map-keys, dead keys, text-persisted combos, name-substring) and adds `qPrintable`; POE2 has the two whole-tree hygiene checks in weaker form and a stubbed format check. A core script = D4's file + FOX's `check_qprintable` + POE2's generic name-substring counter, with the three tables (`HEADER_ONLY`, baseline files, character tokens) supplied per project. Note FOX's `HEADER_ONLY` is nearly empty although FOX has the most header-only helpers (`SearchBox`, `TableCopy`, `MenuText`, `MenuContext`, `PanelPersist`, `Density`, `FontScale`, `CacheMaint`…), so the §7 "anchored include" rule is effectively unchecked there.

---

## 8. Recommendations for the core extraction (app shell)

1. **Take FOX's `app/` as the shell base** for: `Hotkeys.h` (registry + roles + cheat sheet), `AppLog` + `CrashHandler` + `LogConsole`, `StartupProfile`, `StatusLine`, `Density`/`FontScale`, `CheckStyle`/`RowShading`, `FirstRunPage`, `CacheMaint` (as a template the app fills), `ExportNotifier` (drop `glbOptionsLine`), the Settings skeleton (`addPage`/`showTab`/clash check), `applyHotkeys()`/`triggerExportAction()`/`focusCurrentSearch()`, `restoreWindowLayout`/`closeEvent` behind one key, and `PanelBox`+`NPanel`+`PanelPersist`.
2. **Take D4 for:** `BrowserTab` export-hook interface and `LazyTab`; `IndexDesc` roster + index submenu/indicator/toast; export toast + tray; `ViewportSettings` reset-by-removal shape with keep-prefixes; Settings `showEvent` screen clamp and Cancel snapshot/derived-namespace revert; `AppLog::setFileLogging` live toggle; `HoverInfo` settings keys; `TextReportDialog`; `CsvCopy`'s view-generic install; `verify-src.py` as the base script; `ProcQuiet`; Ctrl+K palette + Alt-nav given a `jumpTo` hook.
3. **Take POE2 for:** the self-test table shape (but log via `qWarning`), `restoreExportDefaults()` as the model for scoped reset-by-removal, `ThumbnailCache`, the minimal `ExportNotifier`, and `Config`/`ExportConfig` "options-from-settings" one-liner pattern.
4. **Search:** core = FOX's three-layer split; the per-term `isIdTerm` policy and the `#tag` resolver become injectable (SNO digits / MurmurHash hex / PathFileNameCode).
5. **Funnel:** core ships the widgets (sticky popup with grouped boxes + counts + Match-any, chip flow layout, tinted button); each app decides whether ticks rewrite the query (FOX) or persist facet ids (POE2). Neither current class is reusable as-is (`Facets.h` pulls `MaterialFamilyIndex`; `TagFunnel` pulls `ModelTags`).
6. **Menus:** core = FOX `MenuText` formatting helpers + `MenuContext` rule + `MenuDump`; vocabulary constants stay per app (a `MenuText::Nouns` struct the app fills: id noun, collection noun or absent). D4's `ViewportPartMenu::{Info,Actions}` part-menu builder shape goes to the viewport core, not the shell.
7. **Known defects worth fixing during extraction:** D4's positional hotkey binding (`d4/src/app/MainWindow.cpp:509-517`) and hand-written F1 sheet (`:465-497`); D4's Restore Defaults writing export/hotkey defaults (`SettingsDialog.cpp:2356-2372`) against §3.10; D4's hand-copied prune versions (`main.cpp:239-256`); FOX's `session/tab` stored by label (`MainWindow.cpp:194`); FOX's missing scoped Restore Defaults (`SettingsDialog.cpp:883-885`); FOX `verify-src.py` `HEADER_ONLY` table of two entries (`:38-41`); POE2's hotkeys bound once at construction (`MainWindow.cpp:131-151`), POE2's dead `AppLog.h`/`LogConsole`/`SehGuard`/`PanelPersist` and no message handler at all (`poe2/src/main.cpp` has none), and the no-op format check (`verify-src.py:88-106`).
