# After a game patch

A patch adds assets the metadata snapshot has not described yet, and often new encrypted
records. Two downloads and two reports cover it.

## 1. Re-download both folders

**Settings ▸ General ▸ Directories** — the TACT keys first, then d4data. New seasonal and
collaboration content usually needs both. Keys are small; d4data is the long one.

## 2. Ask what arrived

**Help ▸ Patch contents** lists what each game build added, newest first, grouped by asset type
and named. The tool records this the first time it is opened on a build — so it builds up as you
use it, and it cannot reconstruct a build it was never opened on. Each entry says what it was
diffed against, so a gap between two observed builds reads as a gap rather than as one patch.

The **Latest** filter in Catalogue, Models and Bulk Extract is the same information applied to
one tab: tick it to see only what this build introduced.

## 3. Ask what broke

**Help ▸ Health check** verifies storage, keys, snapshot freshness and the live format probes in
one screen. Its *snapshot coverage* row — how many shop products and items the game has versus
how many the snapshot describes — is the honest answer to "why is this bundle missing".

`Audit - Asset Health.bat` walks every appearance and **diffs against your previous run**, so a
patch reports "+312 now renderable, 4 newly broken" instead of being discovered months later.
Attaching that diff to an issue saves a great deal of back-and-forth.

## Why something can still be missing

- **Encrypted and no key.** Around 11,500 assets sit behind keys nobody has harvested. The
  records are present; they cannot be read. Not fixable here.
- **Newer than the snapshot.** d4data is a community snapshot and lags the game. The tool falls
  back to reading the game's own binary tables for appearances, materials and texture
  definitions, so much of this resolves anyway — but names may not.
- **No name at all.** Encrypted pieces reach the index without one. Cloth-bearing pieces can
  have their name recovered from their cloth data; plain helms, gloves and boots cannot.
