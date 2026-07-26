#pragma once
// d4cloth — Scene: a merged outfit in memory, mirroring the state GLModelWidget holds.
//
// Loads a test case (piece list) from the corpus, parses each piece with the app's own
// ModelParser, merges with mergeGeometries, and builds the exact arrays the solver code
// consumes: interleaved bind verts (11 floats: pos3 normal3 uv2 tangent3), per-vertex
// joints/weights, the merged skeleton with rest globals, authored capsules and ClothSims
// (with per-piece .clt.json tuning filled the same way WardrobeTab2::fillClothSimTuning
// does), and the full ClothDocs with follower indices remapped to the unified skeleton.

#include "AssetSource.h"
#include "ClothDoc.h"

#include "model/AnimParser.h"
#include "model/ModelGeometry.h"
#include "model/RigMath.h"

#include <QHash>
#include <QString>
#include <QVector>
#include <array>
#include <cmath>

namespace d4cloth {

// Averaged authored tuning across the outfit's cloth pieces — WardrobeTab2::loadClothTuning.
struct GameClothTuning {
    float boneTrack = 0.45f, actorTrack = 0.6f, stretch = 0.21f, horiz = 0.5f,
          shear = 0.15f, bend = 0.47f, damping = 0.93f, attach = 0.2f,
          friction = 0.30f, density = 2.0f, drag = 0.0f, lift = 0.0f;
    bool  found = false;
};

// Live solver parameters — GLModelWidget::ClothParams with identical defaults, plus the
// game-driven overrides applyClothParams() applies when "Use game cloth data" is on.
struct ClothParams {
    float gravity          = 0.0f;
    float damping          = 0.93f;
    float maxDistance      = 1.0f;
    float bendStiffness    = 0.47f;
    float stretchStiffness = 0.21f;
    int   iterations       = 10;
    int   subSteps         = 2;
    float selfCollision    = 0.004f;
    float collisionMargin  = 0.02f;
    float friction         = 0.30f;
    float backstop         = 0.020f;
    float capsuleRadius    = 0.52f;
    float boneTracking     = 0.45f;
    float actorTracking    = 0.6f;
    float horizStiffness   = 0.5f;
    float shearStiffness   = 0.15f;
    float attachStiffness  = 0.2f;
    float windX = 0, windY = 0, windZ = 0;
    float dragFactor       = 0.0f;
    float liftFactor       = 0.0f;
    float boneStiffness    = 0.02f;
    bool  userSpin         = false;    // headless: spin is scripted per scenario, not mouse-driven
    float userSpinForce    = 0.1f;
};

struct Scene {
    QString caseName;
    QStringList pieces;

    // ── merged geometry (GLModelWidget state mirror) ──
    ModelGeometry geo;                       // merged; skeleton + clothSims + clothCapsules
    QVector<float>   bindVerts;              // 11 floats/vert: pos3 normal3 uv2 tangent3
    QVector<quint32> indices;
    struct Part { int offset = 0, count = 0; QString material; int pieceIdx = -1; };
    QVector<Part>    parts;
    QVector<std::array<quint16,4>> vJoints;
    QVector<std::array<float,4>>   vWeights;

    int   baseBones = 0;                     // merged nBaseBones (first piece's skeleton size)
    float radius = 0;                        // half bbox DIAGONAL of bind verts (m_radius)
    std::array<float,3> homeCenter {{0,0,0}};

    QVector<RigMath::Mat4> restGlobal;       // per-bone rest world (y-up model space)

    // ── authored cloth, full parse, per piece ──
    QVector<ClothDoc> docs;                  // followerIndices remapped to unified skeleton
    QVector<QVector<int>> docFollowerUnified;// per doc, per real vert: unified bone or -1
    QVector<QVector<int>> docDriverBoneUnified; // per doc, per driver: unified bone or -1
                                             // (ptDriverMap assumed to index the piece skeleton)

    // Full per-doc authored tuning (.clt.json), used by the corrected solver. Per-frame
    // quantities are already converted: accel × dt² with dt = 1/60 s.
    struct DocTuning {
        float stretch = 0.7f, horiz = 0.5f, shear = 0.15f, bend = 0.3f;  // per-class stiffness
        float dampingFactor = 0.0f, dragFactor = 0.0f;
        float attachStiffness = 0.3f;
        float gravPerStep = -22.0f / 3600.0f;   // vGravity.z (native, y-up vertical) × dt²
        float windX = 0, windY = 0, windZ = 0;  // vSelfWind (y-up) × flWindFactor × dt²
        int   nIterations = 1;
        float boneTrack = 0.5f;
        bool  found = false;
    };
    QVector<DocTuning> docTuning;            // parallel to docs

    GameClothTuning gct;                     // averaged .clt tuning (game-driven params)
    ClothParams     params;                  // defaults + game-driven overrides applied

    // ── animation (optional; empty when the case ships no anim or load fails) ──
    AnimParser::DecodedAnim anim;
    QHash<quint32,int>      animByHash;      // bone nameHash → anim.bones index
    QString                 animName;

    bool ok = false;
    QString error;
};

// Load + assemble. `applyGameTuning` mirrors "Use game cloth data" (default on, like the app).
Scene loadScene(const CorpusSource& corpus, const QString& caseName,
                const QStringList& pieces, bool applyGameTuning = true,
                const QString& animName = QString());

// GLModelWidget's file-static composeTRS: composes D4-native TRS AND applies the z-up→y-up
// swap inline — t→(x,z,-y), q→(qx,qz,-qy,qw), s→(sx,sz,sy). RigMath::composeTRS does NOT
// swap; the solver world is y-up, so all solver-side composition uses this one.
inline RigMath::Mat4 composeTRSSwapped(const float q[4], const float t[3], const float s[3])
{
    float tx = t[0], ty = t[2], tz = -t[1];
    float qx = q[0], qy = q[2], qz = -q[1], qw = q[3];
    const float qm = std::sqrt(qx*qx + qy*qy + qz*qz + qw*qw);
    if (qm < 1e-9f) { qx = qy = qz = 0; qw = 1; } else { qx/=qm; qy/=qm; qz/=qm; qw/=qm; }
    const float sx = s[0], sy = s[2], sz = s[1];
    const float xx=qx*qx, yy=qy*qy, zz=qz*qz, xy=qx*qy, xz=qx*qz, yz=qy*qz;
    const float wx=qw*qx, wy=qw*qy, wz=qw*qz;
    const float m[4][4] = {
        {(1-2*(yy+zz))*sx, (2*(xy-wz))*sy,   (2*(xz+wy))*sz,   tx},
        {(2*(xy+wz))*sx,   (1-2*(xx+zz))*sy, (2*(yz-wx))*sz,   ty},
        {(2*(xz-wy))*sx,   (2*(yz+wx))*sy,   (1-2*(xx+yy))*sz, tz},
        {0, 0, 0, 1}};
    RigMath::Mat4 out; int k = 0;
    for (int col = 0; col < 4; ++col) for (int row = 0; row < 4; ++row) out[k++] = m[row][col];
    return out;
}

} // namespace d4cloth
