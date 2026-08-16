@echo off
setlocal enabledelayedexpansion
title D4 Asset Browser - StoreProduct .prd binary probe

REM ---------------------------------------------------------------------------
REM  ONE STEP: builds, runs, writes the report, prints it. No separate rebuild,
REM  nothing to close, no judgement call about when it is finished.
REM
REM  WHAT WE ARE TRYING TO DO
REM    The Catalogue tab only knows about shop bundles that d4data describes in
REM    json/base/meta/StoreProduct/*.prd.json - 7,553 of them. The GAME has
REM    9,308. The ~1,800 in the gap are the encrypted and newly-patched ones,
REM    which is exactly the content people go looking for: the Doom collab
REM    armour is missing from the tab for all classes except Druid and Rogue,
REM    because only those two shipped a .prd.json.
REM
REM    Those products ARE in the game files, just in Blizzard's binary format
REM    rather than as readable JSON. To read them we need to know where each
REM    piece of information sits inside that binary - which byte holds the list
REM    of items in a bundle, which holds the thing each item points at, and so
REM    on. Nobody documents that, so we measure it.
REM
REM  HOW IT MEASURES INSTEAD OF GUESSING
REM    For the 7,553 products that DO have JSON we already know every answer.
REM    So the probe hunts those known values inside the binary and reports where
REM    it found them. A value that turns up at the same offset in every single
REM    product is a field. Anything less is a coincidence and is reported as
REM    such, not turned into a parser.
REM
REM  WHAT THE REPORT CONTAINS (all of it, one run)
REM    1. child-array offsets + stride, for 40 sample bundles
REM    2. VERIFICATION - parses all ~1,900 bundles from the binary alone and
REM       compares against their JSON. Ends in a plain verdict line.
REM    3. payload SNO offset per product kind, with a formula check
REM    4. art-handle offset histogram
REM    5. a raw hexdump of the header, so any question not yet asked can be
REM       answered from this same file rather than another run
REM
REM  Needs a configured game folder. Reads only; writes just the report.
REM ---------------------------------------------------------------------------

cd /d "%~dp0"
set "EXE=%~dp0build\release\D4AssetBrowser.exe"
set "OUT=%~dp0build\release\data\prd_probe.txt"

taskkill /im D4AssetBrowser.exe /f >nul 2>&1

REM --- 1/3 build ------------------------------------------------------------
where cl >nul 2>&1
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    set "VSPATH="
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    if not defined VSPATH ( echo  [X] VS 2022 C++ tools not found. & pause & exit /b 1 )
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
)
if not exist "build\release\CMakeCache.txt" (
    echo  [X] No build directory - run build.bat once first.
    pause & exit /b 1
)

set "PYEXE="
py -3 -c "import sys" >nul 2>&1 && set "PYEXE=py -3"
if not defined PYEXE python -c "import sys" >nul 2>&1 && set "PYEXE=python"
if defined PYEXE %PYEXE% "%~dp0verify-src.py" --quiet

echo.
echo  [1/3] Building...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --build --preset release 2>&1 | Tee-Object -FilePath '%~dp0build_log.txt'; exit $LASTEXITCODE"
if errorlevel 1 (
    echo.
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Select-String -Path '%~dp0build_log.txt' -Pattern 'error C|error LNK|fatal error|: error' | ForEach-Object { $_.Line } | Select-Object -First 40"
    echo.
    echo  [X] BUILD FAILED - errors above.
    pause & exit /b 1
)
echo  Build OK.

REM --- 2/3 run --------------------------------------------------------------
if exist "%OUT%" del /q "%OUT%"
echo.
echo  [2/3] Probing. UNATTENDED - do not close the window.
echo.
echo        It opens CASC, builds the store-product index if it is cold (a few
echo        minutes on a first run - it crawls ~7,500 files), writes the report
echo        and QUITS on its own. This script continues when it does.
echo.

set D4_DUMP_PRD=1
start "" /wait "%EXE%"
set "D4_DUMP_PRD="

REM --- 3/3 report -----------------------------------------------------------
echo.
echo  [3/3] Report
echo.
if exist "%OUT%" (
    type "%OUT%"
    echo.
    echo  ------------------------------------------------------------
    echo   Full report: %OUT%
    echo   It is long - the hexdump is at the end. Send the whole file.
    echo  ------------------------------------------------------------
) else (
    echo  [X] No report written. The probe runs only after CASC opens - check
    echo      build\release\data\D4AssetBrowser.log for "prd-probe:" and confirm
    echo      the game folder is set in File - Settings.
)
echo.
pause
