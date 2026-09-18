# Keyboard & mouse

The app carries this list itself — **Help ▸ Shortcuts**, or press <kbd>F1</kbd> or
<kbd>?</kbd>. That dialog is modeless, so you can leave it open beside the app while you
learn. This page is the same list with the reasoning attached.

---

## Everywhere

| | |
|---|---|
| <kbd>Ctrl</kbd>+<kbd>1</kbd> … <kbd>Ctrl</kbd>+<kbd>6</kbd> | Switch tab |
| <kbd>Ctrl</kbd>+<kbd>K</kbd> | Jump to any model or texture by name or SNO id |
| <kbd>Alt</kbd>+<kbd>←</kbd> / <kbd>Alt</kbd>+<kbd>→</kbd> | Back / forward through jumps |
| <kbd>Ctrl</kbd>+<kbd>,</kbd> | Settings |
| <kbd>F5</kbd> | Reload |
| <kbd>Ctrl</kbd>+<kbd>`</kbd> | Toggle the console |
| <kbd>F1</kbd> or <kbd>?</kbd> | This sheet |

**<kbd>Ctrl</kbd>+<kbd>K</kbd> is the one worth learning first.** Type a name, part of a
name, or a bare SNO id, and it lands you on that asset in whichever tab owns it — you do
not have to know which tab that is. Every jump is recorded, so <kbd>Alt</kbd>+<kbd>←</kbd>
walks back the way you came even across tabs. Following a model into the Catalogue and
back out again is two keys.

---

## The 3D viewport — Models · Wardrobe · Stable

| | |
|---|---|
| Drag | Orbit |
| Right-drag | Pan |
| Wheel | Zoom |
| Middle-click | Re-frame the model |
| **Click** | **Select the part under the cursor; empty space clears** |
| **<kbd>Ctrl</kbd>+click · <kbd>Shift</kbd>+click** | **Add to the selection, or take one back out** |
| **Double-click** | **Frame the part** — camera snap is a Camera-panel option |
| Right-click | Part menu — acts on the whole selection if that part is in it |
| <kbd>Esc</kbd> | Deselect part · exit fullscreen |
| <kbd>F</kbd> | Fullscreen — maximise the viewport in place |
| <kbd>H</kbd> · <kbd>Shift</kbd>+<kbd>H</kbd> · <kbd>Alt</kbd>+<kbd>H</kbd> | Hide selected · solo selected · show all |
| Gizmo click / double-click | Snap to an axis view · toggle orthographic |
| Wheel on the shading **⌄** | Cycle the view channel |

**Selection is a set.** Click one part, <kbd>Ctrl</kbd>-click three more, and all four are
outlined in blue. Right-clicking a part that is already in the selection scopes the menu to
all of it — *Export 4 parts (7,412 tris)…*, *Isolate 4 parts*, *Copy 4 material names*.
Right-clicking a part **outside** the selection replaces the selection with that one part
first, so the blue outline and the menu can never disagree about what is about to happen.
The Parts panel and the viewport are the same selection seen twice; changing it in either
place updates the other.

**<kbd>F</kbd> and <kbd>H</kbd> are scoped to the viewport**, not global. Click the viewport
once to give it focus. That is deliberate: an unscoped <kbd>F</kbd> would eat the letter *f*
typed into a search box.

**<kbd>H</kbd> needs a selection**; <kbd>Alt</kbd>+<kbd>H</kbd> does not, because showing
everything again is the one visibility action that is always safe. Hiding works from the
Parts tree as well as the viewport, since hiding parts is something you do while looking at
the model.

---

## Export & capture — rebindable

These six live in **Settings ▸ Hotkeys** and can be set to anything, or left unbound.
Defaults:

| | |
|---|---|
| <kbd>Ctrl</kbd>+<kbd>E</kbd> | Export selection… |
| <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>E</kbd> | Export to last folder |
| <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>A</kbd> | Export animations only… |
| <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>I</kbd> | Save preview image… |
| *(unbound)* | Turntable GIF… |
| *(unbound)* | Animation-loop GIF… |

**Export to last folder** is the one that changes how the tool feels. Set a destination
once with *Export selection…*, and every export after that is a single key with no dialog.
Rebinding takes effect immediately — no restart.

---

## Timeline — Models

| | |
|---|---|
| Wheel on the slider | Step one frame |
| <kbd>Shift</kbd>+wheel | Playback speed |

---

## Search boxes

The Models search box takes three prefixes as well as plain text:

| | |
|---|---|
| `c:name` | Filter by collection |
| `#tag` | Filter by tag — the funnel button lists them all |
| `12345` | Bare digits are an SNO lookup |

<kbd>↑</kbd> / <kbd>↓</kbd> in a search box walks the recent-search history;
<kbd>Esc</kbd> closes that popup, and a second <kbd>Esc</kbd> clears the box.

---

## Lists, grids and trees

| | |
|---|---|
| <kbd>↑</kbd> <kbd>↓</kbd> <kbd>←</kbd> <kbd>→</kbd> | Move through a grid, row and column aware |
| <kbd>PgUp</kbd> / <kbd>PgDn</kbd> | Ten at a time |
| <kbd>Home</kbd> / <kbd>End</kbd> | First / last |
| <kbd>Enter</kbd> or <kbd>Space</kbd> | Load the item under the cursor |
| <kbd>Esc</kbd> | Clear the selection |
| Double-click | Open the item in Models — textured, with its parts tree and animations |
| Drag out | Drop a model straight into Blender or Explorer |

---

## Anything not listed here

Right-click it. The same object offers the same actions wherever you find it — the list,
the grid, the outliner, the Parts panel and the 3D viewport all raise one menu, so a
context menu is a faster way to discover what is possible than hunting for a hotkey.
See [The six tabs](Tabs) for what each surface offers.
