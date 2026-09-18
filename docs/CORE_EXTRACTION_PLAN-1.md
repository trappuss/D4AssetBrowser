# AssetBrowser core extraction — the plan

**What this is.** The concrete plan for turning the `(Game|Engine)AssetBrowser` family from three
copies of one design into one shared core plus one plugin per game. It was produced by auditing
the real source of D4AssetBrowser, FOXAssetBrowser and POE2AssetBrowser (513 files, ~198k lines)
against `ASSETBROWSER_TEMPLATE.md`. Every classification below is grounded in a file:line
citation in the five audit reports under `core-audit/`; where a decision is a judgement call
rather than a measurement it is marked **HUNCH** and listed in §8 for you to settle.

**Read this before starting the extraction.** Do not start from the template alone: the template
says "copy the mechanism from D4", and the audit found that D4 is the *wrong* base for most of the
generic code (§3). The right core is assembled from the best version in each of the three
projects.

The five audit reports (same folder, `core-audit/`):

| Report | Covers |
|---|---|
| `viewport.md` | `GLModelWidget` ×3 — feature matrix, public APIs, couplings, drift, base choice |
| `geometry-export.md` | geometry / rig / clip / material types, the three glTF writers, capture, layout, name templates, the `core::` struct sketches |
| `app-shell.md` | main window, settings dialog, config, hotkeys, log, notifier, panels, search, funnel, menus, `verify-src.py` |
| `tabs-store.md` | list models, the row → parser → viewport call chains, background-index shape, bulk, panels, **the plugin interface** |
| `plugin-build.md` | the engine-specific side of each project, the store APIs, ids, online data, CMake / vcpkg / verify / headless testing, session kickoff |

---

## 1. What the audit measured

**The three tools share almost no code today.** A same-name comparison across the three `src`
trees found exactly one file byte-identical in all three — `GifEncoder.cpp` — plus `SehGuard`.
D4 and POE2 share eight more small helpers verbatim (`AppLog.h`, `CsvCopy`, `Hotkeys.h`,
`LogConsole`, `NameTemplate.h`, `PanelPersist.h`, `TextReportDialog.h`, `GifEncoder.h`).
Every other file that carries the same name in two projects is a rewrite: line similarity below
0.30 for `MainWindow`, `SettingsDialog`, `ModelsTab`, `TexturesTab`, `GLModelWidget`,
`BulkExtractorTab`, `ExportLayout`, `QueryTerm`, `Config`, `ExportNotifier`. FOX diverged even
on the helpers D4 and POE2 kept identical (its `Hotkeys.h` is 289 lines to their 38).

**The divergence is not random — each project got some things right that the others lack.**
That is the finding that shapes the plan:

- FOX has the most template-shaped **shell and viewport**: a `ShadingMode` enum, an overlay
  gate, a selection set living in the widget, hide/isolate/fullscreen/help driven by a hotkey
  registry, camera presets, a first-run page, crash handler, startup profiler, density and font
  scale, cache-maintenance table, the three-layer search split, the menu vocabulary/rule/dump
  split, `NPanel`+`PanelBox`, a ~190-check regression suite. (`app-shell.md` §1, `viewport.md` §5)
- POE2 has the cleanest **contracts**: a neutral `ModelGeometry` (one shared buffer, parts as
  index ranges), a `GlbExporter` that takes geometry + skeleton + materials + options and builds
  in memory, an `AssetStore` whose four methods are all `BulkExtractor` needs, one material
  resolver feeding viewport and export, `.gltf+.bin`, three KHR extensions, MASK and BLEND, a
  scoped reset-by-removal, a ten-entry self-test table. (`geometry-export.md` §1.4, §5.3;
  `tabs-store.md` §5, §7)
- D4 has the most **features and the most scars encoded**: the Rendered pipeline (shadows,
  SSAO, cubemap IBL, ACES), the click-on-release selection with the double-click swallow flag,
  worker-thread model loading with token and SEH guard, the `BrowserTab` export-hook interface
  and `LazyTab`, the `IndexDesc` roster, retarget presets and X-mirror, hardpoint empties,
  animation-library export, the time-accessor cache, the most complete `verify-src.py` (12 check functions).
  (`viewport.md` §1, `geometry-export.md` §5, `app-shell.md` §1)

**Each project also carries dead or drifted code the others fixed.** POE2 compiles `SehGuard`,
`AppLog.h`, `LogConsole` and `PanelPersist` and calls none of them; D4 links `fastgltf` and
`tinygltf` and includes neither; D4's Textures tab bypasses `QueryTerm` so `a|b` silently
returns nothing there; FOX's `.fcnp` lookup is a linear scan over every file on each model
load; POE2 embeds textures for hidden parts' materials; FOX's Ctrl+double-click is the exact
toggle-twice shape that template §11 scar 1 describes. (`tabs-store.md` §9, `app-shell.md` §8.7,
`geometry-export.md` appendix, `viewport.md` §1.4)

**FOX has no engine-neutral geometry type.** Its parser output (`fox::FmdlFile`) goes straight
to GPU-shaped uploads and its exporter consumes `FmdlFile` directly. An `FmdlFile →
core::Geometry` adapter is the single largest piece of genuinely new code the extraction
implies. (`tabs-store.md` §7.2, `plugin-build.md` §1.2)

---

## 2. Target layout

One repository, one folder in `Claude Current`:

```
AssetBrowser\
  CMakeLists.txt                 root: qt_standard_project_setup, one vcpkg manifest, assetbrowser_configure_target()
  vcpkg.json                     union: qtbase{widgets,opengl,gui,png,jpeg}, qtsvg?, zlib, lz4
  CLAUDE.md                      the generic rules ONCE (today they are restated in three files)
  docs\                          ASSETBROWSER_TEMPLATE.md moves here; CONTEXT_MENUS.md; this plan; core-audit\
  core\                          static library `abcore` (Q_OBJECT classes listed for AUTOMOC)
    types\                       Geometry, Skeleton, Clip, Material, TextureImage, Hardpoint, AssetId, AssetRow, ExportOptions
    math\                        Mat4/Quat, one multiply convention (§8.1)
    store\                       IGameStore, BackgroundIndex (the §2 shape, once), RunCache
    gl\                          GLModelWidget, GLTextureWidget, ThumbnailRenderer, ViewportBar/Gizmo/HUD, PartMenu
    export\                      GlbExporter, ObjExporter, ExportCapture (GIF ladder), GifEncoder, ExportLayout, NameTemplate, Retarget
    app\                         AppShell (MainWindow), SettingsDialog skeleton, Hotkeys, AppLog+CrashHandler+LogConsole, Config pattern,
                                 ExportNotifier, StatusLine, FirstRunPage, CacheMaint, StartupProfile, Density/FontScale, SehGuard
    ui\                          AssetListModel, FunnelFilter/chips, SearchBox/SearchQuery/QueryTerm, NPanel/PanelBox/PanelPersist,
                                 MenuText/MenuContext/MenuDump, HintBar, HoverPreview, ThumbnailCache, TextReportDialog, CsvCopy/TableCopy
    tabs\                        BrowserTab base, ModelsTab, TexturesTab, BulkTab (generic; driven by IGameStore)
    tex\                         bcdec + DDS container probe + BlockImage::decode; FoxZlib; ZipReader/Writer; hash primitives; NaturalOrder
    tools\                       verify-src.py (tables per game), check-links.py, render_offscreen, conformance runner
  games\
    d4\                          CascReader, SnoIndex, ModelParser, AnimParser, MaterialDecode, tex tables, the eleven indexes,
                                 deps (d4data/TACT/update), Wardrobe/Stable/Catalogue tabs, verify.toml, CLAUDE.md, NEXT_SESSION.md
    fox\                         fox\*, anim\*, ArchiveIndex + catalogs, ModelLoader (becomes the adapter), Customize/Files tabs, dict\, harness flags
    poe2\                        bundle\*, MeshParser, AstSkeleton, AssetText, AssetStore, NameIndex, MaterialFamilyIndex, Customize tab, third_party\ooz
  conformance\
    fixtures\<game>\             minimal pulls (FOX already has treeV/instS; POE2 staged three data bundles)
    tests\                       self-test registry runner, headless render, export twice + md5, stdout-needle checks under xvfb
```

Each game is a `qt_add_executable` that links `abcore` and its own `games/<x>/<x>fmt` static
format library (so probe/harness tools link the format lib instead of re-listing sources as
FOX's `foxab_probe` does today). One `build.bat` / `rebuild.bat` / `run.bat` per game, generated
from a template, in `games/<x>/`.

Not in this repository: DIAssetBrowser (Python/PySide6 — a different line; a rewrite as a
`games/di` plugin is possible later but is not part of this extraction).

---

## 3. Core inventory — source of truth per module

"Base" is the file the core version starts from. "Graft" is what must be ported into it from
the other two before the base is complete. "Strip" is what leaves the base because it is
game-specific. Citations are in the named report.

### 3.1 Data types (`core/types`, `core/math`) — `geometry-export.md` §1–4, §7

| Type | Base | Graft | Strip / stays in plugin |
|---|---|---|---|
| `Geometry` / `Vertex` / `Part` | POE2 `ModelGeometry.h` (shared buffer, index-range parts, bbox, tangent, provenance) | D4: `hasColor/hasUv1` flags, per-part `doubleSided`, `slotHash`; FOX: group-tree `groupParent`; widen joints to `uint16` (POE2's `uint8_t` caps rigs at 256 bones) | D4 cloth block, `vertexBuffers`; FOX SoA layout (flattened by the adapter) |
| `Skeleton` / `Joint` | POE2 shape, but with D4's per-joint content (name, hash, parent, rest TRS, IBM) | D4 `cloth/chain` flags + `nBaseBones` as optional; **sort parents-before-children at ingest and assert** (D4 and FOX already guarantee it; POE2 resolves recursively) | D4's mixed-space contract (Y-up verts, Z-up rest TRS) — the D4 adapter swaps once at ingest |
| `Clip` / `Track` | POE2 sparse per-channel keys, seconds, bound by joint index | D4: dense → sparse (times = i/fps), hash → index at ingest; FOX: baked-dense from the pose solver | FOX `.frig`/IK solver, D4 AnimSet taxonomy |
| `Material` / `TextureImage` | POE2 `ExportMaterial` (alphaMode enum, specular colour, transmission, emissive strength) | D4 `hasX` presence flags; FOX `lossyOk`/`srgb`; one `NormalConvention{greenDown, needsZ}` replacing D4 `flipNormalGreen`, FOX `normalsGreenDown`, POE2 `reconstructNormalZ` | D4 dye/detail bakes, FOX colour-layer bake, FOX SRM→ORM easing (all adapter-side); FOX/POE2 viewport-only fields go in an engine extension bag |
| `Hardpoint` | D4 `ModelHardpoint` + FOX `ConnectPoint` merged (`modelSpace` flag carries D4's "authored ≠ identity ⇒ model-space" rule) | — | POE2 has none (returns empty) |
| `Mat4`/`Quat` | POE2 `RigMath.h` | D4's `invertRigid/invert/decomposeTRS`; FOX `quatFromMatrix`, `rotate`, `fromTo` | see §8.1 — all three already share one 16-float memory layout; only the multiply argument order differs |
| `AssetId` | new: `{quint8 scheme; quint64 value}` + plugin `toString/fromString` | D4 packs `(group<<32)\|sno`; FOX carries its GZ-vs-TPP scheme bit; POE2 is the MurmurHash | (`plugin-build.md` §4) |
| `AssetRow` | new, from the union in `tabs-store.md` §1.4: id, name, inGameName, collection, size, tags, flags bitset | flags: Encrypted/Unrenderable/Animated/Rigged/Orphaned/New (D4), Shadowed/Unnamed (FOX) | everything else answered lazily through `IGameStore` |
| `ExportOptions` | FOX `ExportOptions` → one `sceneOptionsFrom` converter + text presets (the pattern) | POE2 fields (`gltf`, `looseTextures`, `onlyClip`), D4 (`animationLibrary`, `xMirror`, `hardpointEmpties`, retarget preset) | D4's ~20 loose QSettings keys read in `ModelsTab_Export.cpp` |

### 3.2 Store and index (`core/store`) — `tabs-store.md` §3, §7; `plugin-build.md` §3

| Module | Base | Graft | Notes |
|---|---|---|---|
| `IGameStore` | POE2 `AssetStore` API shape (`open/readFile/loadModel/loadSkeletonFor/resolveMaterials/loadTexture/attachmentsForModel/collectAssetFiles`) | D4: `encryptedSnos`, `payloadSize`, two-blob reads (meta+payload) → a "part" selector; FOX: `readContainer` (read once, pull children), install scope | Full sketch with the per-method implementation map for all three projects: `tabs-store.md` §7. Threading contract must be stated: `open/loadModel/loadTexture/resolveMaterials` thread-safe; `row/tagGroups/searchBlob` GUI-thread |
| `BackgroundIndex` (the template §2 shape, once) | FOX `ArchiveIndex` (detached thread → fingerprint-signed cache → `install()` queued → `readyChanged` → `reset()` → generation counter, all present) | D4 `IndexDesc` roster struct for the File ▸ Index menu | D4's older singletons (AppearanceMeta, AssetLinks, IconIndex) lack the generation counter the MainWindow comment claims they have; POE2 mutates the store on the worker with no `install()` swap. Eleven D4 indexes re-implement this shape by hand — they become subclasses |
| Model load path | D4 `ModelsTab::loadGeometry` (worker thread, token check, `seh::runGuarded`, geometry cache) | — | FOX and POE2 parse synchronously on the GUI thread. The core `ModelsTab` uses D4's pattern for every game |
| `RunCache` (RAII, run-scoped) | D4 `MaterialDecode::TextureCacheScope` interface | FOX `blobcache::Scope` | POE2 returns a no-op |

### 3.3 Viewport (`core/gl`) — `viewport.md` §1–6

| Module | Base | Graft | Strip |
|---|---|---|---|
| `GLModelWidget` | **FOX** (structural base: neutral `GLMeshUpload/GLSkeletonUpload/GLConnectPoint` inputs, `ShadingMode` enum, selection+context sets, hide/isolate/frame/fullscreen/help, `renderAtSize`, turntable, camera presets, RAII scope guards, harness hooks) | POE2: `AlphaMode` two-pass (opaque+mask, then blend/additive), `renderToImage(scale, transparentBg, cropToModel)`, `skeletonMatchesMesh` fail-closed gate; D4: click-on-release selection with `m_swallowLeftClick` cleared on every press, Ctrl/Shift toggle, right-click scoping (outside replaces, inside acts on the set), stencil outline + `grabSupersampled` + `setCoverageAlpha`, live-pose `partsBounds` framing, camera glide, the Rendered pipeline (shadow map, SSAO, cubemap IBL, A2C, ACES) behind feature toggles; **overlay master gate moves into the widget** (POE2's placement) so no tab can bypass it | FOX `GLPbrMaterial` field vocabulary, `DebugView` names, `GameId`, `Hotkeys.h` include, QSettings camera presets (→ injected); D4's `Config::d4dataDir()` read + cloth tuning JSON in `setGeometry`, the `detail_mask_probe.txt` write on every load, bone-hash dictionaries, ~100 D4 material setters, the cloth solver (~1,900 lines — stays in `games/d4` as a viewport extension); POE2's hard-coded `toYUp` (the adapter rotates at ingest) and in-widget engine pose evaluation |
| Viewport furniture | FOX `ViewportBar`, `ViewportGizmo`, `ViewportHud`, `RenderPanel` | D4 `CameraOrbitRow`, `ViewportSettings` reset-by-removal groups | — |
| Part menu | D4 `ViewportPartMenu::{Info,Actions}` builder shape (labels count sets, plural vocabulary) | FOX `partmenu::Context` (viewport + tree share one builder) | D4 `Info::sno/collection/isSim/isFx` → nouns supplied by the plugin |
| `GLTextureWidget` | POE2 (`setImage(QImage)`, engine-agnostic) | D4's BC-upload path only if a game needs GPU decode | D4 `eTexFormat` parameter |
| Thumbnails | FOX `ThumbnailRenderer` (own thread + context + disk cache) | POE2 `ModelThumbnailRenderer` API (`render(Geometry, baseColors)`) | FOX's `ArchiveIndex`/`ModelLoader` addressing → `IGameStore` |
| Skinning | keep CPU skinning; palette computed by a core pose evaluator over `core::Clip` | FOX's `applyPose(palette, skeletonLines)` API for engines with their own solver | — |

### 3.4 Export (`core/export`) — `geometry-export.md` §5–6

| Module | Base | Graft | Strip |
|---|---|---|---|
| `GlbExporter` | **POE2** (neutral inputs, in-memory `build()`, `.gltf+.bin`, loose textures, three KHR extensions, MASK+BLEND, IBM-order and accessor-order scars documented in code, always-TRS nodes) | D4: animation-library mode, time-accessor cache per (count,fps), hardpoint empties with the `IBM·socket` rule, retarget layer (presets, `.L/.R`, X-mirror, `flipNormalGreen` self-test), `AnimExportScope`/`AnimClipFilter` as generic clip-selection policies; FOX: `usedMaterials` pruning (fixes POE2 embedding hidden parts' textures), image de-dup + JPEG + size cap, wrapper-node scale/up-axis mode, constant-track elision, per-clip shared time accessor, `prepareMesh` shared with OBJ, `reduceRig`, u8/u16 joint width, `SceneAccum` multi-part with clips keyed by name, end-of-run `qInfo` summary | D4 `swapTRS`/`SymBone`/`DecodedAnim` coupling and its two duplicated container tails; FOX `FmdlFile` input |
| `ObjExporter` | FOX | — | — |
| `ExportCapture` (GIF ladder, stills) | FOX `ViewCapture` structure (options struct, interactive wrappers, memory guard, clipboard, one `encodeGif`) | POE2's viewport interface (`renderToImage/orbitYaw/setAnimTime/currentClipFps`), turntable clip-snap (D4/POE2 — FOX lacks it) | D4 `settleCloth`/`CaptureScope` → a plugin "pre-capture" hook. The ladder constants (¾, 32, 0.93, 96 px) are identical in all three copies — one copy in core |
| `GifEncoder` | any (byte-identical ×3) | — | — |
| `ExportLayout` | FOX (universal sanitizer, `modes()` table with labels/hints, group + per-file APIs) | POE2 self-test; D4 legacy `bulk/organize` migration | the three taxonomies (AppearanceMeta / path tree / ModelTags) → `folderKeyFor(item)` callback |
| `NameTemplate` | FOX `applyNameTemplate` (`{{Frame}}` auto-padded, `{{Part}}`, `{{Clip}}`, `{{Date}}`, tidy pass, " CON " reserved-name story) | `{{Id}}` as the generic id token (`{{SNO}}` = D4 alias) | POE2's copy of D4's file has no UI and one literal call — dead |
| `Retarget` | FOX `Retarget.h` rules (readable names, mirror names, `reduceRig`) + D4 `Retarget` (`collapseClothChains`, `remapToAnchors`, X-mirror) | D4's bone renaming currently lives in `GLModelWidget` (`blenderizeSkeletonNames`) — it moves here | D4's 26 curated anchor hashes stay in `games/d4` |

### 3.5 Application shell (`core/app`, `core/ui`) — `app-shell.md` §1–5, §8

| Module | Base | Graft | Strip |
|---|---|---|---|
| `AppShell` (MainWindow) | FOX (first-run stack, status bar with `StatusLine` source filter, `applyHotkeys` by registry key, `populateExportMenu(QMenu*)` delegation, `restoreWindowLayout`, DevShot scheduling skeleton) | D4: `BrowserTab` virtual export hooks + `LazyTab`, `IndexDesc` roster → File ▸ Index submenu + indicator + toast, export toast/tray, Ctrl+K palette + Alt-nav given a `jumpTo(kind,id)` hook, Help ▸ Copy diagnostic info | the hardcoded tab lists (all three) → `AppPlugin::tabs()`; game rows in Help; D4's positional hotkey binding (binds by array index — CONTEXT_MENUS §6 warns this "binds the wrong keys silently") and hand-written F1 sheet; POE2's hotkeys bound once at construction |
| `AppPlugin` hook | new — sketch in `app-shell.md` §2.5: `productName`, `tabs()`, `indexRoster()`, `reload()`, `helpEntries()`, `diagnosticInfo()`, `firstRunPage()`, `isConfigured()`, `settingsPages()`, `wireCrossTab()` | — | — |
| `SettingsDialog` skeleton | FOX (`addPage`, `showTab("Export/Images")` by name, hotkey-clash detector) | D4 `showEvent` screen clamp and Cancel snapshot/revert (the only "Cancel truly cancels" for live-written keys); POE2 `restoreExportDefaults()` as the reset-by-removal model; D4 `ViewportSettings` scoped groups + keep-prefixes; `SettingsPage{title, order, build, apply, revert, resetGroups, keepPrefixes}` per `app-shell.md` §3.3 | every game page. Core-owned pages: Settings profile, Interface (Startup && layout, On-hover with D4's `HoverInfo` keys, Diagnostics), Export ▸ Images && GIFs / File names, Hotkeys, Maintenance ▸ Caches && reset (from a registered `cachemaint::defs()`), Information frame |
| `Hotkeys` | FOX (36 rows with hints, `seq(key)`, cheat-sheet text/HTML generators, `Role` lookup) | — | the row table is per game |
| `AppLog` + `CrashHandler` + `LogConsole` | FOX | D4 `AppLog::setFileLogging` live toggle (wired to Settings ▸ Diagnostics) | product name in the crash-file name → parameter |
| `Config` | FOX pattern (accessor pair per key, named constants, **session overrides** — what makes headless tests safe) | POE2 `exportOptionsFromConfig()` one-liner | all game keys |
| `ExportNotifier` | POE2 (minimal) | FOX `file` argument + after-export action (`{{File}}`/`{{Folder}}` command) | D4/FOX `glbOptionsLine` (game-typed) |
| `StatusLine`, `StartupProfile`, `Density`, `FontScale`, `CheckStyle`, `RowShading`, `FirstRunPage`, `CacheMaint`, `NaturalOrder` | FOX (zero coupling) | `FirstRunPage` takes a folder-list provider; `CacheMaint` takes registered defs | — |
| `main()` | the generic sequence in `app-shell.md` §5: SEH install, org/app/version, INI path, log + crash handler, cache prune from the plugin's registry, self-test runner (POE2 table shape, FOX `qWarning` reporting — POE2 prints to stderr which a Windows GUI build discards), icon, style, `restoreWindowLayout`, exec + shutdown | — | GL version/profile, migrations table, CLI flag set → per game |
| Search | FOX three-layer split: `QueryTerm` (term matcher + self-test) / `SearchQuery` (outer language) / `SearchBox` (Esc, ↓ history, remember-on-commit) | POE2 `Query{and,not,meta,id}` + `isHexId`; **`isIdTerm` policy injected** (SNO digits / MurmurHash hex / PathFileNameCode) | — |
| Funnel + facets + chips | core ships the widgets: sticky popup with grouped checkboxes + live counts (POE2), Match-any toggle, chip flow layout (FOX `FilterChips`), tinted funnel button | each game decides FOX's "ticks rewrite the query as `#tag`" or POE2's "persist facet ids" | POE2 `Facets.h` pulls `MaterialFamilyIndex`; FOX `TagFunnel` pulls `ModelTags` — both vocabularies become plugin-supplied `TagGroup`s |
| Menus | FOX `MenuText` formatting helpers + `MenuContext` selection rule + `MenuDump` census | D4 `LookIcon::addActions` (Copy/Save image, disabled-not-hidden) | nouns (`kCopySno` vs `kCopyHash`, collection noun or absent) → a `MenuText::Nouns` struct the plugin fills |
| Panels | FOX `PanelBox` + `NPanel` + `PanelPersist` (pane-count guard fixes a measured Qt defect) | — | glyphs from `ViewGlyphs` → a core glyph set |
| `AssetListModel` | POE2 (`std::function` icon provider, `applyFilters` once per keystroke, facet counts) | D4 `SnoListModel` hooks (`setSearchBlob/setPredicate/setFailedPredicate/setPresence`) as the lazy-fact callbacks | — |
| `CsvCopy`/`TableCopy` | merge: D4's view-generic `install` + FOX's `addMenuActions`/`installWithMenu` | — | — |
| `HoverPreview` | FOX widget (dwell timer, wheel resize, screen clamp) | D4 `HoverInfo` settings surface | — |
| `ThumbnailCache` | POE2 (zero coupling) | FOX disk tier | — |
| `HintBar`/`TipBar` | FOX (namespaced `tips/` keys, reset-all in Settings) | POE2's `setSizePolicy(Fixed)` fix (the "huge band" bug) | — |
| `TextReportDialog` | D4/POE2 (identical) | — | FOX's copy lives inside `MgsvMetaDialog` — misplaced |

### 3.6 Tabs and bulk (`core/tabs`) — `tabs-store.md` §2, §5, §6

| Module | Base | Graft | Strip |
|---|---|---|---|
| `ModelsTab` | POE2 structure (store-driven, tabbed panels Parts/Animations/Attachments/Info, one resolver) | D4 worker-thread load, panel data set from `tabs-store.md` §6 (parts: name/tris/material/visibility; materials: name/family/roles; info: id/name/path/size/counts/bones; clips; attachments) | every game-specific panel (D4 CLOTH/Looks/Dye; FOX RENDER/help bones; POE2 alpha modes) → plugin-contributed panels |
| `TexturesTab` | POE2 + D4's funnel (formats, tags of using assets, orphans) | D4's tab must route through `QueryTerm` (today it uses bare `contains()` — `a\|b` fails there) | atlas frames (D4-only) behind `IGameStore::atlasFrames` |
| `BulkTab` + `BulkExtractor` | POE2 `BulkExtractor` (standalone QObject taking `IGameStore*` + items + options; coordinator loop; TSan-clean) | FOX: hash-keyed queue, path-aware manifest, dry run, saved queries, degraded-write rule, headless harness; D4: Both mode, factory presets, co-textures, deps, buffers, per-folder passes, shared decode cache, per-item `seh::runGuarded` | D4's delegation to `ModelsTab::bulkExport` (bulk depends on tab internals — parallelism is disabled when animations are on because those paths read GUI state); FOX's extension-name dispatch inside the worker |
| `BrowserTab` base | D4 virtuals (`refresh/reset/onSettingsChanged/persistView` + the export hooks CONTEXT_MENUS §6 lists) | FOX `populateExportMenu(QMenu*)` | D4's `CascReader*/SnoIndex*/GLModelWidget*` setters → `setStore(IGameStore*)` |

### 3.7 Engine-neutral code currently living in game layers — `plugin-build.md` §2

Move to `core/tex`, `core/util`: `bcdec` (POE2's vendored block decoder, the superset — D4's
row-pitch alignment and fmt-46/47 rule sit above it as plugin data), a DDS container probe
(POE2 DX10 + FOX slice/mip walker), FOX `BcEncode` (the family's only encoder, with its
round-trip self-test), `FoxZlib`, `ZipReader/ZipWriter`, `FoxCity` (CityHash64, already Qt-free),
MurmurHash64A and DJB2 primitives, `NaturalOrder`. The hash *schemes* (51+13-bit packing,
lowercase-then-hash) stay in plugins.

### 3.8 Build and verification (`core/tools`, `conformance/`) — `plugin-build.md` §6–9

- **CMake:** one `assetbrowser_configure_target(<exe>)` function replacing three copy-pasted
  sets of `/EHa /MP /bigobj NOMINMAX`, PCH, `.rc` OBJECT_DEPENDS, and the static/dynamic Qt
  packaging (which have already drifted: FOX `/Z7`, POE2 no PCH, no static branch). Drop
  `fastgltf`/`tinygltf` (unused). Decide Svg (linked by D4/FOX, included by nothing — only for
  static plugin baking). Core headers get a `core/` prefix so the seven same-named headers stop
  shadowing each other. Root vcpkg manifest; FOX's baseline moves to match.
- **`verify-src.py`:** D4's script as base (checks 0–7 + dead-key + text-persisted-combo +
  name-substring with the inventory/baseline framework), plus FOX's `check_qprintable`, minus the
  d4data JSON check (→ `games/d4`). The tables (`HEADER_ONLY`, baselines, tokens, allow-lists)
  come from `games/<x>/verify.toml`. Retire POE2's port (no-op format check, dead-key check that
  cannot see concatenated keys, substring count that never fails).
- **Conformance target** (the thing that ends "small inconsistencies"): a registry of every
  `QString selfTest()` (POE2 ten, FOX four, D4 two, plus core's own) run by one `ctest`
  executable without a display; a Linux compile of core + every plugin's format library from the
  real CMake graph (replacing POE2's hand-listed `verify_container.sh`, which has already
  drifted and cannot link the current tree); a headless render taking `core::Geometry` from any
  plugin and including the viewport's shader from the repo (POE2's `render_offscreen.cpp`
  includes a shader copy from `/root/work/scratch/` — outside the repo); export a fixture twice
  and compare md5; FOX's stdout-needle harness pattern (`--tab/--shot/--export`) as the core
  CLI, run under `xvfb-run` against `conformance/fixtures/<game>/`.

---

## 4. The plugin interface

The full interface with the per-method implementation map (which existing function in each
project sits behind each method, and which project would return "unsupported") is in
`tabs-store.md` §7; the `core::` struct sketches with every field tagged [D4]/[FOX]/[POE2] are in
`geometry-export.md` §7; the `AppPlugin` shell hook is in `app-shell.md` §2.5. The shape:

```cpp
class IGameStore {                                   // one per game; thread-safety per method stated in the header
  // lifecycle   open(dir, err, progress) · fingerprint() · buildSecondaryIndexes(progress)
  // rows        kinds() · rowCount(kind) · row(kind,i) → AssetRow · searchBlob(id) · tagGroups(kind) · hasTag(id,tag)
  // bytes       open(id, part, err) · dependencyFiles(id)
  // models      loadModel(id, Geometry&, err) · loadSkeleton(id, geo, decodeClips) · resolveMaterials(id, geo, decodeImages) · attachments(id)
  // textures    loadTexture(id, TexInfo*, err) · atlasFrames(id, img)
  // links/icons usedBy(id) · uses(id) · gameIcon(id, px) · variants(id)
  // caches      beginRunCache() → RAII
};
class AppPlugin { productName · tabs() · indexRoster() · reload(done) · helpEntries() · diagnosticInfo()
                  · firstRunPage() · isConfigured() · settingsPages() · wireCrossTab() · isIdTerm policy · MenuText::Nouns
                  · cachemaint defs · self-test list · verify.toml };
```

What each plugin implements today, by name: D4 `CascReader::open/readPayloadBySno/readMetaBySno`,
`ModelParser::parseApp`, `buildExportMats` (must merge with `applyPartMaterials` — D4 has two
material resolvers, POE2 one), `AppearanceMeta::tagsFor/titleFor`, `AssetLinks`, `IconIndex`;
FOX `ArchiveIndex::rebuild/readFile`, `FmdlFile::parse` + **the new adapter**, `modelload::load*`,
`ModelTags`, `TextureUsers/RefIndex`, `IconCatalog`; POE2 `AssetStore::*` almost verbatim.

D4's tab-owned indexes (`ensureEntityIndex`, `ensureAnimatedIndex`, blocklist, animated/rigged/
orphaned sets — `ModelsTab.h:243-246, 362-402`) move to the D4 store; that is what lets
`BulkExtractorTab` stop holding a `ModelsTab*`.

---

## 5. Migration order and gates

The order is chosen so that the first thing built is the contract, the second is the project
with the least to lose, and the most refined tool is ported last against a core that two games
already pass. Each phase has a gate that must be green before the next starts; no phase ships
new features.

| Phase | Work | Gate |
|---|---|---|
| **0 — Freeze and fixture** | Tag all three repos. Build `conformance/fixtures/<game>/` from what already exists (FOX treeV/instS, POE2's three data bundles + a coat, D4 the five repro assets in `CLAUDE.md`). Record md5 of a reference export per game. | The three tools export the fixtures and the md5s are recorded |
| **1 — Contract** | `core/types`, `core/math`, `IGameStore`, `AppPlugin`, `BackgroundIndex`, `AssetId`, `AssetRow`. Header-only where possible. Write the POE2 store against it (nearly a rename). Write the D4 and FOX *adapters* as thin functions over the existing parsers (`ModelGeometry→core::Geometry` for D4 with the single Z-up swap; `FmdlFile→core::Geometry` for FOX — the biggest new piece). | Adapter round-trip self-tests: each fixture model parsed through the adapter has the same triangle count, bbox, joint count and parent order as the original path |
| **2 — Export core** | `GlbExporter` from POE2 + the grafts in §3.4; `ExportLayout`, `NameTemplate`, `ExportCapture`, `GifEncoder`, `Retarget`. | Export every fixture through the core writer and the original writer; Blender import check (`blender_import_check.py`, pip bpy) and `gltf_skin_check.py` on both; per-game diffs explained (e.g. POE2 no longer embeds hidden materials) |
| **3 — Viewport core** | FOX widget as base + grafts in §3.3. Overlay gate in-widget. Selection in-widget with mirror signal. | Headless render of every fixture (core `render_offscreen`) compared to the original tool's render; part isolate test (POE2's `part_isolate_test` generalised) |
| **4 — Shell + generic tabs** | `AppShell`, `SettingsDialog` skeleton, `Hotkeys`, log/crash, `Config` pattern, search, funnel, menus, panels, `ModelsTab`/`TexturesTab`/`BulkTab`, `verify-src.py` with tables. | `games/poe2` runs on the core end-to-end; the POE2 self-tests and `bulk_verify` pass; stdout-needle checks for every tab under xvfb |
| **5 — FOX on core** | Customize and Files tabs stay FOX-owned but consume core viewport/panels/menus/export. The ~190-check `Verify - Regression.bat` and `regress.sh` are the acceptance suite. | Regression suite green on the user's install; container `regress.sh` green |
| **6 — D4 on core** | Models/Textures/Bulk on core; Wardrobe/Stable/Catalogue stay D4-owned as viewport extensions (cloth solver, dye, fur, FX, detail maps hang off the core widget through an extension hook). This is the riskiest phase and is last on purpose. | Every repro asset in `d4/CLAUDE.md` renders and exports as before; `Audit - Asset Health.bat` diff shows no newly-broken; the five `Dump -` / `Test -` bats still pass |
| **7 — Retire** | Delete the three old `src` trees from the monorepo (they stay in the tagged old repos). Move the generic rules from three `CLAUDE.md`s into one. Restructure kickoff files per §7. | `grep` finds no duplicated helper name across `games/`; `verify-src.py` clean on the whole tree |

Effort is not estimated here (**HUNCH** would be all it could be). What can be said from the
audit: phase 1's FOX adapter and phase 6 are the two large items; phases 2–4 are mostly moving
code that already exists; phase 5 is mostly mechanical because FOX's viewport and shell are
the base.

---

## 6. Rules that keep it from drifting again

These go into the root `CLAUDE.md` and the `assetbrowser-design` skill. Each one names the drift
it prevents, as the template does.

1. **A [U] feature is implemented in `core/`, never in `games/<x>/`.** The GIF ladder exists as
   three hand-copies with identical constants; `ExportLayout` as three ports; the §2 index shape
   as thirteen hand-rolled singletons. One home.
2. **A plugin talks to the core only through `IGameStore` and `AppPlugin`.** D4's showpiece tabs
   reach around `ModelsTab` into `ModelParser`/`MaterialDecode`/`CascReader` directly; POE2's
   Customize does the same against `AssetStore`. That is fine — they already program against the
   store layer — but it must be *the* store layer.
3. **Nothing generic goes into `core/` until a second game needs it.** Cloth, AnimSet taxonomy,
   appearance sets, Fox rig units, PoE2 `.ao` assembly all stay game-owned.
4. **Conformance is the definition of "consistent".** A change to core must leave every game's
   self-tests, headless renders and export md5s green before hand-back. The prose template stays
   as the *why*; the test is the *what*.
5. **Scars become tests, not paragraphs.** The double-click swallow flag, the IBM order, the
   accessor-after-animation rule, the pane-count guard, the "Cancel truly cancels" snapshot — each
   is one regression test in `conformance/tests/`, cited from the template section that tells the
   story.
6. **One `verify-src.py`, per-game tables.** The script is core; `HEADER_ONLY`, baselines, token
   lists and allow-lists live in `games/<x>/verify.toml`.

---

## 7. What a new game costs after this

Phase 0 stays as it is today and is the part the audit found done best in POE2: research →
`FORMATS.md` with the method stated → a Python oracle → the C++ parser verified against the
oracle, distinguishing present · unnamed · absent.

Then `new_game.py <name>` scaffolds `games/<name>/`: a stub `IGameStore` with every method
returning "unsupported", a stub `AppPlugin` with the generic tabs registered, `verify.toml`,
`CLAUDE.md` (identity, data location, ground-truth pointer, refuted hypotheses, repro assets,
env-gated diagnostics — the seven items `plugin-build.md` §10 found to be the only genuinely
per-game content), an empty `NEXT_SESSION.md` (the work queue and last tree hash, kept
separate from the stable facts so it does not grow to 1,279 lines as FOX's has), build bats, a
wiki skeleton, and a fixture folder. A session then implements `open/loadModel/loadTexture/
resolveMaterials` and has the whole Tier 1–2 tool; the compiler and the conformance target
enforce the rest.

---

## 8. Decisions for you (marked HUNCH in the reports)

1. **Matrix multiply convention.** All three projects already use one 16-float memory layout
   (translation at [12..14]); D4 is column-vector `mat4mul(a,b)`, POE2 and FOX row-vector
   `mul(a,b)` = "a then b" (`geometry-export.md` §2.4). The recommendation is row-vector because
   two of three and the CPU-skinning code use it, and D4's function is the same bytes with
   swapped arguments. Your call.
2. **Viewport base = FOX, not D4.** The trade-off is stated in `viewport.md` §5: FOX has the
   structure the template asks for and neutral inputs; D4 has more rendering features but its
   6.7k-line widget reads the game-data directory and writes a probe file on every load. The
   grafts D4 contributes are listed. Alternative is D4 as base with the cloth/dye/fur/FX systems
   extracted out — larger surgery. Your call.
3. **POE2 first, D4 last.** My earlier suggestion was "seed core from POE2 then port D4 as the
   proof". The audit refines it: the *contracts* come from POE2, the *shell and viewport* from
   FOX, and D4 is ported last. Same spirit, corrected base choice.
4. **Model loading on a worker thread for every game** (D4's pattern). FOX and POE2 load on the
   GUI thread today. Recommended yes; it changes FOX's `modelload` call sites.
5. **`AssetId` as `{scheme, value}`** with plugin `toString/fromString` — the alternative is a
   plain `quint64` with D4 giving up group scoping. Recommended the struct.
6. **Svg module**: linked by D4 and FOX, included by nothing. Drop, or keep as a per-game option?
7. **DIAssetBrowser**: outside this extraction (Python). Leave as its own line, or plan a
   `games/di` rewrite later?
8. **The defects found on the way** (§1, fourth paragraph; full list in `tabs-store.md` §9,
   `app-shell.md` §8.7, `geometry-export.md` appendix): fix them in the old trees now, or only
   as they are reached during extraction? Fixing now keeps the phase gates honest (a known bug in
   the reference export becomes a known diff); it costs three rebuild cycles.
