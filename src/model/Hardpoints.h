#pragma once
#include "model/ModelGeometry.h"

#include <QString>

// Rig hardpoints (attach points) for a model, read from its .app.json.
// Populated into ModelGeometry::hardpoints on export when the user opts in, so weapon
// grips / sheaths / trail emitters / look-at sockets come across as named empties in the
// exported .glb — the exact game transforms for aligning props in Blender or other games.
namespace Hardpoints {

// Human-readable name for a hardpoint hash. Verified table (D4_BoneHash_Research_Report.md);
// returns "HP_<8hex>" for hashes not in the table.
QString nameForHash(quint32 hash);

// Parse `<appJsonPath>` (…/json/base/meta/Appearance/<name>.app.json), read
// tStructure.ptBoneData[0].ptHardpoints, and append a ModelHardpoint per entry whose
// nBoneIndex is valid for `geo.skeleton`. Records the parent bone's nameHash so the
// index survives later skeleton reorder/reduction (call resolveBoneIndices after retarget).
// Returns the number appended (0 if the file/section is missing).
int readInto(ModelGeometry& geo, const QString& appJsonPath);

// After any skeleton edit (retarget remap/collapse, rename), rewrite each hardpoint's
// boneIndex from its boneHash against the CURRENT geo.skeleton; drop hardpoints whose
// parent bone no longer exists. Call right before exportGlb.
void resolveBoneIndices(ModelGeometry& geo);

}  // namespace Hardpoints
