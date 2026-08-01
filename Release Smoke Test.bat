@echo off
setlocal enabledelayedexpansion
title D4AssetBrowser - Release Smoke Test
cd /d "%~dp0"

REM ---------------------------------------------------------------------------
REM  Builds the release, then tests it the way a DOWNLOADER gets it: unzipped
REM  into a clean folder OUTSIDE this repo, with nothing of yours nearby.
REM
REM  WHY NOT JUST RUN build\release\D4AssetBrowser.exe
REM    That exe sits next to a data\ folder that is already populated, next to
REM    a vcpkg_installed tree full of Qt DLLs, and next to your d4data. It runs
REM    for reasons the zip does not inherit. Every "works here, dead on their
REM    machine" bug - a missing plugin DLL, a path that resolved relative to the
REM    source tree, a cache that was assumed to exist - hides in exactly that
REM    gap. The zip is the artifact; the zip is what gets tested.
REM
REM  WHAT IT ASSERTS
REM    1. the zip extracts to a real folder TREE (not files literally named
REM       "D4AssetBrowser\platforms\qwindows.dll" - see package-release.bat)
REM    2. exe + Qt6Core.dll + platforms\qwindows.dll are present
REM    3. the app STARTS from that clean folder and stays up
REM    4. it writes data\ beside the exe, with an .ini in it
REM    5. it leaves NOTHING in %APPDATA% and NOTHING in the registry
REM
REM  3-5 need the app to actually launch, so this stops and hands it to you:
REM  let it finish loading, then close it. The checks run on the way back.
REM ---------------------------------------------------------------------------

set "SANDBOX=%TEMP%\D4ABSmoke"
set "FAIL=0"

echo.
echo  ============================================================
echo   STEP 1/4 - build + package
echo  ============================================================
echo.
call "%~dp0package-release.bat"
if errorlevel 1 (
    echo.
    echo  [X] package-release.bat failed - nothing to test.
    pause & exit /b 1
)

REM package-release.bat version-stamps the zip from main.cpp, so read it the same way
REM rather than hardcoding - a version bump must not silently test last release's zip.
set "APPVER="
for /f tokens^=2^ delims^=^" %%v in ('findstr /c:"setApplicationVersion" "%~dp0src\main.cpp"') do set "APPVER=%%v"
if "%APPVER%"=="" set "APPVER=dev"
set "ZIP=%~dp0dist\D4AssetBrowser_v%APPVER%.zip"
if not exist "%ZIP%" (
    echo  [X] Expected zip not found: %ZIP%
    pause & exit /b 1
)

echo.
echo  ============================================================
echo   STEP 2/4 - unzip into a clean folder outside the repo
echo  ============================================================
echo.
taskkill /im D4AssetBrowser.exe /f >nul 2>&1
if exist "%SANDBOX%" rmdir /s /q "%SANDBOX%"
mkdir "%SANDBOX%" 2>nul
powershell -NoProfile -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; [System.IO.Compression.ZipFile]::ExtractToDirectory('%ZIP%', '%SANDBOX%')"
if errorlevel 1 ( echo  [X] Extract failed. & pause & exit /b 1 )

set "APP=%SANDBOX%\D4AssetBrowser"
if not exist "%APP%\D4AssetBrowser.exe" (
    echo  [X] %APP%\D4AssetBrowser.exe missing after extract.
    echo      If you see files with backslashes in their NAMES, the zip was
    echo      written with backslash separators - package-release.bat documents
    echo      that trap.
    dir /b "%SANDBOX%"
    pause & exit /b 1
)
echo  [OK] tree extracted: %APP%
for %%F in ("D4AssetBrowser.exe" "Qt6Core.dll" "platforms\qwindows.dll") do (
    if exist "%APP%\%%~F" ( echo  [OK] %%~F ) else ( echo  [X]  %%~F MISSING & set "FAIL=1" )
)
if exist "%APP%\data" (
    echo  [X]  data\ already exists in a FRESH unzip - build artefacts leaked into the package.
    set "FAIL=1"
) else (
    echo  [OK] no data\ yet ^(correct for a fresh unzip^)
)

REM Baseline the two places a NON-portable app would write, so "clean afterwards"
REM is measured rather than assumed.
echo.
echo  ============================================================
echo   STEP 3/4 - launch it. LET IT LOAD, THEN CLOSE IT.
echo  ============================================================
echo.
echo   It is unconfigured, so expect the first-run prompts. You do not need to
echo   download d4data - just get to the main window and close it.
echo.
set "APPDATA_BEFORE=0"
if exist "%APPDATA%\D4AssetBrowser"      set "APPDATA_BEFORE=1"
if exist "%APPDATA%\Diablo4AssetBrowser" set "APPDATA_BEFORE=1"
if "%APPDATA_BEFORE%"=="1" (
    echo   NOTE: %%APPDATA%% already has a D4AssetBrowser folder from an earlier
    echo         non-portable build. The registry check below is still meaningful;
    echo         the AppData one will be inconclusive.
    echo.
)
pause

start "" /wait "%APP%\D4AssetBrowser.exe"

echo.
echo  ============================================================
echo   STEP 4/4 - what did it leave behind?
echo  ============================================================
echo.
if exist "%APP%\data" (
    echo  [OK] data\ created beside the exe
    dir /b "%APP%\data"
) else (
    echo  [X]  no data\ was created - settings went somewhere else.
    set "FAIL=1"
)
if exist "%APP%\data\D4AssetBrowser\*.ini" (
    echo  [OK] settings written as INI under data\D4AssetBrowser\
) else (
    echo  [!]  no .ini under data\D4AssetBrowser\ - it may not have reached a
    echo       point where it saves settings. Re-run and open File-^>Settings.
)

echo.
for %%K in ("HKCU\Software\D4AssetBrowser" "HKCU\Software\Diablo4AssetBrowser") do (
    reg query %%K >nul 2>&1
    if errorlevel 1 ( echo  [OK] no registry key %%~K ) else ( echo  [X]  REGISTRY KEY EXISTS: %%~K & set "FAIL=1" )
)

if "%APPDATA_BEFORE%"=="0" (
    set "LEAK=0"
    if exist "%APPDATA%\D4AssetBrowser"      set "LEAK=1"
    if exist "%APPDATA%\Diablo4AssetBrowser" set "LEAK=1"
    if "!LEAK!"=="1" ( echo  [X]  wrote into %%APPDATA%% & set "FAIL=1" ) else ( echo  [OK] nothing in %%APPDATA%% )
) else (
    echo  [-]  %%APPDATA%% check skipped ^(a folder was already there before launch^)
)

echo.
echo  ------------------------------------------------------------
if "%FAIL%"=="0" (
    echo   SMOKE TEST PASSED - the zip is publishable.
) else (
    echo   SMOKE TEST FAILED - see the [X] lines above. Do not publish.
)
echo   Sandbox left in place for inspection: %APP%
echo   Delete it with:  rmdir /s /q "%SANDBOX%"
echo  ------------------------------------------------------------
echo.
pause
