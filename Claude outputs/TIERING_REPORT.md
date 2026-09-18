# Report — tiering the AssetBrowser template

`docs\ASSETBROWSER_TEMPLATE.md`: **567 lines / 38,657 bytes → 910 lines / 58,119 bytes**, still
pure CRLF (asserted `LF == CRLF` before the write, and again after reading the file back off your
disk). Both changed files were read back **md5-identical** to the container copies. No git was
run in the folder; `src\` untouched.

## What moved

| Was | Is now |
|---|---|
| Intro + §14 porting guide | **Front matter** — what this is · how to use it · the four tiers · the portability key · engine-specific vs generic · **the minimum viable build path** · asking a session for a feature |
| §1 Product identity | §1, tier 1 (whole) |
| §2 Architecture skeleton | §2, tier 1 (whole) |
| §3 Cross-cutting conventions | **§3, tier 1, unsplit and deliberately still numbered 3** |
| §4 Browse tab | §4 list + one matcher (t1) · §9 filters (t2) · §21 Outliner/Grid/hover (t3) |
| §5 3D viewport | §5 shared widget + flat + camera (t1) · §10 shading/channels/overlays (t2) · §11 part selection as a set (t2) · §22 polish (t3) |
| §6 Panel system | §12 the panel set (t2) · §23 the stack machinery (t3) |
| §7 Textures tab | §13 core (t2) · §24 extras (t3) |
| §8 Bulk Extract | §14 the run (t2, **tier-by-intent note**) · §25 machinery (t3) · *output layout moved to §15* |
| §9 Exporting | §6 single-model export (t1) · §15 options/layout/names (t2) · §26 GIF ladder (t3) · §27 retarget presets (t3) |
| §10 Settings dialog | §16 skeleton + mechanics (t2) · §28 Information tab + profiles (t3) |
| §11 Hotkeys | §29 (t3) |
| §12 Context menus | §17 (t2, whole) |
| §13 Tooling | §7 build + verify-src.py (t1) · §18 log console (t2) · §8 link checker (t1) · §30 audit/github/release-notes (t3) |
| §15 QoL checklist | §20 — this tier's index, all 22 boxes, each with a pointer |
| §16 Tool explains itself | §18 Explain-this + Find-id + instrument-first (t2) · §31 Health check, Patch contents, Diagnostic output (t3) |
| §17 Documentation | §8 the skeleton (t1) · §19 the manual (t2) |
| §18 Releasing | §32 (t3) |
| — | **§33 the game-specific showpiece tab (t4)** — see below |

**§3 keeping its number is the one deliberate exception to free renumbering.** Its ten items are
cited individually as `§3.1`…`§3.10` from six places inside the document, and sub-references are
the easiest ones to break. Leaving the section whole and at number 3 means all six resolve
unchanged and no sub-numbering had to be re-derived.

## Citations

- **29 internal references before → 127 after, zero dangling.** Every one was rewritten by hand
  and then proved by a throwaway checker that parses every `§n` and `§n.m` in the file, resolves
  `n` against the real `### n.` headings and `m` against §3's real numbered items, and reports
  anything unresolved. It also asserts the section numbers are contiguous 1..33.
- **`docs\README.md`** — the template's row now states 33 sections, the four tiers, the
  portability marks, the minimum viable build path, and the standing rule about §3's number.
- **The `assetbrowser-design` skill** — proposed via `propose_skills` (kind `improvement`), not
  written to disk. `§8 → §14`, `§10 → §16`, `§12 → §17`, `§14 → the front matter`, `§16 → §18/§31`,
  `§17 → §8/§19`; "18 sections, extend a section never renumber one" becomes the tier/mark
  summary plus the §3 rule. Its Steps now ask a session to name the **tier** as well as the
  section, and to build tiers 1–2 when asked for a minimum viable version.
- **Your grep claims verified, not trusted.** Every `§` in every `.md`, `.py`, `.bat`, `.ps1`,
  `.txt` and `.yml` in the repo was inspected in context: `wiki\*.md`'s four point at
  *Asset-formats*, `docs\STABLE_PARITY_AUDIT.md`'s `§5` and `docs\CONTEXT_MENUS.md`'s nine are
  their own sections, `tools\d4cloth\PLAN.md`'s fourteen are its own, `docs\DD2_ADDON_CONTEXT.md`'s
  three are its own, and `docs\notes\STATUS.md`'s three point at a design doc. `ASSETBROWSER_TEMPLATE`
  appears in exactly one other file, `docs\README.md`. All left alone, correctly.
- **One thing I found and did not change:** `docs\CONTEXT_MENUS.md` line 582 says *"from the
  central `Hotkeys` registry (§ the template's Hotkeys section)"* — a reference to this document
  with no number, so renumbering cannot break it. It could now cite `§29`; say the word and I will.

## One content error fixed

`§2`'s source-tree comment read `util/ … (each anchored by a real #include, see §14)`. §14 was the
porting guide and says nothing about anchored includes — the rule is `verify-src.py`'s
*"header-only helpers used without a real anchored `#include`"*, which was §13. It now points at
**§7**, where that check lives.

## Preservation, checked mechanically rather than by eye

- **131 discrete items before → 204 after.** Nothing was removed; the increase is prose-only
  sections (§7 Textures, §11 Hotkeys, §12 Context menus, §14 Porting) becoming marked bullets,
  because every item has to carry a portability mark.
- **22 of 22 checkboxes** survive verbatim in §20.
- **All 134 backticked identifiers** in the old file are present in the new one (0 missing).
- **Every measured number and every named scar** was checked by name: `wolfHead`, `primSlot >= 0`,
  `trophy_*` vs `back_*` with its six placeholders and zero real trophies, `Hero_eyes_mat` vs
  `global_eyeball_mat`, the three drifted search parsers, the Qt press→release→DoubleClick→release
  swallow flag, the Bayer/dither inversion with the ×0.75 ladder and `sqrt(target/actual) × 0.93`,
  the one-byte release body, the eleven releases that disagreed on a `v`, "eneral",
  24 of 133 groups and 109 unnamed, ~10% incomplete and 93 unnamed-but-decoding, sixty visibility
  passes, 30-deep undo, the fortnight-old missing-parts question settled by one primitive count.
- A word-level diff of the old file against the new flagged **13 words absent**, of which 3 were
  real losses and were restored: *"every list-driven tab"* (§4), *"Five that earn their place in
  every tool"* and *"commonly deferred… you are debugging opaque binary data by eye"* (§18, plus a
  line in the build path). The other 10 are renamed section titles ("Tooling **around** the tool",
  "Porting **guide**") and line-break artefacts (`reset-by-` / `removal`).

## Tier 4 — the honest answer

The document never specified a showpiece tab. Wardrobe, Stable and Catalogue appear only as
passing mentions: §14's engine-specific list, §10's Wardrobe settings page and Export sub-tab,
§15's 30-deep undo, §5's snap-to-slot. **§33 collects exactly those commitments and nothing else**,
states the underlying question for the whole tier (*what is the one thing your game lets players
assemble that no generic browser could show them?*), and says in its first line that this is the
one section not written yet. It then points at the three places in the repo where D4's own
showpiece work *is* written down — `docs\CONTEXT_MENUS.md`, `docs\STABLE_PARITY_AUDIT.md`,
`docs\notes\BUNDLES-TAB-RESEARCH.md`. Nothing about D4's Wardrobe UX was invented to fill it.

## What I would have cut, and why I did not

1. **§16's Settings tab list** names D4's own tabs (Models, Wardrobe, Catalogue, Bulk). For
   another engine it is unusable literally. Kept because the *ordering philosophy* is the rule and
   the list is its only worked example; marked **[A]**.
2. **§29's four shipping hotkey defaults** are pure D4 trivia. Kept because they are the worked
   example of "adding a shortcut = adding one row", and a reader porting the registry wants to see
   a populated table. Marked **[A]**.
3. **§19's twelve wiki pages** include rows any project would guess (FAQ, Glossary). Kept because
   the value is in the *Answers* column — "Keyboard & mouse, *generated by reading the source*",
   "Troubleshooting, **routed by symptom**" — not in the page names.
4. **Drag-out appears twice**, once as a texture action (§24) and once as a model action (§15).
   Looks like duplication; it is two surfaces with two different path rules, and the model one
   carries the output-layout exemption. Both kept.
5. **Collision shapes and physics bones** (§10) are so D4-specific they are nearly noise in a
   generic document. Kept as **[E]** with the underlying question stated, because that is exactly
   what the E mark is for — a reader can now skip them in one glance instead of wondering.
6. **The GIF budget ladder** is the strongest candidate for deletion in a template aimed at other
   engines. Kept whole, with the bluntest skip-line in the document: *"Skip this entirely unless
   somebody will make turntables with your tool."*

## Where the content resists the scheme — stated, not forced

1. **§32 Releasing is not polish**, it is distribution. It sits in tier 3 because it is genuinely
   skippable, and the tier opener says so rather than pretending otherwise.
2. **§23 (panel persist) and §29 (hotkey registry) are tier-3 features with tier-1 costs** — each
   is an afternoon now and a refactor later. The tier opener names both.
3. **§3.3 and §3.4 are tier-1 conventions whose payoff arrives in tier 2.** That is what a
   conventions section is for, but a reader stopping at tier 1 deserves to be told, and each item
   now says where its feature lands.
4. **§14 Bulk extraction is tier 2 by default and tier 1 by intent** — per your decision, with the
   note written into the section rather than hedged in its label.
