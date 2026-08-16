# Store bundles — data research for a "Bundles" tab

Written 2026-08-02. Everything below is measured against the current d4data snapshot and the
installed game build, not inferred. Counts are reproducible with the commands noted.

---

## 0. The headline: we exclude the entire store catalogue today

SNO **group 110 is `StoreProductDefinition`** — the whole Cosmetics Shop. `definitions.json` says so
outright:

    StoreProductDefinition   snoGroup=110   size=592   fields=73

`SnoIndex.cpp:43` calls it **`"Power (3)"`**, and `SnoIndex.cpp:67` lists it in `excludedGroups()`
with the comment *"Python fork's EXCLUDED_GROUPS: Powers (29/104/110)"*. So the mislabel was
inherited from the Python fork and the exclusion followed it.

Consequence: **9,308 store products, including 1,628 named bundles, are invisible to every tab.**
Fixing this is step zero and costs two lines — rename 110 to `StoreProduct` in the group map and
drop it from `excludedGroups()`. Groups 29 and 104 should be checked the same way before trusting
their labels.

---

## 1. What a "bundle" actually is

There is no separate bundle type. A bundle is **a StoreProduct whose `arBundledProducts` is
non-empty**; its children are ordinary StoreProducts. Same struct, one level of nesting.

    Bundle_HArmor_rog_stor251  (sno 2162860, eType 0)
      ├─ Helm_Cosmetic_Rogue_251_stor    → snoItemTransmog 2162794
      ├─ Chest_Cosmetic_Rogue_251_stor   → snoItemTransmog
      ├─ Gloves_Cosmetic_Rogue_251_stor  → snoItemTransmog
      ├─ Pants_Cosmetic_Rogue_251_stor   → snoItemTransmog
      ├─ Boots_Cosmetic_Rogue_251_stor   → snoItemTransmog
      ├─ marking_rog061_stor             → snoMarkingShape 2162822
      ├─ twoHandCrossbow_stor055         → snoItemTransmog
      └─ sword_stor106                   → snoItemTransmog

Each child entry in the JSON already carries `__targetFileName__`, `name` and `__group__`, so
walking a bundle costs no extra lookup.

Four more relationship arrays exist and are worth surfacing: `arAddOnBundles`, `arHiddenProducts`,
`arRequiresOwning`, `arRequiresNotOwning`.

## 2. The payload pointers — what a leaf product can grant

Exactly one of these is set per leaf. This is the complete list from the 73-field struct:

`snoItemTransmog` · `snoMount` · `snoEmote` · `snoMarkingShape` · `snoJewelry` · `snoEmblem` ·
`snoHeadstone` · `snoTownPortal` · `snoHairStyle` · `snoFacialHair` · `snoCompanion` · `snoPower` ·
`snoDyeArmor`

Frequency over a 500-product random sample: `snoItemTransmog` 304, `snoMarkingShape` 12,
`snoHeadstone` 9, `snoEmote` 7, `snoEmblem` 5, `snoTownPortal` 4, `snoMount` 4, `snoCompanion` 2.
Transmogs dominate; the long tail is what a bundle view would otherwise silently drop.

`eType` classifies the product (0 = container/bundle, 1 = transmog, then 2–24 for the rest). The
enum names are not in d4data — `eType` is a bare int, so the labels have to be derived by
correlating with the payload field that is set. Do not hard-code a guess.

## 3. Names and lore text

**Not in `json/base`.** Display names live in group **42 (StringList)** under
`json/enUS_Text/meta/StringList/StoreProduct_<productName>.stl.json` — 59,310 files. Each has
`arStrings[]` of `{szLabel, szText}`:

    StoreProduct_Bundle_HArmor_rog_stor251
      Name        "The Lost Zealot"
      Description "Wheezing through sulfur-choked lungs, she marched toward the demonic
                   legionnaire. …"

The tool already reads this directory (`AnimActionIndex.h:56` does it for emotes), so the plumbing
exists. This is the only source of the shop-facing name — the SNO name is `Bundle_HArmor_rog_stor251`,
which is not what the wiki or the shop calls it.

## 4. Store art — already decodable as of this session

Fourteen image fields: `hSplashImage`, `hConfirmImage`, `hCategoryIcon`, `hTileImage`,
`hSmallTileImage`, `hOwnedTileImage`, `hSmallOwnedTileImage`, `hPurchaseCompleteImage`,
`hDetailsDisplayImage`, `hIconRepresentation`, `hAddOnDetailsScreenImage`, `hStoreIconOverride`,
`hCoinImage`, `hBundleTypeLabel`, plus `reliquaryArt` / `reliquaryArtHeader` / `arCardArtVariants`.

These are **UI image handles in the same handle space `IconIndex` maps** (handle → atlas SNO + frame
+ UV). So `IconIndex::iconImage(handle, reader)` decodes them as-is. Sample usage: `hCategoryIcon`
115/500, `hStoreIconOverride` 88, `hSplashImage` 62, `hConfirmImage` 56.

Separately, each bundle has named group-44 textures following a strict convention:

    2DInventory_Bundle_HArmor_bar_stor251              the card art
    2DUI_Bundle_HArmor_bar_stor251                     shop tile
    2DUI_Bundle_HArmor_bar_stor251_details
    2DUI_Bundle_HArmor_bar_stor251_background
    2DUI_Bundle_HArmor_bar_stor251_WebImage

## 5. Getting from a bundle to MODELS — the one real trap

`snoItemTransmog` → Item (group 73) → Actor → Appearance **does not work**. It resolves for every
readable item and lands on the *proxy body mesh*: all seven classes' chest items point at appearance
`217477`. This is already recorded in `ENCRYPTED-CONTENT-HANDOFF.md` and was measured, not assumed.
Do not rebuild on it.

The working route is the naming convention the tool already implements:
`AppearanceMeta::cosmeticAppearanceNames(itemName)` and `AppearanceMeta::styleAppearanceNames()`,
which the icon audit and the wardrobe both use. The bundle name encodes class and style token —
`Bundle_HArmor_<cls>_stor251` → `<cls><gender>_stor251_<SLOT>` — which is exactly what
`WardrobeTab2::resolveTheme` / `themeToken()` already does. **A Bundles tab is largely a reversal of
resolveTheme: today we go item → theme; there we would go bundle → items.**

## 6. Other metadata worth showing

`szProductReleaseBranch` gives the patch a product shipped in (e.g. `"2_3_0"`) — a real release
timeline with no guessing. Also `ePointOfOrigin`, `snoAssociatedSeason`, `snoAssociatedCatalog`,
`snoAssociatedExpansion`, `ePassType`, `nSeasonActive`, `bHasVFX`, `bHighlight`,
`fPreviewOnClasses`, and the in-shop preview camera (`snoMainProductPreview`,
`bUseCustomizedPreview`, `tCustomizedPreview`, `StorePreviewCameraPositioning`).

**There is no price field.** Prices are server-side; nothing in the client data carries them.

## 7. Numbers

| Measure | Value |
|---|---|
| Group-110 entries in CoreTOC | 9,308 |
| …named | 7,495 |
| …**blank = encrypted** | **1,813** |
| `.prd.json` files in d4data | 7,496 |
| Named products with no `.prd.json` | 1 (`Axe Bad Data`) |
| Named `Bundle_*` | 1,628 |
| Products with children, sampled | 128 of 500 (26%) |
| Children per bundle | 1–14, mean 3.9 |

Bundle families by name: HArmor 424, Battlepass 353, BPArmor 111, HMount 76, Promo 54,
Accessory 54, Sub 44, MountAmor 40, mnt 35, Collection 30, ArmorVar 30, BPWeapons 27,
BPAccessories 21, emblem 19, Companion 17, then weapon types.

## 8. The encrypted 1,813

Every collab/store bundle we have been chasing this session is in that blank set — no name in
CoreTOC and no `.prd.json`. Two of the three legs are already solved:

* **Names** — the `EncryptedNameDict` pass added this session recovers them from CASC.
* **Art** — `TextureDefTable` + the `IconIndex` CASC pass decode their textures and icons.
* **The struct itself** — still unread. `Bundle_HArmor_bar_stor251` has no JSON, so
  `arBundledProducts` cannot be walked for exactly the bundles most worth viewing.

**This is where the schema-driven reader we deferred earns its keep.** `definitions.json` gives
`StoreProductDefinition`'s 73 field offsets and `DT_VARIABLEARRAY` layout, so the same reader that
would replace `AppearanceMatBin` would parse a StoreProduct straight out of `base/meta/<sno>` with
no d4data at all. One reader, both problems.

## 9. Suggested shape for the tab

**Tree**: bundle → children grouped by payload kind (Armour / Weapons / Mount / Marking / Emote /
Emblem / Headstone / Portal / Companion / Dye). Header shows the StringList Name + Description, the
card art, the release branch and the season.

**Export "everything"** would mean, per bundle:
1. every appearance resolved from each transmog child, per class and gender — models,
2. their materials and textures via the existing wardrobe/models export path,
3. the bundle's own 2D art (card, tile, background, details, web image),
4. each child's icon (via the image handles),
5. a manifest JSON — names, SNOs, lore text, release branch.

**Filters**: class, release branch/patch, season, payload kind, has-VFX, encrypted-only.

## 10. Open questions before building

1. `eType`'s enum is unnamed — derive the labels from payload correlation, or leave the raw int?
2. Battlepass products (353 `Battlepass_*` + `BP*`) are structurally StoreProducts. Same tab or
   filtered out?
3. Do we want the reverse index too — "which bundle does this appearance belong to?" — which would
   give the Wardrobe and Models tabs a proper "Collection" answer for store items.

---

### Reproducing

    # group id and field list
    python3 -c "import json;d=json.load(open('definitions.json'));
      [print(v['name'],v['snoGroup'],len(v['fields'])) for v in d.values()
       if isinstance(v,dict) and v.get('name')=='StoreProductDefinition']"

    # counts
    ls json/base/meta/StoreProduct/*.prd.json | wc -l
    python3 -c "import json;d=json.load(open('json/base/CoreTOC.dat.json'));g=d['110'];
      print(len(g), sum(1 for n in g.values() if not n))"
