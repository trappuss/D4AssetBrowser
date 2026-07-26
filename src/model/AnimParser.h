#pragma once
#include <QByteArray>
#include <QHash>
#include <QVector>

#include <array>

// Diablo IV .ani animation decoder — port of d4extract's anim_parser.py.
// Reads the 208-byte AnimPayloadData header (13 DT_VARIABLEARRAY records) from
// the binary payload at the meta's ptPayloadData offset, then decodes each
// bone's translation / rotation / scale curve to per-frame transforms.
// Supports flCompression 0–6 (one short-form layout) + long-form (>255 frame)
// u16 timestamps, matching the reference.
namespace AnimParser {

struct DecodedBone {
    quint32 boneHash = 0;
    QVector<std::array<float, 3>> translations;  // per-frame (x,y,z), absolute local
    QVector<std::array<float, 4>> rotations;     // per-frame (x,y,z,w), D4-native
    QVector<std::array<float, 3>> scales;        // per-frame (x,y,z)
};

struct DecodedAnim {
    float frameRate = 30.0f;
    int   frameCount = 0;
    int   compression = 0;
    QVector<DecodedBone> bones;
    bool  valid = false;
};

// Rest pose per bone (fallback for empty curves): quat(xyzw), translation, scale.
struct RestTRS {
    std::array<float, 4> q{{0, 0, 0, 1}};
    std::array<float, 3> t{{0, 0, 0}};
    std::array<float, 3> s{{1, 1, 1}};
};

// Decode the permutation whose AnimPayloadData header sits at payloadOffset.
// Scalars (frameCount, compression, frameRate) come from the meta JSON.
DecodedAnim decode(const QByteArray& payload, int payloadOffset,
                   int frameCount, int compression, float frameRate,
                   const QHash<quint32, RestTRS>& restPose = {});

}  // namespace AnimParser
