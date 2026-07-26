@echo off
setlocal enabledelayedexpansion
title Build Diablo4AssetBrowserNative
cd /d "%~dp0"

:: Close any running instance so the linker can overwrite the .exe (avoids LNK1104).
taskkill /im D4AssetBrowser.exe /f >nul 2>&1

echo ============================================================
echo  Diablo4AssetBrowserNative - build
echo  (native C++/Qt6 - first build compiles Qt6 via vcpkg, slow)
echo ============================================================
echo.

:: 1. Make sure MSVC (cl.exe / cmake / ninja) is on PATH; if not, run vcvars64.
where cl >nul 2>&1
if errorlevel 1 (
    echo [1/4] Initializing Visual Studio 2022 build tools...
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo   ERROR: Visual Studio 2022 not found.
        echo   Install "Visual Studio 2022" with the "Desktop development with C++" workload.
        pause & exit /b 1
    )
    set "VSPATH="
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    if not defined VSPATH (
        echo   ERROR: MSVC C++ tools not found in your VS install.
        pause & exit /b 1
    )
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat"
    if errorlevel 1 ( echo   ERROR: vcvars64 failed. & pause & exit /b 1 )
) else (
    echo [1/4] MSVC already on PATH.
)

:: 2. vcpkg.
if not defined VCPKG_ROOT (
    echo   ERROR: VCPKG_ROOT is not set. One-time setup:
    echo       git clone https://github.com/microsoft/vcpkg
    echo       .\vcpkg\bootstrap-vcpkg.bat
    echo       setx VCPKG_ROOT C:\path\to\vcpkg
    echo   ^(then open a NEW terminal and re-run build.bat^)
    pause & exit /b 1
)
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    echo   ERROR: VCPKG_ROOT="%VCPKG_ROOT%" is not a vcpkg checkout.
    pause & exit /b 1
)

:: 3. Pin a vcpkg baseline matching your vcpkg checkout (required for manifest mode).
::    Writes "builtin-baseline" into vcpkg.json using your local vcpkg HEAD (no network).
echo [2/5] Pinning vcpkg baseline...
"%VCPKG_ROOT%\vcpkg.exe" x-update-baseline --add-initial-baseline
if errorlevel 1 (
    echo   WARNING: could not set the baseline automatically. Run it yourself:
    echo       "%VCPKG_ROOT%\vcpkg" x-update-baseline --add-initial-baseline
)

:: 4. Configure (pulls Qt6, CascLib, fastgltf, tinygltf) and build. Both steps are
::    tee'd to build_log.txt, with the error lines distilled into a small
::    build_errors.txt (the full log gets truncated when read back through the
::    sandbox mount; the compact file stays readable).
:: Both steps stream LIVE to the console AND capture to build_log.txt via PowerShell Tee-Object
:: (a cmdlet, so %errorlevel% keeps cmake's real exit code). No silent/frozen-looking window.
echo [3/5] Configuring (vcpkg dependency build can take a long time the first time)...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --preset windows-msvc-release 2>&1 | Tee-Object -FilePath '%~dp0build_log.txt'; exit $LASTEXITCODE"
set "RC=%errorlevel%"
if not "%RC%"=="0" (
    findstr /i /c:"CMake Error" /c:"error" /c:"Failed to find" /c:"FAILED" "%~dp0build_log.txt" > "%~dp0build_errors.txt"
    echo   CONFIGURE FAILED ^(see build_errors.txt^). & pause & exit /b 1
)

echo [4/4] Building (LIVE output; the two big files can take a few minutes each)...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --build --preset release 2>&1 | Tee-Object -FilePath '%~dp0build_log.txt'; exit $LASTEXITCODE"
set "RC=%errorlevel%"
findstr /i /c:"error C" /c:": error" /c:"error LNK" /c:"fatal error" /c:"FAILED" /c:"ninja: build stopped" "%~dp0build_log.txt" > "%~dp0build_errors.txt"
if not "%RC%"=="0" ( echo   BUILD FAILED ^(see build_errors.txt^). & pause & exit /b 1 )

echo.
echo ============================================================
echo  BUILD OK.  Launch with:  run.bat
echo  (run.bat deploys the Qt DLLs + plugins next to the exe.)
echo ============================================================
pause
