# Encrypted (TACT) content — state and next steps

Written 2026-07-29. Goal: make encrypted content (Doom collab Praetor's Suits,
seasonal store sets) browsable and renderable.

## Settled facts — do not re-derive

Measured from `d4data/json/base/EncryptedSNOs.dat.json` and `D4_DUMP_ENCRYPTED`:

- **14,101 encrypted SNOs over 197 TACT keys.** We hold 9 keys, covering 2,593.
- The 7 Praetor's Suit item SNOs are all group 73 under key `f159f1f70eabaab1`,
  **which we hold**. That key covers 1,116 SNOs: 392 Texture, 157 Particle,
  130 Cloth, 109 Material, 93 StringList, 72 Appearance, 50 StoreProduct,
  42 Actor, 35 Item, 28 EffectGroup, 8 Marking*.
- 35 items = 7 classes x 5 slots. `tInvImages` gives each item's class directly:
  Sorcerer 0, Druid 1, Barbarian 2, Rogue 3, Necromancer 4, Spiritborn 5,
  Paladin 6.
- **Encrypted records reach CoreTOC with the name BLANKED.** Indexed as
  `~unnamed_<sno>` (`CoreToc.cpp:97`, `SnoIndex.cpp:337`).
- **Every roster in the tool is name-shaped.** The wardrobe slot filter is
  literally `startsWith("barf") && l[4]=='_' && endsWith("_trs")`
  (`WardrobeTab2.cpp:4840`). Decrypting is not enough — a nameless appearance
  cannot appear anywhere.

### Name sources — three ruled out, one works

| Source | Verdict |
|---|---|
| `Appearance/<name>.app.json` self-name field | **None.** Walked every string in `BarF_stor212_BTS.app.json`; zero contain the stem. |
| `EncryptedNameDict.dat.json` | **4 entries total.** Useless. |
| Actor / Item / Cloth / Material payloads | **Zero** name-shaped strings: 0 of 102, 0 of 77, 0 of 146, 0 of 303. |
| **Appearance payload (ClothData name field)** | **WORKS.** `necM_stor245_TRS_cape`, `palF_stor171_LEG_hipPlate`, `DruM_stor235_GLV_fur_HQO`, `spiM_stor190_HLM_main`. |

`stor245` is the Doom set (`Chest_Cosmetic_Necro_245_stor`).

### Also ruled out

`Item +24 snoActor -> Actor +36 snoAppearance` resolves for **all 77** readable
nameless items, but lands on the **PROXY body mesh, not the transmog** — all
seven classes' chest items point at appearance `217477`. `ItemDef.h` warned about
this; now measured. Do not build on it.

## What is implemented and working

- `SnoIndex::recoverEncryptedNames` — scans nameless-but-readable Appearance
  payloads for identifier-shaped ASCII runs, peels trailing part tokens until the
  result matches `^[A-Za-z]{3}[FfMm]_[A-Za-z]+[0-9]+_(HLM|TRS|GLV|LEG|BTS)$`.
  Anything failing the shape stays nameless (a wrong name seats a piece on the
  wrong class). Two cloth blocks in one appearance must agree or the record is
  abandoned. Runs before `saveToCache`, so it rides the per-build index cache.
  **Result: 576 nameless, 125 readable, 30 recovered, 0 duplicates.**
- Index cache is `coretoc_v2.bin`. **Bump this filename whenever entry-name
  meaning changes** — a v1 cache loaded clean and silently skipped the recovery
  pass, and the only symptom was the absence of something you went looking for.
- Models tab: **"Only encrypted (TACT)"** filter, mutually exclusive with
  "Only decrypted". Backed by `CascReader::tactKeyFor` / `haveTactKey`
  (header-only BLTE frame probe, memoised in a memory-only hash).
- `Dump Encrypted SNOs.bat` — `D4_DUMP_ENCRYPTED=1`, with a stale-binary guard.
- `Check Encrypted Render.bat` — triages the render failure below.

**Partial by construction:** only cloth-bearing pieces carry a ClothData name, so
this recovers capes/skirts/chests, not plain boots or gloves. Closing that gap by
SNO adjacency would be guessing.

## SOLVED: multi-frame BLTE decode (was tool-wide silent data loss)

`necF_stor245_TRS` renders. The cause was **not** the parser and **not** the
Doom set — it was `CascReader::blteDecode`.

```
BLTE: INCOMPLETE decode — 26 of 27 frame(s) failed, 954470 of 1760400 byte(s)
recovered; first failure frame 1 type 'E' key f159f1f70eabaab1
```

Frame 0 succeeded, frames 1-26 all failed, key held, no missing-key warning. The
only input differing between frame 0 and the rest is the BLOCK INDEX, which feeds
the Salsa20 nonce. Two constructions are in circulation — append the block index
to the IV, or XOR it into the IV's first 4 bytes — and **they are identical when
the block index is 0**. Almost every encrypted asset is a single frame, so the bug
had no symptom until a 1.7 MB payload split across 27 frames appeared.

Both are now tried and the chunk header's declared uncompressed length decides
(length match, not inner-type match: a wrong nonce yields plausible bytes whose
first byte reads as a frame type and passes by luck ~1 in 256). The verified
winner is memoised in an atomic and tried first thereafter.

`blteDecode` also now reports shortfalls instead of returning a short buffer. It
previously appended `decompressFrame`'s empty result and continued, so callers got
truncated data with no indication — and each blamed itself. **Any encrypted asset
in more than one BLTE frame was affected**, which likely explains earlier
missing-texture / incomplete-mesh reports in this project.

Verified after fix: zero `BLTE: INCOMPLETE`, zero `model-parse: GATE`, and both
`necF_stor245_TRS` and `necF_stor245_LEG` render on the wardrobe character.

## Open problem: recovered pieces render UNTEXTURED

Geometry is correct and complete. Only material binding is missing.

**Confirmed symptom** (Wardrobe -> Necromancer -> Female -> Torso -> stor245):
PARTS lists base pieces with real material names (`necF_base01_GLV_mat`,
`armor_skin_mat`, `necF_base01_BTS_mat`) but every stor245 sub-object as
`part 7`, `part 8`, ... and MATERIALS shows 10 entries, **none** from stor245.

**Cause.** `WardrobeTab2.cpp:7145-7146`:

```cpp
const QStringList roster = MaterialDecode::appearanceRoster(d4, name);
for (MeshPrimitive& p : geo.primitives) p.materialName = roster.value(p.materialIndex);
```

`appearanceRoster` (`MaterialDecode.cpp:179`) opens
`json/base/meta/Appearance/<name>.app.json` and returns an empty list when the
file is absent — which it always is for encrypted content, because the d4data
dump cannot contain what it could not decrypt. Empty roster -> empty
`materialName` -> no material, no texture, white render, `part N` labels.

The Models tab INFO panel fails the same way at `ModelsTab.cpp:5736` (silent
`return` on the same missing JSON), which is why Format/Bounds/LODs/Bones/
Materials/Textures are all `—` while Filesize (from CASC) is correct. Same
by-name pattern at `5807`, `6781`, `7149`, `7164`, `6300`.

## Material-sno table — derivation status (IN PROGRESS, do not build on it yet)

Goal: read an appearance's material snos from the CASC meta binary, so encrypted
appearances (which have no `.app.json`) can be textured. The binary already gives
the per-sub-object material ORDER at `ModelParser.cpp:356`; only the sno LIST is
missing.

Driven by `Dump Material SNO Table.bat` (builds + sweeps + reports, unattended).
Scores a candidate rule against the JSON on ~63,700 named appearances.

### Established

- Records contain the material sno at **+20**, and the descriptor is
  `(dataOffset, totalBytes)` — the `arr()` convention `ModelParser` already uses.
- Descriptor search finds a candidate for **62,974 of 63,694** (98.9%).
- **Contiguity is 100%** of located: every record the walk reads is a real sno.
  The format is not in doubt.

### Not established — the open question

`record[i] == ptAppearanceMaterials[i]` holds for only **14,288 (22.7%)**.
Split by cause (`matsno_sweep.csv`):

| | count |
|---|---|
| declared count != JSON count | 31,221 |
| count agrees but order differs | 17,652 |

Count mismatches are dominated by `decl=1` against `json=2/3/4`
(8,108 + 4,237 + 1,790).

### Hypotheses tested and REJECTED — do not retry

1. **First-forward-match picked a sub-array.** Changed to longest-fully-valid.
   No change.
2. **Validator too strict (cloth/blank slots rejected the real array).** Widened
   to "0, or any sno the index knows, with at least one real Material". Numbers
   moved by 3 (31,218 -> 31,221). Falsified.

### The remaining candidate

**The 72-byte record size is probably not universal.** It was measured from ~32
hand-picked appearances and has been an assumption ever since; every search since
filters on `size % 72 == 0`, so any appearance with a different record size can
only ever match a coincidental 1-record array — which is exactly the `decl=1`
population.

Next step: stop assuming 72. For each candidate `(dataOffset, totalBytes)`, try
record sizes that divide `totalBytes`, and score by how many records land on a
valid sno at `+20`. Let the stride be *derived* per appearance rather than fixed.
If a single stride then explains the corpus, it is the answer; if several do, the
record size is versioned and the descriptor must say which.

### Method note

Three rounds, three "REJECTED" verdicts, and each was the metric or the filter
rather than the data: a stride test comparing in sno order, a coverage test
against a superset of sno kinds, a validator demanding Material for every slot.
The sweep caught all three before a reader was written on top of them, which is
the point of scoring against ground truth. Treat a low score as "my rule is
wrong" before "the format is odd".

## Do this next, in order

1. **Numeric material path** — the main job. `appearanceRoster` needs a CASC
   fallback used when the JSON is absent: appearance meta -> material SNOs ->
   `Material/<sno>` meta -> texture SNOs, never touching a name.
   Starting point: the MATERIALS panel already shows a **SNO column**, so
   material SNOs are reachable somewhere. Find where that column is populated
   and whether it has a binary route or is also JSON-derived. Serves all **125**
   readable encrypted appearances, not just the 30 that got names.

2. **Multi-vertex-buffer gap** — separate, pre-existing, still open. `ModelParser`
   decodes only vertex buffer 0; **156 of 1500** wardrobe appearances have LOD0
   sub-objects on another buffer and lose them (`barF_base00` 14/31). The log
   still shows ~37 drops per session. `nVertOffset` is per-buffer relative, so
   dropping is the safe failure. Proper fix = decode each buffer with its own
   layout and merge. Core geometry path — use `Cloth Audit.bat` as the regression
   net.

3. **Optional: widen name recovery.** Only cloth-bearing pieces carry a ClothData
   name, so helms/gloves/boots of a recovered set stay `~unnamed_<sno>`. Do NOT
   infer them from SNO adjacency. A defensible route is class+slot from the item
   (`tInvImages` for class, `snoItemType` for slot) synthesising e.g.
   `necF_enc2519634_BTS` — honest about identity while still landing in the right
   wardrobe cell.

## Method notes worth keeping

- **Stale artefacts caused two false diagnoses in one session.** An exe older than
  the source, then a `coretoc_v1.bin` written before the recovery pass existed.
  Both loaded clean and presented as "the feature does nothing". Check mtimes and
  cache versions before theorising.
- **Close the Wardrobe tab before reading the log.** Its cloth sim writes
  ~1 line/second and buries the window you need.
- **Silent early exits cost more than they save.** Twelve unlogged `return`s in
  `parseApp` and a silent guard in `loadGeometry` made a CASC bug look like a
  parser bug for several rounds. They are all instrumented now — keep it that way.
- `verify-src.py` miscounts commas inside string literals as printf argument
  separators. Use em dashes in log messages.

## Commits

`66944da` index nameless records - `cf8bd79` TACT filter - `b034467` + `a7e2cc7`
dump v1/v2 - `43667b7` name recovery - `a332e83` cache v2 - `d68141b` name the
silent parser gates - `d9dbe40` no-buffers self-diagnosis - `d47a923` BLTE
incomplete-decode reporting - `dd14fd7` BLTE nonce verification (**the fix**) -
`e6882bf` nonce memo
