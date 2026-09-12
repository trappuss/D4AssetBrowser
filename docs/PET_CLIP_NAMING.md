# Companion (pet) clip naming — measured

Measured from `CoreTOC.dat.json` on the live d4data snapshot: **67,733 Appearance records,
45,681 Anim records**. Everything below is a count, not a reading of the naming grammar.

## The finding

The game names companion clips **two ways**, and any resolution keyed on the appearance's
variant can only ever see one of them.

| family | example | belongs to |
|---|---|---|
| stem-named | `cmp_stor100_murloc_idle` | that one appearance |
| species-named | `CMP_dogLarge_nav_idle` | every `*_dogLarge` variant, shared |

28 of 48 companion appearances ship a stem-named set. The rest have only the species-named
family — which carries **no variant token at all**.

## What that cost

The tab's pet token was `appr.section('_', 0, 1)` — the first two segments, e.g.
`cmp_stor105`. `clipBuckets` filters candidate files on
`name contains "mnt" || "mount" || token`, so a species-named clip matched none of the three
and was discarded **before ownership was ever consulted**:

- **13 of 48** companions could not see a single one of their clips
- **15 of 48** saw only part of the set
- the species token (last segment, via `catOf`) sees **100% of both families for all 48**

## Per-appearance counts

`stem` = clips named `<appearance>_*` · `species` = clips named `cmp_<species>_*`

| appearance | species token | stem | species |
|---|---|---:|---:|
| `cmp_base000_catDomestic` | `catdomestic` | 0 | 28 |
| `cmp_base000_crab` | `crab` | 0 | 14 |
| `cmp_base000_dogLarge` | `doglarge` | 0 | 20 |
| `cmp_base000_fox` | `fox` | 0 | 0 |
| `cmp_base000_goatman` | `goatman` | 0 | 17 |
| `cmp_base000_murloc` | `murloc` | 14 | 0 |
| `cmp_base000_quillrat` | `quillrat` | 16 | 0 |
| `cmp_base000_woodwraith` | `woodwraith` | 3 | 15 |
| `cmp_base001_bird` | `bird` | 0 | 0 |
| `cmp_base001_dogLarge` | `doglarge` | 0 | 20 |
| `cmp_base002_bird` | `bird` | 0 | 0 |
| `cmp_stor100_bearLarge` | `bearlarge` | 0 | 14 |
| `cmp_stor100_bearSmall` | `bearsmall` | 0 | 0 |
| `cmp_stor100_bird` | `bird` | 9 | 0 |
| `cmp_stor100_cacodemonOG` | `cacodemonog` | 11 | 0 |
| `cmp_stor100_catDomestic` | `catdomestic` | 0 | 28 |
| `cmp_stor100_chickenLarge` | `chickenlarge` | 14 | 0 |
| `cmp_stor100_chimeraMount` | `chimeramount` | 14 | 0 |
| `cmp_stor100_deer` | `deer` | 15 | 0 |
| `cmp_stor100_dogLarge` | `doglarge` | 0 | 20 |
| `cmp_stor100_fox` | `fox` | 15 | 0 |
| `cmp_stor100_gardenDefiler` | `gardendefiler` | 11 | 0 |
| `cmp_stor100_goatman` | `goatman` | 0 | 17 |
| `cmp_stor100_goatSmall` | `goatsmall` | 14 | 0 |
| `cmp_stor100_hydralisk` | `hydralisk` | 14 | 0 |
| `cmp_stor100_lostSoul` | `lostsoul` | 11 | 0 |
| `cmp_stor100_murloc` | `murloc` | 14 | 0 |
| `cmp_stor100_otter` | `otter` | 14 | 0 |
| `cmp_stor100_pachimari` | `pachimari` | 14 | 0 |
| `cmp_stor100_pantheraKitten` | `pantherakitten` | 0 | 0 |
| `cmp_stor100_quillrat` | `quillrat` | 2 | 0 |
| `cmp_stor100_ragnaros` | `ragnaros` | 14 | 0 |
| `cmp_stor100_schnoz` | `schnoz` | 15 | 0 |
| `cmp_stor100_spiderLarge` | `spiderlarge` | 17 | 0 |
| `cmp_stor100_spiderSmall` | `spidersmall` | 13 | 0 |
| `cmp_stor100_wyvern` | `wyvern` | 9 | 0 |
| `cmp_stor101_bird` | `bird` | 7 | 0 |
| `cmp_stor101_dogLarge` | `doglarge` | 0 | 20 |
| `cmp_stor101_fox` | `fox` | 6 | 0 |
| `cmp_stor101_goatSmall` | `goatsmall` | 3 | 0 |
| `cmp_stor101_pantheraKitten` | `pantherakitten` | 0 | 0 |
| `cmp_stor101_wyvern` | `wyvern` | 9 | 0 |
| `cmp_stor102_bird` | `bird` | 0 | 0 |
| `cmp_stor102_dogLarge` | `doglarge` | 0 | 20 |
| `cmp_stor102_fox` | `fox` | 6 | 0 |
| `cmp_stor102_woodwraith` | `woodwraith` | 0 | 15 |
| `cmp_stor103_dogLarge` | `doglarge` | 20 | 20 |
| `cmp_stor105_dogLarge` | `doglarge` | 0 | 20 |

Three own nothing under either family and correctly show no clips: `cmp_stor100_bearSmall`,
`cmp_stor100_pantheraKitten`, `cmp_stor101_pantheraKitten`.

## Mounts, for contrast

Only **8** Anim records start with `mnt_`, and all eight are FX (`mnt_stor052_horse_wings_flap`,
`mnt_amor124_cat_stor_wings_idle`). Real mount clips are prefixed by the *rider*:
`MERC_Shieldbearer_HTH_mount_horse_nav_gallop`, `NPC_mount_chimera_nav_idle`. That is why the
filename filter carries `"mount"` — 1,019 of the 1,110 files it admits for `horse` match on
that arm, not on the species.

It is also why a mount must NOT prefer its own appearance the way a pet does: what a mount skin
owns in its own name is those FX extras, so "own first" would hand the user one wing-flap in
place of thirty gaits.

## Why the stage-3 fallback matches on segment boundaries

Species tokens are short, and the bucket set for *every* token already contains the whole mount
corpus (the `mnt`/`mount` arm is unconditional). Unanchored substring matching is not safe:

| token | anchored | unanchored | false matches |
|---|---:|---:|---:|
| `crab` | 21 | 91 | 70 |
| `cat` | 236 | 365 | 129 |
| `bird` | 18 | 32 | 14 |
| `chimera` | 233 | 249 | 16 |
| `deer` | 34 | 40 | 6 |
| `horse` | 357 | 360 | 3 |
| `fox` | 58 | 58 | 0 |

`deer` ⊂ `reindeer`, `crab` ⊂ many, `cat` ⊂ `catapult`/`indicator`/…. Anchoring to `_`
boundaries removes all of them.

## What is still open

This is a **name** study. Which appearance a `CMP_dogLarge_*` clip actually names in its
`snoAppearance` is not in CoreTOC, and the fix's stage 2 assumes that owner is a
`cmp_base*_<species>` appearance. Run the app once with **`D4_DUMP_PETANIM=1`** and the log
answers it per companion — any line reading `(shared rig)` whose carrier is not a `cmp_*`
appearance is the case stage 2 misses.

Two appearances own clips in *both* families — `cmp_base000_woodwraith` (3 stem / 15 species)
and `cmp_stor103_dogLarge` (20 / 20). The carrier is a single sno, so stage 1 returns their own
bucket and the species set is never reached. Closing that means carrying a SET of carriers, not
one; it is not done.
