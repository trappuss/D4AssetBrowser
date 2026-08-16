# Handoff — build & verify only

**Scope: compile the pending changes and verify them. Nothing else.**
Do not redesign, refactor, rename, or "improve" anything below. If something looks wrong, report it
and stop — the decisions were made deliberately in the previous session and the reasoning is in the
code comments at each site.

---

## Directories

| What | Path |
|---|---|
| Project root | `C:\Users\notso\Downloads\Claude Current\Diablo4AssetBrowser Native` |
| Source | `C:\Users\notso\Downloads\Claude Current\Diablo4AssetBrowser Native\src` |
| Build output | `C:\Users\notso\Downloads\Claude Current\Diablo4AssetBrowser Native\build\release` |
| Audit reports | `C:\Users\notso\Downloads\Claude Current\Diablo4AssetBrowser Native\build\release\data` |
| Backups | `C:\Users\notso\Downloads\Claude Current\Diablo4AssetBrowser Native\.Backups` |
| d4data snapshot | `C:\Users\notso\AppData\Roaming\Diablo4AssetBrowser\Diablo4AssetBrowserNative\d4data` |
| Game install | `G:\G Games\Diablo IV` |

Scripts in the project root: `build.bat` (incremental), `rebuild.bat`, `clean-rebuild.bat`,
`backup-src.bat`, `Audit - Asset Health.bat`.

---

## 0. Back up first — this has NOT been done

The last snapshot is `.Backups\src_20260808_181721`. **Everything after it is unprotected**, which is
all of the work listed below. The previous session could not run the backup (its Linux sandbox
failed to start and computer-use timed out).

Run `backup-src.bat` from the project root **before** building.

---

## 1. Build

Run `build.bat` from the project root. It brace-checks, snapshots `src` to `.Backups`, then builds.

Full output goes to `build_log.txt`; errors to `build_errors.txt`, both in the project root.

### Files changed since the last successful compile

- `src/app/SettingsDialog.cpp`
- `src/app/MainWindow.cpp`
- `src/tabs/BulkExtractorTab.cpp` + `.h`
- `src/tabs/ModelsTab.cpp`
- `src/tabs/ModelsTab_Export.cpp`
- `src/tabs/WardrobeTab2.cpp`, `src/tabs/StableTab2.cpp` (animation-scope call sites)
- `src/util/AnimExportScope.h` (new), `src/util/QueryTerm.h` (new)

### Two compile errors already hit and fixed — do not reintroduce

1. **`qobject_cast` on `LazyTab`** (MainWindow.cpp). `LazyTab` has no `Q_OBJECT`, so `qobject_cast`
   static-asserts. It uses `dynamic_cast`. Leave it.
2. **`animClipsFor` arity.** It now takes `(sno, nameLower, wantOriginal, wantBase)`. The hover-badge
   caller in `ModelsTab.cpp` passes `true, true` on purpose — that badge describes the DATA, not the
   export scope.

### Known cosmetic risk

New `QGroupBox` titles use `&&` to render a literal `&` (e.g. `"Browsing && loading"`). If a title
shows a stray underline or a missing ampersand, that is the escaping — a one-character fix, not a
design problem.

---

## 2. Verify

### 2a. Settings dialog (File ▸ Settings)

Nine tabs, in this order: **General, Interface, Models, Wardrobe, Export, Hotkeys, Maintenance,
Information, Experimental**.

- **General** → 3 boxes: Directories, Game data, Settings profile
- **Interface** (new tab) → 3 boxes: Startup & layout, On-hover previews & info, Icon indicators
- **Maintenance** → 3 boxes, **Caches & reset must be FIRST**, then Diagnostics, then Indexing
  (advanced). Diagnostics is constructed earlier in the function, so `cache` is added with
  `insertWidget(0, …)` to force the order — if Diagnostics is on top, that call was lost.
- **Export** → 4 sub-tabs holding 6 boxes total:
  - Models → Model export (.glb)
  - Images → Textures, Image & GIF capture
  - Wardrobe & Catalogue → Wardrobe, Catalogue
  - File names → Templates

Nothing should be missing. No setting key was changed — only which layout each box is added to, and
group-box display strings.

### 2b. Export ▸ Model export (.glb) — new options

- **Include base body** — appends the class's `test999` suite (TRS/GLV/LEG/BTS) merged into ONE
  `__baseBody` material.
- **Include base head** — appends only the `_HED` submesh of `<class><gender>_P00` as one
  `__baseHead` material.

Test: load an armour piece (e.g. a `barF_stor…_TRS`), tick both, export, open the `.glb` in Blender.
Expect exactly two extra material slots, each deletable in one action. The body/head arrive
untextured **by design**.

Negative test: exporting a weapon or a mount must add neither (rig share < 50 bones). The log says
why — check `D4AssetBrowser.log` for lines starting `export: base body` / `export: base head`.

### 2c. Animation export scopes

Settings ▸ Export ▸ Models now has four independent checkboxes replacing the old two-item combo:
**Original / Previewed / Pulled / Base**.

Key test: with **only "Original"** ticked, export `sorF_stor191_LEG` (or any gear piece) — it must
come out with **zero** animation clips, because a gear piece owns none. Then export `sorF_base00`
— it must come out **with** clips. That contrast is the whole point of the feature.

`collectExportAnims` logs `export anims: scope=… → N clip(s) for …` on every export.

### 2d. Bulk Extract tab

- The options row has **"Export settings…"** (opens Settings on the Export tab) and, after a
  separator, **"This run:"** with Loose textures / Buffers / Write report / Parallel.
  The old Textures / All animations / Pulled anims / Raw sources checkboxes are **intentionally
  gone** — they were duplicate controls for Settings keys.
- Preset dropdown lists **27 built-ins** prefixed `★`, above any saved presets.
- With >5000 matches in manual mode, **"Select all" must queue every match**, not 5000. The list
  caps display at 5000 and shows a greyed, non-selectable
  `…and N more — not listed, but still extracted`.

### 2e. Audits

- **Help ▸ Audit bulk presets…** → writes `build\release\data\preset_audit.txt`.
  Expect **27 presets, 0 EMPTY**. Each line has a count plus up to 3 example names — check the
  examples match the preset (e.g. "All Warlock Armor" must list `warF_`/`warM_` pieces, not another
  class). A count of 0 means a naming family moved in d4data; report which.
- **`Audit - Asset Health.bat`** → `build\release\data\icon_audit.txt`.
  Expect **`0 missing`** and 5 `NOSPRITE` lines (shared placeholder handles — known and benign).
  If missing is non-zero, the appearance-meta cache did not rebuild: run **File ▸ Index All** and
  re-run.
- Startup log should contain no `SELF-TEST FAILED` line (the `QueryTerm` guard).

---

## 3. If the build fails

Report the first error verbatim with its file and line. Fix only that error. Do not batch
speculative fixes — the previous two failures were each a single line, and one-at-a-time is how
they were found quickly.

---

## 4. Out of scope for this session

Do not start these; they are tracked and belong elsewhere:

- Shipping 2.2.7 (changelog / release)
- The ~10% of models that render with partial geometry despite a non-zero vertex buffer
- Counters on the silent name-join drops (Wardrobe slot roster, 4 Stable sites)
- Any git operation — the user releases packaged builds only and will say when
