@echo off
title D4 Asset Browser - Encrypted SNO Dump

REM ---------------------------------------------------------------------------
REM Launches the browser with D4_DUMP_ENCRYPTED=1 to answer ONE question:
REM does a decrypted-but-NAMELESS record carry its authored name inside the
REM binary?
REM
REM WHY THIS EXISTS
REM   Encrypted content (Doom collab, seasonal store sets) reaches CoreTOC with
REM   its name blanked, so the index carries it as "~unnamed_<sno>". The payload
REM   decodes fine when the TACT key is held - the Doom key f159f1f70eabaab1 is,
REM   and it covers 1116 snos. But every roster path in the tool is name-SHAPED
REM   (the wardrobe slot filter is startsWith("barf") + endsWith("_trs")), so a
REM   nameless appearance can never show up. Recovering the name IS the fix, and
REM   the binary is the last place it could be hiding.
REM
REM WHAT IT PRINTS
REM   For every nameless Appearance / Actor / Item / Cloth / Material blob:
REM   its size and every printable ASCII run of 6+ chars, plus per-group counts
REM   of nameless / readable (key held) / carrying-ascii.
REM   Look for "barF_stor245_TRS"-shaped strings. Present = small fix. Absent =
REM   class/gender/slot have to be derived numerically instead.
REM
REM Result (next to the exe):
REM   build\release\data\encrypted_dump.txt
REM
REM NOTE: the dump lives in AppearanceMeta's build pass, which is SKIPPED when
REM its disk cache is still valid - so this deletes that one cache file first.
REM Nothing else is touched; it rebuilds in a few seconds on next launch.
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

set "DATA=%~dp0build\release\data"
set "OUT=%DATA%\encrypted_dump.txt"

REM Force the crawl to actually run, and clear any stale dump so a failure to
REM write can't be mistaken for last run's output.
if exist "%DATA%\appearance_meta_v*.json" (
    echo  [-] Clearing appearance-meta cache so the crawl re-runs...
    del /q "%DATA%\appearance_meta_v*.json"
)
if exist "%OUT%" del /q "%OUT%"

echo.
echo  Launching with D4_DUMP_ENCRYPTED=1
echo  The dump is written during the appearance-metadata crawl - give it the
echo  progress bar's worth of time, then close the window.
echo.

set D4_DUMP_ENCRYPTED=1
"%EXE%"

echo.
if exist "%OUT%" (
    echo  [OK] %OUT%
    echo.
    for /f %%A in ('find /c /v "" ^< "%OUT%"') do echo       %%A lines
    echo.
    echo  --- per-group summary ---
    findstr /b /c:"-- " "%OUT%"
    echo.
    choice /c YN /n /m "  Open it now? [Y/N] "
    if errorlevel 2 goto :done
    start "" notepad "%OUT%"
) else (
    echo  [X] No dump was written.
    echo      The crawl only runs when a game install or d4data folder is
    echo      configured and the CASC reader came up ready - check the log.
)

:done
echo.
pause
