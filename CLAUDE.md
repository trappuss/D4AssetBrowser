# Diablo4AssetBrowser Native — Session Rules

C++17 / Qt6 / OpenGL Diablo IV asset browser. MSVC 2022, CMake+Ninja preset `release`, vcpkg.
The user builds with `rebuild.bat` (→ `build_log.txt`, errors distilled to `build_errors.txt`) and runs via `run.bat`. You never build; they rebuild and report.

Deeper references — read when relevant, don't guess instead:
- `PHYSICS_AUDIT.md` — cloth physics state: what's fixed, what's refuted, the open shared-cage defect. **Mandatory before any physics change.**
- `PHYSICS_HARNESS_PROMPT.md` — brief for the standalone cloth-diagnostic CLI, if physics work resumes.
- Skills: `d4browser-codebase`, `d4browser-gamedata`, `d4browser-debugging` (detail behind everything below).

## Non-negotiable working rules

1. **Root cause before fix.** State the cause, the evidence for it, and your confidence BEFORE editing. If confidence is low, add an env-gated diagnostic instead of a speculative fix — one discriminating measurement beats three guesses.
2. **"Nothing changed" / "still broken" is decisive evidence**, not a request to try harder on the same theory. It means the code path you edited is not the one executing. Stop, instrument, locate the real path. This has been true every single time it occurred in this project.
3. **Verify before replying.** Every hand-back includes: brace/paren/bracket balance 0/0/0 on touched files (strip comments/strings first); a grep proving each claimed change exists (and removed code is gone); printf-style format specifiers match argument counts. If behavior seems unchanged after a rebuild: exe mtime > source mtime, then `strings -a build/release/D4AssetBrowser.exe | grep <new-literal>`.
4. **Never retry a refuted hypothesis** without new evidence. The refuted list lives in `PHYSICS_AUDIT.md` and the `d4browser-debugging` skill (capsule 1.0, skin-index cage provenance, min-normalized skeleton scoring, "unknown ⇒ unfiltered" guards, velocity fixes inside solver loops…).
5. **Backup before risky refactors:** `cp -r src .Backups/src_<TITLE>_<timestamp>`. Backups live in BOTH `.Backups/` and `_backups/` — check both when researching when something last worked; that archaeology found the physics root cause.
6. **A function "doesn't exist" only after checking the split files:** ModelsTab logic spans `ModelsTab.cpp`, `ModelsTab_Panels.cpp`, `ModelsTab_Export.cpp`; Wardrobe spans `WardrobeTab2.cpp` + `WardrobeTab2_Panels.cpp`. A wrong "it's missing" conclusion here caused a real duplicate-definition regression.
7. **Batch related fixes; don't waste the user's prompts.** Concise replies. State honestly what is verified vs. assumed.

## Hard code conventions (each has a bug behind it)

- **Overlay master gate:** every tab's overlay state flows through its `reapplyOverlays()` (master `m_overlaysOn` AND each box). Never call `m_view->setShow*()` from a settings-replay path (`applyClothParams`, `applyModelRig`) — ungated replays re-enable overlays the user turned off.
- **One setting, one QSettings key.** Two widgets controlling one state get linked to the same key (see `StableTab2::linkColliderToggles`). Namespaces: `models/`, `wardrobe2/`, `stable2/`, `textures/`. Settings persist live; a bad default shipped once needs a versioned migration to undo.
- **Parents precede children in every skeleton array** — `mergeGeometries` guarantees it; skinning, rest pose, overlays, and the exporter all assume it. D4 authors cloth chains out of hierarchy order, so never resolve parents inline during a merge.
- **Filters fail closed.** An unresolvable family/key means "expand nothing", never "allow everything". A guard that disables itself when its input is unknown fails exactly when needed (this produced a 20,000-row animation flood).
- **Skeleton-overlap matching is Jaccard (`inter/union`), capped to ~3 best.** `inter/min` makes every armour piece match every humanoid rig.
- **Shared builders over per-view copies:** Models list+grid menus compose from `addRowImageActions` / `addRowExportCopyActions`; extend those.
- **Diagnostics are env-gated and permanent:** `D4_DUMP_CLOTH`, `D4_DUMP_ANIMS`; throttle time-based; add new ones in the same style. The app writes no log on its own — user exports via Help → Export log.
- **Update-proof by default:** name-hash and data-driven lookups, tolerate missing/renamed d4data fields, treat absent arrays as unknown. The tool must survive game patches and new d4data commits.
- Qt/MSVC traps: `emit` is a macro (never a lambda name); no member definitions in anonymous namespaces (C2888); most TUs lack `<climits>`/`<limits>` — use literals.

## Domain constants (verified, don't re-derive)

- Cloth `attachLen` is **normalized 0..1**, not a world distance; capsule visual scale **0.52** (1.0 is wrong, tested); sim cages (`*_sim`) are driver proxies, never rendered by default.
- `HP_rightWeapon` = 3636304447, `HP_leftWeapon` = 4036545548; per-class weapon orientation from ItemType `tHardpointOffsets`.
- appearance→AnimSets accumulates EVERY referencing actor — always family-filter set expansions.
- d4data snapshot: `C:\Users\notso\AppData\Roaming\Diablo4AssetBrowser\Diablo4AssetBrowserNative\d4data` · game: `G:\G Games\Diablo IV`.
- Repro assets: `barF_base03_TRS` (cape physics) · `barF_base03_HLM`+`barF_stor161_TRS` (shared cage) · `barF_stor151_TRS` (fur, fragile) · `barF_stor189_LEG` (anim flood) · `barF_base00` (canonical rig, ~700 clips).
