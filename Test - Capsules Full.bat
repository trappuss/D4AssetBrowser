@echo off
setlocal
title D4 Asset Browser - authored capsules at FULL size (D4_CAPS_FULL=1)
cd /d "%~dp0"

set "EXE=build\release\D4AssetBrowser.exe"
if not exist "%EXE%" ( echo Build first ^(rebuild.bat^). & pause & exit /b 1 )

:: Deploy Qt runtime next to the exe, same as run.bat.
set "VINST=build\release\vcpkg_installed\x64-windows"
copy /y "%VINST%\bin\*.dll" "build\release\" >nul 2>&1
for %%P in (platforms imageformats styles iconengines) do (
    if not exist "build\release\%%P" mkdir "build\release\%%P"
    copy /y "%VINST%\Qt6\plugins\%%P\*.dll" "build\release\%%P\" >nul 2>&1
)

:: THE POINT OF THIS SCRIPT ------------------------------------------------
:: Authored collision capsules at 1.0x instead of the 0.52 "Capsule size"
:: default. The game's own radii are 70-331 mm; the slider has been shrinking
:: them to ~half. Off by default in the app because 0.52 is visually verified.
set D4_CAPS_FULL=1
:: Prints "cloth-caps: N authored | radius1 a..b | slider s | APPLIED xN -> a..b"
set D4_DUMP_CLOTH=1

echo.
echo  ===========================================================
echo   AUTHORED CAPSULES AT FULL SIZE   (D4_CAPS_FULL=1)
echo  ===========================================================
echo.
echo   Compare against a normal run.bat launch. What to do:
echo.
echo     1. Load spiF_stor210_LEG and spiF_stor211_LEG
echo     2. Overlays -^> tick "Collision model"  (it mirrors the solver)
echo     3. Physics panel -^> cycle the capsule-axis button:
echo          X  ->  Y  ->  Z  ->  bone-dir      (default is bone-dir)
echo        Watch which axis makes capsules WRAP the thigh rather
echo        than lie across it.
echo     4. Note which axis stops 210 clipping WITHOUT freezing 211.
echo     5. Help -^> Export log   (contains the cloth-caps: line)
echo.
echo   Verify the setting is live: the log's cloth-caps line must
echo   read  APPLIED x1.00  [D4_CAPS_FULL=1].  If it says x0.52 the
echo   variable did not reach the app.
echo  ===========================================================
echo.

start "" "%EXE%"
