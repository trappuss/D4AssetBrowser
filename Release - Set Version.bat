@echo off
title D4AssetBrowser - Set Version
cd /d "%~dp0"

REM ---------------------------------------------------------------------------
REM  Sets the release version in src\main.cpp, CMakeLists.txt and vcpkg.json.
REM
REM  Usage:  "Set Version.bat"          prompts for the version
REM          "Set Version.bat 2.3.0"    non-interactive
REM
REM  This file is deliberately a thin launcher. The first version embedded the
REM  whole job in a powershell -Command string, and the two regexes containing a
REM  quote character silently failed to match while the one without it worked -
REM  so it reported "no version found" for two of the three files. A regex that
REM  quietly matches nothing is the worst possible failure for a script whose only
REM  job is to report what it found, and no version bump is worth fighting cmd's
REM  escaping rules for. The logic lives in set-version.ps1 where a quote is just
REM  a quote. -File (not -Command) means cmd never re-parses the script body.
REM ---------------------------------------------------------------------------

if not exist "%~dp0set-version.ps1" (
    echo.
    echo  [X] set-version.ps1 is missing - it must sit beside this file.
    echo.
    pause & exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0set-version.ps1" -Version "%~1"
set "RC=%errorlevel%"

echo.
pause
exit /b %RC%
