@echo off
setlocal enabledelayedexpansion
title D4 Asset Browser - Encrypted Render Triage

REM ---------------------------------------------------------------------------
REM Answers ONE question with no rebuild: when an encrypted appearance shows
REM "This asset has no displayable geometry", did ModelParser::parseApp actually
REM RUN and fail, or was it never called?
REM
REM WHY IT MATTERS
REM   ModelsTab.cpp:7101 loads TWO separate CASC files:
REM       meta    = readMetaBySno(sno)      -> base/meta/<sno>
REM       payload = readPayloadBySno(sno)   -> base/payload/<sno>
REM   and only calls the parser if BOTH are non-empty:
REM       if (!meta.isEmpty() && !payload.isEmpty()) *geo = parseApp(meta,payload);
REM   That guard logs NOTHING. Every early exit inside parseApp is also silent
REM   (12 of them). So the two failures look identical from the UI.
REM
REM   "loadGeometry: parse produced no geometry" (ModelsTab.cpp:7131) is emitted
REM   ONLY on the path where geo was produced and came back invalid - i.e. only
REM   when parseApp ran. Its presence/absence is the discriminator.
REM
REM HOW TO USE
REM   1. Close the WARDROBE tab first. Its cloth sim writes ~1 line/second and
REM      will bury the lines you want.
REM   2. In the Models tab, search necF_stor245_TRS and click the row.
REM   3. Run this bat.
REM ---------------------------------------------------------------------------

cd /d "%~dp0"
REM data\ beside the exe - see the note in Test Encrypted Chain.bat. The old path still
REM exists on disk from earlier runs, so leaving it here reads a stale log silently.
set "LOG=%~dp0build\release\data\D4AssetBrowser.log"

if not exist "%LOG%" (
    echo.
    echo  [X] No log at %LOG%
    echo      Run the app at least once first.
    echo.
    pause
    exit /b 1
)

echo.
echo  Log: %LOG%
echo.

set "HITS=0"
for /f %%A in ('findstr /c:"loadGeometry: parse produced no geometry" "%LOG%" ^| find /c /v ""') do set "HITS=%%A"

echo  ================= VERDICT =================
if "%HITS%"=="0" (
    echo.
    echo   parseApp was NEVER CALLED.
    echo.
    echo   The guard at ModelsTab.cpp:7103 short-circuited, which means
    echo   readMetaBySno^(^) came back EMPTY - base/meta/^<sno^> is missing or
    echo   encrypted under a TACT key you do not hold. The 950 KB payload is
    echo   fine; the META file is the blocker.
    echo.
    echo   NEXT: extend the dump to report meta size alongside payload size for
    echo   group 9, and check EncryptedSNOs.dat.json for the meta file's key.
) else (
    echo.
    echo   parseApp RAN and returned invalid  ^(%HITS% occurrence^(s^)^).
    echo.
    echo   meta and payload were both non-empty, so the failure is inside
    echo   ModelParser::parseApp. All 12 early exits there are SILENT, so the
    echo   log cannot say which one.
    echo.
    echo   NEXT: add a one-line qWarning to each early exit in
    echo   src/model/ModelParser.cpp ^(lines 790, 791, 797, 817, 820, 823, 824,
    echo   834, 892, 921, 932, 945^) naming the gate. Worth doing regardless -
    echo   right now every parse failure in the tool is indistinguishable.
)
echo.
echo  ==========================================
echo.

echo  --- encrypted-name recovery ---
findstr /c:"encrypted-name recovery" "%LOG%"
if errorlevel 1 echo  ^(none - index came from cache, or the pass did not run^)
echo.

echo  --- last 5 model/geometry warnings ---
findstr /c:"loadGeometry:" /c:"model: " /c:"parse INVALID" "%LOG%" > "%TEMP%\d4gw.txt"
for /f %%A in ('find /c /v "" ^< "%TEMP%\d4gw.txt"') do set /a SKIP=%%A-5
if !SKIP! LSS 0 set SKIP=0
more +!SKIP! < "%TEMP%\d4gw.txt"
del /q "%TEMP%\d4gw.txt" 2>nul
echo.
pause
