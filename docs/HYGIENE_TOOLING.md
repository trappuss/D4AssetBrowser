# Hygiene tooling — what the tool now checks about itself

Five defect classes in this project have each shipped more than once, been found by a person
noticing something missing, and been fixed one instance at a time. This is what now catches them
mechanically, plus the two reports that answer the questions those investigations always start from.

---

## 1. `verify-src.py` — three new whole-tree checks

All three follow the existing **inventory + baseline** contract: current entries are listed as
"reviewed, not a failure", and only *growth* fails. None proves a defect on its own — each says
"a human should look at this one", which is precisely what did not happen for any of the bugs below.

### Settings keys written and never read

A key is a string, nothing validates it, and a control wired to a dead one looks like it works
forever. Four dead keys came out of the Stable parity audit by hand; two more (R5) are still open.

The check is concatenation-aware, which is what makes it usable: most of this codebase reads grouped
keys as `value(QStringLiteral("wardrobe2/viewport/") + leaf)`, and a first draft that ignored that
reported fifteen keys of which fourteen were fine. With prefix and tail concatenation understood, it
reports **one** — and that one was real:

> `tex/listHeader` — the Textures tab has saved its column layout on every header-menu close since
> that menu was added, and nothing ever read it back. Un-hiding NAME or COLLECTION lasted exactly as
> long as the session. Fixed: restore on build, save on resize/move/sort like the Models tab, with a
> fallback if the saved state hides every column and a debounce on resize (stretch-last-section fires
> `sectionResized` on every window drag, and each save was a registry write).

### Combos persisted by `currentText()`

The display string is not an identity — relabel an item and every saved profile silently loses that
selection. Baseline 4, all in `WardrobeTab2.cpp`, all reviewed and **correct**: the armour slots, the
look presets and the theme resolver speak appearance NAMES end to end, so the label *is* the identity
at that layer. `skinTone`/`skinDetail` were the two that did not fit (their items carry a colour and a
style token their consumers actually use) and were converted to `currentData()` — that was R6.

### Character-vs-equipment decided by a material name

`head` is a substring of `wolfHead`, `brow` of `browplate`, `lash` of `backlash`. Baseline 22, of
which 18 are the Wardrobe classification loop. Not all are wrong — some decide *shading*, where no
slot tag applies — but every one is a place where a material name is asked a question it cannot
answer. See `PALADIN_MATERIAL_SHARING.md` for the one that took a whole torso off screen.

---

## 2. `src/app/ViewportSettings.h` — one inventory of the three tabs' render state

Models, Wardrobe and Stable each own a full copy of the same rendering controls under three prefixes,
never held in one place, and drifted in every way three hand-maintained copies can:

| | Models | Wardrobe | Stable |
|---|---|---|---|
| self-shadows | `viewport/shadows` | `viewport/shadow` | `light/shadowStr` |
| fur / FX | `viewport/fur*`, `viewport/fx*` | `viewport/fur` | `fur/*`, `fx/*` |
| background | `viewport/bg` | `viewport/bg` | `gfx/bg` |
| skin SSS default | 0.24 | 0.24 | **0.15** |
| reset by Restore Defaults | no | ten keys, by name | no |

The header does **not** unify the schemes — renaming a shipped key orphans every saved value for no
gain, and both spellings are load-bearing in their own tabs. It names the three groups once, says
which subtrees are render state, and gives one correct way to reset a group.

**Reset by removal, not by writing defaults.** The obvious design is a table of key → default value.
It is also the design that guarantees a new bug: the table's value and the reader's own fallback are
two copies of one number, and when they disagree the app renders at one and reports the other. That
is exactly R3 (slider 15, renderer 24), which shipped for months. Removing the key makes every reader
fall back to the value it already carries at its read site, so there is nothing to keep in sync and a
newly added setting is covered the day it lands.

Saved light, camera and cloth presets live under the same subtrees and are **work product** — a
"Restore Defaults" that deletes them is data loss wearing a reassuring label. `keepPrefixes()` lists
them and the reset skips them.

Wired up: Restore Defaults now resets all three viewports and reports how many settings it cleared,
and the Stable tab gained a **Clear Stable memory** button — it previously appeared nowhere in the
dialog, so a mount that failed to load on restore could only be escaped by editing the registry
(that was G18).

The Stable SSS divergence is **left alone and recorded**. Stable is internally consistent about it
(slider and consumer agree), so it reads as a deliberate per-tab look for creatures rather than a
bug. Noted in the header so the next person does not "fix" it in either direction without deciding
which answer is wanted.

---

## 3. Two reports over data the tool already had

### "Which appearances use this material?"

`AssetLinks` has built and cached a material → appearances map since it existed, and the only way in
was through a *texture* — so the question could not be asked at all, even though the answer was in
memory. It is now the **USED BY** section at the bottom of "Explain this material…", one right-click
from any part in any of the three tabs. Nothing new is built or cached; it is a hash lookup.

This is the question every material bug starts from. The Paladin HED bug was found by sweeping
CoreTOC by hand; this answers it directly.

Two traps worth recording, both caught in review:

* The section first re-derived the material sno through `SnoIndex::snoForName`, which **skips
  `~unnamed_` placeholders** and knows only group 57 — narrower on both counts than the sno the
  report had already resolved fifteen lines earlier through `MaterialDecode::snoForMaterial`, which
  reads the sno straight out of a placeholder and covers groups 37 *and* 57. It would have answered
  "no sno" for exactly the encrypted materials the section exists to trace. It now reuses the one the
  report already prints, so the two cannot disagree.
* An appearance sno with no name is **not** necessarily encrypted. `AssetLinks` is built from d4data
  while the index normally comes from live CASC, so a d4data snapshot *ahead* of the installed build
  lands there routinely. The label says "(no name in this index)".

### Help ▸ Patch contents

`SnoIndex` keeps a **build ledger** — one record per game build the tool has been opened on, holding
the SNOs that first appeared in it. It drives the "Latest" filter in Catalogue, Models and Bulk
Extract, and until now the only way to read it was to tick Latest in one tab and see what survived,
which answers "what is new in THIS tab" and never "what did the patch contain".

This prints the whole ledger, newest build first, each build's additions grouped by SNO type and
named. Read-only; it records nothing of its own.

The ledger's limits are printed rather than hidden: nothing in CoreTOC stamps an asset with the build
that introduced it, so a build the tool was never opened on cannot be reconstructed, and when two
observed builds are not consecutive each entry says what it was diffed against.

Three things review caught here too: an sno the index no longer holds was being labelled
"(unnamed — encrypted)" under a header that said "(no longer present)" — asserting a fact about an
asset this build cannot see; names were being fetched with `nameForSno`, which builds a full reverse
map for the whole group and keeps it for the index's lifetime (tens of MB across Texture + Material +
Appearance, to print 25 names each) when the walk was already standing on every entry; and the
per-group cap bounded one group, not the report, so a long ledger could hand `QPlainTextEdit` several
MB in one string.

---

## 4. Help ▸ Find SNO

"What is sno 2462986" is where nearly every investigation here begins, and answering it meant
grepping a 43 MB CoreTOC dump outside a tool that holds the same table in memory. One box takes an id
or part of a name and reports group, name, and — from indexes that are already loaded, never by
probing — whether it is new this build, an appearance's collection, and how many appearances use a
material.

Deliberately a **lookup, not a navigator**. Jumping to the owning tab needs a reveal hook on
`BrowserTab` implemented across five tabs with five different selection models; this is the part that
needs none of that, and it is the part the question actually asks for.

Two things review caught, both about *when* facts are true rather than what they are:

* The `m_reloading` guard was checked before `QInputDialog::getText`, which runs a **nested event
  loop** — so a reload deferred as `m_reloadPending` could start its worker (whose first act is
  `clear()` on the index) while the dialog was open, and the search then walked freed storage. The
  guard is now re-checked after the dialog, which is the only point at which it means anything.
* Material lives in **two** groups, 37 and 57. This window's own name resolver takes both; a lookup
  that knew only 57 would have reported "used by nothing" for every group-37 record.

## 5. Help ▸ Diagnostic output

Every probe ends the same way: a differently-named `.txt` or `.csv` appears beside the exe and you go
hunting for which file just changed. The submenu lists them, newest first, with size and age, and
opens any of them in the report dialog.

Derived from the filesystem when the menu opens, **never** from a hard-coded table of probe names. A
table would be wrong the first time a probe is added or renamed, and wrong silently — which is the
failure this whole family of tooling exists to avoid. The one exclusion is the TACT key list, matched
by canonical path rather than by name because its location is free-form.

This is the discoverable-and-readable half. The other half — running a probe from inside the app
instead of setting an environment variable and relaunching — needs each probe to be callable
independently of startup, which most are not today. That is the remaining work, and it is per-probe
rather than one change.

## 6. Still scoped, not built

**Reveal in the owning tab.** A new virtual on `BrowserTab` implemented in Textures, Models,
Catalogue, Wardrobe and Stable, each with its own selection model, so Find SNO can jump rather than
report. The lookup above is the useful 80% and stands on its own; this is the remaining 20%.
