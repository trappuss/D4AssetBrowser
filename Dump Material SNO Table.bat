@echo off
title D4 Asset Browser - Material SNO Table Probe

REM ---------------------------------------------------------------------------
REM Finds where the MATERIAL SNO LIST lives inside an appearance's META binary.
REM
REM WHY
REM   Encrypted appearances (Doom collab, seasonal sets) have no
REM   json/base/meta/Appearance/<name>.app.json, and the ENTIRE material chain
REM   goes through that file:
REM       appearanceRoster(.app.json) -> material NAME
REM       -> Material/<name>.mat.json -> material SNO + texture names
REM       -> Texture/<name>.tex.json  -> width/height/format
REM   So they render white with parts labelled "part 7", "part 8", ...
REM
REM   The binary ALREADY gives the per-sub-object material ORDER
REM   (ModelParser.cpp:356, meta.i32(so + 0x60)). The only missing half is the
REM   sno LIST that order indexes into.
REM
REM HOW IT AVOIDS GUESSING
REM   For a NAMED appearance the JSON states exactly which material snos exist.
REM   This searches the meta blob for those u32 values and prints every offset
REM   they occur at. Across many appearances, a constant offset - or a constant
REM   stride between consecutive materials - IS the table. One sample proves
REM   nothing; look for the pattern.
REM
REM HOW TO USE
REM   1. Close the WARDROBE tab (its cloth sim floods the log).
REM   2. In the Models tab, click through 15-20 NAMED appearances with several
REM      materials each - barF_base00_TRS, necF_base01_TRS, palF_stor171_TRS
REM      are good ones. Variety matters more than count.
REM   3. Close the app and read the report this prints.
REM
REM Reads: build\release\D4AssetBrowser.log
REM ---------------------------------------------------------------------------

cd /d "%~dp0"

set "EXE=%~dp0build\release\D4AssetBrowser.exe"
set "LOG=%~dp0build\release\D4AssetBrowser.log"
set "SRC=%~dp0src\tabs\ModelsTab.cpp"

if not exist "%EXE%" (
    echo.
    echo  [X] Not found: %EXE%   ^(build first^)
    echo.
    pause
    exit /b 1
)

REM Stale-binary guard - an exe older than the probe does not contain it, and the
REM empty result looks exactly like "the probe found nothing".
for /f %%T in ('powershell -NoProfile -Command ^
    "if ((Get-Item '%EXE%').LastWriteTime -lt (Get-Item '%SRC%').LastWriteTime) {'STALE'} else {'OK'}"') do set "FRESH=%%T"
if "%FRESH%"=="STALE" (
    echo.
    echo  [X] STALE BINARY - exe is older than src\tabs\ModelsTab.cpp, so it does
    echo      not contain the D4_DUMP_MATSNO probe. Rebuild, then run this again.
    echo.
    pause
    exit /b 1
)

echo.
echo  Launching with D4_DUMP_MATSNO=1
echo.
echo  Click through 15-20 NAMED appearances in the Models tab, with the
echo  Wardrobe tab CLOSED. Then close the app.
echo.

set D4_DUMP_MATSNO=1
"%EXE%"

echo.
echo  ==================== RESULTS ====================
echo.

findstr /c:"matsno:" "%LOG%" > "%TEMP%\d4matsno.txt" 2>nul
for /f %%A in ('find /c /v "" ^< "%TEMP%\d4matsno.txt"') do set "N=%%A"

if "%N%"=="0" (
    echo   No probe output. Either no appearance with materials was selected,
    echo   or every one you clicked was encrypted ^(no JSON = no ground truth,
    echo   so nothing to search for^). Click NAMED appearances.
    goto :done
)

echo   %N% appearance^(s^) probed.
echo.
echo   --- summary: how often were all snos found, and was the stride uniform? ---
findstr /c:"stride UNIFORM" "%TEMP%\d4matsno.txt" | find /c /v "" > "%TEMP%\d4u.txt"
set /p UNI=<"%TEMP%\d4u.txt"
echo   uniform-stride hits: %UNI% of %N%
echo.
echo   A high uniform count means the material snos sit in a contiguous array
echo   and the stride is its record size - that is the table. A low count means
echo   they are scattered and the reference is indirect ^(offset table, or the
echo   snos live in a sub-structure^).
echo.
echo   --- full output ---
type "%TEMP%\d4matsno.txt"
del /q "%TEMP%\d4matsno.txt" "%TEMP%\d4u.txt" 2>nul

:done
echo.
pause
