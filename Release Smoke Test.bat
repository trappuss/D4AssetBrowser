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
REM    5. it leaves NOTHING in AppData and NOTHING in the registry
REM
REM  3-5 need the app to actually launch, so this stops and hands it to you:
REM  let it finish loading, then close it. The checks run on the way back.
REM
REM  EVERY VERDICT LINE IS ALSO WRITTEN TO smoke_test.txt. The first version of
REM  this script printed to the console only, which meant the result vanished
REM  with the window and could not be attached to anything or read back later.
REM ---------------------------------------------------------------------------

set "SANDBOX=%TEMP%\D4ABSmoke"
set "APP=%SANDBOX%\D4AssetBrowser"
set "LOG=%~dp0smoke_test.txt"
set "FAIL=0"

REM Fresh log each run - a stale PASS sitting next to a failed run is worse than no log.
if exist "%LOG%" del /q "%LOG%"
call :say "D4AssetBrowser - release smoke test"
for /f "tokens=*" %%D in ('powershell -NoProfile -Command "Get-Date -Format ''yyyy-MM-dd HH:mm:ss''"') do call :say "  run: %%D"

echo.
echo  ============================================================
echo   STEP 1/4 - build + package
echo  ============================================================
echo.
call "%~dp0package-release.bat"
if errorlevel 1 (
    call :say "[X] package-release.bat failed - nothing to test."
    pause & exit /b 1
)

REM package-release.bat version-stamps the zip from main.cpp, so read it the same way
REM rather than hardcoding - a version bump must not silently test last release's zip.
set "APPVER="
for /f tokens^=2^ delims^=^" %%v in ('findstr /c:"setApplicationVersion" "%~dp0src\main.cpp"') do set "APPVER=%%v"
if "%APPVER%"=="" set "APPVER=dev"
set "ZIP=%~dp0dist\D4AssetBrowser_v%APPVER%.zip"
call :say "  version: %APPVER%"
if not exist "%ZIP%" (
    call :say "[X] expected zip not found: %ZIP%"
    pause & exit /b 1
)
for %%A in ("%ZIP%") do call :say "  zip: %%~zA bytes, %%~tA"
call :say ""

echo.
echo  ============================================================
echo   STEP 2/4 - unzip into a clean folder outside the repo
echo  ============================================================
echo.
taskkill /im D4AssetBrowser.exe /f >nul 2>&1
if exist "%SANDBOX%" rmdir /s /q "%SANDBOX%"
mkdir "%SANDBOX%" 2>nul
powershell -NoProfile -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; [System.IO.Compression.ZipFile]::ExtractToDirectory('%ZIP%', '%SANDBOX%')"
if errorlevel 1 ( call :say "[X] extract failed" & pause & exit /b 1 )

if not exist "%APP%\D4AssetBrowser.exe" (
    call :say "[X] D4AssetBrowser.exe missing after extract"
    call :say "    If the sandbox holds files with BACKSLASHES IN THEIR NAMES, the zip"
    call :say "    was written with backslash separators - package-release.bat documents"
    call :say "    that trap. Windows copes; unzip on Linux/macOS does not."
    dir /b "%SANDBOX%" >> "%LOG%"
    dir /b "%SANDBOX%"
    pause & exit /b 1
)
call :say "[OK] tree extracted to %APP%"
for %%F in ("D4AssetBrowser.exe" "Qt6Core.dll" "platforms\qwindows.dll" "styles\qmodernwindowsstyle.dll" "imageformats\qjpeg.dll") do (
    if exist "%APP%\%%~F" ( call :say "[OK] %%~F" ) else ( call :say "[X]  %%~F MISSING" & set "FAIL=1" )
)
if exist "%APP%\data" (
    call :say "[X]  data\ already present in a FRESH unzip - build artefacts leaked into the package"
    set "FAIL=1"
) else (
    call :say "[OK] no data\ yet, correct for a fresh unzip"
)
call :say ""

REM Baseline the two places a NON-portable app would write, so "clean afterwards"
REM is measured rather than assumed. Pre-existing AppData folders from an older
REM non-portable build would make that check meaningless, so say so rather than
REM reporting a pass this run did not earn.
set "APPDATA_BEFORE=0"
if exist "%APPDATA%\D4AssetBrowser"      set "APPDATA_BEFORE=1"
if exist "%APPDATA%\Diablo4AssetBrowser" set "APPDATA_BEFORE=1"

echo.
echo  ============================================================
echo   STEP 3/4 - launch it. LET IT LOAD, THEN CLOSE IT.
echo  ============================================================
echo.
echo   It is unconfigured, so expect the first-run prompts. You do not need to
echo   download d4data - just get to the main window, open File-^>Settings once
echo   so it has a reason to write an ini, then close it.
echo.
if "%APPDATA_BEFORE%"=="1" (
    echo   NOTE: AppData already has a D4AssetBrowser folder from an earlier
    echo         non-portable build, so that check will be inconclusive.
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
    call :say "[OK] data\ created beside the exe"
    for /f "delims=" %%E in ('dir /b "%APP%\data"') do call :say "       data\%%E"
) else (
    call :say "[X]  no data\ was created - settings went somewhere else"
    set "FAIL=1"
)
if exist "%APP%\data\D4AssetBrowser\*.ini" (
    call :say "[OK] settings written as INI under data\D4AssetBrowser\"
) else (
    call :say "[?]  no .ini under data\D4AssetBrowser\ - it may not have reached a point"
    call :say "     where it saves settings. Re-run and open Settings once."
)
if exist "%APP%\data\D4AssetBrowser.log" (
    call :say "[OK] log written to data\D4AssetBrowser.log"
    copy /y "%APP%\data\D4AssetBrowser.log" "%~dp0smoke_test_app.log" >nul
    call :say "     copied here as smoke_test_app.log"
    REM Startup complaints a downloader would hit. Not fatal on their own - an
    REM unconfigured first run legitimately reports no game folder - but they are
    REM the difference between "started" and "started correctly".
    call :say ""
    call :say "  first WARN/ERROR lines from that log:"
    powershell -NoProfile -Command "Select-String -Path '%APP%\data\D4AssetBrowser.log' -Pattern 'ERROR|WARN|FAIL|missing|cannot' | ForEach-Object { '       ' + $_.Line } | Select-Object -First 15" >> "%LOG%" 2>nul
    powershell -NoProfile -Command "Select-String -Path '%APP%\data\D4AssetBrowser.log' -Pattern 'ERROR|WARN|FAIL|missing|cannot' | ForEach-Object { '       ' + $_.Line } | Select-Object -First 15" 2>nul
) else (
    call :say "[?]  no data\D4AssetBrowser.log - it may have exited before logging"
)

call :say ""
for %%K in ("HKCU\Software\D4AssetBrowser" "HKCU\Software\Diablo4AssetBrowser") do (
    reg query %%K >nul 2>&1
    if errorlevel 1 ( call :say "[OK] no registry key %%~K" ) else ( call :say "[X]  REGISTRY KEY EXISTS: %%~K" & set "FAIL=1" )
)

if "%APPDATA_BEFORE%"=="0" (
    set "LEAK=0"
    if exist "%APPDATA%\D4AssetBrowser"      set "LEAK=1"
    if exist "%APPDATA%\Diablo4AssetBrowser" set "LEAK=1"
    if "!LEAK!"=="1" ( call :say "[X]  wrote into AppData" & set "FAIL=1" ) else ( call :say "[OK] nothing written into AppData" )
) else (
    call :say "[-]  AppData check skipped, a folder was already there before launch"
)

call :say ""
if "%FAIL%"=="0" (
    call :say "RESULT: PASSED - the zip is publishable."
) else (
    call :say "RESULT: FAILED - see the [X] lines above. Do not publish."
)
call :say "Sandbox left in place: %APP%"
call :say "Delete it with:  rmdir /s /q %SANDBOX%"

echo.
echo  ------------------------------------------------------------
echo   Results also written to: %LOG%
echo  ------------------------------------------------------------
echo.
pause
exit /b %FAIL%

REM ── say: one line to the console AND to the log, so the verdict outlives the window.
REM  "echo(" not "echo " so an empty argument prints a blank line instead of "ECHO is on".
REM  The redirection goes FIRST: "echo %~1>> log" is parsed as "echo %~1" with a
REM  1>> redirect whenever the text happens to end in a digit, which silently ate
REM  lines. Keep the argument free of  > < ^ & | % !  - cmd rescans expanded text
REM  for those and there is no quoting that survives it here.
:say
echo(%~1
>>"%LOG%" echo(%~1
exit /b 0
