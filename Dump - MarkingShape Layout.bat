@echo off
setlocal enabledelayedexpansion
title D4 Asset Browser - MarkingShape .msh binary probe

REM ---------------------------------------------------------------------------
REM  ONE STEP: builds, runs, writes the report, prints it. No separate rebuild,
REM  nothing to close, no judgement call about when it is finished.
REM
REM  WHAT WE ARE TRYING TO DO
REM    The Wardrobe's Marking list is built ENTIRELY from d4data's
REM    json/base/meta/MarkingShape/*.msh.json. When the game ships a marking the
REM    community snapshot has not described yet, the Wardrobe cannot show it at
REM    all - not greyed out, not named, simply absent - and nothing on screen
REM    tells you the difference between that and "this class has no such
REM    marking".
REM
REM    That is not hypothetical. The Diablo IV x Berserk collab's Brand of
REM    Sacrifice markings are in the game (their textures extract fine from the
REM    Textures tab) and have no .msh.json at all. They sit exactly in the gaps
REM    of each class's numbering - bar 057,[058],059 - dru 045,[046],047 -
REM    nec 051,[052-053],054 - sor 051,[052-055],056 - spi 026,[027-028],029 -
REM    rog 058,[059-060],061 - which is what a lagging snapshot looks like from
REM    underneath.
REM
REM    To read those markings from the game instead of from the snapshot we need
REM    to know where each field sits inside Blizzard's binary record. Nobody
REM    documents that, so we measure it.
REM
REM  HOW IT MEASURES INSTEAD OF GUESSING
REM    For every marking that DOES have JSON we already know every answer -
REM    the two mask textures, the default colour, the swatch icon, the class
REM    restriction. So the probe hunts those known values inside each record's
REM    own binary and reports where it found them. A value that turns up at the
REM    same offset in EVERY record is a field. Anything less is a coincidence,
REM    and is reported as one rather than turned into a parser.
REM
REM  WHAT YOU GET EVEN IF THE LAYOUT IS INCONCLUSIVE
REM    The last section needs no layout at all: it lists, BY NAME, every marking
REM    the game has and the snapshot does not. That is the answer to "what am I
REM    missing" - paste those names into the Textures tab and the masks are
REM    there to extract by hand today.
REM ---------------------------------------------------------------------------

cd /d "%~dp0"
set "EXE=%~dp0build\release\D4AssetBrowser.exe"
set "OUT=%~dp0build\release\data\msh_probe.txt"

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
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Select-String -Path '%~dp0build_log.txt' -Pattern 'error C^|error LNK^|fatal error^|: error' ^| ForEach-Object { $_.Line } ^| Select-Object -First 40"
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
echo        It opens CASC, reads every MarkingShape record, writes the report
echo        and QUITS on its own. This script continues when it does. Unlike the
echo        StoreProduct probe it needs no background index, so it is quick.
echo.

set D4_DUMP_MSH=1
start "" /wait "%EXE%"
set "D4_DUMP_MSH="

REM --- 3/3 report -----------------------------------------------------------
echo.
echo  [3/3] Report
echo.
if exist "%OUT%" (
    type "%OUT%"
    echo.
    echo  ------------------------------------------------------------
    echo   Full report: %OUT%
    echo   Two things matter in it:
    echo     * "UNANIMOUS" lines - those offsets are the binary's fields
    echo     * the "snapshot coverage" list at the end - markings the game
    echo       has that the Wardrobe cannot show. Send the whole file.
    echo  ------------------------------------------------------------
) else (
    echo  [X] No report written. The probe runs only after CASC opens - check
    echo      build\release\data\D4AssetBrowser.log for "msh-probe:" and confirm
    echo      the game folder is set in File - Settings.
)
echo.
pause
