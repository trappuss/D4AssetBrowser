# Diagnostics

Everything here reads data the tool already has. None of it decodes pixels, writes to your
game folder, or changes what is on screen.

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
