@echo off
REM Automated 3D-icon render benchmark, with an A/B on the read-ahead.
REM
REM Three launches, all self-driving:
REM   1. warm-up      - discarded, so the OS file cache is in the SAME state for both measured
REM                     runs. Without this the first run pays for cold reads and the second does
REM                     not, which is exactly the confound that made the last comparison useless.
REM   2. baseline     - D4_ICONPERF_NOPREFETCH=1: reads happen inline on the GUI thread
REM   3. read-ahead   - the shipping path: reads run a chunk ahead on worker threads
REM
REM Same binary, same models, same cache state - the only difference between 2 and 3 is where the
REM reads happen. Compare the "total" and "CASC read (waited)" lines.
setlocal
cd /d "%~dp0"

set "EXE=build\release\D4AssetBrowser.exe"
if not exist "%EXE%" (
    echo D4AssetBrowser.exe not found - build it first:  rebuild.bat
    pause & exit /b 1
)

set "N=%~1"
if "%N%"=="" set "N=150"
set "DATA=build\release\data"
set D4_DUMP_ICONPERF=1
set D4_ICONPERF_AUTO=%N%

echo [1/3] Warm-up (discarded)...
set D4_ICONPERF_NOPREFETCH=1
"%EXE%" >nul 2>&1

echo [2/3] Baseline - read-ahead OFF...
set D4_ICONPERF_NOPREFETCH=1
del /q "%DATA%\icon_perf.txt" >nul 2>&1
"%EXE%"
if exist "%DATA%\icon_perf.txt" move /y "%DATA%\icon_perf.txt" "%DATA%\icon_perf_baseline.txt" >nul

echo [3/3] Read-ahead ON...
set D4_ICONPERF_NOPREFETCH=
del /q "%DATA%\icon_perf.txt" >nul 2>&1
"%EXE%"
if exist "%DATA%\icon_perf.txt" move /y "%DATA%\icon_perf.txt" "%DATA%\icon_perf_prefetch.txt" >nul

echo.
echo ================= BASELINE (read-ahead OFF) =================
if exist "%DATA%\icon_perf_baseline.txt" (type "%DATA%\icon_perf_baseline.txt") else (echo not written)
echo.
echo ================= READ-AHEAD ON =============================
if exist "%DATA%\icon_perf_prefetch.txt" (type "%DATA%\icon_perf_prefetch.txt") else (echo not written)
echo.
echo Both files are in %DATA%\.
pause
endlocal
