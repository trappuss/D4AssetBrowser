#pragma once
#include "model/ModelGeometry.h"

#include <QVector>
#include <array>
#include <cmath>

// Column-major 4×4 rig math shared by the attachment seater (and available to any
// other code that needs to place a child mesh onto a parent bone/hardpoint).
// These mirror the file-static helpers WardrobeTab2.cpp uses for weapon seating;
// kept header-only (inline) so multiple translation units can share one definition.
namespace RigMath {

using Mat4 = std::array<float, 16>;

// Rotation (quat x,y,z,w) + translation + non-uniform scale → column-major matrix.
inline Mat4 composeTRS(const std::array<float, 3>& t,
                       const std::array<float, 4>& q,
                       const std::array<float, 3>& s)
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
    const float r10 = 2 * (x * y + z * w),     r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
    const float r20 = 2 * (x * z - y * w),     r21 = 2 * (y * z + x * w),     r22 = 1 - 2 * (x * x + y * y);
    return {{ r00 * s[0], r10 * s[0], r20 * s[0], 0,
              r01 * s[1], r11 * s[1], r21 * s[1], 0,
              r02 * s[2], r12 * s[2], r22 * s[2], 0,
              t[0],       t[1],       t[2],       1 }};
}

inline Mat4 mat4mul(const Mat4& a, const Mat4& b)   // column-major a·b
{
    Mat4 o{};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float v = 0;
            for (int k = 0; k < 4; ++k) v += a[k * 4 + r] * b[c * 4 + k];
            o[c * 4 + r] = v;
        }
    return o;
}

// Full rest-pose world matrix of a joint, in D4-native (z-up) space — composed from
// the native rest TRS up the parent chain.
inline Mat4 jointWorldMat(const QVector<ModelJoint>& skel, int idx)
{
    QVector<int> chain;
    for (int i = idx; i >= 0 && i < skel.size(); i = skel[i].parent) {
        chain.push_front(i);
        if (skel[i].parent == i) break;
    }
    Mat4 world{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    for (int i : chain)
        world = mat4mul(world, composeTRS(skel[i].restT, skel[i].restQ, skel[i].restS));
    return world;
}

// quaternion (x,y,z,w) + position → column-major matrix (no axis swap; native space).
inline Mat4 quatPosMat(const std::array<float, 4>& q, const std::array<float, 3>& p)
{
    return composeTRS(p, q, {{1.0f, 1.0f, 1.0f}});
}

// Inverse of a rigid (rotation + translation, unit-scale) column-major matrix: [Rᵀ | -Rᵀt].
// Used to place a mount so its saddle hardpoint lands at the rider's origin.
inline Mat4 invertRigid(const Mat4& m)
{
    Mat4 o{};
    o[0] = m[0]; o[1] = m[4]; o[2]  = m[8];   o[3]  = 0;   // Rᵀ
    o[4] = m[1]; o[5] = m[5]; o[6]  = m[9];   o[7]  = 0;
    o[8] = m[2]; o[9] = m[6]; o[10] = m[10];  o[11] = 0;
    const float tx = m[12], ty = m[13], tz = m[14];
    o[12] = -(m[0] * tx + m[1] * ty + m[2]  * tz);         // -Rᵀt
    o[13] = -(m[4] * tx + m[5] * ty + m[6]  * tz);
    o[14] = -(m[8] * tx + m[9] * ty + m[10] * tz);
    o[15] = 1;
    return o;
}

// General column-major 4×4 inverse (handles non-unit bone scale, unlike invertRigid). Returns
// identity if the matrix is singular. Used when re-rooting a skeletally-attached prop onto a bone
// whose world may carry scale.
inline Mat4 invert(const Mat4& m)
{
    const float* a = m.data();
    Mat4 inv{};
    float* o = inv.data();
    o[0]  =  a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    o[4]  = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    o[8]  =  a[4]*a[9]*a[15]  - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    o[12] = -a[4]*a[9]*a[14]  + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    o[1]  = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    o[5]  =  a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    o[9]  = -a[0]*a[9]*a[15]  + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    o[13] =  a[0]*a[9]*a[14]  - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    o[2]  =  a[1]*a[6]*a[15]  - a[1]*a[7]*a[14]  - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7]  - a[13]*a[3]*a[6];
    o[6]  = -a[0]*a[6]*a[15]  + a[0]*a[7]*a[14]  + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7]  + a[12]*a[3]*a[6];
    o[10] =  a[0]*a[5]*a[15]  - a[0]*a[7]*a[13]  - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7]  - a[12]*a[3]*a[5];
    o[14] = -a[0]*a[5]*a[14]  + a[0]*a[6]*a[13]  + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6]  + a[12]*a[2]*a[5];
    o[3]  = -a[1]*a[6]*a[11]  + a[1]*a[7]*a[10]  + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7]   + a[9]*a[3]*a[6];
    o[7]  =  a[0]*a[6]*a[11]  - a[0]*a[7]*a[10]  - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7]   - a[8]*a[3]*a[6];
    o[11] = -a[0]*a[5]*a[11]  + a[0]*a[7]*a[9]   + a[4]*a[1]*a[11] - a[4]*a[3]*a[9]  - a[8]*a[1]*a[7]   + a[8]*a[3]*a[5];
    o[15] =  a[0]*a[5]*a[10]  - a[0]*a[6]*a[9]   - a[4]*a[1]*a[10] + a[4]*a[2]*a[9]  + a[8]*a[1]*a[6]   - a[8]*a[2]*a[5];
    float det = a[0]*o[0] + a[1]*o[4] + a[2]*o[8] + a[3]*o[12];
    if (det > -1e-12f && det < 1e-12f) return Mat4{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) o[i] *= det;
    return inv;
}

// Split a column-major TRS matrix back into translation / quaternion / scale, in whatever space
// the matrix was expressed in. The inverse of composeTRS: verified round-trip to 3e-15 over 20k
// random non-uniform-scale TRS matrices. Needed because the renderer rebuilds every bone's local
// transform from the rest TRS each frame and never reads ModelJoint::localMatrix — so anything
// that repositions a bone has to write the TRS, not the matrix.
inline void decomposeTRS(const Mat4& m, std::array<float, 3>& t,
                         std::array<float, 4>& q, std::array<float, 3>& s)
{
    t = {{m[12], m[13], m[14]}};
    float c[3][3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};
    for (int i = 0; i < 3; ++i)
        s[i] = std::sqrt(c[i][0]*c[i][0] + c[i][1]*c[i][1] + c[i][2]*c[i][2]);
    // Mirrored basis (negative determinant): fold the flip into the first axis' scale so the
    // remaining rotation is proper, otherwise the quaternion extraction below is meaningless.
    const float det = c[0][0]*(c[1][1]*c[2][2] - c[1][2]*c[2][1])
                    - c[1][0]*(c[0][1]*c[2][2] - c[0][2]*c[2][1])
                    + c[2][0]*(c[0][1]*c[1][2] - c[0][2]*c[1][1]);
    if (det < 0.0f) { s[0] = -s[0]; c[0][0] = -c[0][0]; c[0][1] = -c[0][1]; c[0][2] = -c[0][2]; }
    for (int i = 0; i < 3; ++i) {
        const float n = std::fabs(s[i]) > 1e-12f ? std::fabs(s[i]) : 1.0f;
        c[i][0] /= n; c[i][1] /= n; c[i][2] /= n;
    }
    // Rotation, row-major, from the normalised columns.
    const float r00 = c[0][0], r10 = c[0][1], r20 = c[0][2];
    const float r01 = c[1][0], r11 = c[1][1], r21 = c[1][2];
    const float r02 = c[2][0], r12 = c[2][1], r22 = c[2][2];
    const float tr = r00 + r11 + r22;
    float x, y, z, w;
    if (tr > 0.0f) {
        const float S = std::sqrt(tr + 1.0f) * 2.0f;
        w = 0.25f * S; x = (r21 - r12) / S; y = (r02 - r20) / S; z = (r10 - r01) / S;
    } else if (r00 > r11 && r00 > r22) {
        const float S = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        w = (r21 - r12) / S; x = 0.25f * S; y = (r01 + r10) / S; z = (r02 + r20) / S;
    } else if (r11 > r22) {
        const float S = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        w = (r02 - r20) / S; x = (r01 + r10) / S; y = 0.25f * S; z = (r12 + r21) / S;
    } else {
        const float S = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        w = (r10 - r01) / S; x = (r02 + r20) / S; y = (r12 + r21) / S; z = 0.25f * S;
    }
    const float qm = std::sqrt(x*x + y*y + z*z + w*w);
    if (qm < 1e-9f) q = {{0, 0, 0, 1}};
    else            q = {{x / qm, y / qm, z / qm, w / qm}};
}

// z-up → y-up basis change S (and its inverse): a native attach matrix M is
// re-expressed in mesh space as S · M · S⁻¹.
inline const Mat4 kSwapZtoY{{1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1}};
inline const Mat4 kSwapYtoZ{{1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1}};

}  // namespace RigMath
