# d4cloth — cloth physics diagnostic harness

A standalone, **text-first, deterministic, headless** CLI for diagnosing and rebuilding the
cloth/physics system of Diablo4AssetBrowser Native. No GUI, no OpenGL, no wall clock, no RNG:
the same inputs produce byte-identical output, which is what makes regression diffs and A/B
comparisons meaningful. See `PLAN.md` for the full design and `../../PHYSICS_AUDIT.md` for
the defect history this tool exists to close out.

## Why this exists

Iterating on the main app cost a full rebuild per hypothesis, and the only feedback was a
screenshot. Four consecutive "fixes" built on screenshot inference were wrong. This harness
inverts that: every claim about the solver must cite a number this tool printed.

## Build

**Linux / CI (fast iteration loop):**
```
cmake -S tools/d4cloth -B build-d4cloth -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-d4cloth
```
Requires Qt6 Core, zlib, liblz4 (`apt install qt6-base-dev zlib1g-dev liblz4-dev`).

**Windows (extraction + in-situ verification):** run `Build d4cloth + Extract Corpus.bat`
in the repo root — it configures with the same vcpkg toolchain as the app (binary-cache hit
on qtbase, so no Qt rebuild), builds `d4cloth.exe`, and runs the one-time corpus extraction.

## Commands

```
d4cloth extract --casc "G:\G Games\Diablo IV" --d4data <snapshot> --cases cases.json --out corpus/
    One-time (Windows): pulls the test-matrix assets out of CASC into a portable corpus:
    appearance/<name>.{meta,payload}.bin (+ .app.json), anim/..., cloth/*.clt.json,
    manifest.json with SNOs + sizes + FNV-1a hashes.

d4cloth inspect --corpus corpus/ (--case <name> | --piece <appearance>...) [--out report.txt] [--csv cov.csv]
    Authored-data coverage report: every one of the 27 ClothData arrays + the full header,
    parsed/consumed/fallback status against the app runtime, min/max/mean with units,
    index validity, cluster partition check, attachmentLengths reference-candidate
    correlations (H1), driver-frame vs bone-rest matching (H2), and hex dumps of the
    still-undecoded arrays.

d4cloth version
```

Planned next (see PLAN.md milestones): `run <scenario>` with `--dump particles|bones|summary`,
SVG/ASCII rendering, invariants, `--trace`, `--compare`, `--param`, LegacySolver replica,
then the corrected solver.

## Test cases (`cases.json`)

`cape-solo`, `cape-outfit` (primary reproduction: bones 326→328), `two-capes` (shared-cage
defect), `fur-regression` (barF_stor151), `helm-15`, `skirt`, `hair`. Edit the piece lists
freely — `extract` re-pulls only what is missing from the corpus.

## Ground rules (from the audit — do not re-litigate)

- `capsuleRadius` 0.52 is correct for skin-fit capsules; authored capsules are exact.
- Provenance inferred from skin indices is refuted; per-piece provenance must come from
  merge time or from the authored driver arrays.
- `driveW=1.00` measured everywhere: anchor-distance weighting is not the cape fix.
- `attachLen` consumed as a world distance was a confirmed dimensional bug; its true
  semantics are an open research item this tool's `inspect` is designed to settle.
- Units: `m_radius` in the app is half the bounding-box **diagonal**, not a body radius.
