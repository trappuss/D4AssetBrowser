@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"
title Release - Finish Publish

:: ===========================================================================
::  Release - Finish Publish
::
::  Two jobs that github.bat does not do, both of which bit 2.3.0:
::
::    1. Rewrite the GitHub release body from the matching CHANGELOG.md
::       section. github.bat only writes the body when it CREATES the release,
::       and the release workflow usually gets there first, so a hand-run
::       uploaded the zip onto a release that kept the workflow's generated
::       notes.
::
::    2. Push wiki\ to the GitHub wiki, which is a separate repository that
::       github.bat has never touched at all.
::
::  Safe to run twice. Nothing here deletes a tag, a release or a branch.
:: ===========================================================================

echo.
echo   ===============================================================
echo    Release - Finish Publish
echo   ===============================================================
echo.

:: ------------------------------------------------------------------ preflight
:: Never git-init. A missing .git means this was launched from the wrong place,
:: and the right answer is to stop rather than to create a repository here.
if not exist ".git" (
    echo   [X] No .git in this folder.
    echo       "%~dp0"
    echo       Keep this file in the repository folder and run it from there.
    goto :fail
)

call :need git    || goto :fail
call :need gh     || goto :fail
call :need python || goto :fail

gh auth status >nul 2>&1
if errorlevel 1 (
    echo   [X] gh is not signed in.
    echo       Run:  gh auth login -h github.com
    echo       Then check it took:  gh api user
    goto :fail
)

:: The version in the exe is the single source of truth, same as github.bat.
set "APPVER="
for /f "tokens=2 delims=()" %%V in ('findstr /c:"setApplicationVersion" "src\main.cpp"') do set "APPVER=%%~V"
if not defined APPVER (
    echo   [X] Could not read the version from src\main.cpp.
    goto :fail
)

:: Identity comes from the signed-in account, so the noreply address is the
:: real one rather than a guess at the format.
set "GHLOGIN="
set "GHID="
set "REPOURL="
set "REPONAME="
for /f "delims=" %%I in ('gh api user --jq .login 2^>nul')          do set "GHLOGIN=%%I"
for /f "delims=" %%I in ('gh api user --jq .id 2^>nul')             do set "GHID=%%I"
for /f "delims=" %%I in ('gh repo view --json url --jq .url 2^>nul')  do set "REPOURL=%%I"
for /f "delims=" %%I in ('gh repo view --json name --jq .name 2^>nul') do set "REPONAME=%%I"
if not defined GHLOGIN  ( echo   [X] gh could not read your account. & goto :fail )
if not defined GHID     ( echo   [X] gh could not read your account id. & goto :fail )
if not defined REPOURL  ( echo   [X] gh could not identify this repository. & goto :fail )
if not defined REPONAME ( echo   [X] gh could not identify this repository. & goto :fail )
set "NOREPLY=!GHID!+!GHLOGIN!@users.noreply.github.com"

echo     version    : !APPVER!
echo     account    : !GHLOGIN!
echo     commit as  : !NOREPLY!
echo     repository : !REPOURL!
echo.

:: ============================================================ 1. release body
echo   --- 1. release body -------------------------------------------

if not exist "dist" md "dist"
set "NOTES=%~dp0dist\RELEASE_BODY.md"
:: Delete first. A leftover from an earlier version would otherwise be published
:: as THIS version's notes, and the only symptom is a release describing the
:: wrong release.
if exist "!NOTES!" del /q "!NOTES!"

python "release-notes.py" "!APPVER!" "!NOTES!"
if errorlevel 1 (
    echo   [X] Could not build the release body from CHANGELOG.md.
    echo       release-notes.py prints which of the two it was: no section for
    echo       !APPVER!, or a section too short to be real.
    goto :fail
)

:: Size, not existence. The extractor this replaced wrote a one-byte file and
:: "if exist" let it through, which is how 2.3.0 shipped with an empty body.
set "NOTESOK="
for %%S in ("!NOTES!") do if %%~zS GTR 32 set "NOTESOK=1"
if not defined NOTESOK (
    echo   [X] The extracted body is too small to publish. Nothing sent.
    goto :fail
)

:: Bare tag first, because every published tag on this repo is a bare number.
:: v-prefixed is accepted as a fallback so an older release stays fixable.
set "TAG=!APPVER!"
gh release view "!TAG!" >nul 2>&1
if errorlevel 1 (
    gh release view "v!APPVER!" >nul 2>&1
    if errorlevel 1 (
        echo   [X] There is no !APPVER! or v!APPVER! release on GitHub yet.
        echo       Cut it with github.bat first, then run this to fill the body.
        goto :fail
    )
    set "TAG=v!APPVER!"
)

echo     release    : !TAG!
gh release edit "!TAG!" --notes-file "!NOTES!"
if errorlevel 1 (
    echo   [X] gh could not update the release body.
    goto :fail
)

:: Read it back. "gh said ok" is not the same as "the body is on the page".
set "BODYLEN=0"
for /f "delims=" %%B in ('gh release view "!TAG!" --json body --jq ".body|length" 2^>nul') do set "BODYLEN=%%B"
if "!BODYLEN!"=="0" (
    echo   [X] The release body still reads as empty. Stopping here.
    goto :fail
)
echo     body is now !BODYLEN! characters on GitHub.

:: Report a leftover v-tag; do not delete it. Removing a tag is the user's call.
if /i not "!TAG!"=="v!APPVER!" (
    set "VTAG="
    for /f "delims=" %%R in ('git ls-remote --tags origin "refs/tags/v!APPVER!" 2^>nul') do set "VTAG=%%R"
    if defined VTAG (
        echo.
        echo   [note] A v!APPVER! tag is still on GitHub next to !APPVER!.
        echo          Remove it when you are ready:
        echo              git push origin :refs/tags/v!APPVER!
        echo              git tag -d v!APPVER!
    )
)

:: =================================================================== 2. wiki
echo.
echo   --- 2. wiki ---------------------------------------------------

if not exist "wiki\*.md" (
    echo   [X] No markdown pages in "%~dp0wiki".
    goto :fail
)

set "WURL=!REPOURL!.wiki.git"
set "WIKI=%~dp0..\!REPONAME!.wiki"

if not exist "!WIKI!\.git" (
    echo     cloning the wiki...
    git clone "!WURL!" "!WIKI!"
    if errorlevel 1 (
        echo   [X] Could not clone the wiki.
        echo       If git said the repository was not found, the wiki has never
        echo       been created. Open !REPOURL!/wiki , click "Create the first
        echo       page", save anything at all, then run this again.
        goto :fail
    )
)

:: Set the identity on the wiki clone every run. A fresh clone has none, and
:: GitHub rejects a push whose commits carry a private email - that is GH007.
git -C "!WIKI!" config user.name  "!GHLOGIN!"
git -C "!WIKI!" config user.email "!NOREPLY!"

:: /I so xcopy treats the destination as a folder without asking, and no trailing
:: backslash on it - a "path\" inside quotes escapes the closing quote.
xcopy /Y /Q /I "%~dp0wiki\*.md" "!WIKI!" >nul
if errorlevel 1 (
    echo   [X] Could not copy the pages into the wiki clone.
    goto :fail
)

git -C "!WIKI!" add -A
git -C "!WIKI!" diff --cached --quiet
if errorlevel 1 (
    git -C "!WIKI!" commit -m "Wiki for !APPVER!"
    if errorlevel 1 (
        echo   [X] Could not commit the wiki pages.
        goto :fail
    )
) else (
    echo     pages already match what is committed.
)

:: Anything already committed with the old email still carries it, so re-author
:: every UNPUSHED commit before trying again. Pushed history is left alone.
::
:: The upstream is resolved on its own line first. If @{u} does not exist the
:: rev-list below prints nothing and AHEAD stays 0, which would read as "already
:: up to date" and skip the push -- a silent no-op is the one outcome this whole
:: script exists to stop.
set "UPSTREAM="
for /f "delims=" %%U in ('git -C "!WIKI!" rev-parse --abbrev-ref --symbolic-full-name "@{u}" 2^>nul') do set "UPSTREAM=%%U"
if not defined UPSTREAM (
    echo   [X] The wiki clone has no upstream branch set, so there is no way to
    echo       tell what still needs pushing. In "!WIKI!" run:
    echo           git branch --set-upstream-to=origin/master master
    goto :fail
)

set "AHEAD=0"
for /f %%C in ('git -C "!WIKI!" rev-list --count "@{u}..HEAD" 2^>nul') do set "AHEAD=%%C"

if "!AHEAD!"=="0" (
    echo     nothing to push - the wiki is already up to date.
) else (
    echo     re-authoring !AHEAD! unpushed commit^(s^) as !NOREPLY! ...
    git -C "!WIKI!" rebase --exec "git commit --amend --reset-author --no-edit" "@{u}"
    if errorlevel 1 (
        git -C "!WIKI!" rebase --abort >nul 2>&1
        echo   [X] Could not re-author the pending commits. The wiki clone was
        echo       put back the way it was; nothing was pushed.
        goto :fail
    )
    git -C "!WIKI!" push
    if errorlevel 1 (
        echo   [X] Push failed. The commits are re-authored and waiting in
        echo       "!WIKI!" - run "git push" there once the cause is fixed.
        goto :fail
    )
    echo     pushed.
)

:: =================================================================== finished
echo.
echo   ===============================================================
echo    Done.
echo      release : !REPOURL!/releases/tag/!TAG!
echo      wiki    : !REPOURL!/wiki
echo   ===============================================================
echo.
pause
exit /b 0

:: ---------------------------------------------------------------------------
:need
where %1 >nul 2>&1 && exit /b 0
echo   [X] "%1" is not on PATH.
exit /b 1

:fail
echo.
echo   Stopped. Nothing further was changed.
echo.
pause
exit /b 1
