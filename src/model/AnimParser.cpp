#include "model/AnimParser.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;

constexpr int HEADER_SIZE = 208;
constexpr int VARARRAY_SIZE = 16;
constexpr int ARRAY_COUNT = 13;
constexpr int SLOT_BONE_NAMES = 0;
constexpr int SLOT_TRANS = 10, SLOT_ROT = 11, SLOT_SCALE = 12;
constexpr int TRANSLATION_FMT = 0x04;
constexpr float QUAT_NORM = 32767.0f;
constexpr int LONG_FORM_LIMIT = 255;

int align4(int x) { return (x + 3) & ~3; }
int align2(int x) { return (x + 1) & ~1; }

const quint8* P(const QByteArray& b) { return reinterpret_cast<const quint8*>(b.constData()); }
quint32 rdU32(const QByteArray& b, int o) { return qFromLittleEndian<quint32>(P(b) + o); }
quint16 rdU16(const QByteArray& b, int o) { return qFromLittleEndian<quint16>(P(b) + o); }
qint16  rdI16(const QByteArray& b, int o) { return qint16(qFromLittleEndian<quint16>(P(b) + o)); }
float   rdF32(const QByteArray& b, int o) { float f; quint32 v = rdU32(b, o); std::memcpy(&f, &v, 4); return f; }

struct VarArray { quint32 off = 0, size = 0; bool empty() const { return size == 0; } };
struct Curve { QByteArray raw; bool empty() const { return raw.isEmpty(); } };

VarArray readVarArray(const QByteArray& payload, int offset)
{
    VarArray v;
    if (offset + VARARRAY_SIZE > payload.size()) return v;
    v.off  = rdU32(payload, offset + 8);
    v.size = rdU32(payload, offset + 12);
    return v;
}

QByteArray slicePayload(const QByteArray& payload, const VarArray& a)
{
    if (a.empty()) return {};
    const qint64 end = qint64(a.off) + a.size;
    if (a.off >= quint32(payload.size()) || end > payload.size()) return {};
    return payload.mid(int(a.off), int(a.size));
}

// A curve list is an array of 16-byte DT_VARIABLEARRAY records; each points at
// that bone's raw key blob.
QVector<Curve> decodeCurveList(const QByteArray& payload, const VarArray& listArr)
{
    QVector<Curve> out;
    const QByteArray blob = slicePayload(payload, listArr);
    const int n = blob.size() / VARARRAY_SIZE;
    for (int i = 0; i < n; ++i) {
        const VarArray a = readVarArray(blob, i * VARARRAY_SIZE);
        Curve c;
        c.raw = slicePayload(payload, a);
        out.push_back(c);
    }
    return out;
}

// ── per-frame fill helpers (scalar ports of the numpy versions) ──
template <int D>
QVector<std::array<float, D>> constPerFrame(const std::array<float, D>& v, int n)
{
    QVector<std::array<float, D>> out(n);
    for (int i = 0; i < n; ++i) out[i] = v;
    return out;
}

// LERP fill for translation/scale.
QVector<Vec3> fillPerFrame(int frameCount, const QVector<Vec3>& keys,
                           const QVector<int>& frameIdx, const Vec3& fallback)
{
    QVector<int> slot(frameCount, -1);
    for (int k = 0; k < frameIdx.size(); ++k) {
        const int f = frameIdx[k];
        if (f >= 0 && f < frameCount) slot[f] = k;
    }
    QVector<int> placed;
    for (int f = 0; f < frameCount; ++f) if (slot[f] >= 0) placed.push_back(f);
    QVector<Vec3> out(frameCount);
    if (placed.isEmpty()) { for (int f = 0; f < frameCount; ++f) out[f] = fallback; return out; }
    for (int f = 0; f < frameCount; ++f) {
        if (f <= placed.first()) {
            // before/at first placed: interp from fallback@first? Python uses
            // left=fallback only before the first key; at/after, real values.
            if (f < placed.first()) { out[f] = fallback; continue; }
        }
        // find bracketing placed frames
        int lo = placed.first(), hi = placed.last();
        for (int j = 0; j + 1 < placed.size(); ++j) {
            if (placed[j] <= f && f <= placed[j + 1]) { lo = placed[j]; hi = placed[j + 1]; break; }
        }
        if (f >= placed.last()) { out[f] = keys[slot[placed.last()]]; continue; }
        const Vec3& a = keys[slot[lo]];
        const Vec3& b = keys[slot[hi]];
        const float t = hi > lo ? float(f - lo) / float(hi - lo) : 0.0f;
        for (int c = 0; c < 3; ++c) out[f][c] = a[c] + (b[c] - a[c]) * t;
    }
    return out;
}

float quatDot(const Quat& a, const Quat& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3]; }

// SLERP fill for rotation.
QVector<Quat> fillPerFrameSlerp(int frameCount, const QVector<Quat>& keys,
                                const QVector<int>& frameIdx, const Quat& fallback)
{
    QVector<int> slot(frameCount, -1);
    for (int k = 0; k < frameIdx.size(); ++k) {
        const int f = frameIdx[k];
        if (f >= 0 && f < frameCount) slot[f] = k;
    }
    QVector<int> placed;
    for (int f = 0; f < frameCount; ++f) if (slot[f] >= 0) placed.push_back(f);
    QVector<Quat> out(frameCount);
    if (placed.isEmpty()) { for (int f = 0; f < frameCount; ++f) out[f] = fallback; return out; }
    for (int f = 0; f < frameCount; ++f) {
        if (f < placed.first()) { out[f] = fallback; continue; }
        if (f >= placed.last()) { out[f] = keys[slot[placed.last()]]; continue; }
        if (slot[f] >= 0)       { out[f] = keys[slot[f]]; continue; }
        int lo = placed.first(), hi = placed.last();
        for (int j = 0; j + 1 < placed.size(); ++j)
            if (placed[j] <= f && f <= placed[j + 1]) { lo = placed[j]; hi = placed[j + 1]; break; }
        Quat qa = keys[slot[lo]];
        Quat qb = keys[slot[hi]];
        const float t = hi > lo ? float(f - lo) / float(hi - lo) : 0.0f;
        float d = quatDot(qa, qb);
        if (d < 0.0f) { for (float& c : qb) c = -c; d = -d; }
        d = std::min(d, 1.0f);
        const float theta = std::acos(d);
        if (theta < 0.0175f) {  // NLERP band
            Quat r;
            for (int c = 0; c < 4; ++c) r[c] = qa[c] * (1 - t) + qb[c] * t;
            const float n = std::sqrt(quatDot(r, r));
            if (n > 1e-9f) for (float& c : r) c /= n; else r = qa;
            out[f] = r;
        } else {
            const float st = std::sin(theta);
            const float wa = std::sin((1 - t) * theta) / st;
            const float wb = std::sin(t * theta) / st;
            Quat r;
            for (int c = 0; c < 4; ++c) r[c] = wa * qa[c] + wb * qb[c];
            out[f] = r;
        }
    }
    return out;
}

// ── curve decoders ──
QVector<Vec3> decodeTranslation(const Curve& cur, int frameCount, const Vec3& rest)
{
    if (cur.empty()) return constPerFrame<3>(rest, frameCount);
    const QByteArray& b = cur.raw;
    if (b.size() < 16) return constPerFrame<3>(rest, frameCount);
    const int count = quint8(b[0]);
    const int fmt = quint8(b[1]);
    if (count == 0) return constPerFrame<3>(rest, frameCount);
    if (fmt != TRANSLATION_FMT) return constPerFrame<3>(rest, frameCount);
    const Vec3 baseline{{rdF32(b, 4), rdF32(b, 8), rdF32(b, 12)}};
    if (count == 1) return constPerFrame<3>(baseline, frameCount);

    int dataOff; QVector<int> ts;
    const int tsOff = 16;
    if (frameCount > LONG_FORM_LIMIT) {
        if (b.size() < tsOff + count * 2) return constPerFrame<3>(baseline, frameCount);
        for (int i = 0; i < count; ++i) ts.push_back(rdU16(b, tsOff + i * 2));
        dataOff = align4(tsOff + count * 2);
    } else {
        for (int i = 0; i < count; ++i) ts.push_back(quint8(b[tsOff + i]));
        dataOff = align4(tsOff + count);
    }
    if (dataOff + count * 12 > b.size()) return constPerFrame<3>(baseline, frameCount);
    QVector<Vec3> keys(count);
    for (int i = 0; i < count; ++i)
        keys[i] = {{rdF32(b, dataOff + i*12) + baseline[0],
                    rdF32(b, dataOff + i*12 + 4) + baseline[1],
                    rdF32(b, dataOff + i*12 + 8) + baseline[2]}};
    return fillPerFrame(frameCount, keys, ts, baseline);
}

QVector<Quat> decodeRotation(const Curve& cur, int frameCount, const Quat& rest)
{
    if (cur.empty()) return constPerFrame<4>(rest, frameCount);
    const QByteArray& b = cur.raw;
    if (b.size() < 4) return constPerFrame<4>(rest, frameCount);
    const int count = rdU16(b, 0);
    if (count == 0) return constPerFrame<4>(rest, frameCount);
    if (count == 1) {
        if (b.size() < 10) return constPerFrame<4>(rest, frameCount);
        Quat q{{rdI16(b,2)/QUAT_NORM, rdI16(b,4)/QUAT_NORM, rdI16(b,6)/QUAT_NORM, rdI16(b,8)/QUAT_NORM}};
        return constPerFrame<4>(q, frameCount);
    }
    int dataOff; QVector<int> frameIdx;
    if (frameCount > LONG_FORM_LIMIT) {
        if (b.size() < 2 + count * 2) return constPerFrame<4>(rest, frameCount);
        for (int i = 0; i < count; ++i) frameIdx.push_back(rdU16(b, 2 + i * 2));
        dataOff = align2(2 + count * 2);
    } else {
        const int tsCount = count - 1;
        frameIdx.push_back(0);
        for (int i = 0; i < tsCount; ++i) frameIdx.push_back(quint8(b[2 + i]));
        dataOff = align2(2 + tsCount);
    }
    if (dataOff + count * 8 > b.size()) return constPerFrame<4>(rest, frameCount);
    QVector<Quat> quats(count);
    for (int i = 0; i < count; ++i)
        quats[i] = {{rdI16(b, dataOff+i*8)/QUAT_NORM, rdI16(b, dataOff+i*8+2)/QUAT_NORM,
                     rdI16(b, dataOff+i*8+4)/QUAT_NORM, rdI16(b, dataOff+i*8+6)/QUAT_NORM}};
    // W recovery for non-unit quats (only past row 0).
    for (int i = 1; i < count; ++i) {
        const float len2 = quatDot(quats[i], quats[i]);
        if (len2 < 0.9025f) {
            const float xyz = quats[i][0]*quats[i][0]+quats[i][1]*quats[i][1]+quats[i][2]*quats[i][2];
            float w = std::sqrt(std::max(0.0f, 1.0f - xyz));
            if (quats[i-1][3] < 0.0f) w = -w;
            quats[i][3] = w;
        }
    }
    return fillPerFrameSlerp(frameCount, quats, frameIdx, quats[0]);
}

QVector<Vec3> decodeScale(const Curve& cur, int frameCount, const Vec3& rest)
{
    if (cur.empty()) return constPerFrame<3>(rest, frameCount);
    const QByteArray& b = cur.raw;
    if (b.size() < 4) return constPerFrame<3>(rest, frameCount);
    const int count = quint8(b[0]);
    const int fmt = quint8(b[1]);
    if (count == 0) return constPerFrame<3>(rest, frameCount);
    if (fmt != TRANSLATION_FMT) return constPerFrame<3>(rest, frameCount);
    if (count == 1) {
        if (b.size() < 16) return constPerFrame<3>(rest, frameCount);
        return constPerFrame<3>(Vec3{{rdF32(b,4), rdF32(b,8), rdF32(b,12)}}, frameCount);
    }
    int dataOff; QVector<int> ts;
    const int tsOff = 2;
    if (frameCount > LONG_FORM_LIMIT) {
        if (b.size() < tsOff + count * 2) return constPerFrame<3>(rest, frameCount);
        for (int i = 0; i < count; ++i) ts.push_back(rdU16(b, tsOff + i * 2));
        dataOff = align4(tsOff + count * 2);
    } else {
        for (int i = 0; i < count; ++i) ts.push_back(quint8(b[tsOff + i]));
        dataOff = align4(tsOff + count);
    }
    if (dataOff + count * 12 > b.size()) return constPerFrame<3>(rest, frameCount);
    QVector<Vec3> keys(count);
    for (int i = 0; i < count; ++i)
        keys[i] = {{rdF32(b, dataOff+i*12), rdF32(b, dataOff+i*12+4), rdF32(b, dataOff+i*12+8)}};
    return fillPerFrame(frameCount, keys, ts, rest);
}

}  // namespace

AnimParser::DecodedAnim AnimParser::decode(const QByteArray& payload, int payloadOffset,
                                           int frameCount, int compression, float frameRate,
                                           const QHash<quint32, RestTRS>& restPose)
{
    DecodedAnim anim;
    anim.frameRate = frameRate;
    anim.frameCount = frameCount;
    anim.compression = compression;
    if (frameCount <= 0 || payloadOffset < 0 || payloadOffset + HEADER_SIZE > payload.size())
        return anim;

    VarArray raw[ARRAY_COUNT];
    for (int i = 0; i < ARRAY_COUNT; ++i)
        raw[i] = readVarArray(payload, payloadOffset + i * VARARRAY_SIZE);

    const QByteArray boneBlob = slicePayload(payload, raw[SLOT_BONE_NAMES]);
    QVector<quint32> boneNames;
    for (int i = 0; i + 4 <= boneBlob.size(); i += 4) boneNames.push_back(rdU32(boneBlob, i));

    const QVector<Curve> trans = decodeCurveList(payload, raw[SLOT_TRANS]);
    const QVector<Curve> rot   = decodeCurveList(payload, raw[SLOT_ROT]);
    const QVector<Curve> scale = decodeCurveList(payload, raw[SLOT_SCALE]);

    for (int i = 0; i < boneNames.size(); ++i) {
        const quint32 h = boneNames[i];
        RestTRS rest = restPose.value(h);
        DecodedBone bone;
        bone.boneHash = h;
        const Curve tc = i < trans.size() ? trans[i] : Curve{};
        const Curve rc = i < rot.size()   ? rot[i]   : Curve{};
        const Curve sc = i < scale.size() ? scale[i] : Curve{};
        bone.translations = decodeTranslation(tc, frameCount, rest.t);
        bone.rotations    = decodeRotation(rc, frameCount, rest.q);
        bone.scales       = decodeScale(sc, frameCount, rest.s);
        anim.bones.push_back(bone);
    }
    anim.valid = !anim.bones.isEmpty();
    return anim;
}
