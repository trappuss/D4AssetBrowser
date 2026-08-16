@echo off
setlocal enabledelayedexpansion
title D4AssetBrowser - GitHub
cd /d "%~dp0"

:: ============================================================================
::  github.bat - everything this repo needs, from a menu. Double-click it.
::
::  Put this file in the D4AssetBrowser working folder (the one with .git in
::  it). Unlike the DIAssetBrowser version there is NO staging copy and no file
::  syncing here: this folder IS the repository, and .gitignore already decides
::  what ships.
::
::  You can also skip the menu:
::      github.bat  fixed the cloth cage on shared skeletons
::
::  WHAT IS ON GITHUB TODAY (checked, not assumed): main holds LICENSE and
::  README.md and nothing else - 19 commits of documentation. No source has
::  ever been pushed from anywhere. This folder's 163 commits are a completely
::  separate history, so the FIRST push has to join the two rather than replace
::  either. That is handled below and asks before it does anything.
::
::  Target branch: this folder is on "master", GitHub's default is "main". The
::  branch to push INTO is asked once and remembered in git config
::  (d4browser.targetbranch); pushing "master" blindly would create a second
::  branch on GitHub that nobody reads.
:: ============================================================================

set "WEBURL=https://github.com/trappuss/D4AssetBrowser"
set "ORIGIN=https://github.com/trappuss/D4AssetBrowser.git"

:: THE PAGER. git sends anything longer than a screen to "less", which takes
:: the console over and waits for keys nobody expects to have to press - the
:: script looks frozen. With 100+ changed files here that is not an edge case,
:: it is the normal path, so every command that PRINTS gets --no-pager. Doing
:: it per command rather than globally leaves the user's own git config alone.
set "GIT=git --no-pager"

call :preflight
if errorlevel 1 exit /b 1

if not "%*"=="" (
    set "MSG=%*"
    call :do_update
    pause
    exit /b 0
)

:: ============================================================================
:menu
cls
echo ============================================================
echo   D4AssetBrowser  -  GitHub
echo   %WEBURL%
echo ============================================================
call :show_state
echo.
echo   1   Update GitHub        commit and push  ^(the usual one^)
echo   2   See what changed     no writes, nothing is sent
echo   3   Pull from GitHub     take changes made on the website
echo   4   Cut a release        tag this commit; the workflow builds the zip
echo   5   Open the repo in your browser
echo   6   Open the build workflow ^(Actions^)
echo   7   Settings             identity, sign-in, remote, target branch
echo   0   Exit
echo.
set "OPT="
set /p "OPT=  Choose: "

if "%OPT%"=="1" ( call :do_update   & call :hold & goto :menu )
if "%OPT%"=="2" ( call :do_status   & call :hold & goto :menu )
if "%OPT%"=="3" ( call :do_pull     & call :hold & goto :menu )
if "%OPT%"=="4" ( call :do_release  & call :hold & goto :menu )
if "%OPT%"=="5" ( start "" "%WEBURL%" & goto :menu )
if "%OPT%"=="6" ( start "" "%WEBURL%/actions" & goto :menu )
if "%OPT%"=="7" ( call :do_settings & call :hold & goto :menu )
if "%OPT%"=="0" exit /b 0
if "%OPT%"==""  goto :menu
echo   "%OPT%" is not one of the options.
call :hold
goto :menu

:: ============================================================================
:hold
echo.
echo   ---- press a key to go back to the menu ----
pause >nul
exit /b 0

:: ============================================================================
:preflight
where git >nul 2>&1
if errorlevel 1 (
    echo.
    echo   git is not installed, or not on PATH.
    echo   Get it from https://git-scm.com/download/win, then run this again.
    echo.
    pause
    exit /b 1
)

if not exist ".git" (
    echo.
    echo   This folder is not a git repository. This script is meant to sit in
    echo   the D4AssetBrowser working folder, beside CMakeLists.txt.
    echo.
    pause
    exit /b 1
)

:: A leftover lock stops every git command with a message that sounds like git
:: is already running. It usually is not - it is a crashed or killed process,
:: or a git command run through a mounted share that could not unlink the file.
if exist ".git\index.lock" (
    echo.
    echo   A leftover .git\index.lock is blocking git.
    set "YN="
    set /p "YN=  Delete it and carry on? [Y/n] "
    if /i not "!YN!"=="n" del /q ".git\index.lock"
)

set "GITNAME="
for /f "delims=" %%N in ('git config user.name 2^>nul') do set "GITNAME=%%N"
if not defined GITNAME (
    echo.
    echo   git does not know who you are yet.
    call :set_identity
)

:: Sign-in. Without a credential helper, git asks for a username and password
:: IN THE CONSOLE the first time it talks to GitHub - and GitHub stopped
:: accepting account passwords there in 2021, so the only thing that prompt
:: will accept is a token most people do not have to hand. It gives no hint of
:: any of that; it just sits there looking like the script has hung.
:: Git for Windows ships the manager helper, so this is one config line.
set "CHELP="
for /f "delims=" %%H in ('git config --get credential.helper 2^>nul') do set "CHELP=%%H"
if not defined CHELP (
    echo.
    echo   ------------------------------------------------------------
    echo    NOT SIGNED IN TO GITHUB YET
    echo   ------------------------------------------------------------
    echo   git has no sign-in helper, so the first time it contacts GitHub
    echo   it will ask for a username and password in this window. GitHub
    echo   no longer accepts account passwords there, so that prompt cannot
    echo   be answered - it just waits.
    echo.
    echo   Git for Windows includes a helper that opens a proper browser
    echo   window instead, and remembers you afterwards.
    echo.
    set "YN="
    set /p "YN=  Turn the sign-in helper on? [Y/n] "
    if /i not "!YN!"=="n" (
        git config --global credential.helper manager
        echo   Done. GitHub will open a browser window when it needs you.
    ) else (
        echo   Left off. If a "Username for https://github.com" prompt shows
        echo   up, press Ctrl+C rather than typing - then Settings, 4.
    )
)

git remote get-url origin >nul 2>&1
if errorlevel 1 (
    echo.
    echo   This repository has NO remote configured - it has never been
    echo   connected to GitHub.
    set "YN="
    set /p "YN=  Point it at %ORIGIN% ? [Y/n] "
    if /i "!YN!"=="n" exit /b 1
    git remote add origin "%ORIGIN%" || ( echo   Could not add the remote. & pause & exit /b 1 )
    echo   Done.
)

call :get_target
exit /b 0

:: ============================================================================
:: Which branch on GitHub this folder pushes into. Asked once, remembered.
:get_target
set "TARGET="
for /f "delims=" %%T in ('git config --get d4browser.targetbranch 2^>nul') do set "TARGET=%%T"
if defined TARGET exit /b 0
set "BR=?"
for /f "delims=" %%B in ('git rev-parse --abbrev-ref HEAD 2^>nul') do set "BR=%%B"
echo.
echo   Which branch on GitHub should this folder push into?
echo   ^(GitHub's default for this repo is "main"; this folder is on "!BR!"^)
set "TB="
set /p "TB=  Branch [main]: "
if not defined TB set "TB=main"
git config d4browser.targetbranch "!TB!"
set "TARGET=!TB!"
echo   Saved - pushing into "!TARGET!" from now on. Change it under Settings.
exit /b 0

:: ============================================================================
:show_state
set "BR=?"
for /f "delims=" %%B in ('git rev-parse --abbrev-ref HEAD 2^>nul') do set "BR=%%B"
set "DIRTY=0"
for /f %%C in ('git status --porcelain 2^>nul ^| find /c /v ""') do set "DIRTY=%%C"
set "WHO="
for /f "delims=" %%N in ('git config user.name 2^>nul') do set "WHO=%%N"
echo.
echo   local branch  : !BR!
echo   pushes into   : !TARGET!  ^(on GitHub^)
if "!DIRTY!"=="0" (
    echo   changes       : none since the last commit
) else (
    echo   changes       : !DIRTY! files changed since the last commit
)
echo   commits as    : !WHO!
exit /b 0

:: ============================================================================
:: Read-only. The long list is offered, not forced: 100+ lines of file names
:: scroll the useful part off the top of the window before it can be read.
:do_status
set "N=0"
for /f %%C in ('git status --porcelain 2^>nul ^| find /c /v ""') do set "N=%%C"
echo.
if "!N!"=="0" (
    echo   Nothing has changed since the last commit.
    exit /b 0
)
echo   !N! files differ from the last commit.
echo.
echo   ---- how much changed, per file ----
%GIT% diff --stat HEAD
echo.
set "YN="
set /p "YN=  List every file with its status letter? [y/N] "
if /i "!YN!"=="y" (
    echo.
    %GIT% status --short
)
echo.
echo   Nothing has been staged, committed or sent.
exit /b 0

:: ============================================================================
:do_update
echo.
echo   Staging...
%GIT% add -A || ( echo   [X] git add failed. & exit /b 1 )

%GIT% diff --cached --quiet
if not errorlevel 1 (
    echo   Nothing has changed since the last commit.
    echo   Checking whether the last push completed...
    goto :push
)

set "N=0"
for /f %%C in ('git --no-pager diff --cached --name-only 2^>nul ^| find /c /v ""') do set "N=%%C"
echo.
echo   ---- what will be committed: !N! files ----
%GIT% diff --cached --stat
echo.

:: The size guard. GitHub warns over 50 MB and hard-rejects over 100 MB, and by
:: the time it rejects, the object is already in your history - getting rid of
:: it then means rewriting history. Catch it while it is only staged.
set "TOOBIG="
for /f "usebackq delims=" %%F in (`git --no-pager diff --cached --name-only --diff-filter=d`) do (
    set "FP=%%F"
    set "FP=!FP:/=\!"
    if exist "!FP!" (
        for %%S in ("!FP!") do if %%~zS GTR 52428800 (
            echo   [X] %%F is %%~zS bytes - too large for GitHub.
            set "TOOBIG=1"
        )
    )
)
if defined TOOBIG (
    echo.
    echo   Stopping. Nothing was committed or sent; those files are staged only.
    echo   Un-stage everything with:  git reset
    exit /b 1
)

if not defined MSG (
    echo   ------------------------------------------------------------
    echo    Type one short line saying what changed, then press Enter.
    echo    Example:   detail maps now bake into every model export
    echo    Press Enter on its own to cancel.
    echo   ------------------------------------------------------------
    set /p "MSG=  what changed: "
)
if not defined MSG ( echo   Cancelled - nothing committed. & exit /b 1 )

echo.
echo   Message: !MSG!
set "YN="
set /p "YN=  Commit all !N! files and push? [Y/n] "
if /i "!YN!"=="n" ( set "MSG=" & echo   Cancelled - nothing committed. & exit /b 1 )

echo.
echo   Committing...
%GIT% commit -m "!MSG!" || ( set "MSG=" & echo   [X] commit failed. & exit /b 1 )
set "MSG="

:push
echo.
echo   Contacting GitHub...
echo   ^(a sign-in window may open the first time^)
%GIT% fetch origin
if errorlevel 1 (
    call :auth_hint
    exit /b 1
)

%GIT% rev-parse --verify "origin/!TARGET!" >nul 2>&1
if errorlevel 1 (
    echo   "!TARGET!" does not exist on GitHub yet - this push will create it.
    goto :do_push
)

:: THE IMPORTANT CHECK. If the two sides share no common commit, a rebase would
:: try to replay this folder's entire history onto a stranger's, and a push
:: would be refused. Say so plainly instead of producing a wall of git output.
%GIT% merge-base HEAD "origin/!TARGET!" >nul 2>&1
if errorlevel 1 goto :join_histories

echo   Replaying your commits on top of GitHub's...
%GIT% rebase "origin/!TARGET!"
if errorlevel 1 call :resolve_conflict
if errorlevel 1 exit /b 1

:do_push
echo   Pushing...
%GIT% push -u origin "HEAD:!TARGET!"
if errorlevel 1 (
    echo.
    echo   [X] Push failed. If it says "rejected / fetch first", something
    echo       landed on GitHub while this was running - just choose 1 again.
    call :auth_hint
    exit /b 1
)

:pushed
echo.
echo   ============================================================
echo    PUSHED to !TARGET!
for /f "delims=" %%C in ('git --no-pager log -1 --oneline') do echo     %%C
echo   ============================================================
exit /b 0

:: ============================================================================
:: Printed after any failed conversation with GitHub. Nearly all of them are
:: sign-in, and what git prints for that is "authentication failed" with no
:: word on what to do next.
:auth_hint
set "CH2="
for /f "delims=" %%H in ('git config --get credential.helper 2^>nul') do set "CH2=%%H"
echo.
echo   If that was a sign-in problem:
if not defined CH2 (
    echo     - No sign-in helper is set. Settings, option 4 turns it on.
) else (
    echo     - Helper in use: !CH2!
    echo     - If it keeps refusing, your saved sign-in may be stale.
    echo       Settings, option 4, then 3 opens the place to clear it.
)
echo     - GitHub does NOT accept your account password at a console prompt.
echo       If it ever asks for one, press Ctrl+C rather than typing it.
echo   Otherwise: check your internet connection and try again.
exit /b 0

:: ============================================================================
:: The one-time join. GitHub's !TARGET! is a README/LICENSE landing page with
:: its own history; this folder is the source with its own. They were never
:: connected, so there is no common commit to rebase onto.
::
:: A force-push would be simpler and would throw away 19 commits of README work
:: including the images. Merging keeps both sides. It happens once - afterwards
:: the histories share a commit and every later push is an ordinary rebase.
:join_histories
echo.
echo   ------------------------------------------------------------
echo    FIRST PUSH - this folder and GitHub have separate histories
echo   ------------------------------------------------------------
echo   GitHub's "!TARGET!" holds the published README and LICENSE.
echo   This folder holds the source. Neither has ever seen the other.
echo.
echo   1   Join them   keep GitHub's README history AND this source
echo   2   Cancel      nothing is sent
echo.
set "UO="
set /p "UO=  Choose: "
if not "!UO!"=="1" ( echo   Cancelled. Nothing was sent. & exit /b 1 )

echo.
echo   Joining. Files that exist on BOTH sides - README.md, LICENSE - will
echo   need a decision; you get asked about those next.
echo.
%GIT% merge --allow-unrelated-histories --no-edit "origin/!TARGET!"
if errorlevel 1 call :resolve_conflict
if errorlevel 1 exit /b 1
echo   Joined.
goto :do_push

:: ============================================================================
:: Works for a stopped REBASE and a stopped MERGE - the join above produces the
:: second kind, and "git rebase --continue" on a merge does nothing useful.
:resolve_conflict
set "OPKIND=rebase"
if exist ".git\MERGE_HEAD" set "OPKIND=merge"
echo.
echo   ------------------------------------------------------------
echo    STOPPED - both sides changed the same file
echo   ------------------------------------------------------------
echo   Files needing a decision:
for /f "usebackq delims=" %%F in (`git --no-pager diff --name-only --diff-filter=U`) do echo     %%F
echo.

:conflict_menu
echo   1   Open those files so I can fix them
echo   2   Keep GITHUB's version of every conflicted file
echo   3   Keep THIS FOLDER's version of every conflicted file
echo   4   I have fixed them - carry on
echo   5   Give up and put everything back the way it was
echo.
set "CO="
set /p "CO=  Choose: "

if "%CO%"=="1" (
    for /f "usebackq delims=" %%F in (`git --no-pager diff --name-only --diff-filter=U`) do (
        set "FP=%%F"
        set "FP=!FP:/=\!"
        start "" notepad "!FP!"
    )
    echo.
    echo   Each file has the two versions marked with rows of angle brackets.
    echo   Keep the text you want, delete the marker lines, save, then choose 4.
    echo.
    goto :conflict_menu
)

:: In a MERGE, --theirs is the branch being merged in, i.e. GitHub. In a
:: REBASE the sides are swapped, because your commits are being replayed on
:: top of GitHub's - so GitHub is --ours there. Getting this backwards is how
:: people quietly keep the wrong README.
if "%CO%"=="2" (
    if "!OPKIND!"=="merge" ( set "SIDE=--theirs" ) else ( set "SIDE=--ours" )
    goto :take_side
)
if "%CO%"=="3" (
    if "!OPKIND!"=="merge" ( set "SIDE=--ours" ) else ( set "SIDE=--theirs" )
    goto :take_side
)

if "%CO%"=="4" (
    %GIT% add -A
    if "!OPKIND!"=="merge" ( %GIT% commit --no-edit ) else ( %GIT% rebase --continue )
    if errorlevel 1 (
        echo.
        echo   Still conflicted - there is more than one commit to work through.
        echo.
        goto :conflict_menu
    )
    echo   Resolved.
    exit /b 0
)

if "%CO%"=="5" (
    if "!OPKIND!"=="merge" ( %GIT% merge --abort ) else ( %GIT% rebase --abort )
    echo   Put back. Your commit is still here, it just has not been pushed.
    exit /b 1
)

echo   "%CO%" is not one of the options.
goto :conflict_menu

:take_side
for /f "usebackq delims=" %%F in (`git --no-pager diff --name-only --diff-filter=U`) do (
    %GIT% checkout !SIDE! -- "%%F"
    %GIT% add -- "%%F"
)
if "!OPKIND!"=="merge" ( %GIT% commit --no-edit ) else ( %GIT% rebase --continue )
if errorlevel 1 (
    echo.
    echo   Still conflicted - there is more than one commit to work through.
    echo.
    goto :conflict_menu
)
echo   Resolved.
exit /b 0

:: ============================================================================
:do_pull
echo.
%GIT% diff --quiet
if errorlevel 1 (
    echo   You have uncommitted changes. Commit them first ^(option 1^),
    echo   otherwise a pull could collide with them.
    exit /b 1
)
echo   Contacting GitHub...
%GIT% fetch origin || ( call :auth_hint & exit /b 1 )
%GIT% rev-parse --verify "origin/!TARGET!" >nul 2>&1
if errorlevel 1 ( echo   "!TARGET!" does not exist on GitHub yet - nothing to pull. & exit /b 0 )
%GIT% merge-base HEAD "origin/!TARGET!" >nul 2>&1
if errorlevel 1 (
    echo   This folder and GitHub have no history in common yet - use option 1,
    echo   which offers to join them.
    exit /b 1
)
%GIT% rebase "origin/!TARGET!"
if errorlevel 1 call :resolve_conflict
if errorlevel 1 exit /b 1
echo.
echo   Up to date with GitHub:
for /f "delims=" %%C in ('git --no-pager log -1 --oneline') do echo     %%C
exit /b 0

:: ============================================================================
:: .github\workflows\release.yml builds the portable zip and publishes a
:: GitHub Release whenever a tag matching v* is pushed.
::
:: NOTE: releases 2.2.0 - 2.2.7 are tagged as bare numbers, which v* does not
:: match - so that workflow has never actually run and those zips were uploaded
:: by hand. The first v-tag pushed from here will be the first time it builds.
:do_release
echo.
echo   Pushing a tag like v2.2.8 makes GitHub build the portable zip and
echo   publish it as a Release. Nothing is built on this machine.
echo.
echo   [note] Your existing releases are tagged 2.2.0 - 2.2.7 with no "v", and
echo          the workflow only fires on v* - so it has never run. A v-tag is
echo          the first one that will actually build.
echo.
set "LASTTAG="
for /f "delims=" %%T in ('git --no-pager tag --sort^=-v:refname 2^>nul') do (
    if not defined LASTTAG set "LASTTAG=%%T"
)
if defined LASTTAG ( echo     most recent local tag: !LASTTAG! ) else ( echo     ^(no tags in this folder yet^) )
echo.

%GIT% diff --quiet
if errorlevel 1 (
    echo   [note] You have uncommitted changes. The release is built from the
    echo          last COMMIT, so anything uncommitted will not be in the zip.
    echo.
)

set "TAG="
set /p "TAG=  New tag, e.g. v2.2.8 (blank cancels): "
if not defined TAG ( echo   Cancelled. & exit /b 1 )

echo.
echo   This tags the current commit and pushes it, which starts the build:
for /f "delims=" %%C in ('git --no-pager log -1 --oneline') do echo     %%C
set "YN="
set /p "YN=  Go ahead? [y/N] "
if /i not "!YN!"=="y" ( echo   Cancelled. & exit /b 1 )

%GIT% tag -a "!TAG!" -m "!TAG!" || ( echo   [X] Could not create the tag. & exit /b 1 )
%GIT% push origin "!TAG!"
if errorlevel 1 (
    echo   [X] Could not push the tag. Removing it locally so you can retry.
    %GIT% tag -d "!TAG!" >nul 2>&1
    call :auth_hint
    exit /b 1
)
echo.
echo   Tag !TAG! pushed. Watch the build at %WEBURL%/actions
exit /b 0

:: ============================================================================
:do_settings
echo.
echo   1   Name and email on your commits
echo   2   Which branch on GitHub this folder pushes into
echo   3   The remote URL
echo   4   Sign-in to GitHub
echo   0   Back
echo.
set "SO="
set /p "SO=  Choose: "

if "%SO%"=="1" ( call :set_identity & exit /b 0 )

if "%SO%"=="2" (
    echo.
    echo   Currently pushing into "!TARGET!".
    set "TB="
    set /p "TB=  New target branch (blank keeps it): "
    if defined TB (
        git config d4browser.targetbranch "!TB!"
        set "TARGET=!TB!"
        echo   Saved.
    )
    exit /b 0
)

if "%SO%"=="3" (
    echo.
    for /f "delims=" %%U in ('git remote get-url origin 2^>nul') do echo   current: %%U
    set "NU="
    set /p "NU=  New URL (blank keeps it): "
    if defined NU (
        git remote set-url origin "!NU!" || git remote add origin "!NU!"
        echo   Saved.
    )
    exit /b 0
)

if "%SO%"=="4" ( call :do_signin & exit /b 0 )
exit /b 0

:: ============================================================================
:do_signin
echo.
set "CH3=none"
for /f "delims=" %%H in ('git config --get credential.helper 2^>nul') do set "CH3=%%H"
echo   Sign-in helper currently: !CH3!
echo.
echo   1   Use the Git for Windows sign-in helper ^(opens a browser^)
echo   2   Test the connection to GitHub now
echo   3   Clear the saved sign-in and start over
echo   0   Back
echo.
set "GO="
set /p "GO=  Choose: "

if "%GO%"=="1" (
    git config --global credential.helper manager
    echo   Done. GitHub will open a browser window when it next needs you.
    exit /b 0
)
if "%GO%"=="2" (
    echo.
    echo   Contacting GitHub ^(a sign-in window may open^)...
    %GIT% ls-remote --heads origin >nul 2>&1
    if errorlevel 1 ( call :auth_hint ) else ( echo   Success - GitHub answered and knows who you are. )
    exit /b 0
)
if "%GO%"=="3" (
    echo.
    echo   Windows stores the saved sign-in, not git. Opening Credential
    echo   Manager - delete the entry named github.com under Windows
    echo   Credentials, then use option 2 to sign in again.
    start "" rundll32.exe keymgr.dll,KRShowKeyMgr
    exit /b 0
)
exit /b 0

:: ============================================================================
:set_identity
echo.
echo   Every commit carries a name and an email, and both are PUBLIC on GitHub.
echo   If you would rather not publish your real address, GitHub gives you a
echo   no-reply one under Settings - Emails - "Keep my email addresses private".
echo.
set "CURN=" & set "CURE="
for /f "delims=" %%N in ('git config user.name 2^>nul')  do set "CURN=%%N"
for /f "delims=" %%E in ('git config user.email 2^>nul') do set "CURE=%%E"
if defined CURN echo   current name  : !CURN!
if defined CURE echo   current email : !CURE!
echo.
set "NN="
set /p "NN=  Name  (blank keeps it): "
if defined NN git config --global user.name "!NN!"
set "NE="
set /p "NE=  Email (blank keeps it): "
if defined NE git config --global user.email "!NE!"
echo.
echo   Saved.
exit /b 0
