# Troubleshooting

## Nothing loads at all

Check the game folder in **File ▸ Settings** points at the installation itself, and that both
downloads under **Directories** have completed. **Help ▸ Health check** reports which of the
two is missing rather than leaving you to guess.

## A piece renders white, or with no roughness or metal

Almost always an encrypted record with no key, or an asset newer than the metadata snapshot.
Right-click the part and use **Explain this material** — it says which, and whether the values
on screen are authored or substituted. Then re-download the TACT keys and d4data.

## A model is missing parts

Around 10% of wardrobe appearances render incomplete. The model parser decodes only vertex
buffer 0; sub-objects on other buffers are dropped rather than drawn scrambled. This is
measured and documented rather than fixed — a missing piece here is a known limit, not
something wrong with your install.

If a *whole* piece is missing rather than a few submeshes, check the part list on the right of
the viewport: a part hidden by the FX, SIM, FORM or HED buttons is greyed there with a tooltip
naming the button responsible, because ticking its checkbox cannot override a category toggle.

## The character is bald, or the torso vanished

Check **HED**. It hides the character's head group — face, teeth, tongue, brows, lashes. It
does not hide worn equipment, and as of 2.3.0 it cannot: a helm or an ornament whose material
name happens to contain a head word is no longer swept up by it.

## An asset appears in the game but not in a list

Three possible causes, in order of likelihood:

1. **It is encrypted and no key covers it.** ~11,500 assets are in this state.
2. **The metadata snapshot has not described it yet.** The tool falls back to the game's own
   binary tables, so it often still renders — but it may have no name, and name-keyed lists
   cannot show a nameless record.
3. **It has no name at all.** 93 appearances decode perfectly and have none. Cloth-bearing
   pieces can have their name recovered from their cloth data; plain helms, gloves and boots
   cannot.

## The app crashes on startup, every time

The Wardrobe leaves a breadcrumb while it loads an outfit. If it crashes mid-load, the next
launch detects the breadcrumb, clears that remembered selection and opens on a safe tab. If it
is still looping, **Settings ▸ Clear Wardrobe memory** or **Clear Stable memory** forgets the
outfit or mount entirely without turning the remember feature off.

## A setting does not seem to stick

**Settings ▸ Restore Defaults** resets the viewport settings of all three 3D tabs — lighting,
shading, overlays, cloth — without touching your folders, your saved light, camera and cloth
presets, or your remembered selections.

## Reporting it

Open an [issue](https://github.com/trappuss/D4AssetBrowser/issues) with
`data\D4AssetBrowser.log` attached. For a rendering fault add the **Explain this material**
report; for something missing after a patch add the `Audit - Asset Health.bat` diff. Either one
turns a guess into a fix.
