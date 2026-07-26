#include "ClothDoc.h"

#include <cstring>

namespace d4cloth {

namespace {

// Bounds-safe little-endian reader (mirrors the app's ModelParser Reader).
struct Reader {
    const quint8* d = nullptr;
    int n = 0;
    Reader(const QByteArray& b) : d(reinterpret_cast<const quint8*>(b.constData())), n(b.size()) {}
    bool in(int off, int len) const { return d && off >= 0 && len >= 0 && off + len <= n; }
    quint8  u8(int o)  const { return in(o, 1) ? d[o] : quint8(0); }
    quint16 u16(int o) const { return in(o, 2) ? quint16(quint16(d[o]) | (quint16(d[o + 1]) << 8)) : quint16(0); }
    quint32 u32(int o) const {
        return in(o, 4) ? (quint32(d[o]) | (quint32(d[o + 1]) << 8) | (quint32(d[o + 2]) << 16)
                           | (quint32(d[o + 3]) << 24))
                        : quint32(0);
    }
    qint32 i32(int o) const { return qint32(u32(o)); }
    float  f32(int o) const { float f; quint32 v = u32(o); std::memcpy(&f, &v, 4); return f; }
};

// The 27 array slots of ClothData, in authored order (definitions.json).
struct SlotDef { const char* name; int fieldOff; };
constexpr SlotDef kSlots[] = {
    {"ptBindVertices",       288}, {"ptBindNormals",        304},
    {"ptInvMasses",          320}, {"ptBlendWeights",       336},
    {"ptAnimBlendFractions", 352}, {"ptDeltaFrames",        368},
    {"ptLevels",             384}, {"ptAttachmentLengths",  400},
    {"ptParentIndices",      416}, {"ptKinematicRoots",     432},
    {"ptTangentIndices",     448}, {"ptWeights",            464},
    {"ptDriverInfluences",   480}, {"ptFollowerIndices",    496},
    {"ptTriangles",          512}, {"ptConstraintIndices",  528},
    {"ptConstraintLengths",  544}, {"unk_8ecbb2b",          560},
    {"unk_9f71907",          576}, {"ptWarpClusters",       592},
    {"ptWeftClusters",       608}, {"ptShearClusters",      624},
    {"ptBendClusters",       640}, {"ptCapsuleDefs",        656},
    {"ptPlaneDefs",          672}, {"ptDriverBindPose",     688},
    {"ptDriverMap",          704},
};

QVector<float> asF32(const QByteArray& raw)
{
    QVector<float> v(raw.size() / 4);
    std::memcpy(v.data(), raw.constData(), size_t(v.size()) * 4);
    return v;
}
QVector<quint16> asU16(const QByteArray& raw)
{
    QVector<quint16> v(raw.size() / 2);
    std::memcpy(v.data(), raw.constData(), size_t(v.size()) * 2);
    return v;
}
QVector<std::array<float,4>> asVec4(const QByteArray& raw)
{
    QVector<std::array<float,4>> v(raw.size() / 16);
    std::memcpy(v.data(), raw.constData(), size_t(v.size()) * 16);
    return v;
}
QVector<ClothDoc::Cluster> asClusters(const QByteArray& raw)
{
    QVector<ClothDoc::Cluster> v(raw.size() / 4);
    std::memcpy(v.data(), raw.constData(), size_t(v.size()) * 4);
    return v;
}

} // namespace

quint64 fnv1a64(const QByteArray& b)
{
    quint64 h = 1469598103934665603ull;
    for (char c : b) { h ^= quint8(c); h *= 1099511628211ull; }
    return h;
}

QVector<ClothDoc> parseClothDocs(const QByteArray& metaBytes, const QByteArray& payloadBytes)
{
    QVector<ClothDoc> out;
    Reader meta(metaBytes), payload(payloadBytes);

    for (int off = 0; off + 16 <= meta.n; off += 4) {
        // DT_VARIABLEARRAY ref: {0, 0, dataOffset@8, dataSize@12} with dataSize == sizeof(ClothData).
        if (meta.u32(off) != 0 || meta.u32(off + 4) != 0) continue;
        if (meta.i32(off + 12) != 720) continue;
        const int dOff = meta.i32(off + 8);

        // Same header sanity check the app uses (rejects render-mesh false positives).
        auto valid = [&](const Reader& r, int b) {
            if (!r.in(b, 720)) return false;
            const int vc = r.u16(b + 252), tc = r.u16(b + 258),
                      con = r.u16(b + 268), cc = r.u16(b + 278);
            return vc > 0 && vc < 4000 && tc < 4000 && con > 0 && cc <= 64;
        };
        const bool inPayload = valid(payload, dOff);
        const Reader* cd = inPayload ? &payload : (valid(meta, dOff) ? &meta : nullptr);
        if (!cd) continue;
        const int base = dOff;

        // Skip duplicates: the same block offset can be referenced from more than one place
        // (LODs share cloth). One doc per distinct block.
        bool dup = false;
        for (const ClothDoc& e : out)
            if (e.blockOffset == base && e.blockInPayload == inPayload) { dup = true; break; }
        if (dup) continue;

        ClothDoc doc;
        doc.blockOffset = base;
        doc.blockInPayload = inPayload;

        // ── header ──
        { QByteArray nm;
          for (int k = 0; k < 32; ++k) { const char c = char(cd->u8(base + 216 + k)); if (!c) break; nm.append(c); }
          doc.name = QString::fromLatin1(nm); }
        doc.density          = cd->f32(base + 248);
        doc.vertexCount      = cd->u16(base + 252);
        doc.vertexCapacity   = cd->u16(base + 254);
        doc.kinematicCount   = cd->u16(base + 256);
        doc.triangleCount    = cd->u16(base + 258);
        doc.warpClusterCount = cd->u16(base + 260);
        doc.weftClusterCount = cd->u16(base + 262);
        doc.shearClusterCount= cd->u16(base + 264);
        doc.bendClusterCount = cd->u16(base + 266);
        doc.constraintCount  = cd->u16(base + 268);
        doc.unk_9460e91      = cd->u16(base + 270);
        doc.maxLevel         = cd->u16(base + 272);
        doc.boneCount        = cd->u16(base + 274);
        doc.driverCount      = cd->u16(base + 276);
        doc.capsuleCount     = cd->u16(base + 278);
        doc.planeCount       = cd->u16(base + 280);

        // ── all 27 arrays, raw ──
        for (const SlotDef& sd : kSlots) {
            ArraySlot s;
            s.name = QString::fromLatin1(sd.name);
            s.fieldOff = sd.fieldOff;
            s.dataOff  = cd->i32(base + sd.fieldOff + 8);
            s.dataSize = cd->i32(base + sd.fieldOff + 12);
            if (s.dataSize > 0) {
                if (payload.in(s.dataOff, s.dataSize)) {
                    s.present = true; s.inPayload = true;
                    s.raw = payloadBytes.mid(s.dataOff, s.dataSize);
                } else if (meta.in(s.dataOff, s.dataSize)) {
                    s.present = true; s.inPayload = false;
                    s.raw = metaBytes.mid(s.dataOff, s.dataSize);
                }
            }
            doc.arrays.push_back(s);
        }

        // ── typed views — only when the byte size divides as the layout predicts ──
        auto rawOf = [&](const char* n) -> QByteArray {
            const ArraySlot* s = doc.slot(QString::fromLatin1(n));
            return (s && s->present) ? s->raw : QByteArray();
        };
        // Per-vert arrays are sized by vertexCAPACITY, not vertexCount (measured on
        // barF_base03_TRS_cape: count=77, capacity=80, every per-vert array 80 elements).
        // Entries past vertexCount are padding/extra particles — the analyses call them out.
        const int nv  = doc.vertexCount;
        const int cap = qMax(doc.vertexCount, doc.vertexCapacity);
        auto sized = [&](const char* n, int elemBytes) -> QByteArray {
            const QByteArray r = rawOf(n);
            if (r.isEmpty()) return r;
            if (cap > 0 && r.size() == cap * elemBytes) return r;
            if (nv  > 0 && r.size() == nv  * elemBytes) return r;   // count-sized also accepted
            return QByteArray();
        };
        doc.bindVertices       = asVec4(sized("ptBindVertices", 16));
        doc.bindNormals        = asVec4(sized("ptBindNormals", 16));
        doc.weights            = asVec4(sized("ptWeights", 16));
        doc.invMasses          = asF32(sized("ptInvMasses", 4));
        doc.blendWeights       = asF32(sized("ptBlendWeights", 4));
        doc.animBlendFractions = asF32(sized("ptAnimBlendFractions", 4));
        doc.attachmentLengths  = asF32(sized("ptAttachmentLengths", 4));
        doc.levels             = asU16(sized("ptLevels", 2));
        doc.parentIndices      = asU16(sized("ptParentIndices", 2));
        doc.tangentIndices     = asU16(sized("ptTangentIndices", 2));
        doc.kinematicRoots     = asU16(sized("ptKinematicRoots", 2));
        doc.followerIndices    = asU16(sized("ptFollowerIndices", 2));
        doc.driverInfluences   = asU16(sized("ptDriverInfluences", 8));   // 4 x u16 per vert
        { const QByteArray r = sized("ptDeltaFrames", 64);                // 4x4 matrix per vert
          if (!r.isEmpty()) {
              doc.deltaFrames.resize(r.size() / 64);
              std::memcpy(doc.deltaFrames.data(), r.constData(), size_t(r.size()));
          } }
        { const QByteArray r = rawOf("ptDriverMap");                      // per-BONE (boneCount)
          if (!r.isEmpty() && ((doc.boneCount > 0 && r.size() == doc.boneCount * 2)
                               || r.size() % 2 == 0))
              doc.driverMap = asU16(r); }
        { const QByteArray r = rawOf("ptTriangles");
          if (doc.triangleCount > 0 && r.size() == doc.triangleCount * 6) doc.triangles = asU16(r);
          else if (r.size() > 0 && r.size() % 6 == 0) doc.triangles = asU16(r); }
        { const QByteArray r = rawOf("ptConstraintIndices");
          if (r.size() > 0 && r.size() % 4 == 0) doc.constraintIndices = asU16(r); }
        { const QByteArray r = rawOf("ptConstraintLengths");
          if (r.size() > 0 && r.size() % 4 == 0) doc.constraintLengths = asF32(r); }
        { const QByteArray r = rawOf("unk_8ecbb2b");
          if (r.size() > 0 && r.size() % 2 == 0) doc.unkU560 = asU16(r); }
        { const QByteArray r = rawOf("unk_9f71907");
          if (r.size() > 0 && r.size() % 4 == 0) doc.unkF576 = asF32(r); }

        auto clustersOf = [&](const char* n, int count) -> QVector<ClothDoc::Cluster> {
            const QByteArray r = rawOf(n);
            if (r.size() > 0 && (count <= 0 || r.size() == count * 4) && r.size() % 4 == 0)
                return asClusters(r);
            return {};
        };
        doc.warpClusters  = clustersOf("ptWarpClusters",  doc.warpClusterCount);
        doc.weftClusters  = clustersOf("ptWeftClusters",  doc.weftClusterCount);
        doc.shearClusters = clustersOf("ptShearClusters", doc.shearClusterCount);
        doc.bendClusters  = clustersOf("ptBendClusters",  doc.bendClusterCount);

        { const QByteArray r = rawOf("ptDriverBindPose");
          if (r.size() > 0 && r.size() % 48 == 0) {
              Reader rr(r);
              for (int i = 0; i * 48 < r.size(); ++i) {
                  ClothDoc::Frame f;
                  for (int k = 0; k < 4; ++k) f.q[k] = rr.f32(i * 48 + k * 4);
                  for (int k = 0; k < 4; ++k) f.p[k] = rr.f32(i * 48 + 16 + k * 4);
                  for (int k = 0; k < 4; ++k) f.s[k] = rr.f32(i * 48 + 32 + k * 4);
                  doc.driverBindPose.push_back(f);
              }
          } }
        { const QByteArray r = rawOf("ptCapsuleDefs");
          if (r.size() > 0 && r.size() % 80 == 0) {
              Reader rr(r);
              for (int i = 0; i * 80 < r.size(); ++i) {
                  const int b = i * 80;
                  ClothDoc::Capsule c;
                  for (int k = 0; k < 3; ++k) c.localP[k] = rr.f32(b + k * 4);
                  for (int k = 0; k < 4; ++k) c.localQ[k] = rr.f32(b + 16 + k * 4);
                  for (int k = 0; k < 3; ++k) c.scale[k]  = rr.f32(b + 32 + k * 4);
                  c.radius1 = rr.f32(b + 48); c.radius2 = rr.f32(b + 52);
                  c.height  = rr.f32(b + 56); c.friction = rr.f32(b + 60);
                  c.boneIndex = rr.u16(b + 64);
                  c.solver = rr.u8(b + 66); c.hide = rr.u8(b + 67);
                  doc.capsules.push_back(c);
              }
          } }
        { const QByteArray r = rawOf("ptPlaneDefs");
          if (r.size() > 0 && r.size() % 48 == 0) {
              Reader rr(r);
              for (int i = 0; i * 48 < r.size(); ++i) {
                  const int b = i * 48;
                  ClothDoc::Plane p;
                  for (int k = 0; k < 3; ++k) p.localP[k] = rr.f32(b + k * 4);
                  for (int k = 0; k < 4; ++k) p.localQ[k] = rr.f32(b + 16 + k * 4);
                  p.stiffness = rr.f32(b + 32); p.friction = rr.f32(b + 36);
                  p.boneIndex = rr.u16(b + 40);
                  doc.planes.push_back(p);
              }
          } }

        out.push_back(doc);
    }
    return out;
}

} // namespace d4cloth
