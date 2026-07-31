@echo off
REM ===================================================================
REM  Extract Diablo IV TACT decryption keys from the RUNNING game and
REM  drop them into the app's configured TACT keys folder.
REM
REM  Just double-click this file. It will:
REM    1. ask for Administrator rights (needed to read game memory),
REM    2. find the running "Diablo IV.exe",
REM    3. scan its memory for TACT keys,
REM    4. write them next to your other key files,
REM    5. tell you to hit File > Reload in the app.
REM
REM  Before running: launch Diablo IV and load into a character (the
REM  keys are only in memory once the game has streamed assets).
REM ===================================================================

setlocal EnableDelayedExpansion
title Diablo IV TACT key extractor

REM ---- Self-elevate to Administrator ---------------------------------
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting Administrator rights...
    powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"

REM ---- Locate Python -------------------------------------------------
set "PY="
where py >nul 2>&1 && set "PY=py -3"
if not defined PY ( where python >nul 2>&1 && set "PY=python" )
if not defined PY (
    echo.
    echo ERROR: Python was not found on PATH.
    echo Install Python 3 from https://www.python.org/downloads/  ^(tick "Add to PATH"^)
    echo then run this file again.
    echo.
    pause
    exit /b 1
)

REM ---- Confirm the scanner is next to this file ----------------------
if not exist "%~dp0tact_scan.py" (
    echo.
    echo ERROR: tact_scan.py was not found next to this batch file.
    echo Both files must sit in the same folder.
    echo.
    pause
    exit /b 1
)

REM ---- Find the running Diablo IV process ----------------------------
set "D4PID="
for /f "tokens=2 delims=," %%A in ('tasklist /fi "imagename eq Diablo IV.exe" /fo csv /nh 2^>nul') do set "D4PID=%%~A"
if not defined D4PID (
    echo.
    echo ERROR: "Diablo IV.exe" is not running.
    echo Launch the game and load into a character, then run this again.
    echo.
    pause
    exit /b 1
)

REM ---- Read the app's configured TACT keys folder from the registry --
REM  QSettings stores it under HKCU\Software\Diablo4AssetBrowser\
REM  D4AssetBrowser\paths\tactKeysPath.
set "KEYSET="
for /f "tokens=2,*" %%A in ('reg query "HKCU\Software\Diablo4AssetBrowser\D4AssetBrowser\paths" /v tactKeysPath 2^>nul ^| find "tactKeysPath"') do set "KEYSET=%%B"

set "KEYDIR="
if defined KEYSET (
    if exist "%KEYSET%\" (
        set "KEYDIR=%KEYSET%"
    ) else if exist "%KEYSET%" (
        for %%I in ("%KEYSET%") do set "KEYDIR=%%~dpI"
    )
)
if not defined KEYDIR (
    set "KEYDIR=%APPDATA%\Diablo4AssetBrowser\D4AssetBrowser\tact_keys"
    set "NEEDSET=1"
)
if not exist "%KEYDIR%" mkdir "%KEYDIR%"
set "OUT=%KEYDIR%\extracted_tact_keys.txt"

echo.
echo Found Diablo IV at PID !D4PID!.
echo Scanning game memory for TACT keys (this can take up to a minute)...
echo.
%PY% "%~dp0tact_scan.py" !D4PID! "%OUT%"

echo.
echo ============================================================
echo  Keys written to:
echo    %OUT%
if defined NEEDSET (
    echo.
    echo  NOTE: the app did not have a TACT keys folder set yet.
    echo  In the app open  File ^> Settings  and set the TACT keys
    echo  folder to:
    echo    %KEYDIR%
)
echo.
echo  Then in the app:  File ^> Reload
echo ============================================================
echo.
pause
