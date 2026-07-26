@echo off
setlocal enabledelayedexpansion
title CLEAN Rebuild Diablo4AssetBrowserNative
cd /d "%~dp0"

:: ---------------------------------------------------------------------------
:: GUARANTEED-CLEAN rebuild. Wipes every compiled object first, so EVERY source
:: file is recompiled against the CURRENT headers (no stale-object / ABI mismatch).
:: ---------------------------------------------------------------------------

:: Close any running instance so the linker can overwrite the .exe (avoids LNK1104).
taskkill /im D4AssetBrowser.exe /f >nul 2>&1

:: Snapshot the source into ..\.Backups before building (recovery insurance).
call "%~dp0backup-src.bat"

:: Make sure MSVC (cl.exe / cmake / ninja) is on PATH; if not, run vcvars64.
where cl >nul 2>&1
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    set "VSPATH="
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    if not defined VSPATH ( echo ERROR: Visual Studio 2022 C++ tools not found. & pause & exit /b 1 )
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
)

if not exist "build\release\CMakeCache.txt" (
    echo No build yet - run build.bat first ^(it does the one-time dependency build^).
    pause & exit /b 1
)

echo [1/2] Wiping all compiled objects (ninja clean)...
cmake --build --preset release --target clean >nul 2>&1

:: Full recompile with LIVE output on the console AND captured to build_log.txt at the same
:: time (PowerShell Tee-Object; a cmdlet, so %errorlevel% keeps cmake's real exit code).
echo [2/2] Full recompile of every source file (LIVE output; this takes several minutes)...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --build --preset release 2>&1 | Tee-Object -FilePath '%~dp0build_log.txt'; exit $LASTEXITCODE"
set "RC=%errorlevel%"

:: Distill just the error lines into a small file so the exact compiler error stays readable.
findstr /i /c:"error C" /c:": error" /c:"error LNK" /c:"fatal error" /c:"FAILED" /c:"ninja: build stopped" /c:"BUILD FAILED" "%~dp0build_log.txt" > "%~dp0build_errors.txt"
if not "%RC%"=="0" (
    echo.
    echo BUILD FAILED - full output saved to build_log.txt ^(errors in build_errors.txt^)
    pause & exit /b 1
)

echo.
echo Clean build OK. Deploying + launching...
call "%~dp0run.bat"
