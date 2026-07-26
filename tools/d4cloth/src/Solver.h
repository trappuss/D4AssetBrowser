#pragma once
// d4cloth — the CORRECTED solver: authored data end-to-end (see FINDINGS.md and
// ROOTCAUSE.md for the evidence behind each design point).
//
// Differences from LegacySolver, in causal order:
//   1. Cage-particle kinematic targets come from the AUTHORED cage skinning —
//      ptDriverInfluences (4×u16/vert) + ptWeights (vec4/vert, sums=1) against the bones
//      the piece's ptDriverMap binds its driver frames to. Per-piece by construction:
//      a cape target can never come from a skirt bone (removes ROOTCAUSE link 1).
//   2. Bones are driven by the AUTHORED follower map (ptFollowerIndices): the bone head
//      IS its particle's position (rest alignment ≤5 µm) — no anchor search, no
//      (pos − target) arithmetic (removes ROOTCAUSE link 4), no cross-garment latching.
//   3. The motion constraint is the authored TETHER: |P_k − root_k| ≤ attachmentLengths[k]
//      (absolute wu; root = the kinematic root particle's CURRENT position). No cageSpan,
//      no tnorm², no skinned-pose reference.
//   4. Real particle set is [0, vertexCount); padding self-pair constraints are dropped.
//   5. Distance constraints are solved per authored CLASS (warp/weft/shear/bend cluster
//      ranges) with the class's .clt.json stiffness; gravity is the authored vGravity.
//   6. Cloth bones with no follower entry (e.g. the hood, which ships no ClothData) run
//      the pre-24-July pure spring-bone path — never cage-driven.
//
// Deliberate non-authentic choices (documented; revisit with evidence):
//   · Verlet damping keeps the app's global `damping` (0.93) as velocity retention —
//     NvCloth's damping/drag semantics differ; authored flDragFactor additionally scales
//     velocity by (1 − drag·0.1) per step.
//   · Iteration count: max(authored nIterations, 4) × app iteration scaling, since this
//     PBD solve replaces NvCloth's semi-implicit solver.

#include "LegacySolver.h"

namespace d4cloth {

class Solver : public LegacySolver {
public:
    explicit Solver(const Scene& sc) : LegacySolver(sc) {}

    void step() override;

    // ── authored cage runtime (parallel to `cages`; built on first step) ──
    struct AuthoredCage {
        int docIdx = -1;                 // into sc.docs / docTuning / docDriverBoneUnified
        int nv = 0;                      // REAL particle count (vertexCount)
        QVector<std::array<quint16,4>> inf;   // driver influences per vert
        QVector<std::array<float,4>>   w;     // weights per vert
        QVector<int>   driverBone;       // unified bone per driver (-1 = unresolved)
        QVector<int>   con;              // filtered constraints: a,b pairs (endpoints < nv)
        QVector<float> conRest;
        QVector<float> conStiff;         // per filtered constraint, from its cluster class
        QVector<int>   tetherRoot;       // per vert: kinematic root vert (-1 = none)
        QVector<float> tetherLen;        // per vert [wu]
        QVector<float> pos, prev, target;
        QVector<quint8> pinned;
        bool seeded = false;
        bool usable = false;             // authored arrays complete enough to run
    };
    QVector<AuthoredCage> acages;

    // per-step diagnostics beyond the base class
    int   tetherHits = 0;                // particles clamped by the tether this step
    float worstPinnedTargetErr = 0;      // max |pinnedTarget − v2b bone anim pos| (driver-table validation)

private:
    void buildAuthored();
    bool m_authoredBuilt = false;
};

} // namespace d4cloth
