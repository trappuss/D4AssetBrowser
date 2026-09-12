# Tabs

| Tab | What it does |
|---|---|
| **Models** | Browse and inspect all 67,000-plus appearances in a live PBR viewport. |
| **Wardrobe** | Dress a character — armour, weapons, dyes, hair, markings, animations. |
| **Stable** | The same for mounts and pets. |
| **Textures** | Browse and decode every texture in the game. |
| **Catalogue** | The Cosmetics Shop — every bundle, what was in it, and export the lot. |
| **Bulk Extract** | Filter the index and export in one run. |

## Models

The general browser. Everything the game has an appearance record for, in a list or a
grid, with a live viewport. Right-click a part for its material, its source, and what it
would export as.

## Wardrobe

A character built from real equipment slots — helm, torso, legs, gloves, boots, two weapon
hands, a back trophy — plus the creator options: face, hair, facial hair, eyes, makeup,
skin tone, body markings and jewellery. Dyes apply per slot and per zone. Animations come
from the game's own wardrobe AnimSets, and attachments (weapons, back trophies) animate on
their own timelines alongside the body.

The part list on the right is the assembled character broken into submeshes. Unticking one
hides it; the FX, SIM, FORM and HED buttons above the viewport hide whole categories.

## Stable

Mounts, barding, mount trophies and pets, with the same viewport, cloth simulation and
animation handling. Mounts and pets differ in where their animation clips live — a mount's
belong to a base appearance, a pet's are shared across every variant of its species — and
the tool resolves each from the data rather than from a naming assumption.

## Textures

Every texture in the game, decoded. Channel views isolate base colour, normal, ORM, masks
and the dye ramp. The **Associated models** panel answers the other direction: which
materials use this texture, and which appearances use those materials.

Column layout — widths, order, which columns are shown — is remembered.

## Catalogue

The Cosmetics Shop as shipped: every bundle, its contents, its season and its patch. Sort
by name, season, patch or SNO. Products whose records are encrypted are shown rather than
hidden, so a gap reads as "locked" instead of "missing".

## Bulk Extract

A filter over the whole index plus an export run. Built-in presets cover the common
families; `Audit bulk presets` under Help reports how many assets each preset currently
matches, which is how a preset that a game rename quietly reduced to zero gets noticed.
