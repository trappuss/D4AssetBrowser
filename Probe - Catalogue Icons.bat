@echo off
REM Catalogue icon probe -> why a Catalogue row is blank, WITHOUT a rebuild.
REM
REM Reads the caches the app has already written into build\release\data and answers in about a
REM second. It does NOT build, does NOT launch the app, and changes nothing.
REM
REM Cache files are found by STEM, not by version: store_products_v*.json, coretoc_v*.bin,
REM icon_index_v*.json. A cache version bump is exactly when this is most needed, so it must not
REM be the thing that breaks.
REM
REM Reports both routes a row can get a picture by:
REM   1. BY NAME    the templates cardImage/heroImage derive from the product's SNO name
REM   2. BY HANDLE  the product's authored art handles, with a policy comparison so a policy that
REM                 would stamp one image across many rows is rejected on numbers, not on a hunch
REM   3. BY CONTENT the icon of the first thing the product contains
REM
REM Writes build\release\data\catalogue_icon_probe.txt as well as printing to this window, so
REM the answer survives closing the console and shows up under Help ^> Diagnostic output.
REM
REM After a game patch the caches are stale: launch the app once to refresh them, then re-run.
setlocal
cd /d "%~dp0"

set "DATA=build\release\data"

REM python, then the py launcher - a Windows install often has only one of the two.
set "PY="
where python >nul 2>&1 && set "PY=python"
if not defined PY where py >nul 2>&1 && set "PY=py -3"
if not defined PY (
    echo Python 3 was not found on PATH ^(tried "python" and "py"^).
    echo Install it from python.org, or run: tools\catalogue_icon_probe.py
    pause & exit /b 1
)

REM UTF-8 output, so the console codepage cannot turn a report into a crash.
set "PYTHONIOENCODING=utf-8"

%PY% "tools\catalogue_icon_probe.py" "%DATA%"
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo.
    echo Probe exited with code %RC% - the message above says why.
)

echo.
pause
