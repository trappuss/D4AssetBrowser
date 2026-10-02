# d4shader — shader-reuse feasibility probe

A standalone, **headless, read-only** CLI that answers one question from bytes:

> Can Diablo IV's own compiled shaders be extracted and run, or does the game ship only shader
> *declarations* — slot names and parameters — with the instructions somewhere unreachable?

Built the same way as `tools/d4cloth`, for the same reason its README gives: *"Four consecutive
'fixes' built on screenshot inference were wrong."* A shader viewer built before this answer would
have nothing to run, so this comes first and is deliberately not a viewer.

## Why the question is open

The app's renderer is hand-written GLSL 450 (`GLModelWidget.cpp`, `kVert` / `kFrag`). It does use
the game's data — the shader NAME classifies parts (`hero_opaque_hollow` finds facial hair,
`Hero_Eye` finds eyes, `vfx*` marks unlit effects), declared texture slots drive the eye
composite, and every material value is the authored one. What it does not use is the game's shader
*code*; `WardrobeTab2.cpp` says so outright: *"the procedural Hero_Eye shader can't be run"*.

Two facts narrow the search. SNO groups **107 Shader** and **108 ShaderMap** exist in
`SnoIndex.cpp`'s table, so the records are named. And the d4data snapshot has **no `Shader`
folder** — 24 group folders, none of them 107/108 — so whatever exists is in CASC, not the JSON
export. That is exactly what this probe reads.

## What it does

For every record in groups 107 and 108: read `base/meta/<sno>` and `base/payload/<sno>`, classify
the first bytes, and separately search the blob for an embedded container. A record that does not
*start* with `DXBC` may still *contain* one, and the report never conflates the two.

Reports per group: record count, how many have a meta / payload / neither, how many are
TACT-encrypted (and how many of those we hold a key for), a size histogram, and a histogram of
what the bytes actually are. `--dump <dir>` writes the first few blobs out whole so the counts can
be checked by eye.

It reads the game install. It does **not** touch the running game, attach to any process, or hook
a graphics device.

## Reading the verdict

- **`DXBC` / `DXIL` / `SPIR-V` present** → compiled programs ship here. Worth pursuing; the next
  milestone is *one* shader cross-compiled to GLSL on *one* material, not a viewer.
- **Small records, no recognised container** → declaration-only. The question closes: reimplementing
  in GLSL is the only path, and the remaining fidelity gap is the lighting rig and tonemap, which
  can be matched from reference screenshots for a fraction of the effort.
- **Groups absent from CoreTOC** → shaders are not SNO records at all; nothing to extract.
- **Mostly TACT-encrypted with no key** → unreachable regardless of format.

## Build + run

```
Dev - Build d4shader + Probe.bat
```

Configures with the same vcpkg toolchain as the app (binary-cache hit on qtbase, so no Qt
rebuild), builds into `tools/d4shader/build`, and writes `Claude outputs\shader_probe.txt` plus
sample blobs in `tools/d4shader/out`. The app build is untouched.

Standalone, by hand:

```
cmake -S tools/d4shader -B tools/d4shader/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build tools/d4shader/build
tools/d4shader/build/d4shader --casc "G:\G Games\Diablo IV" --out shader_probe.txt --dump out
```
