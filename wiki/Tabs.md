# The six tabs

Switch with <kbd>Ctrl</kbd>+<kbd>1</kbd> … <kbd>Ctrl</kbd>+<kbd>6</kbd>. If you do not know
which tab owns the thing you are looking for, press <kbd>Ctrl</kbd>+<kbd>K</kbd>, type its
name or SNO id, and let the tool decide.

| Tab | The question it answers |
|---|---|
| **[Models](#models)** | *What is this asset, and what is it made of?* |
| **[Wardrobe](#wardrobe)** | *What does this look like on a character?* |
| **[Stable](#stable)** | The same, for mounts and pets. |
| **[Textures](#textures)** | *What is in this image, and who uses it?* |
| **[Catalogue](#catalogue)** | *What was sold, when, and what was in it?* |
| **[Bulk Extract](#bulk-extract)** | *Export all of these at once.* |

Right-click works nearly everywhere, and the same object offers the same actions wherever
you find it — list, grid, outliner, Parts panel and 3D viewport all raise one menu. If you
are unsure what a surface can do, right-click it before hunting for a hotkey.

---

## Models

The general browser: everything the game has an appearance record for — 67,733 of them —
in a live PBR viewport.

**Three views of one list.** *List* is dense flat rows. *Outliner* is a scene tree where the
loaded model's parts, looks, animations and bones hang off its row. *Grid* is thumbnails.
Switch from the display dropdown in the header; the list itself does not change, only how you
read it.

**One search box, four syntaxes.**

| | |
|---|---|
| `text` | name and tags |
| `123456` | a SNO id |
| `#tag` | matches tags, title and collection — but *not* the filename |
| `c:some collection` | collection; reads to end of line, so put it last |

Space-separated terms must all match, and a leading `-` excludes: `pandem -destroyed -pillar`,
or `-#cape`. <kbd>Ctrl</kbd>+<kbd>F</kbd> focuses, <kbd>Esc</kbd> clears,
<kbd>↓</kbd> recalls your last ten searches. The same syntax drives Bulk Extract and Textures,
so learning it once is worth it.

**Filters** live in a funnel popup that stays open while you tick things: grouped tag
checkboxes (Category, Class, Gender, Type) with **Match any (OR)**, plus *Only decrypted*,
*Only encrypted (TACT)*, *Hide un-renderable*, and the usage facets *Latest* (new this game
update), *Animated*, *Rigged* and *Orphaned*. Every active filter appears as a removable chip,
so you can always see why a list is short.

**Viewport.** Four shading modes — Wireframe, Flat, Shaded, and Rendered with IBL, shadows,
SSAO and tonemapping. A **channel viewer** for Base Colour, Normal, Roughness, Metallic, AO
and Emissive, which you can cycle by scrolling the **⌄** beside the shading balls. Overlays
for statistics, ground grid, axis gizmo, skeleton, hardpoints, collision capsules, physics
bones (anchored grey, simulated orange), bone names and translated bone names. **FX / SIM /
GIB** submesh toggles. Popovers for Graphics, Pigment, Camera and Lighting.

→ [Keyboard & mouse](Keyboard-and-mouse#the-3d-viewport--models--wardrobe--stable) ·
[Glossary: FX / SIM / GIB](Glossary#rigging-and-motion)

---

## Wardrobe

A character built from real equipment slots, dyed the way the game dyes.

**Ten slots.** Helm · Torso · Gloves · Legs · Boots · Main hand · Off hand · Sheath ·
Sheath 2 · Back trophy. Slots your class cannot use are greyed out rather than hidden, so the
absence is legible. Each slot has its own search box and collection filter.

**Nine creator categories**, each filtered to what your class and gender can actually use:
Face, Hair style, Hair colour, Eye colour, Facial hair, Makeup, Marking, Marking colour,
Jewelry — plus skin tone and a skin-detail overlay (freckles, vitiligo). Eight classes
including Paladin and Warlock, both genders. Eye colour is composited from the game's own
base, normal, ORM and emissive maps rather than tinted.

**Markings come from the game as well as the snapshot.** The community snapshot describes 304
of the 374 body markings the game ships. The missing 70 are newest-first, so collaboration
sets — the Berserk *Brand of Sacrifice* among them — were exactly the ones absent, with
nothing on screen to explain why. Those are now read straight out of the game and drawn with a
swatch composited from the marking's own mask and default colour.

**Pigments — the game's dye system, rebuilt.** Dyes are read from the game's own `Dye`
records, each swatch taken as **four colours, one per DyeMask zone**, and rendered the way the
game shader does it: the mask's red channel picks the zone, a ramp texture supplies the value
multiplier, and unmasked texels stay undyed. NPC, debug and hidden-from-UI dyes are filtered
out.

- **Set Look / Set Pigment** per slot, mirroring the in-game flow
- Per-slot pigments, or **Apply to all slots** across the five armour slots
- **Custom pigments** — colour wheel, four numbered zone buttons, hex entry, and *Save
  Pigment* to name your own; customs carry a gold border in the library
- **Eight memory swatches** — drag a colour in to store, click to apply, right-click to
  clear; colours drag from one zone onto another
- **Copy / Paste / Clear pigment** between slots from the slot right-click menu
- Weapons are correctly **not** dyeable, greyed with a reason, matching the game

**Ensembles.** Save the whole look — class, gender, skin, all nine creator picks, all ten
slots, every per-slot pigment, and the animation. The tile art is **a real viewport snapshot
taken at save time**, not a generic icon. Save · Overwrite · Delete · Rename; double-click to
load. Right-click a card for **Equip Theme**, which equips the whole matching set at one of
four scopes — everything, armour only, markings only, weapons only — and frames what changed.

**Auto Animate** *(Settings ▸ Wardrobe)*. When your **weapon class** changes, the tool plays
the game's own wardrobe unsheathe clip once and settles into that loadout's idle, resolved
from the shipped `ui_wardrobe` AnimSets. Armour changes do not trigger it.

**The Parts panel** is the assembled character broken into submeshes. Unticking one hides it;
the **FX**, **SIM**, **FORM** and **HED** buttons above the viewport hide whole categories.
Selection is shared with the viewport in both directions.

**Also:** <kbd>Ctrl</kbd>+<kbd>Z</kbd> undo, 30 deep · attached models (back trophy, weapons)
keep their own rigs and play their own clips from a pinned ATTACHED list · camera snap-to-slot
· cloth physics.

---

## Stable

Mounts and pets across three slots — **Mount** (labelled *Pet* for companions), **Mount
Armor** and **Trophy** — with the same viewport, cloth simulation and animation handling as
the Wardrobe. Species-aware: horse, cat and basilisk rigs differ.

**Equip matching set** builds the themed trio from the game's own bundle data, with individual
*Equip Armor* and *Equip Trophy* entries. Trophies seat onto the correct mount bone.

Mounts are not dyeable in Diablo IV, so there is deliberately no dye control here — an absence
that is a design decision rather than a gap.

**Where the animations live differs by kind**, and this is the source of two historical bugs.
A mount's clips can belong to a **storefront** appearance rather than the base one the lookup
assumed, which left basilisks and several mounts listing nothing. Pet clips are named **two
ways** — per appearance and per species — and matching only one left 13 of 48 pets with no
animations and 15 more with a partial list. Both are resolved from the data now rather than
from a naming assumption.

---

## Textures

Every texture in the game, decoded — BC1, BC3, BC4, BC5 and BC7.

**Inspection.** Channel isolation (RGB · R · G · B · A), an alpha checkerboard, a cubemap and
array face selector, and a **pixel inspector** reporting `(x, y) RGBA` under the cursor.
Scroll to zoom, drag to pan, double-click to reset. Images drag straight out into another
application.

**Filters.** By format, by **gear tags** (the class, type and gender of appearances that use
the texture), orphans-only, decrypted-only. Search takes `#tag`, bare SNO digits and
`-exclude`.

**TEXFRAMES** lists the sprite frames packed into an atlas, with an optional **Trim** cropping
each export to its tight bounds — which is what turns a 60-icon sheet into 60 usable icons
rather than 60 copies of the sheet.

**ASSOCIATED MODELS** walks the reference graph the other way: texture → material →
appearance, with a jump straight to the model in Models. It is the fastest way to answer
"what is this map actually for".

**Exported normal maps get their blue channel rebuilt** *(Settings ▸ Export ▸ Texture
export)*. Diablo IV stores normals as BC5, which carries only two channels; the third is
implied and reconstructed at render time. That is why the in-app preview always looked right
while the exported PNG lit wrong in Photoshop, Blender and Substance. Filling the channel with
white by hand is not the same thing — measured, it flattens relief by about 4% on average and
up to 24% on the steepest texels. **Only exports are affected**; the preview and the channel
tiles still show the game's own bytes.

Column layout — widths, order, which columns are shown — is remembered between sessions.

→ [Glossary: Textures](Glossary#textures) ·
[Asset formats §7](Asset-formats#7-textures)

---

## Catalogue

The Cosmetics Shop, browsable: every bundle it has sold, with hero art, card and lore text,
and each item inside resolved to the appearance or texture it actually is. Search matches the
shop title, the SNO name and the lore.

**Filters** use the same funnel as the other tabs — contents kind, **class**, **slot**, patch,
season, *Latest* (new in this game update) and *Reward only* — plus a sort by name, season, patch
or SNO. Active filters show as removable chips and tint the funnel.

*Class* and *slot* are the game's own authored fields, not guesses from a name, and both are asked
of what a bundle **contains**: a pack matches when any piece inside it does. Their entries are
built from the data, so a class or a slot added in a later patch appears by itself, and each is
labelled with how many rows it will return. A product authored for every class is not treated as
belonging to one class — picking *Necromancer* means the Necromancer's own cosmetics, not the
thousands of pieces everyone can wear.

*Reward only* is the shop's `requires` relationship, and it answers a question the shop's own
listing cannot: which cosmetics were never sold, and arrived with a battlepass or a season pass
instead.

**Ctrl+F** focuses the search from anywhere in the tab; **Esc** inside it clears.

**Two views of the contents, mirrored.** The shop's own *INCLUDES 8 ITEMS* strip shows each
piece's real inventory icon, one row per gender, because armour resolves to a female and a
male appearance and both are openable. Beneath it a tree carries SNOs and the **SLOT** each
piece occupies — read from the item → gear reference graph, **not** guessed from the name —
and a plain *no appearance found* wherever a product could not be resolved. Selecting in
either pane selects in the other. The bundle's own shop art is its own branch.

**Collection packs nest, and the tree follows them down.** A pack like *Warcraft Collection
Pack* does not contain items; it contains one bundle per class, each holding that class's five
to seven pieces. Those sets appear as group rows carrying a piece count, with the real pieces
underneath and their own slots. A set has no slot of its own and shows none — a set of five is
not a helmet, even though the helmet is the first thing inside it.

**How a row gets its picture, and why a few have none.** Four sources are tried in order: art
named after the product, then art the product itself authors, then the icon of the first thing
it contains, and last its payload actor’s own portrait — which is all a companion or a mount
trophy has. Art shared across a whole family — a class banner, a promotion's artwork — is
deliberately *not* used, because one picture repeated down a screen of rows reads as real data
in a way a blank does not. A row that ends up blank says which of those it is when you hover
it: locked, nothing authored, or nothing of its own. Locked rows are the largest group and are
genuinely unknowable — the record is encrypted.

Where the tool looks for shop art is measured from the installed game build rather than fixed
in the tool, so a new season's artwork appears without waiting for an update. `Probe -
Catalogue Icons.bat` prints the current coverage without launching the app.

**Where else did this ship?** Right-click any piece — in the strip or in the contents tree — for
*Also sold in*, listing every other product that carries the same item, with its season. Picking
one goes there. The same helmet often appears in a class bundle, a mega bundle and a later
re-release, and until now nothing in the tool could say so.

**The art pane is a viewer.** Scroll to zoom about the cursor, drag to pan, double-click to fit
back to the window. Right-click it for the same export and copy actions the lists carry. Picking
any row under *Bundle images* shows that image here at full resolution instead of only naming it —
the corner strip says which one you are looking at, its pixel size and the zoom.

**Double-click** any item to open it in Models, textured, with its parts tree and animations.
Bundle art opens in Textures.

**Provenance.** Supported classes, whether it shipped with VFX, its associated season, and the
shop's own *requires* / *add-on to* / *excludes* relationships. That is how you discover a
mount trophy was never sold at all but was a Season 3 premium-pass reward.

**Bundles the snapshot has never heard of are read from the game itself.** d4data describes
about 7,500 shop products; the game has 9,310. The ~1,800 in the gap are the encrypted and
newly-patched ones — the Doom collaboration armour was missing for every class except Druid
and Rogue purely because only those two shipped a description file. Those records are parsed
from the game's binary now, so their contents, artwork and models are present and export
normally. What the game files do *not* carry is the shop's display text, so such a bundle
shows its asset name and says so plainly: *"read from game files — no shop text in this
snapshot"*. Re-downloading d4data once it catches up fills the text back in.

---

## Bulk Extract

Filter the whole index, watch the match count update live, then export everything at once.
Same funnel filters as Models, same search syntax.

**Three modes.** *Models* extracts appearances as `.glb`. *Textures* extracts game textures as
images. **Both** runs the two queries over the same NAME box in one run — models go into the
folder layout, textures into a `textures\` subfolder beside them. Both is how you reach loose
maps no material binds: fur masks, dye masks and ramps, atlas sheets, recolour variants.

The funnel's tag filters apply to the **model** half only, because textures carry no tags; the
texture half is narrowed by the texture category checkboxes instead. Both mode shows both sets
of controls at once and reports the two counts separately — *"1,547 model(s) + 5,922
texture(s) match"* — so you can always see which filter is acting on what. Rows carry a
`[model]` / `[tex]` tag, and each mode keeps its own queue.

**Presets.** 34 built-in, under the ★ entries: per class an *Appearance*, a *Textures* and an
*— Everything*, plus Global & Base, Weapons, Mounts, Back Trophies and Body Markings.
**Help ▸ Audit bulk presets** resolves every one through the real matcher and writes
`data\preset_audit.txt` with its match count — a preset that a game rename quietly reduced to
zero is invisible in the UI until you extract it.

**Pick items manually** moves matches into a persistent **Queue** that survives filter
changes, mode switches and restarts — so you can build a list across several sessions before
committing to a run.

**Options.** Include textures · all animations · pulled animations · raw sources. **Parallel**
workers, auto-set to your core count. **Only new** skips anything already exported, tracked in
a `_bulk_manifest.json` ledger, or **Overwrite** to redo. A live console with a working Cancel
(or <kbd>Esc</kbd>) and a Pause/Resume that excludes paused time from the ETA. Failures land
in `_bulk_failed.txt` with a reason each, and **one bad model cannot take the run down**.

**Output layout** is one choice — Flat, or a subfolder per class, per type, or per model — and
it is a property of exporting models rather than of this tab, so every batch path obeys it:
Bulk Extract, a multi-selection from Models, the context-menu batch, *export all*. Single-model
exports deliberately do not, because <kbd>Ctrl</kbd>+<kbd>E</kbd> quietly becoming
`Barbarian\foo.glb` is a surprise you cannot undo. Inside a group the shape is identical in
every mode — the models, plus `deps\`, `textures\` and `buffers\` for whichever options are on
— which is what makes Flat simply *one group at the root* rather than a special case.

**Repeated textures decode once per run.** A set of appearances typically shares the same
detail and dye maps, so a run-scoped cache makes the second and later uses a memory hit rather
than a fresh BC decode. It is bounded, lives only for the run, and writes nothing to disk.

Everything this tab used to keep its own copy of — raw buffers, loose textures, the CSV report
— lives in **Settings ▸ Export** alongside the rest, so each setting has one home and one
value regardless of which tab starts the run.

→ [Exporting](Exporting) · [Settings](Settings#export--everything-that-lands-on-disk)
