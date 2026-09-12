# Exporting

## Models

Rigged, animated `.glb`. Exports **exactly what is visible** — hidden parts stay out unless
you picked them explicitly. Wardrobe exports the assembled outfit, Stable exports the mount,
Models exports the selection, and you can drag straight out of the list into Blender or
Explorer.

**Detail maps are baked in** (*Settings ▸ Export ▸ Models*). The game layers a tiling detail
normal and roughness over the base maps per dye zone — fabric weave, leather grain, scale
texture all live there. A plain material dump leaves it behind, which is why an exported piece
can look flat next to the same piece in the viewport. With this on, the detail layers are
composited into the exported normal and ORM zone by zone, exactly as the viewport does it, on
every path that writes a model.

**Animation libraries** — skeleton plus selected clips and no mesh, for retargeting.

## Images and GIFs

PNG (lossless, alpha), JPEG (smallest, no alpha) or WebP. Resolution 25–400%, and **above 100%
the scene is re-rendered larger** rather than a screenshot being upscaled. Optional transparent
background and crop-to-model.

GIFs are turntable or animation loop, with an **optimise-to-target-size** mode that tries
palette reduction, then dithering off, then aimed downscales, and ships the *smallest* result
rather than the last one it tried — and says plainly when a target is not reachable.
Turntables snap to whole animation loops so orbit and pose wrap together, and run a warm-up lap
so cloth has settled before the first captured frame.

## Textures

Single, batch, or every frame of an atlas. PNG or JPEG, optionally trimmed to non-transparent
bounds. Filenames follow templates using `{{FileName}}`, `{{SNO}}`, `{{FrameIdx}}` and
`{{FrameName}}`.

## Catalogue

A whole bundle into its own folder — `models\`, `art\`, `icons\`, and a `manifest.json` naming
everything that resolved **and** everything that did not. Several bundles in one run with Multi
select. The Export menu names what it will act on before you commit.

## The normal-map convention

Diablo IV authors normal maps DirectX-style, with green pointing down the texture. glTF and
Blender expect OpenGL. Exports flip the green channel to match by default; the Unreal/Skyrim
preset turns that off because its target wants the channel as the game stores it. Every export
names the convention it wrote in its completion message, either way.

This is worth reading twice if a model looks subtly wrong-lit in your DCC — it is the single
most common cause, and the direction of the mistake is not visually obvious.

## Modding and retarget options

*Settings ▸ Export ▸ Advanced*: engine presets (Blender, Unreal/Skyrim, Unity — unit scale and
normal convention), rebuild the normal map's blue channel, readable bone names, hardpoints as
empties, Blender-friendly `.L`/`.R` rig names, symmetrise for X-Axis Mirror, reduce to a
26-bone humanoid rig, strip cloth chains, include the base body as a fit reference, and batch a
whole armour set with a manifest.

## Shortcuts

`Ctrl+E` export selection · `Ctrl+Shift+E` export to last folder · `Ctrl+Shift+A` animations
only · `Ctrl+Shift+I` save preview image. All rebindable in *Settings ▸ Hotkeys*.

## Which option do I actually want?

*Settings ▸ Information* answers that inside the tool — including a side-by-side of the two
ways to get loose texture files, which look interchangeable and are not: one copies the maps a
model already decoded, the other decodes every map in the material whether the model uses it
or not.
