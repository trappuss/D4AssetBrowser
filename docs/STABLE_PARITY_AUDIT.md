# Stable ⇄ Wardrobe parity audit

Full sweep of `StableTab2` against `WardrobeTab2`, done across four dimensions so that a
header-level diff could not hide anything: **QSettings keys**, **panels and chrome**, **menus,
export paths and indexes**, and the **method surface**. Line numbers are as of the audit; treat
them as a starting point, not gospel.

The trigger for going this deep: `stable2/showAnims` was read once and written by nothing — a
setting the user could never change — and a method-surface diff had no way to see it. Three more
dead keys turned up once the sweep looked at both halves of every key.

---

## 1. Fixed this session

| # | Was | Now |
|---|---|---|
| 1 | `animCarrierSno()` assumed `mnt_base00_<species>`; `mnt_base00_chimera` does not exist, so Basilisks listed no clips | Clips bucketed by owning appearance; carrier falls out of the data, with the name convention kept as a verified first stage |
| 2 | `exportMenuSuffix` and `exportAppearanceModel` each re-derived the carrier inline **with no fallback** — Basilisk exports wrote zero clips | One resolver, one row source, all paths |
| 3 | Whole-mount export ignored `export/includeAnim` + scope and always wrote just the playing clip — the menu said "+ 24 animations" and wrote one | Honours the scope; falls back to the live clip |
| 4 | ANIMATIONS list left at Qt's default `SingleSelection` | `ExtendedSelection`, matching Models and Wardrobe |
| 5 | ANIMATIONS panel bolted into the left picker column with **no toggle**; `stable2/showAnims` dead | Fifth sidebar PanelBox section with a strip toggle; transport moved under the viewport |
| 6 | `stable2/rememberCam` written by the checkbox, read by nothing — the box did nothing | Honoured in both `saveCameraState` and `restoreCameraState` |
| 7 | `stable2/ovl/hardpoints` live but **missing from the startup overlay replay** | Replayed with its siblings, inside the same master gate |
| 8 | `reset()` cleared only decode caches: `m_petReady`/`m_loaded` survived, so a game-build change left the **old mount roster on screen** even though MainWindow had deleted the cache | Full reset in the `.cpp`, incl. rosters, themes, atlas, thumbs (disk too), `m_lastGeo`, `m_undo` |
| 9 | No generation counter in `ensurePetIndex` — latent until #8, then a live stale-install bug | `m_petGen`, checked in the queued install |
| 10 | Worker wrote `stable_index_v6.bin` **before** the generation check — a stale scan could re-persist the old roster past MainWindow's deletion, permanently | Written on the GUI thread after the check, via temp + atomic rename |
| 11 | `refresh()` connected two singleton lambdas; #8 made `refresh()` re-runnable, so they accumulated per reload | `m_signalsWired`, once for the tab's life |
| 12 | No `export/hardpointEmpties`, `export/boneNamesTranslated` or `Hardpoints::resolveBoneIndices` in **any** Stable export — mounts exported with no saddle/trophy sockets and raw `bone_<hash>` names | Added to all three paths in the canonical order |
| 13 | `export/includeTex` ignored | Honoured; drops the four images, keeps names/alpha/scalars |
| 14 | `export/exportFxSim` ignored, while the Settings tooltip claimed it covered this tab | Honoured — widens the visibility baseline only |
| 15 | 202-line `#if 0` ensembles block; dead `hasTheme()`; four hand-built `.app.json` path strings | Removed / consolidated into `apprJsonPath()` (JSON-read baseline 7 → 4) |
| 16 | No "why does this part look like this" | `showMaterialReport` on the part menu, same pane as Wardrobe |
| 17 | No rig-only clip export | `exportAnimLibrary` + a count-aware menu label |
| 18 | **Anim scan had no disk cache** — ~45,549 files walked per species token per session | `stable_anims_v1.json`, signed with the build stamp + the Anim dir's mtime + buildVersion.txt's mtime; temp+rename; registered in `CacheVersioning.h` and the prune list |
| 19 | `stable2/ovl/axis` + `ovl/gridcolors` forked the app-wide `viewer/*` keys | `addOverlay` takes a full key; those two are the shared keys, so the toggle finally reaches this tab |
| 20 | `stable2/fov` + `stable2/ortho` were a second key for state `stable2/cam/*` already held, never applied at load | Both deleted; the Camera panel seeds from the live viewport |
| 21 | `rebuildMount()` ran once per card the pointer passed through | `scheduleRebuild()` — 35 ms debounce on the four repeatable interactive paths; `rebuildMount()` cancels any pending one |
| 22 | Neither clip cache was in `main.cpp`'s prune list | Both added |
| 23 | The env / light-preset / channel combos bound their enum by POSITION — inserting one item would have re-pointed every saved value *and* every `setEnvironment`/`setViewChannel` call — and no read was validated, so an out-of-range value reached a shader uniform while the combo showed blank | Each combo carries its enum as `userData`, restored by `findData`; `envOrDefault` / `lightPresetOrDefault` / `channelOrDefault` clamp every read |
| 24 | `setViewChannel`'s documentation stopped at channel 6; the shader has 9 branches | Header and GLSL comments corrected |
| 25 | Panel order and heights not persisted; ▲▼ hidden; column kept full width with every panel down; per-panel bools were a second store for the same state | `stable2/panels/shown` + `/sizes` (ordered, migrated once from the bools), `moveSidePanel`, `saveSidePanelLayout`, `updateSidebarCollapse`, `splitterMoved` wired; `section()` takes a stable id separate from the title |

Also shipped: `D4_DUMP_MNTTROPHYANIM=1`, a one-shot bounded diagnostic answering whether mount
trophies own clips at all — see §5.

---

## 2. Remaining bugs (not just gaps)

| # | Bug | Where | Effect |
|---|---|---|---|
| B4 | No card context menu on the pre-scan fallback grid | `StableTab2.cpp` ~1690 | Right-click does nothing until `ensurePetIndex` lands |
| B5 | `stable2/showFx` defaults **on**; `wardrobe2/showFx` defaults **off** | — | Wardrobe's reasoning ("made an ordinary outfit look wrong before the user touched anything") applies to mounts. Changing it needs a versioned migration |
| B6 | Between `reset()` and the next `refresh()`, old cards stay clickable | `StableTab2.cpp` ~1654 | Not a crash (bounds-checked) but dirties the undo stack and re-runs the pipeline for nothing |

---

## 3. Feature gaps, ranked

| # | Gap | Effort | What the user can't do |
|---|---|---|---|
| G3 | **`rebuildMount()` is still synchronous** — coalescing is done; the async decode split and the "Loading…" overlay are not, and the decode cache is still a plain `QHash` flushed wholesale at >160 entries | LARGE | Avoid the freeze on the card the pointer settles on |
| G4 | **MATERIAL TEXTURES section + tabbed MATERIALS** — Stable has one flat `# · Material · Tris` table | LARGE | Inspect a material's texture bindings, values, shader fields, detail maps, SNO/flags/cloth |
| G5 | **Theme highlighting and pinning** — no `kCardMatchQss`, no gold outline, no pinning of matching pieces | MEDIUM | See which barding/trophy belongs to the equipped mount |
| G6 | **Theme resolution is bundle-only** — no collection fallback, no `storNNN` name-token fallback | MEDIUM | Get a set for anything not sold as `Bundle_*Mount*` |
| G7 | **Physics: "Use game cloth data"**, `refreshGameDrivenSliders`, Unlocked limits, editable value field, Wind | LARGE | Have mount cloth match its authored `.clt`, or even see those numbers |
| G8 | **Camera Snap / Follow / snap margin / hover-snap + `frameSlot`** — `m_partSourceSlot` already exists | MEDIUM | Zoom to the selected slot and track it through an animation |
| G9 | **Clip-list category filter + sort; arrow-key clip navigation** (`currentItemChanged` not wired) | SMALL | Narrow hundreds of clips to "Idle"; walk the list with arrows |
| G10 | **`loadReflectionProbe`** — the "Reflections (game probe)" checkbox gates nothing; no cubemap is ever loaded | SMALL | Real character-screen reflections on metal barding |
| G11 | **Grid keyboard navigation** (`navGrid` / `navLookGrid`) | SMALL | Arrow through cards, Enter to equip |
| G12 | **Transport**: step buttons, frame spin, "/ N", time label, ticks, 1.5× | SMALL | Step frame-by-frame or jump to an exact frame |
| G13 | Texture tiles are 44px and static (Wardrobe: 92px + hover-zoom) | SMALL | Read a PBR channel |
| G14 | Colour-grade **LUT** field | SMALL | Apply the authentic D4 grade LUT |
| G15 | Debug console / Copy debug | SMALL | Read or copy the per-piece assembly log |
| G16 | Theme umbrella label shows a count, not names; no "N pieces not in the archive" report | SMALL | Read what a theme will equip before clicking |
| G17 | `exportAnimLibrary` has no `toLast` twin | SMALL | Re-export without the dialog |
| ~~G18~~ | **Done.** Restore Defaults now resets all three viewports through `ViewportSettings::resetAll()`, and a **Clear Stable memory** button clears the remembered mount. See `HYGIENE_TOOLING.md` | — | — |
| G19 | No crash-recovery breadcrumb (`MainWindow` iterates `{"wardrobe","wardrobe2"}` only) | SMALL | Escape a mount that crashes the loader on restore |
| G20 | `ensurePetIndex`'s cache carries no signature (deliberate — MainWindow deletes it instead) | — | Documented in `CacheVersioning.h`; leave it |

---

## 4. Reverse gaps — bugs found **in Wardrobe**

### Fixed

| # | Was | Now |
|---|---|---|
| R2 | The look-card menu labelled "Export model to last folder (…)" from `wardrobe2/lastExportDir`, but `exportItemModel` reads and writes `wardrobe2/exportDir` — so the menu advertised one destination and the export used another, and with `exportDir` unset it showed a path and then opened a dialog | Labels from `exportDir`. The two remembered folders stay separate on purpose (one item vs a whole outfit); only the label was wrong |
| R3 | `wardrobe2/light/sss`: slider default 15, renderer default 24 — a fresh profile rendered at 24 while the slider read 15, so the first nudge produced a jump no input accounted for | Slider default 24. The renderer's value wins because it is the one that has been shipping as the look |
| R4 | Crash recovery cleared `/trophy`; the tab writes `/backTrophy` — a back trophy that faulted on load survived the sweep untouched and was restored again, i.e. an actual **crash-loop** | `/backTrophy` added, plus `/weaponSheath*`, `/attachClip`, `/skinTone`, `/skinDetail` — everything the rebuild consumes. `/trophy` kept: removing an absent key is a no-op |
| R6 | `skinTone` / `skinDetail` persisted `currentText()` — the display label, not the identity. The same rule the env / light-preset / view-channel combos were already fixed under | Persist `currentData()`; restore by `findData` with a permanent `findText` fallback, because saved **look presets** carry these keys and write labels straight back in. The restore also now lands on an index instead of returning early — an ensemble saved with no skin tone used to leave the previous one selected while the setting said empty, so the widget (what the renderer reads) and the setting (what the next save records) diverged for good |
| R8 | "Collision model" exists in both the Overlays and Physics panels over **one** settings key, unmirrored — ticking either left the other showing the opposite of the truth. The Physics copy also drove the GL directly, ignoring the overlay master gate that the startup replay applies | `linkColliderToggles()` ported from Stable, called from both builders so whichever runs second completes the pair; the Physics handler now ANDs `m_overlaysOn` |

### Open

| # | Bug | Where |
|---|---|---|
| R1 | `withDeps` raw files are **counted in the menu label and never written** — "1 model + 37 raw files" produces zero. Stable's `exportAppearanceModel` has the only working implementation | `WardrobeTab2.cpp` 10770–10780 |
| R5 | Dead keys: `weaponType`/`weaponType2` (written, never read — and via `currentText()`), `dyeSel` (read, never written, and its default can never match) | 2762 / 2772 / 4524 |
| R7 | `wardrobe2/weap/*` are live-persisted but absent from `liveSettingKeys()` — **Cancel does not revert them** | `SettingsDialog.cpp` 1065 vs 2450 |
| R9 | `exportItemModel` ignores `export/bakeDetail` and `export/includeAnim`; Stable's equivalent honours both | 4207 |

Found while fixing the above, not yet acted on:

* `exportAnimLibrary` reads **and writes** `wardrobe2/exportDir` — the single-item key — even though
  it exports a whole-character clip library, so it re-points the folder the item menu advertises.
  It never surfaces today only because `exportAnimLibrary(toLast=true)` has no caller (G17).
* The Physics panel is not `setEnabled(m_overlaysOn)` the way the Overlays panel is, so with the
  master guide toggle off, ticking "Show collision models" writes the setting and does nothing
  visible. Parity with Stable, so not a regression — but R8's mirror makes it easier to notice.
* `wardrobeLookKeys()` snapshots `slot/0..9`; `slot/5..9` are never written by anything, so every
  saved ensemble carries five dead entries.
* Stable renders skin SSS at 0.15 while Wardrobe and Models use 0.24, and only Stable overrides
  `GLModelWidget`'s own documented default. Internally consistent, so not the R3 bug — but the
  three tabs do not agree on what skin looks like.

Worth porting **from** Stable: the INFO sidebar section, `makePopupFrame()`, the `m_gridReflow`
debounce, "Copy all" on the clip menu, fullscreen honouring an existing collapse.
(`linkColliderToggles()` is done — R8.)

---

## 5. Deliberate differences — do not re-audit these

- **No dye/pigment** anywhere: mounts are not dyeable in D4.
- **No character creator, weapons, class/gender, markings/hair/face.**
- **Saved "Stables" loadouts removed** — a browser does not need saved loadouts. The `#if 0`
  implementation was deleted; git history has it.
- **Trophies are seated static.** `seatTrophyOnMount` records that the rig-preserving
  `attachSubRig` path had positioning issues and was reverted. Attachment clips therefore need
  that placement bug reopened first, and are only worth it if mount trophies own clips —
  run once with `D4_DUMP_MNTTROPHYANIM=1` to find out before spending anything on it.
- **`stable2/env`** (Environment combo) is Stable-only because Wardrobe *retired* its version;
  that is a divergence to decide on, not drift to fix blindly.
- **`ExportLayout.h` is not used by either tab** — its own header says single-model entry points
  pass `applyLayout=false`.

---

## 6. Suggested order

1. **G9, G11, G12** — small, high-visibility interaction wins (clip filter/sort + arrow keys, grid
   keyboard nav, transport step/frame/time controls).
2. **R1–R4** in Wardrobe — R4 is a crash-loop and R1/R2 are silent data-loss-shaped.
3. **G10, G13, G14, G15** — the remaining SMALLs.
4. **G4 / G7** — the two LARGE ones, worth doing only once the cheap list is empty.

### A correction worth recording

The first draft of this audit listed the env / light-preset / channel keys as "combo index
persistence" violations needing the `ExportLayout.h` migration. **That was wrong.** Those ints are
the *renderer's* documented contract values (`setEnvironment`: 0 Studio 1 Outdoor 2 Dungeon 3 Night;
`setViewChannel`: 0 shaded … 8 dye zones), and all three tabs store the same numbering. No migration
was needed. What was genuinely wrong was narrower: the combo↔enum binding was positional and
implicit, and nothing validated a stored value on the way to a shader uniform. Both fixed — the
stored values are untouched.

Models and Wardrobe still bind those same three by position. Not a defect while every list stays in
the same order, but the invariant now holds in one tab of three.

### A note for whoever edits next

**This repo has MIXED line endings and no `.gitattributes` (deliberately).** `main.cpp`,
`verify-src.py` and `WardrobeTab2.cpp` are CRLF; `StableTab2.cpp/.h` and most of `src/util` are LF.
Any tool that reads a file as text and writes it back (Python's `read_text`/`write_text` included)
will silently normalise CRLF to LF and make the whole file show as rewritten in git. Check with
`file` before committing.
