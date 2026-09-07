# Menus — the AssetBrowser specification

Covers **both menu surfaces**: the right-click context menus (§1–§7) and the application
**menu bar** — File · Export · Help (§8). They share one label vocabulary and one
omit-vs-disable law, which is why they are documented together.

**Use this as a prompt.** Hand this whole file to a session building or extending any
`(Game|Engine)AssetBrowser` tool. Everything here is a decision already made and shipped in
D4AssetBrowser; reproduce it, don't redesign it. Where a target engine genuinely lacks a
concept (collections, cloth, dyes), omit that action — never invent a placeholder for it.

The reference implementation is `src/util/ViewportPartMenu.h` (which also contains the
`MenuText` vocabulary namespace) plus `src/util/LookIcon.h` and `src/util/CsvCopy.h` for
context menus, and `src/tabs/BrowserTab.h` + `MainWindow::buildMenu/updateExportMenu` for the
menu bar. **Port those files first.** Every rule below falls out of them.

---

## 0. The four laws

1. **One vocabulary.** Every user-visible string in every context menu comes from the
   `MenuText` namespace, never inline at the call site. This exists because the labels were
   written per-tab and drifted: the same clipboard action shipped as "Copy SNO id", "Copy SNO"
   and "Copy source SNO ID" simultaneously.

2. **One builder per menu family.** The list view and the grid view are two views of the *same*
   model and selection, so they compose from the same two builder functions. The viewport and
   the parts panel across three tabs share one builder. Adding an action lights it up
   everywhere at once. Menus built independently *will* drift — in D4 the grid had 4 entries
   against the list's full set, and six part-menu entry points had converged on three
   different subsets.

3. **A menu never shows an action it cannot perform.** Callers supply only what they can
   resolve; empty strings and null callbacks are omitted. Compare with *disabled-not-hidden*
   below — the two rules apply in different cases and §6 says which.

4. **The label states the subject and the destination.** You can read what a menu item will act
   on, how many, and where the output goes, without invoking it.

---

## 1. Label grammar — non-negotiable

| Rule | Right | Wrong |
|---|---|---|
| Sentence case | `Export part` | `Export Part` |
| `…` means **and only means** "this opens a dialog" | `Save image…` (file dialog) vs `Save image to last folder` (silent) | `Save image` / `Save image as…` — one word apart, opposite behaviours |
| An action writing somewhere remembered **names the folder** | `Export model to last folder (…/D4/exports)` | `Export to last dir` |
| Copy actions carry their value in parentheses | `Copy SNO (1234567)` | `Copy SNO` |
| Counted sets say the count and the noun | `Export 3 models…` | `Export model… — 3 textures` |

**Helper functions** (all in `MenuText`, re-exported from `ViewportPartMenu` for legacy callers):

```cpp
condensePath("C:/Users/me/Documents/D4/exports")  →  "…/D4/exports"
    // last two components only; a full path makes the menu unreadable
withCount("Export model", 12345)                  →  "Export model (12,345 tris)"   // QLocale grouped
withValue("Copy SNO", "1234567")                  →  "Copy SNO (1234567)"
prompts("Export model (12,345 tris)")             →  "Export model (12,345 tris)…"
exportSetPrompt("3 models")                       →  "Export 3 models…"
exportSetLast("3 models", "…/D4/exports")         →  "Export 3 models to last folder (…/D4/exports)"
```

**Never bake an ellipsis into a base label.** `kExportModel` is `"Export model"` with no `…`,
because suffixes get appended (`(12,345 tris)`) and an ellipsis stranded mid-label reads as a
typo. `prompts()` is applied **last, to the finished string**, and is the only way an ellipsis
reaches a label.

**Value previews are truncated at 30 chars** with a real ellipsis character:
`s.size() > 30 ? s.left(29) + QChar(0x2026) : s`. An empty value renders as `—` and the action
is **disabled**.

**The canonical vocabulary** (extend this list, never duplicate an entry with new wording):

```cpp
kCopySno          = "Copy SNO"                  // → "Copy id" or the engine's term
kCopyFileName     = "Copy file name"
kCopyName         = "Copy name"
kCopyCollection   = "Copy collection name"
kCopyPartMaterial = "Copy part material name"
kExportModel      = "Export model"              // no ellipsis — see above
kExportModelLast  = "Export model to last folder"
kExportPart       = "Export part"
kExportPartLast   = "Export part to last folder"
kCopyImage        = "Copy image"
kSaveImage        = "Save image…"               // PROMPTS
kSaveImageLast    = "Save image to last folder" // SILENT
kShowDeps         = "Show dependencies…"
```

Note `kCopyPartMaterial` is deliberately *not* "Copy part file name": a submesh has no authored
name of its own, so what the action copies is its **material** name. The old label read as if
it copied a file name for that part. Name the action after what it actually puts on the
clipboard.

---

## 2. Which rows a menu acts on — the selection rule

**This is the single most bug-prone thing in the whole system.** Implement it exactly.

```
hit          = view->indexAt(clickPos)          // the row under the cursor
selection    = view->selectionModel()->selectedRows()
hitInSel     = selection contains hit.row()

if (hit.isValid() && !hitInSel)  →  act on the CLICKED ROW ALONE (do not clear the selection)
else                             →  act on the WHOLE SELECTION
```

Right-clicking outside the selection acts on what you clicked; right-clicking inside it acts on
all of it. Right-clicking a row you had not selected must **not** silently operate on a
selection elsewhere in the list — that reads as the menu simply being wrong.

Three refinements, each of which was a shipped bug:

- **Pass the view in.** `contextSnos(view, pos)` takes the view as a parameter. A hard-coded
  list view hit-tested with grid coordinates returns a *plausible wrong row* — list rows are
  ~18px single-column, grid cells ~122px multi-column, and a hidden view in a `QStackedWidget`
  keeps its geometry. Every copy and export then acted on a model the user had not clicked.
- **Reject subtree nodes.** If `hit.parent().isValid()`, the click landed on an outliner child
  (a part, a look, a clip), whose row number is *not* a browse row. Return empty, and strip
  child rows out of the selection too — same aliasing hazard.
- **Single-subject actions take the clicked row, not `selection.first()`.** The clipboard holds
  ONE image, so `Copy image` inside a 30-row selection must copy the row you right-clicked.
  Keep a separate `clickedSno(view, pos, fallback)` helper for these; list actions (export,
  multi-copy) still take the whole set.

```cpp
const QList<int> snos    = contextSnos(view, p);      // the set
const int        clicked = clickedSno(view, p, snos); // the one subject
```

---

## 3. Browse-tab row menu (list rows, grid tiles, outliner root)

Composed from **two shared builders** so list and grid cannot drift. Full order:

```
Load / preview
─────────────────────────────────
[addRowImageActions(menu, snos, clicked)]
Copy image                                       ← the CLICKED row
Save image to last folder [— N icons]            ← silent, whole selection
Save image… | Save N images…                     ← prompts, whole selection
─────────────────────────────────
Render icon | Render N icons
Un-block && render (may crash)                   ← only if any selected row is blocklisted
Clear render blocklist (N)                       ← only if the blocklist is non-empty
─────────────────────────────────
[addRowExportCopyActions(menu, snos)]
Export N model(s) to last folder (…/D4/exports)  ← OMITTED ENTIRELY if no remembered folder
Export N model(s)…
─────────────────────────────────
Copy SNO  (1234567)                              ← single selection: value shown
Copy file name  (barF_base03_TRS)
Copy name  (Sunfire Regalia)
Copy collection name  (—)                        ← DISABLED when empty
   …or, multi-selection:
Copy SNO  —  12 rows                             ← copies newline-joined list
Copy file name  —  12 rows
Copy name  —  12 rows
Copy collection name  —  12 rows
─────────────────────────────────                ← single selection only
Variants (3) ▸                                   ← submenu, one entry per sibling; omitted if none
Show dependencies…
─────────────────────────────────                ← only on the LOADED model's own root row
Show all
Hide all
Invert
```

Rules this encodes:

- **"To last folder" is omitted entirely when no folder is remembered.** It used to appear
  anyway, reading `Export 3 models to last folder` with no folder named, and then open a
  directory dialog — a label promising the opposite of what it did.
- **`&&` in a Qt action label renders one `&`.** `Un-block && render` displays as
  `Un-block & render`. A bare `&` becomes a mnemonic and eats the next letter.
- **Multi-copy joins with `\n`**, so the clipboard is one item per line, paste-ready.
- **The menu does not branch on which column was clicked.** It used to: the icon column got
  image actions and returned, every other column got export/copy — mutually exclusive halves,
  neither view ever showing the full set. Worse, List mode hides the icon column entirely, so
  the image actions were unreachable by any click at all. The row is the same asset whichever
  cell you hit.
- **`Variants` and `Show dependencies…` are single-selection only** — they have no meaningful
  multi-subject form.

**Grid tiles** run the identical composition. A tile is entirely icon, so both halves apply.

---

## 4. Viewport and parts-panel menu (the shared part menu)

**One builder, six entry points**: the 3D viewport right-click and the parts panel, in each of
the three model-viewing tabs. Structure:

```
Sunfire Regalia  —  chest_armor  [SIM]           ← DISABLED header, only when a part was hit
─────────────────────────────────
Export model to last folder (…/D4/exports)       ← omitted if no remembered folder
Export model (48,120 tris)…
Export part to last folder (…/D4/exports)        ← part-scoped, omitted with no folder
Export part (12,345 tris)…
─────────────────────────────────
Copy part material name (mat_chest_a)            ← part-scoped only
Copy file name (barF_base03_TRS)
Copy SNO (1234567)
Copy name (Sunfire Regalia)
Copy collection name (Season 3)
─────────────────────────────────
Frame part
Select part
Hide part | Show part                            ← label reflects CURRENT state
Isolate part
─────────────────────────────────
Show all
Hide all
Invert
```

Details that matter:

- **Header** is `"<model>  —  <part>"`, disabled, with `  [SIM]` / `  [FX]` appended for cloth
  cage proxies and FX submeshes. Falls back to `part <n>` when the part has no resolvable name.
  Omitted entirely when the click hit empty space.
- **The Copy block is NOT gated on hitting a part.** Right-clicking empty viewport space, or
  the outliner's root row (which *is* the loaded model), must still offer the model's own name,
  file name, SNO and collection — all four values are sitting right there. Only
  `Copy part material name` is genuinely part-scoped. This was a real gap.
- **The "All parts" block works with nothing under the cursor**, so a right-click on empty
  space still gets Show all / Hide all / Invert.
- **`Hide part` / `Show part` is one action whose label reflects current state** — not two
  actions, not a checkbox.
- **`Select part` must not move the camera.** Framing is its own explicit action. Selecting
  expands the parent, sets the current index, scrolls to it — nothing else.
- **A `closed` callback fires however the menu is dismissed** (action chosen, Esc, click-away),
  and fires even for an empty menu. Callers use it to drop the transient right-click highlight;
  without it the outline survived until the next right-click on empty space.
- **`menu.exec()` is blocking** and spins a nested event loop. **Snapshot every value you need
  before calling it** — never capture a pointer into a container that a background index thread
  can move-assign. In D4 this would leave copy actions dereferencing freed memory.
- **Group nodes keep a reduced menu.** An outliner node covering several parts has no single
  subject for `Export part` or `Frame part`, so it gets `Solo` / `Show all` / `Invert` instead.
  A group of exactly one part delegates to the full part menu.

---

## 5. Per-tab specifics

### Textures tab — row menu

Same skeleton, minus what a texture doesn't have:

```
Export N texture(s) to last folder (…/D4/textures)
Export N texture(s)…
Copy image  (tex_name_preview)                   ← decodes the FIRST selected texture
Save image…                                      ← prompts, PNG/JPEG filter
─────────────────────────────────
Copy SNO  (1234567)      | Copy SNO  —  N rows
Copy file name  (…)      | Copy file name  —  N rows
```

**No `Copy name` and no `Copy collection name`.** A texture's entry name *is* its file name —
two labels for one payload means whichever you pick you get the same string, so the duplicate is
dropped rather than faked. A texture has no collection, so that action could never do anything.
**An action that can never apply is not parity, it is clutter.**

Other Textures-tab surfaces, each with the same two-action image pair from `MenuText`:
the GL preview (`Copy image` / `Save image…`), each channel tile, each atlas frame in the
frame gallery, and the ASSOCIATED MODELS list, which adds
`Reveal model in Models tab` / `Reveal texture in list` / `Copy name` / `Copy SNO`.

### Wardrobe tab

**Slot cells and look-grid cards share one builder** (`addItemActions`) — a slot cell and a
browser icon are the same idea: a picture of one item you want to do something with.

```
Clear                                            ← slot cells only. FIRST, and DISABLED when empty
─────────────────────────────────
Copy pigment                                     ← armour slots only; weapons are not dyeable
Paste pigment                                    ← DISABLED until a pigment has been copied
Clear pigment
─────────────────────────────────
Copy image                                       ← DISABLED, not hidden, when there is no icon
Save image…                                      ← DISABLED, not hidden
─────────────────────────────────
Export model to last folder (…/exports)  —  12 animations + 4 raw files
Export model  —  12 animations + 4 raw files…
─────────────────────────────────
Copy SNO  (1234567)
Copy file name  (…)
Copy name  (…)
Copy collection name  (—)                        ← DISABLED when empty
```

**`Clear` comes first and is disabled rather than hidden.** It is the one action about the
*slot* rather than the item in it, and it is the one people reach for most; it was last, which
meant scanning past a dozen item actions to unequip. Everything below `Clear` is appended by
the shared item builder, which adds nothing at all when the slot is empty.

The **export suffix** is computed live from *Settings ▸ Export* and states what the export will
actually contain: animation count (or `clip: <name>` when scope is "playing clip"), plus raw
source file count when raw export is on. The literal `1 model` component is **stripped** — it
carries no information where only one item can be selected — and what remains is appended as
`  —  <parts joined by " + ">`.

**Look cards** additionally offer, above the export block:

```
Equip
─────────────────────────────────
Equip Theme  (Sunfire Helm, Sunfire Mail, Sunfire Grips, +3 more)
Equip Theme Armor  (Sunfire Helm, Sunfire Mail, Sunfire Grips, +2 more)
Equip Theme Markings  (Ember Sigil)
Equip Theme Weapons  (None)                      ← stays ENABLED; see below
─────────────────────────────────
[the shared item builder: image · export · copy, exactly as above]
```

Each theme action **names what it will equip in parentheses**, resolved live: duplicates
removed, at most three names, then `+N more`. An empty scope renders `(None)` and the action
**stays enabled** here — the label already says it will do nothing, and greying four sibling
actions inconsistently reads worse than one that no-ops. (Stable's equivalents *are* disabled,
because there the scope is a single named piece rather than a set — see below. Follow each
tab's rule; they differ deliberately.)

Weapon slots the class cannot use are greyed **with a reason**, matching the game.

A look card must end by calling the shared item builder. It used to re-implement that block
inline and omit exactly the image actions, so a look-grid card silently offered less than the
slot cell for the same item — while a comment claimed the two shared it.

**Pigment cards:**

```
Apply to this slot
Apply to all slots
─────────────────────────────────
Copy name  (Crimson)
Copy colours  (#8B1A1A #A32020 #6E1414 #C03030)  ← all four zone hexes, space-joined
─────────────────────────────────
Delete custom pigment                            ← custom pigments only
```

**Memory swatches:** `Clear`. **Colour wheel:** `Reset to white`.

**ANIMATIONS panel:**

```
Export animation library (.glb)…                 ← or "— N clip(s) (.glb)…" when N > 1
Export animation library to last dir
─────────────────────────────────
Copy file name                                   ← copies the CLIP file name (UserRole), not the
                                                    display row "Neutral — barF_nav_idle · 56 frames"
```

If the right-clicked clip is not in the selection, the selection is replaced with it before
exporting — the §2 rule, applied to a list widget.

### Stable tab

Mount/pet cards mirror the Wardrobe card exactly, with mount-specific theme actions:

```
Equip
─────────────────────────────────
Equip theme  (3 items)
Equip Armor  (Gilded Barding)                    ← DISABLED, shows "(None)", when no match
Equip Trophy  (None)                             ← DISABLED
─────────────────────────────────
Export model to last folder (…) — <suffix>
Export model — <suffix>…
Copy image / Save image…                         ← via the shared LookIcon builder
─────────────────────────────────
Copy SNO / file name / name / collection name
```

Stable resolves icons through its own mount/barding/trophy crawl, so the shared builder takes
**the icon**, not an id — what is worth sharing is the pair of actions (wording, order,
disabled-not-hidden), not the lookup.

### Catalogue tab

```
Sunfire Regalia Bundle                           ← DISABLED header (subject name, or bundle title)
─────────────────────────────────
Export bundle "Sunfire Regalia" to last folder (…/D4/shop)
Export bundle "Sunfire Regalia"…
   …or when several bundles are ticked:
Export 3 bundles to last folder (…/D4/shop)
Export 3 bundles…
─────────────────────────────────
Export model "chest_armor" to last folder (…)    ← row/selection scope
Export model "chest_armor"…
   …or: Export 12 selected items…                ← n > 1
   …or: Export image "shop_art_01"…              ← when the row is a texture
─────────────────────────────────
View in Models                                   ← appearance rows
View in Textures                                 ← texture rows
─────────────────────────────────
Copy SNO (98765)
Copy file name (StoreProduct_Sunfire)
Copy name (Sunfire Regalia Bundle)               ← omitted when the bundle has no shop title
```

The row-scope block follows §2 as a **count**, not a view: fewer than two selected assets means
"the row you clicked". A row with no name falls back to `SNO 12345` — never `image "image 12345"`,
which is what naive composition produced.

### Any detail table, anywhere

`CsvCopy::install(view)` gives every table and tree `Copy` / `Copy all` plus `Ctrl+C`: selected
rows, or all rows when nothing is selected, as RFC-4180 quoted CSV including the header.

**Ordering trap, hit three times:** `CsvCopy::install` skips installing its own menu only when
the view *already* has a non-default context-menu policy. So if a view needs a custom menu,
**set `setContextMenuPolicy(Qt::CustomContextMenu)` BEFORE calling `CsvCopy::install`** — or
you get two menus fighting, and the generic one wins.

---

## 6. The menu bar — File · Export · Help

**Exactly three menus.** Anything that would become a fourth (View, Tools, Options) is a
Settings tab or a per-tab toolbar instead. The bar holds only what is genuinely
application-wide.

### File

```
&Settings…                        Ctrl+,
&Reload                           F5
&Index ▸                          submenu — see below
Icon &audit (write report)
Toggle &console window            Ctrl+`
──────────────────────────────
E&xit                             QKeySequence::Quit
```

Note what is **not** here: no "Export settings profile" (it lives in *Settings ▸ General ▸
Settings profile*), no per-tab preferences. When an item's natural home is a Settings page,
it goes there and the menu keeps a comment saying where it went — otherwise it grows back.

**The `Index` submenu** is a submenu rather than one action because each index also reports
its own state and can be rebuilt on its own:

```
Index &all                        tooltip: start everything not already built or running;
                                           anything already done is left alone
&Re-index everything…             tooltip: drop every cache and rebuild from scratch —
                                           minutes of work, use after a game patch
──────────────────────────────
<Index name>  —  done                     ← one row per index, relabelled live
<Index name>  —  running 42%
<Index name>  —  not started
```

Three rules, each of which was a bug:

- **`menu->setToolTipsVisible(true)`.** Qt hides per-action tooltips in menus by default, so
  without this the explanatory text you wrote is simply never shown.
- **Guard every build on storage being open.** Clicking an index row before the game archive
  is ready builds a *degraded* index — the builder accepts a null reader and an empty asset
  list, caches the near-empty result, and marks itself ready. Every later "Index all" then
  skips it as done, permanently. Refuse with a status line instead.
- **Report what actually happened, not what you intended.** A build is a no-op when its own
  data directory is missing; saying "started" there leaves the row re-labelling itself to
  "not started" a moment later with no explanation. Check the state after starting and say
  `nothing to index (check the d4data folder)` when that is the truth.

### Export — one place to export from

This menu is the reason the tool has no per-tab export buttons scattered around. Everything
that writes a file is reachable from here, and the menu **rewrites itself for the active tab**
every time it opens.

```
Export selected models…                      ← noun supplied by the tab, count-aware
Export selected models last dir
Export animations only (.glb) — 12 clips…    ← HIDDEN unless the tab supports it
Export selected TexFrame/s…                  ← HIDDEN unless the tab supports it
Export selected TexFrame/s last dir
Export all TexFrames…
Export all TexFrames last dir
Export all 74 matching bundles…              ← HIDDEN unless the tab supports it
Export all 74 matching bundles last dir
──────────────────────────────
Save preview &image…                         ← enabled when the tab has a live 3D preview
&Turntable GIF…                              ←   "
&Animation loop GIF…                         ← enabled only when that preview HAS animation
──────────────────────────────
Export &settings…                            ← opens Settings ▸ Export directly
```

**The mechanism.** One connection does all of it:

```cpp
connect(exp, &QMenu::aboutToShow, this, &MainWindow::updateExportMenu);
```

The menu relabels and re-enables itself **on open**, not on tab change and not on selection
change. Those are hundreds of events for one that matters, and any of them can be missed;
`aboutToShow` cannot be, because the user cannot read the menu without firing it.

**The tab interface.** Every tab derives from one base class that declares the export hooks,
so `MainWindow` never knows which tab it is talking to. Port this verbatim:

```cpp
class BrowserTab : public QWidget {
    // A live 3D preview to capture, or null.
    virtual GLModelWidget* previewWidget()          { return nullptr; }

    // Can this tab export right now?  ("right now" = there is a selection)
    virtual bool    hasExportSelection() const      { return false; }
    virtual void    exportSelection()               {}   // prompt, then export
    virtual void    exportSelectionToLast()         {}   // straight to the remembered folder
    virtual QString exportNoun() const              { return "selection"; }

    // Rig-only animation export — Models only.
    virtual bool    hasAnimExport() const           { return false; }
    virtual QString animExportLabel() const         { return "Export animations only (.glb)…"; }
    virtual void    exportAnimations()              {}

    // Atlas-frame export — Textures only.
    virtual bool    hasFrameExport() const          { return false; }
    virtual void    exportFramesSelected()          {}
    virtual void    exportFramesSelectedToLast()    {}
    virtual void    exportFramesAll()               {}
    virtual void    exportFramesAllToLast()         {}

    // "Export everything the current filter matches" — Catalogue only so far.
    virtual bool    hasExportAllFiltered() const    { return false; }
    virtual QString exportAllFilteredLabel() const  { return "Export all matching"; }
    virtual void    exportAllFiltered()             {}
    virtual void    exportAllFilteredToLast()       {}
};
```

A tab that doesn't implement a hook gets the default, and the corresponding menu item hides
itself. Adding a capability to one tab is one override; the menu needs no edit.

**Hidden vs disabled — the same law as §7, applied to a static menu:**

| | |
|---|---|
| The tab **cannot do this at all** (animation export outside Models, TexFrames outside Textures, export-all-matching outside Catalogue) | `setVisible(false)` — **hide** |
| The tab **can do this but not right now** (nothing selected; no preview loaded; the preview has no animation) | `setEnabled(false)` — **grey out** |

A permanently-greyed item on every tab but one teaches the user nothing; a *sometimes*-greyed
item teaches them that a selection is required.

**The tab supplies the whole label wherever a count appears**, because only the tab knows the
live count and **a stale count is worse than no count**:

| Tab | `exportNoun()` |
|---|---|
| Models | `selected model` / `selected models` |
| Textures | `selected texture` / `selected textures` |
| Wardrobe | `selected look` |
| Stable | `mount` |
| Catalogue | `3 models + 2 images` · `3 models` · `2 images` · `2 bundles` · `bundle "Sunfire Regalia"` |

Two labels are built entirely by the tab rather than composed from a noun:

- `animExportLabel()` → `Export animations only (.glb) — 12 clips…`, `— 1 clip…`, or
  `— no clips in scope` (that last one carries **no ellipsis**: it opens nothing). Count it
  from **the same function the exporter uses** to decide what to write. It originally reported
  the list *selection* size, which is 0 in the normal case of "export whatever the scope says"
  — so the menu said nothing about how many clips were coming, and a settings change that
  halved the export looked identical from here.
- `exportAllFilteredLabel()` → `Export all 74 matching bundles`, with `…` or ` last dir`
  appended by MainWindow.

**Capture actions** sit below a separator because they produce *images of the viewport*, not
asset files. `Animation loop GIF…` tests `previewWidget()->animFrameCount() > 0`, a strictly
stronger condition than "has a viewport" — the other two only need the viewport.

**Shortcuts** are assigned from the central `Hotkeys` registry (§ the template's Hotkeys
section), applied by walking the action array in the **same order** as `Hotkeys::defs()`.
Keep the two lists adjacent in the file; a reordering that desyncs them binds the wrong keys
silently.

**`Export settings…` belongs in this menu**, even though it opens the Settings dialog. Someone
who has just noticed the export options are wrong is standing in this menu, not hunting
through File.

### Help — the post-patch triage menu

```
&Shortcuts…                       F1  and  Shift+/     ← two shortcuts, one action
──────────────────────────────
&Health check…
Audit bulk &presets…
&Copy log to clipboard
&Export log…
──────────────────────────────
Open &data folder
Open &log folder
&Copy diagnostic info
──────────────────────────────
&About
```

- **`Shortcuts…`** is a cheat sheet for the viewport keys, which are otherwise invisible.
  Bind both `F1` and `Shift+/` (i.e. `?`) — `setShortcuts({...})` takes a list.
- **`Health check…`** is one screen that verifies storage, keys, metadata freshness, index
  state and live format probes. It answers *"what broke?"* in seconds after a game update, and
  it is the first thing to open after a patch.
- **`Audit bulk presets…`** measures the built-in Bulk Extract presets against the current
  data. They are hard-coded queries against a dataset that changes every patch: a family the
  game renames turns its preset into a silent zero, and nothing in the UI distinguishes that
  from a preset nobody happened to click. Measuring is one pass, so it is a menu item rather
  than a thing to remember.
- **`Copy log to clipboard` exists alongside `Export log…`** deliberately. Pasting a log into
  a bug report or a chat is the common case; writing a file only to attach it is pure friction.
  Report what was copied: `Log copied to clipboard — 1,284 lines, 96 KB`.
- **`Export log…`** suggests a timestamped name (`d4browser_log_20260830_142301.txt`) and, on
  success, says *"attach it to a bug report"* — the status line is where you tell people what
  to do with the file they just made.
- **`Copy diagnostic info`** produces a paste-ready block containing everything a bug report
  needs and nobody thinks to include: app version and build date, Qt version, OS, GPU string,
  archive open/closed state and build id, the configured game and metadata paths, and the
  index counts. One click, then paste.
- The tool **writes no log file on its own** — the log lives in a memory buffer and reaches
  disk only through this menu. No stray files beside the exe.

### Mnemonics

Give every bar menu and every item an `&` accelerator, and check for collisions within each
menu. `E&xport` rather than `&Export` because `&E` is wanted elsewhere; `E&xit` in File for the
same reason. In a *label containing a literal ampersand*, double it (`Un-block && render`) or
Qt eats the next letter into a mnemonic.

---

## 7. Omit vs disable — the deciding rule

| Situation | Behaviour |
|---|---|
| The action is **structurally impossible** here (a texture has no collection; a Models part has no owning outfit piece; there is no remembered export folder) | **Omit entirely.** The menu shape legitimately differs between object kinds. |
| The action **applies to this kind of object but has no value right now** (an item with no icon; an empty collection field; a theme scope that matched nothing) | **Disable, don't hide.** The menu keeps a stable shape and the absence reads as *"this item has no icon"* rather than *"this menu is different"*. |

Disabled entries still show their value slot, rendered as `—`.

In the **menu bar** the same law reads slightly differently, because the menu is static and
every tab sees it: *structurally impossible for this tab* → hide (`setVisible(false)`);
*possible but not right now* → grey out. See §6.

---

## 8. Porting checklist

- [ ] `ViewportPartMenu.h` copied, including the whole `MenuText` namespace and its comment block
- [ ] `LookIcon.h`-equivalent for the icon action pair (disabled-not-hidden)
- [ ] `CsvCopy` installed on every table/tree — **policy set before install**
- [ ] `contextSnos(view, pos)` takes the view; rejects subtree nodes; `clickedSno` separate
- [ ] Single-subject actions use the clicked row, list actions use the selection
- [ ] "To last folder" variants omitted when nothing is remembered; folder named via `condensePath`
- [ ] Every copy action carries its value; empty → `—` and disabled; previews cut at 30 chars
- [ ] `…` appears only via `prompts()`, only on dialog-opening actions
- [ ] Multi-select copy joins with `\n`; labels read `Copy X  —  N rows`
- [ ] Every value snapshotted before `menu.exec()` — no pointers into index-owned containers
- [ ] A `closed` callback clears transient highlight state on every dismissal path
- [ ] List and grid (and every other view of the same objects) compose from the same builders
- [ ] Grep the codebase for inline `addAction(QStringLiteral(` — every user-visible menu string
      should resolve to `MenuText`, and anything left inline is a future drift

**Menu bar:**

- [ ] Exactly three menus: File · Export · Help. A fourth means something belongs in Settings
- [ ] `BrowserTab` export-hook interface ported; every tab overrides only what it supports
- [ ] Export menu wired to `aboutToShow` — never to tab-change or selection-change signals
- [ ] Unsupported capabilities **hidden**; supported-but-unavailable ones **greyed**
- [ ] `exportNoun()` count-aware per tab; count-bearing labels built by the tab, from the same
      function the exporter uses
- [ ] Index submenu: `setToolTipsVisible(true)`, storage-open guard, honest post-hoc status
- [ ] Hotkey action array in the same order as the central registry's table
- [ ] Help carries: shortcuts sheet (F1 + `?`), health check, copy-log, export-log, open data
      folder, copy diagnostic info, about — and the app writes no log file unasked
