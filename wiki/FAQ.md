# FAQ

Short answers. Each links to the long one.

---

## Getting started

**Do I need the game installed?**
Yes. The tool reads your own Diablo IV install and ships no game assets and no decryption
keys. It does not download anything from Blizzard.

**Why does it want a second folder as well as the game?**
The game install is authoritative and current but numeric — paths are SNO ids, not names.
The community JSON snapshot supplies readable names and decoded metadata. The tool uses the
snapshot for names and the game for truth, and says so when the two disagree.
→ [Install](Install#what-the-two-downloads-are)

**Do I need Python, or a separate extractor?**
No. It is a single native executable. Python is only needed if you build from source.

**Windows SmartScreen flagged it.**
Expected for an unsigned executable with no download history.
→ [Install](Install#windows-flagged-the-download)

**Where does it put things?**
Everything goes in `data\` beside the exe. No installer, no registry footprint outside Qt's
own settings. Move the folder and the whole installation moves with it; delete `data\` and
you are back to a first run.

---

## Exporting

**What format do I get?**
Rigged, animated `.glb`. Images as PNG, JPEG or WebP; GIFs for turntables and animation
loops. → [Exporting](Exporting)

**My export looks flat compared to the viewport.**
Detail maps. The game layers a tiling detail normal and roughness over the base maps per dye
zone, and a plain material dump leaves it behind. Turn on **Bake detail maps** in
Settings ▸ Export. → [Settings](Settings#bake-detail-maps--export--model-export)

**Everything looks inverted / the lighting is inside out in Blender.**
The normal-map convention. Diablo IV authors DirectX-style; Blender and glTF are OpenGL, so
green has to be flipped. Pick the preset for where the model is going.
→ [Exporting](Exporting#the-normal-map-convention)

**The normal map has no blue channel.**
It is BC5, which stores only two channels — the third is rebuilt at render time. The
exporter computes it. Filling blue with white instead is not equivalent; measured, it loses
4.2% of the surface tilt on average and 23.6% on the steepest texels.
→ [Glossary](Glossary#textures)

**Can I export just a few parts?**
Yes. Click a part in the viewport, <kbd>Ctrl</kbd>-click more, then right-click inside the
selection — the menu counts what it has and exports exactly that.
→ [Exporting](Exporting#exporting-parts-you-picked-in-the-viewport)

**Does it export hidden parts?**
No. Exports contain exactly what is visible, unless you picked parts explicitly.

**Can I export animations without the mesh?**
Yes — animation libraries are skeleton plus selected clips, for retargeting.

---

## Things that look like bugs but are not

**A piece renders white, or with no roughness and metal.**
Usually an encrypted material: the record exists, its content needs a TACT key the tool does
not have. The log names the key. → [Troubleshooting](Troubleshooting)

**An item is in the game but not in a list.**
Most often it is newer than the community snapshot. The game ships it, the snapshot has not
described it yet. The tool reads the game as a fallback, so it usually resolves — but a
sparse checkout of the snapshot looks identical to "this does not exist".
→ [After a game patch](After-a-patch)

**Something shows as "(unnamed — encrypted)".**
The record is there; its *name* is encrypted. That is different from missing, and the tool
labels the two differently on purpose.
→ [Glossary](Glossary#words-this-project-uses-in-a-specific-way)

**A character is bald, or a torso vanished when I toggled something.**
A classification fault — the tool deciding a piece is part of the body when it is equipment,
or the reverse. These are worth reporting with the piece name; they are usually a single
wrong test. → [Troubleshooting](Troubleshooting#the-character-is-bald-or-the-torso-vanished)

**A mount or pet has no animations.**
Some mounts own their clips through a storefront appearance rather than the base one, and
pets are named two different ways. Both are handled as of 2.3.0; if you find one that is
not, the piece name is the whole bug report.

---

## The data

**What is a SNO?**
The game's asset id, and the only identity that survives a rename. Everything in this tool is
ultimately addressed by one. → [Glossary](Glossary#storage-and-identity)

**How do I find an asset if I only know part of its name?**
<kbd>Ctrl</kbd>+<kbd>K</kbd> anywhere. Name, part of a name, or a bare SNO id, and it lands
you on it in whichever tab owns it. **Help ▸ Find SNO** answers the same question with more
detail — group, collection, how many appearances use a material, and whether it arrived in
this game build. → [Diagnostics](Diagnostics#help--find-sno)

**Can I see what a patch added?**
**Help ▸ Patch contents**, grouped by asset type and named.
→ [After a game patch](After-a-patch)

**Is any of the format documentation guesswork?**
No, and where something is unverified the page says so rather than filling the gap. The
project names 24 of the 133 SNO groups and deliberately leaves the other 109 unnamed, because
unverified is not the same as wrong — inventing names for the rest is precisely the mistake
that produced a documented bug. [Section 13](Asset-formats#13-how-to-verify-a-claim-about-this-data)
gives you the method to re-derive any claim on the page yourself.

**Does any of this apply to other games?**
The tooling patterns do — read the authoritative store rather than a dump, select by
reference rather than by name, gate decisions on codec and shader rather than filename. The
SNO group model and the specific formats do not; they are Diablo IV's.
→ [Asset formats §14](Asset-formats#14-applying-this-to-other-games-on-the-same-stack)

---

## The project

**Is this affiliated with Blizzard?**
No. It is an independent tool for personal use with a copy of the game you own.

**Can I use exported assets in my own work?**
That is between you and Blizzard's terms. The tool takes no position and grants no rights it
does not have.

**How do I report something?**
Open an [issue](https://github.com/trappuss/D4AssetBrowser/issues) with
`data\D4AssetBrowser.log` attached. For a rendering fault add the **Explain this material**
report — right-click the part. It names the shader, every texture slot and format, what
resolved and what did not, and every appearance sharing the material.
→ [Diagnostics](Diagnostics#right-click-a-part--explain-this-material)

**Can I build it myself?**
Yes. → [Building from source](Building)
