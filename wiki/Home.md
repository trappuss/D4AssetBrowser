# D4AssetBrowser

A Diablo IV asset browser and 3D wardrobe / mount studio. It reads your installed game
directly, decodes textures, and previews or exports appearances, armour, weapons, mounts
and pets as animated `.glb` — with cloth physics, dyes, markings, hair, makeup and
animations.

Windows 10/11 x64 · a Diablo IV install · a GPU with OpenGL 4.5. Single native
executable — no Python, no external extractor.

> Not affiliated with or endorsed by Blizzard. For personal use with a copy of the game
> you own. No game assets and no decryption keys are distributed with it.

## Start here

| | |
|---|---|
| **[Install](Install)** | Download, first run, and the two folders you have to point it at. |
| **[Tabs](Tabs)** | What each of the six tabs is for. |
| **[Exporting](Exporting)** | Formats, scopes, animations, and the options that change the result. |
| **[After a game patch](After-a-patch)** | What to re-download and how to find out what changed. |
| **[Diagnostics](Diagnostics)** | The reports built into the tool, and what each one answers. |
| **[Troubleshooting](Troubleshooting)** | Nothing renders, a piece is white, a model is missing parts. |
| **[Asset formats](Asset-formats)** | How the data is actually put together — storage, SNO groups, naming, materials, textures, cloth, animation, encryption. Measured, with the method for checking any of it yourself. |
| **[Building from source](Building)** | Prerequisites, the build, and cutting a release. |

## Where things live

Everything the tool writes goes in `data\` beside the exe — settings, caches, logs,
saved looks. There is no installer, no registry footprint outside Qt's own settings, and
moving the folder moves the whole installation.

## Reporting a problem

Open an [issue](https://github.com/trappuss/D4AssetBrowser/issues) and attach
`data\D4AssetBrowser.log`. It names the exact asset that failed and, where relevant, the
decryption key it needed. For a rendering fault, right-click the part and use
**Explain this material** — paste that report in too; it is the difference between a fix
and a round of guessing.
