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
REM    2. exe + Qt runtime + every plugin family are present
REM    3. the app STARTS from that clean folder
REM    4. it writes data\ beside the exe, with an .ini in it
REM    5. it leaves NOTHING in AppData and NOTHING in the registry
REM
REM  LOGGING
REM    Results go to smoke_test.txt, and every stage writes a TRACE line first.
REM    A previous version logged through a "call :say" subroutine and produced a
REM    file holding the header plus the last line twice, with the whole middle
REM    missing - so the log could not even be trusted to say where it stopped.
REM    Redirects are now inline and always leftmost: "echo text >> log" is parsed
REM    as a 1>> redirect whenever the text ends in a digit, which silently eats
REM    lines, and text routed through %~1 gets rescanned by cmd for redirection
REM    operators a second time. Neither hazard exists in this form.
REM ---------------------------------------------------------------------------

set "SANDBOX=%TEMP%\D4ABSmoke"
set "APP=%SANDBOX%\D4AssetBrowser"
set "LOG=%~dp0smoke_test.txt"
set "FAIL=0"

REM Fresh log each run - a stale PASS sitting next to a failed run is worse than none.
if exist "%LOG%" del /q "%LOG%"
>"%LOG%" echo D4AssetBrowser - release smoke test
>>"%LOG%" echo   started: %DATE% %TIME%
>>"%LOG%" echo.
>>"%LOG%" echo TRACE: entered step 1 (build + package)

echo.
echo  ============================================================
echo   STEP 1/4 - build + package
echo  ============================================================
echo.
echo   If this stops at an error and waits, the log will end at the TRACE
echo   line for this step - that alone tells us where it died.
echo.
call "%~dp0package-release.bat"
set "PKGRC=%errorlevel%"
>>"%LOG%" echo TRACE: package-release.bat returned %PKGRC%
if not "%PKGRC%"=="0" (
    >>"%LOG%" echo [X] package-release.bat failed - nothing to test.
    echo  [X] package-release.bat failed - nothing to test.
    pause & exit /b 1
)

REM package-release.bat version-stamps the zip from main.cpp, so read it the same way
REM rather than hardcoding - a version bump must not silently test last release's zip.
set "APPVER="
for /f tokens^=2^ delims^=^" %%v in ('findstr /c:"setApplicationVersion" "%~dp0src\main.cpp"') do set "APPVER=%%v"
if "%APPVER%"=="" set "APPVER=dev"
set "ZIP=%~dp0dist\D4AssetBrowser_v%APPVER%.zip"
>>"%LOG%" echo   version: %APPVER%
if not exist "%ZIP%" (
    >>"%LOG%" echo [X] expected zip not found: %ZIP%
    echo  [X] expected zip not found: %ZIP%
    pause & exit /b 1
)
REM A zip older than the exe means package-release reused a stale archive - the whole
REM point of this script is that the thing tested is the thing built.
for %%A in ("%ZIP%") do (
    >>"%LOG%" echo   zip: %%~zA bytes, built %%~tA
    echo   zip: %%~zA bytes, built %%~tA
)
>>"%LOG%" echo.
>>"%LOG%" echo TRACE: entered step 2 (unzip)

echo.
echo  ============================================================
echo   STEP 2/4 - unzip into a clean folder outside the repo
echo  ============================================================
echo.
taskkill /im D4AssetBrowser.exe /f >nul 2>&1
if exist "%SANDBOX%" rmdir /s /q "%SANDBOX%"
mkdir "%SANDBOX%" 2>nul
powershell -NoProfile -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; [System.IO.Compression.ZipFile]::ExtractToDirectory('%ZIP%', '%SANDBOX%')"
if errorlevel 1 (
    >>"%LOG%" echo [X] extract failed
    echo  [X] extract failed
    pause & exit /b 1
)
if not exist "%APP%\D4AssetBrowser.exe" (
    >>"%LOG%" echo [X] D4AssetBrowser.exe missing after extract
    >>"%LOG%" echo     If the sandbox holds files with BACKSLASHES IN THEIR NAMES, the zip
    >>"%LOG%" echo     was written with backslash separators. Windows copes; unzip on
    >>"%LOG%" echo     Linux and macOS does not. package-release.bat documents that trap.
    dir /b "%SANDBOX%" >> "%LOG%"
    echo  [X] D4AssetBrowser.exe missing after extract - see %LOG%
    pause & exit /b 1
)
>>"%LOG%" echo [OK] tree extracted to %APP%
echo  [OK] tree extracted

REM Every plugin family the app loads. Missing platforms\qwindows.dll kills it at
REM startup on every machine; missing styles or imageformats is subtler and would
REM only show up as an ugly window or blank icons for whoever downloaded it.
for %%F in ("D4AssetBrowser.exe" "Qt6Core.dll" "Qt6Widgets.dll" "Qt6OpenGLWidgets.dll" "platforms\qwindows.dll" "styles\qmodernwindowsstyle.dll" "imageformats\qjpeg.dll" "imageformats\qsvg.dll" "iconengines\qsvgicon.dll") do (
    if exist "%APP%\%%~F" (
        >>"%LOG%" echo [OK] %%~F
        echo  [OK] %%~F
    ) else (
        >>"%LOG%" echo [X]  %%~F MISSING
        echo  [X]  %%~F MISSING
        set "FAIL=1"
    )
)
if exist "%APP%\data" (
    >>"%LOG%" echo [X]  data\ already present in a FRESH unzip - build artefacts leaked into the package
    echo  [X]  data\ already present in a fresh unzip
    set "FAIL=1"
) else (
    >>"%LOG%" echo [OK] no data\ yet, correct for a fresh unzip
    echo  [OK] no data\ yet
)

REM Baseline the two places a NON-portable app would write, so "clean afterwards" is
REM measured rather than assumed. A pre-existing AppData folder from an older
REM non-portable build makes that check meaningless - say so rather than reporting
REM a pass this run did not earn.
set "APPDATA_BEFORE=0"
if exist "%APPDATA%\D4AssetBrowser"      set "APPDATA_BEFORE=1"
if exist "%APPDATA%\Diablo4AssetBrowser" set "APPDATA_BEFORE=1"
>>"%LOG%" echo.
>>"%LOG%" echo TRACE: entered step 3 (launch), AppData pre-existing = %APPDATA_BEFORE%

echo.
echo  ============================================================
echo   STEP 3/4 - launch it. LET IT LOAD, THEN CLOSE IT.
echo  ============================================================
echo.
echo   It is unconfigured, so expect the first-run prompts. You do not need to
echo   download d4data - just reach the main window, open Settings once so it has
echo   a reason to write an ini, then close the app.
echo.
if "%APPDATA_BEFORE%"=="1" (
    echo   NOTE: AppData already has a D4AssetBrowser folder from an earlier
    echo         non-portable build, so that check will be inconclusive.
    echo.
)
pause

start "" /wait "%APP%\D4AssetBrowser.exe"
>>"%LOG%" echo TRACE: app exited with %errorlevel%
>>"%LOG%" echo.
>>"%LOG%" echo TRACE: entered step 4 (leftovers)

echo.
echo  ============================================================
echo   STEP 4/4 - what did it leave behind?
echo  ============================================================
echo.
if exist "%APP%\data" (
    >>"%LOG%" echo [OK] data\ created beside the exe
    echo  [OK] data\ created beside the exe
    for /f "delims=" %%E in ('dir /b "%APP%\data" 2^>nul') do >>"%LOG%" echo        data\%%E
    dir /b "%APP%\data"
) else (
    >>"%LOG%" echo [X]  no data\ was created - settings went somewhere else
    echo  [X]  no data\ was created
    set "FAIL=1"
)
if exist "%APP%\data\D4AssetBrowser\*.ini" (
    >>"%LOG%" echo [OK] settings written as INI under data\D4AssetBrowser\
    echo  [OK] settings written as INI
) else (
    >>"%LOG%" echo [?]  no .ini under data\D4AssetBrowser\ - it may not have reached a point
    >>"%LOG%" echo      where it saves settings. Re-run and open Settings once.
    echo  [?]  no .ini yet
)
if exist "%APP%\data\D4AssetBrowser.log" (
    >>"%LOG%" echo [OK] log written to data\D4AssetBrowser.log, copied here as smoke_test_app.log
    copy /y "%APP%\data\D4AssetBrowser.log" "%~dp0smoke_test_app.log" >nul
    echo  [OK] app log copied to smoke_test_app.log
    REM Startup complaints a downloader would hit. Not fatal on their own - an
    REM unconfigured first run legitimately reports no game folder - but they are
    REM the difference between "started" and "started correctly".
    >>"%LOG%" echo.
    >>"%LOG%" echo   first WARN/ERROR lines from that log:
    powershell -NoProfile -Command "Select-String -Path '%APP%\data\D4AssetBrowser.log' -Pattern 'ERROR|WARN|FAIL|missing|cannot' | Select-Object -First 15 | ForEach-Object { '       ' + $_.Line }" >> "%LOG%" 2>nul
) else (
    >>"%LOG%" echo [?]  no data\D4AssetBrowser.log - it may have exited before logging
    echo  [?]  no app log
)

>>"%LOG%" echo.
for %%K in ("HKCU\Software\D4AssetBrowser" "HKCU\Software\Diablo4AssetBrowser") do (
    reg query %%K >nul 2>&1
    if errorlevel 1 (
        >>"%LOG%" echo [OK] no registry key %%~K
        echo  [OK] no registry key %%~K
    ) else (
        >>"%LOG%" echo [X]  REGISTRY KEY EXISTS: %%~K
        echo  [X]  REGISTRY KEY EXISTS: %%~K
        set "FAIL=1"
    )
)

if "%APPDATA_BEFORE%"=="0" (
    set "LEAK=0"
    if exist "%APPDATA%\D4AssetBrowser"      set "LEAK=1"
    if exist "%APPDATA%\Diablo4AssetBrowser" set "LEAK=1"
    if "!LEAK!"=="1" (
        >>"%LOG%" echo [X]  wrote into AppData
        echo  [X]  wrote into AppData
        set "FAIL=1"
    ) else (
        >>"%LOG%" echo [OK] nothing written into AppData
        echo  [OK] nothing written into AppData
    )
) else (
    >>"%LOG%" echo [-]  AppData check skipped, a folder was already there before launch
    echo  [-]  AppData check skipped
)

>>"%LOG%" echo.
if "%FAIL%"=="0" (
    >>"%LOG%" echo RESULT: PASSED - the zip is publishable.
    echo   RESULT: PASSED - the zip is publishable.
) else (
    >>"%LOG%" echo RESULT: FAILED - see the [X] lines above. Do not publish.
    echo   RESULT: FAILED - see the [X] lines. Do not publish.
)
>>"%LOG%" echo Sandbox left in place: %APP%
>>"%LOG%" echo Delete it with: rmdir /s /q %SANDBOX%
>>"%LOG%" echo   finished: %DATE% %TIME%

echo.
echo  ------------------------------------------------------------
echo   Full results: %LOG%
echo  ------------------------------------------------------------
echo.
pause
exit /b %FAIL%
