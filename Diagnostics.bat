@echo off
setlocal enabledelayedexpansion
title D4 Asset Browser - Diagnostics

REM ---------------------------------------------------------------------------
REM One entry point for every diagnostic in the project.
REM
REM WHY: the diagnostics had grown to nine separate .bat files, several of them
REM one-shot derivation harnesses whose question is long since answered. Nine
REM files with similar names is its own failure mode - you cannot tell which is
REM the current one, so you run the wrong one and read a stale report.
REM
REM Nothing was deleted. The one-shot harnesses still exist and are still
REM launchable from here (section D); they are just no longer competing for
REM attention with the tools you actually use week to week.
REM ---------------------------------------------------------------------------

:menu
cls
echo.
echo   D4 ASSET BROWSER - DIAGNOSTICS
echo   ==============================
echo.
echo   ROUTINE
echo     1  Asset health audit        corpus-wide; diffs against the last run.
echo                                  Run on patch day or after adding TACT keys.
echo                                  Read: newly working / newly BROKEN.
echo     2  Encrypted chain test      seconds. 6 samples + named controls.
echo                                  Run after touching material/texture code.
echo     3  Cloth audit               every cloth piece; tuning + driver arrays.
echo                                  Run after any physics change.
echo.
echo   BUILD
echo     4  Rebuild (fast)            incremental, then launch.
echo     5  Clean rebuild             full, including dependencies.
echo.
echo   ONE-SHOT HARNESSES  (formats already derived - only for re-deriving)
echo     6  Dump encrypted SNOs       what is inside nameless records.
echo     7  Dump material SNO table   63k sweep + raw blobs + TVFS table. SLOW.
echo     8  Check encrypted render    triage a single render failure.
echo     9  Cloth overlay / capsules  cloth debugging visuals.
echo.
echo     0  Exit
echo.
set "C="
set /p "C=  Choose: "

if "%C%"=="1" call "%~dp0Audit Asset Health.bat" & goto menu
if "%C%"=="2" call "%~dp0Test Encrypted Chain.bat" & goto menu
if "%C%"=="3" call "%~dp0Cloth Audit.bat" & goto menu
if "%C%"=="4" call "%~dp0rebuild.bat" & goto menu
if "%C%"=="5" call "%~dp0clean-rebuild.bat" & goto menu
if "%C%"=="6" call "%~dp0Dump Encrypted SNOs.bat" & goto menu
if "%C%"=="7" (
    echo.
    echo   NOTE: this harness exists to DERIVE binary formats. All three
    echo   appearance/material/texture layouts are already derived and verified,
    echo   so a run answers nothing new - it sweeps 63,000 appearances and writes
    echo   a 30 MB TVFS table. Only useful if a format has CHANGED.
    echo.
    choice /c YN /n /m "  Run anyway? [Y/N] "
    if errorlevel 2 goto menu
    call "%~dp0Dump Material SNO Table.bat"
    goto menu
)
if "%C%"=="8" call "%~dp0Check Encrypted Render.bat" & goto menu
if "%C%"=="9" call "%~dp0Debug Cloth Overlay.bat" & goto menu
if "%C%"=="0" exit /b 0
goto menu
