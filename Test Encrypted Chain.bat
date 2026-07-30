@echo off
setlocal enabledelayedexpansion
title D4 Asset Browser - Encrypted Chain Test

REM ---------------------------------------------------------------------------
REM Builds, then verifies the encrypted-content chain end to end, then exits.
REM Seconds, not minutes.
REM
REM WHAT IT CHECKS
REM   appearance meta -> material snos -> texture snos -> dimensions -> pixels
REM   through the PRODUCTION path (appearanceRosterFromMeta + MaterialDecode),
REM   so a regression anywhere in it shows up here rather than in the viewport.
REM
REM   Samples: 4 encrypted pieces (necF_stor245_TRS/LEG, spiM_stor190_TRS,
REM   palF_stor171_TRS) and 2 NAMED controls (necF_base01_TRS, barF_base02_GLV).
REM   The controls matter: if those fail too, the fault is general rather than
REM   encryption-specific, and that is a different investigation.
REM
REM WHY THIS REPLACED "Dump Material SNO Table.bat"
REM   That bat existed to DERIVE the material array layout. The layout is now
REM   settled and verified, so re-deriving it across 63,000 appearances every run
REM   answers a question already answered - and it dumped a 30 MB TVFS table each
REM   time that was only ever needed once. The old bat still works if a format
REM   ever needs re-deriving; this one is for day-to-day "is it still working".
REM ---------------------------------------------------------------------------

cd /d "%~dp0"
set "EXE=%~dp0build\release\D4AssetBrowser.exe"
set "LOG=%~dp0build\release\D4AssetBrowser.log"

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
REM Not piped through findstr: after a pipe, %errorlevel% is the LAST command's, so
REM a failed build would report success. Tee-Object is a cmdlet, so cmake's code survives.
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --build --preset release 2>&1 | Tee-Object -FilePath '%~dp0build_log.txt'; exit $LASTEXITCODE"
if errorlevel 1 (
    echo.
    REM Tee-Object writes UTF-16 which findstr cannot read - it produces an EMPTY file.
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Select-String -Path '%~dp0build_log.txt' -Pattern 'error C|error LNK|fatal error|: error' | ForEach-Object { $_.Line } | Select-Object -First 40"
    echo  [X] BUILD FAILED - errors above.
    pause & exit /b 1
)
echo  Build OK.
echo.
echo  Running chain test...
echo.

set D4_CHAINTEST=1
"%EXE%"

echo.
echo  ==================== RESULT ====================
powershell -NoProfile -ExecutionPolicy Bypass -Command "$m=Select-String -Path '%LOG%' -Pattern 'chaintest:' | Select-Object -Last 1; if($m){ $i=$m.LineNumber; Get-Content '%LOG%' | Select-Object -Skip ($i-1) -First 8 } else { 'No chaintest output - check that a game install and d4data folder are configured.' }"
echo.
echo  ------------------------------------------------
echo   PASS = that piece produced a decoded baseColor image.
echo   A named control failing means the fault is NOT encryption-specific.
echo  ------------------------------------------------
echo.
pause
