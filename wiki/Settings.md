# Settings

**File ▸ Settings**, or <kbd>Ctrl</kbd>+<kbd>,</kbd>.

Nine tabs, ordered by how often you touch them and what they are about: setup → presentation
→ the three per-area pages → what lands on disk → keys → upkeep → reference → experimental.

Everything is stored in `data\` beside the exe. There is no registry footprint outside Qt's
own settings, and deleting `data\` returns the app to a first run without touching the game.

---

## The options that change *results*

Most settings change what you see. These change what you get, so they are worth knowing
before you export anything.

### Bake detail maps — Export ▸ Model export

**Leave this on.** The game layers a tiling detail normal and roughness over the base maps
per dye zone — fabric weave, leather grain, scale texture. A plain material dump leaves it
behind, which is why an exported piece can look flat next to the same piece in the viewport.
With this on, the detail layers are composited into the exported normal and ORM zone by zone,
exactly as the viewport does it, on every path that writes a model.

### The normal-map target — Export ▸ Model export

Diablo IV authors normal maps **DirectX**-style: green points toward the bottom of the
texture. glTF mandates OpenGL, and Blender is OpenGL. Pick the preset that matches where the
model is going and the green channel is handled for you; get it wrong and every surface
lights inverted. [The long version](Exporting#the-normal-map-convention).

### Modding / retarget — Export ▸ Modding / retarget

Changes the skeleton and naming the `.glb` is written with, for porting into another engine
or retargeting onto a different rig. It is not a cosmetic option — a file written with these
on is a different file.

### Texture export — Export ▸ Texture export and Templates

Format, trimming to non-transparent bounds, and the filename template
(`{{FileName}}`, `{{SNO}}`, `{{FrameIdx}}`, `{{FrameName}}`). Atlases expand to one file per
frame, so the template is what stops a 60-icon sheet becoming 60 files called `icon`.

---

## The tabs

### General — where the data comes from

| Box | What lives there |
|---|---|
| **Directories** | The two folders the tool reads: your Diablo IV install and the community JSON snapshot. [Install](Install#what-the-two-downloads-are) explains why both. |
| **Game data** | How the game install is read. |
| **Updates** | Update checking. |
| **Settings profile** | Export your whole configuration to a file and import it back — the way to move a setup between machines, or to keep a known-good state before experimenting. |

### Interface — what the app shows you

Split out of General once that page had grown to seven boxes spanning four unrelated
concerns.

| Box | What lives there |
|---|---|
| **Startup & layout** | Which tab opens, how panels are arranged. |
| **On-hover previews & info** | The preview popups and what they say. |
| **Icon indicators** | The badges drawn on list and grid entries. |
| **Diagnostics** | Which probe files the app writes beside the exe. See [Diagnostics](Diagnostics). |

### Models

| Box | What lives there |
|---|---|
| **Browsing & loading** | How the list behaves and what loads when. |

### Wardrobe

| Box | What lives there |
|---|---|
| **Outfit & preview** | How outfits assemble and what the preview shows. |
| **Weapons** | Held versus sheathed, and the per-class hardpoint handling. |
| **Performance** | The cost/quality dials for the live viewport. |

### Export — everything that lands on disk

The largest page, and the one worth reading once end to end.

| Box | What lives there |
|---|---|
| **Texture export (Textures tab)** | Format, trimming, resolution. |
| **Templates** | Filename patterns. |
| **Model export (.glb)** | Detail baking, the normal-map target, what a "part" export includes. |
| **Wardrobe** · **Catalogue** · **Bulk Extract** | Per-surface export behaviour, because each answers a different question about scope. |
| **Image & GIF capture** | Resolution, transparent background, crop-to-model, GIF optimisation. |
| **All exports** | The settings that apply to every path. |
| **Modding / retarget (.glb export)** | Skeleton and naming for other engines. |
| **Loose textures — which option writes what** | A written-out table, because "export the texture" means three different things depending on where you started. |

### Hotkeys

Six rebindable export and capture actions. Defaults and reasoning are on
[Keyboard & mouse](Keyboard-and-mouse#export--capture--rebindable). Rebinding takes effect
immediately — no restart. Leave one blank to unbind it.

### Maintenance

| Box | What lives there |
|---|---|
| **Caches & reset** | Clear the icon, texture and model caches; clear Stable memory; restore defaults. |

**Restore Defaults is scoped.** It resets the viewport group you are looking at rather than
every viewport in the app, and it works by *removing* keys rather than writing defaults over
them — so a setting the app has never written stays unwritten, and a later change to a
default is actually picked up. Saved looks, camera presets and cloth presets are preserved
across a reset on purpose.

### Information

Reference, not configuration. What the tool has loaded, what it found, and the version and
build details you would paste into an issue.

### Experimental

Deliberately the far-right tab: the least-used page and the one most likely to be opened by
accident, so it sits at the end rather than between everyday settings.

| Box | What lives there |
|---|---|
| **Indexing (advanced)** | How the asset index is built. |

---

## Two behaviours worth knowing

**Settings are read back, not assumed.** Several past bugs were a setting that was written
and never read, or read under a different key than it was written with — a slider showing 15
while the viewport rendered at 24, a column layout saved on every change and never restored.
`verify-src.py` now fails the build on a settings key that is written and never read, and on
a combo box persisted by its display label instead of its value, so that class of fault
cannot come back quietly. See [Building from source](Building#verify-srcpy).

**Mirrored controls stay in step.** Where the same setting appears in two places — the
collision-model checkbox exists in both the viewport overlay menu and the Physics panel —
both are driven by one value and both update together.
