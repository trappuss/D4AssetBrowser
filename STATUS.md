# D4AssetBrowser — status & handoff

Native C++/Qt6 Diablo IV asset browser (Qt6 Widgets · OpenGL 4.5 · native CASC ·
fastgltf/tinygltf). This file is the per-session engineering log; newest entries at
the bottom, oldest context at the top.

## Current state (2026-07-18)
Everything below in the session log is LIVE and running against real installs daily:
the native CASC reader, BC decode, model/anim parsing, the full PBR viewport
(IBL/shadows/SSAO/tonemap, cloth spring bones, fur, FX, dyes), and `.glb`/image/GIF
export. Five tabs: **Textures · Models · Wardrobe · Stable (W.I.P) · Bulk Extract**.
(The old Files / String Lists tabs and the "Developer mode" gate are removed.)

The Models and Wardrobe tabs share one UI system on purpose — change it once,
both tabs follow:
- `tabs/BrowserTab.h` — control skin (kToolBtnQss/kIconBtnQss/kArrowBtnQss/kPanelQss,
  kBarH) + panel title styles (kHdrQss/kSubHdrQss).
- `tabs/PanelBox.h` — the right-column STACKING panel system (icon-strip toggles →
  panels stack in a vertical splitter; drag to resize, ▲▼ reorder, ✕ hide; arrival
  sizing via panelBoxArrive; layout persisted per tab). The whole column hides
  Blender-style behind a floating »/« arrow on the viewport's N-strip.
- `tabs/ViewGlyphs.h` — painter-drawn toolbar glyphs (shading spheres, overlay
  circles, the N-strip icons).
- Both viewports: shading spheres + channel ⌄ (wheel-cycles), Overlays sphere+▾
  (grid/axes/skeleton/bone toggles), FX/SIM/(GIB|FORM), Fullscreen = maximize-in-place
  (F/Esc), MMB re-frame, dbl-click part select (camera snap = `viewer/framePartOnPick`),
  H/Shift+H/Alt+H visibility, N-strip popovers opening leftward with red open-state.

## Build
`rebuild.bat` (incremental) or `build.bat` (full reconfigure; runs vcpkg). Deps via
vcpkg manifest: qtbase, qtsvg, fastgltf, tinygltf, zlib, lz4. Output +
runtime log: `build/release/`.

## Session 2026-07-02 — robust icon solver + encrypted-content groundwork

**Icon solver (Task 1)** — the item→appearance→icon resolution is now principled:
- `ItemDef.*`: 8 hero classes (adds Paladin=6/Warlock=7 + `pal`/`war` prefixes),
  tolerant `tInvImages` parse, `actorAppearance` fixed to read @36 (was @16 = self-sno).
- `AppearanceMeta` crawl (`kCacheVersion` → **14**):
  - Class expansion uses the item JSON's `fUsableByClass` 8-mask (eHeroClass order)
    when present; name tokens only as fallback.
  - The cosmetic-name and actor-`SLOT_style` rules are shared public statics
    (`cosmeticAppearanceNames` / `styleAppearanceNames`) used by crawl AND audit.
  - **Fill-if-empty**: no rule can zero an icon that has no other source anymore
    (bespoke items now bind at weakest score instead of being refused outright).
  - **New Phase D (delta recovery)**: items in CoreTOC group 73 but ABSENT from the
    d4data snapshot (the "icons vanish after update" case, e.g. `barF_sets53_HLM`)
    get appearances via cosmetic-name or CASC `ItemDefinition` → actor style rule,
    and handles from the **diablo4.dad DB** (preferred) or the binary `tInvImages`.
  - Cache also invalidates on a d4dad.json change (`dadSig` in the cache header).
- **diablo4.dad integration** (`src/index/DadOverride.*`): `<AppData>/d4dad.json` is
  auto-fetched with conditional curl (MainWindow, before index rebuild on every
  fingerprint change + first run); parsed item sno → per-class handles.
- **Icon audit** (`src/index/IconAudit.*`, File ▸ Icon audit): cross-checks every
  resolved appearance against diablo4.dad's handles → `icon_audit.txt` next to the
  exe (MISSING / DIFF / NOSPRITE + summary). Run it after any game/d4data update.

**Encrypted content (Task 2) — two real bugs fixed, needs on-machine verification:**
1. TACT keys are now applied **BEFORE** `CascReader::open()` (they were applied
   after, so encrypted nested manifests could never decode during TVFS expansion —
   their whole subtrees were invisible no matter what keys were loaded).
2. `expandNestedManifests()` is now **recursive** (4 levels, worklist) and no longer
   skips containers with `_` in the name (only `^[a-z]{4}_(text|speech)$` locale
   packs are skipped). Undecryptable containers are counted and their missing TACT
   key ids logged: `CASC: N container manifest(s) undecryptable — missing key id(s)…`.

**To verify after rebuild** (wait for "Indexing" to finish; close app before reading log):
1. Log: `CASC: total TVFS paths` should JUMP well above ~1.03M if encrypted/underscore
   packs expanded; check for the missing-key warning line and add those key ids.
2. `barF_sets53_HLM`, `barF_gnrc125_HLM`, `barF_pvpa76_HLM`, `barF_base12/13_HLM`
   should show icons (delta phase log line reports counts).
3. File ▸ Icon audit → `build/release/icon_audit.txt` — expect ~0 MISSING.
4. DOOM probe (user): `rustydemon-cli export --archive "G:\G Games\Diablo IV"
   --output <dir> --dry-run --path "**/*Praetor*"` to confirm the assets are
   TACT-gated, then check they appear in Models after the fixes + current keys.

### Post-launch fixes (same session, after first run on real CASC)
- Crash 1: probing giant locale packs OOM'd → 64 MB stored-size cap + `_cutscene/_video` skip.
- Slow start: 16k `base/child/<sno>-<idx>` chunks were probed → container heuristic now
  requires a LETTER in the leaf. Startup expansion should drop from ~15 s to ~2 s.
- First audit on real data: 8653 checked / 8603 ok / 2 missing / 48 diffs / 35 no-sprite.
  - Most DIFFs were audit noise (class-specific uniques share style numbers) → audit now
    respects d4dad `usableByClass`.
  - sets56 spi/pal/war diffs were REAL: local d4data's tInvImages predates the new classes →
    Phase B now prefers diablo4.dad's per-class handle before falling back to another class's
    icon (`kCacheVersion` → **15**).
  - NOSPRITEs (incl. barF_sets53_HLM, which now RESOLVES a handle — original bug fixed) are
    atlas frames missing from stale d4data `.tex.json` → update d4data (File ▸ Settings ▸ Download).
- Encrypted content: 16 containers gated by ONE missing TACT key `b7b9a971473265a3`
  (= `a365324771a9b9b7` byte-reversed, a known EncryptedNameDict id). Its VALUE isn't public:
  extract from your own running game with `.Resources/tact_scan.py <D4 PID>` (from
  HoldMyBeer-gg/rustydemon research/) — output is already in the key-file format; append the
  line to the TACT keys folder file and File ▸ Reload. Expect the DOOM/collab trees to appear.

### TACT scanner v2 (self-calibrating)  [.Resources/tact_scan.py]
The fixed "value = pointer at name+16" rule broke after a game patch (derefed into
strings/heap ptrs -> garbage values; e.g. known key F159F1F70EABAAB1 came back wrong).
v2 locates several KNOWN public keys in memory, tries candidate layouts (inline vs
pointer at +8/+16/+24/+32, normal or byte-reversed), keeps whichever reproduces the
known values, then extracts all keys with that proven layout. Run via
Extract-TACT-Keys.bat unchanged. Prints the detected layout + flags the collab key
B7B9A971473265A3 explicitly.

## Session 2026-07-02 (cont.) — render/material fixes
Issue 3 (dark normal seam at sharp angles) — FIXED: vertices carry a 3-float tangent with
NO handedness sign and the shader used a fixed bitangent cross(N,T); on mirrored UV islands
(universal on symmetric armour) that inverted the bitangent → dark normal-map seam along
symmetry lines. Replaced the vertex-tangent TBN with a per-pixel cotangent frame (Schuler)
derived from screen-space position/UV gradients — handedness always correct, no vertex-layout
change. Applied to both the base normal and the detail normal. (src/gl/GLModelWidget.cpp)
Issue 2 (detail) — safe correctness fixes: MaterialDecode::byRole now falls through to the
next slot of the same role if the first texture fails to decode (Detail Map 1 missing no
longer drops detail); added detail-roughness slot 220 (Detail Map 3) to Material.cpp +
AssetLinks.cpp. Deeper detail behaviour (3-map masked blend vs single global map) pending
user symptom clarification.

Fur-noise regression fix: the cotangent frame speckled on extruded fur shells (their
screen-space derivatives are dominated by the normal extrusion). Fur shells now use the
smooth interpolated vertex tangent; solid surfaces keep the cotangent frame. Detail reuses
the same frame (uniform UV scaling doesn't rotate T/B), avoiding a second derivative sample.
Detail findings: 161/166 have NO detail slots in their d4data materials (so "no detail" is
correct); 127 detail textures decode (BC5/BC4) so its miss is authored-intensity/blend;
54 "wrong pattern" is the tool applying only Detail Map 1 globally vs D4's 3-map masked
blend. Fur "wrong place" (base09/stor151) still to diagnose (likely fur-mask gating).
Markings material looks (Issue 1) not yet started.

## Session 2026-07-02 (cont.) — detail 3-map blend + fur "wrong place": diagnosis
ROOT CAUSE (detail "wrong pattern"/single-map): D4's uber shader blends the THREE tiled
detail maps (slots 212/213/214 normal, 218/219/220 rough — leather/fabric/metal) per-texel
using the per-vertex COLOR_0 RGB channels as blend weights. The parser DROPPED COLOR_0
(MeshVertex had no colour), so the tool could only apply Detail Map 1 globally → wrong/one
pattern. Now COLOR_0 is parsed (ModelGeometry.h + ModelParser.cpp); nothing renders it yet.
Added a zero-risk probe: on model load, writes detail_mask_probe.txt next to the exe with
per-part COLOR_0 meanRGB + zone count (confirms COLOR_0 is the 3-zone selector before the
render change). Also added a fur-mask coverage line to the Wardrobe2 load log (distinguishes
"mask gated everything off" vs "mask not restricting → fur everywhere = wrong place").
NEXT (after probe confirms): plumb COLOR_0 as a static vertex attribute + bind all 3 detail
normals/roughs + blend in the shader by the colour weights. Fur: fix per the coverage finding.

## Session 2026-07-02 (cont.) — 3-map detail blend implemented
Probe result: NO per-vertex detail mask (COLOR_0/COLOR_1 black on armor, alpha flat; second UV
present everywhere). Conclusion: D4 layers the 3 tiled detail maps as a combined micro-surface,
weighted by authored intensities (no per-region selector in the mesh). Implemented CPU-side:
MaterialDecode::detailComposite() blends the up-to-3 detail normals (slots 212/213/214) by
reconstructing each map's Z and accumulating weighted unit normals + renormalising (exact for a
single map, proper blend for several), and the up-to-3 roughness maps (218/219/220) as an
intensity-weighted average written GREYSCALE (also fixes the shader's `.g` read for BC4 rough).
Overall detail strength = dominant map's authored intensity. Kept the shader on ONE detail
sampler (no new GL bindings, no vertex-layout change). Wired in WardrobeTab2 (cached per material).
TODO: mirror the same call in ModelsTab for the Models tab; then fur "wrong place" (needs the
FUR-DIAG coverage lines — close the app fully so the log flushes).

## Session 2026-07-02 (cont.) — normal-frame: cotangent → det-sign vertex tangent
User feedback after cotangent frame: black outlines on alpha/thin two-sided cards (hair,
cutouts) and gnrc127 detail muddy/flat. Both were the per-pixel cotangent frame misbehaving on
thin/degenerate geometry (gnrc127 has NO authored detail intensities, so it's single-map Map1 —
its regression was the frame, not the blend). Replaced the cotangent frame with the SMOOTH
vertex-tangent frame whose bitangent handedness is flipped when the UV Jacobian determinant is
negative (mirrored UV island). This keeps the mirrored-UV seam fix (Issue 3) and the earlier
fur-noise fix, and removes the alpha-edge black outlines + gnrc127 muddiness — only a SIGN is
taken from derivatives, never the frame direction. sets54's 3-map blend is unaffected (compositing
is separate from the frame). Removed the now-unused cotangentFrame() GLSL helper.

## Session 2026-07-02 (cont.) — detail model fix + troubleshooting sliders
User: detail maps ON made armors matte/flat, killed reflectivity (crft26 gold → matte). Root
cause: (a) my detail-roughness default 0.5 forced roughness up on materials that author NO
roughness-detail intensity (crft26); the original only looked OK due to a BC4 `.g`=0 quirk that
zeroed detail rough. (b) mix()-replacing the base normal/rough washed out shape + reflectivity.
Fixes: detail now applies ONLY where the material authors an intensity (unauthored → 0, verified
against MaterialValue defs which carry no default). Shader detail NORMAL is now ADDITIVE (perturbs
base, doesn't replace) and detail ROUGHNESS is an OFFSET around 0.5 (modulates, no-op when
intensity 0). Added global Preview-Settings sliders (temporary): Detail normal ×, Detail rough ×,
Detail tiling — persisted, live. Defaults: normalMul 0.5, roughMul 1.0, tiling 8.
Also: fur coverage now also written to fur_probe.txt next to the exe (the log is mount-stale while
the app runs), so we can finally read the fur "wrong place" coverage numbers.

## Session 2026-07-02 (cont.) — detail RESEARCH + correct automatic model
Researched the real D4 detail system (dumped material params across 6 armors):
  * Materials carry ONLY per-map scalars: "Normal Intensity 1/2/3", "Roughness Intensity 1/2/3",
    "Roughness Offset 1/2/3", "Color Add Intensity 1/2/3" — plus snoShaderMap + the 6 detail
    texture slots (212-214 normal, 218-220 rough). NO tiling param, NO per-region mask param.
  * Conclusion: the blend lives in the compiled shader graph (not in d4data). Models are NOT
    unique — they share the same library detail textures (Leather_*/Fabric_*/Metal_*) and shader;
    the ONLY per-model variation is the authored intensity scalars. So it's fully automatic.
  * The game LAYERS (adds) each tiled detail map's perturbation × its intensity — it does NOT
    select per-region or average. My averaging (renormalised unit-normal blend) was the flattening
    bug; using max() intensity was also wrong.
FIX (automatic, no tweaking): composite now stores the intensity-WEIGHTED-AVERAGE raw tangent XY;
overall dNInt/dRInt = SUM of intensities; shader applies additively → N += Σ(intensityᵢ·detailXYᵢ),
matching the game. Default multipliers = 1.0 (game-accurate). Tiling fixed 8× = baseRes/detailRes.
Still MISSING (minor, secondary): "Color Add Intensity" (detail also tints albedo) and
"Roughness Offset" bias — can add later; normal (main visual) is now the game's model.

## Session 2026-07-02 (cont.) — detail: real defaults + base01 regression fix
Traced the default: MaterialValue defs (.mtv.json) carry NO default and snoMaterialValueSetOverride
is null → the default lives in the compiled base shader (not extractable). But the DATA proves it:
female barF_base01_TRS_mat authors NO detail values yet has detail slots (Leather+Fabric); the male
variant authors 0.6/1.0; and base01-male/sets54 author Roughness Intensity = 0 to DISABLE it — you
only override to 0 if the default is nonzero. => unauthored detail intensity DEFAULT = 1.0 (not 0).
My "unauthored → 0" had killed base01's detail (the regression). Fixed: default intensity 1.0,
offset 0. Only PRESENT maps count — detailComposite now returns the summed strengths (dOvN/dOvR)
and Σ roughness offset, so the sums are correct regardless of how many slots exist.
Added the "Roughness Offset - Detail Map N" bias (real formula term) as a per-part uniform.
Removed the temporary detail sliders from Preview Settings (kept just the Detail-maps toggle);
global muls remain internal hooks at 1.0 (game-accurate additive).
Formula now: N += Σ(intensityᵢ·detailXYᵢ);  rough += (detailRough-0.5)·Σintensity + Σoffset.
Still TODO (real formula, secondary): "Color Add Intensity - Detail Map N" (detail also tints albedo).

## Session 2026-07-02 (cont.) — detail over-sharpening fix (bounded blend)
base01 over-sharpened because the additive model summed two default-1.0 maps → strength 2.0 and
the unbounded `N += Σ(intensity·detailXY)` over-tilts the normal into harsh grain. Fix: the detail
normal is now a BOUNDED blend — reconstruct the unit detail normal Nd from the composited XY and
mix(N, Nd, strength) — and the aggregate is the AVERAGE of present maps' intensities, not the sum.
So base01 = full-but-bounded detail (not a 2× additive blowup). Roughness aggregate also averaged;
roughness offset stays an additive bias. If the default 1.0 reads too strong/subtle we adjust the
one number (the shader default), but it can't over-sharpen now (mix is clamped 0..1).

## Session 2026-07-02 (cont.) — detail inspection UI
1) Added "Detail maps" to the channel viewer (Shaded/Base/Normal/… dropdown) → shader channel 7:
   shows the composited tiled detail normal (flat blue where a material has no detail map).
2) Added a "Detail" tab to the MATERIAL TEXTURES panel (Wardrobe 2 right side, next to
   Textures/Values/Shaders): per selected material it lists Detail Map 1/2/3 → library texture +
   authored N.Int / R.Int / R.Off, plus a bold "→ Applied" row with the effective game-model
   strengths (avg intensity, summed offset, 8× tiling). Tab count shows how many detail maps load.

## Session 2026-07-02 (cont.) — detail masking RESEARCH: dye mask = region selector
In-game vs ours: the game MASKS detail maps per material region (metal smooth, leather→leather
grain, fabric→fabric weave); ours smears a blended detail over everything. base01_TRS has only 2
materials (body skin + barF_base01_TRS_mat), so metal+leather+fabric are ALL in ONE material with
2 detail maps → the game masks WITHIN the material. The dye mask (slot 54) is a single-channel
BC4 1024² value-banded texture = a region/material-ID mask (the tool already bands it into 4 zones
for dyeing). HYPOTHESIS: dye-mask zones select which detail map applies per region (Detail Map
index ↔ dye zone; metal/near-0 = no detail). My compositing-into-one discarded this.
Added a "Dye zones" debug view (channel 8) to confirm the mask segments metal/leather/fabric before
rebuilding the detail path around per-zone selection (which needs the 3 maps bound separately +
dye-zone lookup in the shader, not a CPU composite — tiling prevents pre-compositing).

## Session 2026-07-02 (cont.) — CONFIRMED dye mask = detail region selector + metal mask
User's "Dye zones" debug view confirmed: the dye mask cleanly segments base01 into metal (yellow),
leather (green/red), fabric (blue) zones. So the game selects detail maps per dye-mask region;
compositing destroyed that. Interim safe fix shipped: detail now fades out as metalness rises
(detailMask = 1 - smoothstep(0.35,0.65,metal)) so metal collar/studs/chain no longer show
leather/fabric grain — the most glaring, unambiguous error. NEXT (big): bind the up-to-3 detail
maps SEPARATELY and select per-texel by dye-mask zone (leather zone → Leather map, fabric zone →
Fabric map). Best-guess mapping from base01: mapIndex = dyeZone - 1 (zone0/none, zone1→Map1,
zone2→Map2, zone3=metal→none) — to be verified with a "detail select" debug view.

## Session 2026-07-02 (cont.) — per-region detail selection via dye mask (BIG rewrite)
Implemented the real model: detail maps are NO LONGER composited/blended — the shader now picks
ONE tiled detail map PER TEXEL from the dye-mask region. detailMapsSeparate() decodes the up-to-3
maps separately; GLModelWidget binds them on units 17-22 (3 normal + 3 rough); the shader
classifies the dye-mask value into a zone and maps zone→layer (layer = dyeZone-1: zone1→map0,
zone2→map1, zone3→map2; mv<=0.02/metal → none). Metal also masked by metalness.
The "Detail maps" channel view (7) now shows the SELECTION: red=map0, green=map1, blue=map2,
black=none — so we can verify/correct the zone→map mapping against the real regions.
Mapping (layer=zone-1) is a best guess from base01; to be confirmed/adjusted from the debug view.

## Session 2026-07-02 — DETAIL MAPS RESOLVED ✓
Per-region dye-mask selection VERIFIED on base01: Detail-maps view shows leather chest/belt = red
(map0=Leather), fabric wrap = green (map1=Fabric), metal = black (none) — correct. Shaded render
now matches the in-game reference (leather grain on leather, weave on fabric, clean metal, no
smearing). Mapping layer = dyeZone-1 confirmed. Detail is selected per region from the dye mask
exactly like the game — no compositing, no manual knobs. Roughness intensity/offset + metal mask
also applied. Remaining detail nicety (optional): "Color Add Intensity" (detail tints albedo).
Still open from Issue 2: fur "wrong place" (fur_probe.txt) — deferred per user.

## Session 2026-07-02 (cont.) — normal black-line fix + Color Add
Black lines "at certain angles": the two-sided flip `if(dot(N,V)<0) N=-N` was applied to the
DETAIL-perturbed normal, so fine detail tipped normals past grazing and produced dark bands
(view-dependent). Fixed: flip only genuine back-faces via gl_FrontFacing (detail/view-independent).
Color Add Intensity implemented: detail faintly darkens albedo in its grooves (micro-AO from the
detail normal's z), scaled by the authored "Color Add Intensity - Detail Map" (dominant of the 3),
per part. Subtle by design.

## Session 2026-07-02 — DATA-DRIVEN detail tiling (the real driver) + normal black-line fix
Found the missing data: each detail texture entry has ptTexAnim.flUScale/flVScale = a PER-MAP
tiling scale (sets54: bull 18, fine 12, metal 6; base09: lamb 20, muslin 6, plain 5; base01:
leather 10, fabric 8). The hardcoded 8× made every map the wrong grain size (sets54 bull at 8×
instead of 18× = far too coarse = read as "wrong texture"). Now parsed into MatTexture.uScale
(Material.cpp) and plumbed per-map to the shader (uDetailScales vec3, selected by dye-zone layer).
Zone→map selection (layer = dyeZone-1) was already correct and consistent across armors; this fixes
the grain size. Fully data-driven — no per-armor tuning.
Also this session: normal "black lines at angles" fixed (two-sided flip now uses gl_FrontFacing,
not the detail-perturbed dot(N,V)); Color Add Intensity implemented (detail micro-AO tint).

## Session 2026-07-07 — hair roughness DATA-DRIVEN + 3 Wardrobe-2 UI regressions
HAIR: the hero_hair shader hardcoded `hairRough = 0.40`. Live diagnostic (Copy debug → HAIR line)
showed the material actually authors `Hair Roughness = 0.50` (and roughFactor = 0.60), so 0.40 was
too low → sheen too sharp/shiny. Now fully data-driven: per hair part we read the hero_hair
MaterialValues `Hair Roughness` / `Hair Specular` / `Hair Highlight Shift` and pass them to the
shader as `uHairParams` (vec3). Fallbacks: Hair Roughness→the part's own roughFactor, Specular→0.3,
Shift→0. Roughness sets the sheen width (exP = mix(50,170,1-rough)); Specular scales both Scheuermann
lobes (0.15/0.30×spec reproduces the old 0.045/0.09 at spec=0.3); Highlight Shift sets how far the
two lobes sit apart. No hardcoded look constants. Plumbing: WardrobeOutfitMaps.hairParams (3/part) →
GLModelWidget::setPartHairParams → per-part glUniform3f (with a (0.5,0.3,0) fallback so other
consumers like StableTab2 that don't set it still render sane hair).
UI regressions (from the WardrobeTab2 refactor/truncation):
  1. Animation timeline missing — my earlier truncation-recovery closed playAnimByName one line early,
     losing the tail (show m_timeline, set slider range, m_animFps, applyAnimSpeed, start timer,
     button→"Pause"). Restored verbatim from backup_20260705_175838.
  2. Copy debug copied only the one-line status summary — the full per-piece/MARK/HAIR log lived in
     the label's tooltip. New showDebugConsole(): a scrollable, selectable log window (Copy/Close),
     copies the full log to the clipboard, and mirrors it into the app-wide live console + log file.
  3. Ensembles panel toggle removed by the refactor (hard-coded always-on). Restored the
     "Ensembles panel" checkbox in Preview Settings ▸ Geometry & debug; both visibility sites now
     honour wardrobe2/viewport/ensembles (default on).
Note: the Linux bash mount served stale/partial copies during verification (phantom truncation +
brace/paren imbalances + "binary file" on WardrobeTab2.cpp). All files verified against the
authoritative copies via the Read/Grep tools — endings correct, every new symbol decl/def/call
consistent. Rebuild via rebuild.bat to confirm.

## Session 2026-07-07 (cont.) — hair physics self-oscillation ("ponytail wiggles on its own")
ROOT CAUSE: the cloth sim spring-simulates EVERY bone past the base rig "regardless of name"
(buildSpringBones / buildClothSim), so hair physics bones (ponytails/braids) are swept in. Hair
has no authored NvCloth cage, so its bones get m_sbSim=-1 and fall back to the GLOBAL cloth
params — which loadClothTuning builds by AVERAGING the equipped ARMOUR's Cloth/*.clt.json
(snoCloth) tuning. So a ponytail is driven with cape/skirt-grade low bone-tracking + high swing;
a long low-mass hair chain is under-damped at those settings → self-oscillates at rest.
Research: D4 cloth tuning (dmClothTuningMirror: flBoneTrackingFactor/flActorTrackingFactor/
stiffnesses/damping/wind) is per-snoCloth, referenced from an appearance. The tool only reads
snoCloth off EQUIPPED ARMOUR appearances (m_partSource) — hair's own tuning (if any) is never
applied to hair bones. Per-hair d4data couldn't be fetched here (hair assets omitted locally,
mirror unreachable), so a diagnostic was added to confirm on the user's machine.
FIX (class-based, like the renderer's hair/skin shading): flag every bone a HAIR part is skinned
to (m_sbHair, data-driven from vertex joints — no name guessing), and in springBoneStep give those
bones hair-class physics ONLY where no authored cage tunes them (authored data wins): stiffer
return-to-pose (≥0.55), heavier damping (keep≤0.45), 0.35× gravity, and tight head-tracking
(trk≥0.85 → small max-distance). All overrides only REDUCE motion → cannot add energy, so the
worst case is slightly-stiff hair, never a wiggle. Diagnostic: `SPRINGBONES … hairBones=N
hairLoose(untuned)=M` to the app console. Tunable if hair now reads too stiff.

## Session 2026-07-08 — Camera/Export cleanup, Rig panel, VERIFIED bone-name hash
Camera popup: removed the Screenshot / Turntable GIF buttons. Export: added an "Include current
animation in .glb" checkbox (bakes the playing clip as a glTF anim track) + an "Open folder" button
on success (export dir was already remembered).
New "Rig" popup (between Detail maps and Physics, dev-mode gated): toggles for Skeleton, Physics
bones, Axis gizmos, Bone names, Translated names. All shared flags route through
WardrobeTab2::applyRigToggle(key,on) — one source of truth that writes the setting, applies it to the
view, and mirrors every duplicate control (the centre Skeleton button, the Physics-panel checkboxes,
the Rig checkboxes) with signals blocked, so nothing desyncs. Bone-name labels draw via a transparent
QPainter overlay child (BoneLabelOverlay) projecting bone heads with the last frame's proj*view.
BONE NAMES (Fable research → d4_hash_tables.h / D4_BoneHash_Research_Report.md in the project root):
  - Hash algorithm VERIFIED: DJB2 seed 0 (NOT 5381), h = h*33 + tolower(c), 32-bit, ASCII. Matches
    gbidHash (blizzhackers/d4data) + 54 real (name,hash) pairs from Diablo IV.exe + barM_base00.
    GLModelWidget::d4NameHash implements it.
  - D4 stores NO authored bone-name strings, only hashes. So translateBoneName maps the 26 SHARED
    PLAYER-RIG bone hashes (barM/barF base00) to labels DERIVED from the game's own hardpoint
    attachments + IK chains (head, mouth, hands, feet, pelvis, shoulders, elbows, knees, weapon
    attach, IK roles). Real & sourced — no guesses. Unknown/per-rig bones (cloth/hair chains) fall
    back to bone_<hash>; SKELHASHES logs them for future expansion.
  - Also available for future use (not yet wired to UI): kD4HardpointNames — 121 hardpoint
    hash→name entries (54 VERIFIED / 67 SOURCED) in d4_hash_tables.h.

## Session 2026-07-12 — UI/UX redesign proposal (Blender-style tiled workspaces)
Backup taken first: `_backups/src_20260712_103747_before_uiux_rewrite` (diff-verified).
Design doc: **docs/UI_REDESIGN.md** (+ docs/UI_REDESIGN_wardrobe_mockup.svg). Summary:
replace the QTabWidget + hand-built splitters + 17 `Qt::Popup` panels with workspace pages
(QTabBar + QStackedWidget), each an `ads::CDockManager` (vcpkg `qt-advanced-docking-system`
4.5.0, LGPL; CMake target `ads::qtadvanceddocking-qt6`) with the GL viewport as central
widget, floating disabled (rigid tiling, GL widget never reparented; AA_ShareOpenGLContexts
already set). All popups fold into a right Properties dock (vertical icon tabs); new
Outliner dock + ContextBus (mode/selection → adaptive toolbar + auto-raised properties);
`applyRigToggle` generalized into SettingsBinder. Single Theme QSS replaces inline sheets.
6-phase migration, pilot = Wardrobe; existing settings keys preserved.

**Phase 0 implemented (same session) — foundations, no behavior change intended:**
- Dep: `qt-advanced-docking-system` added to vcpkg.json (baseline has 4.5.0);
  CMake: `find_package(qtadvanceddocking-qt6 CONFIG REQUIRED)` + link
  `ads::qtadvanceddocking-qt6`. ADS config flags (OpaqueSplitterResize,
  FocusHighlighting) set in main.cpp before any manager exists.
- `src/ui/Theme.{h,cpp}` — main.cpp's installCheckmarkStyle moved verbatim +
  design tokens (§8 of the doc) + workspace-tab-bar QSS. main.cpp now calls
  `Theme::apply(app)`.
- `src/ui/WorkspaceBar.{h,cpp}` — QTabBar+QStackedWidget with the exact
  QTabWidget API MainWindow used; `MainWindow::m_tabs` is now a WorkspaceBar
  (same addTab/count/widget/currentIndex/setCurrentWidget/currentChanged usage,
  lazy refresh + Ctrl+1..9 + view/lastTab untouched).
- ⚠ First build after this: run **build.bat** (full reconfigure so vcpkg installs
  the new ADS port), not rebuild.bat. Expect ~identical UI; the top tab strip is
  now flat-dark with a red underline on the active workspace.
- Note: the Linux-mount phantom-truncation trap hit again during verification —
  files re-verified authoritative via Read/Grep (all intact, balanced).
- Phase 0 build + runtime CONFIRMED on-machine (screenshot: workspace strip live,
  all tabs/viewport/index toast working).

**Phase 1 implemented (same session) — Wardrobe pilot on the dock shell:**
- `src/ui/PropertiesPanel.{h,cpp}` (new, reusable): Blender-style properties editor —
  28px vertical strip of exclusive buttons + QStackedWidget of scroll-wrapped pages;
  `addSection/setSectionVisible/raiseSection`, `sectionChanged` re-emits even for the
  current section (openers re-read settings on it). Themed from Theme tokens.
- WardrobeTab2: outer QSplitter → `ads::CDockManager m_dock`. Central "Viewport" dock
  (toolbar row + m_view) registered FIRST (ADS requirement); docks: Equipment (left
  column incl. ensembles/animations), Inspector (m_sidebar, applySidebars now toggles
  the DOCK), Properties (below Inspector). All docks non-floatable + non-closable →
  the GL widget can never be reparented to a top-level.
- The 7 popups (Camera/Graphics/Lighting/Shaders/Detail/Rig/Physics) are now Properties
  pages, bodies reused verbatim; built EAGERLY (applyRigToggle mirrors exist from
  startup). toggleXxxPanel ×7 + clampPopupToWindow + hoverBtn eventFilter plumbing
  deleted from the Wardrobe files (Models/Stable popups untouched — Phase 3).
  Toolbar buttons now `showPropertiesSection(id)`. Dev sections gated in applySidebars.
  Detail panel's "reset rebuilds the popup" trick → stable outer frame + rebuildable
  `m_detailInner`.
- Layout persisted: `ui/ws/wardrobe/dockState` (kDockLayoutVersion=1, saved in
  persistView(), restored transactionally at ctor end). All wardrobe2/* keys unchanged.
- Verified by inspection against installed ADS headers; compile pending — run
  rebuild.bat. Watch for: dock drag/resize with anim playing, popup-parity of every
  control, Inspector visibility toggles from Settings, fullscreen viewport toggle.
- Phase 1 build + runtime CONFIRMED (screenshot: Equipment/Viewport/Properties docks
  live, Camera page at parity). Polish applied after: dock-area close/undock buttons
  hidden app-wide (main.cpp config flags), Properties strip labels 9px bold.

**Phase 3 implemented (same session) — Models tab on the dock shell (Wardrobe recipe):**
NOTE: doc-Phase 2 (extracting shared panel classes / de-dup) was deliberately RESEQUENCED
to after both tabs are docked — in-place conversion is lower-risk with the recipe proven.
- ModelsTab: outer QSplitter → CDockManager. Central "Viewport" dock = whole centre pane
  (pvHead + viewbar + vsplit with GL viewport + timeline/ANIMATIONS — models/centerSplit
  and models/rightSplit persistence unchanged; models/mainSplit key retired with the
  splitter). Docks: Browser (left column), Inspector (right m_rsplit column), Properties
  (below Inspector). Old splitter sizes {640,760,560} carried over.
- All EIGHT popups → Properties pages: camera, graphics, lighting, **pigment (Dye — not
  dev-gated)**, shaders, detail, rig, physics. Built eagerly (m_fovSlider / m_rigSkelChk /
  m_dyeCombo consumers exist from startup; rebuildDyeCombo now scans at construction).
  toggleXxxPanel ×8 + mtb_clampPopupToWindow + hoverBtn eventFilter block deleted.
  Detail panel got the stable-outer-frame + rebuildable m_detailInner fix (same
  reset-rebuilds-popup trick as Wardrobe's). Rig raise resyncs the Skeleton checkbox
  from the toolbar button (Models' original toggle-on-open semantics, narrower than
  Wardrobe's). applyViewportDevGating extended to gate the dev sections.
- New key: `ui/ws/models/dockState` (v1) via new persistView() override (Models had none).
- Verified by inspection (assembly order, all 8 buttons rewired, no live popup code
  remains). Compile pending — rebuild.bat. Remaining for later phases: StableTab2's 2
  popups, Textures/Dev workspaces, panel de-dup (old Phase 2), Outliner/ContextBus.
- Phase 3 (Models) build + runtime CONFIRMED (screenshot: Browser/Viewport/Inspector/
  Properties docks live, all 8 strip sections incl. Dye).

**Phase 3b implemented (same session) — StableTab2 on the dock shell:**
- Root QSplitter → CDockManager: central "Viewport" (toolbar row + m_view, registered
  first), "Stable" dock (whole left column), "Properties" dock (RightDockWidgetArea —
  Stable has no Inspector column). Old sizes {320,760} → setSplitterSizes {320,760,240}.
- The 2 popups (Camera, Lighting) → Properties pages (bodies verbatim, built eagerly —
  safe: every control sets its value BEFORE its connect, so nothing fires at build).
  showPopup() deleted; btnCam/btnLight → showPropertiesSection(). QSplitter include
  dropped.
- persistView() now saves `ui/ws/stable/dockState` (v1) + saveCameraState (was inline
  camera-only); restore transactional at ctor end. All stable2/* keys unchanged.
- With this, NO Qt::Popup panels remain anywhere in the app (Textures' hover-zoom
  tooltips are Qt::ToolTip transients, intentionally kept). Compile pending.

**Phase 4a implemented (same session) — Wardrobe workspace completed to the §4 map:**
(Panel de-dup a.k.a. old Phase 2 deliberately deferred — highest regression risk,
zero visible value; revisit after the workspaces stabilise.)
- Timeline dock: the ANIMATIONS player moved out of the Equipment column to a dock
  UNDER the viewport (relative-add to centralArea). wardrobe2/dbg/anims now toggles
  the dock. Inner m_timeline strip behaviour unchanged.
- Ensembles: left auto-hide fly-out (addAutoHideDockWidget SideBarLeft; auto-hide
  config enabled app-wide in main.cpp). wardrobe2/viewport/ensembles toggles the tab.
- Outliner v1: PARTS section left the Inspector splitter → own dock above Inspector
  (right column = Outliner/Inspector/Properties). Existing select/hover-highlight +
  visibility checkboxes kept; ADDED: double-click row → partsBounds +
  frameRegionKeepRotation (slot-snap margin) + followParts retarget; filter box
  (applyPartTreeFilter, re-applied after rebuildPartList). Inspector's anySection no
  longer counts parts.
- kDockLayoutVersion → 2 (saved v1 Wardrobe layouts discarded once, by design).
  Factory sizes re-keyed off leftArea (centralArea's parent is now the vertical
  central|Timeline splitter — keying off it would size the wrong splitter).
- Verified: ADS + GLModelWidget API signatures checked against headers; m_secParts
  zero references. Compile pending — rebuild.bat (build also picks up Stable 3b).

**Polish pass (same session) — user feedback "complete mess, inconsistent, wasted space":**
Root cause: content was moved into docks without the §8 component spec — default ADS
chrome, duplicated headers, redundant buttons, layouts authored for their old homes.
- Theme::dockStyle() (new): themed ADS chrome — 22px inset title bars, gold active tab,
  dark handles. APPENDED to each manager's own sheet (ADS sets its own stylesheet at
  construction, overriding app QSS — that's why the chrome looked stock).
- Central "Viewport" dock title bars hidden in all 3 tabs (pure chrome noise;
  centralArea->titleBar()->hide(), + DockAreaTitleBar.h include).
- Redundant popup-opener toolbar buttons DELETED in all 3 tabs (Wardrobe ×7, Models ×8,
  Stable ×2) — they only flipped Properties sections, duplicating the strip. Dev gating
  now solely via m_props->setSectionVisible. Detail "Copy config" feedback retargeted
  from the deleted button to the Copy button itself. panelOpen QSS remnants removed.
  Repo-wide grep: zero references to any deleted symbol.
- Wardrobe Timeline re-laid for a wide short dock: transport strip full-width top row
  (slider earns the width), below it clip list (left, stretch) + fixed-240px
  category/sort column; "ANIMATIONS" header label gone (dock title suffices).
- Wardrobe Inspector m_rsplit: stretch 1/1/0 + sizes {280,280,tile+32} + preview pane
  max-height — kills the dead space PARTS' departure left behind.
- PropertiesPanel min width 316 (pages were authored as ~280-320px popups; narrower
  clipped buttons/sliders, e.g. "Frame full bod|").
- Compile pending — rebuild.bat.

**Polish round 2 (post-screenshot, Models):** chrome/toolbar fixes confirmed good on
screen; two Models leftovers fixed:
- Inspector void: the PREVIEW pane spread matValues/tiles across surplus height (no
  trailing stretch) → pv->addStretch(1) + pane maxHeight kTile+250 + m_rsplit stretch
  factors 1/0/1/1/0. Saved sizes perpetuated the void → key renamed
  models/rightSplit → rightSplit2 (old blob orphaned, new defaults land).
- Truncated Browser filter row: the user's saved v1 dock layout kept Browser narrow →
  Models kDockLayoutVersion 1→2 (one-time layout reset; factory {640,760,560} applies).

**⟲ FULLY REVERTED (same day, user decision).** Everything above in this 2026-07-12
section was undone: src/ + CMakeLists.txt restored byte-identical from
`_backups/src_20260712_103747_before_uiux_rewrite` (diff-verified), vcpkg.json's
qt-advanced-docking-system dependency removed, src/ui/ + docs/UI_REDESIGN.* deleted.
The complete redesigned state (incl. design doc + mockup) is preserved in
`_backups/src_20260712_125145_uiux_rework_shelved` if it's ever wanted again.
Harmless leftovers: orphaned `ui/ws/*` + `models/rightSplit2` keys in the settings INI,
and the ADS package in vcpkg's cache. First build after the revert: run **build.bat**
(vcpkg.json + CMakeLists changed → full reconfigure). The app is back to the classic
QTabWidget + splitters + Qt::Popup panels UI.

## Marking model — DEFINITIVE (do not revert to luminance)
This took a lot of iteration to pin down against real game data + raw mask textures. The rule is
fully data-driven; there is NO per-marking tuning. Code: applyMarking / applyMarkingMaterial in
src/tabs/WardrobeTab2.cpp (a startup self-test in the WardrobeTab2 ctor guards it).

Data sources (all real, per marking):
- MarkingShape (.msh.json): snoMaskFace / snoMaskBody (the mask textures), snoDefaultColor
  (→ MarkingColor; may be null → fall back to a neutral charcoal ink so the design still shows),
  flEmissiveStrength (glow amount).
- MarkingColor (.mcl.json): arPaintColorSamples = 3-point ramp [shadow, mid, highlight] (LINEAR
  floats → sRGB-encode ~pow(1/2.2) before compositing), flPaintRoughness, flPaintMetalness,
  fIsTattoo.

THE KEY INSIGHT — the mask is a TWO-CHANNEL encoding, not grayscale coverage:
- RED channel   = COVERAGE / opacity (where the design sits vs bare skin).
- GREEN channel = RAMP POSITION / material (0 = ink → ramp shadow/black, 1 = gold → ramp highlight).
- Grayscale (BC4) masks have R==G, so both collapse to the design value and it still works.
Reading luminance instead of R/G was the long-standing bug: it produced faint mid-tone washes and
never separated ink from gold. Masks are often BC7 (eTexFormat 50) using 3-subset modes 0/2 — the
BC7 decoder now handles those (kP3/kA3a/kA3b tables); before, 3-subset blocks decoded as noise
("rough/pixelly edges" + corrupted G → wrong fill colours).

Application (per texel):
- albedo   = lerp(skin, rampLerp(ramp, G), opacity = R).
- metalness= flPaintMetalness * G, applied at coverage R  (gold foil is metallic, ink stays matte).
- roughness= flPaintRoughness at coverage R.
- emissive = ramp highlight colour, gated by G (only the bright runes glow), strength = flEmissive
  Strength → uEmisMul. Skin SSS is faded by metalness in-shader so gold reads as clean metal.
- NO normal emboss — D4 markings render flat (gold foil + ink), not raised.

Residual/known-minor: (1) FIXED 2026-07-07 — a mask with a lifted/noisy dark background used to
faintly tint skin. Replaced the fixed cov<0.02 floor with a per-mask BLACK-LEVEL subtract
(markingBlackLevel = the dark modal red; coverageOf renormalises (R-bg)/(1-bg), gradient-safe).
Data-driven, no fixed threshold: a clean mask (bg≈0) is unchanged, a lifted one has its pedestal
removed. Applied to all three composite loops (albedo/ORM/emissive); self-test unaffected (bg=0).
(2) CLOSED 2026-07-07 — this was my own observation during the marking work, not a reported bug.
User reviewed metal/gold looks and confirmed it reads fine, so no reflection-environment change is
needed. (If a specific piece ever looks dull, revisit with an in-game vs tool side-by-side.)

## Session 2026-07-17/18 — Models↔Wardrobe UI parity + house-cleaning

**Panel system (both tabs).** The right column is now the shared PanelBox stack
(PanelBox.h): strip toggles bring panels up, splitter handles size them, ▲▼ reorder,
✕ hides, arrival height fits content (kWantH hints; no more fixed-height tables —
the old setFixedHeight/setMaximumHeight combo was exactly what vented dead gaps
between title and table when panels were dragged). Layouts persist (`*/panels/shown`
+ heights by NAME, not QSplitter::saveState — that blob is positional and hidden
panels still hold slots). Whole column hides Blender-style via a »/« arrow on the
viewport N-strip (wardrobe restore order matters: arm the flag only after m_sidebar
exists, or the width clamp squeezes a 26px sliver).

**Toolbar parity (Wardrobe ← Models).** Shading spheres + channel ⌄, Overlays
sphere+▾ (absorbed the Rig popup; applyRigToggle mirrors survive — the overlay
checkboxes ARE m_rigChk*), FX/SIM/FORM, Fullscreen = maximize-in-place, Reset-view →
MMB, N-strip popovers (panelPosLeftOf) with the [panelOpen] red state. Viewport:
dbl-click part select w/ optional camera snap (global `viewer/framePartOnPick`),
Esc unselect, F fullscreen, H-family visibility hotkeys. Axis gizmo: 35% opacity at
rest, full + backdrop on hover, per-ball hover highlight (negative ends label −X/−Y/−Z).

**Removed.** Files + String Lists tabs (sources deleted; in backups), Developer mode
(debug panels always available), Settings gained a proper Models tab.

**Dead-code purge (~1.3k lines, all verified zero-caller before deletion):**
- GLModelWidget: the ENTIRE legacy vertex-Verlet cloth path (clothStep/cageStep/
  buildCageBindings/closestPtTri + 19 m_cage*/grid members, ~580 lines) — superseded
  by springBoneStep; plus never-called API (setDyeGradient/setDyeMode + pending-grad
  upload, setDetail*Mul/Scale hooks, partName, misc getters).
- Models: retired Rig popup, hidden Reset/Reload state-holders, animRowsForModel,
  six dead members. Wardrobe: randomizeOutfit, applySet + set-combo fill,
  exportScreenshot/exportTurntableGif (Export menu owns capture), applyFxSim.
- Modules: CascReader::fileSize/isDecryptable, ItemDef::actorAppearance,
  SnoListModel::setThumbnail, MaterialDecode::detailComposite, ModelParser::
  bakeRestPose, UpdateCheck::tactKeysUrl. Orphaned settings writes
  (models/filtersOpen, models/rememberPreview + its checkbox) removed.

README/STATUS heads rewritten to the current app (they still described the
"four tabs / CascLib / milestone 5" era).
