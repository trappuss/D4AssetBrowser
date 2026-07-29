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

`necF_stor245_TRS` (sno 2519633) is now in the index and searchable, but shows
"This asset has no displayable geometry" with every INFO field blank except
Filesize. **Two independent silent failures:**

### 1. INFO panel — confirmed, name/JSON-driven

`ModelsTab.cpp:5736`:

```cpp
QFile f(QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4, name));
if (!f.open(QIODevice::ReadOnly))
    return;                 // NO LOG
```

Filesize comes from CASC just above; Format, Bounds, LODs, Bones, Materials and
Textures all come from that JSON, by name. Encrypted assets have none.
Same by-name pattern at `5807`, `6781`, `7149`, `7164`, `6300`.

### 2. Geometry — needs one measurement to pin down

`ModelsTab.cpp:7101` loads **two separate CASC files**:

```cpp
const QByteArray meta    = reader->readMetaBySno(quint64(sno));   // base/meta/<sno>
const QByteArray payload = reader->readPayloadBySno(quint64(sno)); // base/payload/<sno>
if (!meta.isEmpty() && !payload.isEmpty())
    *geo = ModelParser::parseApp(meta, payload);                   // guard is SILENT
```

The dump only ever read the **payload** for group 9, so whether the **meta** is
readable was never established.

All 12 early exits in `ModelParser::parseApp` are silent (lines 790, 791, 797,
817, 820, 823, 824, 834, 892, 921, 932, 945). The only warning, line 919, fires
only when `droppedSubs > 0`, so a mesh with zero usable sub-objects is silent too.

### Confirmed symptom (Wardrobe, Necromancer Female)

PARTS lists base pieces with real material names (`necF_base01_GLV_mat`,
`armor_skin_mat`, `necF_base01_BTS_mat`) but every stor245 sub-object as
`part 7`, `part 8`, ... and MATERIALS shows 10 entries, none from stor245.
That is `roster.value(p.materialIndex)` returning empty at
`WardrobeTab2.cpp:7146`. Geometry is correct; only material binding is missing.

## Do this next, in order

1. **Numeric material path.** `MaterialDecode::appearanceRoster` (MaterialDecode.cpp:179) opens `Appearance/<name>.app.json` and returns empty for encrypted assets. Needs a CASC fallback: appearance meta -> material SNOs -> `Material/<sno>` meta -> texture SNOs. Note the MATERIALS panel already shows a SNO column, so material SNOs are reachable somewhere — start by finding where that column is populated and whether it has a binary route. Serves all 125 readable encrypted appearances.
   tab first (its cloth sim writes ~1 line/second and buries everything), select
   `necF_stor245_TRS` in Models, then run it. It greps for
   `loadGeometry: parse produced no geometry`, which is emitted **only** when
   `parseApp` actually ran.

2. **If that line is ABSENT** — `parseApp` was never called, so `readMetaBySno`
   returned empty. The meta file, not the payload, is the blocker. Extend the
   dump to print meta size alongside payload size for group 9, and check
   `EncryptedSNOs.dat.json` for whether `base/meta/<sno>` sits under a different
   key.

3. **If that line is PRESENT** — the failure is inside `parseApp`. Add a
   one-line `qWarning` to each of the 12 early exits naming the gate. Worth doing
   regardless: right now every parse failure anywhere in the tool is
   indistinguishable from every other.

4. **Then** the INFO panel and material paths need a numeric fallback:
   Appearance payload -> material SNOs -> texture SNOs, never touching a name.
   That single change would serve all **125** readable encrypted appearances, not
   just the 30 that got names.

## Related, still open

`ModelParser` only decodes vertex buffer 0. **156 of 1500** wardrobe appearances
have LOD0 sub-objects on another buffer and lose them (`barF_base00` 14/31);
`nVertOffset` is per-buffer relative, so dropping is the safe failure. Proper fix
= decode each buffer with its own layout and merge. Core geometry path, wants its
own pass with `Cloth Audit.bat` as the regression net.

## Commits

`66944da` index nameless records · `cf8bd79` TACT filter · `b034467` +
`a7e2cc7` dump v1/v2 · `43667b7` name recovery · `a332e83` cache v2
