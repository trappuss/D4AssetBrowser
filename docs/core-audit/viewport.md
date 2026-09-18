# Viewport & GL layer audit — D4 / Fox / POE2

Scope: `d4/src/gl/*`, `d4/src/app/ViewportSettings.h`, `d4/src/util/ViewportPartMenu.h`, `d4/src/util/CameraOrbitRow.h`;
`fox/src/gl/*`, `fox/src/view/Viewport*.{h,cpp}`, `fox/src/view/DisplayModeButton.*`, `fox/src/view/RenderPanel.*`, `fox/src/util/PartMenu.*`;
`poe2/src/gl/*`. Template authority: `d4/docs/ASSETBROWSER_TEMPLATE.md` §2 (168-197), §5 (276-283), §10 (366-383), §11 (385-413), §22 (679-692).

Every claim cites `file:line`. Items marked **HUNCH** are inferred from reading rather than observed at runtime.

Sizes (wc -l): D4 `GLModelWidget.h` 856 / `.cpp` 6727; Fox `.h` 1138 / `.cpp` 3677; POE2 `.h` 201 / `.cpp` 1010.
GL profile: D4 **4.5 core** (`d4/src/gl/GLModelWidget.h:13,31`, `d4/src/main.cpp:163-164`, shader `#version 450 core` at `d4/src/gl/GLModelWidget.cpp:230,285`); Fox **3.3 core** (`fox/src/gl/GLModelWidget.h:14,366`, shaders `fox/src/gl/GLModelWidget.cpp:23,55`); POE2 **3.3 core** (`poe2/src/gl/GLModelWidget.h:9,23`, `poe2/src/main.cpp:33-34`).

Note on scope: `fox/src/view/DisplayModeButton.*` is the **list** display switch (List / Outliner / Grid — `fox/src/view/DisplayModeButton.h:1-3`), not a viewport control; it is listed here only to record that it is out of the viewport's remit.

---

## 1. Feature matrix

Legend: ✔ present · ✘ absent · ◐ partial / lives outside the widget. "Where" gives the declaration.

### 1.1 Camera

| Feature | D4 | Fox | POE2 |
|---|---|---|---|
| Orbit (left drag) | ✔ `d4/src/gl/GLModelWidget.cpp:5713-5718` (0.01 rad/px, pitch clamp ±1.553 rad) | ✔ `fox/src/gl/GLModelWidget.cpp:3267-3273` (0.5 °/px, clamp ±89°) | ✔ `poe2/src/gl/GLModelWidget.cpp:888-892` (0.01 rad/px, clamp ±1.55; latched drag threshold 3px) |
| Pan | ✔ **right** drag `d4:5719-5732` (scale dist·0.0015) | ✔ **middle or right** drag `fox:3274-3295` (dist·0.0022) | ✔ **middle** drag or **Alt+right** `poe2:893-900` (dist·0.0015) |
| Zoom (wheel) | ✔ `d4:6719-6727` factor 0.9^steps, min 0.05·r, **no max** | ✔ `fox:3669-3677` 0.88^steps, clamp [0.05r, 40r] | ✔ `poe2:941-945` ×1.15 per notch, clamp [0.05r, 40r] |
| Middle click | reset view **on press** `d4:5656-5659` | reset camera on release if not dragged `fox:3228-3229` | ✘ (middle = pan only) |
| Orthographic | ✔ `d4/src/gl/GLModelWidget.h:348-349` | ✔ `fox/src/gl/GLModelWidget.h:645-647` (gizmo double-click) | ✘ (fixed perspective `poe2/src/gl/GLModelWidget.cpp:629-633`) |
| FOV | ✔ `d4.h:171` setFov (10–100) | ✔ `fox.h:839,847` setFieldOfView (15–100) | ✘ fixed 45° `poe2.cpp:632` |
| Camera state save/restore | ✔ `CamState` `d4.h:351-354` | ✔ `CameraPose` + toString/fromString `fox.h:623-632`; named presets in **QSettings inside the widget** `fox.cpp:1500-1550` | ◐ orbitYaw/orbitTarget only `poe2.h:92-96` |
| Axis views (front/back/…) | ◐ `orbitToAxis(yaw,pitch)` `d4.h:422` driven by gizmo | ✔ `applyCameraPreset("front"…)` `fox.h:636-637`, `viewAlongAxis` `fox.h:650` | ✘ |
| Frame all / reset | ✔ `resetView` `d4.h:328`, `frameAll(keepRotation, animate)` `d4.h:331` | ✔ `resetCamera` `fox.h:478`, `centerOn` `fox.h:750` | ✔ `frameAll` `poe2.h:70` |
| Frame selected part(s) | ✔ `frameRegion*`/`partsBounds` on **live posed** verts `d4.h:337-343`, `d4.cpp:5774-5800` | ✔ `frameMesh(meshId)` on **bind** bounds through group transform `fox.h:850`, `fox.cpp:2991-3040` | ✔ `frameSelected` on bind verts `poe2.h:71`, `poe2.cpp:964-980` |
| Smooth camera glide | ✔ `m_camAnim` `d4.h:843-845` | ✘ | ✘ |
| Follow parts each anim frame | ✔ `followParts` `d4.h:346` | ✘ | ✘ |
| Auto-fit on load rule | tab-side `models/autoFrame` `d4/src/tabs/ModelsTab.cpp:8266-8269`; widget `keepView` flag `d4.h:40` | in-widget heuristic (first scene / user placed / radius ratio 6× / centre moved 3r) `fox.cpp:1018-1072`, `setAutoFit` `fox.h:845` | always re-fit on `setModel` `poe2.cpp:209-215` |
| Numeric yaw/pitch controls | ✔ `setOrbitAngles` `d4.h:360` + shared `util/CameraOrbitRow.h:63` | ✔ via `setCameraPose` + `cameraChanged` signal `fox.h:632,734` | ✘ |

### 1.2 Shading modes & channel viewer (template §10, §22)

| Feature | D4 | Fox | POE2 |
|---|---|---|---|
| Shading mode as ONE state | ✘ — widget exposes `setWireframe` `d4.h:393`, `setPbr` `d4.h:152`, `setShowTextures` `d4.h:387`; the four "balls" are composed **in each tab** (`d4/src/tabs/ModelsTab.cpp:2056-2095`, `WardrobeTab2.cpp:1862-1897`, `StableTab2.cpp:452-479`) | ✔ `enum class ShadingMode {Wireframe, Flat, Shaded, Rendered}` `fox.h:360`, `setShadingMode` `fox.h:390`; legacy `setWireframe`/`setPbrShading` are views of it `fox.cpp:1178-1192` | ◐ `enum Shading {Flat, Shaded, Wireframe}` `poe2.h:29` — **no Rendered**; driven by a QComboBox `poe2/src/tabs/ModelsTab.cpp:160,255` |
| Wireframe | ✔ `glPolygonMode` `d4.cpp:4297` | ✔ `fox.cpp:2443-2444` (never in pick pass) | ✔ `poe2.cpp:708-709` |
| Flat | ◐ "Flat: base colour only" = `setPbr(false)` → **two-sided Lambert, still lit** `d4/src/tabs/ModelsTab.cpp:2072,2083`, shader `d4.cpp:826` | ✔ = unlit Albedo channel `fox.cpp:2471-2479` | ◐ `uShading==0` → `diffuse*(0.4+0.6·N·V)` "unlit-ish" `poe2.cpp:141` |
| Shaded | ✔ PBR, post off (`ModelsTab.cpp:2073,2084-2094`) | ✔ base+normal lambert, no PBR maps `fox.cpp:1165-1168` | ✔ metal-rough GGX two-light `poe2.cpp:120-143` |
| Rendered | ✔ PBR + IBL + shadows + SSAO + tonemap (tab flips 4 feature keys `ModelsTab.cpp:2085-2093`) | ✔ full SRM/TRM/FMTT/rig `fox.cpp:1170-1173`, `m_pbrShading` `fox.cpp:2703` | ✘ |
| Channel viewer | ✔ `setViewChannel(int)` `d4.h:392` — 0 shaded·1 base·2 normal·3 rough·4 metal·5 AO·6 emissive·**7 detail-select·8 dye zones** (shader `d4.cpp:781-812`); tab combo lists only 0–6 `ModelsTab.cpp:2028-2031` | ✔ `fox::DebugView` `fox/src/gl/ViewEnvironment.h:84-100` — Off/Albedo/Normal/Roughness/ReflectionMask/Occlusion(=spec mask)/Translucency/Metalness/UV/LightingOnly/Dirty/DirtSplats; names `ViewEnvironment.cpp:204-222` | ✔ `enum Channel {BaseColor=0(=lit!), Normal, Roughness, Metallic, AO, Emissive, None(=flat base)}` `poe2.h:30`; shader `poe2.cpp:107-116`; combo labels `poe2/src/tabs/ModelsTab.cpp:161` |
| Channel cycled by wheel on ⌄ | ✔ tab eventFilter `ModelsTab.cpp:2096-2106` | ✔ `ViewportBar::scrollChannelForShot` `fox/src/view/ViewportBar.h:108` (non-wrapping) | ✘ |

### 1.3 Overlays

| Overlay | D4 | Fox | POE2 |
|---|---|---|---|
| Master gate location | **tab** (`reapplyOverlays` `d4/src/tabs/ModelsTab.cpp:8339`, `WardrobeTab2.cpp:10356`, `StableTab2.cpp:5337`; `m_overlaysOn` gating at `ModelsTab.cpp:2177-2180`) | **ViewportBar** (`ViewportOverlays` struct `fox/src/view/ViewportBar.h:40-64`, `reapplyOverlays` `ViewportBar.cpp:363-384`) | **widget** (`setOverlaysOn` `poe2.h:61`, checked in `paintGL` `poe2.cpp:648,656`) |
| Ground grid | ✔ `setShowGrid` `d4.h:394`, `buildGrid` `d4.cpp:6680`; X/Z axis tint `setGridAxisColors` `d4.h:424` | ✔ `setShowGrid` `fox.h:397` (unit grid scaled per draw `fox.h:1028-1034`) | ✔ `setShowGrid` `poe2.h:62`; auto-sized 1/2/5 step, sits at model's lowest Y `poe2.cpp:325-365` |
| Skeleton lines | ✔ `setShowSkeleton` `d4.h:395`, `buildSkeleton` `d4.cpp:6256` | ✔ `setShowSkeleton` `fox.h:450`; reposed lines via `applyPose(...,skeletonLines)` `fox.h:745-746` | ✔ `setShowSkeleton` `poe2.h:63`; **bind pose only** (built once in `rebuildBuffers` `poe2.cpp:428-442`) |
| Statistics | ◐ tab-side QLabel `m_statsOv` + `updateStatsOverlay` `d4/src/tabs/ModelsTab.cpp:2187-2192,8580` | ✔ `setShowStats`/`statsText()` `fox.h:405-406,421`, painted by `ViewportHud` | ◐ `setShowStats` `poe2.h:64` only **emits** `statsText` signal to a status bar `poe2.h:123`, `poe2.cpp:1005-1009` — nothing drawn over the viewport |
| Bone names | ✔ `setShowBoneNames` `d4.h:400`, translated/hide-unknown `d4.h:401-402`, `BoneLabelOverlay` (file-local QWidget) `d4.cpp:204` | ✔ `setShowBoneNames` `fox.h:407`, `boneLabelsOnScreen()` `fox.h:424` → HUD | ✘ |
| Axis gizmo (clickable) | ✔ `setShowAxisGizmo` `d4.h:423`; `AxisGizmoOverlay` file-local class `d4.cpp:42-203` | ✔ separate `fox::ViewportGizmo` widget `fox/src/view/ViewportGizmo.h:36` (also toggles ortho on ring dbl-click) | ✘ |
| Axis triad (lines) | ✘ (grid axes tinted instead) | ✔ `setShowAxes` `fox.h:403` | ✘ |
| Hardpoints / sockets | ✔ `setHardpoints(QVector<ModelHardpoint>)` + `setShowHardpoints` `d4.h:396-397`, RGB gizmo + label `d4.cpp:6319,6386` | ✔ `setConnectPoints(QVector<GLConnectPoint>)` + `setShowConnectPoints` `fox.h:409-414` (crosses + labels; hover/click pick `fox.h:583-588`) | ✘ |
| Collision shapes | ✔ `setShowColliders` `d4.h:306`, `buildColliderLines` `d4.cpp:6600` | ✘ | ✘ |
| Physics bones | ✔ `setShowPhysBones`/`setShowPhysAxes` `d4.h:398-399`, `buildPhysBones` `d4.cpp:6435` (grey/orange/yellow/red contact classes `d4.h:792-796`) | ✘ | ✘ |
| Root-motion path / ghost skeletons | ✘ | ✔ `setRootPath`, `setGhostSkeletons` `fox.h:601-608` | ✘ |
| Selection outline as gated overlay | ✘ (always drawn) | ✔ `setShowSelection` `fox.h:784` | ✘ (tint always drawn) |
| Help (F1) overlay | ◐ MainWindow dialog `d4/src/app/MainWindow.cpp:483-484` | ✔ `setShowHelp` `fox.h:856`, drawn by HUD `fox/src/view/ViewportHud.cpp:106` | ✘ |
| "Loading…" / empty-state text | ✔ `setOverlayText` `d4.h:86` | ✘ | ✘ |

### 1.4 Part picking & selection (template §11)

| Feature | D4 | Fox | POE2 |
|---|---|---|---|
| Where selection truth lives | **parts panel**; widget only mirrors via `setHighlightParts` (red) `d4.h:67` and `setPickedParts` (blue) `d4.h:71` | **widget** `m_selected`/`m_picked` `fox.h:1004-1005`, `m_context` `fox.h:1006` | **widget** `m_selected` `poe2.h:190` |
| Pick method | CPU Möller–Trumbore raycast over **posed** `m_verts` `d4.cpp:5840-5903` (`pickPart` public `d4.h:385`); hidden parts skipped `d4.cpp:5879` | GPU colour-id: re-runs `paintGL` with `m_picking` into an FBO `fox.cpp:2849-2904`; invisible meshes skipped `fox.cpp:2548-2549`; glass drawn opaque in pick `fox.cpp:2519` | GPU colour-id with a dedicated `m_pickProg` `poe2.cpp:845-874`; hidden parts skipped `poe2.cpp:861`; **attachments not pickable** (body VAO only `poe2.cpp:859-865`) |
| Single click picks / empty clears | ✔ on **release**, <4px `d4.cpp:5680-5686` → `partClicked(part, mods)` `d4.h:432`; tab does ClearAndSelect `d4/src/tabs/ModelsTab.cpp:3314-3338` | ✘ **bare left click does not select** — only Ctrl/Shift-click on **press** `fox.cpp:3212-3220`; a bare left release picks a **bone/socket overlay** `fox.cpp:3234-3243`; selection by plain click is on **double-click** `fox.cpp:3105-3113` | ✔ on release if not latched-dragging `poe2.cpp:907-916` |
| Ctrl / Shift | both **toggle** (tab) `ModelsTab.cpp:3316,3331` | **Ctrl toggles, Shift adds** `fox.cpp:3090-3103` | both **toggle** `poe2.cpp:909-910` |
| Double-click frames, no selection change | ✔ `d4.cpp:5691-5707` (frames only if QSettings `viewer/framePartOnPick`, emits `partFocused`; tab only reveals in outliner `ModelsTab.cpp:3301-3309`) | ✘ double-click **selects** (`applyPickGesture`) and does **not** frame `fox.cpp:3105-3113`; framing is the `.` key `fox.cpp:3145,3186` | ✔ emits `partDoubleClicked` `poe2.cpp:933-939`; tab calls `frameSelected()` `poe2/src/tabs/ModelsTab.cpp:262`; empty space → `frameAll` |
| Swallow-flag scar (§11 scar 1) | ✔ `m_swallowLeftClick` `d4.h:853`; set in dbl-click `d4.cpp:5693`, consumed on release `d4.cpp:5684`, **cleared on every press** `d4.cpp:5652` | ✘ no flag (selection is on press/dbl-click, not release). **HUNCH:** Ctrl+double-click = press toggles (`fox.cpp:3215-3218`) then DoubleClick toggles again (`fox.cpp:3111`) → net no-op, the exact toggle-twice shape the scar describes | ✔ `m_swallowNextRelease` `poe2.h:187`; set `poe2.cpp:935`, consumed `poe2.cpp:905`, cleared on press `poe2.cpp:879` |
| Clear picked set on geometry change (§11 scar 3) | ✔ `d4.cpp:1222-1227` | ✘ `setModel` clears visibility/pose/transforms `fox.cpp:947-952` but **not** `m_selected`/`m_picked`/`m_context` (only cleared by `clearSelection` `fox.cpp:2965-2971`). **HUNCH:** stale id in range outlines the wrong submesh after a rebuild unless the tab clears | ✔ `poe2.cpp:194,227` |
| Selection visual | stencil-jitter **outline**, red highlight + blue picked, wireframe fallback without stencil `d4.cpp:4524-4637` | stencil-jitter **outline** ported from D4, **orange** select + blue context `fox.cpp:2906-2913,3333-3358` | **flat 35 % tint** `poe2.cpp:148` — destroys the texture being inspected (the very thing `d4.h:64-65` warns against) |
| Right-click scoping (§11) | ✔ in-selection → whole; outside → **replaces** selection and syncs parts table; blue = acted set `d4/src/tabs/ModelsTab.cpp:7229-7259` | ✘ deviates: outside → subject is that part only, **selection untouched**; on the sole selected part or empty space → **clears** selection `fox/src/export/ViewCapture.cpp:865-890` | ✔ outside → replaces; inside → whole; empty leaves alone `poe2.cpp:917-929` |
| Right-drag ≠ click | ✔ <4px manhattan on release `d4.cpp:5666-5667` | ✔ >4px sets `m_rightDragged`, `event()` swallows `QEvent::ContextMenu` `fox.cpp:3283-3313`; harness `testRightDrag` `fox.h:806-807` | ✔ latched `m_dragging` `poe2.cpp:920` |
| Per-part hide / isolate | ✔ `setPartVisible/partVisible` `d4.h:62-63`; H / Shift+H / Alt+H in tab eventFilter `d4/src/tabs/ModelsTab.cpp:9994-10019` | ✔ `setMeshVisible/hiddenMeshes/meshIds` `fox.h:432-443`; `hidePicked/unhideAll/isolatePicked` `fox.h:861-863`; hotkeys H / Alt+H / Shift+H from registry `fox/src/app/Hotkeys.h:111-124` | ✔ `setHiddenParts/isolateParts/clearHiddenParts` `poe2.h:78-81`; **no hotkeys**; no "hide" in menu, only isolate/show-all `poe2/src/tabs/ModelsTab.cpp:575,587` |
| Selection mirrored to parts panel | ✔ panel→view `setHighlightParts` `ModelsTab.cpp:3046,3242`; view→panel `partClicked` `ModelsTab.cpp:3314` | ✔ `setSelectedMeshes` `fox/src/tabs/ModelsTab.cpp:1084`; `meshPicked` `fox/src/tabs/ModelsTab.cpp:675` | ✔ `selectionChanged`/`setSelectedParts` `poe2/src/tabs/ModelsTab.cpp:260,264,631-649` |
| Right-click part menu builder | ✔ shared header-only `ViewportPartMenu::exec(Info, Actions)` `d4/src/util/ViewportPartMenu.h:204-344`; labels count sets `:211-231,247-258`, MenuText vocabulary `:45-126` | ✔ shared `partmenu::build(QMenu*, Context)` `fox/src/util/PartMenu.h:33-71`; viewport entry in `ViewCapture.cpp:908-949`, tree entry `SceneTree.cpp:217` | ✘ ad-hoc menu in tab `poe2/src/tabs/ModelsTab.cpp:564-597` (Frame/Isolate/Export/Copy names/Show all/Select all/Clear); no tri counts, no Hide, no last-folder export |

### 1.5 Capture / export hooks

| Feature | D4 | Fox | POE2 |
|---|---|---|---|
| renderToImage / offscreen larger | ✔ `grabSupersampled(factor)` `d4.h:53`, `d4.cpp:5450-5500` (redirects all passes via `targetFbo()` `d4.h:620`; degrades factor to fit `GL_MAX_TEXTURE_SIZE`; CombinedDepthStencil so outline survives) | ✔ `renderAtSize(w,h)` `fox.h:677`, `fox.cpp:1789-1831` (cap 8192; RAII restore; no MSAA in FBO `fox.cpp:1800`) | ✔ `renderToImage(scalePercent 25–400, transparentBg, cropToModel)` `poe2.h:88`, `poe2.cpp:795-843` (4× MSAA FBO `poe2.cpp:812`; **asset only, no overlays** `poe2.cpp:672-673`) |
| Transparent background | ✔ `setTransparentClear` `d4.h:411`; also **coverage alpha** mode `setCoverageAlpha` `d4.h:414`, `d4.cpp:3908-3912` | ✔ `setTransparentBackground` `fox.h:683` (needs alpha bits, `fox/src/main.cpp:60-72`) | ✔ parameter of `renderToImage` `poe2.cpp:819` |
| Thumbnail | ✔ `grabThumbnail(size)` `d4.h:47` on the widget (persistent FBO `d4.h:622-623`); `grabEnsembleThumb` `d4.h:57` | ◐ separate `fox::ThumbnailRenderer` singleton with its **own thread/context** `fox/src/gl/ThumbnailRenderer.h:62-91,93-183`, disk cache `:129-141` | ◐ separate `ModelThumbnailRenderer` (own offscreen context, **GUI thread only**) `poe2/src/gl/ModelThumbnailRenderer.h:17-28` |
| Turntable hooks | ✔ `orbitYaw()/setOrbitYaw` `d4.h:355,383`; `setFrame` `d4.h:215`; `animFrameRate/animFrameCount` `d4.h:237,242`; `CaptureScope`/`setCaptureMode`/`settleCloth`/`setCaptureTime` `d4.h:365-382`; driven by `d4/src/app/ExportCapture.cpp:341-399` | ✔ widget owns the loop: `renderTurntable(frames)` `fox.h:710`, `fox.cpp:1833-1857`; anim frames via installed `AnimFrameProvider` `fox.h:824-833`; **no public yaw setter** (deliberate, `fox.h:698-704`) | ✔ `orbitYaw/setOrbitYaw/modelCenter/orbitTarget/setOrbitTarget` `poe2.h:92-96`; `setAnimTime/clipDuration/currentClipFps` `poe2.h:112-118`; driven by `poe2/src/app/ExportCapture.cpp:227-241` |
| Auto-spin / turntable live | ✔ `setAutoSpin/setSpinSpeed` `d4.h:326-327` (rad/tick) | ✔ `setTurntable(on, degPerSec)` `fox.h:665` (clock-stepped, ±120 °/s) | ✘ |

### 1.6 Rendering pipeline

| Feature | D4 | Fox | POE2 |
|---|---|---|---|
| Skinning | **CPU** `applySkinning` `d4.cpp:3134-3272` (pose composed inside the widget from `AnimParser::DecodedAnim` `d4.h:213,471`; multi-clock attached ranges `d4.h:221-236`) | **CPU** `applyPoseBuffers` `fox.cpp:2267-2372`; pose palette computed **by the tab**, handed as `QVector<animmath::Mat4>` `fox.h:745` | **CPU** `fillVertexBuffer(verts, skin)` `poe2.cpp:367-405`; palette from `AstSkeleton::skinMatrices` called **inside** the widget `poe2.cpp:297`; wall-clock playback in widget `poe2.cpp:279-289` |
| Two-pass alpha | ◐ opaque pass with **alpha-to-coverage** cutout `d4.cpp:4323-4335`; then blended **FX pass** (per-part additive or alpha) `d4.cpp:4640-4654`, `drawFxParts` `d4.cpp:4894-4932` (`glBlendFuncSeparate` `:4911-4912`) | ◐ discard cutoff `fox.cpp:292,332`; **glass** materials held to a far-to-near sorted blend pass `fox.cpp:2501-2547,2708-2714`; no additive | ✔ `AlphaMode` 0 Opaque/1 Mask/2 Blend/3 Additive `poe2.cpp:61,75`; pass 1 opaque+mask, pass 2 blend/additive `poe2.cpp:765-783`; not depth-sorted `poe2.cpp:771` |
| IBL | ✔ hemisphere + optional **real D4 cubemap probe** `setReflectionCubemap` `d4.h:196`, `uReflCube` `d4.cpp:382-385`, feature toggle `setFeatureIbl` `d4.h:158` | ◐ analytic hemisphere sky/ground in `ViewEnvironment` `fox/src/gl/ViewEnvironment.h:50-51` (no cubemap) | ✘ constant ambient `poe2.cpp:142` |
| Shadows | ✔ key-light shadow map `setShadowEnabled/Params/Extra` `d4.h:204-206`, `renderShadow` `d4.cpp:3789`, PCF `d4.cpp:449-461` | ✘ | ✘ |
| SSAO | ✔ position G-buffer prepass `renderPos` `d4.cpp:3740`, `setSsaoEnabled/Params` `d4.h:209-210`, shader `d4.cpp:468-497` | ✘ | ✘ |
| Tonemap | ✔ **ACES** optional (`setFeatureTonemap` `d4.h:160`, shader `d4.cpp:1031`), + colour grade / LUT `d4.h:169-170` | ✔ **Khronos PBR Neutral**, always when `linearColor` on `fox.cpp:199-224,237-239`, `setLinearColor` `fox.h:688` | ✔ **Reinhard**, unconditional `poe2.cpp:146-147` |
| Lighting rig | ✔ 3-point camera-relative `LightRig` `d4.h:180-190`, presets, lock-to-world `d4.h:207` | ✔ data-driven `fox::ViewEnvironment` presets, per-game auto `fox.h:492-506`, key az/el `fox.h:511-519` | ✘ two hard-coded directionals `poe2.cpp:127-128` |
| Backface cull | ✔ on by default, toggle `d4.h:323`, `d4.cpp:4301-4302` | ✘ never enabled (`fox.cpp:3410,3496` only disable) | ✘ |
| MSAA | ✔ 4× default surface `d4/src/main.cpp:165` (+ stencil 8 `:167`); `paintGL` queries `GL_SAMPLES` for A2C `d4.cpp:4309-4312`; capture FBO single-sampled | ✘ no `setSamples` on the window (`fox/src/main.cpp:57-73` asks depth/stencil/alpha only); capture FBO explicitly 0 `fox.cpp:1800` | ✔ 4× surface `poe2/src/main.cpp:36`; 4× capture FBO `poe2.cpp:812` |
| Fullscreen | ◐ **per-tab** maximize-in-place: `ModelsTab::toggleFullscreen` `d4/src/tabs/ModelsTab_Panels.cpp:995-1043` (✕ Exit button `:1035`), `StableTab2::toggleFullscreen` `StableTab2.cpp:1123`, Wardrobe `WardrobeTab2.cpp:1288` — three copies | ✔ widget flag + `fullscreenChanged` `fox.h:853-854,884`; host implements (`fox/src/tabs/ModelsTab.cpp:672,709`); Exit button in HUD `fox/src/view/ViewportHud.cpp:72-86`; Esc `fox.cpp:3121-3127` | ✘ |
| Attachments / assembly | ◐ outside the widget: tab merges skeletons (`ModelParser::mergeGeometries`), widget only gets `setAttachAnimRanges` `d4.h:227` + `pinnedBones` `d4.h:516` | ✔ rigid `setGroupTransform(groupId, QMatrix4x4)` `fox.h:446` (attachment rides its parent bone through it) | ✔ `setAttachments(QVector<Attachment>)` `poe2.h:48-52` — widget bakes verts at the parent bone `poe2.cpp:541-599`, re-bakes per anim frame `poe2.cpp:605-622` |
| Cloth sim | ✔ ~2 000 lines: `ClothParams` `d4.h:252-305`, `buildSpringBones` `d4.cpp:1589`, `springBoneStep` `d4.cpp:2209`, `buildClothSim` `d4.cpp:3273`, cages `d4.h:558-575` | ✘ | ✘ |
| Fur / hair / eye / dye / FX / detail maps | ✔ all in-widget: fur `d4.h:141-150`, hair `d4.h:126,157`, eye `d4.h:127-129`, dye `d4.h:161-166`, FX `d4.h:131-139`, detail `d4.h:92-118` | ◐ hair lobe, eye parallax, layer/mask, dirt, glass, MTM presets — all in `GLPbrMaterial` `fox.h:75-320` | ◐ FurV2 preview `poe2.h:38-39`, SSS/translucent/spec-colour `poe2.cpp:60,122` |
| SEH guarding of GL submissions | ◐ **not in the widget**: tabs wrap `setGeometry+repaint` in `seh::runGuarded("render")` `d4/src/tabs/ModelsTab.cpp:8262-8272`, `"stableGpu"` `StableTab2.cpp:2243`, `"w2Gpu"` `WardrobeTab2.cpp:9056`, thumbnails `ModelsTab.cpp:8319`, `CatalogueTab.cpp:2401` | ◐ translator installed `fox/src/main.cpp:77`; only the **thumbnail worker** is guarded `fox/src/gl/ThumbnailRenderer.cpp:108-110`; on-screen widget relies on RAII scope guards to survive an unwind (`fox.cpp:1809-1823,2876-2885`) | ✘ `app/SehGuard.{h,cpp}` compiled (`poe2/CMakeLists.txt:50`) but **never called** anywhere (`grep runGuarded poe2/src` → only the definition) |

---

## 2. Public API of each `GLModelWidget`

### 2.1 D4 (`d4/src/gl/GLModelWidget.h`)

**Geometry / parts**
`setGeometry(const ModelGeometry&, bool keepView)` :40 · `clearGeometry()` :41 · `partCount/partTriangles/setPartVisible/partVisible` :60-63 · `setHighlightPart(s)` :66-67 · `setPickedParts/setPickedPart/pickedPart/pickedParts` :71-76 · `pickPart(QPoint)` :385 · `partsBounds` :343 · `followParts` :346 · `snapshotPose(ModelGeometry&)` :241 · `setOverlayText` :86 · `baseBoneCount` :417.

**Materials (per part, D4-specific)**
`setPartTextures/Normals/Orm/Emissive/EmissiveMult/EmissiveColor` :79,87-91 · detail maps `setPartDetail*`, `DetailConfig`, `setDetailConfig` :92-118 · `setPartTranslucency/Mask/DyeMask/DyeRamp/DyeRegion` :119-123 · `setPartFlags(hair,skin,cloth)` :124 · `setPartHairParams/Eye/Head`, `setEyeParams`, `setEmissiveScale` :126-130 · FX `setPartFx/FxNoise/FxAdditive/FxParams`, `setFxIntensity/ScrollSpeed/Wobble` :131-139 · fur `setPartFur/FurMask/FurNoise`, `setFurEnabled/Shells/Length/Density/Coverage/Gravity/Curl` :141-150 · `setPartFactors(metal,rough)` :151 · `setPartDye` :166 · `setDyeColor` :162 · VRAM pool `setVramPoolEnabled/setPartMatKeys` :84-85.

**Shading features / post**
`setPbr` :152 · `setFeatureDetail/SpecAA/Subsurface/Hair/Ibl/Mask/Tonemap/Dye` :154-161 · `setEnvironment(int)` :167 · `setExposure` :168 · `setColorGrade/setColorGradeLut` :169-170 · `setFov` :171 · `LightRig`/`setLightRig/lightRig` :180-190 · `setReflectionCubemap/Enabled/Strength` :196-198 · `setSkinWarmth/SssStrength/Wetness/Snow` :199-202 · `setShadowEnabled/Params/Extra`, `setLightLock` :204-207 · `setSsaoEnabled/Params` :209-210 · `setBackfaceCull` :323 · `setShowTextures` :387 · `setViewChannel(int)` :392 · `setWireframe` :393 · `setBackgroundColor/backgroundColor/setBackgroundGradient` :406-408.

**Animation / cloth**
`setAnimation(const AnimParser::DecodedAnim&)`/`clearAnimation`/`setFrame` :213-215 · `AttachRange`/`setAttachAnimRanges`/`animFrameFor` :221-236 · `animFrameCount/animFrameRate/animFrame` :237,242-243 · `setPlaybackTimer/animPlaying` :248-249 · `ClothParams`/`setClothParams/clothParams` :252-305 · `setShowColliders` :306 · `setClothEnabled/clothEnabled` :309-310 · `setCapsuleAxis` :313.

**Camera**
`setAutoSpin/setSpinSpeed` :326-327 · `resetView` :328 · `frameAll` :331 · `frameThreeQuarter` :334 · `frameRegion/frameRegionKeepRotation` :337-339 · `setOrthographic/orthographic` :348-349 · `CamState`/`cameraState/setCameraState` :351-354 · `setOrbitYaw/orbitYaw` :355,383 · `setOrbitAngles` :360 · `camYaw/camPitch` :420-421 · `orbitToAxis` :422.

**Capture**
`grabThumbnail` :47 · `grabSupersampled` :53 · `grabEnsembleThumb` :57 · `setCaptureMode/capturing` :365-366 · `settleCloth` :371 · `setCaptureTime` :374 · `CaptureScope` :376-382 · `setTransparentClear` :411 · `setCoverageAlpha` :414 · `static glInfo()` :44.

**Overlays**
`setShowGrid/setShowSkeleton` :394-395 · `setHardpoints/setShowHardpoints` :396-397 · `setShowPhysBones/PhysAxes` :398-399 · `setShowBoneNames/BoneNamesTranslated/BoneNamesHideUnknown` :400-402 · `static translateBoneName/translateSkeletonNames/blenderizeSkeletonNames` :403-405 · `setShowAxisGizmo/setGridAxisColors` :423-424.

**Signals** :427-432 — `partFocused(int)`, `partRightClicked(int, QPoint)`, `partClicked(int, Qt::KeyboardModifiers)`.
**No slots; no shading-mode enum; no fullscreen; no hide/isolate helpers (tab-side).**

### 2.2 Fox (`fox/src/gl/GLModelWidget.h`)

**Scene**
`setModel(QVector<GLMeshUpload>, QVector<QImage> textures, GLSkeletonUpload, QVector<QImage> normalMaps={}, QVector<GLPbrMaterial> pbr={})` :375-381 · `clearModel` :382 · `hasGeometry` :694 · `setGroupVisible` :427 · `setMeshVisible/meshVisible/meshIds/hiddenMeshes/clearMeshVisibility` :432-443 · `setGroupTransform/clearGroupTransforms` :446-447 · `materialSlotOf` :659 · `setConnectPoints/hasConnectPoints` :413-414.

**Shading / material**
`setShadingMode/shadingMode` :390-391 · `setWireframe/wireframe` :448-449 · `setNormalMapping/normalMapping/hasNormalMaps` :454-457 · `setPbrShading/pbrShading/hasPbrMaps` :471-477 · `setDebugView/debugView` :533-534 · `setDirtLook/dirtAmount/dirtMaskChannel` :548-550 · `MaterialOverrides`/`setMaterialOverrides/materialOverrides` :557-568 · `setLinearColor/linearColor` :688-689.

**Lighting / environment**
`setEnvironment/environment` :492-493 · `setEnvironmentAuto/environmentAuto` :500-501 · `setSceneGame/sceneGame` :505-506 · `setKeyAngles/keyAzimuth/keyElevation` :511-513 · `setKeyFollowsCamera/keyFollowsCamera` :518-519 · `setKeyIntensity/keyIntensity/setAmbientIntensity/ambientIntensity/setExposure/exposure` :522-527 · `setBackgroundColor/backgroundColor` :530-531.

**Overlays**
`setShowGrid/showGrid` :397-398 · `setShowRootPath/showRootPath` :401-402 · `setShowAxes/showAxes` :403-404 · `setShowStats/showStats` :405-406 · `setShowBoneNames/showBoneNames` :407-408 · `setShowConnectPoints/showConnectPoints` :409-410 · `setShowSkeleton/showSkeleton` :450-451 · `setShowSelection/showSelection` :784-785 · HUD feeds `statsText/boneLabelsOnScreen/connectLabelsOnScreen` :421-425 · overlay pick `OverlayHit`/`pickOverlayAt/hoveredOverlay/selectedOverlay/overlayScreenPos` :575-588 · `setRootPath/rootPath/rootPathCurrent` :601-603 · `GhostPose`/`setGhostSkeletons/ghostSkeletons` :606-608 · `boneParents/jointsUnder/projectWorld` :609-614 · `setShowHelp/showHelp/toggleHelp` :856-857,866.

**Camera**
`resetCamera` :478 · `centerOn` :750 · `CameraPose`/`cameraPose/setCameraPose` :623-632 · `applyCameraPreset/builtinCameraPresets/savedCameraPresets/saveCameraPreset/deleteCameraPreset` :636-640 · `setOrthographic/orthographic/toggleOrthographic` :645-647 · `viewAlongAxis` :650 · `cameraYaw/Pitch/Distance` :652-654 · `setTurntable/turntable/turntableSpeed` :665,711-712 · `setFieldOfView/fieldOfView` :839,847 · `setAutoFit/autoFit` :845-846 · `frameMesh/framePicked` :850,864.

**Animation**
`applyPose(QVector<animmath::Mat4> skin, QVector<float> skeletonLines)` :745-746 · `clearPose` :747 · `AnimFrameProvider`/`setAnimationFrameProvider/animationFrameProvider` :824-833.

**Selection**
`pickMeshAt` :758 · `pickedMesh/setPickedMesh` :765-766 · `selectedMeshes/setSelectedMeshes/addToSelection/removeFromSelection/toggleInSelection/clearSelection` :767-772 · `setContextMeshes/contextMeshes` :778-779 · `hidePicked/unhideAll/isolatePicked` :861-863 · harness `rightDragged/testRightDrag/testPickGesture/selectionForShot` :795-817.

**Capture / fullscreen / keys**
`grabViewport` :671 · `renderAtSize` :677 · `setTransparentBackground` :683 · `renderTurntable` :710 · `setViewportFullscreen/viewportFullscreen/toggleFullscreen/setFullscreenSupported` :853-854,865,870 · `installViewportShortcuts` :873.

**Signals** :717-737, :880-884 — `shadingModeChanged(ShadingMode)`, `sceneChanged()`, `colourPipelineChanged()`, `displayChanged()`, `cameraChanged()`, `overlayHovered(int,QString)`, `overlayPicked(int,QString)`, `meshPicked(int)`, `meshVisibilityChanged()`, `fullscreenChanged(bool)`.
Also overrides `keyPressEvent` :895 and `event` :896 (context-menu swallow).

### 2.3 POE2 (`poe2/src/gl/GLModelWidget.h`)

**Scene** `setModel(const ModelGeometry&, const AstSkeleton::Skeleton& = {})` :34 · `MaterialTextures`/`setMaterialTextures` :38-40 · `setPartTextures(QVector<QImage>)` :54 · `clearModel` :55 · `Attachment`/`setAttachments/setAttachmentVisible/clearAttachments/attachmentCount` :48-52 · `partCount/partName` :76-77.
**Shading** `enum Shading{Flat,Shaded,Wireframe}` :29 · `enum Channel{BaseColor,Normal,Roughness,Metallic,AO,Emissive,None}` :30 · `setShading/setChannel` :57-58.
**Overlays** `setOverlaysOn/setShowGrid/setShowSkeleton/setShowStats/overlaysOn` :61-65.
**Selection / visibility** `selectedParts/setSelectedParts` :68-69 · `hiddenParts/setHiddenParts/isolateParts/clearHiddenParts` :78-81.
**Camera** `frameAll/frameSelected` :70-71 · `orbitYaw/setOrbitYaw/modelCenter/orbitTarget/setOrbitTarget` :92-96.
**Capture** `renderToImage(scalePercent, transparentBg, cropToModel)` :88.
**Animation** `skeletonMatchesMesh/isSkinnedMesh/clipNames/clipCount/setClip/currentClip/setPlaying/isPlaying/setAnimTime/animTime/clipDuration/currentClipFps` :104-118.
**Signals** :121-125 — `selectionChanged(QSet<int>)`, `partDoubleClicked(int)`, `statsText(QString)`, `animTimeChanged(float,float)`, `viewportPartMenuRequested(QPoint)`.

### 2.4 Union / intersection

**Intersection (all three, same concept though not same name):** load geometry + clear · per-part textures (base colour at minimum) · wireframe · a channel viewer · grid + skeleton overlays · a selected set of parts (D4 via panel mirror) · per-part visibility · a pick under a point (`pickPart` / `pickMeshAt` / private `pickPart`) · frame-all · offscreen render larger with transparent background · turntable driver (yaw setter or loop) · CPU skinned animation · a "right-click happened" signal · a double-click signal.

**In exactly two:** orthographic + FOV (D4, Fox) · camera state struct (D4, Fox) · axis gizmo (D4, Fox) · bone names (D4, Fox) · hardpoints/sockets (D4, Fox) · stencil outline selection (D4, Fox) · fullscreen (D4 tab / Fox widget) · lighting rig (D4, Fox) · turntable auto-spin (D4, Fox) · stats overlay drawn (D4 tab, Fox HUD) · hide/isolate hotkeys (D4 tab, Fox widget) · shared part-menu builder (D4, Fox) · two-pass alpha with distinct passes (D4 FX, POE2 Blend/Additive; Fox glass only) · attachment placement in widget (Fox group transform, POE2 bake) · swallow flag (D4, POE2) · GPU colour-id pick (Fox, POE2) · thumbnail renderer with its own context (Fox, POE2).

**Unique to D4:** cloth/physics (params, colliders, phys-bone overlay), shadows, SSAO, cubemap IBL, ACES + LUT grade, fur shells, dye, mesh FX, detail maps, hair/eye/skin specifics, VRAM texture pool, `snapshotPose`, camera glide, follow-parts, capture determinism (`CaptureScope/settleCloth/setCaptureTime`), coverage-alpha capture, bone-name translation/Blenderize statics, `setOverlayText`.
**Unique to Fox:** `ShadingMode` enum + `shadingModeChanged`, `sceneChanged/displayChanged/cameraChanged` signals, named camera presets (QSettings), `viewAlongAxis`, root path / ghost skeletons, overlay hover/pick of bones & sockets, `MaterialOverrides`, environment auto-per-game, linear-colour pipeline switch, `AnimFrameProvider`, `setAutoFit` heuristic, F1 help, hotkey registry install, dev-harness methods (`testRightDrag`, `testPickGesture`, `selectionForShot`).
**Unique to POE2:** in-widget clip playback with wall-clock (`setPlaying/advancePlayback`), `skeletonMatchesMesh` fail-closed gate, `cropToModel`, `AlphaMode` additive pass, `Attachment` bake with per-frame rigid follow.

---

## 3. Coupling — what leaks into each widget

### 3.1 D4 — heaviest

**Interface (`d4/src/gl/GLModelWidget.h`)**
- `#include "model/AnimParser.h"` :2 → `AnimParser::DecodedAnim` in `setAnimation` :213 and member :471 (D4 .ani decoder).
- `#include "model/ModelGeometry.h"` :3 → `ModelGeometry` :40,241; `ModelJoint` :404-405,470; `ModelHardpoint` :396,780; `ClothCapsule` :512; `ClothSim` :515.
- `ClothParams` fields are named after `.clt.json`/`dmClothTuningMirror` (`flBoneTrackingFactor`… :282-296).
- `LightRig::preset` "0 D4 Wardrobe (campfire)" :181; `setEnvironment` presets Studio/Outdoor/Dungeon/Night :167.
- `setReflectionCubemap(payload, faceSize, faceOffsets)` :196 takes the raw CASC RGBA16F payload layout.
- `setColorGradeLut` "real D4 grade LUT (256×16)" :170.
- Dye (`setDyeColor/setPartDye/setPartDyeMask/DyeRamp/DyeRegion` :119-166), fur "hero_opaque_fur_dualNoise" :140, FX "vfx_actor_*" :718, `setEyeParams` "EyeColor flIrisRoughness" :129, hair `hero_hair` :693, detail-map zone tables :101-114 — all D4 material-system vocabulary.
- `translateBoneName(quint32 nameHash)` / `translateSkeletonNames` / `blenderizeSkeletonNames` :403-405 — D4 hash→name dictionary lives in the widget (`d4.cpp:6045-6184`).
- `setViewChannel` values 7 (detail select) and 8 (dye zones) :388-391.

**Implementation (`d4/src/gl/GLModelWidget.cpp`)**
- `#include "model/Retarget.h"` :2 → `Retarget::restHeadsD4`, `Retarget::mirrorPairs` :6150-6151.
- `#include "model/ModelParser.h"` :3 → `ModelParser::resolveClothTuning` :1191.
- `#include "app/Config.h"` :4 → `Config::d4dataDir()` :1187 — the **viewport reads the game data directory** to fill cloth tuning on every `setGeometry` (:1237, `fillClothTuningFromD4` :1185-1214, parses JSON keys `flBoneTrackingFactor`, `vGravity`, `vSelfWind`… :1194-1212).
- `QSettings` read in the constructor (`viewer/axisGizmo`, `viewer/gridAxisColors`) :1131-1133 and in `mouseDoubleClickEvent` (`viewer/framePartOnPick`) :5699.
- Env-var diagnostics `D4_DUMP_CLOTH`, `D4_CLOTH_LEGACY_ORPHANS`, `D4_CLOTH_PLANES`, `D4_CAPS_FULL` :1874,2108,2192,2667,3463.
- **Side effect:** every `setGeometry` writes `detail_mask_probe.txt` next to the executable :1324-1351.
- Shader: 120 uniform declarations (`grep -c '^uniform'`), including `uReflCube`, `uShadowMap`, dye/fur/FX/detail/eye/hair/wetness/snow :297-402.

**`GLTextureWidget` (`d4/src/gl/GLTextureWidget.{h,cpp}`)**: `setTexture(bcData, w, h, eTexFormat)` `.h:21` takes the D4 `eTexFormat` enum; `.cpp` includes `tex/BcDecode.h` :2 and `tex/TexFormat.h` :3 (`TexFormat::codec/alignedWidth/mip0Size` :202-209, `BcDecode::decode` :227).

**`util/ViewportPartMenu.h`**: `Info::sno` :157 ("Copy SNO" `MenuText::kCopySno` :47), `isSim/isFx` tags :162-163, `collection` :156 — D4 appearance concepts in the shared menu.
**`util/CameraOrbitRow.h`**: includes `gl/GLModelWidget.h` :17 — neutral otherwise (uses `setOrbitAngles/camYaw/camPitch`).
**`app/ViewportSettings.h`**: pure QSettings key bookkeeping for the three D4 tabs (`models/ wardrobe2/ stable2/` :71-82); no GL.

### 3.2 Fox — moderate, and deliberately adapter-shaped

**Interface (`fox/src/gl/GLModelWidget.h`)**
- `#include "anim/AnimMath.h"` :26 → `animmath::Mat4` in `applyPose` :745, `jointsUnder` :613, `m_pose` :1062 (row-major System.Numerics convention, `fox/src/anim/AnimMath.h:1-9`).
- `#include "gl/ViewEnvironment.h"` :27 → `fox::ViewEnvironment`, `fox::DebugView` :492,533; that header pulls `index/GameId.h` (`fox/src/gl/ViewEnvironment.h:27`) → `fox::GameId` in `setSceneGame` :505 and `ViewEnvironment::game` (`ViewEnvironment.h:42`).
- `GLPbrMaterial` :75-320 is an engine-material description in renderer clothing: SRM/TRM/LAYER/LAYERMASK/MTM/FMTT presets, `fox3DDF_Eye` (:91-104), `fox3DFW_Glass` (:116-126), `Dirty_Tex_LIN` (:127-145), hair shift (:296-311), `materialRole` :160, `presetIndex` :192.
- `DebugView` values name Fox channels (`ReflectionMask`, `Dirty`, `DirtSplats` — `ViewEnvironment.h:88-99`).
- `setDirtLook` :548 (dirt-splat composite, a Fox save-game concept).
- Comment-level references to MGO_FACTS, `.fcnp`, FMDL throughout (:1-3,322-346).
- **Input shape is renderer-neutral**: `GLMeshUpload` :39-55, `GLSkeletonUpload` :322-337, `GLConnectPoint` :342-346 carry no Fox types.

**Implementation (`fox/src/gl/GLModelWidget.cpp`)**
- `#include "app/Hotkeys.h"` :8 → `Hotkeys::seq` :3164 (widget builds its own QActions from the app registry :3134-3184).
- `QSettings` for camera presets `view/presets/*` :1500-1550.
- Env vars `FOXAB_DBG_CAMERA` :1049-1050, `FOXAB_SRM_R_AS_AO` :2467, `FOXAB_HAIR_TANGENT_U` :2668-2669.

**Sidecars**
- `ThumbnailRenderer.cpp` includes `app/AppPaths.h`, `app/SehGuard.h`, `index/ArchiveIndex.h`, `preview/ModelLoader.h` :20-23 and is addressed by **archive file index** (`renderOne(int fileIdx…)` `ThumbnailRenderer.h:69`) — fully engine-bound.
- `view/ViewportPanel.cpp` includes `app/Config.h`, `app/Hotkeys.h`, `export/ExportOptions.h`, `export/ViewCapture.h` :31-36; `view/ViewportBar.cpp` includes `gl/ViewEnvironment.h` :18; `view/RenderPanel.cpp` includes `index/ShaderTable.h` :10; `util/PartMenu.cpp` includes `app/Config.h` :9 and `util/MenuText.h` :10.
- `partmenu::Context::fileHash` "Fox's identity, D4's SNO" `fox/src/util/PartMenu.h:48`.

### 3.3 POE2 — lightest, but not zero

**Interface (`poe2/src/gl/GLModelWidget.h`)**
- `#include "model/ModelGeometry.h"` :2, `"model/AstSkeleton.h"` :3, `"model/RigMath.h"` :4 → `ModelGeometry` :34,48,151; `AstSkeleton::Skeleton` :34,152 (bones **and clips**); `RigMath::Mat4` :143,166-167.
- `MaterialTextures::alphaMode` is `int` :38 (documented as the `AssetText::AlphaMode` values, `poe2.cpp:61`) — value-coupled, not type-coupled. `isFur/furNoise/furMask/furDepth` "FurV2" :39.
- `Attachment{ModelGeometry geo; QString bone…}` :48.

**Implementation (`poe2/src/gl/GLModelWidget.cpp`)**
- `toYUp(x,y,z) = (x,-z,y)` :15 — the **PoE2 native→Y-up rotation is hard-coded in the widget** and applied in `fillVertexBuffer` :400, bounds :212-214, grid :344, bones :433, attachments.
- `AstSkeleton::skinMatrices/clipDuration` :256,297 — pose evaluation for the engine's clip format is invoked from inside the widget.
- Shader packs ORM as R=AO G=rough B=metal :88 and reads spec-colour as F0 :122 — PoE2 §6.1 packing baked into the shader.
- No QSettings, no env vars, no Config (`grep` empty).

`GLTextureWidget` (`poe2/src/gl/GLTextureWidget.h:18`) takes a `QImage` — engine-agnostic. `ModelThumbnailRenderer::render(const ModelGeometry&, baseColors)` (`ModelThumbnailRenderer.h:28`) — coupled only to `ModelGeometry`.

---

## 4. Geometry / material / skeleton input types

| | D4 | Fox | POE2 |
|---|---|---|---|
| Geometry struct | `ModelGeometry` `d4/src/model/ModelGeometry.h:189-217` — `QVector<MeshPrimitive>` (each with its **own** `vertices` + `indices` `:40-48`), `MeshVertex` :22-38 (pos, nrm, uv, COLOR_0/1, uv1, joints u16[4], weights) | `QVector<GLMeshUpload>` `fox/src/gl/GLModelWidget.h:39-55` — pre-**interleaved** `[pos3 nrm3 uv2 tan4]` (`kVertexFloats=12` :34), `indices`, `joints` u16, `weights`, `materialSlot`, `groupId`, `meshId` | `ModelGeometry` `poe2/src/model/ModelGeometry.h:44-70` — **one shared** `vertices`/`indices` buffer, `QVector<MeshPart>` ranges `:28-35`, `MeshVertex` :14-24 (pos, nrm, **tangent xyzw**, uv, uv2, joints **u8**[4], weights, color u8[4]), `bboxMin/Max` |
| Where the struct is filled | `ModelParser` (game payload) — comment `:6-20` | adapter `preview/ModelLoader::buildUploads(const fox::FmdlFile&)` `fox/src/preview/ModelLoader.h:298`, `buildSkeleton` `:357` | `.smd/.fmt` parsers `poe2/src/model/ModelGeometry.h:7-9` |
| Tangents | computed **in widget** (Lengyel) `d4.cpp:1259-1284` | supplied in the upload | authored in `MeshVertex` |
| Frame convention | Y-up already (exporter axis-swap done by parser) | model space (FMDL) | **native Z-down**; widget rotates `toYUp` `poe2.cpp:13-15` |
| Skeleton | `QVector<ModelJoint>` inside `ModelGeometry` `d4:50-71` (name, nameHash, parent, cloth/chain flags, inverseBind col-major, localMatrix, rest TRS) | `GLSkeletonUpload` `fox.h:322-337` (line pairs, names, bind positions, parents) — no bind matrices; pose comes as a palette | `AstSkeleton::Skeleton` `poe2/src/model/AstSkeleton.h:36-43` (bones with **row-major** bind/inverseBind `:14-20`, **plus clips** :29-34) passed separately to `setModel` |
| Animation input | `AnimParser::DecodedAnim` per-frame TRS per bone-hash `d4/src/model/AnimParser.h:16-29` | `QVector<animmath::Mat4>` skin palette (row-vector) `fox.h:745` | clips inside `AstSkeleton::Skeleton`; widget samples time |
| Materials | ~40 per-part `QVector<…>` setters (index = part) `d4.h:79-166` | `QVector<QImage>` base + `QVector<QImage>` normals + `QVector<GLPbrMaterial>` (index = **material slot**) `fox.h:375-381` | `QVector<MaterialTextures>` (index = `materialIndex`) `poe2.h:38-40` |
| Extra rig data | `hardpoints`, `clothCapsules`, `clothSims`, `pinnedBones`, `nBaseBones` in `ModelGeometry` `d4:201-216` | `GLConnectPoint` list `fox.h:342-346`; group transforms | none |

**Compatibility verdict.** The three shapes are *convertible* but not *compatible*:
- D4 and POE2 both call their struct `ModelGeometry`/`MeshVertex`/`ModelJoint` but the layouts differ (per-primitive buffers vs shared buffer; u16 vs u8 joints; column-major vs row-major bind; tangents present only in POE2; D4 carries cloth/hardpoint payloads). A `#include "model/ModelGeometry.h"` from a shared core would resolve to two different structs.
- Fox is the only one whose widget input is already a renderer-shaped intermediate (`GLMeshUpload`), which is exactly what a shared core needs: every engine's parser → adapter → `GLMeshUpload`. D4's per-primitive `MeshPrimitive` and POE2's `MeshPart` ranges both map onto it trivially (offset/count + material slot + part id); D4's tangent generation would move into the adapter (or stay as an optional core helper).
- Skeleton: Fox's `GLSkeletonUpload` is sufficient for overlays but **not** for in-widget skinning without a palette; D4 and POE2 both compute the palette in/near the widget. A core needs the union: bind positions + parents + names (overlay) **and** a palette-in API (`applyPose`) — the pose evaluator stays engine-side (as Fox does).
- Material: `GLPbrMaterial` (Fox), `MaterialTextures` (POE2) and D4's 40 setters are three spellings of "per-material image set + scalar params"; the neutral subset (base, normal, ORM/roughness, emissive, alpha mode, unlit/skin/hair flags) covers Fox-Shaded, POE2-Shaded and D4-Shaded; everything else is engine extension.

---

## 5. Which implementation should be the BASE?

**Recommendation: Fox `GLModelWidget` as the structural base, with POE2's pass structure and D4's outline/capture code ported in.** Reasoning against the three criteria:

**Fewest game couplings**
- POE2 wins on raw count (3 includes, all `model/`; no QSettings, no Config, no env vars) — but it hard-codes the PoE2 axis rotation (`poe2.cpp:15`), invokes the engine pose evaluator from inside the widget (`poe2.cpp:297`), and takes the parser's own struct.
- Fox is second: its *input* types are neutral (`GLMeshUpload/GLSkeletonUpload/GLConnectPoint`), and the engine leaks are confined to `GLPbrMaterial` field vocabulary, `DebugView`, `GameId`, `Hotkeys.h` and QSettings presets — all of which are replaceable by a template parameter / callback without touching the draw loop. The adapter (`ModelLoader::buildUploads`) already exists as the pattern.
- D4 is far behind: `Config::d4dataDir()` + JSON cloth tuning inside `setGeometry` (`d4.cpp:1185-1237`), `Retarget`, `ModelParser`, hash dictionaries, a file write per load (`d4.cpp:1348-1350`), and ~100 D4-material setters.

**Most template features present**
- D4 has the most *rendered* features (§22 Rendered = IBL+shadows+SSAO+tonemap; cloth; fur) but lacks the §10/§11 *structure*: no shading-mode state, no overlay gate, no hide/isolate/fullscreen in the widget — those are triplicated across the three tabs (`ViewportSettings.h:2-27` documents the resulting key drift).
- Fox has the most *template-shaped* features in the widget: `ShadingMode` enum, `ViewportOverlays` gate (in the bar), selection set + context set + gated outline, hide/isolate/frame/fullscreen/help with a hotkey registry, camera presets, turntable, `renderAtSize`, popovers/bar/gizmo/HUD as reusable sidecars (`view/Viewport*`). Missing versus template: single-click select (it is double-click), Ctrl/Shift semantics, right-click scoping, shadows/SSAO/IBL cubemap.
- POE2 has the fewest features (no Rendered, no ortho/FOV, no gizmo, no bone names, no hotkeys, tint instead of outline) but has the cleanest two-pass alpha (`poe2.cpp:765-783`) and an honest `renderToImage`.

**Cleanliness**
- POE2: 1 010 lines, one draw path shared by screen/export/pick, readable — but hard-coded lights, `glGetUniformLocation` by name every draw, flat tint selection.
- Fox: 3 677 lines with long rationale comments; RAII scope guards on every offscreen path; one camera setter; per-draw uniform discipline (`fox.cpp:2645-2690`); harness hooks. Its size is mostly the `GLPbrMaterial` documentation and the Fox shader.
- D4: 6 727 lines, ~1 900 of which are cloth (`d4.cpp:1589-3130, 3273-3636`), ~860 shader, ~890 `paintGL`; game data access and diagnostics interleaved with rendering.

**Honest trade-off.** Choosing Fox means porting *back* three things D4 already does better: (1) D4's click-on-release selection with the swallow flag and Ctrl/Shift on the tab-defined selection model (`d4.cpp:5641-5707`), (2) D4's live-pose framing (`partsBounds`) and camera glide, (3) D4's Rendered pipeline (shadow map, SSAO G-buffer, cubemap IBL, A2C cutout) — none of which Fox has. Choosing D4 instead would mean extracting the cloth solver, dye/fur/FX/detail systems, the JSON tuning and the bone-hash dictionary *out* of a 6.7k-line file whose `paintGL` binds ~120 uniforms — a much larger and riskier surgery. Choosing POE2 would mean re-adding almost every §22 feature and the whole furniture (bar/gizmo/HUD/popovers) that Fox already has as separate classes.

Concrete base plan (HUNCH on effort, not on facts): core = Fox widget minus `GLPbrMaterial`-specific shader paths + POE2's `AlphaMode` two-pass + D4's outline/`grabSupersampled`/`setCoverageAlpha`; material description becomes a neutral struct with an engine-extension pointer; the overlay gate moves *into* the widget (POE2's placement) so tabs cannot bypass it; selection lives in the widget (Fox/POE2) with a mirror signal (D4's `partClicked(part, mods)` shape).

---

## 6. Drift — same feature, different behaviour

| Area | D4 | Fox | POE2 |
|---|---|---|---|
| Mouse buttons | L orbit · **R pan** · M **reset on press** `d4.cpp:5654-5659,5719` | L orbit · **M or R pan** · M click reset `fox.cpp:3228,3274` | L orbit · **M pan / Alt+R pan** · R click menu `poe2.cpp:893,920` |
| Orbit speed / pitch limit | 0.01 rad/px, ±1.553 rad `d4.cpp:5714-5717` | 0.5°/px (=0.0087 rad), ±89° `fox.cpp:3270-3271` | 0.01 rad/px, ±1.55 `poe2.cpp:891` |
| Zoom | 0.9^n, no max `d4.cpp:6722-6725` | 0.88^n, [0.05r,40r] `fox.cpp:3673-3674` | 1.15^n, [0.05r,40r] `poe2.cpp:943` |
| Default framing | dist 2.6r, yaw 0.6 rad, pitch 0.25 `d4.cpp:1361-1363` | dist 2.4r, yaw 45°, pitch 18° `fox.cpp:1535-1536` | dist 2.6r, yaw 0.6, pitch 0.4 `poe2.cpp:215`, `poe2.h:183` |
| Re-frame on load | tab setting `models/autoFrame` + first-load `ModelsTab.cpp:8266-8269` | heuristic (autoFit ∨ first ∨ !placed ∨ ratio>6 ∨ moved>3r) `fox.cpp:1042-1043` | always `poe2.cpp:209-215` |
| Selection gesture | single click (release) | **double-click**, or Ctrl/Shift+press | single click (release) |
| Ctrl vs Shift | both toggle | Ctrl toggle, Shift add | both toggle |
| Double-click | frames (if `viewer/framePartOnPick`), never selects | selects, never frames | signals; tab frames selection; empty → frame all |
| Right-click on unselected part | replaces selection | selection untouched (context only) | replaces selection |
| Right-click on the only selected part / empty | menu on it / menu with model-level actions | **clears selection** `ViewCapture.cpp:882-884` | menu on it / menu, selection untouched |
| Selection colours | red (highlight) + blue (picked) `d4.cpp:4608-4609` | orange (select) + blue (context) `fox.cpp:2912-2913` | blue tint `poe2.cpp:682` |
| Shading-mode names | Wireframe · Flat · Shaded · Rendered (tab strings `ModelsTab.cpp:2071-2074`) | same four (`fox.cpp:1139-1148`) | Flat · Shaded · Wireframe (enum order differs `poe2.h:29`) |
| Meaning of "Flat" | lit Lambert without PBR (`d4.cpp:826`) | unlit albedo (`fox.cpp:2477-2479`) | `diffuse·(0.4+0.6 N·V)` (`poe2.cpp:141`) |
| Default shading | Rendered (Models/Stable default key 3 `ModelsTab.cpp:2076`, `StableTab2.cpp:471`; Wardrobe default −1 `WardrobeTab2.cpp:1884`) | Rendered `fox.h:992` | Shaded `poe2.h:178` |
| Channel numbering | 0 shaded … 6 emissive, 7/8 extra | `DebugView` 0 Off … 11 DirtSplats (different set) | 0 = lit, 6 = flat base (`None`) |
| Channel-viewer UI | balls + ⌄ popover with combo, wheel cycles | balls + caret menu, wheel cycles, non-wrapping | two QComboBoxes `poe2/src/tabs/ModelsTab.cpp:160-161` |
| Grid default | off (`models/view/grid` false `ModelsTab.cpp:2193`) | off `fox.h:998` | **on** `poe2.h:180` |
| Overlay gate | tab | ViewportBar | widget |
| Stats | tab QLabel | HUD text | status-bar signal only |
| Skeleton overlay when animated | reposed (`m_boneGlobalSim` `d4.h:797`) | reposed via `skeletonLines` | **bind pose only** |
| Tonemap | ACES, only in Rendered | PBR Neutral, always (linear pipeline on) | Reinhard, always |
| Backface culling | on (toggle) | off | off |
| MSAA | 4× window | none on window | 4× window + 4× capture FBO |
| Pick implementation | CPU ray vs posed verts | GPU id via `paintGL` | GPU id via pick program (body only) |
| Hotkeys | tab eventFilter: H / Shift+H / Alt+H / F / Esc `ModelsTab.cpp:2346,9994-10019` | widget QActions from registry: H / Shift+H / Alt+H / . / F / F1 / 1,3,5,7,0 (+Ctrl) `fox/src/app/Hotkeys.h:111-153` | none |
| Fullscreen exit | per-tab ✕ button | HUD ✕ button + Esc | n/a |
| Attachments animate | yes (multi-clock tracks) | yes (group transform per frame, tab-driven) | yes (rigid re-bake per frame) |
| Thumbnail thread | GUI (widget FBO) | render thread | GUI |
| SEH guard around GPU submit | at tab call sites | thumbnail worker only | none (dead code) |
| Offscreen capture MSAA | single-sample (A2C off, `d4.cpp:4304-4312`) | single-sample | 4× |
| Hidden parts & pick | unpickable | unpickable | unpickable (agree) |
| Per-load side effects | writes `detail_mask_probe.txt`, reads d4data JSON | none | none |

Additional observations worth carrying into the extraction plan:
- **Selection-of-truth mismatch**: D4's widget cannot answer "what is selected" (it holds two mirrored sets set by the panel); Fox and POE2 can. A shared core must pick one (recommend widget-owned + mirror signal, per §11 "one state seen twice").
- **Uniform lookup cost**: D4 memoises (`uni()` `d4.h:614-615`), Fox caches only the preset arrays (`fox.h:984-985`) and otherwise looks up by name per draw (`fox.cpp:2446-2703`), POE2 looks up by name per frame (`poe2.cpp:680-707`).
- **Context-loss handling**: Fox rebuilds overlay VAOs on repeated `initializeGL` (`fox.h:922-930`); D4 and POE2 do not mention it (HUNCH: a reparent/context loss leaves stale ids in both).
- `fox/src/view/DisplayModeButton.*` is not viewport code and should not be counted toward the viewport core.
