# Diagnostics

Everything here reads data the tool already has. None of it decodes pixels, writes to your
game folder, or changes what is on screen.

**Which one do I want?**

| The question | The report |
|---|---|
| What *is* this id or name? | [Find SNO](#help--find-sno) |
| Why does this one part look wrong? | [Explain this material](#right-click-a-part--explain-this-material) |
| Is my setup complete and current? | [Health check](#help--health-check) |
| What did the last game build add? | [Patch contents](#help--patch-contents) |
| What changed since my last check? | [Audit - Asset Health.bat](#audit---asset-healthbat) |
| Why are Catalogue rows blank? | [Probe - Catalogue Icons.bat](#probe---catalogue-iconsbat) |
| Is the icon audit checking itself? | [D4_NO_DAD_FORCE](#is-the-icon-audit-grading-itself) |
| What exactly failed, and why? | [The log](#the-log) |

If you are chasing a visible fault rather than a question about the data, start at
[Troubleshooting](Troubleshooting) — it routes by symptom.

## Help ▸ Find SNO

Paste an asset id, or part of a name. You get the asset group, the name, the collection it
belongs to, how many appearances use it if it is a material, and whether it arrived in this
game build.

This is the question most investigations open with. `2462986` is a wolf-head ornament
material used by the Paladin storefront set — knowing that used to mean grepping a 43 MB
metadata dump next to a tool that holds the same table in memory.

A name search is a substring match across every group, capped at 80 results but counting all
of them, so you are told how many you did not see.

## Help ▸ Patch contents

What each game build **added**, newest first, grouped by asset type and named.

The record is observational: nothing in the game's own tables stamps an asset with the build
that introduced it, so this can only be captured by being present when it happens. A build the
tool was never opened on cannot be reconstructed, and when two observed builds are not
consecutive the entry says what it was actually diffed against rather than implying one patch.

## Help ▸ Health check

Storage, keys, snapshot freshness and live format probes in one screen. The first thing to read
after a game update. Its *snapshot coverage* row is the gap between what the game has and what
the metadata snapshot describes.

## Help ▸ Diagnostic output

The reports the `Dump - *.bat` probes write beside the exe — listed newest first with size and
age, and readable in place instead of hunting through the folder for whichever file just
changed.

The list is the folder itself, not a table of probe names, so a new probe shows up without
anything being updated to know about it.

## Right-click a part ▸ Explain this material

The single most useful report in the tool when something looks wrong. It answers, in order:

- is this appearance encrypted, and do we hold its key
- where the material roster came from — the metadata snapshot, the game's own binary, or nowhere
- which material this part actually resolved to
- which of its values are **authored** and which are stand-ins the tool substituted
- every texture role it references, and whether that definition resolves
- **every appearance that uses this material**

That last one tells a piece's own material from one shared across a set — or across both
genders, which is how several Paladin and store sets are authored, and which is invisible from
the name alone.

The authored-versus-assumed distinction is the point of the whole report. A material reporting
roughness 0.6 looks identical whether the game authored 0.6 or the tool gave up and picked it,
and that ambiguity is what let encrypted content look merely ugly instead of unread for months.

## Audit - Asset Health.bat

Walks every appearance and classifies what it can and cannot produce — payload, geometry,
materials, texture definitions — then **diffs against your previous run**. This is the one to
run after a patch and attach to an issue.

## The log

`data\D4AssetBrowser.log` names the exact asset that failed and, where relevant, the decryption
key it needed. **Help ▸ Copy log to clipboard** puts it straight on the clipboard for pasting
into an issue.

## Is the icon audit grading itself?

The audit compares the tool's appearance icons against the diablo4.dad database and reports
0 diffs. It cannot report anything else, because the appearance crawl's last step **copies**
diablo4.dad's handles over the ones the tool worked out for itself — so the audit is checking a
copy of its own reference. If the tool's own route broke after a patch, this would still read 0.

Set `D4_NO_DAD_FORCE=1` before launching and run the audit again. That step is skipped, and the
DIFF count becomes the real answer: how many appearances the tool resolves differently on its own,
with the sample naming them. Nothing else changes, and the metadata cache is kept separate so a
normal run afterwards is unaffected — each switch costs one rebuild of the appearance index.

### The answer, measured

Run on the full 10,769-appearance corpus:

| | forced (default) | `D4_NO_DAD_FORCE=1` |
|---|---|---|
| ok | 10,769 | 10,428 |
| **missing** (no icon at all) | 0 | **311** |
| **diffs** (a different icon) | 0 | **30** |
| no sprite in the local atlas | 5 | 35 |

So the copy is not hiding a working route — it is carrying one. **Keep the forced step.**

The 311 missing are dominated by mounts and mount trophies (202), then two-handers (40) and the
rest of the weapon families; without the copy those appearances resolve no handle at all.

The 30 diffs are one coherent family, which is what makes them worth reading rather than counting:
every `*_sets56_*` piece, for all six non-Sorceress class/gender combinations, across all five
armour slots. Their item record is the shared `*_Legendary_Generic_056`, so the tool's own route
resolves the handle that record carries — which belongs to `sorM`/`sorF`. Without the copy, six
classes wear the Sorceress's icons for that whole set. The audit says so itself on every line:
`[tool handle belongs to: sorM_sets56_HLM — cross-wired]`.

The cost of keeping it is nil: the Catalogue's GRID THUMBNAILS and COVERAGE sections are
**byte-identical** between the two runs. The copy is an appearance/item-icon matter only, so no
Catalogue blank can be explained by it — a useful thing to know before spending a session there.

## What the icon audit says about Catalogue rows

The audit's GRID THUMBNAILS section walks every row the Catalogue lists and reports the four
routes a row's picture can come from, **in the order the tab tries them**, so each row is
attributed to the route that actually draws it rather than to every route that could have:

1. art named after the product (the filename templates, plus the structural scan that catches
   prefixes the templates do not know);
2. the product's own authored art handle;
3. the icon of what the row **contains**;
4. the payload actor's own portrait — all a companion or a mount trophy has.

Routes 3 and 4 are asked of each row's **leaf** contents, the same descent the strip, the tree,
the export and the manifest use, because a bundle has no payload of its own — its pieces do. Two
groups are reported as not checked rather than folded in: locked records, which are unknowable to
anyone without the TACT key, and rows whose leaves carry only a payload SNO, which the audit's
worker thread must not resolve (the tab, on the GUI thread, does resolve them).

## Probe - Catalogue Icons.bat

Answers "why does this Catalogue row have no picture" from the index files the app has already
written, in about a second. It does not build anything and does not launch the app, so it is
the fastest way to check coverage after a game update — and the right thing to run *before*
concluding the tool is broken.

The report is printed to the console **and** written to `data\catalogue_icon_probe.txt`, so it
is still there after the window closes and appears under [Help ▸ Diagnostic
output](#help--diagnostic-output) with everything else.

It reports, for every row the Catalogue lists:

- how many resolve art by name, and how many only the "Has icon" filter can find;
- how many would resolve through the product's own artwork, and whether that artwork is
  actually theirs or a banner shared across a family — a ratio near 1.00 means each row would
  show its own picture, a low one means the same image repeated down the page;
- how many resolve through the icon of what they contain;
- and what is still blank, split into locked records, products whose every image is shared,
  and products with no artwork at all.

The counts are a snapshot of the app's index files. After a game update, launch the app once
so it refreshes them, then run this. It finds those files by name regardless of their version,
so a format change does not stop it working — which is exactly when it is most useful.

It cannot tell whether an image *decodes*, and it cannot see which records are encrypted; the
in-app **File ▸ Icon audit** covers both and the two agree to within one row.
