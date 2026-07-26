#pragma once
#include "model/ModelGeometry.h"

#include <QByteArray>
#include <QJsonObject>

// Parses a Diablo IV .app model (paired meta + payload buffers) into a
// ModelGeometry. Port of d4extract's app_parser.py LOD0 path:
//   - GeoChunkVertexBuffer / GeoChunkIndexBuffer scan (fOptional == LOD0)
//   - layout-driven vertex decode (stride 36 static / 44 skinned)
//   - SubObjectSegment → SubObject pairing → one primitive per draw call
//   - z_up_to_y_up coordinate conversion (x, y, z) → (x, z, -y)
//
// v1 emits static geometry (POSITION / NORMAL / TEXCOORD_0 + per-submesh
// material index). Skinning (JOINTS/WEIGHTS + skeleton) is a documented
// follow-up; skinned .app files still export as rigid meshes here.
namespace ModelParser {
// appName (optional): the appearance's name — stamped into each parsed ClothSim
// (srcApp) so resolveClothTuning can use the game's authoritative snoCloth link.
// Callers that don't know the name keep the legacy name-heuristic fallback.
ModelGeometry parseApp(const QByteArray& meta, const QByteArray& payload,
                       const QString& appName = QString());

// Cloth-only extraction: parses just the embedded ClothData blocks (capsules + sim
// cages + authored driving arrays + tuning name) WITHOUT the mesh/skeleton work.
// Used by the D4_CLOTH_AUDIT corpus sweep, where parsing every appearance's full
// geometry would multiply the run time for data the audit never looks at.
void parseClothOnly(const QByteArray& meta, const QByteArray& payload,
                    QVector<ClothCapsule>& caps, QVector<ClothSim>& sims,
                    const QString& appName = QString());

// Resolve a ClothSim's per-piece tuning (tClothTuning object; empty = unresolved).
// SINGLE implementation used by the wardrobe fill, the Models/Stable-tab fill and the
// D4_CLOTH_AUDIT sweep. Resolution order:
//   1. snoCloth — the game's own per-item physics link: parse the piece's
//      Appearance/<srcApp>.app.json, pair this block by srcOffset (sub-object
//      dataOffset → nMaterialIndex → material's snoCloth), open that exact
//      Cloth/<snoCloth>.clt.json. Authoritative; cross-gender refs (rogF piece →
//      rogM cloth) only resolve this way.
//   2. Embedded-name fallback (pieces shipping no snoCloth, e.g. barF_stor263_HLM):
//      suffix conventions (_sim / bare / _HQ_sim), then the prefix fallback.
// *howOut (optional): "snoCloth:<file>" / "_sim" / "bare" / "_HQ_sim" /
// "prefix:<file>" / "FAILED".
QJsonObject resolveClothTuning(const QString& d4dataDir, const ClothSim& sim,
                               QString* howOut = nullptr);

// Combine several parsed appearances into one ModelGeometry for outfit/character
// assembly: concatenates primitives (preserving material names + slot hashes) with
// index offsets. Static merge (skeleton dropped) — pieces share the bind-pose space.
ModelGeometry mergeGeometries(const QVector<ModelGeometry>& parts);

}
