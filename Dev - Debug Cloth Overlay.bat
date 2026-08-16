@echo off
setlocal EnableDelayedExpansion
title D4 Asset Browser - Cloth Overlay Diagnostic

REM ---------------------------------------------------------------------------
REM Runs D4AssetBrowser with the cloth-overlay dump enabled, then extracts the
REM diagnostic lines into a small file that is easy to paste back.
REM
REM IMPORTANT: the tool does NOT write a log file on its own. You must export
REM one before closing, or there is nothing for this script to read.
REM ---------------------------------------------------------------------------

cd /d "%~dp0"

set "EXE=%~dp0build\release\D4AssetBrowser.exe"
if not exist "%EXE%" (
    echo.
    echo  [X] Could not find: %EXE%
    echo      Build the project first, then run this again.
    echo.
    pause
    exit /b 1
)

set "LIST_BEFORE=%TEMP%\d4_logs_before.txt"
set "LIST_AFTER=%TEMP%\d4_logs_after.txt"
dir /b /a-d "d4browser_log_*.txt" > "%LIST_BEFORE%" 2>nul

set D4_DUMP_CLOTH=1

echo.
echo  ===========================================================
echo   Cloth overlay diagnostic     ^(D4_DUMP_CLOTH=1^)
echo  ===========================================================
echo.
echo   While the tool is open, do this IN ORDER:
echo.
echo     1. Wardrobe tab, load the outfit with the stray lines
echo     2. Turn the phys-bone overlay ON ^(lines visible on screen^)
echo     3. Wait about 5 seconds  ^(the dump prints every 2s^)
echo     4. Help  -^>  Export log...
echo     5. SAVE IT IN THIS FOLDER, keep the suggested filename
echo     6. Close the tool
echo.
echo   Step 4-5 is not optional. Without it no log exists.
echo  ===========================================================
echo.

set "CONSOLE_LOG=%~dp0cloth_console.txt"
"%EXE%" > "%CONSOLE_LOG%" 2>&1

echo.
echo  Tool closed. Looking for a NEW log...
echo.

dir /b /a-d /o-d "d4browser_log_*.txt" > "%LIST_AFTER%" 2>nul

REM A "new" log is one present now that was not present before launch.
set "SRC="
for /f "usebackq delims=" %%F in ("%LIST_AFTER%") do (
    if not defined SRC (
        findstr /x /c:"%%F" "%LIST_BEFORE%" >nul 2>&1
        if errorlevel 1 set "SRC=%%F"
    )
)

REM Fall back to captured console output if it has anything useful.
if not defined SRC (
    if exist "%CONSOLE_LOG%" (
        findstr /i /c:"cloth-overlay" "%CONSOLE_LOG%" >nul 2>&1
        if not errorlevel 1 set "SRC=cloth_console.txt"
    )
)

if not defined SRC (
    echo  [X] No new log file was created during this run.
    echo.
    echo      This is almost always step 4-5: the tool only writes a log
    echo      when you use  Help -^>  Export log...  and save it here:
    echo      %~dp0
    echo.
    echo      Old logs in this folder were ignored on purpose - they were
    echo      written before the diagnostic existed and cannot contain it.
    echo.
    pause
    exit /b 1
)

set "OUT=%~dp0cloth_overlay_report.txt"
> "%OUT%" (
    echo === cloth overlay report ===
    echo source log: !SRC!
    echo generated : %DATE% %TIME%
    echo.
)
findstr /i /c:"cloth-overlay" /c:"len=" /c:"cloth-build" /c:"cloth-diverge" /c:"cloth-health" "!SRC!" >> "%OUT%" 2>nul

echo  Source log : !SRC!
echo  Report     : cloth_overlay_report.txt
echo.

findstr /i /c:"cloth-overlay" "!SRC!" >nul 2>&1
if errorlevel 1 (
    echo  [!] The log is new, but contains no "cloth-overlay" lines.
    echo.
    echo      Two possible reasons, in order of likelihood:
    echo.
    echo        a^) The exe was not rebuilt after the diagnostic was added.
    echo           Check: does the log contain any "cloth-build" lines?
    findstr /i /c:"cloth-build" "!SRC!" >nul 2>&1
    if errorlevel 1 (
        echo           -^>  NO cloth-build lines either. Cloth code is not
        echo               running at all, or the exe is stale. Rebuild.
    ) else (
        echo           -^>  cloth-build lines ARE present, so the exe runs
        echo               cloth code but predates the overlay dump. Rebuild.
    )
    echo.
    echo        b^) The phys-bone overlay was never switched on while an
    echo           outfit was loaded.
    echo.
) else (
    echo  [OK] Diagnostic captured. Opening the report...
    echo.
    start "" notepad "%OUT%"
)

pause
endlocal
