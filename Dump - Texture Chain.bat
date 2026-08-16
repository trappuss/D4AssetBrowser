@echo off
REM Walk one or more texture SNOs through every stage of the decode and write the result to the log.
REM
REM Defaults to druM_stor235_HLM's textures - the case that has resisted several fixes. Pass your own
REM comma-separated SNOs as the first argument to probe something else.
REM
REM The tool starts normally; the report is in data\D4AssetBrowser.log, tagged "dump-tex".
setlocal
set SNOS=%~1
if "%SNOS%"=="" set SNOS=2334281,2334282,2334283,2334284,2334285,2334287,1662692

set D4_DUMP_TEX=%SNOS%
set D4_DUMP_MAT=1
echo Probing texture SNOs: %SNOS%
echo.
REM NOT "start": run it attached so a fault that kills the process before the log opens still
REM prints here. A detached launch is exactly why a startup crash looked like an empty log.
"%~dp0build\release\D4AssetBrowser.exe"
echo.
echo Exit code: %ERRORLEVEL%   (0 = clean, anything else = fault)
echo Now read data\D4AssetBrowser.log - look for "dump-tex", "mat-resolve", "mat-meta".
pause
endlocal
