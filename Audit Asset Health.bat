@echo off
setlocal enabledelayedexpansion
title D4 Asset Browser - Asset Health Audit

REM ---------------------------------------------------------------------------
REM CORPUS-WIDE. Walks EVERY appearance and reports what the tool can and cannot
REM show, then diffs against the previous run. Builds first, exits on its own.
REM
REM WHY THIS EXISTS
REM   Finding the Doom set was the symptom, not the goal. The goal is knowing
REM   what ELSE is missing, and catching breakage the next time the game patches
REM   or a new TACT key drops - without waiting for someone to notice a white
REM   model months later.
REM
REM WHAT EACH STATUS MEANS
REM   OK               geometry + materials + texture definitions all resolve
REM   NO-TEXTURE-DEFS  materials resolve, their textures have no definition
REM   NO-MATERIALS     geometry loads, material list came back empty
REM   NO-GEOMETRY      meta+payload present, parse produced nothing
REM   LOCKED           encrypted, no TACT key held - NOT a defect, just gated
REM   NO-DATA          nothing in CASC at all
REM
REM   LOCKED is separated deliberately: lumping it in with real breakage would
REM   bury a handful of genuine faults under thousands of gated rows.
REM
REM THE DIFF IS THE POINT
REM   Each run saves a baseline. The next run reports "newly working" and
REM   "newly BROKEN" against it. After a patch: run it, read two numbers.
REM   After adding TACT keys: newly working tells you what the keys bought.
REM
REM Writes next to the exe:
REM   asset_health.txt           summary, ranked, with the change since last run
REM   asset_health.csv           one row per appearance
REM   asset_health_baseline.csv  this run, for the next run to diff against
REM ---------------------------------------------------------------------------

cd /d "%~dp0"
set "EXE=%~dp0build\release\D4AssetBrowser.exe"
set "TXT=%~dp0build\release\asset_health.txt"

taskkill /im D4AssetBrowser.exe /f >nul 2>&1

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

echo  Building...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --build --preset release 2>&1 | Tee-Object -FilePath '%~dp0build_log.txt'; exit $LASTEXITCODE"
if errorlevel 1 (
    echo.
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Select-String -Path '%~dp0build_log.txt' -Pattern 'error C|error LNK|fatal error|: error' | ForEach-Object { $_.Line } | Select-Object -First 40"
    echo  [X] BUILD FAILED - errors above.
    pause & exit /b 1
)
echo  Build OK.
echo.

if exist "%TXT%" del /q "%TXT%"

echo  Auditing every appearance. A progress dialog will appear; this parses each
echo  model, so give it a few minutes. It closes on its own.
echo.

set D4_HEALTH_AUDIT=1
"%EXE%"

echo.
if exist "%TXT%" (
    type "%TXT%"
) else (
    echo  [X] No report written. Needs a configured game install; check the log
    echo      for "health:".
)
echo.
pause
