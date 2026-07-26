#pragma once
// d4cloth — FULL ClothData parse (all 27 authored arrays + complete header).
//
// Layout source: d4data/definitions.json, type ClothData (hash 2666466548, 720 B) and
// dmClothDataMirror (hash 982587744, 288 B). The 720 B block = 288 B header + 27
// DT_VARIABLEARRAY refs of 16 B each ({pad,pad,dataOffset@8,dataSize@12}).
//
// Everything is kept RAW (D4-native z-up, exact bytes) plus typed views that are only
// filled when the array's byte size divides as the layout predicts. A size that does
// not divide is NOT silently dropped — the coverage report calls it out. That rule is
// the whole point of this tool: the most expensive bug in the app was an array that
// silently parsed as empty and fell back to a default.

#include <QByteArray>
#include <QString>
#include <QVector>
#include <array>

namespace d4cloth {

struct ArraySlot {
    QString    name;             // authored field name (definitions.json)
    int        fieldOff = 0;     // offset of the 16 B array ref inside the 720 B block
    int        dataOff = 0;      // resolved data offset (into meta or payload)
    int        dataSize = 0;     // bytes
    bool       present = false;  // size > 0 and the span is readable
    bool       inPayload = false;
    QByteArray raw;              // exact bytes
};

struct ClothDoc {
    // ── provenance ──
    int     blockOffset = -1;      // offset of the 720 B ClothData block
    bool    blockInPayload = false;
    QString pieceName;             // appearance name (set by the loader)

    // ── dmClothDataMirror header ──
    QString name;                                  // @216 (32 chars)
    float   density = 0;                           // @248
    int vertexCount = 0, vertexCapacity = 0;       // @252 @254
    int kinematicCount = 0, triangleCount = 0;     // @256 @258
    int warpClusterCount = 0, weftClusterCount = 0;    // @260 @262
    int shearClusterCount = 0, bendClusterCount = 0;   // @264 @266
    int constraintCount = 0, unk_9460e91 = 0;      // @268 @270
    int maxLevel = 0, boneCount = 0;               // @272 @274
    int driverCount = 0, capsuleCount = 0, planeCount = 0;  // @276 @278 @280

    // ── the 27 arrays, raw, in authored order ──
    QVector<ArraySlot> arrays;

    // ── typed views (empty when the size does not divide as predicted) ──
    QVector<std::array<float,4>> bindVertices, bindNormals, weights;   // vec4, D4-native z-up
    QVector<float>   invMasses, blendWeights, animBlendFractions, attachmentLengths,
                     constraintLengths, unkF576;
    QVector<quint16> levels, parentIndices, kinematicRoots, tangentIndices,
                     driverInfluences, followerIndices, triangles, constraintIndices,
                     unkU560, driverMap;
    struct Cluster { quint16 start = 0, end = 0; };
    QVector<Cluster> warpClusters, weftClusters, shearClusters, bendClusters;
    struct Frame { std::array<float,4> q {}, p {}, s {}; };            // dmFrameMirror, 48 B
    QVector<Frame>   driverBindPose;
    QVector<std::array<float,16>> deltaFrames;                         // 64 B/vert: 4x4 matrix per vert
    struct Capsule {                                                    // dmClothCapsuleDefMirror, 80 B
        std::array<float,3> localP {};      // localTransform position @0 (vec4, w dropped)
        std::array<float,4> localQ {};      // localTransform quaternion @16
        std::array<float,3> scale {};       // @32 (vec4, w dropped) — unread by the app today
        float radius1 = 0, radius2 = 0, height = 0, friction = 0;      // @48 @52 @56 @60
        int   boneIndex = -1;               // @64 (u16)
        int   solver = 0, hide = 0;         // @66 @67 (u8) — unread by the app today
    };
    QVector<Capsule> capsules;
    struct Plane {                                                      // dmClothPlaneDefMirror, 48 B
        std::array<float,3> localP {};
        std::array<float,4> localQ {};
        float stiffness = 0, friction = 0;  // @32 @36
        int   boneIndex = -1;               // @40 (u16)
    };
    QVector<Plane> planes;

    const ArraySlot* slot(const QString& n) const
    {
        for (const ArraySlot& s : arrays) if (s.name == n) return &s;
        return nullptr;
    }
};

// Find + parse every ClothData block referenced by `meta`, using the same discovery rule
// the app's parseClothCapsules uses (a {0,0,off,720} array ref whose target passes the
// header sanity check), then parse ALL of it. Positions stay D4-native (z-up) — distance
// analyses are axis-swap invariant, and the sim layer owns the y-up conversion.
QVector<ClothDoc> parseClothDocs(const QByteArray& meta, const QByteArray& payload);

// (x,y,z) → (x,z,-y): the same z-up→y-up swap the app applies to mesh/skeleton/cages.
inline void zUpToYUp(float& x, float& y, float& z) { const float oy = y; (void)x; y = z; z = -oy; }

// FNV-1a 64 of a byte buffer — printed in every run header so a result names its inputs.
quint64 fnv1a64(const QByteArray& b);

} // namespace d4cloth
