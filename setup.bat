@echo off
setlocal enabledelayedexpansion
title D4AssetBrowser - setup, build, and run
cd /d "%~dp0"

echo ============================================================
echo   D4AssetBrowser - one-time setup / recovery
echo   Checks tools, sets up vcpkg, wipes stale cache, builds
echo   Qt6, and launches the app. Run this after a PC reset, a
echo   Visual Studio upgrade, or a fresh checkout.
echo   ^(For day-to-day work use rebuild.bat instead.^)
echo ============================================================
echo.
set /a MISSING=0

echo --- Checking required tools --------------------------------
:: Visual Studio C++ toolset (via vswhere)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSPATH=%%i"
if defined VSPATH (
    echo [ OK ] Visual Studio C++ tools ^> !VSPATH!
    echo [ OK ] CMake + Ninja ^(bundled with the VS C++ workload^)
) else (
    echo [MISS] Visual Studio with "Desktop development with C++".
    echo        Install: https://visualstudio.microsoft.com/downloads/  ^(tick that workload^)
    set /a MISSING+=1
)
:: git
where git >nul 2>&1
if %errorlevel%==0 ( echo [ OK ] git ) else (
    echo [MISS] git   ^(winget install --id Git.Git -e  /  https://git-scm.com/download/win^)
    set /a MISSING+=1
)
:: curl (optional - only for the TACT-keys download)
where curl >nul 2>&1
if %errorlevel%==0 ( echo [ OK ] curl ) else ( echo [WARN] curl not found ^(only needed for the in-app TACT-keys download^) )

if not %MISSING%==0 (
    echo.
    echo   %MISSING% required tool^(s^) missing. Install the [MISS] items above,
    echo   open a NEW terminal, and run setup.bat again.
    pause & exit /b 1
)

echo.
echo --- vcpkg ---------------------------------------------------
if defined VCPKG_ROOT if exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    echo [ OK ] vcpkg ^> %VCPKG_ROOT%
    goto vcpkg_ok
)
echo Setting up a standalone vcpkg at C:\vcpkg ^(most reliable^)...
if not exist "C:\vcpkg\.git"    git clone https://github.com/microsoft/vcpkg C:\vcpkg
if not exist "C:\vcpkg\vcpkg.exe" call C:\vcpkg\bootstrap-vcpkg.bat
if not exist "C:\vcpkg\vcpkg.exe" (
    echo   ERROR: vcpkg bootstrap failed ^(check internet / antivirus^), then run
    echo          C:\vcpkg\bootstrap-vcpkg.bat  manually and read its output.
    pause & exit /b 1
)
setx VCPKG_ROOT C:\vcpkg >nul
set "VCPKG_ROOT=C:\vcpkg"
echo [ OK ] vcpkg ready ^(C:\vcpkg^)
:vcpkg_ok

echo.
echo --- Cleaning any stale build cache -------------------------
:: A cache from a different machine / VS version points CMake at compilers that no
:: longer exist. Wipe the cache + the old compiled deps so the configure is clean.
:: This forces a one-time Qt6 rebuild ^(slow^). Files you placed next to the exe
:: ^(e.g. 2D_table.dat^) are preserved.
if exist "build\release\CMakeCache.txt"  del /q "build\release\CMakeCache.txt"
if exist "build\release\CMakeFiles"       rmdir /s /q "build\release\CMakeFiles"
if exist "build\release\vcpkg_installed"  rmdir /s /q "build\release\vcpkg_installed"
echo       done.

echo.
echo --- Building ^(first Qt6 build is slow: 30-90 min, several GB^) ---
echo.
call "%~dp0build.bat"
if errorlevel 1 (
    echo.
    echo   Build failed - see build_errors.txt. Fix the error and run setup.bat
    echo   ^(or rebuild.bat once the environment is good^) again.
    pause & exit /b 1
)

echo.
echo ============================================================
echo   Build succeeded - launching Diablo4AssetBrowser...
echo ============================================================
call "%~dp0run.bat"
