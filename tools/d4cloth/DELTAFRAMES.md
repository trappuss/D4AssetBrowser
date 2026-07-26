# Project brief: ptDeltaFrames cage-driven render skinning (game-exact cloth)
Goal: skin cloth RENDER verts to the simulated CAGE (as the game does) instead of bones.
Closes: residual leg clipping, coarse folds. Prereq reading: tools/d4cloth/FINDINGS.md, skill d4browser-debugging.

## Data
ptDeltaFrames @ ClothData+? (see ClothDoc.cpp offsets table): capacity×64 B (e.g. 104 verts → 6656 B).
Hypothesis: per cage vert a bind FRAME — candidates: quat(16)+pos(16)+2×vec4 aux; or 3×vec4 basis+origin;
likely relates render verts to cage triangles (cf. ptTangentIndices, ptBindNormals — both parsed, unused).

## M1 — decode (harness, corpus already extracted)
1. d4cloth inspect: dump 64B rows as 16 floats for barF_base03_TRS cape; test candidates:
   unit quats? orthonormal 3x3? positions matching ptBindVertices (zUpToYUp!)?
2. Validation target: reconstruct each SIM-submesh render vert at REST from (frame_i, weights)
   to <1mm. Weights source: the render mesh's own skin weights may be reused, or ptWeights rows.
3. Add findings to FINDINGS.md as F4 with measured residuals; goldens for 2 pieces.

## M2 — app port (GLModelWidget)
1. Identify cloth render submeshes (m_partCloth / SIM material names) → per vert: nearest cage
   frames + rest offset (or authored mapping if M1 finds one).
2. After cage solve: pose frames from particle positions + triangle normals; skin those verts
   from frames INSTEAD of the bone palette (keep bone path for non-cloth verts + fallback).
3. Keep bone drive for the overlay/exporter. Gate behind a setting until visually verified
   vs in-game footage. Verify: thigh no longer pokes hem in spiF_stor214_LEG walk.

## Guardrails
Instrument first; "nothing changed" = wrong path. No per-model constants. Planes stay OFF.
Run Cloth Audit.bat + repro list (skill) after. Use git before starting.
