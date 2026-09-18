# Install

## First run

1. Download the release `.zip` and unzip it anywhere. It is fully portable — everything
   the tool writes lives in `data\` next to the exe.
2. Run **`D4AssetBrowser.exe`**.
3. **File ▸ Settings** — set your **Diablo IV game folder** (the Battle.net or Steam
   install).
4. **Settings ▸ General ▸ Directories ▸ "d4data folder" ▸ Download.**
5. **Settings ▸ General ▸ Directories ▸ "TACT keys folder" ▸ Download.**

Steps 4 and 5 are one click each and are not optional — the game stores assets by number,
and those two downloads are what turn a number into a name and a locked record into a
readable one.

## What the two downloads are

**d4data** is a community metadata snapshot: what each asset is called, which material a
piece uses, which texture a material references. Budget **~4–6 GB** and 10–20 minutes.
Most of that is not the transfer — it writes around 460,000 small JSON files, so the
*Extracting* step is limited by your drive. Only the 24 asset groups the tool actually
reads are fetched, out of 133, and the folder is NTFS-compressed as it is written.

**TACT keys** are the community-harvested decryption keys. Seasonal and collaboration
content ships encrypted; without keys those records are present but unreadable. Around
11,500 assets stay locked behind keys nobody has harvested — that is a limit of what is
public, not of this tool.

Neither download reaches Blizzard, and neither ships game assets. d4data is community-decoded
metadata; TACT keys are community-harvested. The tool reads your own installation for
everything else. → [Asset formats §3](Asset-formats#3-directories-and-file-types) ·
[Glossary](Glossary#storage-and-identity)

## Requirements

- Windows 10/11 x64
- A Diablo IV installation (Battle.net or Steam)
- A GPU with **OpenGL 4.5** — on a laptop with switchable graphics, make sure Windows is
  handing the app the discrete GPU
- An internet connection on first run

## What to do next

Once both downloads finish, **[the six tabs](Tabs)** is the five-minute tour, and
**[Keyboard & mouse](Keyboard-and-mouse)** is worth keeping open beside the app for the first
session — several viewport actions are not written on any button.

If something does not look right on first run, **Help ▸ Health check** reports which of the
two folders is incomplete rather than leaving you to guess.
→ [Diagnostics](Diagnostics#help--health-check)

## "Windows flagged the download"

An unsigned executable from the internet gets flagged on reputation, not on content. The
zip is built by [the release workflow](https://github.com/trappuss/D4AssetBrowser/actions)
from the tagged commit, so what you download is what the public source at that tag
produces — you can read the build log for your exact release.

## Moving or resetting it

Move the whole folder; `data\` goes with it and nothing breaks. To start clean, delete
`data\` — you will be re-pointing at the game folder and re-downloading afterwards.

To reset only part of it, **Settings ▸ Restore Defaults** resets the viewport settings of
all three 3D tabs without touching your folders, your saved presets or your remembered
selections. **Clear Wardrobe memory** and **Clear Stable memory** in the same dialog forget
a remembered outfit or mount while leaving everything else alone.
