@echo off
setlocal
cd /d "%~dp0"
:: ---------------------------------------------------------------------------
::  Is a candidate TACT key list actually Diablo IV's?
::
::  Community key dumps are published per-product and get mixed up constantly --
::  CascLib's KeyService.cs, the obvious place to look, is Overwatch + WoW keys.
::  Guessing is unnecessary: the install ships one
::  base/EncryptedNameDict-0x<keyName>.dat per key, encrypted WITH that key, and
::  a correct key decodes it to a 0xABCD4567 header. So a key belongs to D4, and
::  to us, exactly when its dict decodes.
::
::  Usage:   "Verify TACT Keys.bat" path\to\candidate_keys.txt
::  Format:  KEYNAME(16 hex) <space> KEYVALUE(32 hex), one per line -- the wowdev
::           TACTKeys format, same as data\d4_tact_keys_clean.txt.
::
::  Reports, per key: VALID (+ how many SNOs it names), "no dict in this build"
::  (not a D4 key, or unused by this patch), or "present but did NOT decode"
::  (right key name, wrong key value).
::
::  Output: the app log + data\tact_key_verify.txt
:: ---------------------------------------------------------------------------

set "KEYS=%~1"
if "%KEYS%"=="" (
    echo Usage: "Verify TACT Keys.bat" ^<candidate_keys.txt^>
    echo.
    echo   Verified keys are registered, so anything that checks out is usable
    echo   immediately -- and every valid key names more encrypted assets.
    pause & exit /b 1
)
if not exist "%KEYS%" ( echo   ERROR: no such file: %KEYS% & pause & exit /b 1 )

if not exist "build\release\D4AssetBrowser.exe" (
    echo   ERROR: build\release\D4AssetBrowser.exe not found -- build first.
    pause & exit /b 1
)

echo Verifying "%KEYS%" against this game build's EncryptedNameDict files...
echo.
set "D4_VERIFY_KEYS=%KEYS%"
start /wait "" "build\release\D4AssetBrowser.exe"
set "D4_VERIFY_KEYS="

echo.
if exist "build\release\data\tact_key_verify.txt" (
    type "build\release\data\tact_key_verify.txt"
) else (
    echo   No report written -- did the app reach a loaded CASC? See data\D4AssetBrowser.log
)
echo.
pause
