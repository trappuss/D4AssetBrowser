@echo off
title D4 Asset Browser - Cloth Audit

REM ---------------------------------------------------------------------------
REM Corpus-wide cloth audit: launches the browser with D4_CLOTH_AUDIT=1.
REM ~1.5s after the window appears, a progress dialog sweeps EVERY Appearance
REM SNO, extracts each embedded ClothData block, and checks:
REM   - does its .clt.json tuning resolve (the feathers/crest bug class)
REM   - which authored driving arrays are present (missing = legacy fallback
REM     path runs for that piece - the cape-jut bug class)
REM Results (next to the exe):
REM   build\release\cloth_audit.csv           one row per cloth piece
REM   build\release\cloth_audit_summary.txt   ranked problem lists
REM Fix order: TUNING-FAILED first, then NO-DRIVERSKIN / NO-FOLLOWERS.
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

set D4_CLOTH_AUDIT=1
set D4_DUMP_CLOTH=1
"%EXE%"
