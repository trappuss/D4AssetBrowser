@echo off
setlocal enabledelayedexpansion
title D4 Asset Browser - Body marking model sweep

REM ---------------------------------------------------------------------------
REM  ONE STEP: builds, runs, writes everything, prints the summary. Nothing to
REM  close, no judgement about when it is done, and NO second run needed.
REM
REM  WHY
REM    A body marking renders with the wrong colour and without the slight
REM    transparency the game has. Diagnosing that one field at a time has cost a
REM    rebuild per question, and twice the number that was measured could not
REM    have answered the question it was measured for. This dumps every fact
REM    about the marking model at once, including PICTURES - because "is the
REM    output dark or bright" is settled by looking at it, not by arithmetic.
REM
REM  WHAT YOU GET
REM    data\marking_sweep.txt
REM      PART A - all 93 MarkingColors: the three samples in linear AND sRGB,
REM               each one's luminance, the ramp DIRECTION, plus authored
REM               roughness / metalness / isTattoo. Answers whether "sample 0 is
REM               the shadow" is a real rule or an accident of one colour.
REM      PART B - all 300+ MarkingShapes: mask size, ink coverage, and the green
REM               channel measured OVER THE INK ONLY (a whole-sheet average is
REM               worthless here - the design is a few percent of the sheet, so
REM               the empty background drags every mean to zero). Ends with the
REM               composited ink colour the model actually produces.
REM
REM    data\marking_swatch\*.png   for three representative markings:
REM      _maskR_coverage.png        the R channel - where the design is
REM      _maskG_rampPos.png         the G channel - where it sits on the ramp
REM      _ramp.png                  the ramp itself, G=0 left to G=255 right
REM      _composite_on_flat_skin.png  the result over flat skin
REM
REM  Needs a configured game folder. Reads only; writes only the report + PNGs.
REM  The mask decode is the slow part - expect a few minutes with a progress bar.
REM ---------------------------------------------------------------------------

cd /d "%~dp0"
set "EXE=%~dp0build\release\D4AssetBrowser.exe"
set "TXT=%~dp0build\release\data\marking_sweep.txt"
set "IMG=%~dp0build\release\data\marking_swatch"

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
if exist "%TXT%" del /q "%TXT%"
if exist "%IMG%" rmdir /s /q "%IMG%"
echo.
echo  [2/3] Sweeping. UNATTENDED - do not close the window.
echo.
echo        It decodes every marking mask, so a progress bar will sit there for
echo        a few minutes. It writes the report and QUITS on its own.
echo.

set D4_MARKING_SWEEP=1
start "" /wait "%EXE%"
set "D4_MARKING_SWEEP="

REM --- 3/3 report -----------------------------------------------------------
echo.
echo  [3/3] Report
echo.
if exist "%TXT%" (
    powershell -NoProfile -Command "Get-Content '%TXT%' | Select-Object -First 40"
    echo.
    echo   ... truncated. Full report:
    echo   %TXT%
    if exist "%IMG%" (
        echo.
        echo   Pictures:
        dir /b "%IMG%"
        echo   in %IMG%
    )
    echo.
    echo   Send BOTH the .txt and the .png files.
) else (
    echo  [X] No report written. Check build\release\data\D4AssetBrowser.log for
    echo      "marking sweep:" and confirm the game folder is set in File - Settings.
    echo.
    echo      If a marking_sweep.txt exists NEXT TO the exe instead of in data\,
    echo      you are running an older build - rebuild and re-run this script.
)
echo.
pause
