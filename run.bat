@echo off
setlocal
title Diablo4AssetBrowserNative
cd /d "%~dp0"

set "DESTDIR=build\release"
set "EXE=%DESTDIR%\D4AssetBrowser.exe"
if not exist "%EXE%" (
    echo D4AssetBrowser.exe not found - build it first:  build.bat
    pause & exit /b 1
)

set "VINST=%DESTDIR%\vcpkg_installed\x64-windows"

:: Deploy the Qt runtime + plugins + the other DLLs next to the exe so it runs
:: stand-alone (this replaces the cmake --install / windeployqt step).
echo Deploying runtime DLLs and Qt plugins...
copy /y "%VINST%\bin\*.dll" "%DESTDIR%\" >nul 2>&1

for %%P in (platforms imageformats styles iconengines) do (
    if not exist "%DESTDIR%\%%P" mkdir "%DESTDIR%\%%P"
    copy /y "%VINST%\Qt6\plugins\%%P\*.dll" "%DESTDIR%\%%P\" >nul 2>&1
)

:: DIAGNOSTIC: verify the authored ClothData parse (capsules / inv-masses) → cloth_parse.txt
del /q "%DESTDIR%\cloth_parse.txt" >nul 2>&1
set "D4_DUMP_CLOTH=1"

echo Launching...
start "" "%EXE%"
