# Publishing releases on GitHub

This project ships as a **portable Windows folder**. You can build it locally
(`package-release.bat`) or let GitHub build and publish it for you when you push
a version tag (the `.github/workflows/release.yml` workflow).

Below is the one-time setup, then the repeatable "cut a release" flow.

---

## 0. One-time prerequisites

- A **GitHub account** — https://github.com/join
- **GitHub Desktop** (easiest, GUI) — https://desktop.github.com
  *or* the **git** command line — https://git-scm.com/download/win

The rest of this guide shows GitHub Desktop first (recommended), with the
equivalent git commands after.

---

## 1. Create the repository on GitHub

1. Go to https://github.com/new
2. **Repository name:** `Diablo4AssetBrowser` (anything you like).
3. **Visibility:** Public or Private — either works. (Public = anyone can
   download your Releases.)
4. Leave "Add a README / .gitignore / license" **unchecked** — this project
   already has them.
5. Click **Create repository**. Keep that page open; you'll need the URL
   (e.g. `https://github.com/YOURNAME/Diablo4AssetBrowser.git`).

---

## 2. Push the project

### Option A — GitHub Desktop (GUI)

1. Open GitHub Desktop → **File ▸ Add local repository…**
2. Choose this project folder
   (`…\Diablo4AssetBrowser Native`).
3. If it says "this directory is not a Git repository", click
   **create a repository** → **Create repository**.
4. Click **Publish repository** (top bar). Pick the account/name, choose
   Public/Private, click **Publish**.
   - GitHub Desktop respects `.gitignore`, so `build/`, `data/`, `dist/`,
     `_backups/`, etc. are **not** uploaded — only the source.

### Option B — git command line

Open a terminal **in the project folder** and run:

```bat
git init
git add .
git commit -m "Diablo IV Asset Browser v2.1.0"
git branch -M main
git remote add origin https://github.com/YOURNAME/Diablo4AssetBrowser.git
git push -u origin main
```

(Replace the URL with your repo's.)

> First push only: if git asks you to sign in, a browser window will handle it.

---

## 3. First build (check it works)

Builds use a **prebuilt Qt** (downloaded, not compiled) plus vcpkg for a few
small libraries, so a run takes only **a few minutes** — the first one too.

To kick off a test build without making a release yet:

1. On your repo page, click the **Actions** tab.
2. In the left list, click **Release**.
3. Click **Run workflow ▸ Run workflow** (uses the `main` branch).
4. Wait for it to go green. Then open the finished run and download the
   **`D4AssetBrowser-portable`** artifact (bottom of the page) to test the zip.

If it fails, open the failed step to see the log, and send it to me.

---

## 4. Cut a release (this is the repeatable part)

A release is triggered by pushing a **tag** that starts with `v`.

### Option A — GitHub Desktop

1. **History** tab → right-click your latest commit → **Create Tag…**
2. Name it `v2.1.0` (match the app version) → **Create Tag**.
3. **Repository ▸ Push** (make sure "push tags" happens — Desktop pushes tags
   with the branch).

### Option B — git command line

```bat
git tag v2.1.0
git push origin v2.1.0
```

### What happens next

- The **Release** workflow runs, builds the portable folder, and **creates a
  GitHub Release** for that tag with **`D4AssetBrowser.zip`** attached and
  auto-generated notes.
- Watch it under the **Actions** tab; when green, find it under the
  **Releases** section (right side of the repo home, or `/releases`).

---

## 5. Where people download it

Your repo home page → **Releases** (right sidebar) → the version → the
**`D4AssetBrowser.zip`** asset. That's the whole shippable product: unzip and
run `D4AssetBrowser.exe`.

---

## 6. Shipping a new version later

1. Make your code changes and commit/push them (steps 2/Option).
2. Bump the version in three spots (keep them in sync):
   - `CMakeLists.txt` → `project(... VERSION 2.2.0 ...)`
   - `vcpkg.json` → `"version": "2.2.0"`
   - `src/main.cpp` → `setApplicationVersion("2.2.0")`
   - (optional) `RELEASE_README.txt` header.
3. Commit, then tag `v2.2.0` and push the tag (step 4). New Release appears.

---

## Notes & gotchas

- **Don't commit `data/`, `build/`, `dist/`, `_backups/`, or `d4data`** — the
  `.gitignore` already excludes them. `d4data` and the game files are large and
  not yours to redistribute; users download `d4data` themselves on first run.
- **CI builds are fast** — Qt is downloaded prebuilt (via `jurplel/install-qt-action`),
  not compiled. If Qt ever fails to install for a version, bump the `version:`
  in `.github/workflows/release.yml` (e.g. `6.7.3` → `6.8.1`).
- **Local** builds still use vcpkg for everything (including Qt) via
  `package-release.bat` — that first local build is the slow one; CI is not.
- **Private repo Releases** are only downloadable by people you share access
  with. Make the repo Public if you want an open download page.
