@echo off
REM Test - Icon Audit Compare.bat
REM
REM Answers ONE question: is the icon audit checking the tool, or checking a copy of its own
REM reference?
REM
REM The appearance crawl's last step copies diablo4.dad's per-class handles over the ones the tool
REM worked out for itself. The audit then compares the tool against diablo4.dad and reports 0 diffs
REM - which it cannot help doing, because both sides came from the same place. If the tool's own
REM route broke after a game patch, this would still read 0 and nothing would say otherwise.
REM
REM D4_NO_DAD_FORCE=1 skips that step. The DIFF count from that run is the real answer: how many
REM appearances the tool resolves differently on its own. This script takes both readings and puts
REM them side by side.
REM
REM WHAT IT COSTS: two launches, and the appearance index rebuilds once each way - the flag is
REM folded into the cache signature on purpose, so a diagnostic run can never leave its results in
REM the cache a normal run would read. Expect a few minutes per pass.
REM
REM WHAT YOU DO: let each launch sit until the status bar stops reporting progress - the audit runs
REM by itself once all three indexes are up - then close the window. The script does the rest.
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "EXE=build\release\D4AssetBrowser.exe"
set "DATA=build\release\data"
set "AUDIT=%DATA%\icon_audit.txt"
set "A_FORCED=%DATA%\icon_audit_dadforced.txt"
set "A_FREE=%DATA%\icon_audit_nodadforce.txt"

if not exist "%EXE%" (
    echo D4AssetBrowser.exe not found - build it first:  rebuild.bat
    pause & exit /b 1
)

echo ============================================================
echo  PASS 1 of 2 - normal run ^(diablo4.dad handles forced^)
echo ============================================================
echo.
echo Let the app finish indexing, then close it.
echo.
del /q "%AUDIT%" >nul 2>&1
set "D4_NO_DAD_FORCE="
start /wait "" "%EXE%"
if not exist "%AUDIT%" (
    echo.
    echo No audit was written. It runs once all three indexes are ready - if you closed the
    echo window early, run this again and give it longer.
    pause & exit /b 1
)
copy /y "%AUDIT%" "%A_FORCED%" >nul
echo   captured: %A_FORCED%
echo.

echo ============================================================
echo  PASS 2 of 2 - D4_NO_DAD_FORCE=1 ^(the tool's own answer^)
echo ============================================================
echo.
echo The appearance index rebuilds from scratch this time, so it takes longer.
echo Same again: wait for indexing to finish, then close the app.
echo.
del /q "%AUDIT%" >nul 2>&1
set "D4_NO_DAD_FORCE=1"
start /wait "" "%EXE%"
set "D4_NO_DAD_FORCE="
if not exist "%AUDIT%" (
    echo.
    echo No audit was written on the second pass. The first reading is still at %A_FORCED%.
    pause & exit /b 1
)
copy /y "%AUDIT%" "%A_FREE%" >nul
echo   captured: %A_FREE%
echo.

echo ============================================================
echo  RESULT
echo ============================================================
echo.
echo -- forced ^(what you normally see^) ------------------------
findstr /b /c:"Icon audit:" "%A_FORCED%"
findstr /c:"rows:" "%A_FORCED%"
findstr /c:"PORTRAIT:" "%A_FORCED%"
echo.
echo -- not forced ^(the tool on its own^) ----------------------
findstr /b /c:"Icon audit:" "%A_FREE%"
findstr /c:"rows:" "%A_FREE%"
findstr /c:"PORTRAIT:" "%A_FREE%"
echo.
echo Read the DIFF count on the second line. 0 means the tool already agrees with diablo4.dad
echo without being told to, and the forcing step is dead weight. A large number means the two
echo genuinely disagree, and the DIFF samples further down %A_FREE% name which appearances -
echo check a few by hand before deciding which side is right.
echo.
echo Both readings are kept, so you can diff them directly:
echo   fc "%A_FORCED%" "%A_FREE%"
echo.
echo The Catalogue numbers should be IDENTICAL between the two passes - they do not depend on
echo diablo4.dad at all. If they moved, something else changed between the runs.
echo.
pause
