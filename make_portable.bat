@echo off
setlocal enabledelayedexpansion
REM ============================================================
REM  Make a portable, zip-and-send build of D4 Asset Browser.
REM  Double-click this file. It produces D4AssetBrowser_portable.zip
REM  next to itself -- send that one zip to your friend.
REM ============================================================
cd /d "%~dp0"

set "SRC=build\release"
set "OUT=dist\D4AssetBrowser"
set "ZIP=D4AssetBrowser_portable.zip"

if not exist "%SRC%\D4AssetBrowser.exe" (
  echo [X] Can't find "%SRC%\D4AssetBrowser.exe"
  echo     Build the Release target first, then run this again.
  pause & exit /b 1
)

echo [1/4] Preparing "%OUT%" ...
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%" 2>nul

echo [2/4] Copying app + Qt runtime DLLs + plugin folders ...
copy /y "%SRC%\D4AssetBrowser.exe" "%OUT%\" >nul
copy /y "%SRC%\*.dll"              "%OUT%\" >nul
for %%D in (platforms styles imageformats iconengines) do (
  if exist "%SRC%\%%D" xcopy /e /i /y /q "%SRC%\%%D" "%OUT%\%%D" >nul
)

echo [3/4] Bundling the Microsoft C++ runtime (so no installer is needed) ...
set "GOTCRT="
for %%R in (vcruntime140.dll vcruntime140_1.dll msvcp140.dll) do (
  if exist "%windir%\System32\%%R" (
    copy /y "%windir%\System32\%%R" "%OUT%\" >nul
    set "GOTCRT=1"
  )
)
if not defined GOTCRT (
  echo     ^(Could not find the CRT DLLs on this PC. Your friend may need the
  echo      "Microsoft Visual C++ 2015-2022 Redistributable ^(x64^)" one time.^)
)

echo [4/4] Zipping ...
if exist "%ZIP%" del /q "%ZIP%"
powershell -NoProfile -Command "Compress-Archive -Path '%OUT%\*' -DestinationPath '%ZIP%' -Force"

if not exist "%ZIP%" (
  echo.
  echo [X] Zipping failed -- "%ZIP%" was not created.
  pause & exit /b 1
)

echo.
echo ============================================================
echo  Done. Created:  "%CD%\%ZIP%"
echo.
echo  Send that single zip. Your friend unzips it ANYWHERE and runs
echo  D4AssetBrowser.exe -- no install needed.
echo.
echo  Note: the tool needs Diablo IV game data. Your friend points it
echo  at their OWN extracted d4data / game folder in Settings. Do not
echo  ship game assets ^(large + copyrighted^); ship only this tool.
echo ============================================================
pause
