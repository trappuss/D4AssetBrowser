# Glossary

The vocabulary the rest of this wiki uses, one entry at a time. Everything here is either a
term from the game's own data or a term this project uses consistently. Where a number
appears it was measured against a retail build — see
**[Asset formats](Asset-formats)** for how, and
**[section 13](Asset-formats#13-how-to-verify-a-claim-about-this-data)** for how to check
any of it yourself.

---

## Storage and identity

**SNO** — the game's asset id. Every record of every kind has one, and it is the *only*
identity that survives a rename. `2469678` is `PalF_sets50_LEG`. When this wiki says "select
by reference, not by name", it means: follow the SNO.

**SNO group** — the *kind* of record a SNO is. Group 9 is Appearance, 44 is Texture, 57 is
Material, 6 is Anim. There are 133 groups holding 862,731 records in total; this project has
verified names for 24 of them and deliberately leaves the rest unnamed, because unverified is
not the same as wrong. A SNO id is only unique *within* its group, so an id without a group is
half an address.

**CoreTOC** — the game's master table mapping SNO → name, for every group. It is where the
tool learns what anything is called. Records whose content is encrypted appear in CoreTOC with
a **blank name**, which is why "unnamed" and "missing" are different things.

**CASC** — Blizzard's content storage format, and how the game install is laid out on disk.
Paths inside it are numeric (`base/meta/<sno>`), not readable. It is always current with the
installed build, which is the reason this tool reads the game directly rather than relying on
a dump.

**TVFS / BLTE** — the layers inside CASC: TVFS is the file-system table, BLTE the per-block
container that carries compression and, where present, encryption.

**TACT key** — the decryption key an encrypted record needs. The tool does not ship any. When
a piece will not load and the log names a key, that is what it is naming.

**d4data snapshot** — `DiabloTools/d4data`, a community dump of the meta records as readable
JSON, organised by group *name* rather than by number. Convenient, and **always behind the
game**. Measured: the game ships 374 body markings, the snapshot describes 304. The 70-record
gap is exactly where new content lives.

---

## Models

**Appearance** — group 9, and the record most things in this tool ultimately are. An armour
piece, a hairstyle, a weapon, a mount, a face. It names the mesh, the material roster, the
skeleton and the animation sets. 67,733 records.

**Primitive** *(also submesh, or "part")* — one drawable chunk of an appearance, with one
material. What the Parts panel lists and what the viewport selects. A single torso piece can
easily be five primitives: the armour, a cape, a strap, a trim and a simulated cloth panel.

**Material** — group 57, 102,751 records. Holds the texture roster and points at a shader map.
21,202 of them end in `_mat` — a convention, not a rule, which is why the tool does not key on
the suffix.

**ShaderMap** — group 37, 3,633 records. What a material's `tUberMaterial.snoShaderMap` points
at, and **the reliable way to ask what a submesh is**. `hero_opaque_skin` is skin, `hero_hair`
is hair *and* facial hair, `hair_pbr_igc` is eyelashes, `Hero_Eye` is eyes, `vfx*` is effects.
Names drift between classes; shaders do not.

**Slot** — the authored equipment position: helm (`HLM`), torso (`TRS`), gloves (`GLV`), legs
(`LEG`), boots (`BTS`). Measured across 67,163 named appearances, those five carry essentially
all of it. `HED` — the character's own head — appears on just 11. The distinction matters:
**slot is authored data, and it is how the tool tells an equipment piece from part of the
body.** A name test cannot, which is how a pauldron ornament called `wolfHead` once hid an
entire torso.

**Collection** — the tool's grouping of assets by where they came from (a set, a store
product, a class). `c:name` in the Models search box filters by it.

---

## Textures

**Texture definition** — group 44, 145,563 records. Points at the actual image data and
describes its format.

**BC1 / BC3 / BC4 / BC5 / BC7** — block-compression codecs. The tool decodes all five.
Which one a texture uses changes how it must be handled, so decisions are gated on the
**codec**, never on the file name.

**The implied Z** — BC5 stores only two channels, so a BC5 normal map has **no blue channel**;
the renderer reconstructs it as `nz = √(1 − x² − y²)`. Export the decoded bytes untouched and
B is zero, which lights wrong in every DCC. Filling B with white is not a fix — measured, it
loses 4.2% of the surface tilt on average and 23.6% on the steepest texels.

**DirectX vs OpenGL normals** — which way green points. Diablo IV authors **DirectX**: green
points toward the bottom of the texture. glTF mandates OpenGL, and Blender is OpenGL, so
**exporting to glTF means flipping green**. Only a DirectX target wants the channel as
decoded. See [Exporting](Exporting#the-normal-map-convention).

**ORM** — the packed roughness / metalness / ambient-occlusion texture the glTF exporter
writes, one channel each.

**Detail map** — a tiling normal and roughness the game layers over the base maps per dye
zone: fabric weave, leather grain, scale texture. A plain material dump leaves it behind,
which is why an exported piece can look flat next to the same piece in the viewport. The
exporter composites it in, zone by zone.

**Atlas** — one image holding many icons, cut up by a frame table. Icons live this way, which
is why "export the icon" and "export the texture" are different operations.

---

## Rigging and motion

**Skeleton** — the bone hierarchy an appearance is bound to. Shared across a class's gear, so
a torso and a pair of boots animate together.

**Hardpoint** — a named attachment socket on the skeleton: where a weapon sits in the hand,
where it sits when sheathed, where a back trophy hangs. Offsets are **per class**, which is
why ignoring them seats a sword correctly on a Barbarian and wrongly on everyone else.

**AnimSet** — group 8, 11,915 records. A named bundle of animation clips. The Wardrobe's
emote list is driven by the game's own wardrobe AnimSets, which is why the emotes are named
the way the game names them.

**Clip** — one animation. Pets are the awkward case: the game names their clips two ways, per
appearance *and* per species, and matching only one of the two leaves 13 of 48 pets with no
animations at all and 15 more with a partial list.

**Cloth** — group 11, 15,481 records. The simulation definition for a garment: which vertices
are simulated, how stiff, what collides with them. Chain, fabric and hair are different
regimes and behave wrong if treated alike.

**Collision capsule** — a body-region-aware capsule the cloth solver collides against.

**Sim mesh** — the low-resolution cage that is actually simulated; the visible mesh follows it.
Primitives flagged `SIM` in a parts dump are these.

**FX / SIM / GIB** — submesh flags. `FX` is an effect layer (glows, ribbons), `SIM` is
simulated cloth, `GIB` is gore geometry. Each has its own viewport toggle, because you rarely
want all of them in an export.

---

## Cosmetics

**Dye zone** — a region of a piece that takes a colour independently. Pigment is applied per
zone, which is why one armour can carry several dyes at once.

**Marking** — group 115, MarkingShape, 374 records. Body art: tattoos, brands, scarring. 70 of
the 374 are absent from the community snapshot, including every Berserk *Brand of Sacrifice*.

**Marking colour · hair colour · eye colour · makeup** — groups 133, 134, 131 and 132. Small
tables, each a palette rather than a texture.

**Hair colour is not a ramp.** The array is `[shadow, highlight, mid]`. Walking it in order
hands the brightest strands the dullest stop, and the result reads as flat neon plastic.

**Back trophy** — the ornament that hangs behind a character. Named `trophy_<class><NN>_stor`,
**not** `back_*`; scanning for `back_*` returns six placeholder proxies and zero real
trophies.

**Series** — the game's own name for a set, stored as a `Series` row in a **store product's**
string table (`StoreProduct_<name>.stl.json`). Measured: 7,017 of the snapshot's 61,330 string
tables carry the row, and every one of them is a store product — item, emblem, marking, emote and
actor tables carry none. A thing you can own therefore inherits its set name from the product that
sold it. The value is quote-wrapped as authored (`"Ash Knight"`), and per-class bundles append the
class (`"Ash Knight" Barbarian Equipment`).

**Collection** — a Series after that tidying: one named set, every piece the products under it
sell, and a derived **source**. Seasonal when a product names a season or sits behind `requires`
("came with a pass, never sold alone"), Shop when neither, Uncategorised when every product for it
is TACT-locked and the answer is unreadable. Nothing in the data marks a set as *promotional*, so
there is no such category rather than a guessed one.

---

## Words this project uses in a specific way

**Measured** — checked against a retail build or the game's own tables, with the method
recorded. Anything in this wiki marked *(measured)* can be re-derived.

**Unnamed vs missing** — *unnamed* means the record exists and its name is encrypted;
*missing* means the record is not there. The tool labels these differently on purpose,
because they call for different responses.

**Fail closed** — when a lookup cannot answer, say so rather than substituting something
plausible. An asset that is merely newer than the snapshot must not be indistinguishable
from an asset that does not exist.

**Placeholder / proxy** — a stand-in record the game ships so a reference resolves to
*something*. The shader `hero_opaque_hollow` marks one specific kind. An empty texture roster
is **not** a placeholder test — Paladin's face alone carries five legitimate empty-roster
materials.
