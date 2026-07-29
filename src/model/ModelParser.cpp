#include "model/ModelParser.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QHash>
#include <QPair>
#include <QString>
#include <QtGlobal>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

// ── Format constants (app_parser.py § Format constants) ──────────────
namespace {

constexpr quint32 APP_META_MAGIC = 0xDEADBEEFu;
constexpr int STRIDE_SIMPLE = 36;   // eVBFormat 4, static
constexpr int STRIDE_SKINNED = 44;  // eVBFormat 6, skinned

// eSemantic
enum { SEM_POSITION = 0, SEM_TEXCOORD_0 = 1, SEM_TEXCOORD_1 = 2,
       SEM_COLOR_0 = 7, SEM_COLOR_1 = 8, SEM_NORMAL = 9, SEM_TANGENT = 10,
       SEM_BLENDINDICES = 11, SEM_BLENDWEIGHTS = 12 };

// eFormat
enum { FMT_R32G32B32_FLOAT = 1, FMT_R16G16_SNORM = 2, FMT_R8G8B8A8_UINT = 4,
       FMT_R8G8B8A8_UNORM = 5, FMT_R16G16_FLOAT = 7, FMT_PACKED_SNORM4 = 8 };

int formatSize(int fmt)
{
    switch (fmt) {
    case FMT_R32G32B32_FLOAT: return 12;
    case FMT_R16G16_SNORM:    return 4;
    case FMT_R8G8B8A8_UINT:   return 4;
    case FMT_R8G8B8A8_UNORM:  return 4;
    case FMT_R16G16_FLOAT:    return 4;
    case FMT_PACKED_SNORM4:   return 4;
    default:                  return 0;
    }
}

// ── Little-endian reads ──────────────────────────────────────────────
// Byte reader over a payload/meta buffer. The accessors are SELF-PROTECTING: an out-of-bounds
// (or null-buffer) read returns 0 instead of dereferencing wild memory. Callers still guard
// spans with in() for correctness, but a single missed guard anywhere can no longer segfault
// the tool — it degrades to a garbage-but-safe parse (rejected downstream by validity checks).
struct Reader {
    const quint8* d = nullptr;
    int n = 0;
    Reader(const QByteArray& b) : d(reinterpret_cast<const quint8*>(b.constData())), n(b.size()) {}
    // 64-bit sum: `off + len` in int WRAPS for file-derived offsets near INT_MAX, so the guard
    // returned true and the accessor read far past the buffer.
    bool in(int off, int len) const
    { return d && off >= 0 && len >= 0 && qint64(off) + qint64(len) <= qint64(n); }
    quint8  u8(int o)  const { return in(o, 1) ? d[o] : quint8(0); }
    quint16 u16(int o) const { return in(o, 2) ? quint16(quint16(d[o]) | (quint16(d[o+1])<<8)) : quint16(0); }
    quint32 u32(int o) const {
        return in(o, 4) ? (quint32(d[o]) | (quint32(d[o+1])<<8) | (quint32(d[o+2])<<16) | (quint32(d[o+3])<<24))
                        : quint32(0);
    }
    qint32  i32(int o) const { return qint32(u32(o)); }
    float   f32(int o) const { float f; quint32 v = u32(o); std::memcpy(&f, &v, 4); return f; }
};

float halfToFloat(quint16 h)
{
    const int sign = (h >> 15) & 1;
    const int exp  = (h >> 10) & 0x1F;
    const int frac = h & 0x3FF;
    float val;
    if (exp == 0)
        val = std::ldexp(frac / 1024.0f, -14);
    else if (exp == 31)
        val = frac ? NAN : INFINITY;
    else
        val = std::ldexp(1.0f + frac / 1024.0f, exp - 15);
    return sign ? -val : val;
}

inline float snorm8(quint8 b) { return qint8(b) / 127.0f; }

// ── Vertex layout ────────────────────────────────────────────────────
struct VElem { int semantic, format, offset; };
struct VLayout { int stride = 0; QVector<VElem> elems; };

VLayout canonical36()
{
    return {STRIDE_SIMPLE, {
        {SEM_POSITION,   FMT_R32G32B32_FLOAT, 0},
        {SEM_NORMAL,     FMT_PACKED_SNORM4,   12},
        {SEM_COLOR_0,    FMT_R8G8B8A8_UNORM,  16},
        {SEM_COLOR_1,    FMT_R8G8B8A8_UNORM,  20},
        {SEM_TEXCOORD_0, FMT_R16G16_FLOAT,    24},
        {SEM_TEXCOORD_1, FMT_R16G16_FLOAT,    28},
        {SEM_TANGENT,    FMT_PACKED_SNORM4,   32}}};
}
VLayout canonical44()
{
    return {STRIDE_SKINNED, {
        {SEM_POSITION,     FMT_R32G32B32_FLOAT, 0},
        {SEM_NORMAL,       FMT_PACKED_SNORM4,   12},
        {SEM_TANGENT,      FMT_PACKED_SNORM4,   16},
        {SEM_COLOR_0,      FMT_R8G8B8A8_UNORM,  20},
        {SEM_COLOR_1,      FMT_R8G8B8A8_UNORM,  24},
        {SEM_TEXCOORD_0,   FMT_R16G16_FLOAT,    28},
        {SEM_TEXCOORD_1,   FMT_R16G16_FLOAT,    32},
        {SEM_BLENDINDICES, FMT_R8G8B8A8_UINT,   36},
        {SEM_BLENDWEIGHTS, FMT_R8G8B8A8_UNORM,  40}}};
}

// First VertexElem of any layout: (sem=0, fmt=1, off=0) as three LE u32.
const QByteArray kVertexElemSig = QByteArray::fromHex("000000000100000000000000");

VLayout tryReadLayout(const Reader& meta, int origin, int stride)
{
    const int lo = qMax(0, origin - 0x800);
    const int hi = qMin(meta.n, origin + 0x800);
    QByteArray hay(reinterpret_cast<const char*>(meta.d) + lo, hi - lo);
    const int rel = hay.indexOf(kVertexElemSig);
    if (rel < 0) return {};
    int cursor = lo + rel;
    VLayout L; L.stride = stride;
    int last = -1;
    while (cursor + 12 <= meta.n) {
        const int sem = int(meta.u32(cursor));
        const int fmt = int(meta.u32(cursor + 4));
        const int off = int(meta.u32(cursor + 8));
        const int sz = formatSize(fmt);
        if (sz == 0 || off < last || off + sz > stride) break;
        L.elems.push_back({sem, fmt, off});
        last = off;
        cursor += 12;
        if (off + sz == stride) return L;
    }
    return {};
}

VLayout resolveLayout(int stride, const Reader& meta, int origin)
{
    VLayout parsed = tryReadLayout(meta, origin, stride);
    if (!parsed.elems.isEmpty()) return parsed;
    if (stride == STRIDE_SIMPLE)  return canonical36();
    if (stride == STRIDE_SKINNED) return canonical44();
    return {};
}

const VElem* findElem(const VLayout& L, int sem)
{
    for (const VElem& e : L.elems) if (e.semantic == sem) return &e;
    return nullptr;
}

// ── GeoChunk scans ───────────────────────────────────────────────────
struct VB { int fileOffset, arrayIndex, stride, dataOffset, dataSize; bool fOptional; };
struct IB { int fileOffset, arrayIndex, dataOffset, dataSize; bool fOptional; };

QVector<VB> scanVertexBuffers(const Reader& meta, int payloadSize)
{
    QVector<VB> out;
    for (int off = 0; off + 80 <= meta.n; off += 4) {
        const quint32 vbf = meta.u32(off);
        if (vbf != 4 && vbf != 6 && vbf != 27) continue;
        const int stride = int(meta.u32(off + 4));
        if (stride != STRIDE_SIMPLE && stride != STRIDE_SKINNED) continue;
        const int dataOffset = int(meta.u32(off + 0x38));
        const int dataSize   = int(meta.u32(off + 0x3C));
        if (dataOffset <= 0 || dataSize <= 0) continue;
        // 64-bit: two file-derived values near 2^31 wrapped to a negative sum and passed, after
        // which vcount = dataSize / stride became tens of millions and the vertex allocation
        // asked for gigabytes (bad_alloc, which this tool's crash log shows every time).
        if (qint64(dataOffset) + qint64(dataSize) > qint64(payloadSize)) continue;
        if (dataSize % stride != 0) continue;
        const quint32 fOpt = meta.u32(off + 0x4C);
        if (fOpt != 0 && fOpt != 1) continue;
        out.push_back({off, -1, stride, dataOffset, dataSize, fOpt == 1});
    }
    std::sort(out.begin(), out.end(), [](const VB& a, const VB& b){ return a.fileOffset < b.fileOffset; });
    for (int i = 0; i < out.size(); ++i) out[i].arrayIndex = i;
    return out;
}

QVector<IB> filterIbCluster(const QVector<IB>& cands, const QVector<VB>& vbs)
{
    if (cands.isEmpty()) return cands;
    int lastVbEnd = 0;
    for (const VB& v : vbs) lastVbEnd = qMax(lastVbEnd, v.fileOffset + 80);
    QVector<IB> window;
    for (const IB& ib : cands)
        if (ib.fileOffset > lastVbEnd && ib.fileOffset <= lastVbEnd + 1024) window.push_back(ib);
    if (window.isEmpty()) return cands;
    QVector<QVector<IB>> runs; runs.push_back({window[0]});
    for (int i = 1; i < window.size(); ++i) {
        if (window[i].fileOffset - runs.last().last().fileOffset == 24) runs.last().push_back(window[i]);
        else runs.push_back({window[i]});
    }
    const QVector<IB>* best = &runs[0];
    for (const auto& r : runs) {
        if (r.size() > best->size() ||
            (r.size() == best->size() && r[0].fileOffset < (*best)[0].fileOffset))
            best = &r;
    }
    return *best;
}

QVector<IB> scanIndexBuffers(const Reader& meta, int payloadSize, const QVector<VB>& vbs)
{
    QVector<IB> out;
    for (int off = 0; off + 24 <= meta.n; off += 4) {
        if (meta.u32(off) != 0 || meta.u32(off + 4) != 0) continue;
        const int dataOffset = int(meta.u32(off + 0x08));
        const int dataSize   = int(meta.u32(off + 0x0C));
        if (dataOffset <= 0 || dataSize <= 0) continue;
        if (dataOffset <= meta.n) continue;            // meta-resident, not an ibuf
        if (qint64(dataOffset) + qint64(dataSize) > qint64(payloadSize)) continue;   // see above
        if (dataSize % 2 != 0) continue;
        const qint32 ibid = meta.i32(off + 0x10);
        const quint32 fOpt = meta.u32(off + 0x14);
        if (fOpt != 0 && fOpt != 1) continue;
        if (ibid != -1 && !(ibid >= 0 && ibid <= 1024)) continue;
        out.push_back({off, -1, dataOffset, dataSize, fOpt == 1});
    }
    std::sort(out.begin(), out.end(), [](const IB& a, const IB& b){ return a.fileOffset < b.fileOffset; });
    if (!vbs.isEmpty()) out = filterIbCluster(out, vbs);
    for (int i = 0; i < out.size(); ++i) out[i].arrayIndex = i;
    return out;
}

// ── Segment / SubObject scans ────────────────────────────────────────
struct Segment { int fileOffset; quint32 vc, vo, ic, io; QVector<int> bonePalette; };
struct SubObj  { int fileOffset; qint32 mat, vbi, ibi; quint32 hash, slotHash; Segment seg; };

// SubObjectSegment.pBoneIDs: DT_VARIABLEARRAY<int32> at the segment struct head
// (8 zero bytes, then dataOffset/dataSize). Meta-resident values live at
// meta[dataOffset + 16] (16-byte prefix convention).
QVector<int> readBonePalette(const Reader& meta, int structOffset)
{
    QVector<int> out;
    if (structOffset + 16 > meta.n) return out;
    const quint32 doff = meta.u32(structOffset + 8);
    const quint32 dsz  = meta.u32(structOffset + 12);
    if (doff == 0 || dsz == 0 || dsz % 4 != 0) return out;
    // All arithmetic in 64-bit and the count clamped to what the buffer can hold. `dsz` is a
    // file-derived u32: any value with bit 31 set still satisfies `% 4 == 0`, and int(dsz) is then
    // NEGATIVE, so the old `base + int(dsz) > meta.n` test passed and `QVector<int> vals(n)` was
    // constructed with a negative size. scanSegments calls this for EVERY 4-byte offset in the
    // meta blob that looks segment-shaped, and a negative float (0xBF800000 etc.) is exactly that
    // bit pattern — so one false positive anywhere in a large meta was enough to fault. Every
    // other count in this file is clamped against the buffer before allocating; this one was not.
    const qint64 base64 = qint64(doff) + 16;
    const qint64 end64  = base64 + qint64(dsz);
    if (base64 < 0 || end64 > qint64(meta.n)) return out;
    const int base = int(base64);
    const int n = int(qint64(dsz) / 4);
    if (n <= 0) return out;
    QVector<int> vals(n);
    for (int i = 0; i < n; ++i) {
        const qint32 v = meta.i32(base + i * 4);
        if (v < 0 || v > 100000) return QVector<int>();   // reject implausible
        vals[i] = v;
    }
    return vals;
}

QVector<Segment> scanSegments(const Reader& meta)
{
    QVector<Segment> out;
    for (int off = 16; off + 16 <= meta.n; off += 4) {
        if (meta.u32(off - 16) != 0 || meta.u32(off - 12) != 0) continue;
        const quint32 vc = meta.u32(off);
        const quint32 vo = meta.u32(off + 4);
        const quint32 ic = meta.u32(off + 8);
        const quint32 io = meta.u32(off + 12);
        if (!(vc >= 1 && vc <= 200000)) continue;
        if (!(ic >= 3 && ic <= 5000000) || ic % 3 != 0) continue;
        if (io > 5000000 || io % 3 != 0) continue;
        out.push_back({off, vc, vo, ic, io, readBonePalette(meta, off - 16)});
    }
    return out;
}

QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
    b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
    return b;
}

QVector<SubObj> findSubObjects(const Reader& meta, const QByteArray& metaBytes, const QVector<Segment>& segs)
{
    QVector<SubObj> out;
    QHash<int, bool> usedSeg;
    for (const Segment& seg : segs) {
        if (usedSeg.value(seg.fileOffset - 16, false)) continue;
        const int target = seg.fileOffset - 32;
        if (target < 0) continue;
        const QByteArray pat = le32(quint32(target));
        int pos = 0;
        while (true) {
            const int p = metaBytes.indexOf(pat, pos);
            if (p < 0) break;
            if (p < 8 || p + 8 > meta.n) { pos = p + 1; continue; }
            if (meta.u32(p - 8) != 0 || meta.u32(p - 4) != 0 || meta.u32(p + 4) != 32) { pos = p + 1; continue; }
            const int so = p - 0x08 - 0xC8;
            if (so < 0 || so + 240 > meta.n) { pos = p + 1; continue; }
            const qint32 mat = meta.i32(so + 0x60);
            const qint32 vbi = meta.i32(so + 0x68);
            const qint32 ibi = meta.i32(so + 0x6C);
            if (!(vbi >= -1 && vbi <= 16) || !(ibi >= -1 && ibi <= 16)) { pos = p + 1; continue; }
            const quint32 hash = meta.u32(so + 0x64);
            const quint32 slot = meta.u32(so + 0x38 + 0x10);
            out.push_back({so, mat, vbi, ibi, hash, slot, seg});
            usedSeg[seg.fileOffset - 16] = true;
            break;
        }
    }
    std::sort(out.begin(), out.end(), [](const SubObj& a, const SubObj& b){ return a.fileOffset < b.fileOffset; });
    return out;
}

// Coordinate conversion (x, y, z) → (x, z, -y).
inline void zUpToYUp(float& x, float& y, float& z) { const float ny = z, nz = -y; y = ny; z = nz; }

// ── Skeleton (BoneData / BoneStructure) ──────────────────────────────
constexpr int BONE_STRUCT_SIZE   = 232;
constexpr int BONE_PARENT_OFF    = 0x28;   // int16 (0xFFFF = root)
constexpr int BONE_HASH_OFF      = 0x20;   // uint32
constexpr int BONE_LOCAL_TRS_OFF = 0x94;   // PRSTransform transformParentRel
constexpr int BONE_INVBIND_OFF   = 0xBC;   // PRSTransform transformSkinningInv

struct PRS { float qx,qy,qz,qw, tx,ty,tz, sx,sy,sz; };

PRS readPRS(const Reader& d, int base)
{
    PRS p;
    p.qx = d.f32(base + 0x00); p.qy = d.f32(base + 0x04);
    p.qz = d.f32(base + 0x08); p.qw = d.f32(base + 0x0C);
    p.tx = d.f32(base + 0x10); p.ty = d.f32(base + 0x14); p.tz = d.f32(base + 0x18);
    p.sx = d.f32(base + 0x1C); p.sy = d.f32(base + 0x20); p.sz = d.f32(base + 0x24);
    return p;
}

// Compose a TRS into a 4x4 column-major matrix, applying z_up_to_y_up to each
// component: t→(x,z,-y), q→(qx,qz,-qy,qw) renormalised, s→(sx,sz,sy).
std::array<float,16> composeBoneMatrix(const PRS& p)
{
    float tx = p.tx, ty = p.ty, tz = p.tz; zUpToYUp(tx, ty, tz);
    float qx = p.qx, qy = p.qz, qz = -p.qy, qw = p.qw;   // quaternion axis swap
    const float qm = std::sqrt(qx*qx + qy*qy + qz*qz + qw*qw);
    if (qm < 1e-9f) { qx = qy = qz = 0.0f; qw = 1.0f; }
    else { qx/=qm; qy/=qm; qz/=qm; qw/=qm; }
    const float sx = p.sx, sy = p.sz, sz = p.sy;          // scale axis swap

    const float xx=qx*qx, yy=qy*qy, zz=qz*qz, xy=qx*qy, xz=qx*qz, yz=qy*qz;
    const float wx=qw*qx, wy=qw*qy, wz=qw*qz;
    const float r00=1-2*(yy+zz), r01=2*(xy-wz),   r02=2*(xz+wy);
    const float r10=2*(xy+wz),   r11=1-2*(xx+zz), r12=2*(yz-wx);
    const float r20=2*(xz-wy),   r21=2*(yz+wx),   r22=1-2*(xx+yy);
    // row-major rows scaled by S, translation in last column; emit column-major.
    const float m[4][4] = {
        {r00*sx, r01*sy, r02*sz, tx},
        {r10*sx, r11*sy, r12*sz, ty},
        {r20*sx, r21*sy, r22*sz, tz},
        {0,      0,      0,      1 }};
    std::array<float,16> out;
    int k = 0;
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            out[k++] = m[row][col];
    return out;
}

// Locate ptBoneStructure: a meta DT_VARIABLEARRAY whose payload data is a
// multiple of BONE_STRUCT_SIZE and whose first record is a plausible root bone.
// Returns payload offset + bone count, or {-1,0}. refOffOut (optional): the META
// offset of the matched 16B array ref — the authored bone-count split lives at
// fixed offsets around it (see parseSkeleton).
QPair<int,int> scanBoneArray(const Reader& meta, const Reader& payload, int* refOffOut = nullptr)
{
    for (int off = 0; off + 16 <= meta.n; off += 4) {
        if (meta.u32(off) != 0 || meta.u32(off + 4) != 0) continue;
        const quint32 doff = meta.u32(off + 8);
        const quint32 dsz  = meta.u32(off + 12);
        if (doff == 0 || dsz == 0 || dsz % BONE_STRUCT_SIZE != 0) continue;
        // 64-bit math: doff/dsz are u32, so a bogus huge dsz must not overflow the
        // bounds check (a wrapped int could pass and then read wildly out of range).
        if (quint64(doff) + quint64(dsz) > quint64(payload.n)) continue;
        const qint16 parent = qint16(payload.u16(int(doff) + BONE_PARENT_OFF));
        if (parent != -1) continue;
        const int q = int(doff) + BONE_LOCAL_TRS_OFF;
        const float qx = payload.f32(q), qy = payload.f32(q+4),
                    qz = payload.f32(q+8), qw = payload.f32(q+12);
        const float mag2 = qx*qx + qy*qy + qz*qz + qw*qw;
        if (!(mag2 > 0.5f && mag2 < 1.5f)) continue;
        if (refOffOut) *refOffOut = off;
        return {int(doff), int(dsz) / BONE_STRUCT_SIZE};
    }
    return {-1, 0};
}

void parseSkeleton(const Reader& meta, const Reader& payload, ModelGeometry& geo)
{
    int refOff = -1;
    const QPair<int,int> loc = scanBoneArray(meta, payload, &refOff);
    if (loc.first < 0 || loc.second == 0) return;
    const int base = loc.first, count = loc.second;
    // Guard: the whole bone array must lie within the payload (overflow-safe).
    if (base < 0 || count <= 0 || count > 200000
        || qint64(base) + qint64(count) * BONE_STRUCT_SIZE > qint64(payload.n))
        return;
    geo.skeleton.resize(count);
    for (int i = 0; i < count; ++i) {
        const int rec = base + i * BONE_STRUCT_SIZE;
        ModelJoint& j = geo.skeleton[i];
        const quint32 hash = payload.u32(rec + BONE_HASH_OFF);
        j.name = QStringLiteral("bone_%1").arg(hash, 8, 16, QLatin1Char('0'));
        j.nameHash = hash;
        j.parent = qint16(payload.u16(rec + BONE_PARENT_OFF));   // -1 for root
        const PRS local = readPRS(payload, rec + BONE_LOCAL_TRS_OFF);
        j.localMatrix  = composeBoneMatrix(local);
        j.inverseBind  = composeBoneMatrix(readPRS(payload, rec + BONE_INVBIND_OFF));
        j.restQ = {{local.qx, local.qy, local.qz, local.qw}};   // D4-native, pre axis-swap
        j.restT = {{local.tx, local.ty, local.tz}};
        j.restS = {{local.sx, local.sy, local.sz}};
    }
    // ── AUTHORED cloth-bone split (BoneData.nBaseBoneCount / nClothBoneCount) ─────────
    // The bone array is authored base-bones-first, cloth-bones-last, and the split sits
    // at fixed offsets around the ptBoneStructure array ref: nBaseBoneCount 4 bytes
    // BEFORE it, nClothBoneCount 16 bytes AFTER (nMaxAnimLOD follows). Verified against
    // the d4data JSON on 6/6 corpus pieces (spiF/druF bodies + armor; counts sum to the
    // parsed bone count exactly). This replaces the index-split GUESS ("cloth = appended
    // after the first piece") that broke every class whose BODY carries cloth: merged
    // spiritborn outfits put base00's 89 authored cloth bones BELOW the guessed
    // boundary (never simulated) and other pieces' base bones above it (simulated,
    // undriven, free-falling to the divergence cap — the measured 'explosion').
    // Self-validating: only applied when the counts sum to the parsed count.
    if (refOff >= 4 && refOff + 20 <= meta.n) {
        const quint32 nBase  = meta.u32(refOff - 4);
        const quint32 nCloth = meta.u32(refOff + 16);
        // Summed as 64-bit and range-checked BEFORE narrowing. `nBase + nCloth` wrapped in u32,
        // so e.g. nBase = 0xFFFFFFFF still passed `nBase > 0` and narrowed to -1, and the loop
        // then wrote before the start of the array. The counts are also required to be sane
        // rather than merely to add up.
        const qint64 sum = qint64(nBase) + qint64(nCloth);
        if (nBase > 0 && nBase <= quint32(count) && sum == qint64(count)) {
            for (int i = int(nBase); i < count; ++i) geo.skeleton[i].cloth = true;
            if (geo.nBaseBones <= 0) geo.nBaseBones = int(nBase);
        }
    }
}

}  // namespace

// ── Public entry ─────────────────────────────────────────────────────
// Parse the authored NvCloth collision capsules from every ClothData in the .app and
// append them to outCaps (deduped — cloths share the body capsules). Schema-exact:
//   ClothData = 720B: dmClothDataMirror[288] header (counts @+252/258/268/278), then 27
//   DT_VARIABLEARRAY ptrs (16B: pad,pad,dataOffset@8,dataSize@12). ptCapsuleDefs @+656.
//   dmClothCapsuleDefMirror = 80B: q@0(vec4) p@16(vec4) r1@48 r2@52 h@56 fric@60 bone@64.
// boneIndex refers to the garment skeleton; mergeGeometries remaps it like vertex joints.
void parseClothCapsules(const Reader& meta, const Reader& payload, QVector<ClothCapsule>& outCaps,
                        QVector<ClothSim>& outSims, bool diag, const QString& appName = QString())
{
    QString dbg;
    for (int off = 0; off + 16 <= meta.n; off += 4) {
        if (meta.u32(off) != 0 || meta.u32(off + 4) != 0) continue;   // DT_VARIABLEARRAY padding
        if (meta.i32(off + 12) != 720) continue;                       // dataSize == sizeof(ClothData)
        const int dOff = meta.i32(off + 8);
        auto valid = [&](const Reader& r, int b) {
            if (!r.in(b, 720)) return false;
            const int vc = r.u16(b + 252), tc = r.u16(b + 258), con = r.u16(b + 268), cc = r.u16(b + 278);
            return vc > 0 && vc < 4000 && tc < 4000 && con > 0 && cc <= 64;   // rejects render-mesh false positives
        };
        const Reader* cd = valid(payload, dOff) ? &payload : (valid(meta, dOff) ? &meta : nullptr);
        if (!cd) continue;
        const int base = dOff, caps = cd->u16(base + 278);
        const int capOff = cd->i32(base + 656 + 8), capSz = cd->i32(base + 656 + 12);
        const Reader* cr = payload.in(capOff, capSz) ? &payload : (meta.in(capOff, capSz) ? &meta : nullptr);
        if (!cr) continue;
        for (int i = 0; i < caps; ++i) {
            const int b = capOff + i * 80;
            if (!cr->in(b, 80)) break;
            ClothCapsule c;
            // dmTransformMirror in the binary is {position@+0 (vec4), quaternion@+16 (vec4)}
            // — the @+0 field reads as a small non-unit vector (a local offset) while @+16
            // reads as a unit quaternion (0.707-type values), confirmed from the dumped data.
            for (int k = 0; k < 3; ++k) c.localP[k] = cr->f32(b + k * 4);
            for (int k = 0; k < 4; ++k) c.localQ[k] = cr->f32(b + 16 + k * 4);
            // The skeleton/mesh are swapped z-up→y-up (see zUpToYUp / the bone PRS swap);
            // the raw capsule transform is still z-up, so apply the SAME swap or it sits
            // 90° off from the bones it attaches to. Position: (x,y,z)->(x,z,-y); quaternion
            // axis swap matches the bone path: (qx,qy,qz,qw)->(qx,qz,-qy,qw).
            zUpToYUp(c.localP[0], c.localP[1], c.localP[2]);
            { const float oqy = c.localQ[1], oqz = c.localQ[2]; c.localQ[1] = oqz; c.localQ[2] = -oqy; }
            c.radius1  = cr->f32(b + 48);
            c.radius2  = cr->f32(b + 52);
            c.height   = cr->f32(b + 56);
            c.friction = cr->f32(b + 60);
            c.boneIndex = int(cr->u16(b + 64));
            if (c.radius1 <= 0.0f || c.radius1 > 5.0f || c.height < 0.0f || c.height > 5.0f) continue;
            bool dup = false;
            for (const ClothCapsule& e : outCaps)
                if (e.boneIndex == c.boneIndex && qAbs(e.radius1 - c.radius1) < 1e-3f
                    && qAbs(e.height - c.height) < 1e-3f) { dup = true; break; }
            if (dup) continue;
            outCaps.push_back(c);
            if (diag) dbg += QStringLiteral("cap bone=%1 r1=%2 r2=%3 h=%4  q=[%5 %6 %7 %8] p=[%9 %10 %11]  scale=[%12 %13 %14]\n")
                .arg(c.boneIndex).arg(double(c.radius1),0,'f',3).arg(double(c.radius2),0,'f',3).arg(double(c.height),0,'f',3)
                .arg(double(c.localQ[0]),0,'f',3).arg(double(c.localQ[1]),0,'f',3).arg(double(c.localQ[2]),0,'f',3).arg(double(c.localQ[3]),0,'f',3)
                .arg(double(c.localP[0]),0,'f',3).arg(double(c.localP[1]),0,'f',3).arg(double(c.localP[2]),0,'f',3)
                .arg(double(cr->f32(b+32)),0,'f',3).arg(double(cr->f32(b+36)),0,'f',3).arg(double(cr->f32(b+40)),0,'f',3);
        }
        // ── Parse the sim cage (verts/masses/constraints/triangles) into a ClothSim.
        // ClothData array ptrs are 16B {pad,pad,dataOffset@8,dataSize@12}; elements:
        // bindVerts=vec4, invMasses/lengths=float, constraintIdx/triangles=word.
        {
            auto arr = [&](int fieldOff, int& outOff, int& outSz) {
                outOff = cd->i32(base + fieldOff + 8); outSz = cd->i32(base + fieldOff + 12);
            };
            auto rdr = [&](int o, int s) -> const Reader* {
                return payload.in(o, s) ? &payload : (meta.in(o, s) ? &meta : nullptr);
            };
            int bvO,bvS, imO,imS, ciO,ciS, clO,clS, trO,trS, dmO,dmS, plO,plS;
            arr(288,bvO,bvS); arr(320,imO,imS); arr(528,ciO,ciS); arr(544,clO,clS); arr(512,trO,trS); arr(704,dmO,dmS); arr(672,plO,plS);
            // Every count clamped at 0: these divide file-derived i32 sizes, and a negative one
            // reaches QList::fill()/reserve() below (conClass.fill(255, nPair)) where a negative
            // size is not merely wrong but faults. Same defect class as readBonePalette's.
            auto cnt = [](int bytes, int per) { return (bytes > 0 && per > 0) ? bytes / per : 0; };
            const int nCage = cnt(bvS, 16), nMass = cnt(imS, 4), nPair = cnt(ciS, 4),
                      nLen  = cnt(clS, 4),  nTri  = cnt(trS, 6), nMap  = cnt(dmS, 2),
                      nPlane = cnt(plS, 48);
            ClothSim sim;
            const Reader* rbv = rdr(bvO,bvS); const Reader* rim = rdr(imO,imS);
            const Reader* rci = rdr(ciO,ciS); const Reader* rcl = rdr(clO,clS); const Reader* rtr = rdr(trO,trS);
            if (rbv && nCage > 0 && nCage < 8000) {
                sim.vertCount = nCage;
                sim.bindVerts.reserve(nCage * 3);
                for (int k = 0; k < nCage; ++k) {
                    float x = rbv->f32(bvO+k*16), y = rbv->f32(bvO+k*16+4), z = rbv->f32(bvO+k*16+8);
                    // A NaN cage position makes the render-vert nearest-search leave bestV=-1
                    // (NaN compares false), which would index a vertex array at -1. Snap to 0.
                    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) x = y = z = 0.0f;
                    zUpToYUp(x, y, z);                     // match the mesh/skeleton/capsule space
                    sim.bindVerts.push_back(x); sim.bindVerts.push_back(y); sim.bindVerts.push_back(z);
                }
                if (rim && nMass == nCage) for (int k=0;k<nMass;++k) sim.invMasses.push_back(rim->f32(imO+k*4));
                if (rci) for (int k=0;k<nPair*2;++k) sim.constraintIdx.push_back(rci->u16(ciO+k*2));
                if (rcl) for (int k=0;k<nLen;++k) sim.constraintLen.push_back(rcl->f32(clO+k*4));
                if (rtr) for (int k=0;k<nTri*3;++k) sim.triangles.push_back(rtr->u16(trO+k*2));
                // Plane colliders: same dmTransformMirror as capsules (pos@0, quat@16),
                // swapped z-up→y-up to match; stiffness@32, friction@36, boneIndex@40.
                if (const Reader* rpl = rdr(plO,plS)) for (int k=0; k<nPlane && k<256; ++k) {
                    const int b = plO + k*48; if (!rpl->in(b,48)) break;
                    ClothPlane pn;
                    for (int j=0;j<3;++j) pn.localP[j] = rpl->f32(b + j*4);
                    for (int j=0;j<4;++j) pn.localQ[j] = rpl->f32(b + 16 + j*4);
                    zUpToYUp(pn.localP[0], pn.localP[1], pn.localP[2]);
                    { const float oqy=pn.localQ[1], oqz=pn.localQ[2]; pn.localQ[1]=oqz; pn.localQ[2]=-oqy; }
                    pn.stiffness = rpl->f32(b + 32);
                    pn.friction  = rpl->f32(b + 36);
                    pn.boneIndex = int(rpl->u16(b + 40));
                    sim.planes.push_back(pn);
                }
                // Per-vert TETHER length (ptAttachmentLengths @ +400) — absolute wu, see
                // ModelGeometry.h. nCage here is the array CAPACITY (bvS/16); the authored
                // real particle count lives in the header.
                { int alO, alS; arr(400, alO, alS); const Reader* ral = rdr(alO, alS);
                  if (ral && alS/4 == nCage) for (int k=0;k<nCage;++k) sim.attachLen.push_back(ral->f32(alO+k*4)); }
                // ── Authored driving system (tools/d4cloth/FINDINGS.md F2) ──────────────
                sim.nRealVerts = cd->u16(base + 252);              // vertexCount (real particles)
                const int simBoneCount   = cd->u16(base + 274);
                const int simDriverCount = cd->u16(base + 276);
                // ptWeights @464: vec4 per vert — the cage's authored skinning weights.
                { int o,s2; arr(464,o,s2); const Reader* r2=rdr(o,s2);
                  if (r2 && s2 == nCage*16)
                      for (int k=0;k<nCage;++k) for (int c=0;c<4;++c) sim.drvW.push_back(r2->f32(o+k*16+c*4)); }
                // ptDriverInfluences @480: 4 x u16 per vert — driver indices per weight lane.
                { int o,s2; arr(480,o,s2); const Reader* r2=rdr(o,s2);
                  if (r2 && s2 == nCage*8)
                      for (int k=0;k<nCage*4;++k) sim.drvInf.push_back(r2->u16(o+k*2)); }
                // ptKinematicRoots @432: per vert, the tether anchor particle.
                { int o,s2; arr(432,o,s2); const Reader* r2=rdr(o,s2);
                  if (r2 && s2 == nCage*2)
                      for (int k=0;k<nCage;++k) sim.kinRoots.push_back(r2->u16(o+k*2)); }
                // ptFollowerIndices @496: per REAL vert, the bone that follows the particle.
                { int o,s2; arr(496,o,s2); const Reader* r2=rdr(o,s2);
                  if (r2 && sim.nRealVerts > 0 && s2/2 >= sim.nRealVerts && s2/2 <= nCage)
                      for (int k=0;k<sim.nRealVerts;++k) {
                          const quint16 f = r2->u16(o+k*2);
                          sim.followerBone.push_back(f == 0xFFFF ? -1 : int(f));
                      } }
                // ptBlendWeights @336: per-vert sim<->skinned blend weight.
                { int o,s2; arr(336,o,s2); const Reader* r2=rdr(o,s2);
                  if (r2 && s2 == nCage*4)
                      for (int k=0;k<nCage;++k) sim.blendW.push_back(r2->f32(o+k*4)); }
                // Constraint classes from the cluster ranges @592 warp / @608 weft /
                // @624 shear / @640 bend ({u16 start, u16 end} into the constraint list;
                // verified to partition [0..constraintCount) exactly on 12/12 blocks).
                { sim.conClass.fill(255, nPair);
                  const int clsOff[4] = { 592, 608, 624, 640 };
                  for (int cls = 0; cls < 4; ++cls) {
                      int o,s2; arr(clsOff[cls], o, s2); const Reader* r2 = rdr(o, s2);
                      if (!r2) continue;
                      for (int c = 0; c*4 + 4 <= s2; ++c) {
                          const int st = r2->u16(o+c*4), en = r2->u16(o+c*4+2);
                          for (int e = st; e < en && e < nPair; ++e) sim.conClass[e] = quint8(cls);
                      }
                  }
                }
                // ptDriverMap @704: boneCount entries, bone -> driver index (mostly 0xFFFF).
                // Inverted here to driver -> bone (validated: three cages agree to 0.0000 wu
                // with an independent skinning source — tools/d4cloth/VALIDATION.md).
                { int o,s2; arr(704,o,s2); const Reader* r2=rdr(o,s2);
                  if (r2 && simDriverCount > 0 && simDriverCount <= 64 && s2 == simBoneCount*2) {
                      sim.drvBone.fill(-1, simDriverCount);
                      for (int b=0;b<simBoneCount;++b) {
                          const quint16 dv = r2->u16(o+b*2);
                          if (dv < simDriverCount && sim.drvBone[dv] < 0) sim.drvBone[dv] = b;
                      }
                  } }
                // ClothData.name (32-byte field @ +216) → keys the matching .clt.json tuning.
                { QByteArray nm; for (int k=0;k<32;++k){ const char c = char(cd->u16(base+216+k) & 0xFF); if (!c) break; nm.append(c); }
                  sim.name = QString::fromLatin1(nm); }
                // Authoritative tuning link (resolveClothTuning): which appearance this
                // block came from + its dataOffset, pairing it with the .app.json's
                // per-sub-object snoCloth reference.
                sim.srcApp = appName;
                sim.srcOffset = base;
                outSims.push_back(sim);
            }
            if (diag) {
                int pinned=0; for (int k=0;k<sim.invMasses.size();++k) if (sim.invMasses[k]==0.0f) ++pinned;
                dbg += QStringLiteral("SIM '%1' cage=%2 pinned=%3 constraints=%4 attachLen=%5\n")
                    .arg(sim.name).arg(nCage).arg(pinned).arg(nPair).arg(sim.attachLen.size());
            }
        }
    }
    if (diag && !dbg.isEmpty()) {
        QFile f(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("cloth_parse.txt")));
        if (f.open(QIODevice::Append | QIODevice::Text)) { f.write(dbg.toUtf8()); f.close(); }
    }
}

// Cloth-only entry point (see ModelParser.h): the ClothData scan without the mesh work.
void ModelParser::parseClothOnly(const QByteArray& metaBytes, const QByteArray& payloadBytes,
                                 QVector<ClothCapsule>& caps, QVector<ClothSim>& sims,
                                 const QString& appName)
{
    Reader meta(metaBytes), payload(payloadBytes);
    if (payload.n < 0x10) return;
    parseClothCapsules(meta, payload, caps, sims, false, appName);
}

// ── resolveClothTuning (see ModelParser.h) ───────────────────────────────────────────
// Step 1 helper: dataOffset → snoCloth-name map for one appearance, from its .app.json.
// Walks tStructure.ptChunks[].ptLODs[].ptSubObjects[]: a sub-object whose ptClothData
// value has dataSize==720 pairs its dataOffset with ptAppearanceMaterials
// [nMaterialIndex].ptSOAs[].snoCloth.name. Cached per appearance (the audit sweeps
// thousands; the wardrobe re-fills on every rebuild).
static QHash<int, QString> snoClothMapFor(const QString& d4, const QString& appName)
{
    static QHash<QString, QHash<int, QString>> cache;
    const auto it = cache.constFind(appName);
    if (it != cache.constEnd()) return it.value();
    QHash<int, QString> map;
    QFile f(d4 + QStringLiteral("/json/base/meta/Appearance/") + appName + QStringLiteral(".app.json"));
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        const QJsonArray mats = root.value(QStringLiteral("ptAppearanceMaterials")).toArray();
        // snoHighQualityClothOverride WINS over snoCloth. The tool renders at full quality, so the
        // HQ tuning is the one the game would be using, and nothing here read it at all — every
        // piece that ships one was being simulated with its low-detail parameters instead.
        //
        // Not a rare case: in a 1200-appearance sample, 392 SOAs carry both and 391 of those name a
        // DIFFERENT file, so the two are genuinely distinct tunings rather than duplicates. A
        // further 14 carry ONLY the override, and those were resolving to no tuning whatsoever.
        auto clothNameForMat = [&](int mi) -> QString {
            if (mi < 0 || mi >= mats.size()) return QString();
            for (const char* key : {"snoHighQualityClothOverride", "snoCloth"})
                for (const QJsonValue& soaV : mats.at(mi).toObject().value(QStringLiteral("ptSOAs")).toArray()) {
                    const QString n = soaV.toObject().value(QLatin1String(key)).toObject()
                                          .value(QStringLiteral("name")).toString();
                    if (!n.isEmpty()) return n;
                }
            return QString();
        };
        for (const QJsonValue& chV : root.value(QStringLiteral("tStructure")).toObject()
                                         .value(QStringLiteral("ptChunks")).toArray())
            for (const QJsonValue& lodV : chV.toObject().value(QStringLiteral("ptLODs")).toArray())
                for (const QJsonValue& soV : lodV.toObject().value(QStringLiteral("ptSubObjects")).toArray()) {
                    const QJsonObject so = soV.toObject();
                    QJsonValue cdv = so.value(QStringLiteral("ptClothData")).toObject()
                                       .value(QStringLiteral("value"));
                    QJsonObject v = cdv.isArray() ? cdv.toArray().at(0).toObject() : cdv.toObject();
                    if (v.value(QStringLiteral("dataSize")).toInt() != 720) continue;
                    const int off = v.value(QStringLiteral("dataOffset")).toInt(-1);
                    const QString cn = clothNameForMat(so.value(QStringLiteral("nMaterialIndex")).toInt(-1));
                    if (off >= 0 && !cn.isEmpty() && !map.contains(off)) map.insert(off, cn);
                }
    }
    cache.insert(appName, map);
    return map;
}

static QJsonObject readTuningFile(const QString& path)
{
    QFile cf(path);
    if (!cf.open(QIODevice::ReadOnly)) return QJsonObject();
    return QJsonDocument::fromJson(cf.readAll()).object()
               .value(QStringLiteral("tClothTuning")).toObject();
}

QJsonObject ModelParser::resolveClothTuning(const QString& d4, const ClothSim& sim, QString* howOut)
{
    auto setHow = [&](const QString& h) { if (howOut) *howOut = h; };
    setHow(QStringLiteral("FAILED"));

    // 1. snoCloth — the game's authoritative per-item physics link.
    if (!sim.srcApp.isEmpty() && sim.srcOffset >= 0) {
        const QString cn = snoClothMapFor(d4, sim.srcApp).value(sim.srcOffset);
        if (!cn.isEmpty()) {
            const QJsonObject t = readTuningFile(
                d4 + QStringLiteral("/json/base/meta/Cloth/") + cn + QStringLiteral(".clt.json"));
            if (!t.isEmpty()) { setHow(QStringLiteral("snoCloth:") + cn); return t; }
            // snoCloth named a file we can't read — fall through to the name heuristics,
            // but note the miss when diagnostics are on.
            if (qEnvironmentVariableIsSet("D4_DUMP_CLOTH"))
                qInfo("cloth-tuning: '%s' snoCloth '%s' has no readable .clt.json — falling back",
                      qPrintable(sim.srcApp), qPrintable(cn));
        }
    }

    // 2. Embedded-name fallback (pieces with no snoCloth, e.g. barF_stor263_HLM feathers).
    const QString& name = sim.name;
    if (name.isEmpty()) return QJsonObject();
    QJsonObject t;
    // Suffix conventions measured in the data: _sim, bare, and _HQ_sim.
    for (const QString& suffix : { QStringLiteral("_sim"), QString(), QStringLiteral("_HQ_sim") }) {
        t = readTuningFile(d4 + QStringLiteral("/json/base/meta/Cloth/") + name + suffix
                           + QStringLiteral(".clt.json"));
        if (!t.isEmpty()) { setHow(suffix.isEmpty() ? QStringLiteral("bare") : suffix); return t; }
    }
    // PREFIX FALLBACK — embedded block name and .clt.json filename follow different
    // conventions on some pieces (measured: 'barF_stor263_HLM_a_HQO' vs file
    // 'barF_stor263_HLM_feather_HQ_sim.clt.json'). Strip trailing '_' tokens until
    // files match that prefix; longest shared prefix with the full name wins.
    static QStringList allClt;   // one dir listing per session (15k files)
    if (allClt.isEmpty())
        allClt = QDir(d4 + QStringLiteral("/json/base/meta/Cloth"))
                     .entryList({ QStringLiteral("*.clt.json") }, QDir::Files);
    QStringList tok = name.split(QLatin1Char('_'));
    for (int cut = 1; cut <= 2 && tok.size() - cut >= 3 && t.isEmpty(); ++cut) {
        const QString pfx = QStringList(tok.mid(0, tok.size() - cut)).join(QLatin1Char('_'))
                            + QLatin1Char('_');
        QString best; int bestLen = -1;
        for (const QString& fn : allClt) {
            if (!fn.startsWith(pfx, Qt::CaseInsensitive)) continue;
            int c = 0;   // shared prefix with the FULL embedded name breaks ties
            while (c < fn.size() && c < name.size()
                   && fn[c].toLower() == name[c].toLower()) ++c;
            if (c > bestLen) { bestLen = c; best = fn; }
        }
        if (!best.isEmpty()) {
            t = readTuningFile(d4 + QStringLiteral("/json/base/meta/Cloth/") + best);
            if (!t.isEmpty()) {
                setHow(QStringLiteral("prefix:") + best);
                if (qEnvironmentVariableIsSet("D4_DUMP_CLOTH"))
                    qInfo("cloth-tuning: '%s' resolved via prefix -> '%s'",
                          qPrintable(name), qPrintable(best));
            }
        }
    }
    return t;
}

ModelGeometry ModelParser::parseApp(const QByteArray& metaBytes, const QByteArray& payloadBytes,
                                    const QString& appName)
{
    ModelGeometry geo;
    Reader meta(metaBytes), payload(payloadBytes);
    if (meta.n < 0xC0 || meta.u32(0) != APP_META_MAGIC) return geo;
    if (payload.n < 0x10) return geo;
    parseClothCapsules(meta, payload, geo.clothCapsules, geo.clothSims,
                       qEnvironmentVariableIsSet("D4_DUMP_CLOTH"), appName);

    const QVector<VB> vbs = scanVertexBuffers(meta, payload.n);
    const QVector<IB> ibs = scanIndexBuffers(meta, payload.n, vbs);
    if (vbs.isEmpty() || ibs.isEmpty()) return geo;

    // Record VB layouts for the informational VertexBuffers panel.
    for (const VB& vb : vbs) {
        VertexBufferInfo info;
        info.index = vb.arrayIndex;
        info.stride = vb.stride;
        info.vertexCount = vb.stride > 0 ? vb.dataSize / vb.stride : 0;
        for (const VElem& e : resolveLayout(vb.stride, meta, vb.fileOffset).elems)
            info.attrs.push_back({e.semantic, e.format, e.offset});
        geo.vertexBuffers.push_back(info);
    }

    // The streamed LOD0 buffer is the fOptional one. Some single-LOD meshes (e.g. the
    // nude "test999" body) ship only the non-optional buffer, so fall back to the first
    // available buffer when no optional one exists.
    const VB* vb0 = nullptr; for (const VB& v : vbs) if (v.fOptional) { vb0 = &v; break; }
    if (!vb0 && !vbs.isEmpty()) vb0 = &vbs.first();
    const IB* ib0 = nullptr; for (const IB& i : ibs) if (i.fOptional) { ib0 = &i; break; }
    if (!ib0 && !ibs.isEmpty()) ib0 = &ibs.first();
    if (!vb0 || !ib0) return geo;

    const int stride = vb0->stride;
    if (stride <= 0) return geo;
    const int vcount = vb0->dataSize / stride;
    const VLayout layout = resolveLayout(stride, meta, vb0->fileOffset);
    if (layout.elems.isEmpty()) return geo;
    if (!payload.in(vb0->dataOffset, vb0->dataSize)) return geo;

    const VElem* ePos = findElem(layout, SEM_POSITION);
    const VElem* eNrm = findElem(layout, SEM_NORMAL);
    const VElem* eUv  = findElem(layout, SEM_TEXCOORD_0);
    const VElem* eCol = findElem(layout, SEM_COLOR_0);        // u8x4 detail-blend weights
    const VElem* eCol1 = findElem(layout, SEM_COLOR_1);       // u8x4 (alt mask candidate)
    const VElem* eUv1 = findElem(layout, SEM_TEXCOORD_1);     // f16x2 second UV
    const VElem* eJnt = findElem(layout, SEM_BLENDINDICES);   // u8x4 (skinned)
    const VElem* eWgt = findElem(layout, SEM_BLENDWEIGHTS);   // unorm4 (skinned)
    if (!ePos) return geo;

    // Decode the full shared vertex stream (D4-native), applying z_up_to_y_up.
    QVector<MeshVertex> verts(vcount);
    for (int i = 0; i < vcount; ++i) {
        const int base = vb0->dataOffset + i * stride;
        MeshVertex& mv = verts[i];
        mv.px = payload.f32(base + ePos->offset);
        mv.py = payload.f32(base + ePos->offset + 4);
        mv.pz = payload.f32(base + ePos->offset + 8);
        // Guard against a malformed payload yielding NaN/Inf positions: they poison the
        // model's bounds → camera framing → MVP matrix (garbage/blank render, and NaN can
        // upset some GL drivers). Snap any non-finite coordinate to 0.
        if (!std::isfinite(mv.px) || !std::isfinite(mv.py) || !std::isfinite(mv.pz))
            mv.px = mv.py = mv.pz = 0.0f;
        zUpToYUp(mv.px, mv.py, mv.pz);
        if (eNrm) {
            float nx = snorm8(payload.u8(base + eNrm->offset));
            float ny = snorm8(payload.u8(base + eNrm->offset + 1));
            float nz = snorm8(payload.u8(base + eNrm->offset + 2));
            const float L = std::sqrt(nx*nx + ny*ny + nz*nz);
            if (L > 1e-9f) { nx /= L; ny /= L; nz /= L; }
            zUpToYUp(nx, ny, nz);
            mv.nx = nx; mv.ny = ny; mv.nz = nz;
        }
        if (eUv) {
            mv.u = halfToFloat(payload.u16(base + eUv->offset));
            mv.v = halfToFloat(payload.u16(base + eUv->offset + 2));
        }
        if (eCol) {   // COLOR_0 = R8G8B8A8_UNORM detail-blend weights
            mv.cr = payload.u8(base + eCol->offset)     / 255.0f;
            mv.cg = payload.u8(base + eCol->offset + 1) / 255.0f;
            mv.cb = payload.u8(base + eCol->offset + 2) / 255.0f;
            mv.ca = payload.u8(base + eCol->offset + 3) / 255.0f;
            mv.hasColor = true;
        }
        if (eCol1) {  // COLOR_1 (diagnostic)
            mv.c2r = payload.u8(base + eCol1->offset)     / 255.0f;
            mv.c2g = payload.u8(base + eCol1->offset + 1) / 255.0f;
            mv.c2b = payload.u8(base + eCol1->offset + 2) / 255.0f;
            mv.c2a = payload.u8(base + eCol1->offset + 3) / 255.0f;
            mv.hasColor1 = true;
        }
        if (eUv1) {   // TEXCOORD_1 (diagnostic)
            mv.u1 = halfToFloat(payload.u16(base + eUv1->offset));
            mv.v1 = halfToFloat(payload.u16(base + eUv1->offset + 2));
            mv.hasUv1 = true;
        }
        // Raw blend indices (segment-local palette bytes) + weights. The
        // palette→global remap happens per-submesh during the gather below.
        if (eJnt) for (int k = 0; k < 4; ++k)
            mv.joints[k] = payload.u8(base + eJnt->offset + k);
        if (eWgt) for (int k = 0; k < 4; ++k)
            mv.weights[k] = payload.u8(base + eWgt->offset + k) / 255.0f;
    }

    // Raw u16 indices.
    const int icount = ib0->dataSize / 2;
    if (!payload.in(ib0->dataOffset, ib0->dataSize)) return geo;
    QVector<quint16> rawIdx(icount);
    for (int i = 0; i < icount; ++i) rawIdx[i] = payload.u16(ib0->dataOffset + i * 2);

    QVector<Segment> segs = scanSegments(meta);
    QVector<SubObj> subs = findSubObjects(meta, metaBytes, segs);
    QVector<SubObj> lod0;
    for (const SubObj& s : subs)
        if (s.vbi == vb0->arrayIndex && s.ibi == ib0->arrayIndex) lod0.push_back(s);
    if (lod0.isEmpty()) return geo;

    std::sort(lod0.begin(), lod0.end(), [](const SubObj& a, const SubObj& b){
        if (a.seg.io != b.seg.io) return a.seg.io < b.seg.io;
        return a.seg.vo < b.seg.vo;
    });

    // One MeshPrimitive per LOD0 SubObject, with a remapped local vertex set.
    for (const SubObj& s : lod0) {
        const int baseV = int(s.seg.vo) / stride;
        const int endIdx = int(s.seg.io + s.seg.ic);
        if (endIdx > rawIdx.size()) return ModelGeometry();
        const QVector<int>& palette = s.seg.bonePalette;   // segment-local → global bone
        MeshPrimitive prim;
        prim.materialName = QStringLiteral("Material_%1").arg(s.mat);
        prim.materialIndex = s.mat;
        prim.subObjectHash = s.hash;
        prim.slotHash = s.slotHash;
        QHash<int, quint32> remap;
        for (int i = int(s.seg.io); i + 2 < endIdx; i += 3) {
            const int a = rawIdx[i], b = rawIdx[i + 1], c = rawIdx[i + 2];
            if (a == b || b == c || a == c) continue;       // drop degenerate
            const int g[3] = {a + baseV, b + baseV, c + baseV};
            for (int k = 0; k < 3; ++k) {
                if (g[k] < 0 || g[k] >= vcount) return ModelGeometry();
                auto it = remap.constFind(g[k]);
                quint32 local;
                if (it == remap.constEnd()) {
                    local = quint32(prim.vertices.size());
                    MeshVertex mv = verts[g[k]];
                    if (!palette.isEmpty())     // map JOINTS_0 bytes → global bone index
                        for (int j = 0; j < 4; ++j) {
                            const int byte = mv.joints[j];
                            mv.joints[j] = (byte >= 0 && byte < palette.size())
                                               ? quint16(palette[byte]) : 0;
                        }
                    prim.vertices.push_back(mv);
                    remap.insert(g[k], local);
                } else {
                    local = it.value();
                }
                prim.indices.push_back(local);
            }
        }
        if (prim.indices.isEmpty()) continue;
        geo.primitives.push_back(std::move(prim));
    }

    if (!geo.primitives.isEmpty())
        parseSkeleton(meta, payload, geo);

    geo.valid = !geo.primitives.isEmpty();
    return geo;
}

ModelGeometry ModelParser::mergeGeometries(const QVector<ModelGeometry>& parts)
{
    ModelGeometry out;
    QHash<QString, int> matIdx;     // stable material index per unique name
    QHash<quint32, int> boneByHash; // bone-name hash → unified skeleton index

    // Build ONE unified skeleton across all pieces, keyed by bone-name hash. Pieces
    // share the base body rig but each armor piece adds its own CLOTH/PHYSICS bones
    // (e.g. body=190 bones, armor=305) — keeping only the first skeleton would drop
    // those, so the cloth bones (and any verts weighted to them) wouldn't animate.
    // Unifying by hash preserves every cloth bone and lets each piece's joints remap
    // onto the shared rig. Pieces with NO skeleton (e.g. a weapon already rigidly
    // bound to a body bone index) keep their joint indices as-is.
    bool firstSkel = true;
    for (const ModelGeometry& g : parts) {
        if (!g.valid) continue;

        // The old single pass resolved `remap[pp]` inline and fell back to -1 whenever pp >= i,
        // i.e. whenever a piece stored a parent AFTER its child. D4 armor rigs do exactly that
        // (cloth chains are authored in chain order, not hierarchy order), so those bones silently
        // became roots — their world transform collapsed to their local one, they landed at the
        // model origin, and every child drew a connection line back across the model.
        //
        // Resolve properly, and append new bones PARENTS-FIRST: skinning, rest-pose construction,
        // the overlays and the glTF exporter all index `parent` assuming it precedes the child, so
        // the unified rig has to honour that invariant no matter how the piece authored it.
        QVector<int>    remap(g.skeleton.size(), -1);
        QVector<quint8> isNew(g.skeleton.size(), 0);
        for (int i = 0; i < g.skeleton.size(); ++i) {
            const int idx = boneByHash.value(g.skeleton[i].nameHash, -1);
            if (idx >= 0) {
                remap[i] = idx;
                // A bone any piece authors as cloth IS cloth (BoneData split, see
                // ModelJoint::cloth) — the union survives merge order.
                if (g.skeleton[i].cloth) out.skeleton[idx].cloth = true;
            } else isNew[i] = 1;
        }
        auto emitBone = [&](int i, int parentUnified) {
            const int idx = out.skeleton.size();
            ModelJoint j = g.skeleton[i];
            j.parent = parentUnified;
            out.skeleton.append(j);
            boneByHash.insert(j.nameHash, idx);
            remap[i] = idx;
        };
        for (bool progress = true; progress; ) {
            progress = false;
            for (int i = 0; i < g.skeleton.size(); ++i) {
                if (!isNew[i] || remap[i] >= 0) continue;
                const int pp = g.skeleton[i].parent;
                if (pp >= 0 && pp < remap.size() && remap[pp] < 0) continue;   // parent not placed yet
                emitBone(i, (pp >= 0 && pp < remap.size()) ? remap[pp] : -1);
                progress = true;
            }
        }
        // Parent cycle or out-of-range parent: place the remainder as roots rather than dropping
        // them (a missing bone would silently unweight every vertex bound to it).
        for (int i = 0; i < g.skeleton.size(); ++i)
            if (isNew[i] && remap[i] < 0) emitBone(i, -1);
        // The first skeletal piece establishes the base/physics-bone boundary. If that piece
        // carries an AUTHORED base-bone count (g.nBaseBones, e.g. a mount/pet whose own
        // skeleton includes its mane/tail physics bones), honour it — the first piece's bones
        // are appended in order starting at index 0, so its base count maps 1:1 into unified
        // space. Otherwise fall back to the heuristic "first piece = base rig, later pieces add
        // the cloth bones" (correct for player body + gear, where nBaseBones is unset/0).
        if (firstSkel && !g.skeleton.isEmpty()) {
            out.nBaseBones = (g.nBaseBones > 0 && g.nBaseBones <= g.skeleton.size())
                             ? g.nBaseBones : out.skeleton.size();
            firstSkel = false;
        }

        const bool hasSkel = !g.skeleton.isEmpty();
        for (MeshPrimitive p : g.primitives) {   // copy (each primitive is self-contained)
            int idx = matIdx.value(p.materialName, -1);
            if (idx < 0) { idx = matIdx.size(); matIdx.insert(p.materialName, idx); }
            p.materialIndex = idx;
            if (hasSkel)   // remap this piece's per-vertex joints onto the unified rig
                for (MeshVertex& v : p.vertices)
                    for (int k = 0; k < 4; ++k)
                        if (v.weights[k] > 0.0f && v.joints[k] < remap.size())
                            v.joints[k] = quint16(remap[v.joints[k]]);
            out.primitives.append(p);
        }
        // Remap the authored cloth capsules' bone indices onto the unified skeleton and
        // collect them (deduped — every piece references the same body capsules).
        for (ClothCapsule c : g.clothCapsules) {
            if (hasSkel && c.boneIndex >= 0 && c.boneIndex < remap.size())
                c.boneIndex = remap[c.boneIndex];
            bool dup = false;
            for (const ClothCapsule& e : out.clothCapsules)
                if (e.boneIndex == c.boneIndex && qAbs(e.radius1 - c.radius1) < 1e-3f
                    && qAbs(e.height - c.height) < 1e-3f) { dup = true; break; }
            if (!dup) out.clothCapsules.append(c);
        }
        // Sim cages are self-contained (cage-local vertex indices, world-space rest verts),
        // so they carry across; the plane colliders' AND the authored driving system's
        // bone indices (followerBone / drvBone — piece-skeleton indices) need remapping.
        for (ClothSim s : g.clothSims) {
            if (s.vertCount <= 0) continue;
            if (hasSkel) {
                for (ClothPlane& pn : s.planes)
                    if (pn.boneIndex >= 0 && pn.boneIndex < remap.size()) pn.boneIndex = remap[pn.boneIndex];
                for (int& b : s.followerBone)
                    b = (b >= 0 && b < remap.size()) ? remap[b] : -1;
                for (int& b : s.drvBone)
                    b = (b >= 0 && b < remap.size()) ? remap[b] : -1;
                if (s.spaceBone >= 0)
                    s.spaceBone = (s.spaceBone < remap.size()) ? remap[s.spaceBone] : -1;
            }
            out.clothSims.append(s);
        }
    }
    out.valid = !out.primitives.isEmpty();
    return out;
}

namespace {
using Mat4b = std::array<float, 16>;   // column-major

Mat4b composeTRSb(const float q[4], const float t[3], const float s[3])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float r00 = 1 - 2 * (y*y + z*z), r01 = 2 * (x*y - w*z), r02 = 2 * (x*z + w*y);
    const float r10 = 2 * (x*y + w*z), r11 = 1 - 2 * (x*x + z*z), r12 = 2 * (y*z - w*x);
    const float r20 = 2 * (x*z - w*y), r21 = 2 * (y*z + w*x), r22 = 1 - 2 * (x*x + y*y);
    return {{ r00*s[0], r10*s[0], r20*s[0], 0,
              r01*s[1], r11*s[1], r21*s[1], 0,
              r02*s[2], r12*s[2], r22*s[2], 0,
              t[0],     t[1],     t[2],     1 }};
}
Mat4b mat4mulb(const Mat4b& a, const Mat4b& b)
{
    Mat4b r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr)
            r[c*4+rr] = a[0*4+rr]*b[c*4+0] + a[1*4+rr]*b[c*4+1]
                      + a[2*4+rr]*b[c*4+2] + a[3*4+rr]*b[c*4+3];
    return r;
}
}  // namespace

// (bakeRestPose removed — no callers; the exporter writes bind pose + clips directly.)
