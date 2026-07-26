@echo off
setlocal enabledelayedexpansion
title Rebuild Diablo4AssetBrowserNative (fast)
cd /d "%~dp0"

:: Close any running instance so the linker can overwrite the .exe (avoids LNK1104).
taskkill /im D4AssetBrowser.exe /f >nul 2>&1

:: Snapshot the source into ..\.Backups before building (recovery insurance against a
:: bad edit/refactor/truncation). Best-effort: never blocks the build.
call "%~dp0backup-src.bat"

:: Pre-build source checks (verify-src.py): delimiter balance, MISSING #include for the
:: header-only helpers, printf/qInfo format-vs-arg mismatches, Qt macro name collisions.
:: These are cheap and catch the mistakes that otherwise cost a full MSVC cycle to discover.
:: Non-blocking by design: a checker false positive must never stop you building.
where python >nul 2>&1
if not errorlevel 1 (
    python "%~dp0verify-src.py" --quiet
    if errorlevel 1 (
        echo.
        echo  ^>^> verify-src found problems ^(listed above^). Building anyway - Ctrl+C to stop.
        echo.
    )
)

:: Fast incremental rebuild: recompiles only changed source files and relinks.
:: Does NOT rebuild vcpkg dependencies (Qt6 etc.) — those are already cached.
:: Use this for the edit -> test loop; use build.bat only for the first build or
:: after changing vcpkg.json dependencies.

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

:: Build with LIVE output on the console AND capture it to build_log.txt at the same time.
:: PowerShell's Tee-Object does both; it is a cmdlet (not a native exe), so %errorlevel% keeps
:: cmake's real exit code (cmake.exe is the last native process in the pipe). This is what stops
:: the window from looking frozen: ninja prints each finished step as a line as it goes.
echo Building (LIVE output below; the two big files can take a few minutes each)...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --build --preset release 2>&1 | Tee-Object -FilePath '%~dp0build_log.txt'; exit $LASTEXITCODE"
set "RC=%errorlevel%"

:: Distill just the error lines into a SMALL file so the exact compiler error stays readable.
findstr /i /c:"error C" /c:": error" /c:"error LNK" /c:"fatal error" /c:"FAILED" /c:"ninja: build stopped" /c:"BUILD FAILED" "%~dp0build_log.txt" > "%~dp0build_errors.txt"
if not "%RC%"=="0" (
    echo.
    echo BUILD FAILED - full output is above and in build_log.txt ^(errors in build_errors.txt^)
    pause & exit /b 1
)

echo.
echo Done. Deploying + launching...
call "%~dp0run.bat"
