@echo off
setlocal
cd /d "%~dp0"
:: ---------------------------------------------------------------------------
:: Tidy the project root: park generated files, file working notes under docs\,
:: and group the occasional .bat scripts behind category prefixes.
::
:: Shows the plan first and changes nothing. It asks before applying, and the
:: same script re-run later is a no-op, so it is safe to run twice.
:: ---------------------------------------------------------------------------
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0organize-folder.ps1"
if errorlevel 1 ( echo. & echo Dry run failed - nothing was changed. & pause & exit /b 1 )

echo.
choice /c YN /m "Apply these changes"
if errorlevel 2 ( echo. & echo Cancelled - nothing was changed. & pause & exit /b 0 )

echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0organize-folder.ps1" -Apply
echo.
echo Review with:  git status
pause
