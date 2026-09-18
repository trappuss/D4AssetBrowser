# D4AssetBrowser

A Diablo IV asset browser and 3D wardrobe / mount studio. It reads your installed game
directly, decodes textures, and previews or exports appearances, armour, weapons, mounts
and pets as animated `.glb` — with cloth physics, dyes, markings, hair, makeup and
animations.

Windows 10/11 x64 · a Diablo IV install · a GPU with OpenGL 4.5. Single native
executable — no Python, no external extractor, no installer.

> Not affiliated with or endorsed by Blizzard. For personal use with a copy of the game
> you own. No game assets and no decryption keys are distributed with it.

**[⬇ Download the latest release](https://github.com/trappuss/D4AssetBrowser/releases/latest)**

---

## Where do I start?

**I just want it running.** → **[Install](Install)** takes about five minutes, most of
which is a download. Then **[The six tabs](Tabs)** tells you what each part of the app is
for, and **[Keyboard & mouse](Keyboard-and-mouse)** is one page you can keep open beside it.

**I want to get a model into Blender.** → **[Exporting](Exporting)**. Read
*[The normal-map convention](Exporting#the-normal-map-convention)* before you judge how
anything looks — it is the single most common reason an export looks wrong in a DCC when
it looked right in the viewport.

**Something is broken.** → **[Troubleshooting](Troubleshooting)** covers the faults people
actually hit, in the order they hit them. If it is not there,
**[Diagnostics](Diagnostics)** shows you how to get the tool to tell you what it knows.

**The game just updated.** → **[After a game patch](After-a-patch)**.

**I want to understand the data itself.** → **[Asset formats](Asset-formats)** is the long
one: how Diablo IV actually stores everything, measured against a retail build rather than
repeated from other projects. **[Glossary](Glossary)** is the short one, for when you hit a
word like *SNO* or *hardpoint* and want a sentence rather than a chapter.

**I want to build it or send a patch.** → **[Building from source](Building)**.

---

## Every page

| Page | What it answers |
|---|---|
| **[Install](Install)** | Download, first run, the two folders you point it at, and what to do when Windows flags the exe. |
| **[The six tabs](Tabs)** | Models, Wardrobe, Stable, Textures, Catalogue, Bulk Extract — what each is for and when to use which. |
| **[Keyboard & mouse](Keyboard-and-mouse)** | Every binding, including the viewport ones that are not written on any button. |
| **[Settings](Settings)** | What each of the nine settings tabs controls, and the handful of options that change results rather than looks. |
| **[Exporting](Exporting)** | Formats, scopes, animations, the normal-map convention, and which option you actually want. |
| **[After a game patch](After-a-patch)** | What to re-download, how to see what the patch added, and how to tell "new" from "broken". |
| **[FAQ](FAQ)** | Short answers to the questions that come up most. |
| **[Troubleshooting](Troubleshooting)** | Nothing renders · a piece is white · a model is missing parts · the character is bald · a setting will not stick. |
| **[Diagnostics](Diagnostics)** | Find SNO, Patch contents, Health check, Explain this material, the audit scripts, and the log. |
| **[Asset formats](Asset-formats)** | Storage, SNO groups, directories, naming, materials, textures, cloth, animation, encryption — measured, with the method for checking any of it yourself. |
| **[Glossary](Glossary)** | One-line definitions of the vocabulary the rest of the wiki uses. |
| **[Building from source](Building)** | Prerequisites, the build, the source checks, CI and cutting a release. |

---

## Where things live

Everything the tool writes goes in `data\` beside the exe — settings, caches, logs, saved
looks, exported presets. There is no installer, no registry footprint outside Qt's own
settings, and moving the folder moves the whole installation. Deleting `data\` resets the
app to a first run without touching the game.

## Reporting a problem

Open an [issue](https://github.com/trappuss/D4AssetBrowser/issues) and attach
`data\D4AssetBrowser.log`. It names the exact asset that failed and, where relevant, the
decryption key it needed.

For a rendering fault, right-click the part and use **Explain this material** — paste that
report in too. It is the difference between a fix and a round of guessing: it names the
shader, every texture slot with its format and resolution, which ones resolved and which
did not, and every appearance that shares the material.
