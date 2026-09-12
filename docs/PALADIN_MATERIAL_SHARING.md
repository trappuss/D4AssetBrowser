# Paladin armour: head-named ornaments and cross-gender material sharing

Two reported Wardrobe bugs, both on Paladin sets, both about names that do not mean what a
substring test assumes. Everything below is **measured** from `d4data/json/base/CoreTOC.dat.json`
(the sno → name table for all 133 groups) and from `build/release/cloth_audit.csv` /
`matsno_sweep.csv`, not inferred from the source.

---

## 1. The HED toggle hid the whole torso — `palF_stor164_TRS`

**Report.** *"wardrobe > paladin > female/male — the HED toggle is hiding TRS as well,
`palF_stor164_TRS`."*

**Cause.** That set is the wolf-themed Paladin cosmetic, and one of its materials is a wolf head
ornament on the pauldron:

| group | sno | name |
|---|---|---|
| 57 (Material) | 2462986 | `palM_stor164_wolfHead` |
| 9 (Appearance) | 2462975 | `PalM_stor164_wolfHead_static` |
| 44 (Texture) | 2479580 | `PalF_stor164_TRS_fxShell_wolfHeadAlpha_Mask` |

`isHead` tested `effMat.contains("head")`, which `wolfHead` satisfies. That made one primitive of
the chest piece "the character's face". The HED **group-expansion** then did what it was written to
do — flag every primitive of any piece containing a head-ish material, so that teeth, tongue and
lashes travel with the face — and flagged the entire `palF_stor164_TRS`. Turning HED off deleted
the torso.

Both genders are affected because the female set authors no wolfHead material of its own: the only
one in the data is the `palM_` spelling, which the female appearance references.

**Fix.** `primSlot` already records where each primitive came from — 0–4 armour, 5/6 weapons, 9
back trophy, and −1 for the base body and every creator appearance (face, hair, beard, jewellery).
Worn equipment is therefore never the character's head, whatever its materials are called, so
`isHead`, `headCore` and the `hed` token list are all gated on `!isEquipment`, and the
group-expansion is additionally restricted to character pieces.

The gate is written as *"is equipment"* rather than *"is character"* on purpose: a short `primSlot`
leaves it false and every test behaves as before. It can only ever remove head-ness from something
positively known to be worn.

Two deliberate knock-on effects, wider than the report:

* `isSkin` is `isBody || isHead || contains("skin")`, so a piece matching only on `head`/`face`
  stops being skin — which un-suppresses its emissive (`emitsOk` is `!isSkin`). A wolfHead pauldron
  with an authored emissive map will now glow. That is the rule the emissive gate already states;
  it just could not apply while the gear was being read as a face.
* A **helm** submesh named `…_face` / `…_head` is no longer in `hed`, so the HED toggle no longer
  hides it. HED means the character's head, and a helm has its own slot cell to unequip. Only the
  toggle changes — exports key off `headCore`.

**The general rule.** `head`, `face`, `brow`, `lash`, `mouth`, `tooth` are all substrings of
ordinary armour vocabulary (`wolfHead`, `faceplate`, `browplate`, `backlash`). Name tests that
decide whether something is part of the *character* must be scoped by the slot tag, which is
authored data, before they are trusted.

---

## 2. `PalF_sets50_*` authors no materials or textures at all

**Report.** *"`PalF_sets50_LEG` — invisible/missing parts; the male version `PalM_sets50_LEG` has
the rest of the parts, `PalM_sets50_LEG_mat`."*

**Measured.** Every name in CoreTOC containing `sets50` and `pal`:

| | group 9 (Appearance) | group 11 (Cloth) | group 44 (Texture) | group 57 (Material) |
|---|---|---|---|---|
| `PalF_sets50_*` | 5 (LEG TRS HLM GLV BTS) | 2 (both TRS cape) | **0** | **0** |
| `PalM_sets50_*` | 5 | 6 | 55 | 6 |

The female set ships appearances and two cloth definitions and *nothing else*. It is a cross-gender
asset-sharing case: the female pieces reference the male set's materials and, for the legs, the
male set's cloth definitions outright. `cloth_audit.csv` shows the resolution directly:

```
"PalF_sets50_LEG",2469678,"palF_sets50_LEG_loincloth",1,OK,"snoCloth:palM_sets50_LEG_loin_sim",…
"PalF_sets50_LEG",2469678,"palF_sets50_LEG_potion",  1,OK,"snoCloth:palM_sets50_LEG_potion_sim",…
```

`matsno_sweep.csv` shows both genders' LEG appearances carry **3** JSON material entries, so the
female roster is not short — the question is which three names it resolves to and whether they
exist on disk.

**Not yet answered from data.** The `.app.json` files sit eight folders below the connected root,
one past this session's file-staging depth limit, so the roster itself could not be read here.
CoreTOC can say what exists; it cannot say what references what.

**How to answer it.** `D4_DUMP_PIECEROSTER=1` now writes `piece_roster.txt` beside the exe on every
outfit rebuild. Per equipped piece it records:

* whether the appearance has a `.app.json` at all;
* the roster as **both** routes resolve it — the JSON and the CASC meta binary — side by side, with
  a marker when they disagree (they have before, silently);
* for each roster entry, whether that material name has a `.mat.json` on disk;
* every primitive of the piece: its material name, `materialIndex`, triangle count, the
  FX/SIM/FORM/HED/COVERED/CHAR flags, and the visibility those produce.

To run it:

```bat
cd /d "%~dp0build\release"
set D4_DUMP_PIECEROSTER=1
D4AssetBrowser.exe
```

Equip Paladin **female** `sets50` legs, then the **male** equivalent, and the file will hold both.
An empty material name, a `.mat.json=NO`, or a `THE TWO ROUTES DISAGREE` line is the answer.

---

## Sources

* `build/release/data/d4data/json/base/CoreTOC.dat.json` — sno → name, all groups
* `build/release/cloth_audit.csv` — per-appearance cloth submesh → resolved `snoCloth`
* `build/release/matsno_sweep.csv` — per-appearance `jsonMats` count
