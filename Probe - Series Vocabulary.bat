@echo off
REM Series / collection probe -> can the tool derive its own collection tree from d4data?
REM
REM Answers three questions from the game's own export and NOTHING else. diablo4.dad is not
REM contacted, not read, and not required; it is a cross-check we do by eye afterwards, never
REM a data source.
REM
REM   A  LABEL VOCABULARY  every szLabel across all ~61,000 StringLists, per SNO-group prefix.
REM                        The discovery pass: if the data carries a grouping field richer than
REM                        "Series", this is what surfaces it. We are looking for what we do
REM                        not already know about, not confirming what we do.
REM   B  SERIES HARVEST     every StringList carrying a Series row, dumped RAW so the
REM                        normalisation rules get written against real strings.
REM   C  ACQUISITION        StoreProduct's reach: which members the shop sells, which carry a
REM                        season, which are gated behind arRequiresOwning (pass-locked, never
REM                        sold), plus the eType and release-branch distributions. eType's
REM                        labels are not in the data, so this is the only honest way to find
REM                        out whether it separates promotional products from ordinary ones.
REM
REM Reads only. Does NOT build, does NOT launch the app, does NOT touch the game install.
REM Writes three files into "Claude outputs":
REM   series_probe.txt     the report (also printed here)
REM   series_rows.tsv      one row per Series-bearing StringList
REM   series_summary.tsv   one row per normalised collection
REM
REM Takes a minute or two: it opens every StringList in the export. If d4data has not been
REM downloaded yet, launch the app once and let it fetch, then re-run.
setlocal
cd /d "%~dp0"

set "DATA=build\release\data"
if not exist "%DATA%" (
    echo Data folder not found: "%DATA%"
    echo Launch the app once so it downloads d4data, then re-run this.
    pause & exit /b 1
)

REM python, then the py launcher - a Windows install often has only one of the two.
set "PY="
where python >nul 2>&1 && set "PY=python"
if not defined PY where py >nul 2>&1 && set "PY=py -3"
if not defined PY (
    echo Python 3 was not found on PATH ^(tried "python" and "py"^).
    echo Install it from python.org, or run: tools\series_probe.py
    pause & exit /b 1
)

REM UTF-8 output, so the console codepage cannot turn a report into a crash. Series strings
REM carry apostrophes and accented characters.
set "PYTHONIOENCODING=utf-8"

if not exist "Claude outputs" mkdir "Claude outputs"

%PY% "tools\series_probe.py" "%DATA%" "Claude outputs"
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo.
    echo Probe exited with code %RC% - the message above says why.
)

echo.
pause
