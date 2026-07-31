@echo off
setlocal enabledelayedexpansion
title Package Diablo4AssetBrowser - portable release folder
cd /d "%~dp0"

:: ============================================================================
::  Builds the portable RELEASE folder:
::    * D4AssetBrowser.exe + the exact Qt6 runtime DLLs and plugins it needs
::      (copied from vcpkg_installed) + README
::    * settings + caches live in a data\ folder beside the exe (no registry, no AppData)
::  Output: dist\D4AssetBrowser\   (+ dist\D4AssetBrowser.zip)
::
::  This uses the normal DYNAMIC Qt from vcpkg (no static Qt build required), so it is
::  the fast path. The folder is fully self-contained: unzip anywhere and run.
:: ============================================================================

taskkill /im D4AssetBrowser.exe /f >nul 2>&1

:: 1. MSVC on PATH (else init vcvars64).
where cl >nul 2>&1
if errorlevel 1 (
    echo [1/5] Initializing Visual Studio 2022 build tools...
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" ( echo   ERROR: Visual Studio 2022 with "Desktop development with C++" required. & pause & exit /b 1 )
    set "VSPATH="
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    if not defined VSPATH ( echo   ERROR: MSVC C++ tools not found. & pause & exit /b 1 )
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" || ( echo   ERROR: vcvars64 failed. & pause & exit /b 1 )
) else ( echo [1/5] MSVC already on PATH. )

:: 2. vcpkg.
if not defined VCPKG_ROOT ( echo   ERROR: VCPKG_ROOT is not set. See README.md ^(Build^). & pause & exit /b 1 )
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ( echo   ERROR: VCPKG_ROOT is not a vcpkg checkout. & pause & exit /b 1 )
"%VCPKG_ROOT%\vcpkg.exe" x-update-baseline --add-initial-baseline >nul 2>&1

:: Each stage streams LIVE to the console AND is captured to build_log.txt via PowerShell
:: Tee-Object (a cmdlet, so %errorlevel% keeps cmake's real exit code). No frozen-looking window.

:: 3. Configure (normal dynamic-Qt release preset).
echo [2/5] Configuring release build...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --preset windows-msvc-release 2>&1 | Tee-Object -FilePath '%~dp0build_log.txt'; exit $LASTEXITCODE"
set "RC=%errorlevel%"
if not "%RC%"=="0" (
    findstr /i /c:"CMake Error" /c:"error" /c:"Failed to find" /c:"FAILED" "%~dp0build_log.txt" > "%~dp0build_errors.txt"
    echo   CONFIGURE FAILED ^(see build_errors.txt^). & pause & exit /b 1
)

:: 4. Build.
echo [3/5] Building (LIVE output; the two big files can take a few minutes each)...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --build --preset release 2>&1 | Tee-Object -FilePath '%~dp0build_log.txt'; exit $LASTEXITCODE"
set "RC=%errorlevel%"
findstr /i /c:"error C" /c:": error" /c:"error LNK" /c:"fatal error" /c:"FAILED" /c:"ninja: build stopped" "%~dp0build_log.txt" > "%~dp0build_errors.txt"
if not "%RC%"=="0" ( echo   BUILD FAILED ^(see build_errors.txt^). & pause & exit /b 1 )

:: 5. Install into the dist folder. The CMake install copies the Qt6 DLLs + the plugin
::    families the app loads next to the exe — by hand, because this vcpkg Qt ships no
::    windeployqt.exe (only the qmake .prf). See the elseif(WIN32) block in CMakeLists.txt.
echo [4/5] Deploying Qt runtime into dist\D4AssetBrowser ...
set "OUT=%~dp0dist\D4AssetBrowser"
if exist "%OUT%" rmdir /s /q "%OUT%"
cmake --install "%~dp0build\release" --prefix "%OUT%" > "%~dp0build_log.txt" 2>&1
set "RC=%errorlevel%"
type "%~dp0build_log.txt"
if not "%RC%"=="0" ( echo   DEPLOY FAILED ^(see build_log.txt^). & pause & exit /b 1 )

:: Verify the deploy actually produced a runnable tree. Without platforms\qwindows.dll the exe
:: dies at startup with "no Qt platform plugin could be initialized" — and that would only be
:: discovered by whoever downloads the release, which is the worst possible place to find it.
if not exist "%OUT%\D4AssetBrowser.exe" (
    echo   [X] No exe in %OUT% - the install step produced nothing.
    pause & exit /b 1
)
if not exist "%OUT%\platforms\qwindows.dll" (
    echo   [X] platforms\qwindows.dll is MISSING from %OUT%.
    echo       The zip would build fine and fail to START on every machine.
    pause & exit /b 1
)
if not exist "%OUT%\Qt6Core.dll" (
    echo   [X] Qt6Core.dll is missing from %OUT% - Qt runtime was not copied.
    pause & exit /b 1
)
echo   Deploy verified: exe + Qt6Core.dll + platforms\qwindows.dll present.
if not exist "%OUT%\D4AssetBrowser.exe" ( echo   ERROR: exe missing in "%OUT%". & pause & exit /b 1 )
if exist "%~dp0RELEASE_README.txt" copy /y "%~dp0RELEASE_README.txt" "%OUT%\README.txt" >nul

:: 6. Zip it — version-stamped from main.cpp's setApplicationVersion, so releases don't overwrite
::    each other and a download's filename says what it is.
echo [5/5] Zipping...
set "APPVER="
for /f tokens^=2^ delims^=^" %%v in ('findstr /c:"setApplicationVersion" "%~dp0src\main.cpp"') do set "APPVER=%%v"
if "%APPVER%"=="" set "APPVER=dev"
set "ZIP=%~dp0dist\D4AssetBrowser_v%APPVER%.zip"
powershell -NoProfile -Command "Compress-Archive -Force -Path '%OUT%' -DestinationPath '%ZIP%'"

echo.
echo ============================================================
echo  RELEASE OK.  (v%APPVER%)
echo   folder : %OUT%\   (exe + Qt DLLs/plugins, self-contained)
echo   zip    : %ZIP%
echo  Unzip anywhere and run D4AssetBrowser.exe. On first run it downloads
echo  d4data into data\d4data (or point it at your own in Settings).
echo ============================================================
pause
