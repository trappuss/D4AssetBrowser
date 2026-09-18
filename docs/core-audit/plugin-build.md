# Plugin boundary and build/verify tooling audit — D4 · FOX · POE2

Scope: the game-specific side of each project (what a per-game plugin would own) and the
build/verification/hygiene tooling a monorepo would have to unify. All paths are relative to
`/home/claude/audit/`. Line numbers are from the staged copies. `HUNCH` marks inferences not
directly stated in the sources.

Sizes for orientation: D4 has 58 .cpp / 82 .h (CascReader.cpp alone is 1,716 lines); FOX has
130 .cpp / 152 .h; POE2 has 39 .cpp / 52 .h.

---

## 1. Engine-specific module inventory

Legend for the "Produces" column: **generic** = a type the viewport/exporter consumes that has no
engine vocabulary in it; **private** = a game-shaped type the UI consumes directly. Qt column:
what the *public header* depends on.

### 1.1 D4 (`d4/src/casc`, `model`, `tex`, `text`, `index`, `deps`)

| Module | Parses / provides | Public entry points | Produces | Qt in header |
|---|---|---|---|---|
| `casc/CascReader.h` | The store: `.build.info`/`.build.config`, `.idx`, TVFS root, BLTE (raw/zlib/lz4/Salsa20), TACT keys, `EncryptedSNOs.dat`, `CoreTOCSharedPayloadsMapping.dat` (`CascReader.h:12-18`, `116-158`) | `open(gameDir, product)` :37, `readFile(name)` :74, `readPayloadBySno` :77, `readMetaBySno` :79, `enumerate(mask, fn)` :196, `buildId()` :60, `lineageKey()` :57, `buildAndKeySignature()` :180, `applyTactKeys` :71, `verifyTactKeys` :192, `encryptedSnos()` :134, `sharedPayloads()` :158 | private (`QByteArray` blobs keyed by SNO; `Entry{name,size,fileDataId,available}` :21) | QString, QByteArray, QHash, QMutex, `std::function` |
| `model/ModelGeometry.h` | The geometry contract (not a parser) | structs `MeshVertex` :22, `MeshPrimitive` :40, `ModelJoint` :50, `ClothCapsule` :86, `ClothPlane` :102, `ClothSim` :110, `ModelHardpoint` :181, `ModelGeometry` :189 | **generic-ish**: positions/normals/UVs/joints are neutral, but `slotHash`, `subObjectHash`, `materialIndex` :44-46, the whole NvCloth block :110-174 and `restQ/restT/restS` "D4-native, pre axis-swap" :66-70 are D4 | QString, QVector, `std::array` |
| `model/ModelParser.h` | `.app` meta+payload → geometry, cloth blocks, tuning resolution via d4data JSON | `parseApp(meta, payload, appName)` :23, `parseClothOnly` :30, `resolveClothTuning(d4dataDir, sim)` :47, `mergeGeometries` :54 | `ModelGeometry` | QByteArray, QJsonObject |
| `model/AnimParser.h` | `.ani` payload (AnimPayloadData, compression 0–6, long-form) | `decode(payload, offset, frameCount, compression, frameRate, restPose)` :40 | private `DecodedAnim{bones[]{boneHash, T/R/S per frame}}` :16-29 — consumed by GLModelWidget directly | QByteArray, QHash, QVector |
| `model/Material.h` | d4data `.mat.json` → texture bindings + MaterialValues | `parseMaterialJson` :26, `parseMaterialValues` :29, `shaderSlotRole(int)` :33 | private `MatTexture{slot, role, texName, texSno, uScale}` :7, `MaterialValues` :17 | QByteArray, QString |
| `model/Appearance.h` | d4data `.app.json` → material roster | `parseAppearanceJson` :23 | private `AppearanceInfo{materials[], looks}` :17 | QByteArray, QStringList |
| `model/AppearanceMatBin.h` | The same roster from the CASC meta BINARY (layout at :77-86) | `read(meta, why)` :57, `snos(meta)` :60 | private `Entry{hash, sno, cloth}` :39 | QByteArray, QVector |
| `model/MaterialDecode.h` | The whole material chain: name→SNO resolver, texture decode, detail maps, dye bands, roster via JSON *or* binary, `TextureCacheScope` | `setNameResolver` :85, `snoForMaterial` :87, `texture(reader, d4, name, sno)` :90, `byRole` :155, `baseColor/normalMap/orm` :222-224, `factors` :230, `uberMaterial` :245, `shaderMap` :251, `appearanceRoster*` :277-307, `texturesFor` :318 | `QImage` (generic) + `QVector<MatTexture>` (private) | **heavy**: QImage, QColor, QJsonObject, QVector3D/4D, `std::function`; takes `CascReader*` + d4data dir on every call |
| `model/Attachments.h` | Actor `.acr.json` trigger events → attach child mesh at a hardpoint; sub-rig salting | `scanActor` :47, `loadHardpointMap` :52, `seat` :60, `saltBoneHash` :72, `attachSubRigAt` :109, `attachSubRig` :122, `seatMount` :134 | mutates `ModelGeometry` in place | QHash, QPair, QString |
| `model/Hardpoints.h` | `.app.json` `ptHardpoints` → `ModelHardpoint` | `nameForHash` :14, `readInto(geo, appJsonPath)` :23, `resolveBoneIndices` :28 | fills `ModelGeometry::hardpoints` | QString |
| `model/FormatProbe.h` | Startup sanity: parse `barM_base00`, check bone count | `run(reader, index)` :24 | `Result{ran, ok, boneCount, summary, warning}` :13 | QString |
| `tex/BcDecode.h` | CPU BC1/3/4/5/7 → RGBA8888 with D3D12 256-byte row pitch (`BcDecode.h:8-13`) | `decode(data, w, h, eTexFormat)` :15, `selfTest()` :21, `isTwoChannel` :38, `withNormalZ` :39 | `QImage` (generic) | QByteArray, QImage |
| `tex/TexFormat.h` | `eTexFormat` (9/41/42/10/46/47/49/50) → codec + GL enum + alignment | `codec(eTexFormat, payloadSize, w, h)` :29, `name` :31, `alignedWidth` :34, `mip0Size` :38 | private `Codec{bytesPerBlock, glInternalFormat, name}` :13 | QString, QtGlobal |
| `tex/TexMeta.h` | d4data `.tex.json` → dims/format/frames/subres; user override files | `parseTexMetaJson` :39, `textureDimOverride` :47, `frameIconOverride` :56, `frameOverrideCount` :60 | private `TexMeta` :26 | QByteArray, QImage |
| `tex/FrameTable.h` | CASC `base/Misc/2D_table.dat` → atlas frame handles (layout :21-24) | singleton `instance()` :33, `ensureLoaded(reader)` :38, `frameCount/handles/locate/atlases` :47-58 | private | QHash, QMutex, `std::atomic` |
| `tex/TextureDefTable.h` | CASC `texture-base-global.dat` + per-key overlays → w/h/format per texture SNO (layout :24-29) | singleton, `ensureBuilt(rd)` :46, `lookup(sno)` :52 | private `Def{width,height,format}` :37 | QHash, QMutex |
| `text/StringTable.h` | d4data `.stl.json` → label/text rows | `parseStringTableJson` :15 | private `StringRow` :6 | QByteArray |
| `index/CoreToc.h` | `CoreTOC.dat` (legacy + 0xBCDE6611 header) → group→entries | `parseCoreToc(data)` :19 | private `SnoEntry{snoId, name}` :8 | QByteArray, QHash |
| `index/SnoIndex.h` | The asset list: CoreTOC from CASC or d4data, disk cache, name↔SNO per group, encrypted-name recovery, build ledger / "Latest" | `loadFromCasc` :22, `loadFromD4data` :23, `loadFromCache(sig)` :26, `entries(group)` :35, `nameForSno` :60, `snoForName` :67, `groupName` :69, `recoverEncryptedNames` :89, `applyEncryptedNameDicts` :109, `buildHistory` :149, `updateLatest(sig, lineage, ver)` :157 | private (group ids, SNOs) — **GUI-thread-only** lazy caches (:39-49) | QHash, QSet, QString |
| `index/SnoListModel.h` | `QAbstractTableModel` over `SnoEntry` with filters/predicates/icons | :37-99 | Qt model | QAbstractTableModel, QIcon, QPixmap, QCache |
| `index/AnimActionIndex.h` | Header-only singleton: regex-crawls d4data `AnimSet/*.ans.json`, `Emote/*.emo.json`, `StringList/Emote_*.stl.json` for clip → action label | `ensure(d4)` :25, `action(clip)`, `animSet(clip)` | private | QDirIterator, QRegularExpression, QMutex — **all inline** (205 lines of logic in a header) |
| `index/AppearanceMeta.h` | Item→Actor→Appearance crawl → tags/titles/collections/icon handles | singleton, `ensureBuilt(d4, index, reader)` :30, `tagsFor/titleFor/collectionFor/iconFor` :35-39, `heroClassPrefixes` :50 | private | QObject (signals `readyChanged`, `progress`) |
| `index/AssetLinks.h` | Texture ← Material ← Appearance reverse map | `ensureBuilt(d4)` :21, `linksForTexture` :33, `appsForMaterial` :44 | private | QObject |
| `index/BackTrophyIndex.h`, `ItemHoverIndex.h`, `WardrobeAnimIndex.h`, `StoreProductIndex.h` | Item/ItemType/Actor/AnimSet/StoreProduct JSON crawls for the showpiece tabs | each: singleton + `ensureBuilt(d4)` + `reset()` + `readyChanged` (e.g. `BackTrophyIndex.h:55-72`, `StoreProductIndex.h:109-160`) | private | QObject |
| `index/IconIndex.h`, `IconAudit.h`, `DadOverride.h`, `ItemDef.h`, `MatSnoSweep.h` | Icon atlas map; diablo4.dad cross-check; binary Item/Actor definition reader (offsets :12-24); corpus sweeps/health audit | `IconIndex::iconImage(handle, reader)` :40; `IconAudit::run`; `DadOverride::ensureLoaded` :42; `ItemDef::parseItem/parseActor`; `MatSnoSweep` free functions | private | QObject / QImage / plain |
| `deps/D4DataDownloader.h` | `git` sparse checkout of DiabloTools/d4data via QProcess | `gitPath` :22, `defaultDest` :23, `start(dest)` :25, signals :29-35 | — | QObject, QProcess, QTimer |
| `deps/UpdateCheck.h` | `git ls-remote` + `curl -I -z` probes for d4data / TACT keys | `tactKeysFile()` :15, `start()`, `finished(...)` signal | — | QObject |
| `deps/DependencyDialog.h` | Settings dialog page driving the downloader | QDialog | — | QDialog |

Seven of the eleven index-like singletons (those with a `readyChanged` signal) follow the template §2 shape (`ASSETBROWSER_TEMPLATE.md:189-194`) but
each is a hand-rolled singleton with its own `m_ready/m_building/m_generation` (e.g.
`BackTrophyIndex.h:80-87`, `StoreProductIndex.h:171-182`). There is no shared base class.

### 1.2 FOX (`fox/src/fox`, `anim`, `audio`, `index`, `preview/ModelLoader.h`)

| Module | Parses / provides | Public entry points | Produces | Qt in header |
|---|---|---|---|---|
| `fox/QarFile.h` | SQAR archives v1/v2 (.dat/.qar), Decrypt1/Decrypt2, zlib per entry (`QarFile.h:1-13`) | `isQar` :36, `open(path)` :39, `adopt(...)` :47 (cache re-install), `entries()` :53, `readEntry(entry)` :62, `readEntryRaw` :65; `qarcrypto::decrypt1/decrypt2` :86-91 | private `QarEntry{hash, sizes, md5, encryption, key}` :22 | QFile, QString, QVector |
| `fox/GzsFile.h` | Ground Zeroes `.g0s` (footer-detected, legacy hash + extension-id table) | `isGzs` :33, `open` :35, `adopt` :38, `readEntry` :45, `extensionForId/idForExtension` :48-52, `hashPath` :57 | private `GzsEntry` :24 | QString, QVector |
| `fox/FpkFile.h` | FPK/FPKD nested containers (real path strings) | `isFpk` :26, `parse(blob)` :30, `entries()` :33, `references()`, `readEntry(blob, entry)` :40, `normalizedPath` :43 | private `FpkEntry{filePath, offset, size, md5}` :17 | QByteArray, QString |
| `fox/PftxsFile.h` | PFTXS texture packs | `isPftxs` :26, `parse` :28, `readEntry` :42 | private `PftxsGroup/SubEntry` | QByteArray |
| `fox/SbpFile.h` | Wwise `.sbp` bundle → embedded `.wem` list | `parse` :32, `listWems` :36, `readWem` :37 | private `SbpWem` | QByteArray |
| `fox/FoxHash.h` | PathFileNameCode (CityHash64 51-bit + 13-bit ext code + meta flag), legacy GZ hash, StrCode32/64; `HashResolver` dictionary tables (:1-10, :50-64) | `hashFileName` :27, `hashFileNameWithExtension` :30, `hashFileNameLatin1` :36, `hashExtension` :45; `HashResolver::instance`, `loadDictionary(file)` :65, `addNames`, `tryResolve(hash, name*)` :79, `tryResolveGzs`, `legacyNameFor` :94, `strCode32NameFor` :98, `extensionFor` | private (quint64 keys) | QByteArray, QHash, QString, `std::atomic`, `std::mutex` |
| `fox/FoxCity.h` | Vendored google/cityhash (`FoxCity.h:1-2`) | `CityHash64`, `CityHash64WithSeed(s)` :8-10 | plain | **none** (pure C++) |
| `fox/FoxZlib.h` | zlib inflate/deflate wrapped and raw | `zlibInflate` :12, `zlibDeflate` :23, `zlibDeflateRaw` :31, `zlibInflateRaw` :35 | `QByteArray` | QByteArray only |
| `fox/BcDecode.h` | CPU DXT1/DXT5/A8R8G8B8/L8 → QImage; DDS header walk (mip/slice) (`BcDecode.h:1-8`) | `decodeDxt1/Dxt5/A8R8G8B8/L8` :18-23, `decodeDds` :27, `ddsSliceCount` :35, `decodeDdsSlice` :45, `ddsMipCount/ddsMipSize` :48-49, `selfTest` :52 | `QImage` (generic) | QByteArray, QImage |
| `fox/BcEncode.h` | RGBA → DXT1/DXT5/DXT5nm/A8R8G8B8/L8, box-filter mip halving; the only ENCODER in the family (`BcEncode.h:56-64`) | `encodeDxt1` :44, `encodeDxt5` :45-46, `encodeDxt5nm` :51, `encodeA8R8G8B8/L8` :56-57, `halve` :63, `encodeSelfTest` :67 | `QByteArray` blocks | QByteArray, QImage |
| `fox/NormalMap.h` | Fox DXT5nm channel swizzle ↔ ordinary RGB normal | `fromRaw` :47, `toYUp` :51, `toRaw` :58, `looksRaw` :63 | `QImage` | QImage |
| `fox/FtexFile.h` | `.ftex` + `.N.ftexs` streamed mips → standard DDS (`FtexFile.h:1-15`) | `isFtex` :36, `parse` :38, `readChunkedMip` :63, `assembleDds(ftex, provider(N), missingMips*)` :71, `describe` :76, `ddsHeader(...)` :84 | private header fields + a DDS `QByteArray` | QByteArray, `std::function` |
| `fox/FtexWriter.h` | DDS/QImage back into `.ftex`+`.ftexs` in the original's own layout (measured chunk rule :12-25) | `writeFtexLike` :54, `writeFtexFromImage` :73, `writeFtexFromFile`, `chunkMip` :94 | `FtexWriteResult{ftex, ftexs map, error}` :38 | QByteArray, QImage, QMap |
| `fox/FmdlFile.h` | `.fmdl` model: bones, meshes, mesh groups, material instances, texture refs (`FmdlFile.h:1-18`) | `isFmdl` :97, `parse` :99, `bones()/meshes()/meshGroups()/materials()` :102-118, `shippedHiddenGroups` :109, `boneSubtreeMask` :121, `describe` :125 | **private**: `FmdlBone` :27, `FmdlTextureRef` :36, `FmdlMaterialParam` :52, `FmdlMaterialInstance` :58, `FmdlMesh` :67 (positions/normals/uv0/tangents/boneIndices+palette/triangles), `FmdlMeshGroup` :87 | QString, QSet, QVector |
| `fox/FoxMaterial.h` | Shader-name → feature model (`classifyShader` :289), texture-role vocabulary (`texrole` :57), measured SRM/TRM channel semantics (:18-48) | `classifyShader(shaderName)` → `MaterialModel` :271 | private | QString |
| `fox/FmttFile.h` | `.fmtt` 256×32-byte material preset table | `isFmtt` :75, `parse` :76 | private `MaterialPreset` :64 | QByteArray |
| `fox/FsopFile.h` | `.fsop` shader pack (Lua source table) → shader defs | `isShaderTable` :54, `tableWindow` :59, `parse` :60 | private `ShaderDef` :43 | QHash, QString |
| `fox/FovaFile.h` | `.fv2` variation: hide/show groups, texture substitutions, attachments | `isFova` :69, `parse` :71, `describe` :93 | private `FovaSubstitution` :52, `FovaAttachment` :59 | QByteArray |
| `fox/Fox2File.h`, `Fox2Refs.h` | Fox2 entity binaries (.fox2/.parts/.vfsm) full property tree; light path extractor | `Fox2File::parse` :65, `lookup(hash)` :70; `looksLikeFox2` :13, `fox2AssetPaths` :18 | private `Fox2Entity/Property` (QVariant values :34-49) | QVariant, QStringList |
| `fox/FrigFile.h`, `fox/GaniAnim.h`, `fox/MtarFile.h` | Rig (`.frig`, rig units→bones), GANI2/v1 clip decode (`decodeGani2` :106, `decodeGaniV1` :114, `parseGaniLayout` :58), motion archive `.mtar` (`parse` :37, `clips()` :43) | as listed | private `GaniAnim{tracks[]{channels}}` :95, `MtarClip{hash,name,offset,size}` :23 | QHash, QString, QVector |
| `fox/FcnpFile.h` | Connect points (attachment sockets) | `parse` :24 | private `ConnectPoint{name, parentBone, pos, quat, scale}` :14-20 | QString |
| `fox/LangFile.h` | `.lng2` localisation (big-endian) | `isLang` :36, `parse` :38, `textFor(label)` :45, `textForHash` :46 | private | QHash, QString |
| `fox/GrxlaFile.h`, `fox/DfrmFile.h` | Light arrays; deform-support file (no shape data, :1-8) | `parse` :39 / :58 | private | QByteArray |
| `anim/AnimMath.h` | Row-major System.Numerics-convention math (`AnimMath.h:1-9`) | `Vec3/Mat4/Quat`, `mul`, `transform`, `invertRigid`, `quatMul`, `rotate`, `fromTo` | plain | **none** (only `<cmath>`) |
| `anim/AnimPose.h` | gani frame → skin matrices for an FMDL skeleton; IK; root-motion | `boneIndexByHash` :81, `setRootMotionInPlace` :114, `selfTest` :125 | `QVector<Mat4>` | includes `fox/FmdlFile.h`, `FrigFile.h`, `GaniAnim.h`, `FrdvFile.h` |
| `anim/FrdvFile.h`, `anim/RigBind.h`, `anim/AnimBind.h`, `anim/AnimFavourites.h` | Help-bone drivers; model→rig resolver (.parts, cached `fox_rigmap_v<N>.bin` :7-8); which clips belong to a model; starred clips in QSettings | `FrdvFile::parse` :35; `rigbind::` free fns; `animbind::forModel(modelPath)` :51, `clearCache` :54; `animfav::keyFor/isFavourite/toggle` :20-24 | private | QSet, QString, QObject (Notifier :28) |
| `audio/WemFile.h` | Wwise `.wem` RIFF parse, PCM→wav, optional external `vgmstream-cli`, playback via `PlaySound`/`aplay` | `parseWem` :32, `wemToWav` :35, `vgmstreamPath` :39, `playWav` :47 | private `WemInfo` :11 | QByteArray; `.cpp` uses QProcess (`WemFile.cpp:8,178,229`) |
| `index/ArchiveIndex.h` | **The store** — see §3 | see §3 | private `IndexedFile` :43, `IndexedArchive` :63 | QObject, QMultiHash, `std::shared_ptr<QarFile/GzsFile>` |
| `index/GameId.h` | Which of 4 games a file/install is; run-scoped `GameFilter` | `gameForAssetPath`, `GameFilter::instance().enabled/setEnabled/overrideForRun` | plain enum | QString |
| `index/HashSolve.h` | Name recovery: harvest path strings from readable files + candidate sweep against unknown keys (`HashSolve.h:1-38`) | `recoverNames(outFile, threads, cap)` → `RecoverResult` | private | QString |
| `index/NameCatalog.h` | In-game display names: `WeaponPartsUiSetting.lua` + `.lng2` join (:1-19) | `instance()`, `nameFor(modelStem)`, `textForLabel` | private | QHash |
| `index/RefIndex.h`, `TextureUsers.h`, `AnimCatalog.h`, `ModelTags.h`, `TexThumbCache.h`, `BuildTimer.h` | Where-used for every file kind; texture → models+role; every animation grouped; the tag vocabulary derived from the index; off-thread thumbnail decode; timing | singletons / free fns | private | QObject / QImage |
| `index/EquipCatalog.h`, `WeaponCatalog.h`, `CharacterCatalog.h`, `PlayerCatalog.h`, `MechaCatalog.h`, `PartCatalog.h`, `MgoGearConfig.h`, `SsdGearConfig.h`, `AvatarPresets.h`, `AvatarTextures.h`, `LayerColors.h`, `IconCatalog.h`, `MaterialPresets.h`, `ShaderTable.h` | The Customize (showpiece) tab's catalogues: chimera weapon slots, gear tables parsed from shipped Lua, avatar presets, skin-tone/hair texture grammar, colour swatches, UI icons, the one `.fmtt`, the shader packs per install | singletons (`instance()`), `ensureBuilt`/`build` | private | QObject / QImage |
| `preview/ModelLoader.h` | FMDL → viewport: parse, decode base/normal (+SRM/TRM/layer under `PbrMode::Full`), build GPU uploads, FOVA overrides, attached models | `load(fileIdx, mode)` :118, `loadForThumbnail` :194, `fovaOverrides` :34, `fovaGroupVisibility` :54, `fovaAttachedModels` :112, `buildUploads(model)` :298, `dropGroups` :303, hair/neck/bandanna group helpers :310-345 | **private**: `LoadedModel{fox::FmdlFile model; QVector<QImage> textures, normalMaps; QVector<GLPbrMaterial> pbr; QVector<GLMeshUpload> uploads; GLSkeletonUpload skeleton}` :68-81 | includes `gl/GLModelWidget.h` and `index/AvatarTextures.h` — the "engine" header depends on the viewport |

Key structural fact: **FOX has no engine-neutral geometry type.** `fox::FmdlFile` is the model
(`ModelLoader.h:71`), the viewport takes `GLMeshUpload` (`gl/GLModelWidget.h:39-53`, an
interleaved float buffer + `materialSlot` + `groupId`), and the exporter takes `const
FmdlFile&` (`model/GlbExporter.h:17-19`, `model/Retarget.h:85,106,123`). D4 and POE2 both put a
`ModelGeometry` between parser and consumer (`d4/src/gl/GLModelWidget.h:40 setGeometry(const
ModelGeometry&)`, `poe2/src/gl/GLModelWidget.h:34 setModel(const ModelGeometry&, const
AstSkeleton::Skeleton&)`), but the two `ModelGeometry.h` files differ in 268 diff lines
(`d4` 217 lines, `poe2` 70 lines) and their `RigMath.h` differ in 288 — D4's is column-major
(`d4/src/model/RigMath.h:14`), POE2's row-major v·M (`poe2/src/model/RigMath.h:5-12`).

### 1.3 POE2 (`poe2/src/bundle`, `model`, `tex`, `store`)

| Module | Parses / provides | Public entry points | Produces | Qt in header |
|---|---|---|---|---|
| `bundle/Bundle.h` | One `*.bundle.bin`: header, block table, on-demand Oodle block decode with per-bundle LRU (`Bundle.h:9-15`) | `open(path)` :31, `fromMemory(bytes)` :34, `readRange(off, size)` :41, `readAll` :43, `setCacheBlocks` :46; `parseBundleHeader` :67 | `QByteArray` | QByteArray, QMutex, `std::shared_ptr` |
| `bundle/BundleIndex.h` | `_.index.bin`: bundles, file records, path generation from the nested path-spec bundle; disk cache keyed on index size+mtime (:78-85) | `hashPath` :41, `hashUtf8Lower` :42, `load(bundlesDir, cacheDir, err, progress)` :46, `fingerprint()` :52, `bundles()/files()/directories()/extensions()` :54-57, `find(path)` :61, `pathOf/nameOf/dirOf` :62-64, stats :68-72 | private `BundleRecord` :21, `FileRecord{hash, bundle, offset, size, dir, nameOff, nameLen, extId}` :25 | QString, QStringList, QVector + `std::vector/string/unordered_map` (deliberately non-Qt storage :82-83) |
| `bundle/Oodle.h` | Entry point into vendored ooz | `decompress(src, srcLen, dst, dstLen)` :17, `kOverrun` :13 | plain | **none** |
| `tex/DdsImage.h` | DDS/DX10 header probe + top-mip decode via vendored bcdec; `.dds.header` split file | `probe` :24, `decode` :28, `probeHeader` :41, `gggFormatName` :44, `selfTest` :47 | `QImage` (generic); `Info` :13, `HeaderInfo` :32 | QByteArray, QImage |
| `tex/BcdecImpl.cpp` | The single TU that instantiates `third_party/bcdec` (MIT) (`BcdecImpl.cpp:1-4`) | — | — | — |
| `model/ModelGeometry.h` | Contract for viewport + exporter (:7-12) | `MeshVertex` :14 (pos/normal/tangent+w/uv0/uv1/joints u8/weights/color), `MeshPart{name, material, indexStart, indexCount, materialIndex}` :28, `ModelJoint{name, parent, bindMatrix, inverseBind}` :37, `ModelGeometry` :44 (+ provenance `sourcePath/formatVersion/vertexFormat/skinned` :54-57) | **generic** — nothing PoE-specific except `materialPaths` being game paths | QString, QVector |
| `model/MeshParser.h` | `.smd` v1–3 and `.fmt` v9 → `ModelGeometry` | `vertexStride` :17, `parseSmd(data, path, out, err)` :22, `parseFmt` :26, `selfTest` :30 | `ModelGeometry` | QByteArray |
| `model/AstSkeleton.h` | `.ast` skeleton + clips (v11/12 full, 6–10 bones only) with nested key bundle | `parse(data, decodeClips)` :47, `clipDuration` :52, `skinMatrices(sk, clip, t)` :56, `selfTest` | private `Skeleton{bones[], clips[]{KeySet[]}}` :36-43 — but shape is neutral (name/parent/bind/inverseBind + TRS keys) | QByteArray, QVector; `RigMath::Mat4` |
| `model/AssetText.h` | UTF-16 `.sm`/`.mat`(JSON)/`.ao` descriptors → material semantics (workflow, alpha mode, effect layers :39-97) | `decodeText` :17, `parseSm` :28, `parseMat` :100, `parseAo` :114, `selfTest` | private `SkinnedMeshDesc`, `Material`, `AnimatedObject` | QByteArray, QStringList |
| `store/AssetStore.h` | **The store** — see §3. Also the material resolver, skeleton resolver, attachment assembly | `open` :33, `readFile(path)/readFile(idx)` :52-53, `loadModel` :58, `resolveMaterials` :61, `loadSkeletonFor` :66, `loadTexture` :69, `filesUnderPrefix` :74, `collectAssetFiles` :82, `attachmentsForModel` :102, `assembleForExport` :109 | `ModelGeometry`, `QImage`, `AstSkeleton::Skeleton`, `QVector<GlbExporter::ExportMaterial>` | QObject, QCache, QMutex; includes `GlbExporter.h` (store depends on the exporter's material struct) |
| `store/IndexLoader.h` | Background build → `finished` / `materialsReady` signals, generation counter (:7-9) | `start(bundlesDir)` :18, `isRunning` | — | QObject, QThread |
| `store/NameIndex.h` | `.datc64` join BaseItemTypes/MonsterVarieties/NPCs → in-game names by model path hash (:10-25) | `build(store)` :35, `load/save(cacheDir, fingerprint)` :38-39, `nameFor(modelPath)` :42, `searchNamesFor` :44, `kCacheVersion=3` :30 | private (`QHash<quint64,QString>`) | QHash |
| `store/MaterialFamilyIndex.h` | Sweep every `.mat`/`.sm`/`.fmt` → family/workflow/effect bits per model, cached on the fingerprint (:69-87) | `build` :54, `load/save` :58-59, `metaForModel` :63, `modelWorkflowMask/EffectMask` :68-69 | private bitmasks | QHash |
| `store/DatFile.h` | `.datc64` columnar table reader, schema supplied by caller (:1-16) | `load(bytes)` :24, `u64(row, col)`, `str(row, col)` :33, `arrayCount/arrayU64/strFromArray` :38-40 | plain | QByteArray, QString |

---

## 2. Engine-neutral modules living in the game layer

| Concern | D4 | FOX | POE2 | Comparison / superset |
|---|---|---|---|---|
| **BC block decode** | `tex/BcDecode.cpp` (358 lines, hand-written): BC1/3/4/5/7, 256-byte D3D12 row pitch applied to input (`BcDecode.h:12-13`, `.cpp:236-238`), punch-through gated on eTexFormat 46 vs 47 (`.cpp:242-254`), BC7 tables with self-test (`BcDecode.h:21`), `withNormalZ` (:39). **No DDS header parsing** — D4 payloads are headerless. | `fox/BcDecode.cpp` (393 lines, hand-written "straight implementations of the D3D BC1/BC3" `.cpp:1`): DXT1, DXT5, A8R8G8B8, L8 only (`BcDecode.h:1-4`). Parses a DDS header (`decodeDds`, slices, mips :27-49). No BC4/5/7. | `third_party/bcdec` (vendored MIT, 4-line `BcdecImpl.cpp`) wrapped by `tex/DdsImage.cpp` (186 lines): BC1/2/3/4/5/6H/7 + RGBA8, DX10 header (`DdsImage.h:3-6`). | **Different code, same algorithms.** bcdec is the block-level superset (adds BC2/BC6H). D4's row-pitch alignment and fmt-46/47 rule are the only game-specific parts and belong above the block decoder. FOX's DDS-slice/mip walker and POE2's DX10 probe are both "DDS container" logic that could sit on one `DdsContainer` helper. FOX is the only project with an **encoder** (`BcEncode.h`) and the only one with a self-test that round-trips encode→decode (`BcEncode.h:59-64`). Core candidate: `bcdec` + a thin `BlockImage::decode(codec, bytes, w, h, rowPitchRule)` + DDS header probe; D4 keeps `TexFormat.h` (eTexFormat table) as plugin data. |
| **zlib** | `<zlib.h>` used inline in `casc/CascReader.cpp:22,102-104` (plus `<lz4.h>`, `<lz4frame.h>` :20-21) | `fox/FoxZlib.h` — the one wrapper: inflate/deflate wrapped and raw (`FoxZlib.h:12-35`) | none (Oodle only, `CMakeLists.txt:13-16`) | FoxZlib is the reusable shape (raw deflate for ZIP :24-35). D4 would move its `inflate` helper onto it. POE2 needs nothing but would get zlib for free via the core. |
| **Third-party decompressors** | zlib + lz4 (vcpkg) + hand-written Salsa20 (`CascReader.cpp:46-49`) | zlib | vendored `third_party/ooz` static lib (`CMakeLists.txt:23-33`) | Salsa20 and ooz are engine-specific; lz4 is generic but only D4 needs it. |
| **Hashing** | DJB2 bone-name hash implied by `ModelJoint::nameHash` (`ModelGeometry.h:52`); `saltBoneHash` (`Attachments.h:72-75`) | `fox/FoxCity.h` vendored CityHash64 (pure C++, no Qt) + `FoxHash.h` PathFileNameCode scheme | `BundleIndex::hashUtf8Lower` MurmurHash64A seed 0x1337B33F (`BundleIndex.h:39-42`) | The primitives (CityHash64, MurmurHash64A, DJB2) are neutral and could live in `core/hash/`; the *schemes* (51+13-bit packing, lowercase-then-hash, meta flag) are plugin. FoxCity.h is the only one already Qt-free. |
| **ZIP** | none (`QProcess` git/curl only) | `util/ZipReader.h` + `util/ZipWriter.h` (154/155 lines) — central-directory reader with CRC check, stored+deflate (`ZipReader.h:1-11`); built on `FoxZlib::zlibInflateRaw` | none | FOX's pair is the only implementation; neutral (`namespace zipreader`). Used for `.mgsv` mod packages — a FOX feature — but the code is generic. |
| **Natural ordering** | none — no `naturalLess`/`QCollator` in `d4/src` | `util/NaturalOrder.h` `fox::naturalLess` (:15-40), digit runs by value, deterministic on leading zeros | none | FOX only. Trivially core (`namespace fox` is the only thing to drop). D4 and POE2 sort by plain QString today (HUNCH: their lists show `x11` before `x2` for numbered stems). |
| **Math** | `model/RigMath.h` column-major (`d4:14`) | `anim/AnimMath.h` row-major System.Numerics conventions, **no Qt** (`AnimMath.h:1-11`) | `model/RigMath.h` row-major v·M (`poe2:5-12`) | Three incompatible conventions; each is internally consistent and documented. A core cannot unify them without re-deriving every skinning path — leave per-plugin, or pick one and convert at the `ModelGeometry` boundary. |
| **Startup self-tests** | `QueryTerm::selfTest` in `main.cpp:232`; `BcDecode::selfTest` only runs when the Wardrobe tab is constructed (`WardrobeTab2.cpp:1254-1258`) | `fox::bc::selfTest`, `QueryTerm`, `searchq`, `animpose` in `main.cpp:121-143` | 10 self-tests in `main.cpp:56-59` (QueryTerm, MeshParser, DdsImage, AssetText, AstSkeleton, GlbExporter, ExportLayout, Facets, MaterialFamilyIndex, NameIndex) | POE2 has the pattern most consistently (every format module exports `QString selfTest()`). A core `conformance` target (§9) can register these. |
| **glTF writer** | `model/ModelExporter.cpp` hand-written with QJson (`ModelExporter.cpp:4-15`; zero `fastgltf::` references) | `model/GlbExporter.cpp` hand-written, "no external glTF library" (`GlbExporter.h:7-8`) | `model/GlbExporter.cpp` hand-written | Three JSON+buffer writers; none shares code. D4's `find_package(fastgltf)` and `TINYGLTF_INCLUDE_DIR` (`CMakeLists.txt:28-31, 194`) are **dead dependencies**. |

---

## 3. The store: where each project opens the install

| | D4 `CascReader` | FOX `ArchiveIndex` (+ `QarFile`/`GzsFile`) | POE2 `AssetStore` (+ `BundleIndex`/`Bundle`) |
|---|---|---|---|
| Class shape | plain class, no QObject, mutex-guarded (`CascReader.h:19, 249-253`) | `QObject` singleton with `readyChanged(bool)`/`progress` signals and a detached rebuild thread (`ArchiveIndex.h:16-19, 109-116, 234-236`) | `QObject`, owned by MainWindow, opened on a worker by `IndexLoader` (`AssetStore.h:27`, `IndexLoader.h:5-8`) |
| **Open** | `bool open(gameDir, product)` :37 — synchronous, returns readiness; `close()` :38; `isReady()` :39; `lastError()` :198 | `void rebuild(gameDirs, dictDir, deepScan)` :115 — **asynchronous**, multiple folders, needs a dictionary dir; `ready()`/`building()` :116-117 | `bool open(bundlesDir, err*, progress)` :33 — synchronous with a progress callback; `isOpen()` :34 |
| **Enumerate** | `enumerate(mask, fn(Entry))` :196 over TVFS paths; `rootPathsFor(sno)` :97, `rootPathsWithPrefix` :108, `rootPrefixCensus` :101. The asset *list* is a separate object (`SnoIndex::entries(group)` `SnoIndex.h:35`) built from `CoreTOC.dat`. | `files()` :149 → `QVector<IndexedFile>`; `archives()` :119; `findByHash` :175-176; `fileIndexForPath` :210; `findByPathHash` :232 | `index().files()` (`BundleIndex.h:55`), `find(path)` :61, `pathOf(i)` :62, `filesUnderPrefix(dir, ext)` (`AssetStore.h:74`) |
| **Read** | `readFile(virtualPath)` :74, `readPayloadBySno(sno)` :77, `readMetaBySno(sno)` :79, `payloadSize(sno)` :85 | `readFile(IndexedFile)` :162 (opens the container for children), `readEntryBlob(archiveId, entryIdx)` :168 | `readFile(path)` / `readFile(fileIndex)` :52-53 |
| **Fingerprint / build id** | `buildId()` :60 (TVFS root hash), `buildAndKeySignature()` :180 (build + key names), `lineageKey()` :57 (product or folder name), `openedVersion()` :46, static `gameVersion(dir)` :63 | `fingerprint()` :228 (every archive path/size/mtime + deep-scan flag + counts), `installGeneration()` :229 | `BundleIndex::fingerprint()` :52 (`"size:mtime"` of `_.index.bin`) |
| Locked/absent distinction | `encryptedSnos()` :134, `missingKeyCount()` :66, `payloadVariants` :111, `payloadSourceSno` :167 (shared payloads) | `IndexedFile::named/shadowed` :50-53, `IndexedArchive::inactive` :104, `unreadableEntries()` (`QarFile.h:57`) | `unnamedFiles()`, `sharedPayloadFiles()`, `skippedRoots()` (`BundleIndex.h:68-71`) |
| Multi-install / product | one install, `product` row selection :33-36 | many folders, four games, install scope (`InstallScope` :184-193), mount priority :72 | one `Bundles2` dir |
| Thread model | reads outside the index mutex so bulk workers decompress in parallel (:250-253) | "Thread-safe" reads :161, thread-local install scope :183 | bundle-handle cache mutex + per-Bundle mutex (`AssetStore.h:25-26`, `Bundle.h:29-30`) |
| Disk caches | TVFS root cache + `.idx` cache signed by buildId+keys (:212-220); SnoIndex cache signed by buildId (`SnoIndex.h:26`) | archive entry-table cache re-installed via `QarFile::adopt` (`QarFile.h:42-48`), deep-scan cache keyed on (path,size,mtime) (:9-14); `pruneOldCaches()` :41 | `bundle_index_v<N>.bin` keyed on size+mtime (`BundleIndex.h:79-80`, `kCacheVersion` :37) |

**Can they sit behind one `IArchive`?** Yes for the *read side*, with three caveats:

1. **Address type.** D4 reads by SNO (`quint64`) or virtual path; FOX by `IndexedFile` (an
   `(archiveId, entryIdx, childIdx)` triple, :44-46); POE2 by path or file-record index. An
   `IArchive::read(AssetId)` needs the opaque id of §4.
2. **Open is sync in D4/POE2 and async in FOX.** FOX's `rebuild()` is the template-§2 background
   index and the store at once; D4 separates `CascReader::open` (sync) from `SnoIndex` (cached
   parse) and each background index; POE2 separates `AssetStore::open` (sync) from
   `IndexLoader` (thread). A core interface should be sync `open()` + a separate core
   `BackgroundIndex` shape (which all eleven D4 indexes, FOX `ArchiveIndex`, and POE2
   `IndexLoader` re-implement by hand today).
3. **Fingerprint semantics differ.** D4's changes per game patch *and* per TACT key set
   (`CascReader.h:174-179`); FOX's per archive mtime + dictionary + deep-scan flag; POE2's per
   index mtime. One `QString fingerprint()` is enough as long as the plugin owns the recipe.

The minimal interface that all three already satisfy (with citations to the concrete method):

```
open(dir, err*)            D4 :37 | FOX :115 (async today) | POE2 :33
isOpen()                   D4 :39 | FOX :116             | POE2 :34
fingerprint()              D4 :60/:180 | FOX :228         | POE2 :52 (BundleIndex)
read(AssetId) -> bytes     D4 :74/:77 | FOX :162          | POE2 :52
enumerate(fn)              D4 :196 | FOX :149 (vector)     | POE2 :55 (vector)
lastError()                D4 :198 | FOX per-QarFile :58  | POE2 err* out-param
```

HUNCH: FOX's `readEntryBlob` (read the container once, then pull children) is a performance
contract the generic interface should keep as an optional `readContainer(id)`; D4's
`payloadSourceSno` redirect and POE2's `Bundle` block cache are internal to `read()` and need no
surface.

---

## 4. Names and ids

| | D4 | FOX | POE2 |
|---|---|---|---|
| Primary id | `snoId` (`qint32`, `CoreToc.h:9`) scoped by **group id** (132 groups, `SnoIndex.h:14-15`); functions take `(group, sno)` (`SnoIndex.h:60,67`). Store reads take `quint64 sno` (`CascReader.h:77`). Materials live in **two** groups, 37 and 57 (`HYGIENE_TOOLING.md:154-155`). | `quint64` PathFileNameCode: low 51 bits CityHash64 of path-without-extension, top 13 bits extension code, plus meta flag `0x4000000000000` (`FoxHash.h:4-10, 21-22`). GZ archives use a different 48-bit legacy hash with extension *id* at bit 52 (`GzsFile.h:13-16`, `IndexedFile::gz` :48). Bone/material names are StrCode32/64 (`FmdlFile.h:13-14`). | `uint64_t` MurmurHash64A(seed 0x1337B33F) of the lowercased UTF-8 path (`BundleIndex.h:39-42`). "PoE2 has no SNO — the asset id IS the MurmurHash64 of the path" (`poe2/CLAUDE.md`, find-by-id paragraph). |
| Position vs identity | `entries(group)` index positions are sorted by name, so a reverse map is lazily built (`SnoIndex.h:50-52`) | `IndexedFile` positions move on rescan; favourites deliberately key on `"<archive path>\|<clip name>"` (`AnimFavourites.h:3-8`) | file-record index is stable per index build; caches key on path hash (`NameIndex.h:55`) |
| Display name source | `CoreTOC.dat` name (blanked for encrypted → `~unnamed_<sno>`, `SnoIndex.h:78-79`); recovered from cloth-data name fields :80-89 and from `EncryptedNameDict-0x<key>.dat` :91-109; item titles from d4data StringLists (`AppearanceMeta.h:12-16`) | Dictionary lookup `HashResolver::tryResolve` (`FoxHash.h:79`), FPK entries carry real paths (`FpkFile.h:6-8`); in-game names via `NameCatalog` (`WeaponPartsUiSetting.lua` + `.lng2`, `NameCatalog.h:5-13`) | Path from the index's own path-spec bundle (`BundleIndex.h:1-3`); in-game names via `.datc64` join (`NameIndex.h:14-20`) |
| Present · unnamed · absent | encrypted-without-key, `~unnamed_`, and shared-payload redirect are all distinct (`CascReader.h:116-167`) | `named=false` renders as `<hex>.<ext>` (`FoxHash.h:76-78`); `shadowed`, `inactive` | `unnamedFiles()`, `skippedRoots()` (`BundleIndex.h:69-70`) |

**What a core `AssetId` needs**, from the three schemes:

- A 64-bit payload (`quint64`) — POE2 and FOX already are; D4 fits `(group << 32) | sno`.
- A **namespace/scheme tag** so the same 64-bit value cannot be confused across games and so
  FOX's two hash schemes (TPP vs GZ) stay distinguishable — FOX already carries this as
  `IndexedFile::gz` (:48) and D4 as the group.
- **Not a display name.** Every project separates id from name and every project has ids with no
  name at all; the core should treat `name()` as an optional, lazily resolved attribute provided by
  the plugin (D4 `nameForSno`, FOX `tryResolve`, POE2 `pathOf`).
- A **stable text form** for settings/favourites/manifests: D4 uses the decimal SNO, FOX uses
  path strings, POE2 accepts decimal or `0x` hex (`poe2/CLAUDE.md` find-by-id). `QueryTerm`'s
  "digits = id" rule (assetbrowser-design skill, step 3) must therefore parse via the plugin.
- HUNCH: a `struct AssetId { quint8 scheme; quint64 value; }` with plugin-provided
  `toString/fromString` covers all three without forcing D4 to give up group scoping.

---

## 5. Community / online data and keys

| | What | Where fetched / stored | Cited |
|---|---|---|---|
| **D4** d4data | Sparse, blob-filtered `git` clone of `https://github.com/DiabloTools/d4data.git`, only `json/base` + `json/enUS_Text` | `D4DataDownloader.cpp:11`; runs `git` via `QProcess` (`D4DataDownloader.h:10-16`); dest `AppPaths::dataDir()/d4data` (`.cpp:65-68`) | A lagging second source of truth — the template's §1 [E] bullet (`ASSETBROWSER_TEMPLATE.md:158-166`) |
| **D4** TACT keys | wowdev-format key lists; candidates `raw.githubusercontent.com/HoldMyBeer-gg/rustydemon/main/keys/d4.keys` and `.../d4_tact_keys_clean.txt`; CascLib `KeyService.cs` named as a *wrong* source | `SettingsDialog.cpp:67-79`, `UpdateCheck.cpp:26-27`; fetched with external **curl** because "qtbase here was built without QtNetwork" (`SettingsDialog.cpp:2651-2656`); verified against `EncryptedNameDict-0x<key>.dat` (`CascReader.h:182-192`) | Keys are gitignored per template §1 :149-152 |
| **D4** diablo4.dad | `https://diablo4.dad/d4dad.json` community transmog DB | `MainWindow.cpp:997`; conditional curl into `AppPaths::dataDir()` (`DadOverride.cpp:20`, `MainWindow.cpp:968-977`); used as icon override + hidden audit (`DadOverride.h:14-18`, `IconAudit.h:7-16`) | |
| **D4** update check | `git ls-remote` + `curl -I -z` — notify only | `UpdateCheck.h:5-9` | |
| **D4** user overrides | `texture_dims_override.txt`, `texture_overrides/`, `icon_overrides/` under data dir | `TexMeta.h:41-56`, `TexMeta.cpp:130,192` | local, not online |
| **FOX** dictionaries | Community path dictionaries (GzsTool/FMDL-Studio lineage) **shipped with the tool** in `dict/` beside the exe, read-only (`AppPaths.h:81-88`); configurable `paths/dictDir` (`Config.cpp:12,51-55`); `HashResolver::loadDictionary` (`FoxHash.h:65`) | no downloader, no network code in `fox/src` (only `QProcess` for `vgmstream`/`aplay`, `WemFile.cpp:178,229`) | The `foxab-codebase` skill records that `AppPaths::dictDir()` is `build\release\dict\`, not the repo's `dict\` — a deployment trap already hit (`NEW_SESSION_PROMPT.md:176-178`) |
| **FOX** hash solving | `index/HashSolve.h` — harvests path strings out of `.fpkd`/`.lua`/DataSets/UI/exe and sweeps generated candidates; writes a dictionary file (`recoverNames(outFile, threads, cap)`), measured ceiling ~4% (`NEW_SESSION_PROMPT.md:182`) | offline, CPU only | The one **name-production** tool in the family |
| **FOX** external tool | optional `vgmstream-cli` on PATH for Wwise codecs (`WemFile.h:4, 37-40`) | | |
| **POE2** | **None.** "nothing proprietary in the repo and no runtime download" (`poe2/CMakeLists.txt:14-15`); Oodle is the vendored open-source ooz, names come from the game's own `.datc64` tables (`NameIndex.h:10-12`). No `curl`/`QNetwork`/`QProcess`/`http` anywhere in `poe2/src` (grep: zero hits). | | The template's [E] "no decryption keys" bullet is moot here, exactly as §1 :149-152 anticipates |

Monorepo consequence: only the D4 plugin needs a "dependencies" subsystem (downloader, key
verifier, staleness banner, update check). A core `IDependency` hook is worth having only if it is
allowed to be empty; FOX would use it for "dictionary folder present and loaded" and POE2 not at
all. All three vcpkg manifests build qtbase with `default-features: false` and no `network`
feature (§7), which is why D4 shells out to curl/git — a core that wanted QtNetwork would change
every manifest.

---

## 6. CMake

| | D4 `CMakeLists.txt` | FOX | POE2 |
|---|---|---|---|
| Project / version | `D4AssetBrowser 2.3.0` :3-4 | `FOXAssetBrowser 0.1.0` :3-4 (exports `FOXAB_VERSION` define :231-232) | `POE2AssetBrowser 0.9.0` :3-4 |
| Targets | one `qt_add_executable(D4AssetBrowser WIN32 …)` :47 | `qt_add_executable(FOXAssetBrowser WIN32 …)` :46; optional console `foxab_probe` behind `FOX_BUILD_PROBE` :343-366 (re-lists 15 format sources by hand :347-362) | `add_library(ooz STATIC …)` :23-33 + `qt_add_executable(POE2AssetBrowser WIN32 …)` :35 |
| Source listing | **explicit**, `.h` and `.cpp` on one line, grouped by folder :47-129; 17 header-only files are *not* listed (e.g. `util/QueryTerm.h`, `index/AnimActionIndex.h`, `app/Hotkeys.h` — see cross-check below) | **explicit**, :46-225, less tidy (some `.h`/`.cpp` on separate lines, `util/` and `view/` files interleaved with `tabs/`); 6 headers unlisted (`AnimFavourites.h`, `Density.h`, `Handbook.h`, `ObjExporter.h`, `AnimTransport.h`, `AssetIcons.h`) | **explicit**, :35-101, every file in the tree is listed (0 unlisted) |
| Qt modules | Core Gui Widgets OpenGL OpenGLWidgets **Svg** :14 | same six :25 | Core Gui Widgets OpenGL OpenGLWidgets (**no Svg**) :16 |
| Third-party | ZLIB, lz4 (CONFIG), fastgltf (CONFIG), tinygltf include dir :20-31 — fastgltf/tinygltf are **unused in source** (no `#include`, zero `fastgltf::` calls; `ModelExporter.cpp:1-19`) | ZLIB :27; Windows `winmm dbghelp` :303-307 | vendored ooz + bcdec include dirs :103-106; no zlib |
| Compile flags | MSVC: `/EHa`, `/MP`, `/bigobj`, `NOMINMAX` :140-154 | MSVC: `/EHa`; `/MP` only under the VS generator (documented as a Ninja no-op :240-247); `/Z7` + `/DEBUG /OPT:REF /OPT:ICF` in Release :266-269; `NOMINMAX` :233; default `CMAKE_BUILD_TYPE=Release` guard :18-22 | MSVC: `/EHa /MP /bigobj`, `NOMINMAX` :110-113; non-MSVC `-w -msse4.1` for ooz :32 |
| PCH | 21 Qt/std headers :161-183 | 19 headers (no QSet/QColor/QRegularExpression) :273-292 | none |
| AUTORCC / icon | `CMAKE_AUTORCC ON` :39; `app.rc` OBJECT_DEPENDS on `app.ico` :44-45; `res/app.qrc` :49 | same, but `.rc` guarded by `if(WIN32)` :36-44 | no resources at all |
| Windows deploy | static-Qt detection → `qt_import_plugins` with 7 plugins + static CRT :201-235; dynamic → hand-copy DLLs from `vcpkg_installed/x64-windows` :236-259 (explains why not `qt_generate_deploy_app_script` :239-247) | same pattern, 6 plugins (no `QGifPlugin`) :313-336; **no dynamic-Qt install branch** (run.bat does it) :309-312 | dynamic-copy branch only, guarded by `EXISTS` :121-132; no static branch |
| Include root | `src/` :131-134 | `src/` :227-229 | `src/` + two `third_party/` dirs :103-106 |

Cross-check of the source lists against the staged trees (script run over each `CMakeLists.txt`):
no listed file is missing from any tree; D4 has 17 headers in the tree that are not listed and
FOX has 6 (all header-only, so they compile — but `Q_OBJECT` headers *must* be listed for AUTOMOC,
and the D4 skill's own rule says so). POE2 is fully consistent.

**What a monorepo `core/` (static lib) + `games/<x>/` (executables) must reconcile:**

1. **Two Qt module sets.** Svg is linked by D4/FOX but no source in either includes `QtSvg`
   (grep: zero hits in `d4/src`, `fox/src`) — it exists only for the `QSvgPlugin`/`QSvgIconPlugin`
   static-plugin bake. Core can standardise on the five modules and make Svg a per-game plugin
   option, or drop it.
2. **Dead deps.** Remove `fastgltf`, `tinygltf`, and (unless the D4 exporter is rewritten to use
   it) keep the hand-written glTF writer in core.
3. **Per-target flags become a function.** `/EHa`, `/bigobj`, `NOMINMAX`, PCH, the `.rc`
   OBJECT_DEPENDS trick and the static/dynamic Qt packaging are copy-pasted three times with
   drift (FOX's `/Z7`, POE2's missing PCH and static branch). One
   `assetbrowser_configure_target(<exe>)` CMake function, plus a shared `res/` handling rule.
4. **Static-lib plus Qt AUTOMOC.** Core's `QObject` classes (`QueryTerm` is header-only, but
   `ExportNotifier`, `LogConsole`, `SettingsDialog` skeleton, `FunnelFilter`, `AssetListModel`
   are `Q_OBJECT`) must be listed in the core target for moc; `qt_add_library(core STATIC)` with
   `qt_standard_project_setup()` at the top level. The static-Qt path then needs
   `qt_import_plugins` on each *executable*, not on core.
5. **Third-party placement.** `third_party/ooz` and `bcdec` are POE2-local today; bcdec becomes
   core (§2), ooz stays under `games/poe2/`. lz4 stays under `games/d4/`. zlib goes to core
   (FoxZlib wrapper).
6. **Version.** Each project has its own `project(VERSION)` and vcpkg `version`; FOX also bakes a
   `FOXAB_VERSION` define. Core needs a per-game version variable the About box and
   `release-notes.py` (template §30) read from one place.
7. **Include style.** All three use `#include "folder/File.h"` relative to `src/`; core headers
   would need a distinct prefix (`core/…`) so the generic files that exist in all three trees
   under the same name (`util/QueryTerm.h`, `util/PanelPersist.h`, `util/ExportLayout.h`,
   `util/NameTemplate.h`, `app/AppPaths.h`, `app/SehGuard.h`, `app/LogConsole.h`) stop shadowing
   each other.
8. **`FOX_BUILD_PROBE` style console harnesses.** FOX re-lists format sources for `foxab_probe`
   (:347-362); POE2's `tools/*.cpp` are compiled ad hoc by scripts. With the format layer in a
   per-game static lib (`games/fox/foxfmt`), harness targets just link it.

## 7. vcpkg dependencies

| Dependency | D4 | FOX | POE2 |
|---|---|---|---|
| `qtbase` (`default-features: false`; features `widgets, opengl, gui, png, jpeg`) | ✓ | ✓ | ✓ |
| `qtsvg` | ✓ | ✓ | — |
| `zlib` | ✓ | ✓ | — |
| `lz4` | ✓ | — | — |
| `fastgltf` | ✓ (unused) | — | — |
| `tinygltf` | ✓ (unused) | — | — |
| `builtin-baseline` | `a1cae005…` | `ea1a7396…` (**different**) | `a1cae005…` |

Union: `qtbase{widgets,opengl,gui,png,jpeg}`, `qtsvg`, `zlib`, `lz4`, (`fastgltf`, `tinygltf`
removable). One manifest at the monorepo root with all of them is the simplest; FOX's baseline
must be moved to match (or all three bumped together). Note the `png`/`jpeg` features and the
absence of `network` are load-bearing: D4's downloader design (curl/git via QProcess) exists
because `network` is off (`SettingsDialog.cpp:2651`).

---

## 8. `verify-src.py`

Line counts: D4 900, FOX 502, POE2 153.

| # | Check | D4 (function, line) | FOX | POE2 | D4-specific? |
|---|---|---|---|---|---|
| 0 | Zero/near-empty file, run first and alone | `check_truncation` :513, wired :839-846 | `check_truncation` :422 | inline block "1. zero-byte files" :53-56 (size==0 only, no near-empty floor) | no |
| 1 | Unbalanced `{}()[]` after stripping comments/strings | `check_balance` :122 (uses `strip_code` :58) | `check_balance` :110 | "2. delimiter balance" :58-65 (+ `raw_strip` for `R"(…)"` GLSL :46-48) | no |
| 2 | Header-only helper used without a *real* `#include` | `check_header_only_includes` :131, table `HEADER_ONLY` :42-53 (8 entries, D4 names) | :119, table of 2 (`AppPaths`, `AppLog`) :40-43 | "3." :67-86, table of 5 (`QueryTerm`, `NameTemplate`, `PanelPersist`, `AppPaths`, `RigMath`) :70-76 | mechanism no; **table is per-project** |
| 3 | printf/qWarning format-vs-arg count | `check_format_args` :273 with `_split_args` :192, `_read_string_run` :251 | :261 (same code) | "4." :88-106 — **a stub**: "No-op body kept as the documented anchor" :103-106 | no |
| 4 | Locals named `emit/signals/slots/foreach` | `check_qt_macro_names` :340, `QT_MACROS` :55 | :334 | "5." :108-113 (regex on 7 type keywords only) | no |
| 5 | Duplicate `auto x = [` lambdas in one body | `check_duplicate_locals` :367 | :389 | — | no |
| 6 | Duplicate keys in one `QHash/QMap` brace initializer | `check_duplicate_map_keys` :436, `MAP_CONTAINERS` :400, `strip_comments` :403 | — | — | no (motivated by `SnoIndex::groupNameMap`, :20-23) |
| 7 | `CsvCopy::install` before `setContextMenuPolicy` | `check_ctx_menu_order` :156 | :144 | — | no (needs `CsvCopy`, which POE2 also has) |
| 8 | `qPrintable` → must be `qUtf8Printable` | — | `check_qprintable` :362 (Windows-only codepage bug, "the container cannot reproduce it" :372-374) | — | no — **FOX-only today, applies to all** |
| 9 | Direct d4data `Material|Appearance` JSON reads above a per-file baseline | `check_d4_json_reads` :627, `D4_JSON_RE` :556, allow-list :563, baseline :618-624, `_strip_exists_probes` :569 | — | — | **yes** (d4data-specific), but the *inventory+baseline contract* (:552-555) is the reusable part |
| 10 | QSettings key written and never read (concatenation-aware) | `check_dead_settings_keys` :711, prefix/tail regexes :706-708, `SETTINGS_DEAD_ALLOWED` :698 | — | "6." :115-127 — **note only**, not concatenation-aware, never fails | no |
| 11 | Combos persisted by `currentText()` above baseline | `check_text_persisted_combos` :761, baseline :755-757 (`WardrobeTab2.cpp: 4`) | — | — | mechanism no; baseline D4 |
| 12 | Character-vs-equipment decided by a material-name substring | `check_character_name_tests` :798, tokens :787-788, baseline :789-794 | — | "7." :129-140 — generic `.contains/startsWith/endsWith(QStringLiteral(...))` **count**, note only | **token list is D4** (head/face/_hed/_bod…); POE2's generalisation is the portable form |
| — | Whole-tree vs per-file | per-file loop :829-859 + three whole-tree passes :861-886 | per-file only :457-486 | all whole-tree, global `problems`/`notes` lists :21 | |
| — | Exit contract | 0 clean, 1 any failure; warnings printed unless `--quiet` :888-896 | same :488-496 | 0 with notes, 1 on `problems` :142-153 | |

**Superset** (what a core `verify-src.py` should carry): checks 0–8 as hard failures (0–7 from
D4 with FOX's near-empty floor, 8 from FOX), plus the inventory+baseline framework of D4 :676-678
for the three convention checks (10, 11, 12) with per-game baseline tables and token lists
supplied by the game. Checks 2 and 9 are table-driven and the tables are the only per-game part;
a core script could read `games/<x>/verify.toml` (HEADER_ONLY, baselines, tokens, allow-lists)
or accept `--config`. POE2's port should be retired: its format check is a no-op, its dead-key
check cannot see concatenated keys (which is why D4's first draft "reported fifteen keys of which
fourteen were fine", `HYGIENE_TOOLING.md:20-23`), and its substring count never fails.

D4-specific by content: #9 entirely; #12's token list and every baseline number; #2's table.
Everything else is family-wide and already violated in more than one project (qPrintable was
found in FOX; the D4 skill lists the same Qt traps).

---

## 9. Container verification and headless testing

**POE2** — the only project with a Linux compile path in the repo:

- `tools/verify_container.sh` (42 lines): full g++ compile + link against distro Qt 6.4 with
  hand-run `moc` (:18-21) and a **hand-maintained source list** (:24-35). Cross-checked against
  `CMakeLists.txt`: `gl/ModelThumbnailRenderer`, `tabs/CustomizeTab`, `util/HoverPreview`,
  `util/ThumbnailCache` are in CMake but missing from the script, and their `Q_OBJECT` headers
  (plus `app/SearchableCombo`) are not moc'd — and `MainWindow.cpp`/`ModelsTab.cpp` do include
  them, so the script as staged cannot link the current tree. It is a second copy of the build
  graph and has drifted.
- `tools/render_offscreen.cpp` (~190 lines): `QOffscreenSurface` + FBO render of a model with
  the real viewport shader and two-pass alpha, optional clip+time (:71-77) and attachment
  assembly (:148), PNG out (:189). It `#include "/root/work/scratch/shaders.inc"` (:24) — a
  copy of `GLModelWidget.cpp`'s `kVert/kFrag` (`GLModelWidget.cpp:42,54`) living **outside the
  repo**, so it neither builds from a clean checkout nor proves the shipped shader.
- `tools/check-links.py` (66 lines): the template-§8 link checker; encodes GitHub's
  no-collapse anchor rule (:2-5, :21-25); walks `README.md`, `wiki/*.md`, `docs/*.md` :30.
- Ten startup self-tests in `main.cpp:56-59` and a tools/ folder of research executables named
  in `poe2/CLAUDE.md` (`skin_diag`, `rig_cover`, `bulk_verify`, `mat_survey`, `name_e2e`, …)
  that are not in CMake and not staged here.

**FOX** — the only project with an end-to-end regression suite, but Windows-only:

- `Verify - Regression.bat` (463 lines): 186 `:check` + 2 `:checkno` + 1 `:checkre` + 2 `:rc`
  invocations, each launching the **real exe** with harness flags (`--games all` :55) and
  matching a needle in stdout (:56-70). The exe carries 247 `QCommandLineParser` options
  (`fox/src/main.cpp:160-…`, `--tab --shot --character --export --bulk --filedump --fox2dump
  --shaderdump --menucensus --whereused --hexpage …`). Checks cover every tab screenshot
  (:164-168), format dumps, skin-tone tables, neck/hair rules, retarget in five export paths
  (:305-310), byte-identical converts, cache hits (:330-336 tail). Output goes to
  `_deliveries\regress\` with screenshots and per-check logs (:25-33). It runs against the
  user's install and so is not reproducible in a container.
- `tools/regress.sh` "stands at 147 checks" (`NEW_SESSION_PROMPT.md:217`) with fixture trees
  `treeV`, `instS`, `ssdinstall` and `tools/TESTTREE.md` (:218-221) — **not staged**. The
  `foxab-codebase` skill amendment describes the container trees (`_probe/sklpull.tgz` etc.)
  and the `--tab`/`--filemenu`/`--undoseq` flags. HUNCH: `regress.sh` is the Linux twin of the
  `.bat` driving the same exe under xvfb, given the skill's "the container is no longer blind".
- Startup self-tests: `bc`, `QueryTerm`, `searchq`, `animpose` (`main.cpp:121-143`); FOX's
  encode/decode round-trip (`BcEncode.h:59-64`).
- `foxab_probe` console harness in CMake (`CMakeLists.txt:343-366`), off by default.

**D4** — no container path and no regression driver staged:

- The `.bat` suite the template describes (`rebuild.bat`, `Audit - Asset Health.bat`,
  `github.bat`, `release-notes.py`, `ASSETBROWSER_TEMPLATE.md:298-302, 803-810`) is not staged.
  Verification is `verify-src.py` + the user rebuilding (`d4/CLAUDE.md` rule 3) + env-gated
  dumps (`D4_DUMP_*`, `d4browser-codebase` skill convention 7). `QueryTerm::selfTest` runs at
  startup (`main.cpp:232`); `BcDecode::selfTest` only when the Wardrobe tab is built
  (`WardrobeTab2.cpp:1254-1258`). Corpus sweeps exist in-process (`MatSnoSweep.h:38-52`
  chain test + asset health audit with diff against the previous run) but are env-triggered.

**What a core `conformance` target could be built from:**

1. **A registry of `QString selfTest()` functions** — POE2 already has ten, FOX five, D4 two.
   Core owns `QueryTerm`, `ExportLayout`, `NameTemplate`, BC decode, glTF writer self-tests;
   each plugin registers its format self-tests. One `ctest` executable runs them without a
   display.
2. **A Linux compile of core + every plugin's format library** — replacing POE2's hand-listed
   `verify_container.sh` with the real CMake graph (the drift above is the argument).
3. **Headless render** — generalise `render_offscreen.cpp` to take core's `ModelGeometry` +
   material set from any plugin, and make it include the viewport's shader source from the
   repo (one `shaders.inc` generated from `GLModelWidget`), so what it proves is what ships.
4. **Fixture trees, not installs** — FOX's `tools/make_chara_tree.py` and `_probe/*.tgz`
   pattern (per the skill amendment) and POE2's "I staged the 3 data-table bundles" note
   (`poe2/CLAUDE.md`, names section) show both projects already exercise real formats on
   partial pulls. A core `conformance/fixtures/<game>/` convention with a per-plugin
   "minimal install" would let the FOX stdout-needle checks run under `xvfb-run` for every
   game, using the harness-flag pattern (`--tab/--shot/--export`) as the core CLI.
5. **Byte-identical exports** across runs (`NEW_SESSION_PROMPT.md:40-43` lists the two
   standing md5s) — a core test that exports a fixture model twice and compares.

---

## 10. Session kickoff

| | D4 | FOX | POE2 |
|---|---|---|---|
| Entry file | `CLAUDE.md`, **39 lines**: one-paragraph identity + build loop; 7 working rules; 9 hard conventions with the bug behind each; domain constants and repro assets. Points to `PHYSICS_AUDIT.md`, `PHYSICS_HARNESS_PROMPT.md` and three skills. | `NEW_SESSION_PROMPT.md`, **1,279 lines**: identity + pointer to the `foxab-codebase` skill and `MGO_FACTS.md` (:1-9); two permanent user instructions (:13-26); the verification bar (:28-51); a delivery protocol with per-file md5 read-back and a batch-letter counter (:55-80); "Where this stands" with a tree hash and batch history (:84-227); a **work queue** of six items with evidence and instrumentation plans (:230-613); then ~660 lines of per-batch reference (9j–9q sections, work items, standing debts :615-1279). No `CLAUDE.md`. | `CLAUDE.md`, **292 lines**: identity + container build/xvfb note (:1-12); ground-truth pointer to `docs/FORMATS.md`; 5 family rules; layout table; then a very long **"Next work"** section that is in practice a DONE log — every feature landed with its verification evidence, running to ~250 lines. |
| Skills | `d4browser-codebase` (85 lines: source map, build loop, 11 conventions, Qt/MSVC pitfalls, verification, research discipline), `d4browser-gamedata` (217), `d4browser-debugging` (36), `d4browser-changelog` (104) | `foxab-codebase` — **as synced it is a 57-line amendment**, its frontmatter `description` is literally "Add the container test tree that has .fv2/.fcnp/.mtar, the 8w–9b harness flags, and two new scars…" and its body opens "Add these three sections" (skill file lines 1-8). The full skill the prompt refers to (:7) is not what is installed. `mgsv-fox-formats` (110) covers the wiki. | none of its own; relies on `assetbrowser-design` (40 lines, the template summary) — which itself says "if it is not in this session's files, ask the user to attach it" |
| Family doc | `docs/ASSETBROWSER_TEMPLATE.md` 910 lines, 4 tiers, U/A/E marks; `docs/README.md` indexes 5 design docs + investigations | `docs/ASSETBROWSER_TEMPLATE.md` **373 lines — an older pre-tier revision** (no `###` sections; header names "DIAssetBrowser" as a sibling) and a FOX-specific `CONTEXT_MENUS.md` | `docs/D4_REUSE_AUDIT.md` (61 lines) and `FORMATS.md` (567); the CLAUDE.md points at "the sibling D4 repo" for the template |
| How the user kicks off | Skill triggers + `CLAUDE.md` auto-load; work stated per prompt | The user pastes/attaches `NEW_SESSION_PROMPT.md`; the session reads the queue and the batch counter ("Next letter after 12f is 12g" :80) and is expected to refresh the file at the end (the `session-handoff` skill exists for this) | `CLAUDE.md` auto-load; the session reads the DONE log to know what exists |

Observations for a plugin kickoff, given a core that carries the generic rules:

- The **generic rules are already duplicated three times** in prose: D4 `CLAUDE.md` rules 1–7
  and the conventions, POE2 `CLAUDE.md` "Non-negotiable working rules (from the family)" 1–5,
  FOX "The verification bar", and the `assetbrowser-design` skill step 3 all restate one-key /
  fail-closed / classify-by-authored-data / one matcher / verify-before-replying. In a monorepo
  these belong in one root `CLAUDE.md` (or the `assetbrowser-design` skill) and nowhere else.
- What is **genuinely per-game** and should be all a plugin kickoff contains:
  1. Identity and where the data is (D4: game dir + d4data + keys paths, `CLAUDE.md` domain
     constants; FOX: the four games and the `dict/` rule; POE2: `Bundles2` and `FORMATS.md`).
  2. The **ground-truth document** for formats (`FORMATS.md`, `MGO_FACTS.md`, D4's
     `d4browser-gamedata` skill + `docs/notes/STATUS.md`) and the instruction that it outranks
     intuition.
  3. **Refuted hypotheses and scars** specific to that engine (D4 rule 4 + `PHYSICS_AUDIT.md`;
     FOX scars 16/17 in the skill amendment; POE2's "vertex explosion" and stale-cache lessons).
  4. Repro assets (D4 `CLAUDE.md` last line; FOX regress fixtures; POE2 named meshes).
  5. The plugin's own verify tables: `HEADER_ONLY` entries, name-token baseline, dead-key
     allow-list (§8).
  6. Env-gated diagnostics and harness flags the plugin adds (`D4_DUMP_*`, FOX's 247 options,
     POE2's tools).
  7. The **work queue** — and only the queue. FOX's file works because :230-613 is a real
     queue; it is 1,279 lines because the batch history behind it was never moved out, and
     POE2's `CLAUDE.md` has the same growth pattern in its DONE log. A per-plugin
     `NEXT_SESSION.md` (queue + last tree hash) separate from the plugin `CLAUDE.md`
     (stable facts) would keep both readable.
- Two hygiene defects to fix regardless of the monorepo: the FOX skill is an amendment
  installed as the skill, and FOX's copy of the family template is stale relative to D4's.

---

## Appendix — cross-project quick facts used above

- Store read entry points: `d4/src/casc/CascReader.h:74-79`, `fox/src/index/ArchiveIndex.h:162-168`, `poe2/src/store/AssetStore.h:52-53`.
- Fingerprints: `CascReader.h:60,180`, `ArchiveIndex.h:228`, `BundleIndex.h:52`.
- Geometry contracts: `d4/src/model/ModelGeometry.h:189-217`, `poe2/src/model/ModelGeometry.h:44-70`; FOX has none (`fox/src/preview/ModelLoader.h:68-81`, `fox/src/gl/GLModelWidget.h:39`).
- BC decoders: `d4/src/tex/BcDecode.cpp` (358 lines, hand-written), `fox/src/fox/BcDecode.cpp` (393, hand-written) + `BcEncode.cpp` (443), `poe2/src/tex/BcdecImpl.cpp` (4, vendored bcdec) + `DdsImage.cpp` (186).
- Online data: D4 only (`D4DataDownloader.cpp:11`, `SettingsDialog.cpp:67-79`, `MainWindow.cpp:997`); FOX ships `dict/` (`AppPaths.h:81-88`); POE2 none (`CMakeLists.txt:14-15`).
- Dead build deps: `d4/CMakeLists.txt:28-31,194` (fastgltf/tinygltf), `d4/vcpkg.json` lines listing them.
- Drifted duplicates: `poe2/tools/verify_container.sh:24-35` vs `poe2/CMakeLists.txt:35-101`; `poe2/tools/render_offscreen.cpp:24` (out-of-repo shader copy); `fox/docs/ASSETBROWSER_TEMPLATE.md` (373 lines) vs `d4/docs/ASSETBROWSER_TEMPLATE.md` (910).
