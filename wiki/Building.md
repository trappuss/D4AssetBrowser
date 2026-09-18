# Building from source

Everything here is Windows-only. The tool is a native MSVC build against Qt6 and OpenGL;
there is no cross-platform path and no Python runtime in the shipped product.

## What you need

- Windows 10/11 x64
- **Visual Studio 2022** with the *Desktop development with C++* workload
- [**vcpkg**](https://github.com/microsoft/vcpkg), with `VCPKG_ROOT` pointing at it
- **CMake 3.21+** and **Ninja** — both ship with the VS workload
- **Python 3**, for the pre-build source checks

## The batch files

Every `.bat` in the repo root is double-clickable and finds its own toolchain — none of them
need a Developer Command Prompt.

```bat
build.bat          :: first build. Finds vcvars64 itself, then lets vcpkg fetch every
                   :: dependency. Qt6 is built FROM SOURCE here, so budget 30-60 min.
rebuild.bat        :: incremental build, then launch. The one you use day to day.
clean-rebuild.bat  :: wipes build\ and configures again, without touching vcpkg.
Diagnostics.bat    :: menu of audits and self-tests
```

The hour on a cold `build.bat` is Qt compiling. It happens once; every build after that is
`rebuild.bat` and takes seconds to a couple of minutes.

## Or drive CMake directly

`CMakePresets.json` carries three configurations:

```bat
cmake --preset windows-msvc-release   &&  cmake --build --preset release   :: build\release
cmake --preset windows-msvc-debug     &&  cmake --build --preset debug     :: build\debug
cmake --preset windows-static-release &&  cmake --build --preset static    :: one static exe
```

## Dependencies

From the `vcpkg.json` manifest, pinned to a baseline commit so a build today resolves the same
versions as a build six months from now: **qtbase** (widgets, opengl, gui, png, jpeg) ·
**qtsvg** · **fastgltf** · **tinygltf** · **zlib** · **lz4**.

## `verify-src.py`

Runs before the compiler, in seconds rather than after a multi-minute MSVC cycle, and catches
the mistakes that have actually broken this build: zero-byte files from a botched write,
unbalanced `{}` `()` `[]`, a header-only helper used without its `#include`, printf-style
format and argument mismatches, locals named `emit` / `signals` / `slots` that Qt's macros
silently delete, duplicate map keys, settings keys written and never read, and combo boxes
persisted by their display label instead of their value.

```bat
python verify-src.py            :: check src\
python verify-src.py --quiet    :: only print problems
```

It is wired into the build, so a failing check stops the compile rather than being something
you have to remember to run.

## Continuous integration

`.github/workflows/release.yml` builds the portable Windows folder on GitHub's runners. Push a
version tag — `2.3.0` or `v2.3.0`, both fire — and it compiles, runs `windeployqt`, zips the
result and publishes it as a GitHub Release, using the matching section of `CHANGELOG.md` as
the release body rather than a list of commit subjects. Run it by hand from
**Actions ▸ Release ▸ Run workflow** to get the same zip as a plain artifact without cutting a
release.

The CI build installs a **prebuilt Qt 6.7.3** instead of letting vcpkg compile Qt, and uses
vcpkg only for the four small dependencies. That is the whole reason a cold CI build takes
minutes while a cold local `build.bat` takes closer to an hour — it is the same source and the
same compiler either way.

## Cutting a release

`github.bat` drives it end to end: set the version, commit, push, tag and publish. The tag is
a bare version number (`2.3.0`), matching every published tag on the repo, and the release body
comes from the matching `## 2.3.0` section of `CHANGELOG.md`.

The workflow publishes its own build of the same tag as a cross-check. Both paths produce a zip
from the same commit; if they ever differ, something is wrong with the local toolchain rather
than with the release.
