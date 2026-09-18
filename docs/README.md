# `docs\` — what is in here

Format notes, design references and investigation write-ups. Everything here was verified
against real data; the numbers are measurements, not estimates.

## Design references

| | |
|---|---|
| [`ASSETBROWSER_TEMPLATE.md`](ASSETBROWSER_TEMPLATE.md) | The family design system for every `(Game\|Engine)AssetBrowser` tool, **sorted into four tiers** — Fundamentals · Essentials · Quality of life · Showpiece — each opening with what you have if you stop there. 33 sections: product identity, architecture skeleton, the ten cross-cutting conventions, the asset list and the one search matcher, the viewport and its part-selection model, exporting and the output layout, the build/verify loop, the documentation skeleton and the manual, filters, panels, textures, bulk extraction, settings, context menus, the self-explanation reports, the GIF budget ladder, hotkeys, releasing, and the game-specific showpiece tab. **Every item carries a portability mark** — U copy as-is · A adapt the data · E D4-specific, with the underlying question stated — and there is a **minimum viable build path** for starting a browser on a new engine. Section 3 keeps its number permanently because its ten items are cited individually; everything else may be renumbered only by fixing every `§` reference in this file and in the `assetbrowser-design` skill. Read this before starting a new browser rather than re-deriving D4's UX. |
| [`CONTEXT_MENUS.md`](CONTEXT_MENUS.md) | The complete context-menu specification — label grammar, the selection rule, row/viewport/per-tab menus, the File · Export · Help menu bar, and when to omit versus disable. |
| [`MODEL_EXPORT.md`](MODEL_EXPORT.md) | What every export option actually does: scopes, detail-map baking, unit scale, rig retargeting, and the normal-map green-channel convention with the measurement behind it. |
| [`PUBLISHING.md`](PUBLISHING.md) | Cutting a release — the order the steps must happen in, the five places the version lives, and the two standing rules for git in this folder. |
| [`HYGIENE_TOOLING.md`](HYGIENE_TOOLING.md) | What `verify-src.py` checks and why each check exists, the one inventory of the three tabs' render settings, and the reports built over data the tool already had. |
| [`CORE_EXTRACTION_PLAN.md`](CORE_EXTRACTION_PLAN.md) · [`core-audit/`](core-audit/) | The plan for extracting one shared core + per-game plugins from D4, FOX and POE2, and the five file:line-cited audits it rests on (viewport, geometry/export, app shell, tabs/store, plugin/build). Read this before the extraction; the template says "copy from D4", the audit says which of the three is actually the right base for each module. |

## Investigations

| | |
|---|---|
| [`RENDER_CRASH_INVESTIGATION.md`](RENDER_CRASH_INVESTIGATION.md) | The GPU-fault triage and what the SEH guards are protecting. |
| [`DD2_ADDON_CONTEXT.md`](DD2_ADDON_CONTEXT.md) | Context for the Blender add-on that consumes these exports. |
| [`PALADIN_MATERIAL_SHARING.md`](PALADIN_MATERIAL_SHARING.md) | Head-named ornaments and cross-gender material sharing — measured from CoreTOC, with the SNOs. |
| [`PET_CLIP_NAMING.md`](PET_CLIP_NAMING.md) | The two ways the game names pet animation clips, measured across all 48 companions. |
| [`STABLE_PARITY_AUDIT.md`](STABLE_PARITY_AUDIT.md) | Stable against Wardrobe across four dimensions: what was fixed, what is open, and nine bugs the reverse pass found in Wardrobe. |
| [`NEXT_SESSION_BUILD_VERIFY.md`](NEXT_SESSION_BUILD_VERIFY.md) · [`NEXT_SESSION_DETAIL_BAKE.md`](NEXT_SESSION_DETAIL_BAKE.md) | Handoff notes with the verification each change still needs. |

## `notes\`

| | |
|---|---|
| [`notes/STATUS.md`](notes/STATUS.md) | The running record of what is solved, what is measured and what is still assumed. The definitive statement of the marking model lives here. |
| [`notes/ENCRYPTED-CONTENT-HANDOFF.md`](notes/ENCRYPTED-CONTENT-HANDOFF.md) | How encrypted (TACT-locked) records are recovered — names from cloth data, materials and texture definitions from the game's own binary tables. |
| [`notes/PHYSICS_AUDIT.md`](notes/PHYSICS_AUDIT.md) · [`notes/PHYSICS_HARNESS_PROMPT.md`](notes/PHYSICS_HARNESS_PROMPT.md) | Cloth solver derivation and the harness used to test it. |
| [`notes/BUNDLES-TAB-RESEARCH.md`](notes/BUNDLES-TAB-RESEARCH.md) | StoreProduct / bundle structure behind the Catalogue tab. |

## `parse_probe.py`

Offline probe for picking apart a binary record without a rebuild. The layouts in `STATUS.md`
and in `MarkingCompose.h` were derived with it.

---

**A note on how these are written.** A claim in this folder should carry the number that
established it — "299 of 299 records", "28.1% across 2,635 atlases", "measured, reverted". A
plausible-sounding claim with no measurement behind it has cost this project real debugging time
more than once; if you add to these docs, add the evidence with it.
