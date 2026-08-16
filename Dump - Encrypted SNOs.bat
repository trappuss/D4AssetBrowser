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

REM STALE-BINARY GUARD. The dump code lives in AppearanceMeta.cpp; if the exe is
REM older than it, the running binary simply does not contain the instrument and
REM you get an empty result that looks exactly like "the crawl didn't run". That
REM already cost one round trip - the exe was 24 minutes older than the source.
set "SRC=%~dp0src\index\AppearanceMeta.cpp"
for /f %%T in ('powershell -NoProfile -Command ^
    "if ((Get-Item '%EXE%').LastWriteTime -lt (Get-Item '%SRC%').LastWriteTime) {'STALE'} else {'OK'}"') do set "FRESH=%%T"
if "%FRESH%"=="STALE" (
    echo.
    echo  [X] STALE BINARY - the exe is OLDER than src\index\AppearanceMeta.cpp,
    echo      so it does not contain the D4_DUMP_ENCRYPTED code at all.
    echo.
    echo      Rebuild first ^(rebuild.bat^), then run this again.
    echo.
    pause
    exit /b 1
)

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
    echo  [X] No dump was written. The exe is current, so the crawl itself did
    echo      not reach the dump. In order of likelihood:
    echo        1. closed too early - the dump is at the END of the crawl, after
    echo           the appearance/item passes. Wait for the progress bar to go.
    echo        2. no CASC reader - the dump needs a configured GAME INSTALL, not
    echo           just d4data, because it reads the encrypted payloads.
    echo        3. no d4data folder configured, so the crawl never starts.
    echo      Check the log for "AppearanceMeta: delta phase" - if that line is
    echo      absent, it is 2 or 3; if present, it is 1.
)

:done
echo.
pause
