@echo off
REM ===========================================================================
REM  Does the export texture cache actually help? Measure it, twice, A/B.
REM ===========================================================================
REM
REM  MaterialDecode::TextureCacheScope memoises decoded textures for the length of
REM  an export run, because nothing downstream remembered a decode: buildExportMats()
REM  decodes once per palette SLOT rather than per distinct material, and a class's
REM  appearances share their detail maps almost completely, so every model re-read
REM  and re-BC-decoded the same ones from scratch.
REM
REM  That is a claim. This script is how you check it on YOUR data:
REM
REM    PASS 1  D4_NO_TEXCACHE=1  - the same binary with the cache switched off
REM    PASS 2  cache on          - identical export, identical settings
REM
REM  Both passes print an "export perf:" line to stderr, which this script captures
REM  and compares. There is no headless export mode, so you drive the export by hand
REM  in each pass - the script's job is to make the two runs identical and to do the
REM  arithmetic honestly.
REM
REM  It temporarily rewrites data\D4AssetBrowser.ini so both passes use the same
REM  export options, and restores it at the end. The backup is left on disk if
REM  anything goes wrong - the path is printed below.
REM ===========================================================================
setlocal EnableDelayedExpansion
title D4 Asset Browser - export cache A/B test
cd /d "%~dp0"

set "EXE=build\release\D4AssetBrowser.exe"
if not exist "%EXE%" ( echo Build first ^(rebuild.bat^). & pause & exit /b 1 )

set "DATA=build\release\data"
set "INI=%DATA%\D4AssetBrowser.ini"
set "INIBAK=%DATA%\D4AssetBrowser.ini.perfbak"
set "OUT=%~dp0_perftest_out"
set "LOGA=%~dp0_perftest_baseline.log"
set "LOGB=%~dp0_perftest_cached.log"
set "PS=%~dp0test-export-cache.ps1"

if not exist "%PS%" ( echo Missing test-export-cache.ps1 next to this script. & pause & exit /b 1 )

REM Deploy the Qt runtime next to the exe, same as run.bat - otherwise pass 1 dies
REM before it logs anything and the result looks like "the cache broke the build".
set "VINST=build\release\vcpkg_installed\x64-windows"
copy /y "%VINST%\bin\*.dll" "build\release\" >nul 2>&1
for %%P in (platforms imageformats styles iconengines) do (
    if not exist "build\release\%%P" mkdir "build\release\%%P"
    copy /y "%VINST%\Qt6\plugins\%%P\*.dll" "build\release\%%P\" >nul 2>&1
)

REM Refuse to measure a stale binary. This is the failure that actually happened the
REM first time this script was run: the exe predated the kill-switch, so the "baseline"
REM pass ran WITH the cache, both halves were identical, and the report was useless.
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS%" -Mode Check -Exe "%EXE%" -Src "%~dp0src"
if errorlevel 1 ( echo. & pause & exit /b 1 )

echo.
echo  ===========================================================
echo   EXPORT TEXTURE CACHE - A/B TEST
echo  ===========================================================
echo.
echo   Your settings will be backed up to:
echo     %INIBAK%
echo   and restored when this finishes.
echo.
echo   Test output folder (cleared before each pass):
echo     %OUT%
echo.
echo  -----------------------------------------------------------
echo   IN BOTH PASSES, DO EXACTLY THE SAME THING:
echo.
echo     1. Bulk Extract tab
echo     2. Preset: "All Barbarian Appearance"  (or any MODELS preset
echo        with a few hundred matches - the more shared detail maps
echo        the better, which is the case this exists for)
echo     3. Extract to...  ->  pick   %OUT%
echo     4. Wait for "done in ... s"
echo     5. CLOSE THE APP  - the script continues when it exits
echo.
echo   Do not change any export setting between passes; the script
echo   has already set them and will check they held.
echo  -----------------------------------------------------------
echo.
pause

REM ---- back up and pin the export settings so both passes are comparable -----
if exist "%INI%" copy /y "%INI%" "%INIBAK%" >nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS%" -Mode Prepare -Ini "%INI%"
if errorlevel 1 ( echo Could not prepare settings. & goto :restore )

REM ===================== PASS 1 - BASELINE, CACHE OFF ========================
if exist "%OUT%" rd /s /q "%OUT%"
mkdir "%OUT%" 2>nul
echo.
echo  === PASS 1 of 2 - BASELINE (cache OFF) ===
echo   Run the export described above, then close the app.
echo.
set D4_DUMP_EXPORTPERF=1
set D4_NO_TEXCACHE=1
REM Attached, NOT "start": the script has to wait for the app to exit, and a fault
REM before the first log line still lands in the capture file this way.
"%EXE%" 2>"%LOGA%"
echo   (app exited, code %ERRORLEVEL%)

REM ===================== PASS 2 - CACHE ON ===================================
if exist "%OUT%" rd /s /q "%OUT%"
mkdir "%OUT%" 2>nul
echo.
echo  === PASS 2 of 2 - CACHE ON ===
echo   Run the SAME export again, then close the app.
echo.
set D4_DUMP_EXPORTPERF=1
set "D4_NO_TEXCACHE="
"%EXE%" 2>"%LOGB%"
echo   (app exited, code %ERRORLEVEL%)

REM ===================== COMPARE =============================================
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS%" -Mode Report -Baseline "%LOGA%" -Cached "%LOGB%"
set "VERDICT=%ERRORLEVEL%"

:restore
echo.
if exist "%INIBAK%" (
    copy /y "%INIBAK%" "%INI%" >nul && del /q "%INIBAK%"
    echo   Settings restored.
) else (
    echo   No settings backup to restore.
)
echo.
echo   Full captures kept for inspection:
echo     %LOGA%
echo     %LOGB%
echo   The export output is in %OUT% - delete it when you are done.
echo.
REM Propagate the comparison's verdict: 0 = the cache measurably helped, 1 = it did
REM not, 2 = the run could not be measured. Useful if this is ever run unattended.
if not defined VERDICT set "VERDICT=2"
echo   Verdict code: %VERDICT%
pause
endlocal & exit /b %VERDICT%
