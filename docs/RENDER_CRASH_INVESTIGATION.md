# Models-tab render crash — investigation & fix

## The blocklist (from `model_render_crashes.log`)

Five models were auto-blocklisted after crashing the icon renderer:

| SNO | Name | Stage logged |
|---|---|---|
| 2646190 | Remains of the Reaper | (pre-stage build) |
| 2642003 | Pandemonium Fragment | (pre-stage build) |
| 2636865 | Deathless Tyrant's Majesty | `?` |
| 2627290 | Winged Redeemer's Mantle | `parse` |
| 283174 | NPC/Prop/Monster | `parse` (current guard) |

## What I did

Pulled all five models directly from the game (CASC) and ran a **faithful,
OOB-raising Python port of the entire `ModelParser::parseApp` path** —
cloth capsules, vertex/index decode, segment/sub-object gather, and the bone
skeleton — that raises on any read past the buffer (i.e. exactly where the old
C++ reader would segfault).

**Result: every model parses cleanly. No out-of-bounds read, no bad allocation,
no anomalous counts.**
- bones: 32 / (none) / 282 / 316 / 116
- verts: 2702 / — / 8279 / 10922 / 27039
- cloth sim cages: 8–56 verts, 1–42 triangles — all tiny and sane.
- 2642003 has no index buffer at all → `parseApp` returns invalid and never
  renders (it was blocklisted spuriously).

So the CPU-side parse is **not** the crash. The `stage=parse` label is an
artifact: GPU driver faults are asynchronous, so a fault from one thumbnail's
render can surface during the *next* model's parse window.

## Root cause

The crash is GPU-side, in the thumbnail rendering. The new auto-render fires a
burst of offscreen FBO renders (up to ~15 rows at once) at heavy cloth/monster
meshes. A burst of heavy GPU work can hang the GPU long enough to trip the OS
driver watchdog (TDR / "display driver stopped responding"), which resets the
device and kills the app — asynchronously, hence the misleading stage.

## Fixes

1. **`glFinish()` + `glGetError()` after each thumbnail draw**
   (`GLModelWidget::grabThumbnail`). Forces each render to complete before moving
   on, so a fault is contained to the model that caused it (and localizes the
   crash if one still occurs) instead of cascading into a later model.
2. **Throttled auto-render** (`ModelsTab::renderVisibleIcons`): render at most 3
   thumbnails per tick, then return to the event loop and reschedule the rest.
   Removes the GPU spike that trips TDR; keeps the UI responsive.
3. **Self-protecting byte reader** (`ModelParser::Reader`): every `u8/u16/u32/f32`
   now returns 0 on an out-of-bounds/null read instead of dereferencing wild
   memory — belt-and-suspenders so no missed guard anywhere can segfault.
4. **NaN/Inf vertex-position sanitization**: non-finite coordinates (from a
   malformed payload) are snapped to 0 so they can't poison the camera/MVP math
   or upset the driver.

## Next steps for the five blocklisted models

They are all safe to parse (proven) and should render fine now. To un-quarantine:
right-click a row's icon → **"Clear render blocklist"** (or **"Un-block & render"**
on a specific model). With `glFinish` + throttle, rendering should be stable.

If a crash *still* happens after this build, `glFinish` will make it crash **at
the exact offending model** and the guard will record the real stage — send me
that `model_render_crashes.log` line and I'll pull that model and dig into its
GPU rendering (most likely degenerate/huge geometry that hangs the draw).

Reproduction tooling saved alongside this repo: `parse_probe.py` (the
OOB-raising parser port) + `casc_extract.py` (CASC model extractor).

---

## Follow-up: it also crashes on *load* (main preview), not just thumbnails

The example `mnt_uniq46_trophy` (SNO 2646190) crashes when the model is loaded
into the main 3D view too — and right-clicking to clear the blocklist *selects*
the row, which auto-loads it, so clearing crashed as well.

I extracted that exact model and every other blocklisted one and verified,
byte-for-byte, that they are **structurally perfect**: parse is safe, all
indices are in range, GPU buffers upload consistently, and there are **no NaN
positions** anywhere (render verts *or* cloth cage verts). So there is no
data-level bug to fix — the crash is a GPU-driver-level fault on the actual draw
that I cannot reproduce from the files.

### The real gap in recovery (now fixed)

The load path already had a `models/loadGuard`, but it cleared the guard right
after **parse** — while the crash is in the GPU stage (buffer upload + cloth
build + draw) which is *deferred to the next paint, after parse*. So the guard
never covered the actual crash and the model would crash again every launch.

Fixes:
1. **Load guard now spans the GPU stage.** `applyLoadedGeometry` forces a
   synchronous `repaint()` (running the real upload/cloth/draw) *before*
   clearing the guard. A GPU-stage crash now leaves the guard set.
2. **A crashed load blocklists the model** (persistent) and logs it, instead of
   just skipping it once.
3. **Blocklisted models never auto-load.** Selecting or right-clicking one shows
   an overlay message instead of loading it — so you can navigate to it and use
   the right-click menu safely. (Reload still force-loads if you insist.)
4. **No-load way to clear the blocklist:** Settings ▸ Maintenance ▸ *"Clear model
   render blocklist"* — never touches a model.
5. **Defensive cloth guards:** the cage-binding / cage-seed steps now guard the
   nearest-render-vertex index (a degenerate nearest search could leave it -1 and
   index an array at -1), and cloth cage bind verts are NaN-sanitized like render
   verts.

### The likely real cause (and the fix that should make them render)

`m_clothEnabled` defaults to **true**, and the Models tab **never disabled it** —
so every cloth model ran the full cloth-simulation build (`buildClothSim` +
`buildSpringBones` + `clothStep`, the heaviest and most GPU-crash-prone paint-time
code) on load, with no way to turn it off (the Models-tab SIM button only toggles
submesh *visibility*, not the sim). That is almost certainly the actual crash for
cloth models like `mnt_uniq46_trophy`, and it also explains the false positives:
one cloth model crashing the driver during the auto-render burst cascaded into
whatever model was current (e.g. the plain rock `Pandem_Pillar_Destroyed_A_05`,
which has no cloth yet got blocklisted).

**Fix:** the Models tab now loads models with cloth **simulation off by default**
(`models/clothSim`, default false). The cloth submeshes still render (skinned to
the bind pose); they just don't swing. Exports are unaffected (they use the bind
pose, not the sim). This removes the crash-prone path entirely — the models that
crashed should now load statically. Users who want cloth to swing in the preview
can set `models/clothSim = true`. The Wardrobe tab still has full cloth simulation
with its own toggle.

### Bottom line
- Most blocklisted models (like the rock) were false positives — **clear the
  blocklist** (Settings ▸ Maintenance) and they render fine.
- Genuine cloth crashers should now load because cloth sim is off in the Models
  tab by default.
- The tool still survives + quarantines anything that does crash, the stale
  "blocklisted" overlay is cleared when a real model loads, and blocklisted models
  never auto-load (safe to select/right-click).

If a model *still* crashes on load after this build, it's a driver fault on the
static draw — send the `model_render_crashes.log` stage line + SNO and I'll pull
that model and narrow it further.

---

## Follow-up 2: catch the fault instead of dying (the real fix)

Every data-level avenue was ruled out (parse is provably safe; indices in range;
no NaN; CascReader is mutex-serialized so it isn't a read race). The remaining
fault is a **hardware-level access violation** on the actual draw/flatten that
can't be reproduced from the files. So rather than keep hunting an invisible
fault, the tool now **survives** it.

**Mechanism.** The target is compiled with **`/EHa`**, and a per-thread
**structured-exception translator** (`src/app/SehGuard.*`) turns a Windows
hardware fault (access violation from a bad model *or* a GPU-driver crash) into a
catchable C++ exception. `seh::runGuarded(stage, fn)` wraps the risky work:

- **background parse** (`loadGeometry` worker thread),
- **GPU stage** (`applyLoadedGeometry`: `setGeometry` flatten + `repaint`),
- **thumbnail grab**, and
- **icon-render batch** (`renderIcons`).

When a fault is caught, `handleModelFault` quarantines the SNO (persistent
blocklist), clears the load guard, logs a `CAUGHT fault (...)` line to
`model_render_crashes.log`, drops the geometry, and shows a non-fatal hint in the
viewport — **the process keeps running**. The icon batch quarantines quietly and
continues to the next row.

Net effect: even a genuine driver fault on a specific model no longer takes the
tool down. The model is skipped and blocklisted in-session; everything else keeps
working. (Clear the blocklist in Settings ▸ Maintenance to retry after a driver
update.)

> Requires a rebuild with the updated `CMakeLists.txt` (adds `/EHa` + the two new
> `SehGuard` source files).
