@echo off
setlocal enabledelayedexpansion
title D4 Asset Browser - Material Value Pair Dump

REM ---------------------------------------------------------------------------
REM UNATTENDED. Builds, launches the app, writes paired material blobs and
REM closes itself. No clicking.
REM
REM WHY THIS EXISTS
REM   matTexFromMeta already reads a material's TEXTURE list out of the meta
REM   binary. Its VALUES - ptRunTimeMaterialValues, the authored scalars and
REM   vectors - are still read only from .mat.json, so for every ENCRYPTED
REM   material the tool substitutes defaults and says nothing:
REM
REM       metal 0.0 . rough 0.6 . emissive multiplier 1.0 . emissive color absent
REM
REM   That is the whole DOOM set rendering on stand-in PBR.
REM
REM WHAT THIS PRODUCES
REM   build\release\matvalue\<sno>_<name>.meta.bin   the raw blob
REM   build\release\matvalue\<sno>_<name>.mat.json   the same material's JSON
REM
REM   Pairs, because the layout can only be DERIVED against ground truth: the
REM   JSON states the authored values, the blob is where they have to be found.
REM   Named materials only, self-selected from the index - no hand-picked list,
REM   which is how a biased sample once made a broken stride test look like a
REM   "12 of 25" result.
REM
REM   Nothing is written into the tool on the strength of this dump. It is the
REM   measurement; the reader comes after, and only if the offsets reproduce
REM   the JSON across the sample.
REM ---------------------------------------------------------------------------

cd /d "%~dp0"

set "EXE=%~dp0build\release\D4AssetBrowser.exe"
set "OUT=%~dp0build\release\matvalue"
set "OUT2=%~dp0build\release\clothvalue"

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

set "PYEXE="
py -3 -c "import sys" >nul 2>&1 && set "PYEXE=py -3"
if not defined PYEXE python -c "import sys" >nul 2>&1 && set "PYEXE=python"
if defined PYEXE %PYEXE% "%~dp0verify-src.py" --quiet

echo  Building...
echo.
REM Do NOT pipe cmake through findstr - after a pipe, %errorlevel% is the LAST
REM command's, so a failed build would report success. Tee-Object is a cmdlet,
REM so cmake.exe stays the last native process and its exit code survives.
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --build --preset release 2>&1 | Tee-Object -FilePath '%~dp0build_log.txt'; exit $LASTEXITCODE"
if errorlevel 1 (
    echo.
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$e = Select-String -Path '%~dp0build_log.txt' -Pattern 'error C|error LNK|fatal error|: error' | ForEach-Object { $_.Line } | Select-Object -First 40 ; $e; $e | Out-File -FilePath '%~dp0build_errors.txt' -Encoding utf8"
    echo  [X] BUILD FAILED - errors above, and in build_errors.txt.
    pause & exit /b 1
)
if not exist "%EXE%" (
    echo.
    echo  [X] Build reported success but the exe is missing.
    pause & exit /b 1
)
echo.
echo  Build OK.
echo.

REM Clear previous output so a failure to write cannot be read as fresh results.
if exist "%OUT%" rd /s /q "%OUT%"
if exist "%OUT2%" rd /s /q "%OUT2%"

echo  Dumping paired material blobs. The app opens, writes them and closes on
echo  its own - leave it alone until it exits.
echo.

REM The dump modes live inside runMatSnoSweep, which only runs when this is set.
set D4_MATSNO_SWEEP=1
REM How many pairs. 240 is enough for a stride and a length rule to be visible
REM without being explained by luck, and small enough to read in one sitting.
set D4_MATVALUE_DUMP=240
REM The cloth half of the identical gap: 55 named cloth pieces run untuned defaults, all of
REM them in encrypted sets. Same pairing, same run - one sitting covers both derivations.
set D4_CLOTHVALUE_DUMP=240

"%EXE%"

echo.
if not exist "%OUT%" (
    echo  [X] Nothing was written.
    echo      This needs a configured GAME INSTALL ^(the blobs come from CASC^)
    echo      AND a d4data folder ^(the JSON is the ground truth^).
    echo      Check the log for "matvalue:".
    echo.
    pause
    exit /b 1
)

set /a NBIN=0
for %%F in ("%OUT%\*.meta.bin") do set /a NBIN+=1
echo  Wrote !NBIN! material pair^(s^) to:
echo    %OUT%
set /a NCLT=0
for %%F in ("%OUT2%\*.meta.bin") do set /a NCLT+=1
echo  Wrote !NCLT! cloth pair^(s^) to:
echo    %OUT2%
echo.
echo  ------------------------------------------------------------------
echo   Next: the layout is derived from these OFFLINE. Nothing reads the
echo   value table yet - that reader is written only once these offsets
echo   reproduce the JSON across the whole sample.
echo  ------------------------------------------------------------------
echo.
pause
