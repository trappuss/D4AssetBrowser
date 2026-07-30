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

REM ── BUILD ────────────────────────────────────────────────────────────────────
REM Built inline rather than by calling rebuild.bat, for two reasons: rebuild.bat
REM finishes with run.bat, which launches DETACHED via `start ""` (so this script
REM could not wait for the report), and run.bat also forces D4_DUMP_CLOTH=1, which
REM floods the log. Same vcvars detection and same cmake preset as rebuild.bat.
REM
REM This also removes the stale-binary class of failure entirely: the exe cannot be
REM older than the source if it is built here every run. That mistake produced two
REM false diagnoses earlier in this project.

taskkill /im D4AssetBrowser.exe /f >nul 2>&1

where cl >nul 2>&1
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    set "VSPATH="
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    if not defined VSPATH (
        echo  [X] Visual Studio 2022 C++ tools not found.
        pause & exit /b 1
    )
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
)

if not exist "build\release\CMakeCache.txt" (
    echo  [X] No build directory yet - run build.bat once first.
    pause & exit /b 1
)

REM Pre-build source checks, same as rebuild.bat (non-blocking).
set "PYEXE="
py -3 -c "import sys" >nul 2>&1 && set "PYEXE=py -3"
if not defined PYEXE python -c "import sys" >nul 2>&1 && set "PYEXE=python"
if defined PYEXE %PYEXE% "%~dp0verify-src.py" --quiet

echo  Building...
echo.
REM NOTE: do NOT pipe cmake through findstr - in cmd, %errorlevel% after a pipe is the
REM LAST command's, so a failed build would report success and this script would go on
REM to read a stale report. rebuild.bat documents the same trap; Tee-Object is a cmdlet,
REM so cmake.exe stays the last native process and its exit code survives.
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --build --preset release 2>&1 | Tee-Object -FilePath '%~dp0build_log.txt'; exit $LASTEXITCODE"
if errorlevel 1 (
    echo.
    findstr /i /c:"error C" /c:": error" /c:"error LNK" /c:"fatal error" "%~dp0build_log.txt" > "%~dp0build_errors.txt"
    echo  [X] BUILD FAILED - errors:
    echo.
    type "%~dp0build_errors.txt"
    pause & exit /b 1
)
if not exist "%EXE%" (
    echo.
    echo  [X] Build reported success but %EXE% is missing.
    pause & exit /b 1
)
echo.
echo  Build OK.
echo.

REM Clear previous output so a failure to write cannot be read as fresh results.
if exist "%TXT%" del /q "%TXT%"
if exist "%CSV%" del /q "%CSV%"

echo.
echo  Sweeping every named appearance. This opens the app, runs the sweep with a
echo  progress dialog, and closes on its own - leave it alone until it exits.
echo.

set D4_MATSNO_SWEEP=1
REM Raw meta blobs for offline layout derivation. Two encrypted (ground truth from the
REM d4analyzer GLB extraction in .Testing) + named ones covering every case the sweep
REM flagged: the 144-byte-stride sample, 5-material, cloth+material, fx-heavy, 1-material.
set D4_METADUMP_NAMES=necF_stor245_TRS,necF_stor245_LEG,BarM_stor258_TRS,barF_stor274_TRS,barM_stor256_TRS,druM_stor177_HLM,rogF_stor231_GLV,warM_stor175_LEG,palF_stor171_TRS,palM_stor171_TRS,barF_base02_GLV,necF_base01_TRS
REM Material + Texture blobs for the encrypted stor245 set, to derive the texture chain.
REM 2335360/2335358 = TRS mat + fur mat, 2335347 = LEG mat, 2520946/2520944/2520945 = cloth,
REM 399607 = armor_skin_mat (NAMED - a control whose JSON we can check the derivation against).
set D4_METADUMP_SNOS=2335360,2335358,2335347,2520946,2520944,2520945,399607

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
