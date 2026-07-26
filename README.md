# Diablo4AssetBrowserNative

A native Diablo IV asset browser:

> **C++17 · Qt 6 Widgets · OpenGL 4.5 · native CASC reader · fastgltf + tinygltf**, built with **CMake + vcpkg (MSVC x64)**.

Five tabs — **Textures · Models · Wardrobe · Stable · Bulk Extract** — over a
self-contained pipeline: CASC storage → SNO index → parsers (.app/.mat/.tex/anim)
→ PBR GL viewport → `.glb`/image export.

> Not affiliated with or endorsed by Blizzard. For personal use with a copy of
> Diablo IV you own. No game assets are included.

---

## What it does today

- **Textures** — browse group 44, GPU BC preview (BC1/3/4/5/7), TexFrames, PNG/JPEG export.
- **Models** — browse all 67k+ appearances with a Blender-style outliner (parts,
  looks, materials, textures, animations hang off the loaded model's row), smart
  search (`c:` collection · `#tag` · digits = SNO), tag/category/class/gender/type
  filters verified against real d4data, icon/3D thumbnails, and a full PBR viewport:
  shading spheres (wire/flat/shaded/rendered), channel viewer, Overlays panel
  (grid/axes/skeleton/bone names), animation playback, cloth (spring-bone solver),
  dye pigments, shell fur, mesh FX. Right side: stacking toggle panels
  (Info/Parts/Looks/Materials/Shading/Cloth/Animations) with drag-resize + reorder,
  and a Blender N-strip of settings popovers on the viewport edge. Exports:
  skinned `.glb` (+ textures/animations), screenshots, turntable GIFs, rig-only
  animation libraries.
- **Wardrobe** — assemble a full character (class/gender, equipment slots,
  character creator, weapons, dyes, markings), same viewport + panel system as
  Models (shared code, not a copy), ensembles, theme equipping, outfit `.glb` export.
- **Stable** — mounts + trophies (W.I.P.).
- **Bulk Extract** — batch model/texture export with a manifest ledger.

Viewport quick keys: **F** fullscreen · **Esc** unselect/exit · **H / Shift+H /
Alt+H** hide / solo / show-all parts · **middle-click** re-frame · double-click
selects a part (camera snap is a Camera-panel option).

---

## Prerequisites (Windows)

- **Visual Studio 2022** with the *Desktop development with C++* workload (MSVC v143).
- **CMake ≥ 3.21** and **Ninja** (both ship with VS 2022, or install separately).
- **vcpkg** — clone and bootstrap once:
  ```bat
  git clone https://github.com/microsoft/vcpkg
  .\vcpkg\bootstrap-vcpkg.bat
  setx VCPKG_ROOT C:\path\to\vcpkg
  ```
  `Qt 6` itself is pulled by vcpkg from `vcpkg.json` (no separate Qt install needed;
  the first configure builds Qt and can take a while).

---

## Run it (one-click)

Once the prerequisites above are installed, just:

```bat
build.bat      ::  finds VS 2022, builds everything (first run compiles Qt6 — slow)
run.bat        ::  launches the app
```

`build.bat` locates Visual Studio automatically (via `vswhere`), pulls every
dependency through vcpkg, builds, and deploys a stand-alone `dist\` folder with the
Qt DLLs. `run.bat` launches `dist\D4AssetBrowser.exe`. After it starts: **File ▸
Settings** → your Diablo IV folder.

> `rebuild.bat` does an incremental build (day-to-day); `build.bat` is the full
> reconfigure that re-resolves vcpkg.

## Build (manual)

From a **x64 Native Tools Command Prompt for VS 2022** (so MSVC is on PATH), in this
folder:

```bat
:: one-time: pin a vcpkg baseline matching your vcpkg checkout (manifest mode needs it)
"%VCPKG_ROOT%\vcpkg" x-update-baseline --add-initial-baseline

cmake --preset windows-msvc-release
cmake --build --preset release
```

The configure step reads `vcpkg.json` (manifest mode) and builds every dependency
into `vcpkg_installed/`. The executable lands in `build/release/`.

For a **stand-alone folder** with the Qt runtime DLLs + plugins deployed next to the
exe (the way d4analyzer ships), run the install step, then launch from there:
```bat
cmake --install build/release --prefix dist
dist\D4AssetBrowser.exe
```
(You can also run `build\release\D4AssetBrowser.exe` directly from the **x64 Native
Tools** prompt if vcpkg's Qt `bin/` is on `PATH`, but the installed `dist/` folder is
the portable, double-clickable result.)

Then **File ▸ Settings** → point *Diablo IV folder* at your install (the folder
containing `.build.info`). The CASC product defaults to **`fenris`** (live D4); set
a different code for a PTR build. The tabs then list the storage.

---

## Build notes / first-build gotchas

These are the spots most likely to need a one-line tweak on your machine; each is
flagged in the source:

- **vcpkg target names.** If a port's config exports a slightly different target
  name than CMakeLists links, CMake will tell you at configure time — adjust the
  `target_link_libraries` line. (CASC needs no library: the reader is native.)
- **tinygltf** is header-only; CMake locates `tiny_gltf.h` via `find_path`. If it
  isn't found, confirm the `tinygltf` port installed and `vcpkg_installed/.../include`
  is on the toolchain's search path.
- **First configure is slow** — vcpkg compiles Qt 6 from source. Subsequent builds
  are fast.

---

## Project layout

```
Diablo4AssetBrowserNative/
├── CMakeLists.txt          ← targets, dependencies, Windows deploy
├── CMakePresets.json       ← MSVC x64 + vcpkg toolchain presets
├── vcpkg.json              ← qtbase, qtsvg, fastgltf, tinygltf, zlib, lz4
├── src/
│   ├── main.cpp
│   ├── app/   MainWindow, SettingsDialog, Config, ExportCapture, hotkeys
│   ├── casc/  CascReader                      ← native CASC (Salsa20/idx/BLTE/TVFS)
│   ├── index/ CoreToc, SnoIndex, AppearanceMeta, IconIndex, ItemDef, …
│   ├── tex/   TexFormat, TexMeta, BcDecode    ← eTexFormat→GL + BC decoding
│   ├── gl/    GLModelWidget, GLTextureWidget, GifEncoder
│   ├── model/ ModelParser, ModelExporter, MaterialDecode, AnimParser, Retarget
│   ├── deps/  first-run d4data download + update check
│   └── tabs/  BrowserTab.h (shared skin) · PanelBox.h (stacking panels)
│              ViewGlyphs.h (toolbar glyphs) · ModelOutliner
│              TexturesTab · ModelsTab(+Panels/Export) · WardrobeTab2(+Panels)
│              StableTab/2 · BulkExtractorTab · MarkingCompose
└── README.md · STATUS.md ← per-session engineering log
```

The three shared headers are deliberate: **BrowserTab.h** carries the button/panel
QSS + bar height, **PanelBox.h** the right-column stacking-panel system, and
**ViewGlyphs.h** the painter-drawn toolbar icons — so the Models and Wardrobe tabs
wear literally the same code and can't drift apart.

---

## Credits

Builds on the Diablo IV community tooling — **fastgltf** (spnda), **tinygltf**
(Syoyo Fujita), **Qt** (The Qt Company) — and the research of the **d4analyzer** /
d4data community. No game assets or third-party sources are bundled; vcpkg fetches
all dependencies.
