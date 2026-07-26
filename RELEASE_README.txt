Diablo IV Asset Browser  —  v2.1.0
====================================

A fast, native viewer/exporter for Diablo IV assets: models, textures, wardrobe
looks, animations, and more.


PORTABLE — NOTHING TO INSTALL
-----------------------------
There is no installer and no runtime to set up. Just unzip the folder and run
D4AssetBrowser.exe. Its Qt runtime (the Qt6*.dll files and the platforms\ /
imageformats\ plugin folders) ships right beside the exe, so it runs on any
Windows PC with nothing else installed.

Everything the tool writes lives in a "data" folder created next to the exe:

    D4AssetBrowser.exe
    Qt6*.dll, platforms\, imageformats\, ...   (the bundled Qt runtime — leave these)
    data\
        settings.ini ............ your settings (no Windows registry is touched)
        d4data\ ................. the game metadata (downloaded on first run)
        index_cache\ ............ one-time scan results (rebuilt after a game/d4data update)
        model_thumbs\ ........... cached 3D thumbnails
        ...

Move or copy the whole folder anywhere — another drive, a USB stick, another PC —
and it runs exactly the same and remembers its state. Delete the folder and it
leaves zero traces on the machine.


FIRST RUN
---------
On the first launch it asks you to set two things (File > Settings):

  1. Diablo IV folder
     Point it at your game install (the folder with "Diablo IV.exe"). This lets
     the tool read live CASC textures/models directly from the game.

  2. d4data
     The extracted game metadata the browser is built around. Click "Download"
     to fetch it automatically into data\d4data — this needs "git" available on
     your PATH (https://git-scm.com). Or click "Browse" and point at an existing
     d4data copy you already have.

You can use the tool with just d4data (browsing/exporting extracted assets), or
add the game folder for live CASC data and full previews.


UPDATES
-------
After a Diablo IV game patch or a fresh d4data pull, the tool notices the change
(via the game build id + d4data's buildVersion) and rebuilds its indexes and
caches automatically. No manual cache clearing needed.


REQUIREMENTS
------------
  * Windows 10/11, 64-bit
  * A GPU/driver supporting OpenGL 4.5 (any reasonably modern card)
  * "git" on PATH only if you want the built-in d4data auto-download
  * Microsoft Visual C++ 2015-2022 x64 redistributable. Virtually every gaming PC
    already has it (Diablo IV and most games install it); if the app won't start
    with a "VCRUNTIME140.dll missing" error, grab it free from Microsoft:
    https://aka.ms/vs/17/release/vc_redist.x64.exe


TROUBLESHOOTING
---------------
  * Blank previews / "Auto-Load is off": open a model and press Reload, or enable
    Auto-Load (top of the Models tab).
  * A model shows a warning triangle: it can't be displayed (a prop, attachment,
    or effect mesh with no geometry). Toggle "Hide un-renderable" to clean up the grid.
  * A diagnostic log is written to D4AssetBrowser.log next to the exe.
  * To reset everything, close the app and delete the data\ folder (you'll be
    re-prompted for setup and it will re-download/rebuild).

This tool reads game data you already own; it ships no Blizzard assets.
