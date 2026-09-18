# Handoff — tier the AssetBrowser template

**Scope: restructure `docs/ASSETBROWSER_TEMPLATE.md` into tiers, and mark every item's
portability. Nothing else.** Do not build, refactor or touch `src\`. If you find a genuine
error in the document's *content* while working, fix it and say so — but the job is
structure, not new material.

---

## Why

The template is the design document for building the *same tool for a different game and
engine* — `(GameName|Engine)AssetBrowser`. D4AssetBrowser is the reference implementation;
FOXAssetBrowser (Fox Engine) and DIAssetBrowser (Diablo Immortal) are the real consumers.

Right now it reads as one flat list of 18 sections, all at equal weight, all written from
D4's perspective. Someone starting a browser for another engine cannot tell:

- **what they must have** before the thing is an AssetBrowser at all,
- **what can wait** until there is something on screen,
- **what is polish** they may never need,
- **what will not translate** to their engine, or would be pure bloat in a smaller tool.

Fix that. The output should let someone say *"I am building a browser for engine X, I want a
minimum viable version"* and get an unambiguous list, then come back later for the rest.

---

## Decisions already made — do not re-litigate these

| | |
|---|---|
| **Renumbering** | **Renumber freely.** Sections get reordered into tiers and numbered 1..N. You must then fix every citation — see *Citations* below and prove each one landed. |
| **Tiers** | **Four: Fundamentals · Essentials · Quality of life · Showpiece.** Definitions below. |
| **Portability** | **A second axis, marked per item — not per section.** Three marks, defined below. |

Everything else is yours to propose. If the content genuinely resists this scheme somewhere,
say so in your report rather than forcing it.

---

## Directories

| What | Path |
|---|---|
| Project root | `C:\Users\notso\Downloads\Claude Current\Diablo4AssetBrowser Native` |
| The document | `docs\ASSETBROWSER_TEMPLATE.md` — 567 lines, ~38 KB, **CRLF** |
| Docs index | `docs\README.md` — has a row describing the template, **CRLF** |
| Sibling design docs | `docs\CONTEXT_MENUS.md`, `docs\MODEL_EXPORT.md`, `docs\HYGIENE_TOOLING.md` |

**Read the whole document before changing a line of it.** It is long, it is dense, and every
number in it was measured. You cannot tier it correctly from the headings.

---

## The four tiers

Each tier's heading must open with a one-line answer to **"if you stop here, what do you
have?"** That sentence is the most useful thing on the page for someone budgeting effort.

**1 · Fundamentals** — *without these it is not an AssetBrowser.* The decisions that are
load-bearing for everything above them, and expensive to retrofit. If you stop here you have
a tool that opens the game's storage, lists assets, and draws one.

**2 · Essentials** — *needed before anyone other than you can use it.* If you stop here you
have something you could hand to a stranger with a README and expect them to succeed.

**3 · Quality of life** — *the polish that makes it feel finished rather than functional.*
Real value, genuinely skippable, and each item should be independently skippable — no QoL
item may be a prerequisite for another tier.

**4 · Showpiece** — *the game-specific tab the whole tool exists to enable*, and the reason
anyone downloads it. D4's Wardrobe, Stable and Catalogue. Every one of them stands on tiers
1–2, which is exactly why it is last.

### Starting proposal — argue with it

This is a first pass from the current section list, not an instruction. Several are genuinely
contentious and are flagged; settle those with the user before writing.

| Current § | Tier | Note |
|---|---|---|
| 1 Product identity | Fundamentals | Portability, reads-the-game-directly, fail-closed, nothing proprietary |
| 2 Architecture skeleton | Fundamentals | One viewport class especially — retrofitting this is a rewrite |
| 3 Cross-cutting conventions | Fundamentals | All ten. Each exists because it was violated |
| 4 Browse tab | **Split** | List + one search box = Fundamentals; funnel/chips/facets = Essentials; three view modes = QoL |
| 5 3D viewport | **Split** | One shared widget + camera + flat shading = Fundamentals; channel viewer + overlays = Essentials; selection set = Essentials; fullscreen/gizmo = QoL |
| 6 Panel system | QoL | **Contentious** — the panels themselves are Essentials, the reorder/persist machinery is QoL |
| 7 Textures tab | Essentials | A browser that cannot show you a texture is half a tool |
| 8 Bulk Extract | **Contentious** | QoL for a viewer, Fundamentals for a ripper. May need to be tier-by-intent |
| 9 Exporting | **Split** | Single model out = Fundamentals; name templates + layout = Essentials; GIF ladder = QoL |
| 10 Settings dialog | Essentials | Skeleton and mechanics are Essentials; Information tab is QoL |
| 11 Hotkeys | QoL | The registry pattern is cheap; the hotkeys themselves are polish |
| 12 Context menus | Essentials | Uniformity is what makes the tool learnable |
| 13 Tooling | **Split** | `verify-src.py` + build bats = Fundamentals; audit/release tooling = QoL |
| 14 Porting guide | *Not a tier* | Becomes the front matter / how-to-use-this-document section |
| 15 QoL checklist | Quality of life | Should become the tier-3 index rather than a trailing list |
| 16 Tool explains itself | **Contentious** | The explain-this-material report is arguably Essentials; the rest QoL |
| 17 Documentation | Essentials | **Contentious** — cheap while small, brutal to retrofit, so possibly Fundamentals |
| 18 Releasing | QoL | Only matters once you ship to someone else |

Where a section splits across tiers, **split the section** — do not leave it whole with a
hedged label. A rule that lives in two tiers helps nobody.

---

## The portability axis

Mark **every item**, not every section. Keep the marks short and put them where the eye
lands — a leading badge on the bullet is fine, a footnote is not.

| Mark | Means | Test |
|---|---|---|
| **U** — Universal | Copy the mechanism as-is. Nothing about it is D4. | Would this rule read identically in a tool for a game you have never heard of? |
| **A** — Adapt | Same mechanism, engine-specific data behind it. | Is the *shape* right but the ids / paths / taxonomy different? |
| **E** — Engine-specific | D4's answer to a problem another engine may not have. | Could a target engine reasonably have no equivalent at all? |

An **E** item must say what the *underlying question* is, so a reader can decide whether
their engine even asks it. "D4 gates classification on an authored slot tag" is E; "classify
by authored data, never by a name substring" is U. Get that distinction right on every one —
it is most of the value of this pass.

### And say what to skip

Add, per tier or per section as fits, an explicit **"skip this unless…"** line wherever an
item is real bloat for a smaller tool. The user's words: *many things do not translate to
other engines, or would not be necessary, and are bloat.* The document currently implies
everything should be ported. It should not.

Examples of the judgement wanted: the GIF budget ladder is superb and irrelevant to a tool
nobody will make turntables with. Bulk Extract's pause/ETA/manifest machinery is weeks of
work that a five-hundred-asset game does not need. The Information settings tab is worth it
only once the option set is confusing.

---

## Hard constraints

**Preserve every measured number and every war story.** The value of this document is that
its rules cite the bug that produced them — the `wolfHead` torso, the three drifted search
parsers, the GIF dither inversion, the one-byte release body, `Ctrl`+double-click destroying
the selection. A tiering pass that summarises those away has destroyed the document. If a
scar's prose has to move, move it whole.

**Nothing gets invented.** If you cannot place an item confidently, ask. Do not resolve a
contentious tier by picking one and moving on quietly.

**Line endings: the file is CRLF.** Python's `read_text` / `write_text` silently normalise
CRLF to LF and have damaged files in this repo twice. Read and write **bytes**, convert `\n`
to `\r\n` explicitly, and assert `data.count(b'\n') == data.count(b'\r\n')` before writing.
For a restructure this large, generating the whole file once is more honest than a hundred
anchored patches — but verify the byte count and the section inventory before and after.

**Never run git in this folder through a file bridge.** It leaves `.git\index.lock` and every
later git command on Windows fails. The user runs git natively.

**`device_bash` does not work on this machine** (no Plan9 shares). Edit in the cloud
workspace and push with `device_commit_files`.

---

## Citations — all of them, proved

Renumbering breaks these. There are exactly three places:

1. **Inside the document itself — 29 internal references.** Current counts, so you can check
   your work: `§17`×6, `§16`×4, `§5`×3, `§3.10`×2, `§18`×2, `§12`×2, `§10`×2, and one each of
   `§3`, `§3.1`, `§3.2`, `§3.3`, `§3.5`, `§3.8`, `§8`, `§14`. Sub-references like `§3.10` point
   at *numbered list items* inside §3 — if you split §3 across tiers those break too, and they
   are the easiest ones to miss.
2. **`docs\README.md`** — the row describing the template names its section count and its
   topics. Update both.
3. **The `assetbrowser-design` skill** — it cites `§8`, `§10`, `§12`, `§14` in its Steps.
   **This is a skill, not a file.** Update it with the `propose_skills` tool (kind:
   `improvement`, target: `assetbrowser-design`), carrying the *complete* updated SKILL.md —
   read the current one first. Editing the synced copy under `.claude/skills/` changes
   nothing; that path is a read-only cache.

Nothing else in the repo cites this document. The `§` references in `wiki\*.md` point at
*Asset-formats*, and `docs\STABLE_PARITY_AUDIT.md`'s `§5` is its own section — leave both
alone. Verify that yourself rather than trusting this paragraph:

```
grep -rn "§" --include=*.md --include=*.py --include=*.bat .
grep -rln "ASSETBROWSER_TEMPLATE" .
```

---

## Suggested order

1. Read the document end to end. Read `docs\README.md` and the current `assetbrowser-design`
   SKILL.md too.
2. Produce a **placement table**: every discrete rule in the document → tier → portability
   mark → whether it splits out of its current section. Show it to the user and settle the
   contentious rows *before* restructuring.
3. Write the new document. Front matter first: what this is, how to use it, the tier
   definitions, the portability key, and a **minimum viable build path** — the ordered
   shortlist that gets a working browser for a new engine.
4. Fix the 29 internal references, then `docs\README.md`.
5. Propose the skill update.
6. Verify (below), then commit the files to the machine.

---

## Definition of done

- [ ] Every rule that was in the 567-line document is in the new one, or its removal is
      listed and justified in your report. Count the bullets both sides.
- [ ] Every measured number and every named scar survives, in prose, not summarised.
- [ ] Four tiers, each opening with its "if you stop here" sentence.
- [ ] Every item carries a portability mark; every **E** item names the underlying question.
- [ ] "Skip this unless…" guidance wherever an item is optional bloat.
- [ ] A minimum viable build path exists and is honest about what it leaves out.
- [ ] No dangling `§n`: every internal reference resolves to a section that exists. Write a
      throwaway checker for this rather than reading them — there are 29.
- [ ] `docs\README.md` updated.
- [ ] The skill update proposed via `propose_skills`, not written to disk.
- [ ] File is still CRLF; assert it.
- [ ] A short report: what moved, what split, what you would have cut and why you did not.

---

## Two things worth knowing about the user

They would rather you be slow and correct than fast and wrong: *"no rushing to finish, be
thorough and do it correctly rather than having to troubleshoot and fix over and over leaving
many mistakes."*

And they do not want inferred facts presented as established ones: *"no guessing… the tool is
supposed to be future proof — if you can find that this leads to that, meaning this, then
that — rather than the user told me this represents this so I can just use that."* If you
place an item on a hunch, mark it as a hunch and ask.
