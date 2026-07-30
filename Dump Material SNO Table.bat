@echo off
setlocal enabledelayedexpansion
title D4 Asset Browser - Material SNO Table Sweep

REM ---------------------------------------------------------------------------
REM UNATTENDED. Launches the app, sweeps EVERY named appearance, writes the
REM report and closes itself. No clicking.
REM
REM WHY THIS EXISTS
REM   Encrypted appearances (Doom collab, seasonal sets) ship no
REM   json/base/meta/Appearance/<name>.app.json, and the whole material chain is
REM   keyed by NAME through that file:
REM       appearanceRoster(.app.json) -> material NAME
REM       -> Material/<name>.mat.json -> material SNO + texture names
REM       -> Texture/<name>.tex.json  -> width/height/format
REM   So they render white with sub-objects labelled "part 7", "part 8", ...
REM   The binary ALREADY gives the per-sub-object material ORDER
REM   (ModelParser.cpp:356). Only the sno LIST it indexes into is missing.
REM
REM WHAT IS ALREADY MEASURED (from ~32 hand-clicked appearances)
REM   - material sno sits at record offset +20
REM   - records are 72 bytes apart (measured twice, two independent ways)
REM   - descriptor reads:  ... 0 0 0 0 0 [dataOffset] [72] ...
REM
REM WHAT THIS SWEEP SETTLES
REM   Where the array ENDS, and whether those offsets actually reproduce the
REM   JSON across the WHOLE corpus rather than the names I happened to pick.
REM   Hand-picking is what let a broken stride test look like a "12 of 25"
REM   result earlier - unattended and exhaustive removes that bias.
REM
REM   Self-validating: if the walk cannot reproduce the JSON on named
REM   appearances, the offsets are wrong and no reader gets written on them.
REM
REM Writes next to the exe:
REM   matsno_sweep.txt   verdict + walk-pattern tally + length rule
REM   matsno_sweep.csv   one row per appearance
REM ---------------------------------------------------------------------------

cd /d "%~dp0"

set "EXE=%~dp0build\release\D4AssetBrowser.exe"
set "SRC=%~dp0src\index\MatSnoSweep.cpp"
set "TXT=%~dp0build\release\matsno_sweep.txt"
set "CSV=%~dp0build\release\matsno_sweep.csv"

if not exist "%EXE%" (
    echo.
    echo  [X] Not found: %EXE%   ^(build first^)
    echo.
    pause
    exit /b 1
)

REM Stale-binary guard. An exe older than the sweep does not contain it, and the
REM empty result is indistinguishable from "the sweep found nothing". This has
REM already cost two false diagnoses in this project.
for /f %%T in ('powershell -NoProfile -Command ^
    "if ((Get-Item '%EXE%').LastWriteTime -lt (Get-Item '%SRC%').LastWriteTime) {'STALE'} else {'OK'}"') do set "FRESH=%%T"
if "%FRESH%"=="STALE" (
    echo.
    echo  [X] STALE BINARY - the exe is older than src\index\MatSnoSweep.cpp, so
    echo      it does not contain the sweep. Rebuild, then run this again.
    echo.
    pause
    exit /b 1
)

REM Clear previous output so a failure to write cannot be read as fresh results.
if exist "%TXT%" del /q "%TXT%"
if exist "%CSV%" del /q "%CSV%"

echo.
echo  Sweeping every named appearance. This opens the app, runs the sweep with a
echo  progress dialog, and closes on its own - leave it alone until it exits.
echo.

set D4_MATSNO_SWEEP=1
"%EXE%"

echo.
if not exist "%TXT%" (
    echo  [X] No report was written.
    echo      The sweep needs a configured GAME INSTALL ^(it reads meta blobs from
    echo      CASC^) and a d4data folder ^(it needs the JSON as ground truth^).
    echo      Check the log for "matsno sweep:".
    echo.
    pause
    exit /b 1
)

type "%TXT%"
echo.
echo  ------------------------------------------------------------------
echo   Full per-appearance detail: %CSV%
echo  ------------------------------------------------------------------
echo.
pause
