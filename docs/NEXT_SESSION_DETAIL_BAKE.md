# Handoff — share `bakeDetail` with the Models and Stable export paths

**Scope: move one function and wire two call sites. Nothing else.**
The design is settled and the tooltip already tells users the current limitation, so there is no
pressure to widen this. If something nearby looks wrong, note it and stop — several things in this
area were deliberately left as they are, and the reasoning is in the code comments at each site.

---

## Why this is its own session

`export/bakeDetail` — *"Bake surface detail into normal / roughness maps"* — is read in exactly
**one** place: `WardrobeTab2.cpp`. Grep across `ModelsTab_Export.cpp`, `ModelsTab.cpp`,
`ModelExporter.cpp` and `StableTab2.cpp` returns **zero** hits, and
`ModelExporter::optionsFromSettings()` does not read it either.

So a Models-tab, Stable or Bulk Extract export silently ignores it: D4's tiled detail maps
(leather / fabric / brushed metal, which the shader picks per DyeMask zone rather than blending)
never reach the file, and the model comes out with a smooth base normal.

The checkbox sits on the **Models** page of Settings, which makes the gap worse than a missing
feature — it reads as a setting that lies. Its tooltip currently says
`APPLIES TO WARDROBE EXPORTS ONLY at present`. That caveat is the last thing to remove.

---

## Current state before you start

| | |
|---|---|
| Version | 2.2.7 |
| Last build | **succeeded** |
| `Release Smoke Test.bat` | **PASSED** — 159,262,911-byte zip, 2026-08-09 09:15 |
| Working tree | **uncommitted** — a large session's worth |
| `organize-folder.ps1` | delivered to the project root, **not yet run** |

**Commit before you touch anything.** The tree already holds the settings reorganisation, the
`File ▸ Index` submenu, the animation-scope rework, panel colours, the exporter time-accessor fix,
clip filters and the material-map export. Landing this refactor on top of all that unsegregated
makes a bisect impossible if it goes wrong.

---

## The four steps

### 1. Move `bakeDetail` into `MaterialDecode` — and change nothing else

`WardrobeTab2.cpp:1111-1172` (62 lines), currently inside the **anonymous namespace opened at
`WardrobeTab2.cpp:131`**, which is the only reason it is not already shareable. Signature:

```cpp
void bakeDetail(QImage& normal, QImage& orm, const QImage& dyeMask,
                const QImage detN[3], const QImage detR[3], const QVector3D& scale,
                const QVector4D& zoneMap, const QVector4D& bands, int metalLayer,
                float nInt, float rInt, float rOff);
```

Move it verbatim to `MaterialDecode.cpp`, declare it in `MaterialDecode.h` beside
`detailMapsSeparate` (`MaterialDecode.h:60-64`) — it belongs there: it is material compositing, and
that header is already the shared home for exactly this.

**Do step 1 on its own and build it.** Wardrobe's call site
(`WardrobeTab2.cpp` ~`9061-9070`) must compile **untouched** — that is the proof the move was clean.
A Wardrobe export before and after must be materially identical.

`MaterialDecode.h` needs `<QVector3D>` / `<QVector4D>`; it currently includes neither.

### 2. Stable — the closer of the two

`StableTab2.cpp` **already calls `detailMapsSeparate`**, so the detail normals, roughness maps,
per-map scales and metal layer are already in hand. What it does not yet have is the DyeMask image
and the `zoneMap` / `bands` vectors. Source them the way Wardrobe does — see how `m_expDyeMask`,
`m_expDZoneMap` and `m_expDBands` are populated during its material assembly, and note Wardrobe's
fallbacks, which are the authored defaults and should be reused verbatim:

```cpp
scale      QVector3D(8, 8, 8)
zoneMap    QVector4D(-1, 0, 1, 2)
bands      QVector4D(0.063f, 0.345f, 0.596f, 0.831f)
metalLayer -1        nInt 1.0f        rInt 1.0f        rOff 0.0f
```

Gate on the same `export/bakeDetail` key and the same guards Wardrobe uses: skip when the normal is
null, and skip when all three detail normals are null.

### 3. Models — needs the decode as well as the bake

`ModelsTab` does **not** call `detailMapsSeparate` anywhere. Add it in `buildExportMats`
(`ModelsTab_Export.cpp`), which is where the Models and Bulk Extract paths assemble
`ModelExporter::ExportMaterial`, then bake exactly as in step 2.

This is the batch path too, so a detail decode now runs per material per model in a bulk run.
Measure it before deciding whether that is acceptable; if it is slow, the fix is to gate the decode
on `export/bakeDetail` being on rather than to cache anything.

### 4. Remove the caveat

`SettingsDialog.cpp` ~`1204` — delete the
`"APPLIES TO WARDROBE EXPORTS ONLY at present …"` paragraph from the `export/bakeDetail` tooltip,
including the sentence pointing at *"…and every other map the materials use"* (that option has since
been renamed **"Also write all material maps (detail, masks, dye, ramp)"**, so the reference is
stale as well as obsolete).

---

## Verification — do not skip

1. `verify-src.py` (it also runs from `rebuild.bat`).
2. `rebuild.bat`. `WardrobeTab2.cpp` and `ModelsTab*.cpp` are the big files — allow a few minutes.
3. **Step 1 in isolation:** export the same Wardrobe outfit before and after. Identical materials.
4. **Steps 2-3:** export an armour piece that genuinely has detail maps (any `_TRS` with a fabric or
   leather material) from **Models**, with `bakeDetail` on and then off, and compare the normal map
   in Blender. Grain present vs smooth. Repeat once from **Stable** on a mount armour.
5. Hand the diff to a subagent for a static audit. This is not optional here: the last four
   unreviewed changes to this export pipeline each had a defect a review caught, including one that
   made every gear-piece export come out with zero animation clips.

---

## Traps this pipeline has already sprung

- **A setting moved is not a setting working.** `export/bakeDetail` was relocated onto the Models
  settings page during the reorganisation on the correct reasoning that it is a model-texture option
  — without checking which code path reads it. Before you move or re-label anything, grep for every
  reader.
- **Every consumer of a shared flag must be updated together.** Adding an animation source and
  forgetting `animAll` in `ModelsTab_Export.cpp` made that scope export nothing at all, silently,
  with no log line, because the "+ N clips" message sat inside the skipped branch.
- **Explicit lambda capture lists.** A `QMetaObject::invokeMethod(this, [this, a, b, …])` in the
  index scan thread cost a full failed build (`C3493`) for one missing capture.
- **`&&` in a QGroupBox title renders a literal `&`; in a rich-text QLabel it does not.**
  `sectionLabel` wraps text in `<b>…</b>`, so a `&&` there renders as `&&`.
- **Measure before theorising.** Three wrong theories about which clips a model owns were settled in
  one step by parsing `data\index_cache\anim_index.bin` directly — `QDataStream` Qt_6_0:
  `QString` magic, `QString` signature, `QSet<int>`, then `QHash<int,QStringList>` rowsBySno.
  Big-endian u32 lengths, UTF-16BE strings. The data was on disk the whole time.

---

## Out of scope — do not start these

- The unexplained export size. `barM_base00` animations-only is 2.15 GB for ~758 clips; the
  arithmetic from bone count × keyframes says ~0.5 GB and the 4x gap is **not** explained. Next
  instrument is decoded key counts per clip vs `nKeyframeCount`. Tracked, not this session.
- The ~10% of models that render with partial geometry despite a non-zero vertex buffer. The log
  already prints `N sub-object(s) … sit on vertex buffer(s) other than 1 and were NOT loaded`.
- Counters on the silent name-join drops (Wardrobe slot roster, 4 Stable sites).
- The Settings tab bar clipping — nine tabs overflow the dialog width, so it reads `eneral` and
  `Experim`.
- Six single-`&` group-box titles in the viewport panels (`Scene & shadows`, `Geometry & debug` in
  `ModelsTab.cpp`, `WardrobeTab2_Panels.cpp`, `StableTab2.cpp`). Cheap, but only worth doing when
  those files are being rebuilt anyway.
- `liveSettingKeys()` omits seven keys, so Cancel does not revert them: `export/looseTextures`,
  `export/gifDither`, both `export/gifPhysics*`, `export/catalogueFrames`, `export/catOnlyNew`,
  `models/baseColorOnly`.
- Shipping 2.2.7. The changelog is drafted and the smoke test passes; the release is a separate act.
- Any git operation beyond the commit named above. The user releases packaged builds and will say
  when.
