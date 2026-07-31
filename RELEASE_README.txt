Diablo4AssetBrowser Native
==========================

A Diablo IV asset browser and 3D wardrobe / mount studio. Reads your installed
game directly, decodes textures, and previews or exports appearances, armour,
weapons, mounts and pets as animated .glb.

Native rewrite (C++17 / Qt6 / OpenGL) of trappuss/Diablo4AssetBrowser.
No Python, no external extractor — this folder is everything.


GETTING STARTED
---------------
1. Run D4AssetBrowser.exe  (unzip the whole folder first — the DLLs next to the
   exe are required).
2. File > Settings ......... set your Diablo IV game folder.
3. File > Dependencies .... download d4data (community metadata). One click.
4. File > Update TACT Keys  fetch the community decryption keys.

First launch builds indexes and can take a minute. Later launches are fast —
the caches live in data\ next to the exe.


PORTABLE
--------
Everything the tool writes goes in data\ beside the exe: settings, caches,
thumbnails, logs. Nothing touches the registry or your user profile. Delete the
folder and it is gone.


REQUIREMENTS
------------
Windows 10/11 x64 · a Diablo IV installation · GPU with OpenGL 4.5 ·
internet on first run.


TROUBLESHOOTING
---------------
"no Qt platform plugin could be initialized"
    The folder was not unzipped intact. platforms\qwindows.dll must sit beside
    the exe.

Nothing loads
    File > Settings — is the game folder correct? It should contain Data\.

A new collab or seasonal item is missing
    File > Update TACT Keys. Some encrypted content needs a key nobody has
    harvested yet; those stay locked until someone does.

Models look grey / untextured
    Usually a missing TACT key for that item. The log names the key.

Logs
    data\D4AssetBrowser.log — worth attaching to any bug report.


NOT AFFILIATED WITH BLIZZARD
----------------------------
For personal use with a copy of Diablo IV you own. No game assets and no
decryption keys ship in this download. Diablo IV and all game assets are the
property of Blizzard Entertainment.

MIT licensed. Source: see the GitHub repository.
