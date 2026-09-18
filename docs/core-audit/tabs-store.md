# Audit: list-driven tabs and the index/store layer (D4 · FOX · POE2)

Scope: the seam where generic list UI meets game data — the row model, the load chain
(row → store → parser → geometry → viewport), the background index shape, filtering, bulk
extraction, panel data needs, and the plugin interface those chains imply.

Method: every claim below cites `file:line` under `/home/claude/audit/`. Statements that are
inferred rather than read are marked **HUNCH**. Where a function is split across `_Panels` /
`_Export` files the split file is cited. Template sections referenced: §2, §4, §9, §12, §13,
§14, §25 of `d4/docs/ASSETBROWSER_TEMPLATE.md`.

Headline findings (details in the sections):

1. The three "list models" are three different classes over three different row types, but the
   **row identity is the same shape in all three: an integer id + a name/path** — D4
   `SnoEntry{snoId,name}` (`d4/src/index/CoreToc.h:8-11`), FOX `IndexedFile` addressed by a file
   index into `ArchiveIndex::files()` (`fox/src/index/ArchiveIndex.h:43-55,149`), POE2 a
   `uint32_t` file-record index into `BundleIndex::files()` (`poe2/src/index/AssetListModel.h:103,133-135`).
2. **Only D4 loads geometry on a worker thread**; FOX and POE2 parse synchronously on the GUI
   thread on row change (`d4/src/tabs/ModelsTab.cpp:8154-8181` vs `fox/src/tabs/ModelsTab.cpp:2484-2487`,
   `poe2/src/tabs/ModelsTab.cpp:444-476`). A core would have to pick one.
3. The one-matcher rule holds in FOX and POE2 everywhere; in D4 it holds for Models/Bulk/list
   model but **the Textures tab predicate still uses bare `contains()`** (`d4/src/tabs/TexturesTab.cpp:2029-2035`),
   so `a|b` OR does not work there. That is exactly the drift `util/QueryTerm.h:8-13` was written to end.
4. The background-index shape (§2) is fully implemented in FOX `ArchiveIndex`; D4's older
   singletons (AppearanceMeta, AssetLinks, IconIndex) lack the generation counter; POE2's
   IndexLoader has the generation but no install()/reset() split.
5. FOX's bulk is the superset (queue by hash, dry-run, saved queries, degraded-write rule,
   headless harness); D4 has the richest matched-set semantics (Both mode, factory presets,
   co-textures, layout groups); POE2 is the cleanest worker/coordinator split.

---

## 1. The asset row

### 1.1 D4 — `SnoEntry` through `SnoListModel`

| Field | Source | Cite |
|---|---|---|
| id (SNO, unique *within a group*) | `SnoEntry::snoId` | `d4/src/index/CoreToc.h:8-11`; group-scoping stated at `d4/src/tabs/BulkExtractorTab.h:49-54` |
| file name | `SnoEntry::name` (blanked to `~unnamed_<sno>` when encrypted) | `d4/src/index/CoreToc.h:10`; `d4/src/index/SnoIndex.h:78-80` |
| in-game name (title) | `AppearanceMeta::titleFor(sno)` — column 3 in Models layout | `d4/src/index/SnoListModel.cpp:408` |
| collection | `AppearanceMeta::collectionFor(sno)` — column 4 | `d4/src/index/SnoListModel.cpp:409` |
| kind/type | the SNO **group** the tab loaded (9 = Appearance, 44 = Texture) — one group per model instance | `d4/src/index/SnoListModel.h:23`; `d4/src/tabs/ModelsTab.h:102` |
| tags | `AppearanceMeta::tagsFor(sno)` (QSet<QString>), folded into a search blob via `setSearchBlob` | `d4/src/index/SnoListModel.h:36-39`; `d4/src/tabs/ModelsTab.cpp:4478` |
| size | not a row column; `CascReader::payloadSize(sno)` read on selection into INFO | `d4/src/casc/CascReader.h:85`; `d4/src/tabs/ModelsTab.cpp:6210-6212` |
| encrypted | `CascReader::encryptedSnos()` manifest (sno → key name) | `d4/src/casc/CascReader.h:116-134`; facets `d4/src/tabs/ModelsTab.h:354-355` |
| renderable / failed | `setFailedPredicate` (blocklist ∪ no-geometry) dims row + "⚠" prefix; `setPresence` ✓/✗ badge | `d4/src/index/SnoListModel.h:52-59`; `.cpp:402-407,413-414` |
| animated / rigged / orphaned / latest | not row fields — usage facets evaluated by a predicate over `m_animatedSnos`, `m_rigFamilyPrefixes`, `m_apprActors`, `SnoIndex::isNew` | `d4/src/tabs/ModelsTab.cpp:4447-4475`; `d4/src/index/SnoIndex.h:159` |
| icon | `setIconProvider(sno→QPixmap)` (2D inventory icon via IconIndex or render thumbnail), memoized in a bounded `QCache<int,QIcon>` | `d4/src/index/SnoListModel.h:44-49,127`; `.cpp:394-395,419-420` |
| class / gender | derived from the **name convention** `<ccc><f|m>_` pre-meta, from tags once meta is ready | `d4/src/index/SnoListModel.cpp:153-162`; `d4/src/tabs/ModelsTab.cpp:4453-4455,4497-4508` |

The Models tab wraps `SnoListModel` in `ModelOutlinerModel` (tree with one injected subtree under the
loaded row) without changing rows (`d4/src/tabs/ModelOutliner.h:112-125`). `SnoListModel` is stated
to be shared by four tabs (`d4/src/tabs/ModelOutliner.h:114`).

### 1.2 FOX — `IndexedFile` through `FmdlListModel` / `TexListModel`

| Field | Source | Cite |
|---|---|---|
| id | 64-bit `IndexedFile::hash` (PathFileNameCode incl. extension bits); the *row* carries `FileIdxRole` = index into `ArchiveIndex::files()` | `fox/src/index/ArchiveIndex.h:47`; `fox/src/tabs/ModelsTab.h:69-73`; `fox/src/tabs/ModelsTab.cpp:137` |
| path / name | `IndexedFile::path` (forward-slashed); `StemRole`, `DirRole` derived | `fox/src/index/ArchiveIndex.h:54`; `fox/src/tabs/ModelsTab.cpp:138-140` |
| in-game name | **not on the row.** `NameCatalog::nameFor(stem)` exists but is consumed by ExportActions/Customize catalogs, not the list | `fox/src/index/NameCatalog.h:161`; `fox/src/util/ExportActions.cpp:703`; outliner writes stem to ColName at `fox/src/view/ModelOutliner.cpp:636` |
| kind/type | `ArchiveIndex::extensionOf(f)` ("fmdl"/"ftex"/…); container kind | `fox/src/index/ArchiveIndex.h:154-158` |
| tags | `ModelTags::tagsOf(fileIdx)` — six measured categories: game, category, family, variant, source, status | `fox/src/index/ModelTags.h:9-18,80` |
| game | `ArchiveIndex::gameOf(f)` (install-decided, not path) | `fox/src/index/ArchiveIndex.h:120-135` |
| size | `IndexedFile::size` (uncompressed) | `fox/src/index/ArchiveIndex.h:48` |
| flags | `named`, `gz`, `shadowed` (mod-override copy), archive `inactive` | `fox/src/index/ArchiveIndex.h:49-53,104` |
| container child | `childIdx ≥ 0` = inside an FPK/FPKD/PFTXS | `fox/src/index/ArchiveIndex.h:46` |
| icon | grid delegate reads `ThumbnailRenderer::cached(fileIdx,px)` (offscreen GL render on its own thread) and/or `IconCatalog::iconFor(stem)` (the game's own UI icon) | `fox/src/tabs/ModelsTab.cpp:214-223`; `fox/src/gl/ThumbnailRenderer.h:1-20`; `fox/src/index/IconCatalog.h:249` |
| what-uses-it | `TextureUsers::usesOf(hash)` / `userCount`; `RefIndex::uses/usedBy(fileIdx)` | `fox/src/index/TextureUsers.h:73-76`; `fox/src/index/RefIndex.h:279-280` |
| shadowed styling | `shadowui::roleFor(f, role, path)` | `fox/src/tabs/ModelsTab.cpp:142-143` |

Rows do not carry animated/rigged/encrypted facets: Fox has no encryption; rigging is discovered
at load (`locateFrig`, `fox/src/tabs/ModelsTab.h:298`).

### 1.3 POE2 — `BundleIndex::FileRecord` through `AssetListModel`

| Field | Source | Cite |
|---|---|---|
| id | `FileRecord::hash` (MurmurHash64A of lowercase path); row carries the `uint32_t` file index (`Qt::UserRole`) | `poe2/src/bundle/BundleIndex.h:26`; `poe2/src/index/AssetListModel.cpp:230-231` |
| path | `BundleIndex::pathOf(fi)` (ColName; short name in grid mode) | `poe2/src/index/AssetListModel.cpp:217-218` |
| in-game name | `NameIndex::nameFor(path)` (ColTrueName; from .datc64 tables) | `poe2/src/index/AssetListModel.cpp:219`; `poe2/src/store/NameIndex.h:10-25` |
| kind/type | `FileRecord::extId` → `extensions()` (ColExt); tab's base set by ext filter | `poe2/src/index/AssetListModel.cpp:220`; `.h:63-65` |
| size | `FileRecord::size` (ColSize) | `poe2/src/index/AssetListModel.cpp:221` |
| bundle | `bundles()[f.bundle].name` (ColBundle) | `poe2/src/index/AssetListModel.cpp:222` |
| tags/facets | **path-segment facets** (Category / Item type) + **shader facets** from `MaterialFamilyIndex` (workflow/effect bitmasks + `#family:/#workflow:/#effect:` meta) | `poe2/src/index/Facets.h:7-31`; `poe2/src/store/MaterialFamilyIndex.h:34-49`; `poe2/src/index/AssetListModel.h:67-70,126-127` |
| icon | `setIconProvider(fi→QPixmap)` via `ThumbnailCache` (GL render on GUI ticks for models, threaded DDS decode for textures), bounded `QCache<uint32_t,QIcon>` | `poe2/src/index/AssetListModel.h:108-118,146`; `poe2/src/util/ThumbnailCache.h:1-11` |
| encrypted / renderable / animated / rigged / orphaned | **absent** | (no such fields in `AssetListModel.h`, `Facets.h`) |

### 1.4 The union — what a core row would need

| Union field | D4 | FOX | POE2 |
|---|---|---|---|
| stable id (int64 + kind/group) | sno + group | 64-bit hash (+ file index) | 64-bit hash (+ file index) |
| display name / path | name | path | path |
| in-game name | title (AppearanceMeta) | NameCatalog (not on row) | NameIndex |
| collection / set | collection | — | — |
| kind | SNO group | extension | extension |
| tags (grouped) | AppearanceMeta tags (Category/Class/Gender/Type) | ModelTags (6 categories) | Facets (Category/Item type/Shader) |
| size | payloadSize (lazy) | size | size |
| encrypted | yes | no | no |
| renderable/failed | yes | no | no |
| animated / rigged / orphaned | facets | no | no |
| latest (new this build) | yes (`isNew`) | no | no |
| container/source | — | archive, childIdx, shadowed, gz, game | bundle |
| icon | provider fn | ThumbnailRenderer / IconCatalog | provider fn |
| what-uses-it | AssetLinks | TextureUsers / RefIndex | — |

**HUNCH:** the minimal core row is `{id:u64, kind:u16, name:QString, inGameName:QString,
size:u64, tags:QStringList, flags:bitset}` with everything else answered lazily through a
per-game "row facts" callback — that is already the shape of D4's `setSearchBlob`/`setPredicate`/
`setIconProvider` std::function hooks (`d4/src/index/SnoListModel.h:36-48`) and POE2's
`setIconProvider` (`poe2/src/index/AssetListModel.h:113`).

---

## 2. The load call chains

### 2.1 Models: "user clicks a row" → viewport

**D4** (asynchronous, cached, fault-guarded)

| Hop | Cite |
|---|---|
| `currentChanged` → `onAppearanceSelected()` | `d4/src/tabs/ModelsTab.cpp:3191` |
| `entryAt(row)` → `showAppearance(sno, name)` (skips if already current) | `d4/src/tabs/ModelsTab.cpp:5967-5971` |
| `showAppearance`: clears panels, sets INFO from `AppearanceMeta`, `payloadSize`, reads `.app.json` (d4data) for looks/material roster; binary roster fallback `MaterialDecode::appearanceRosterFromMeta(readMetaBySno)` when no JSON | `d4/src/tabs/ModelsTab.cpp:6105-6213,6224-6228,6303-6357,6384-6417` |
| blocklist check; `m_geoTimer->start()` (debounce) | `d4/src/tabs/ModelsTab.cpp:6179-6193`; timer at `3425-3428` |
| `loadGeometry()`: `m_geoCache` hit → `applyLoadedGeometry`; else detached `std::thread` under `seh::runGuarded("parse")`: `reader->readMetaBySno` + `readPayloadBySno` → `ModelParser::parseApp(meta,payload)` → `ModelGeometry` | `d4/src/tabs/ModelsTab.cpp:8127-8165`; parser API `d4/src/model/ModelParser.h:21-22`; store API `d4/src/casc/CascReader.h:77-79` |
| result marshalled back `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` with token check; cache insert | `d4/src/tabs/ModelsTab.cpp:8167-8180` |
| `applyLoadedGeometry(geo, token)`: seat roster names on `primitives[].materialName`; `Hardpoints::readInto`; `m_modelView->setGeometry(m_curGeo, keepView)` + `frameAll` under `seh::runGuarded("render")` | `d4/src/tabs/ModelsTab.cpp:8184-8272`; viewport API `d4/src/gl/GLModelWidget.h:40` |
| then `buildOutlinerSubtree`, `applyPartMaterials` (decodes via `MaterialDecode::texture` → `setPartTextures/Normals/Orm/...`), `fillClothPage`, `fillPartsPage`, thumbnail grab | `d4/src/tabs/ModelsTab.cpp:8304-8326,8931-9208`; push calls at `9193-9218` (relative lines 184-208 of the excerpt) |
| texture decode entry: `decodeTexImage` → `MaterialDecode::texture(reader, d4, name, sno)` | `d4/src/tabs/ModelsTab.cpp:11060-11074`; `d4/src/model/MaterialDecode.h:46` |

Geometry struct: `ModelGeometry{primitives[MeshPrimitive{vertices,indices,materialName,materialIndex,slotHash}], skeleton[ModelJoint], vertexBuffers, hardpoints, clothCapsules, clothSims, nBaseBones}` (`d4/src/model/ModelGeometry.h:22-71,189-217`).

**FOX** (synchronous on GUI thread; shared `modelload` pipeline)

| Hop | Cite |
|---|---|
| `currentChanged` → `fileIdxAt(cur)` → `loadModel(fi)` (also outliner/menu paths) | `fox/src/tabs/ModelsTab.cpp:901-904,511,1038,1306` |
| `loadModel(fileIdx)`: bounds check; sibling `.fcnp` located by **linear scan** and parsed | `fox/src/tabs/ModelsTab.cpp:2435-2480` |
| `index.readFile(f)` → `m_model.parse(data)` (`fox::FmdlFile`) | `fox/src/tabs/ModelsTab.cpp:2484-2487`; store API `fox/src/index/ArchiveIndex.h:162` |
| `InstallScope scope(installOf(f))`; `modelload::loadBaseTextures`, `loadNormalMaps`, optional `loadPbrMaps` | `fox/src/tabs/ModelsTab.cpp:2502-2516`; `fox/src/preview/ModelLoader.h:143-151,198-217` |
| `modelload::buildUploads(model)` → `QVector<GLMeshUpload>`; `buildSkeleton(model)` → `GLSkeletonUpload` | `fox/src/tabs/ModelsTab.cpp:2517-2518`; `fox/src/preview/ModelLoader.h:299,358` (grep lines 99,158 of tail) |
| `m_view->setModel(uploads, textures, skeleton, normalMaps, pbr)` | `fox/src/tabs/ModelsTab.cpp:2527`; `fox/src/gl/GLModelWidget.h:375` |
| `.frdv` help bones via `fileIndexForPath`; `.fcnp` → `setConnectPoints` | `fox/src/tabs/ModelsTab.cpp:2555-2611` |
| panels: `refreshInspector`, `refreshSceneTree`, `refreshInfoPanel`, `refreshAttachments`, outliner `setLoadedModel`, `locateFrig`, `syncAnimPanel` | `fox/src/tabs/ModelsTab.cpp:2615-2628` |

Geometry struct: not a neutral struct — `fox::FmdlFile` (meshes/meshGroups/materials/bones,
`fox/src/fox/FmdlFile.h:102-122`) is converted straight to GPU-shaped `GLMeshUpload{interleaved,indices,joints,weights,materialSlot,groupId,meshId}` (`fox/src/gl/GLModelWidget.h:39-55`). `modelload::load(fileIdx)` returns `LoadedModel{model,textures,normalMaps,pbr,uploads,skeleton}` for non-tab callers (`fox/src/preview/ModelLoader.h:72-86,117`).

**POE2** (synchronous on GUI thread; store owns resolution)

| Hop | Cite |
|---|---|
| `currentChanged` → 110 ms `m_loadDebounce` → `loadCurrentRow()` → `pathAt(row)` → `loadModel(path)`; `activated` → `onListActivated` | `poe2/src/tabs/ModelsTab.cpp:231-234,244-245,427-442` |
| `m_store->loadModel(path, geo, &err)`: `readFile(path)` → `MeshParser::parseSmd/parseFmt` → `ModelGeometry`; `.sm` descriptor via `findSmForSmd` → `AssetText::parseSm` attaches per-part materials | `poe2/src/tabs/ModelsTab.cpp:448`; `poe2/src/store/AssetStore.cpp:119-133`; `poe2/src/model/MeshParser.h:22-26` |
| `m_store->loadSkeletonFor(path, decodeClips=true, geo.jointPaletteSize())` → `AstSkeleton::Skeleton{bones,clips}` | `poe2/src/tabs/ModelsTab.cpp:452`; `poe2/src/store/AssetStore.h:66`; `poe2/src/model/AstSkeleton.h:36-39` |
| `m_view->setModel(geo, skel)` | `poe2/src/tabs/ModelsTab.cpp:453`; `poe2/src/gl/GLModelWidget.h:34` |
| `m_store->resolveMaterials(geo, decodeTextures=true)` → `QVector<GlbExporter::ExportMaterial>` (same resolver the exporter uses) → mapped to `GLModelWidget::MaterialTextures` → `setMaterialTextures` | `poe2/src/tabs/ModelsTab.cpp:456-469`; `poe2/src/store/AssetStore.cpp:374-376`; `poe2/src/model/GlbExporter.h:20-38` |
| `refreshAnimBar`, `rebuildPartsPanel(geo)`, `refreshInfo(geo)`, `refreshAttachments`, `frameAll` | `poe2/src/tabs/ModelsTab.cpp:471-475` |

Geometry struct: `ModelGeometry{vertices,indices,parts[MeshPart{name,material,indexStart,indexCount,materialIndex}],joints,materialPaths,bbox,sourcePath,formatVersion,skinned}` (`poe2/src/model/ModelGeometry.h:46-102`).

### 2.2 Textures: row → decoded image → widget

| | D4 | FOX | POE2 |
|---|---|---|---|
| row → handler | `currentRowChanged` → `onSelectionChanged` → `entryAt` → `showTexture(sno,name)` (`d4/src/tabs/TexturesTab.cpp:865-866,2360-2366`) | `currentChanged` → `m_preview->showFile(fi)` + `m_panel->showTexture(fi)` (`fox/src/tabs/TexturesTab.cpp:725-729`) | `currentChanged` → debounce → `loadCurrentRow` → `loadTexture(path)` (`poe2/src/tabs/TexturesTab.cpp:120-123,211-222`) |
| meta | `texMetaFor(reader,d4,name,sno)`: d4data `.tex.json` else CASC `TextureDefTable` keyed on `payloadSourceSno` (`d4/src/tabs/TexturesTab.cpp:3113-3141`) | `.ftex` header parse `FtexFile::parse` (`fox/src/preview/PreviewPane.cpp:586-589`) | `DdsImage::probe(dds)` (`poe2/src/store/AssetStore.cpp:214-219`) |
| bytes | `reader->readPayloadBySno(sno)` (`d4/src/tabs/TexturesTab.cpp:2455`) | `extract::assembleFtexToDds(f)` — joins `.N.ftexs` stream siblings across archives (`fox/src/preview/PreviewPane.cpp:580`; `fox/src/util/Extract.h:48-49`) | `readFile(ddsPath)` (`poe2/src/store/AssetStore.cpp:216`) |
| decode | GPU: `m_preview->setTexture(slice,w,h,eTexFormat)` (BC upload) in `uploadFaceMip`; CPU: `BcDecode::decode` for export/thumbs (`d4/src/tabs/TexturesTab.cpp:2893,3143-3155`; `d4/src/tex/BcDecode.h:12`) | CPU: `fox::bc::decodeDds(m_lastDds)` (`fox/src/preview/PreviewPane.cpp:584`) | CPU: `DdsImage::decode(dds)` via bcdec (`poe2/src/store/AssetStore.cpp:219`; `poe2/src/tex/DdsImage.h:130`) |
| widget | `GLTextureWidget` (`d4/src/tabs/TexturesTab.h:226`) | `PreviewPane::showImagePage(img, caption)` (`fox/src/preview/PreviewPane.cpp:598`) | `GLTextureWidget::setImage(img)` (`poe2/src/tabs/TexturesTab.cpp:229`) |
| thumbnails | `QThreadPool` decode per visible cell (`d4/src/tabs/TexturesTab.cpp:1398`) | `TexThumbCache` worker QThread (`fox/src/index/TexThumbCache.h:169-214`) | `ThumbnailCache` background mode (`poe2/src/util/ThumbnailCache.h:5-6`) |

### 2.3 Bulk: matched set → per-item export

| | D4 | FOX | POE2 |
|---|---|---|---|
| matched set | `computeMatches()` → `matchesFor()` → `ModelsTab::queryEntries(kModelGroup, FilterSpec)` (models) / inline loop over `entries(kTextureGroup)` with `QueryTerm::matches` (textures) (`d4/src/tabs/BulkExtractorTab.cpp:609-675`; `d4/src/tabs/ModelsTab.cpp:4420-4513`) | `matchedFiles()`: walks `ArchiveIndex::files()` with ext/named/container filters + `fox::queryMatchesFile(q,i,f)` (`fox/src/tabs/BulkExtractorTab.cpp:628-646`) | `m_model->applyFilters(query, ext, facets, matchAny)` on a second `AssetListModel` → rows → `ExportLayout::Item` list (`poe2/src/tabs/BulkTab.cpp:187,209`) |
| run thread | detached `std::thread` in `doExtract`; per-folder passes; `BatchSink` marshals progress/log via `invokeMethod(QueuedConnection)` (`d4/src/tabs/BulkExtractorTab.cpp:1446-1478,1530-1554`) | detached `std::thread` workers over atomic cursor in `startExtraction` (`fox/src/tabs/BulkExtractorTab.cpp:973-1191`) | `BulkExtractor` QObject moved to a `QThread`; `run()` spawns `std::thread` pool + coordinator loop (`poe2/src/tabs/BulkTab.cpp:234-238`; `poe2/src/bulk/BulkExtractor.cpp:117-267`) |
| per-item export fn (models) | `ModelsTab::bulkExport(items,dir,onlyNew,sink)` → `exportModels(...)` → `processItem`: `readMetaBySno`+`readPayloadBySno` → `ModelParser::parseApp` → `appearancePalette` → `buildExportMats` → `ModelExporter::exportGlb` (`d4/src/tabs/ModelsTab_Export.cpp:744-789,980-1093`) | in-worker: `modelload::load(fileIdx)` → `glb::ScenePart` → `fox::writeModelFile` (`fox/src/tabs/BulkExtractorTab.cpp:1035-1052`) | `BulkExtractor::exportModel`: `store->loadModel` → `loadSkeletonFor` → `resolveMaterials` → `GlbExporter::write` (`poe2/src/bulk/BulkExtractor.cpp:45-52`) |
| per-item export fn (textures) | `TexturesTab::bulkExportTextures`: `texMetaFor` + `readPayloadBySno` → `BcDecode::decode` → `img.save` (`d4/src/tabs/TexturesTab.cpp:1581-1680`, decode at rel. 57) | `exportactions::writeFtexPng(fileIdx,path)` or `assembleFtexToDds` → `writeOut` (`fox/src/tabs/BulkExtractorTab.cpp:1031-1060`) | `BulkExtractor::exportTexture`: `store->loadTexture` → `img.save(PNG)` (`poe2/src/bulk/BulkExtractor.cpp:55-59`) |
| raw originals | buffers pass (`bulk/buffers`) (`d4/src/tabs/BulkExtractorTab.cpp:1524`) | default mode: raw bytes via `extract::relativePathFor` (`fox/src/tabs/BulkExtractorTab.cpp:997-1007`) | `exportRaw` mirrors game paths from `collectAssetFiles` (`poe2/src/bulk/BulkExtractor.cpp:68`; `poe2/src/store/AssetStore.h:82`) |

---

## 3. Background index shape (§2)

Template elements: (a) detached thread, (b) fingerprint-signed disk cache, (c) `install()` via
`Qt::QueuedConnection`, (d) `readyChanged` the UI repopulates on, (e) `reset()` wired to the
fingerprint change, (f) generation counter.

| Index | a | b | c | d | e | f | Cites |
|---|---|---|---|---|---|---|---|
| D4 `SnoIndex` | built on MainWindow's reload thread (**HUNCH**, not read) | yes: `loadFromCache(sig)` / `saveToCache`, sig = `buildAndKeySignature` | n/a (plain class, no signal) | via tab `refresh()` | `clear()` | no | `d4/src/index/SnoIndex.h:26-28`; `d4/src/casc/CascReader.h:174-180` |
| D4 `AppearanceMeta` | yes, detached | yes: `appearance_meta_v23.json` checked on appCount+namedCount+dadSig+buildSig | yes | yes | yes (`reset()` deletes cache; MainWindow calls it on fingerprint change) | **no** | `d4/src/index/AppearanceMeta.cpp:970-1010,904-931`; `d4/src/app/MainWindow.cpp:1503-1520` |
| D4 `AssetLinks` | yes | yes (`asset_links_v1.bin`) | yes | yes | yes | **no** | `d4/src/index/AssetLinks.cpp:105-119,223-225`; `d4/src/util/CacheVersioning.h:36` |
| D4 `IconIndex` | yes | yes | yes | yes | yes | **no** | `d4/src/index/IconIndex.cpp:81-117,235-237` |
| D4 newer indexes (BackTrophy, StoreProduct, WardrobeAnim) | yes | yes | yes | yes | yes | **yes** | `d4/src/index/BackTrophyIndex.cpp:106,129,189,302`; `StoreProductIndex.h:181-182` |
| D4 fingerprint | `buildId | d4dataDir | d4dataSignature | tactKeyCount` stored in `index/fingerprint`; change → `reset()` cascade | | | | | | `d4/src/app/MainWindow.cpp:1503-1531` |
| FOX `ArchiveIndex` | yes (`std::thread` detached) | yes (`fox_index_v5.bin`, fingerprint = every archive path/size/mtime + loose counts) | yes (`install(result, generation)` queued) | `readyChanged(bool)` | rebuild() supersedes; caches drop on `fingerprint()` mismatch | yes (`m_generation` + `m_installGeneration`) | `fox/src/index/ArchiveIndex.cpp:492-497,978-988,1335-1342,1378`; `.h:16-19,212-229,266-267` |
| FOX `TextureUsers` / `RefIndex` | yes | yes (`fox_texusers_v1.bin` / `fox_refs_v1.bin` against `ArchiveIndex::fingerprint()`) | publish under mutex + `finished` signal | `finished(bool)` | `reset()` | yes | `fox/src/index/TextureUsers.cpp:124-135,167,170,228,278`; `fox/src/index/RefIndex.h:240-243,310` |
| FOX `ModelTags` / `NameCatalog` | **no** — rebuilt lazily on GUI thread, invalidated by `files().constData()` address | no | n/a | n/a | implicit | address key | `fox/src/index/ModelTags.cpp:47-60`; `fox/src/index/NameCatalog.h:206-207` |
| FOX `TexThumbCache` / `GameFilter` | worker QThread / n/a | no | n/a | `ready` | `reset()` | yes (`m_generation`) | `fox/src/index/TexThumbCache.h:213`; `fox/src/index/GameId.h:346-354` |
| POE2 `IndexLoader` → `AssetStore::open` | `QThread::create` | yes: `BundleIndex` cache keyed `size:mtime` of `_.index.bin`; `MaterialFamilyIndex` / `NameIndex` caches keyed on that fingerprint + `kCacheVersion` | **no install()** — worker emits `finished`/`materialsReady`, cross-thread auto-queued; store mutated *on the worker* (**HUNCH:** safe only because tabs read after `finished`) | `finished` → `onIndexReady`, `materialsReady` → `onMaterialsReady` | `start()` restarts; no explicit reset-on-fingerprint | yes | `poe2/src/store/IndexLoader.cpp:11-33`; `poe2/src/bundle/BundleIndex.h:12-13,51-52`; `poe2/src/store/NameIndex.h:24-30`; `poe2/src/app/MainWindow.cpp:38-45,193-195` |

Notes: the D4 comment at `d4/src/app/MainWindow.cpp:1029-1031` claims "generation counters exist"
for AppearanceMeta/IconIndex, but neither class has one (grep of `generation` in
`d4/src/index/AppearanceMeta.cpp`, `AssetLinks.cpp`, `IconIndex.cpp` returns nothing). The
`m_building` guard is what prevents double-kicks (`AppearanceMeta.cpp:935`).

---

## 4. Filtering: one matcher, facets, live count, chips

| Tab | Goes through the shared matcher? | Facets/funnel | Live count | Chips |
|---|---|---|---|---|
| D4 Models list | yes: `SnoListModel::rebuild` → `QueryTerm::matches` (`d4/src/index/SnoListModel.cpp:135-144`) | funnel `m_tagPanel` stays open; groups from `filterTagGroups()`; usage facets animated/rigged/orphaned/latest; only-decrypted/encrypted; hide un-renderable (`d4/src/tabs/ModelsTab.h:100,354-355,445-450,458`; `.cpp:4320-4380`) | `updateCount()` "N of M" (`d4/src/tabs/ModelsTab.cpp:3587-3600`) | `rebuildFilterChips()` (`d4/src/tabs/ModelsTab.cpp:3631`) |
| D4 Bulk | yes: models via `queryEntries`; textures via `QueryTerm::matches` (`d4/src/tabs/BulkExtractorTab.cpp:651-652,674`) | copy of the Models funnel (`buildTagPanel`, texture categories in texture mode) (`.cpp:678-843`) | `updateCount()` also counts already-present (`.cpp:942-960`) | **HUNCH** no chip row (only funnel tint `updateTagBtn` `.cpp:893`) |
| D4 Textures | **NO**: `applyNameFilter` predicate uses `b.contains(t)` / `meta.contains(t)` (`d4/src/tabs/TexturesTab.cpp:2029-2035`) — `a|b` silently fails | funnel `m_filterPanel`: tags (appearance-derived blob), categories, formats (BC1/3/4/5/7), orphan, only-encrypted (`d4/src/tabs/TexturesTab.h:164-188`) | `updateSelLabel` (`.cpp:2227`) | `rebuildFilterChips` (`.cpp:1906`) |
| FOX Models | yes: `searchq::Query` → `fox::queryMatchesFile` → `QueryTerm` (`fox/src/tabs/ModelsTab.cpp:103,121`; `fox/src/util/SearchQuery.h:95-108`) | `TagFunnel` (tags **are** the search text `#tag`; OR within category, AND across) (`fox/src/util/TagFunnel.h:12-16`; `fox/src/index/ModelTags.h:30-34`) | `filterSummary()` "1,204 of 38,875 models · 3 tags" (`fox/src/tabs/ModelsTab.h:346-347`) | `TagFunnel::chips()` (`fox/src/util/TagFunnel.h:33`) |
| FOX Textures | yes: `queryMatchesFile` (`fox/src/tabs/TexturesTab.cpp:95` in `TexListModel::refresh`) | `FilterPopup` hosts Named / game toggles / used-orphan / user-tag / format combos (not tags) (`fox/src/view/FilterPopup.h:1-15`; `fox/src/tabs/TexturesTab.h:199-208`) | `m_count`, `matchCount()` (`.h:146,218`) | `FilterChips` (`.h:204`) |
| FOX Files | yes: `searchq::Query` + `queryMatchesFile` (`fox/src/tabs/FilesTab.cpp:492-504`) | none | — | — |
| FOX Bulk | yes (`fox/src/tabs/BulkExtractorTab.cpp:628,646`) | `TagFunnel` + ext combo + named-only + containers | `updateMatchCount` (`.h:103`) | `TagFunnel::chips` |
| POE2 Models | yes: `AssetListModel::rebuild` → `QueryTerm::parse/test` (`poe2/src/index/AssetListModel.cpp:103,130`) | `FunnelFilter` popup stays open; groups Category / Item type / Shader; Match-any; per-facet live counts (`poe2/src/index/FunnelFilter.h:10-30`; `poe2/src/tabs/ModelsTab.cpp:421-424`) | "N of M models" (`poe2/src/tabs/ModelsTab.cpp:424`) | `FunnelFilter::chipBar()` (`FunnelFilter.h:31-34`) |
| POE2 Textures | yes (`setQuery` on the same model) (`poe2/src/tabs/TexturesTab.cpp:207`) | **none** (no FunnelFilter member in `TexturesTab.h`) | "N of M textures" (`.cpp:208`) | none |
| POE2 Bulk | yes (`applyFilters`) (`poe2/src/tabs/BulkTab.cpp:187`) | `FunnelFilter` minus Shader group (`FunnelFilter.h:21-23`) | yes | yes |

Startup self-tests exist in all three: `d4/src/main.cpp:232`, `fox/src/main.cpp:133-139` (QueryTerm
and searchq), `poe2/src/main.cpp:56-59`. FOX and POE2 QueryTerm add an id-term form (64-bit hash
hex/decimal) D4 lacks (`fox/src/util/QueryTerm.h:47`; `poe2/src/util/QueryTerm.h:31-40`).

---

## 5. Bulk: feature comparison

| Feature | D4 | FOX | POE2 |
|---|---|---|---|
| Queue ("pick manually") | yes, per mode, group-tagged `Item{group,sno,name}`, persisted (`d4/src/tabs/BulkExtractorTab.h:55-59,177-181`) | yes, stored as **hex hashes** so a rescan cannot misfile (`fox/src/tabs/BulkExtractorTab.h:113-121,167`) | **no** |
| Only-new manifest | `_bulk_manifest.json` in `ModelsTab::bulkExport` / `TexturesTab::bulkExportTextures` (`d4/src/tabs/ModelsTab_Export.cpp:756-771,797-805`) | `fox::BulkManifest`, keyed on **hash + relative path** (`fox/src/export/BulkLedger.h:1-40`) | `_bulk_manifest.json` (`poe2/src/bulk/BulkExtractor.cpp:25`) |
| Parallel workers | `std::thread` pool in `exportModels`, `bulk/parallel` (auto=ideal, cap 16); **forced serial when anims/base-body on** (`d4/src/tabs/ModelsTab_Export.cpp:1110-1141`); texture pool in `bulkExportTextures` | `m_workers` spinbox, 0=auto; `std::thread` over atomic cursor (`fox/src/tabs/BulkExtractorTab.h:153`; `.cpp:973`) | `Options::workers` 0=auto clamp [1,32]; pool + coordinator loop (`poe2/src/bulk/BulkExtractor.h:25-26,40`; `.cpp:117-267`) |
| Pause / ETA excluding pause | yes: `sink->canceled` doubles as pause gate; ETA from active time (`d4/src/tabs/BulkExtractorTab.cpp:1449-1478`) | yes: `m_pausedMs`/`m_pauseStart` (`fox/src/tabs/BulkExtractorTab.h:178-183`; `.cpp:1204-1222,1278-1293`) | yes: coordinator `pausedAccum` (`poe2/src/bulk/BulkExtractor.cpp:226-256`) |
| Failures file | `_bulk_failed.txt` with reasons (`d4/src/tabs/ModelsTab_Export.cpp:806-816`) | `fox::BulkFailureLog` (`fox/src/tabs/BulkExtractorTab.h:188`; `.cpp:1251`) | `_bulk_failed.txt` (`poe2/src/bulk/BulkExtractor.cpp:26`) |
| Cancel / Esc | `QShortcut(Key_Escape)` + atomic `m_cancelRequested` polled between items (`d4/src/tabs/BulkExtractorTab.cpp:507-509`; `.h:201`) | `keyPressEvent` Esc only while running (`fox/src/tabs/BulkExtractorTab.cpp:481`) | `keyPressEvent` Esc (`poe2/src/tabs/BulkTab.cpp:196`) |
| Run-scoped decode cache | `MaterialDecode::TextureCacheScope` — shared across threads, opt-in per thread, LRU MB budget (`d4/src/model/MaterialDecode.h:49-113`; opened per run `d4/src/tabs/BulkExtractorTab.cpp:1582`, per worker `ModelsTab_Export.cpp:1128`) | `extract::blobcache::Scope` — decompressed archive-entry blobs, owned by the tab (`fox/src/util/Extract.h:71-110`; `fox/src/tabs/BulkExtractorTab.h:191-196`) | **none** (bundle-handle LRU in `AssetStore` is not run-scoped) (`poe2/src/store/AssetStore.h:128-130`) |
| Layout | `ExportLayout` Flat/Class/Type/Model, groups computed on GUI thread, one pass per folder (`d4/src/tabs/BulkExtractorTab.cpp:303-306,1518-1554`) | `ExportLayout::folderForFile` with tag ranks (`fox/src/tabs/BulkExtractorTab.cpp:852-854,937-940,988-998`) | `ExportLayout::dirFor` Flat/Type/Folder/_model (`poe2/src/bulk/BulkExtractor.cpp:167`; `poe2/src/util/ExportLayout.h:37-40`) |
| Name templates | `NameTemplate::model/texture(name,sno)` (`d4/src/util/NameTemplate.h:28-33`) | `ExportOptions::nameTemplate` `{{Name}}` + `applyNameTemplate` (`fox/src/export/ExportOptions.h:51-61,241`) | `NameTemplate` ported from D4 (`poe2/src/util/NameTemplate.h:28-33`) |
| Modes | Models / Textures / **Both** in one queue (`d4/src/tabs/BulkExtractorTab.h:65`) | any extension (ext combo) + containers toggle | models + textures checkboxes + raw originals (`poe2/src/bulk/BulkExtractor.h:34-39`) |
| Factory presets | yes, generated from hero-class table, audited to `preset_audit.txt` (`d4/src/tabs/BulkExtractorTab.h:97-123`) | `applyPreset(query, ext)` from Export menu; saved queries (`fox/src/tabs/BulkExtractorTab.h:51,68-74`) | no |
| Co-textures / deps / buffers / report | name-matched textures, `deps/`, buffers, run report (`d4/src/tabs/BulkExtractorTab.cpp:1522-1525,1556-1559`; `.h:127`) | dry run to `_bulk_dryrun.txt`; degraded-write not recorded in manifest (`fox/src/tabs/BulkExtractorTab.h:75-78`; `.cpp:1053-1075`) | raw-mode dependency closure (`poe2/src/store/AssetStore.h:76-82`) |
| Headless harness | `auditPresets()` | `configureRun/startRun/runFinished` (`fox/src/tabs/BulkExtractorTab.h:59-85`) | verified via `tools/bulk_verify.cpp` (per `poe2/CLAUDE.md`) |
| Fault guard per item | `seh::runGuarded("bulk-export")` (`d4/src/tabs/ModelsTab_Export.cpp:989-1104`) | none seen (**HUNCH**: relies on parsers not crashing) | parsers throw bounded Underrun (`poe2/CLAUDE.md` rule 3) |

**Superset:** FOX for run *machinery* (hash-keyed queue, path-aware manifest, dry run, saved
queries, degraded rule, harness); D4 for *what a run can mean* (Both mode, presets, co-textures,
deps, buffers, per-folder passes, shared decode cache). POE2 is the smallest and the cleanest
separation: `BulkExtractor` is a standalone QObject that takes `AssetStore*` + items + options
(`poe2/src/bulk/BulkExtractor.h:44`).

**Game-coupled in each:** D4 — bulk is *delegated to the tabs* (`m_models->bulkExport`,
`m_textures->bulkExportTextures`, `d4/src/tabs/BulkExtractorTab.cpp:1536-1537`), so the run
depends on `ModelsTab` internals (roster, anims, base-body); FOX — extension-name dispatch
(`isFtex`, `.fmdl`) and `ftexs` sibling assembly inside the worker (`fox/src/tabs/BulkExtractorTab.cpp:1031-1056`);
POE2 — `exportModel/exportTexture/exportRaw` call seven `AssetStore` methods (`loadModel`, `loadSkeletonFor`, `resolveMaterials`, `loadTexture`, `collectAssetFiles`, `readFile`, `index`) and nothing else
(`poe2/src/bulk/BulkExtractor.cpp:45-68`), which is already the plugin-shaped version.

---

## 6. What the panels need from the loaded model

| Panel | D4 | FOX | POE2 |
|---|---|---|---|
| PARTS | per primitive: label (`partLabel`), tri count (`indices.size()/3`), slot (`slotLabelForHash(slotHash)` or "cloth" via `m_clothMats`), material name (`m_appMatNames[materialIndex]`), visibility (`partVisible`), FX/SIM/GIB class flags (`d4/src/tabs/ModelsTab.cpp:8642-8660`; flags `.h:622-623`) | `SceneTree::Node{label,meshId,tris,materialSlot,material,tip,children}` built from `meshGroups()` → `meshes()` → `materials()[idx].name` (`fox/src/util/SceneTree.h:36-56`; `fox/src/tabs/ModelsTab.cpp:2350-2379`) | `MeshPart{name, indexCount/3, material file name}` (`poe2/src/tabs/ModelsTab.cpp:599-611`) |
| MATERIALS | roster from `.app.json` `ptAppearanceMaterials[].ptSOAs[]` (hash, snoMaterial, snoCloth, override, flags) + per-look names `m_soaNames`; Values / Shaders tables from `.mat.json` (`d4/src/tabs/ModelsTab.cpp:6303-6357`; `.h:575-590`) | `MaterialInspector::Source{label, MaterialEntry{name,shader,textures,params,uses}, base, normals, pbr, slotBase, gz, install}` (`fox/src/preview/MaterialInspector.h:50-93`) | material path list in INFO; `MaterialReport` for detail (`poe2/src/tabs/ModelsTab.cpp:622-626`; `.h:65`) |
| TEXTURE PREVIEW / channel tiles | six tiles decoded by role via `textureByRole/baseColorForMaterial/normal/orm/emissive` (`d4/src/tabs/ModelsTab.h:162-171`) | material cards hold thumbnails (inspector) + `ChannelStrip` on Textures tab (`fox/src/tabs/TexturesTab.h:209`) | channel combo on the viewport; no tile strip (`poe2/src/tabs/ModelsTab.h:87`) |
| INFO | Filename, SNO, Title, Collection, Tags, Sets, Filesize (`payloadSize`), Format, Materials, Textures, Animations, Bounds/LODs/Bones (from `.app.json tStructure`), Actor/Family/Used by/Items (entity index), Sold in, Physics (`d4/src/tabs/ModelsTab.cpp:6122-6171,6229-6262`) | sections FILE (name, path, size, game, archive, tags), MODEL (FMDL version, groups, submeshes, tris, verts, materials, bones, normal maps), SIBLINGS (.frdv/.fcnp/.mtar), SELECTED SUBMESH (index, group, material, shader, texture slots, tris, verts, bone palette) (`fox/src/tabs/ModelsTab.cpp:2827-2967`) | name, path, format/version/vertexFormat, parts/verts/tris/skinned, materials list (`poe2/src/tabs/ModelsTab.cpp:613-627`) |
| ANIMATIONS | clip rows from own/family/base-rig/AnimSet indexes; decode `AnimParser::DecodedAnim{frameRate,frameCount,bones}` on play (`d4/src/tabs/ModelsTab.cpp:5665`; `d4/src/model/AnimParser.h:23-29`) | `.mtar` archive → `MtarFile::clips()` → `GaniAnim`; rig from sibling `.frig`; `AnimationsPanel` + shared `AnimTransport` (`fox/src/tabs/ModelsTab.h:435-461`) | `AstSkeleton::Skeleton::clips` (only when rig covers palette); clip bar + list (`poe2/src/tabs/ModelsTab.cpp:304-350`; `poe2/src/model/AstSkeleton.h:29-39`) |
| ATTACHMENTS | `ModelAttach::Attachment` scanned from Actor JSON, seated by hardpoint map (`d4/src/tabs/ModelsTab.h:226-242,626-642`) | `.fv2` attachments via `modelload::fovaAttachedModels` → `AttachmentsPanel` (`fox/src/preview/ModelLoader.h:88-117`; `fox/src/tabs/ModelsTab.cpp:2975`) | `AssetStore::attachmentsForModel(bodySmd)` → pieces {bone, aoPath, smdPath, label} (`poe2/src/store/AssetStore.h:84-102`; `poe2/src/tabs/ModelsTab.cpp:352`) |
| engine extras | CLOTH page (`.clt.json` tuning) (`d4/src/tabs/ModelsTab.cpp:8681`), Vertex Buffers, SubObject Apps, Looks, Dye/Pigment | RENDER panel, material debug panel, help bones `.frdv`, connect points `.fcnp` | alpha-mode per material (`GlbExporter::ExportMaterial::alphaMode`) |

Common denominator every project fills: **parts (name, tris, material name, visibility)**,
**materials (name, shader/family, texture roles → images)**, **info (id, name, path, size, counts,
bones)**, **clips (name, frames/duration)**, **attachments (label, parent bone, mesh)**.

---

## 7. The plugin interface the core would need

Derived from the three chains above. Pure-virtual sketch; each method names the existing
function that would sit behind it. "—" = would return unsupported / empty.

```cpp
namespace core {

struct AssetId { quint64 id; quint16 kind; };          // D4: (sno, group)  FOX/POE2: (hash, ext)
struct AssetRow {
    AssetId id; QString name; QString inGameName; QString collection;
    quint64 size; QStringList tags; quint32 flags;      // Encrypted|Unrenderable|Animated|Rigged|Orphaned|New|Shadowed|Unnamed
};
struct TagGroup { QString id, label; QVector<QPair<QString,int>> tags; };   // grouped facets + counts
struct Geometry;  struct Skeleton;  struct Clip;  struct Material;          // core neutral structs (§7.2)

class IGameStore {
public:
    virtual ~IGameStore() = default;
    // ── index / lifecycle
    virtual bool   open(const QString& dataDir, QString* err, ProgressFn) = 0;
    virtual QString fingerprint() const = 0;                       // signs every derived cache
    virtual void   buildSecondaryIndexes(ProgressFn) = 0;          // meta/tags/names/links, worker thread
    // ── rows
    virtual QVector<quint16> kinds() const = 0;                    // which list tabs exist
    virtual int    rowCount(quint16 kind) const = 0;
    virtual AssetRow row(quint16 kind, int i) const = 0;           // cheap; flags may be lazily filled
    virtual QString searchBlob(AssetId) const = 0;                 // '#' metadata haystack
    virtual QVector<TagGroup> tagGroups(quint16 kind) const = 0;
    virtual bool   hasTag(AssetId, const QString& tag) const = 0;
    // ── bytes
    virtual QByteArray open(AssetId, QString* err) = 0;            // raw decoded bytes
    virtual QStringList dependencyFiles(AssetId) = 0;              // raw-originals closure
    // ── models
    virtual bool   loadModel(AssetId, Geometry&, QString* err) = 0;
    virtual Skeleton loadSkeleton(AssetId, const Geometry&, bool decodeClips) = 0;
    virtual QVector<Material> resolveMaterials(AssetId, const Geometry&, bool decodeImages) = 0;
    virtual QVector<Attachment> attachments(AssetId) = 0;
    // ── textures
    virtual QImage loadTexture(AssetId, TexInfo* info, QString* err) = 0;
    virtual QVector<Frame> atlasFrames(AssetId, const QImage& decoded) = 0;
    // ── links / icons
    virtual QVector<Use> usedBy(AssetId) = 0;
    virtual QVector<AssetId> uses(AssetId) = 0;
    virtual QPixmap gameIcon(AssetId, int px) = 0;
    virtual QVector<AssetId> variants(AssetId) = 0;
    // ── run-scoped caches
    virtual std::unique_ptr<RunCache> beginRunCache() = 0;        // RAII
};
}
```

### 7.1 Per-method implementation map

| Method | D4 | FOX | POE2 |
|---|---|---|---|
| `open` | `CascReader::open` + `SnoIndex::loadFromCasc/loadFromD4data` (`d4/src/casc/CascReader.h:37`; `d4/src/index/SnoIndex.h:22-23`) | `ArchiveIndex::rebuild(gameDirs, dictDir, deep)` (`fox/src/index/ArchiveIndex.h:115`) | `AssetStore::open(bundlesDir, err, progress)` (`poe2/src/store/AssetStore.h:33`) |
| `fingerprint` | `CascReader::buildAndKeySignature()` + d4data signature (`d4/src/casc/CascReader.h:180`; `MainWindow.cpp:1503-1506`) | `ArchiveIndex::fingerprint()` (`fox/src/index/ArchiveIndex.h:228`) | `BundleIndex::fingerprint()` (`poe2/src/bundle/BundleIndex.h:52`) |
| `buildSecondaryIndexes` | `AppearanceMeta/AssetLinks/IconIndex::ensureBuilt` + entity/anim scans in `ModelsTab` (`ensureEntityIndex`, `ensureAnimatedIndex` — **tab-owned, must move**) (`d4/src/tabs/ModelsTab.h:243-246`) | `TextureUsers::build`, `RefIndex::build`, `ModelTags` lazy (`fox/src/index/TextureUsers.h:101`) | `buildMaterialIndex` + `buildNameIndex` (`poe2/src/store/AssetStore.h:42,49`) |
| `kinds` | SNO groups 9/44 (+110 StoreProduct) | extension ("fmdl","ftex",…) | ext filter list (`.smd`,`.fmt`,`.dds`) |
| `row` | `SnoIndex::entries(group)[i]` + `AppearanceMeta::titleFor/collectionFor/tagsFor` | `ArchiveIndex::files()[i]` + `gameOf` + `ModelTags::tagsOf` | `BundleIndex::files()[i]` + `pathOf` + `NameIndex::nameFor` |
| `searchBlob` | `ModelsTab::modelSearchBlob` / `TexturesTab::texBlob` (tab-owned) (`d4/src/tabs/ModelsTab.cpp:4265`; `TexturesTab.cpp:1946`) | tags are matched by `ModelTags::satisfies` not a blob (`fox/src/index/ModelTags.h:83-84`) | `AssetListModel::metaFor` → `MaterialFamilyIndex` meta + `NameIndex::searchNamesFor` (`poe2/src/index/AssetListModel.h:127`) |
| `tagGroups` | `ModelsTab::filterTagGroups()` (`d4/src/tabs/ModelsTab.h:100`) | `ModelTags::categories()` (`fox/src/index/ModelTags.h:66`) | `Facets::all()` grouped + `facetCounts()` (`poe2/src/index/Facets.h:54`; `AssetListModel.h:94`) |
| flags: Encrypted | `CascReader::encryptedSnos()` | — | — |
| flags: Unrenderable | `m_renderBlocklist ∪ m_noRenderSnos` (tab-owned) (`d4/src/tabs/ModelsTab.h:362-363`) | — | — |
| flags: Animated/Rigged/Orphaned | tab-owned index sets (`d4/src/tabs/ModelsTab.h:364,393,402`) | — (**HUNCH**: could derive Rigged from a `.frig`/`.mtar` sibling) | — (**HUNCH**: could derive Rigged from `loadSkeletonFor` coverage, but that is a load-time cost) |
| flags: New | `SnoIndex::isNew` (`d4/src/index/SnoIndex.h:159`) | — | — |
| flags: Shadowed/Unnamed | — | `IndexedFile::shadowed/named` | — (`unnamedFiles()` count only) |
| `open(bytes)` | `readPayloadBySno` / `readMetaBySno` (two blobs per asset — **interface needs a "part" selector**) | `ArchiveIndex::readFile(f)` (`fox/src/index/ArchiveIndex.h:162`) | `AssetStore::readFile(path)` (`poe2/src/store/AssetStore.h:52`) |
| `dependencyFiles` | `exportModelDeps` (static in `ModelsTab_Export.cpp:126`) | `exportactions` dependency dialog / `RefIndex::uses` | `AssetStore::collectAssetFiles` (`poe2/src/store/AssetStore.h:82`) |
| `loadModel` | `ModelParser::parseApp(meta, payload, appName)` + roster seat (`d4/src/model/ModelParser.h:21`; `ModelsTab.cpp:8227-8230`) | `FmdlFile::parse` + `modelload::buildUploads` — **needs a FmdlFile→core::Geometry adapter**, today it goes straight to `GLMeshUpload` (`fox/src/preview/ModelLoader.h:299`) | `AssetStore::loadModel(path, geo, err)` (`poe2/src/store/AssetStore.h:58`) |
| `loadSkeleton` | embedded in `ModelGeometry::skeleton` from `parseApp` | `modelload::buildSkeleton(model)` + `.frig` via `locateFrig` (tab) | `AssetStore::loadSkeletonFor(path, decodeClips, minBones)` (`poe2/src/store/AssetStore.h:66`) |
| clips | `ModelsTab::animClipsFor` + `decodeAnimByName` → `AnimParser::decode` (tab-owned, family/AnimSet logic) (`d4/src/tabs/ModelsTab.h:313-329`) | `MtarFile::clips()` + `GaniAnim` per clip, user-chosen archive (`fox/src/tabs/ModelsTab.h:299-300`) | inside `Skeleton::clips` |
| `resolveMaterials` | `buildExportMats(pal, geo, name, d4, reader, wantTex)` (static in `ModelsTab_Export.cpp:84`) + `MaterialDecode::*` for the viewport (`applyPartMaterials`) — **two resolvers today** | `modelload::loadBaseTextures/loadNormalMaps/loadPbrMaps` (`fox/src/preview/ModelLoader.h:143-151,198-217`) | `AssetStore::resolveMaterials(geo, decode)` — one resolver for viewport and .glb (`poe2/src/store/AssetStore.h:61`) |
| `attachments` | `ModelsTab::scanAttachments` (Actor JSON, tab-owned) (`d4/src/tabs/ModelsTab.h:229`) | `modelload::fovaAttachedModels` (`fox/src/preview/ModelLoader.h:112-117`) | `AssetStore::attachmentsForModel` (`poe2/src/store/AssetStore.h:102`) |
| `loadTexture` | `TexturesTab::texMetaFor` + `readPayloadBySno` + `BcDecode::decode` (or `MaterialDecode::texture`) (`d4/src/tabs/TexturesTab.h:59`; `d4/src/model/MaterialDecode.h:46`) | `extract::assembleFtexToDds` + `fox::bc::decodeDds` (`fox/src/util/Extract.h:48`) | `AssetStore::loadTexture(path, err, info)` (`poe2/src/store/AssetStore.h:69`) |
| `atlasFrames` | `TexturesTab::atlasFramesFor` (`d4/src/tabs/TexturesTab.h:72-73`) | mip levels instead (`fox/src/view/TextureInfoPanel.h:12-18`) — unsupported | unsupported |
| `usedBy` | `AssetLinks::linksForTexture` / `appsForMaterial` (`d4/src/index/AssetLinks.h:33,44`) | `TextureUsers::usesOf` / `RefIndex::usedBy` (`fox/src/index/TextureUsers.h:73`; `RefIndex.h:280`) | unsupported (no reverse index exists) |
| `uses` | `showDependencies` tree (tab) (`d4/src/tabs/ModelsTab.h:239`) | `RefIndex::uses` (`fox/src/index/RefIndex.h:279`) | `collectAssetFiles` (forward closure) |
| `gameIcon` | `IconIndex::iconImage(handle, reader)` via `AppearanceMeta::iconFor` (`d4/src/index/IconIndex.h:102`) | `IconCatalog::iconFor(stem, h)` (`fox/src/index/IconCatalog.h:249`) | unsupported (item icon .dds is *named*, not linked to the model — **HUNCH** could be added from `ItemVisualIdentity.DDSFile`) |
| `variants` | `m_apprVariantSnos` (tab) (`d4/src/tabs/ModelsTab.h:414`) | `exportactions::addVariantActions` (`fox/src/util/ExportActions.h:66-77`) | unsupported |
| `beginRunCache` | `MaterialDecode::TextureCacheScope` | `extract::blobcache::Scope` | unsupported (no-op) |

### 7.2 Where the core neutral structs already are

- **Geometry**: D4 `ModelGeometry` (`d4/src/model/ModelGeometry.h`) and POE2 `ModelGeometry`
  (`poe2/src/model/ModelGeometry.h`) are near-isomorphic (vertices/indices/parts/joints/material
  refs). FOX has no such struct; `FmdlFile` → `GLMeshUpload` is the only path. **The FOX adapter is
  the single biggest piece of new code the extraction implies.**
- **Material**: D4 `ModelExporter::ExportMaterial` (`d4/src/model/ModelExporter.h:23-36`) and POE2
  `GlbExporter::ExportMaterial` (`poe2/src/model/GlbExporter.h:20-38`) agree on
  baseColor/normal/ORM/emissive + factors; POE2 adds specularColor/alphaMode/transmission/SSS/fur;
  FOX's `GLPbrMaterial` is SRM/TRM/layer-shaped (`fox/src/gl/GLModelWidget.h:75-100`) — a different
  channel model, so `core::Material` needs an "engine channel bag" escape hatch.
- **Clips**: three unrelated decoders (`AnimParser::DecodedAnim`, `GaniAnim`, `AstSkeleton::Clip`).
  Only the *sampled* form (per-bone TRS keys at time t) is shareable.
- **Viewport contract**: D4 `setGeometry(ModelGeometry)`, POE2 `setModel(ModelGeometry, Skeleton)`,
  FOX `setModel(uploads, textures, skeleton, normals, pbr)` (`d4/src/gl/GLModelWidget.h:40`;
  `poe2/src/gl/GLModelWidget.h:34`; `fox/src/gl/GLModelWidget.h:375`). A core viewport would take
  `core::Geometry` and per-material `core::Material` — closest to POE2 today.

### 7.3 Threading contract the interface must state

D4 calls `readMetaBySno/readPayloadBySno/parseApp` from a detached thread and bulk workers
(`d4/src/tabs/ModelsTab.cpp:8154`; `ModelsTab_Export.cpp:1138`); FOX calls `readFile`/`modelload`
from the thumbnail thread and bulk workers (`fox/src/gl/ThumbnailRenderer.cpp:167`;
`fox/src/tabs/BulkExtractorTab.cpp:973`); POE2 documents `AssetStore` reads as thread-safe
(`poe2/src/store/AssetStore.h:25-26`; `BulkExtractor.h:19-24`). But D4 `SnoIndex::nameForSno` is
GUI-thread only (`d4/src/index/SnoIndex.h:39-48`) and FOX `ModelTags/NameCatalog` are lazily
rebuilt on first use (`fox/src/index/ModelTags.cpp:47-60`). **The interface must say: `open/
loadModel/loadTexture/resolveMaterials` are thread-safe; `row/tagGroups/searchBlob` are GUI-thread.**

---

## 8. Game-specific tabs: shared services consumed

| Tab | Viewport | Panels | Menus | Export | Store/index |
|---|---|---|---|---|---|
| D4 Wardrobe (`WardrobeTab2.cpp` + `_Panels.cpp`) | `GLModelWidget` ×22 (first `d4/src/tabs/WardrobeTab2.cpp:20`) | `PanelBox` ×12 (`:4`), `HoverInfo` (`:49`), `TextReportDialog` (`:41`) | `ViewportPartMenu` ×7 (`:2`) | `ModelExporter::exportGlb/optionsFromSettings/ExportMaterial` (`:4394,4385,4410`) | `readPayloadBySno/readMetaBySno` (`:3326,3551`), `ModelParser::parseApp/parseClothOnly/resolveClothTuning/mergeGeometries` (`:4426,10493,10377,4965`), `MaterialDecode::*` (18 distinct fns, first `:192`), `AppearanceMeta`, `IconIndex` (`:2874,2939`) |
| D4 Stable (`StableTab2.cpp`) | `GLModelWidget` ×27 (`:18`) | `PanelBox` ×13, `HoverInfo`, `TextReportDialog` | `ViewportPartMenu` ×10 (`:4`) | `ModelExporter::*` (`:2917,2904`), `MaterialDecode::TextureCacheScope` (`:2840`) | `parseApp/mergeGeometries` (`:2171,2206`), `readPayloadBySno` (`:2165`), `MaterialDecode::*`, `AppearanceMeta`, `IconIndex` |
| D4 Catalogue (`CatalogueTab.cpp`) | none | `HoverInfo` ×12, `CsvCopy` ×6 (`:18-19`) | `ViewportPartMenu` ×10 (`:24`) | delegates to `ModelsTab::bulkExport` with `BatchSink` (`:967,15`), `TexturesTab::texMetaFor/atlasFramesFor` (`:2328,3285`) | `IconIndex`, `AppearanceMeta`, `MaterialDecode::texture` (`:2402`) |
| FOX Customize (`CustomizeTab.cpp`, `_Weapon.cpp`, `_Build.cpp`) | `GLModelWidget` (`fox/src/tabs/CustomizeTab.cpp:35`) | `SceneTree` ×14, `MaterialInspector` ×8, `InfoPanel`, `NPanel`, `AnimationsPanel`, `AnimTransport` (`:178,284,299,262,340,36`) | `partmenu` (`:196`), `exportactions` ×4 (`:288`) | `extract::*` (`:1139`), `fox::writeModelFile` (`:2553`) | `modelload::*` ×59 (`:799`), `ArchiveIndex` ×25, `IconCatalog`, `EquipCatalog` ×39 + `NameCatalog` + `ThumbnailRenderer` in `_Weapon.cpp` (`:59,117,60`) |
| FOX Files (`FilesTab.cpp`) | via `PreviewPane` (`:51,682`) | `InfoPanel`, `NPanel` (`:45-46`) | `exportactions::addFileActions/addFileSetActions/extractSet` (`:816-936`) | `extract::relativePathFor` (`:275`) | `ArchiveIndex` ×11, `searchq::Query` + `queryMatchesFile` (`:492-504`) |
| POE2 Customize (`CustomizeTab.cpp`) | `GLModelWidget` shared (`poe2/src/tabs/CustomizeTab.h:29,97`) | none of the Models panels (own tables) | — | `GlbExporter::write` (`poe2/src/tabs/CustomizeTab.cpp:1149`) | `m_store->loadModel/loadSkeletonFor/resolveMaterials/readFile/filesUnderPrefix/index` (`:555,557,968,267,1206,347`) |

Observation: the D4 showpiece tabs reach *around* the Models tab into `ModelParser`,
`MaterialDecode`, `CascReader` directly, i.e. they already program against the store layer the
interface in §7 names; POE2's Customize does the same against `AssetStore`. FOX's Customize
programs against `modelload` (the GPU-shaped loader), which again points at the FOX adapter as
the missing piece.

---

## 9. Defects and risks noticed on the way (not exhaustive)

1. **D4 Textures tab bypasses `QueryTerm`** — `d4/src/tabs/TexturesTab.cpp:2029-2035`. Real bug;
   `a|b` OR silently returns nothing there while Models/Bulk honour it.
2. **D4 `ensureFilterIndexes`/animated/rigged/orphaned/blocklist state lives in `ModelsTab`**, so
   `BulkExtractorTab` must hold a `ModelsTab*` to filter (`d4/src/tabs/BulkExtractorTab.h:33,143`).
   For the plugin interface these become store-level indexes (§7.1 rows "flags").
3. **FOX `.fcnp` lookup is a linear scan** over all files on every model load
   (`fox/src/tabs/ModelsTab.cpp:2474-2480`) although `fileIndexForPath` exists and is used four lines
   later for `.frdv` (`:2561`). **HUNCH:** oversight; comment at `ArchiveIndex.h:200-209` says six such scans were removed.
4. **FOX and POE2 load synchronously on the GUI thread**; D4's worker + token + SEH pattern
   (`d4/src/tabs/ModelsTab.cpp:8127-8182`) is the one to lift into the core.
5. **D4 has two material resolvers** (viewport `applyPartMaterials` vs export `buildExportMats`);
   POE2 has one (`resolveMaterials`) feeding both — the template's "one source of truth" point.
6. **D4 older singletons lack the generation counter** the MainWindow comment assumes
   (`d4/src/app/MainWindow.cpp:1029-1031`).
7. **POE2 mutates `AssetStore` on the worker thread** and relies on signal ordering; no `install()`
   swap (`poe2/src/store/IndexLoader.cpp:15-31`). Works, but it is not the §2 shape.
8. **D4 bulk parallelism is disabled when animations or base-body are on**
   (`d4/src/tabs/ModelsTab_Export.cpp:1117-1121`) because those paths read GUI state — a store-level
   `clips()` would remove that constraint.
