@echo off
setlocal enabledelayedexpansion
title d4shader - build + shader feasibility probe
cd /d "%~dp0"

echo ============================================================
echo  d4shader - can the game's own shaders be extracted and run?
echo  1) build d4shader.exe (vcpkg cache hit - no Qt rebuild)
echo  2) probe SNO groups 107 Shader / 108 ShaderMap in CASC
echo ============================================================
echo.
echo  Read-only. Reads the game INSTALL. Does not touch the
echo  running game, attach to any process, or hook a device.
echo.

:: 1. MSVC on PATH (same bootstrap as build.bat / the d4cloth harness).
where cl >nul 2>&1
if errorlevel 1 (
    echo [1/3] Initializing Visual Studio build tools...
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" ( echo   ERROR: Visual Studio not found. & pause & exit /b 1 )
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

:: 2. Configure + build the standalone target. Own build dir; the app build is untouched.
echo [2/3] Configuring + building d4shader...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "cmake -S tools/d4shader -B tools/d4shader/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE='%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake' -DVCPKG_TARGET_TRIPLET=x64-windows '-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=%USERPROFILE%/vbt' 2>&1 | Tee-Object -FilePath '%~dp0d4shader_build_log.txt'; exit $LASTEXITCODE"
if not "%errorlevel%"=="0" (
    REM Select-String, not findstr: Tee-Object writes UTF-16, which findstr cannot read - it
    REM warns and produces an EMPTY error file, so a failed build reports no errors at all.
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$e = Select-String -Path '%~dp0d4shader_build_log.txt' -Pattern 'CMake Error','error','FAILED' | ForEach-Object { $_.Line } | Select-Object -First 40 ; $e; $e | Out-File -FilePath '%~dp0d4shader_build_errors.txt' -Encoding utf8"
    echo   CONFIGURE FAILED ^(see d4shader_build_errors.txt^). & pause & exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "cmake --build tools/d4shader/build 2>&1 | Tee-Object -Append -FilePath '%~dp0d4shader_build_log.txt'; exit $LASTEXITCODE"
if not "%errorlevel%"=="0" (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$e = Select-String -Path '%~dp0d4shader_build_log.txt' -Pattern 'error C',': error','error LNK','fatal error','FAILED' | ForEach-Object { $_.Line } | Select-Object -First 40 ; $e; $e | Out-File -FilePath '%~dp0d4shader_build_errors.txt' -Encoding utf8"
    echo   BUILD FAILED ^(see d4shader_build_errors.txt^). & pause & exit /b 1
)

:: 3. Probe. Game folder is read from the app's own setting, so this follows a moved install.
echo [3/3] Probing groups 107 / 108...
set "PATH=%~dp0tools\d4shader\build\vcpkg_installed\x64-windows\bin;%PATH%"
set "GAMEDIR="
for /f "usebackq tokens=1,* delims==" %%a in (`findstr /i "^gameDir=" "%~dp0build\release\data\D4AssetBrowser\D4AssetBrowser.ini" 2^>nul`) do set "GAMEDIR=%%b"
if not defined GAMEDIR set "GAMEDIR=G:\G Games\Diablo IV"
REM Both read from the app's OWN settings file - data\D4AssetBrowser\D4AssetBrowser.ini,
REM which is where QSettings actually writes (the loose D4AssetBrowser.ini one level up is a
REM stale fragment with no [paths] section at all). Verified keys: [paths] gameDir=,
REM [casc] product=. The literals below are only a last resort if the file is missing.
REM Same product the app opens with (casc/product, default fenris). Matters on a machine with
REM more than one active .build.info row - retail beside a PTR - where "first active row" is
REM not necessarily the one the app reads.
set "PRODUCT="
for /f "usebackq tokens=1,* delims==" %%a in (`findstr /i "^product=" "%~dp0build\release\data\D4AssetBrowser\D4AssetBrowser.ini" 2^>nul`) do set "PRODUCT=%%b"
if not defined PRODUCT set "PRODUCT=fenris"
REM d4data, for the hardpoint dump below - it reads json/base/meta/Appearance/<name>.app.json,
REM which sits deep enough that nothing off this machine can open it.
set "D4DATA="
for /f "usebackq tokens=1,* delims==" %%a in (`findstr /i "^d4dataDir=" "%~dp0build\release\data\D4AssetBrowser\D4AssetBrowser.ini" 2^>nul`) do set "D4DATA=%%b"
if not defined D4DATA set "D4DATA=%~dp0build\release\data\d4data"
echo   game: !GAMEDIR!   product: !PRODUCT!
echo   d4data: !D4DATA!
if not exist "Claude outputs" mkdir "Claude outputs"

tools\d4shader\build\d4shader.exe ^
    --casc "!GAMEDIR!" ^
    --product "!PRODUCT!" ^
    --out "Claude outputs\shader_probe.txt" ^
    --dump "tools\d4shader\out" ^
    --dump-n 6 ^
    --group 107,108 ^
    --census "Claude outputs\sno_group_census.tsv" ^
    --d4data "!D4DATA!" ^
    --hardpoints "palM_P00,palF_P00,barM_P00,barF_P00,necM_P00,necF_P00" ^
    > "Claude outputs\shader_probe_console.txt" 2>&1
type "Claude outputs\shader_probe_console.txt"

echo.
echo ============================================================
echo  Done. Report: Claude outputs\shader_probe.txt
echo  Every record name: Claude outputs\shader_probe.txt.records.tsv
echo  Group census:      Claude outputs\sno_group_census.tsv
echo  Every group name:  Claude outputs\sno_group_census.tsv.names.tsv
echo  Console (incl. hardpoints): Claude outputs\shader_probe_console.txt
echo  Sample blobs: tools\d4shader\out
echo ============================================================
pause
