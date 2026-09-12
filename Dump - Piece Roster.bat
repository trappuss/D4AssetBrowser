@echo off
REM Dump the material roster of every equipped Wardrobe piece -> build\release\piece_roster.txt
REM
REM Answers "which material name does this appearance actually resolve to, and does it exist":
REM   * whether the appearance ships a .app.json at all
REM   * the roster as BOTH routes read it - the .app.json and the CASC meta binary - side by side,
REM     flagged when they disagree (they have before, silently)
REM   * per roster entry, whether that material name has a .mat.json on disk
REM   * per primitive: material, materialIndex, tris, the FX/SIM/FORM/HED/COVERED/CHAR flags, and
REM     the visibility those produce
REM
REM Written for "PalF_sets50_LEG has invisible/missing parts". That set authors NO material and NO
REM texture of its own in CoreTOC and borrows the MALE set's cloth definitions, so the roster is the
REM only place the answer can be. See docs\PALADIN_MATERIAL_SHARING.md.
REM
REM The file is REWRITTEN on every outfit rebuild, so the LAST outfit you equipped is what it holds.
REM Equip one piece, close the app, read the file; repeat for the piece you are comparing against.
setlocal
cd /d "%~dp0"

set "EXE=build\release\D4AssetBrowser.exe"
if not exist "%EXE%" (
    echo D4AssetBrowser.exe not found - build it first:  rebuild.bat
    pause & exit /b 1
)

del /q "build\release\piece_roster.txt" >nul 2>&1
set D4_DUMP_PIECEROSTER=1

echo Launching. In the app: Wardrobe ^> Paladin ^> pick a gender ^> equip the piece to inspect.
echo The dump is rewritten on every rebuild, so close the app once the LAST outfit you want is on.
echo.
REM NOT "start": run it attached so a fault that kills the process before the file is written still
REM prints here. A detached launch is exactly why a startup crash once looked like an empty dump.
"%EXE%"
echo.
echo Exit code: %ERRORLEVEL%   (0 = clean, anything else = fault)
echo.
if exist "build\release\piece_roster.txt" (
    echo Wrote build\release\piece_roster.txt
    echo Look for: an EMPTY material name, ".mat.json=NO", or "THE TWO ROUTES DISAGREE".
) else (
    echo piece_roster.txt was NOT written - the app never completed an outfit rebuild.
)
pause
endlocal
