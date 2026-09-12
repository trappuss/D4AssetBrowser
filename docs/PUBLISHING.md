# Cutting a release

The repository is **`trappuss/D4AssetBrowser`**. Tags are **bare numbers** — `2.3.0`, no `v`
prefix. Both of those have been wrong in this file before and the mistakes cost real time, so they
are stated first.

`github.bat` in the project root is the front end for all of this: a menu over git that already
knows the remote and the branch. It will refuse to run if `.git` is missing and it never runs
`git init` — this repository has 160-plus commits of history and re-initialising it would throw
them away.

---

## The order things must happen in

1. **Bump the version** — `Release - Set Version.bat`
2. **Build** — `rebuild.bat` (runs `verify-src.py` first and stops if it fails)
3. **Smoke-test the zip** — `Test - Release Smoke.bat`
4. **Commit and push** — `github.bat`
5. **Tag** — `github.bat`, bare number
6. **Write the release notes** — the workflow attaches the zip; the notes are yours to paste

Steps 2 and 3 are the ones people skip. A release cut from unbuilt source is a zip that does not
match its notes, and there is no way to un-publish it cleanly.

---

## 1. Version

`Release - Set Version.bat` takes the new number and updates **five places in four files**, then
reads them all back from disk to prove it:

| File | What holds the version |
|---|---|
| `src/main.cpp` | the `setApplicationVersion` call — **authoritative**; the release zip is named from it |
| `CMakeLists.txt` | the `project(... VERSION ...)` line |
| `vcpkg.json` | the `version` field |
| `res/app.rc` | `VER_NUM` (comma-separated, and the trailing build field must survive) |
| `res/app.rc` | `VER_STR` (dotted) |

The two `app.rc` entries are what Windows shows under **Properties ▸ Details**. Hand-editing is
possible but has to touch all five; the bat exists because four-out-of-five is silent and the zip
still builds.

## 2. Build

```bat
rebuild.bat
```

`verify-src.py` runs first and the build stops if it reports anything. It is not a linter in the
usual sense — every check in it exists because the defect it catches shipped at least once. Read
`docs/HYGIENE_TOOLING.md` if a check fires and the reason is not obvious.

Check `build_errors.txt` is empty and `build_log.txt` ends clean before going further.

## 3. Smoke test

```bat
Test - Release Smoke.bat
```

This tests **the packaged zip**, not the build tree — which is the point. A build that runs from
`build\release\` and dies from the zip is missing a runtime DLL or a Qt plugin, and that is exactly
what this catches.

`package-release.bat` builds the portable folder on its own if you want the zip without the tests.

## 4. Commit and push

```bat
github.bat
```

Use the menu. Two standing rules for this repository:

- **Never run git against this folder from a mounted or bridged filesystem.** It leaves
  `.git\index.lock` behind and every subsequent git command on Windows then refuses to run.
- **Do not add a `.gitattributes`.** Line endings here are mixed and already committed that way
  across the history; a `text=auto` rule would renormalise the whole tree in one commit and make
  every future diff unreadable.

## 5. Tag

A tag starting with a digit triggers `.github/workflows/release.yml`, which builds the portable
folder and creates the GitHub Release with `D4AssetBrowser.zip` attached.

Tag from `github.bat`. The tag is the bare version — `2.3.0` — and must match
`setApplicationVersion` exactly, because that is what names the zip inside the release.

Watch it under **Actions**. When it is green the release is under **Releases**.

## 6. Release notes

The workflow's auto-generated notes are a commit list and are not what anyone wants to read. Paste
the notes written for the version instead — the house style is symptom first in bold, cause in one
plain sentence, `Fixed` before `Added`, `Build tooling` last and short, no compare link.

`CHANGELOG.md` carries the same content in one place. Keep the two identical; the release page is
canonical and the file is the offline copy.

---

## If the build fails in CI but works locally

Qt is downloaded prebuilt in CI (`jurplel/install-qt-action`) rather than compiled, so a CI-only
failure is usually the Qt version rather than the code. Bump `version:` in
`.github/workflows/release.yml`. Local builds use vcpkg for everything, which is why the first
local build is the slow one and CI is not.

## What never goes in the repository

`data/`, `build/`, `dist/`, `_backups/` and `d4data` are all excluded by `.gitignore` and must stay
that way. The game files are large and not ours to redistribute — users download `d4data`
themselves on first run, and TACT keys are fetched, never shipped.
