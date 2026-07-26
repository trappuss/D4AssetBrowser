#pragma once
#include "model/ModelGeometry.h"

#include <QVector3D>

// ── Retarget & modding transforms for .glb export ────────────────────────────
//
// Pre-export skeleton/weight rewrites for porting D4 models onto other games'
// rigs (see Settings ▸ Export ▸ "Retarget & modding" and docs/MODEL_EXPORT.md §6).
// All functions operate on the caller's COPY of the geometry — never the live
// preview — and are safe no-ops when the model doesn't qualify.
namespace Retarget {

// Remove the game's simulated cloth/physics bones (skeleton index >= nBaseBones)
// and fold their skin weights into the nearest kept ancestor. Returns the number
// of bones removed (0 when nBaseBones is unknown/degenerate).
int collapseClothChains(ModelGeometry& geo);

// Merge skin weights up into the 26 identified player-rig anchor bones (see
// GLModelWidget::blenderizeSkeletonNames — pelvis/chest/head, arm & leg chains,
// weapon attach…) and reduce the exported skeleton to exactly those anchors.
// Every non-anchor bone's weights fold into its nearest anchor ancestor, so the
// exported vertex groups line up with typical humanoid game rigs.
// Anchors keep their world bind transforms (inverse bind matrices unchanged);
// their local TRS is recomputed against the new (anchor-only) hierarchy.
// Returns false — geometry untouched — when the rig doesn't carry enough of the
// player anchors (monsters, props, mounts).
bool remapToAnchors(ModelGeometry& geo);

// Apply the retarget QSettings toggles (retarget/collapseCloth, retarget/remapWeights)
// to an export copy, in the right order.
void applyFromSettings(ModelGeometry& geo);

// Left/right mirror-partner table for a skeleton: partner[i] = index of bone i's mirror
// partner, or -1 (center / unpaired). Curated player-rig pairs (by hash) take precedence;
// remaining bones use reciprocal nearest-neighbour of D4-native rest bone-head positions
// across the sagittal plane (D4 Y=0, character's left = +Y), 6 mm tolerance, never
// overriding an existing pair. This is the SINGLE source of pairing for both the
// .L/.R bone naming and the X-mirror rig symmetrization (see D4_XMirror_Spec.md —
// geometric-only matching gets fooled where weapon/hand/handEnd share a position).
QVector<int> mirrorPairs(const QVector<ModelJoint>& skeleton);

// D4-native rest bone-head world positions (compose of restT/restQ/restS down the
// parent chain). Shared by pairing, naming and the exporter's symmetrization.
QVector<QVector3D> restHeadsD4(const QVector<ModelJoint>& skeleton);

}  // namespace Retarget
