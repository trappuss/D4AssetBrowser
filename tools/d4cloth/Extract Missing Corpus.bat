@echo off
setlocal
title d4cloth - extract corpus (adds any newly-listed cases)
cd /d "%~dp0"

:: Re-runs the corpus extraction for cases.json. Existing pieces are already present, so in
:: practice this fetches whatever was ADDED to cases.json since the last run — currently
:: spiF_stor214_LEG (case "thigh-through-hem"), the primary physics verification asset that
:: every brief references but which was never extracted.

set "EXE=build\d4cloth.exe"
if not exist "%EXE%" ( echo Build the harness first ^(tools\d4cloth^). & pause & exit /b 1 )

:: Adjust if your install moved. --d4data must point at the SNAPSHOT the app uses.
set "GAME=G:\G Games\Diablo IV"
set "D4DATA=%APPDATA%\Diablo4AssetBrowser\Diablo4AssetBrowserNative\d4data"

if not exist "%GAME%" (
    echo.
    echo  [X] Game folder not found: %GAME%
    echo      Edit GAME= at the top of this script.
    echo.
    pause & exit /b 1
)

echo.
echo  Extracting corpus...
echo    game   : %GAME%
echo    d4data : %D4DATA%
echo    out    : corpus\
echo.

"%EXE%" extract --casc "%GAME%" --d4data "%D4DATA%" --cases cases.json --out corpus

echo.
echo  Done. Verify the new piece landed:
if exist "corpus\appearance\spiF_stor214_LEG.meta.bin" (
    echo    [OK]  spiF_stor214_LEG extracted
) else (
    echo    [!!]  spiF_stor214_LEG still missing - check the output above for the reason
    echo          ^(most likely: the appearance name differs, or it is behind a TACT key^)
)
echo.
pause
