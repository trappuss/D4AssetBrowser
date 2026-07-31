@echo off
setlocal enabledelayedexpansion
title d4cloth - build + corpus extraction
cd /d "%~dp0"

echo ============================================================
echo  d4cloth - cloth physics diagnostic harness
echo  1) build d4cloth.exe (vcpkg cache hit - no Qt rebuild)
echo  2) extract the test-matrix corpus from the game CASC
echo ============================================================
echo.

:: 1. MSVC on PATH (same bootstrap as build.bat).
where cl >nul 2>&1
if errorlevel 1 (
    echo [1/3] Initializing Visual Studio 2022 build tools...
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" ( echo   ERROR: Visual Studio 2022 not found. & pause & exit /b 1 )
    set "VSPATH="
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    if not defined VSPATH ( echo   ERROR: MSVC C++ tools not found. & pause & exit /b 1 )
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat"
    if errorlevel 1 ( echo   ERROR: vcvars64 failed. & pause & exit /b 1 )
) else (
    echo [1/3] MSVC already on PATH.
)

if not defined VCPKG_ROOT (
    echo   ERROR: VCPKG_ROOT is not set ^(see build.bat for one-time setup^).
    pause & exit /b 1
)

:: 2. Configure + build the standalone d4cloth target (own build dir; the app build is untouched).
echo [2/3] Configuring + building d4cloth...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "cmake -S tools/d4cloth -B tools/d4cloth/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE='%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake' -DVCPKG_TARGET_TRIPLET=x64-windows '-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=C:/Users/notso/vbt' 2>&1 | Tee-Object -FilePath '%~dp0d4cloth_build_log.txt'; exit $LASTEXITCODE"
if not "%errorlevel%"=="0" (
    findstr /i /c:"CMake Error" /c:"error" /c:"FAILED" "%~dp0d4cloth_build_log.txt" > "%~dp0d4cloth_build_errors.txt"
    echo   CONFIGURE FAILED ^(see d4cloth_build_errors.txt^). & pause & exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "cmake --build tools/d4cloth/build 2>&1 | Tee-Object -Append -FilePath '%~dp0d4cloth_build_log.txt'; exit $LASTEXITCODE"
if not "%errorlevel%"=="0" (
    findstr /i /c:"error C" /c:": error" /c:"error LNK" /c:"fatal error" /c:"FAILED" "%~dp0d4cloth_build_log.txt" > "%~dp0d4cloth_build_errors.txt"
    echo   BUILD FAILED ^(see d4cloth_build_errors.txt^). & pause & exit /b 1
)

:: 3. Extract the corpus. The Qt DLLs live in the build dir's vcpkg tree; put them on PATH.
echo [3/3] Extracting the test-matrix corpus from CASC...
set "PATH=%~dp0tools\d4cloth\build\vcpkg_installed\x64-windows\bin;%PATH%"
set "D4DATA=%APPDATA%\Diablo4AssetBrowser\D4AssetBrowser\d4data"
tools\d4cloth\build\d4cloth.exe extract ^
    --casc "G:\G Games\Diablo IV" ^
    --d4data "%D4DATA%" ^
    --cases tools\d4cloth\cases.json ^
    --out tools\d4cloth\corpus ^
    2>&1 | powershell -NoProfile -Command "$input | Tee-Object -FilePath '%~dp0d4cloth_extract_log.txt'"

echo.
echo ============================================================
echo  Done. Corpus at tools\d4cloth\corpus
echo  Log: d4cloth_extract_log.txt
echo ============================================================
pause
