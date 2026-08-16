#include "model/ModelExporter.h"
#include "model/Retarget.h"

#include <QBuffer>
#include <QFile>
#include <QHash>
#include <QMatrix3x3>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QSettings>
#include <QVector3D>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <cstring>
#include <limits>

namespace {

// Apply the SAME D4-native → glTF (Z-up→Y-up) axis swap the live skinning uses
// (GLModelWidget::composeTRS): translation (x,z,-y), quaternion (x,z,-y,w) normalized,
// scale (x,z,y). Rotation is the only component that matters for the quaternion output,
// etc.; the caller reads whichever field it needs.
struct SwappedTRS { float t[3]; float q[4]; float s[3]; };
SwappedTRS swapTRS(const float q[4], const float t[3], const float s[3])
{
    SwappedTRS o;
    o.t[0] = t[0]; o.t[1] = t[2]; o.t[2] = -t[1];
    float qx = q[0], qy = q[2], qz = -q[1], qw = q[3];
    const float m = std::sqrt(qx*qx + qy*qy + qz*qz + qw*qw);
    if (m < 1e-9f) { qx = qy = qz = 0.0f; qw = 1.0f; } else { qx/=m; qy/=m; qz/=m; qw/=m; }
    o.q[0] = qx; o.q[1] = qy; o.q[2] = qz; o.q[3] = qw;
    o.s[0] = s[0]; o.s[1] = s[2]; o.s[2] = s[1];
    return o;
}

// ── Blender-orientation yaw (glTF-space rotY −90°): (x,y,z) → (−z,y,x). ──
// D4 characters face D4 +X with LEFT = D4 +Y (verified via HP_chestFront / HP_leftHand
// hardpoints in barM_base00). After the standard Z-up→Y-up swap the model imports into
// Blender facing +X with left = Blender +Y, which breaks Blender's X-mirror. This extra
// proper rotation lands it in Blender convention: facing −Y, character's left = +X.
inline void blenderVec(float& x, float& y, float& z)
{
    const float nx = -z, nz = x;
    x = nx; z = nz; (void)y;
}

// Compose the yaw with a glTF-space quaternion: q' = r ⊗ q, r = (0, −√½, 0, √½).
inline void blenderQuat(float q[4])
{
    const float s = 0.70710678f, c = 0.70710678f;
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    q[0] = c*x - s*z;
    q[1] = c*y - s*w;
    q[2] = c*z + s*x;
    q[3] = c*w + s*y;
}

// Apply the yaw to a whole SwappedTRS (ROOT-level bones only — children inherit).
// R·(T·Rq·S) = (R·t)·(r⊗q)·S — exact for any scale, since S applies before the rotations.
inline void blenderTRS(SwappedTRS& o)
{
    blenderVec(o.t[0], o.t[1], o.t[2]);
    blenderQuat(o.q);
}

// Right-multiply a column-major MAT4 by R⁻¹ (inverse bind matrices: IBM' = IBM·R⁻¹ so that
// IBM'·(R·world) == IBM·world). Column permutation: [c0,c1,c2,c3] → [−c2, c1, c0, c3].
inline void blenderIBM(float m[16])
{
    float c0[4] = {m[0], m[1], m[2], m[3]};
    float c2[4] = {m[8], m[9], m[10], m[11]};
    for (int r = 0; r < 4; ++r) { m[r] = -c2[r]; m[8 + r] = c0[r]; }
}

// Left-multiply a column-major MAT4 by R (baked root-bone local matrices: M' = R·M).
// Row permutation per column: row0' = −row2, row2' = row0.
inline void blenderMatL(float m[16])
{
    for (int col = 0; col < 4; ++col) {
        const float r0 = m[col*4 + 0], r2 = m[col*4 + 2];
        m[col*4 + 0] = -r2;
        m[col*4 + 2] = r0;
    }
}

// ── X-mirror rig symmetrization (verified in Blender 4.2.9 — see D4_XMirror_Spec.md) ──
// Blender's Pose ▸ X-Axis Mirror assumes .L/.R bones' REST orientations are mirror images
// across X=0. D4 bones aren't (measured ~180° apart), so we rewrite each paired .R bone's
// world rest ROTATION to Mx·R(.L)·Mx (translation untouched), rebuild every local against
// the new hierarchy, and recompute inverse binds as inv(newWorld) — which keeps the skinned
// bind pose mathematically identical. Anim curves are conjugated per bone by the constant
// C = R⁻¹·R' (see the anim channel writer).

inline void qmulf(const float a[4], const float b[4], float o[4])   // (x,y,z,w)
{
    o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}

inline void qrotv(const float q[4], float v[3])                     // v ← q·v·q*
{
    const float u[4] = {v[0], v[1], v[2], 0.0f};
    const float qc[4] = {-q[0], -q[1], -q[2], q[3]};
    float t[4], r[4];
    qmulf(q, u, t);
    qmulf(t, qc, r);
    v[0] = r[0]; v[1] = r[1]; v[2] = r[2];
}

struct SymBone {
    SwappedTRS trs;                 // glTF-space local rest (yaw + mirror applied)
    // C = R⁻¹·R' — the constant local re-orientation of this bone (identity if untouched).
    float cq[4]     = {0,0,0,1};    // C as quaternion (anim rotation fixup, right factor)
    float cqInv[4]  = {0,0,0,1};    // C⁻¹ as quaternion (children's channel fixup, left factor)
    float cInvM[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};  // C⁻¹ as column-major MAT4
};

// out = a·b for column-major MAT4s.
inline void mat4mulCM(const float a[16], const float b[16], float out[16])
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) v += a[k*4 + r] * b[c*4 + k];
            out[c*4 + r] = v;
        }
}

QVector<SymBone> symmetrizeSkeleton(const QVector<ModelJoint>& skel)
{
    const int nb = skel.size();
    // 1. Current glTF-space locals (swapTRS + yaw at roots) and world rests.
    QVector<SwappedTRS> loc(nb);
    QVector<QMatrix4x4> W(nb);
    for (int i = 0; i < nb; ++i) {
        const ModelJoint& b = skel[i];
        SwappedTRS r = swapTRS(b.restQ.data(), b.restT.data(), b.restS.data());
        const bool isRoot = !(b.parent >= 0 && b.parent < nb);
        if (isRoot) blenderTRS(r);
        loc[i] = r;
        QMatrix4x4 L;
        L.translate(r.t[0], r.t[1], r.t[2]);
        L.rotate(QQuaternion(r.q[3], r.q[0], r.q[1], r.q[2]));
        L.scale(r.s[0], r.s[1], r.s[2]);
        W[i] = isRoot ? L : W[b.parent] * L;
    }
    auto rotOf = [](const QMatrix4x4& M) -> QQuaternion {
        QVector3D c0 = M.column(0).toVector3D().normalized();
        QVector3D c1 = M.column(1).toVector3D().normalized();
        QVector3D c2 = M.column(2).toVector3D().normalized();
        const float rot[9] = {c0.x(), c1.x(), c2.x(),
                              c0.y(), c1.y(), c2.y(),
                              c0.z(), c1.z(), c2.z()};
        return QQuaternion::fromRotationMatrix(QMatrix3x3(rot)).normalized();
    };

    // 2. Mirror .R members of every pair: world rotation ← Mx·R(.L)·Mx, translation kept.
    const QVector<int> partner = Retarget::mirrorPairs(skel);
    QVector<QMatrix4x4> Wn = W;
    QVector<QQuaternion> C(nb, QQuaternion());
    for (int i = 0; i < nb; ++i) {
        const int j = partner[i];
        if (j < 0) continue;
        if (Wn[i].column(3).x() >= 0.0f) continue;          // transform the RIGHT side only (x<0)
        const QQuaternion qL = rotOf(W[j]);
        // Mx·R·Mx in quaternion form: (x,y,z,w) → (x,−y,−z,w)
        const QQuaternion qR(qL.scalar(), qL.x(), -qL.y(), -qL.z());
        const QVector3D t = W[i].column(3).toVector3D();
        const QVector3D sc(W[i].column(0).toVector3D().length(),
                           W[i].column(1).toVector3D().length(),
                           W[i].column(2).toVector3D().length());
        QMatrix4x4 M;
        M.translate(t);
        M.rotate(qR);
        M.scale(sc);
        Wn[i] = M;
        C[i] = rotOf(W[i]).conjugated() * qR;               // C = R⁻¹·R'
    }

    // 3. Rebuild ALL locals against the new hierarchy; inverse binds = inv(new world).
    QVector<SymBone> out(nb);
    for (int i = 0; i < nb; ++i) {
        const int p = skel[i].parent;
        const QMatrix4x4 L = (p >= 0 && p < nb) ? (Wn[p].inverted() * Wn[i]) : Wn[i];
        const QVector3D t = L.column(3).toVector3D();
        const float sx = L.column(0).toVector3D().length();
        const float sy = L.column(1).toVector3D().length();
        const float sz = L.column(2).toVector3D().length();
        const QQuaternion q = rotOf(L);
        SymBone& sb = out[i];
        sb.trs.t[0] = t.x();      sb.trs.t[1] = t.y();      sb.trs.t[2] = t.z();
        sb.trs.q[0] = q.x();      sb.trs.q[1] = q.y();      sb.trs.q[2] = q.z();      sb.trs.q[3] = q.scalar();
        sb.trs.s[0] = sx;         sb.trs.s[1] = sy;         sb.trs.s[2] = sz;
        const QQuaternion c = C[i], ci = C[i].conjugated();
        sb.cq[0]=c.x();  sb.cq[1]=c.y();  sb.cq[2]=c.z();  sb.cq[3]=c.scalar();
        sb.cqInv[0]=ci.x(); sb.cqInv[1]=ci.y(); sb.cqInv[2]=ci.z(); sb.cqInv[3]=ci.scalar();
        // C⁻¹ as MAT4: the inverse-bind fix is IBM' = C⁻¹·IBM — exact for whatever inverse
        // bind the payload authored (does NOT assume IBM == inv(rest world)), because
        // W' = W·C ⇒ W'·(C⁻¹·IBM) == W·IBM at every pose.
        QMatrix4x4 cm;
        cm.rotate(ci);
        std::memcpy(sb.cInvM, cm.constData(), 64);          // QMatrix4x4 is column-major
    }
    return out;
}

// Compose a column-major MAT4 from a SwappedTRS (baked-matrix bone path).
inline void trsToMat(const SwappedTRS& r, float m[16])
{
    QMatrix4x4 M;
    M.translate(r.t[0], r.t[1], r.t[2]);
    M.rotate(QQuaternion(r.q[3], r.q[0], r.q[1], r.q[2]));
    M.scale(r.s[0], r.s[1], r.s[2]);
    std::memcpy(m, M.constData(), 64);
}

void appendU32LE(QByteArray& b, quint32 v)
{
    const char d[4] = {char(v & 0xFF), char((v >> 8) & 0xFF),
                       char((v >> 16) & 0xFF), char((v >> 24) & 0xFF)};
    b.append(d, 4);
}

void appendU16LE(QByteArray& b, quint16 v)
{
    const char d[2] = {char(v & 0xFF), char((v >> 8) & 0xFF)};
    b.append(d, 2);
}

void pad4(QByteArray& b, char pad)
{
    while (b.size() % 4)
        b.append(pad);
}

// glTF componentType / bufferView target constants.
constexpr int FLOAT = 5126, UINT = 5125, USHORT = 5123;
constexpr int ARRAY_BUFFER = 34962, ELEMENT_ARRAY_BUFFER = 34963;

}  // namespace

bool ModelExporter::exportGlb(const ModelGeometry& geo, const QString& path,
                              const QVector<ExportMaterial>& materialDefs,
                              const QVector<AnimParser::DecodedAnim>& anims,
                              const QStringList& animNames,
                              const Options& opt)
{
    // Normal export needs geometry; an "animation library" export (no mesh) needs a
    // skeleton + at least one clip instead.
    const bool animLibrary = geo.primitives.isEmpty() && !geo.skeleton.isEmpty() && !anims.isEmpty();
    if (!geo.valid || (geo.primitives.isEmpty() && !animLibrary))
        return false;

    const bool  reconstructNormalZ = opt.reconstructNormalZ;
    const bool  blenderFriendly    = opt.blenderFriendly;
    const float uscale             = opt.unitScale;
    // Uniform world rescale: local/IBM/anim TRANSLATIONS and vertex positions scale;
    // rotations, scales and normals are invariant under conjugation by a uniform scale.
    const bool  doScale            = std::fabs(uscale - 1.0f) > 1e-6f;
    // X-mirror rig symmetrization (Blender-verified; only meaningful with the yaw applied,
    // since Blender's mirror plane X=0 IS the sagittal plane only after blenderFriendly).
    const bool xmirror = blenderFriendly && opt.xMirror && !geo.skeleton.isEmpty();
    QVector<SymBone> sym;
    if (xmirror) sym = symmetrizeSkeleton(geo.skeleton);

    // Skinning is active when there's a skeleton and the vertices carry weights.
    bool hasWeights = false;
    for (const MeshPrimitive& p : geo.primitives) {
        for (const MeshVertex& v : p.vertices)
            if (v.weights[0] || v.weights[1] || v.weights[2] || v.weights[3]) { hasWeights = true; break; }
        if (hasWeights) break;
    }
    const bool skinned = hasWeights && !geo.skeleton.isEmpty();
    // Emit the bone/skin/animation block for skinned meshes OR an armature-only clip library.
    const bool emitSkeleton = skinned || animLibrary;

    QByteArray bin;
    QJsonArray bufferViews, accessors, meshes, materials, nodes, sceneNodes;
    QJsonArray images, textures, samplers;
    QHash<QString, int> matIndex;       // dedupe key → glTF material index

    // Embed a QImage as a PNG image/texture, returning the glTF texture index.
    auto addTexture = [&](const QImage& img) -> int {
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        if (!img.save(&buf, "PNG"))
            return -1;
        while (bin.size() % 4) bin.append('\0');
        const int off = bin.size();
        bin.append(png);
        QJsonObject bv{{"buffer", 0}, {"byteOffset", off}, {"byteLength", png.size()}};
        const int bvIdx = int(bufferViews.size());
        bufferViews.append(bv);
        QJsonObject image{{"bufferView", bvIdx}, {"mimeType", "image/png"}};
        const int imgIdx = int(images.size());
        images.append(image);
        if (samplers.isEmpty())
            samplers.append(QJsonObject{{"wrapS", 10497}, {"wrapT", 10497},
                                        {"magFilter", 9729}, {"minFilter", 9987}});
        const int texIdx = int(textures.size());
        textures.append(QJsonObject{{"source", imgIdx}, {"sampler", 0}});
        return texIdx;
    };

    // Build one glTF material per primitive, enriched from materialDefs when the
    // primitive's materialIndex resolves to a supplied ExportMaterial.
    auto materialFor = [&](const MeshPrimitive& p) -> int {
        const bool haveDef = p.materialIndex >= 0 && p.materialIndex < materialDefs.size();
        const QString key = haveDef ? QStringLiteral("idx:%1").arg(p.materialIndex)
                                    : p.materialName;
        if (key.isEmpty())
            return -1;
        auto it = matIndex.constFind(key);
        if (it != matIndex.constEnd())
            return it.value();

        QJsonObject pbr;
        QJsonObject m;
        if (haveDef) {
            const ExportMaterial& d = materialDefs[p.materialIndex];
            m["name"] = d.name.isEmpty() ? p.materialName : d.name;
            pbr["metallicFactor"]  = d.hasMetal ? double(d.metal) : 0.0;
            pbr["roughnessFactor"] = d.hasRough ? double(d.rough) : 1.0;
            if (!d.baseColor.isNull()) {
                const int tex = addTexture(d.baseColor);
                if (tex >= 0)
                    pbr["baseColorTexture"] = QJsonObject{{"index", tex}};
            }
            if (!d.orm.isNull()) {
                const int tex = addTexture(d.orm);
                if (tex >= 0) {
                    // ORM packs R=occlusion, G=roughness, B=metallic; the same
                    // texture feeds both metallicRoughness (G,B) and occlusion (R).
                    pbr["metallicRoughnessTexture"] = QJsonObject{{"index", tex}};
                    pbr["metallicFactor"] = 1.0;
                    pbr["roughnessFactor"] = 1.0;
                    m["occlusionTexture"] = QJsonObject{{"index", tex}};
                }
            }
            if (!d.normal.isNull()) {
                QImage nrm = d.normal;
                if (reconstructNormalZ) {
                    // Reconstruct the tangent normal's Z into BLUE (B = √(1−x²−y²)). D4's BC5 normals
                    // decode with B≈0 and the shader only reads XY (rebuilding Z itself) — but Blender's
                    // normal-map node needs a valid blue channel or the surface lights wrong. Off = the
                    // normal exactly as decoded from the game.
                    nrm = nrm.convertToFormat(QImage::Format_RGBA8888);
                    for (int y = 0; y < nrm.height(); ++y) {
                        uchar* s = nrm.scanLine(y);
                        for (int x = 0; x < nrm.width(); ++x) {
                            const float nx = s[x*4]/255.0f*2.0f - 1.0f, ny = s[x*4+1]/255.0f*2.0f - 1.0f;
                            const float nz = std::sqrt(std::max(0.0f, 1.0f - nx*nx - ny*ny));
                            s[x*4+2] = uchar((nz*0.5f + 0.5f) * 255.0f);
                            s[x*4+3] = 255;
                        }
                    }
                }
                if (opt.flipNormalGreen) {
                    // OpenGL → DirectX normal convention (Unreal/Skyrim): invert the G channel.
                    nrm = nrm.convertToFormat(QImage::Format_RGBA8888);
                    for (int y = 0; y < nrm.height(); ++y) {
                        uchar* s = nrm.scanLine(y);
                        for (int x = 0; x < nrm.width(); ++x)
                            s[x*4+1] = uchar(255 - s[x*4+1]);
                    }
                }
                const int tex = addTexture(nrm);
                if (tex >= 0)
                    m["normalTexture"] = QJsonObject{{"index", tex}};
            }
            if (d.doubleSided)
                m["doubleSided"] = true;
            // Alpha-cutout (hair / cut-out cloth): without alphaMode, glTF is OPAQUE and ignores the
            // base-colour alpha, so Blender shows solid quads. MASK + cutoff shows the real cutout.
            if (d.alphaCutout) {
                m["alphaMode"] = QStringLiteral("MASK");
                m["alphaCutoff"] = double(d.alphaCutoff);
            }
            // Emissive is ONLY written when there's an emissive texture to mask it. D4 emissive
            // is gated by this map; without it, a flat emissiveFactor makes the WHOLE surface
            // glow (blown-out white in Blender). emission = emissiveTexture × emissiveFactor ×
            // emissiveStrength. glTF's default emissiveFactor is black (kills emission), so we
            // always set it: the authored colour when it's meaningful, else white so the texture
            // drives the colour. The HDR multiplier goes in KHR_materials_emissive_strength.
            if (!d.emissive.isNull()) {
                const int tex = addTexture(d.emissive);
                if (tex >= 0) {
                    m["emissiveTexture"] = QJsonObject{{"index", tex}};
                    const bool haveColour = d.hasEmissive && (d.emisR > 0.0f || d.emisG > 0.0f || d.emisB > 0.0f);
                    m["emissiveFactor"] = haveColour
                        ? QJsonArray{qBound(0.0, double(d.emisR), 1.0), qBound(0.0, double(d.emisG), 1.0),
                                     qBound(0.0, double(d.emisB), 1.0)}
                        : QJsonArray{1.0, 1.0, 1.0};
                    if (d.hasEmissive && d.emisMult > 1.0f)
                        m["extensions"] = QJsonObject{{"KHR_materials_emissive_strength",
                                          QJsonObject{{"emissiveStrength", double(d.emisMult)}}}};
                }
            }
        } else {
            m["name"] = p.materialName;
            pbr["metallicFactor"] = 0.0;
            pbr["roughnessFactor"] = 1.0;
        }
        m["pbrMetallicRoughness"] = pbr;
        const int idx = int(materials.size());
        materials.append(m);
        matIndex.insert(key, idx);
        return idx;
    };

    auto addBufferView = [&](int byteOffset, int byteLength, int target) -> int {
        QJsonObject bv{{"buffer", 0}, {"byteOffset", byteOffset}, {"byteLength", byteLength}};
        if (target)
            bv["target"] = target;
        const int i = int(bufferViews.size());
        bufferViews.append(bv);
        return i;
    };

    for (const MeshPrimitive& p : geo.primitives) {
        const int vcount = int(p.vertices.size());
        if (vcount == 0 || p.indices.isEmpty())
            continue;

        // POSITION (with required min/max)
        pad4(bin, '\0');
        const int posOff = bin.size();
        float mn[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max()};
        float mx[3] = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                       -std::numeric_limits<float>::max()};
        for (const MeshVertex& v : p.vertices) {
            float pos[3] = {v.px, v.py, v.pz};
            if (blenderFriendly) blenderVec(pos[0], pos[1], pos[2]);
            if (doScale) { pos[0] *= uscale; pos[1] *= uscale; pos[2] *= uscale; }
            for (int k = 0; k < 3; ++k) { mn[k] = qMin(mn[k], pos[k]); mx[k] = qMax(mx[k], pos[k]); }
            bin.append(reinterpret_cast<const char*>(pos), 12);
        }
        const int posBV = addBufferView(posOff, vcount * 12, ARRAY_BUFFER);
        const int posAcc = int(accessors.size());
        accessors.append(QJsonObject{
            {"bufferView", posBV}, {"componentType", FLOAT}, {"count", vcount}, {"type", "VEC3"},
            {"min", QJsonArray{mn[0], mn[1], mn[2]}}, {"max", QJsonArray{mx[0], mx[1], mx[2]}}});

        // NORMAL
        pad4(bin, '\0');
        const int nOff = bin.size();
        for (const MeshVertex& v : p.vertices) {
            float n[3] = {v.nx, v.ny, v.nz};
            if (blenderFriendly) blenderVec(n[0], n[1], n[2]);
            bin.append(reinterpret_cast<const char*>(n), 12);
        }
        const int nBV = addBufferView(nOff, vcount * 12, ARRAY_BUFFER);
        const int nAcc = int(accessors.size());
        accessors.append(QJsonObject{{"bufferView", nBV}, {"componentType", FLOAT},
                                     {"count", vcount}, {"type", "VEC3"}});

        // TEXCOORD_0
        pad4(bin, '\0');
        const int tOff = bin.size();
        for (const MeshVertex& v : p.vertices) {
            const float t[2] = {v.u, v.v};
            bin.append(reinterpret_cast<const char*>(t), 8);
        }
        const int tBV = addBufferView(tOff, vcount * 8, ARRAY_BUFFER);
        const int tAcc = int(accessors.size());
        accessors.append(QJsonObject{{"bufferView", tBV}, {"componentType", FLOAT},
                                     {"count", vcount}, {"type", "VEC2"}});

        QJsonObject attribs{{"POSITION", posAcc}, {"NORMAL", nAcc}, {"TEXCOORD_0", tAcc}};

        if (skinned) {
            // JOINTS_0 (u16 vec4) — slots with zero weight are zeroed so the
            // glTF validator doesn't flag "joint used with zero weight".
            pad4(bin, '\0');
            const int jOff = bin.size();
            for (const MeshVertex& v : p.vertices)
                for (int k = 0; k < 4; ++k)
                    appendU16LE(bin, v.weights[k] != 0.0f ? v.joints[k] : quint16(0));
            const int jBV = addBufferView(jOff, vcount * 8, ARRAY_BUFFER);
            const int jAcc = int(accessors.size());
            accessors.append(QJsonObject{{"bufferView", jBV}, {"componentType", USHORT},
                                         {"count", vcount}, {"type", "VEC4"}});
            attribs["JOINTS_0"] = jAcc;

            // WEIGHTS_0 (f32 vec4)
            pad4(bin, '\0');
            const int wOff = bin.size();
            for (const MeshVertex& v : p.vertices)
                bin.append(reinterpret_cast<const char*>(v.weights), 16);
            const int wBV = addBufferView(wOff, vcount * 16, ARRAY_BUFFER);
            const int wAcc = int(accessors.size());
            accessors.append(QJsonObject{{"bufferView", wBV}, {"componentType", FLOAT},
                                         {"count", vcount}, {"type", "VEC4"}});
            attribs["WEIGHTS_0"] = wAcc;
        }

        // indices (unsigned int)
        pad4(bin, '\0');
        const int iOff = bin.size();
        for (quint32 idx : p.indices)
            appendU32LE(bin, idx);
        const int iBV = addBufferView(iOff, int(p.indices.size()) * 4, ELEMENT_ARRAY_BUFFER);
        const int iAcc = int(accessors.size());
        accessors.append(QJsonObject{{"bufferView", iBV}, {"componentType", UINT},
                                     {"count", int(p.indices.size())}, {"type", "SCALAR"}});

        QJsonObject prim{{"attributes", attribs}, {"indices", iAcc}, {"mode", 4}};
        const int mi = materialFor(p);
        if (mi >= 0)
            prim["material"] = mi;
        if (p.doubleSided && mi >= 0) {
            QJsonObject m = materials[mi].toObject();
            m["doubleSided"] = true;
            materials[mi] = m;
        }

        const int meshIdx = int(meshes.size());
        meshes.append(QJsonObject{{"primitives", QJsonArray{prim}}});
        QJsonObject node{{"mesh", meshIdx}};
        if (skinned)
            node["skin"] = 0;
        const int nodeIdx = int(nodes.size());
        nodes.append(node);
        sceneNodes.append(nodeIdx);
    }

    if (nodes.isEmpty() && geo.skeleton.isEmpty())
        return false;

    // glTF requires any used extension to be declared in extensionsUsed.
    bool usedEmisStrength = false;
    for (const QJsonValue& mv : materials)
        if (mv.toObject().contains("extensions")) { usedEmisStrength = true; break; }
    const QJsonArray extensionsUsed{QStringLiteral("KHR_materials_emissive_strength")};

    // ── Skeleton: bone nodes, skin, inverse bind matrices ──
    if (emitSkeleton) {
        const int boneBase = int(nodes.size());   // first bone node index
        const int boneCount = geo.skeleton.size();
        // Animated nodes MUST use TRS (glTF forbids a baked matrix on an animation target),
        // so when exporting clips we write each bone's rest TRS (with the axis swap) instead
        // of localMatrix. localMatrix == composeTRS(rest), so the bind pose is identical.
        const bool animated = !anims.isEmpty();

        // Bone nodes, built first so children can reference them by index.
        QVector<QJsonArray> children(boneCount);
        for (int i = 0; i < boneCount; ++i) {
            const ModelJoint& b = geo.skeleton[i];
            const bool isRoot = !(b.parent >= 0 && b.parent < boneCount);
            QJsonObject bn{{"name", b.name}};
            if (animated) {
                SwappedTRS r;
                if (xmirror) {
                    r = sym[i].trs;                             // yaw + mirror already baked in
                } else {
                    r = swapTRS(b.restQ.data(), b.restT.data(), b.restS.data());
                    if (blenderFriendly && isRoot) blenderTRS(r);   // children inherit the yaw
                }
                if (doScale) { r.t[0] *= uscale; r.t[1] *= uscale; r.t[2] *= uscale; }
                bn["translation"] = QJsonArray{r.t[0], r.t[1], r.t[2]};
                bn["rotation"]    = QJsonArray{r.q[0], r.q[1], r.q[2], r.q[3]};
                bn["scale"]       = QJsonArray{r.s[0], r.s[1], r.s[2]};
            } else {
                float lm[16];
                if (xmirror) {
                    trsToMat(sym[i].trs, lm);
                } else {
                    for (int k = 0; k < 16; ++k) lm[k] = b.localMatrix[k];
                    if (blenderFriendly && isRoot) blenderMatL(lm);
                }
                if (doScale) { lm[12] *= uscale; lm[13] *= uscale; lm[14] *= uscale; }
                QJsonArray mat;
                for (int k = 0; k < 16; ++k) mat.append(lm[k]);
                bn["matrix"] = mat;
            }
            nodes.append(bn);
        }
        QJsonArray rootBones;
        for (int i = 0; i < boneCount; ++i) {
            const int p = geo.skeleton[i].parent;
            if (p >= 0 && p < boneCount) children[p].append(boneBase + i);
            else                         rootBones.append(boneBase + i);
        }
        // Hardpoint empties: one child node per hardpoint under its parent bone. Local
        // transform = swapTRS(hp bone-local) with the SAME X-mirror conjugation the bone got
        // (C⁻¹, so the socket stays fixed to the model) and the unit scale on its translation.
        // Verified in Blender: bind-pose world identical with symmetrize off/on.
        const float kUnitS[3] = {1.0f, 1.0f, 1.0f};
        // Hardpoint-transform semantics (verified against d4data mount rigs): an AUTHORED
        // (non-identity) transform is the socket's rest placement in MODEL space — nBoneIndex is
        // only the bone it FOLLOWS — so the correct child-of-bone LOCAL is inverseBind · socket
        // (boneWorld · that == socket at bind). An IDENTITY transform means "the bone itself".
        // (The X-mirror rig-rewrite path keeps the legacy bone-local emit; empties there are a
        // niche combo and the bone-local approximation is close for identity-heavy player rigs.)
        auto hpAuthored = [](const ModelHardpoint& hp) {
            return std::fabs(hp.t[0]) > 1e-4f || std::fabs(hp.t[1]) > 1e-4f || std::fabs(hp.t[2]) > 1e-4f
                || std::fabs(hp.q[0]) > 1e-3f || std::fabs(hp.q[1]) > 1e-3f || std::fabs(hp.q[2]) > 1e-3f;
        };
        for (const ModelHardpoint& hp : geo.hardpoints) {
            if (hp.boneIndex < 0 || hp.boneIndex >= boneCount) continue;
            if (hpAuthored(hp) && !xmirror) {
                // Model-space socket → bone-local matrix = inverseBind · socketMatrix (both y-up,
                // column-major). Emit as a matrix node (empties are never animation targets).
                SwappedTRS sh = swapTRS(hp.q.data(), hp.t.data(), kUnitS);   // socket in y-up (model space)
                float sm[16]; trsToMat(sh, sm);
                const auto& ib = geo.skeleton[hp.boneIndex].inverseBind;      // y-up, column-major
                const QMatrix4x4 IB(ib[0],ib[4],ib[8],ib[12], ib[1],ib[5],ib[9],ib[13],
                                    ib[2],ib[6],ib[10],ib[14], ib[3],ib[7],ib[11],ib[15]);
                const QMatrix4x4 SM(sm[0],sm[4],sm[8],sm[12], sm[1],sm[5],sm[9],sm[13],
                                    sm[2],sm[6],sm[10],sm[14], sm[3],sm[7],sm[11],sm[15]);
                QMatrix4x4 loc = IB * SM;
                if (doScale) { loc(0,3) *= uscale; loc(1,3) *= uscale; loc(2,3) *= uscale; }
                QJsonObject hn{{"name", hp.name}};
                QJsonArray mat; const float* d = loc.constData();
                for (int k = 0; k < 16; ++k) mat.append(d[k]);
                hn["matrix"] = mat;
                const int hnIdx = int(nodes.size());
                nodes.append(hn);
                children[hp.boneIndex].append(hnIdx);
                continue;
            }
            SwappedTRS r = swapTRS(hp.q.data(), hp.t.data(), kUnitS);
            if (xmirror) {
                const float* ci = sym[hp.boneIndex].cqInv;      // C⁻¹ (identity for untouched bones)
                float nq[4];
                qmulf(ci, r.q, nq);
                r.q[0]=nq[0]; r.q[1]=nq[1]; r.q[2]=nq[2]; r.q[3]=nq[3];
                qrotv(ci, r.t);
            }
            if (doScale) { r.t[0] *= uscale; r.t[1] *= uscale; r.t[2] *= uscale; }
            QJsonObject hn{{"name", hp.name}};
            hn["translation"] = QJsonArray{r.t[0], r.t[1], r.t[2]};
            hn["rotation"]    = QJsonArray{r.q[0], r.q[1], r.q[2], r.q[3]};
            const int hnIdx = int(nodes.size());
            nodes.append(hn);
            children[hp.boneIndex].append(hnIdx);
        }
        for (int i = 0; i < boneCount; ++i) {
            if (!children[i].isEmpty()) {
                QJsonObject n = nodes[boneBase + i].toObject();
                n["children"] = children[i];
                nodes[boneBase + i] = n;
            }
        }
        // sceneNodes already holds the mesh nodes; add only the root bones
        // (child bones are reached through the node hierarchy).
        for (const QJsonValue& r : rootBones)
            sceneNodes.append(r);

        // inverseBindMatrices accessor (one MAT4 per bone, column-major).
        pad4(bin, '\0');
        const int ibmOff = bin.size();
        for (int bi = 0; bi < boneCount; ++bi) {
            const ModelJoint& b = geo.skeleton[bi];
            if (xmirror) {
                // IBM' = C⁻¹ · blenderIBM(payload IBM) — keeps W'·IBM' == W·IBM, i.e. the
                // skinned bind pose is mathematically identical (Blender-measured 2.4e-7).
                float base[16], ibm[16];
                std::memcpy(base, b.inverseBind.data(), 64);
                blenderIBM(base);                            // yaw part of W is in Wn already
                mat4mulCM(sym[bi].cInvM, base, ibm);
                if (doScale) { ibm[12] *= uscale; ibm[13] *= uscale; ibm[14] *= uscale; }
                bin.append(reinterpret_cast<const char*>(ibm), 64);
            } else if (blenderFriendly || doScale) {
                float ibm[16];
                std::memcpy(ibm, b.inverseBind.data(), 64);
                if (blenderFriendly) blenderIBM(ibm);
                if (doScale) { ibm[12] *= uscale; ibm[13] *= uscale; ibm[14] *= uscale; }
                bin.append(reinterpret_cast<const char*>(ibm), 64);
            } else {
                bin.append(reinterpret_cast<const char*>(b.inverseBind.data()), 64);
            }
        }
        const int ibmBV = addBufferView(ibmOff, boneCount * 64, 0);
        const int ibmAcc = int(accessors.size());
        accessors.append(QJsonObject{{"bufferView", ibmBV}, {"componentType", FLOAT},
                                     {"count", boneCount}, {"type", "MAT4"}});

        QJsonArray joints;
        for (int i = 0; i < boneCount; ++i) joints.append(boneBase + i);
        QJsonObject skin{{"joints", joints}, {"inverseBindMatrices", ibmAcc}};
        // skins[0] referenced by every skinned mesh node (node["skin"] = 0).
        QJsonArray skins{skin};

        // ── Animations: one glTF animation per decoded clip. Each animated bone gets up to
        // three channels (rotation / translation / scale) sampled once per frame at 1/fps. ──
        QJsonArray animationsJson;
        if (animated) {
            QHash<quint32, int> hashToBone;   // bone-name hash → skeleton index (== node - boneBase)
            for (int i = 0; i < boneCount; ++i) hashToBone.insert(geo.skeleton[i].nameHash, i);

            // Append a float array as an accessor (no bufferView target). Returns the accessor index.
            auto addFloatAcc = [&](const QVector<float>& data, int comps, const char* type) -> int {
                pad4(bin, '\0');
                const int off = bin.size();
                for (float fv : data) bin.append(reinterpret_cast<const char*>(&fv), 4);
                const int bv = addBufferView(off, data.size() * 4, 0);
                const int acc = int(accessors.size());
                accessors.append(QJsonObject{{"bufferView", bv}, {"componentType", FLOAT},
                                             {"count", data.size() / comps}, {"type", type}});
                return acc;
            };
            static const float kZ3[3] = {0, 0, 0}, kO3[3] = {1, 1, 1}, kIQ[4] = {0, 0, 0, 1};

            for (int ci = 0; ci < anims.size(); ++ci) {
                const AnimParser::DecodedAnim& an = anims[ci];
                if (!an.valid || an.bones.isEmpty()) continue;
                const float fps = an.frameRate > 1.0f ? an.frameRate : 30.0f;
                QJsonArray samplers, channels;

                // Time (input) accessor for `count` frames, with the required min/max.
                //
                // CACHED PER CLIP. This used to write a fresh timestamp array on every call, and
                // it is called once per CHANNEL — up to three times per bone. On barM's 318-bone
                // rig that is 954 byte-identical float arrays per clip, plus 954 accessors and 954
                // bufferViews of JSON describing them, when every channel of a given key-count can
                // share one. The timestamps depend only on (count, fps), both constant within a
                // clip, so sharing is exact rather than an approximation: glTF explicitly permits
                // several samplers to reference the same input accessor.
                QHash<int, int> timeAccFor;   // key count → accessor index, this clip only
                auto addTimeAcc = [&](int count) -> int {
                    const auto hit = timeAccFor.constFind(count);
                    if (hit != timeAccFor.constEnd()) return hit.value();
                    QVector<float> times(count);
                    for (int i = 0; i < count; ++i) times[i] = float(i) / fps;
                    const int acc = addFloatAcc(times, 1, "SCALAR");
                    QJsonObject a = accessors[acc].toObject();
                    a["min"] = QJsonArray{0.0};
                    a["max"] = QJsonArray{count > 0 ? double(count - 1) / double(fps) : 0.0};
                    accessors[acc] = a;
                    timeAccFor.insert(count, acc);
                    return acc;
                };
                auto addChannel = [&](int node, const char* pathName, int inAcc, int outAcc) {
                    const int si = int(samplers.size());
                    samplers.append(QJsonObject{{"input", inAcc}, {"output", outAcc},
                                                {"interpolation", "LINEAR"}});
                    channels.append(QJsonObject{{"sampler", si},
                        {"target", QJsonObject{{"node", node}, {"path", pathName}}}});
                };

                for (const AnimParser::DecodedBone& ba : an.bones) {
                    const int bidx = hashToBone.value(ba.boneHash, -1);
                    if (bidx < 0) continue;
                    const int node = boneBase + bidx;
                    // Root-level bones carry the Blender yaw; their anim channels must too
                    // (child channels are parent-relative and stay untouched).
                    const int bp = geo.skeleton[bidx].parent;
                    const bool yaw = blenderFriendly && !(bp >= 0 && bp < boneCount);
                    // X-mirror symmetrization: curves are authored in the OLD local frames, so
                    // conjugate each key by the constant per-bone re-orientation (verified —
                    // identical world trajectories): q' = C_parent⁻¹ ⊗ q ⊗ C_bone, t' = C_parent⁻¹·t.
                    static const float kIdQ[4] = {0, 0, 0, 1};
                    const float* cpInv = (xmirror && bp >= 0 && bp < boneCount) ? sym[bp].cqInv : kIdQ;
                    const float* cBone = xmirror ? sym[bidx].cq : kIdQ;
                    if (!ba.rotations.isEmpty()) {
                        QVector<float> out; out.reserve(ba.rotations.size() * 4);
                        for (const auto& q : ba.rotations) {
                            SwappedTRS r = swapTRS(q.data(), kZ3, kO3);
                            if (yaw) blenderQuat(r.q);
                            if (xmirror) {
                                float tmp[4], res[4];
                                qmulf(cpInv, r.q, tmp);
                                qmulf(tmp, cBone, res);
                                r.q[0]=res[0]; r.q[1]=res[1]; r.q[2]=res[2]; r.q[3]=res[3];
                            }
                            out << r.q[0] << r.q[1] << r.q[2] << r.q[3];
                        }
                        addChannel(node, "rotation", addTimeAcc(ba.rotations.size()),
                                   addFloatAcc(out, 4, "VEC4"));
                    }
                    if (!ba.translations.isEmpty()) {
                        QVector<float> out; out.reserve(ba.translations.size() * 3);
                        for (const auto& t : ba.translations) {
                            SwappedTRS r = swapTRS(kIQ, t.data(), kO3);
                            if (yaw) blenderVec(r.t[0], r.t[1], r.t[2]);
                            if (xmirror) qrotv(cpInv, r.t);
                            if (doScale) { r.t[0] *= uscale; r.t[1] *= uscale; r.t[2] *= uscale; }
                            out << r.t[0] << r.t[1] << r.t[2];
                        }
                        addChannel(node, "translation", addTimeAcc(ba.translations.size()),
                                   addFloatAcc(out, 3, "VEC3"));
                    }
                    if (!ba.scales.isEmpty()) {
                        QVector<float> out; out.reserve(ba.scales.size() * 3);
                        for (const auto& sc : ba.scales) {
                            const SwappedTRS r = swapTRS(kIQ, kZ3, sc.data());
                            out << r.s[0] << r.s[1] << r.s[2];
                        }
                        addChannel(node, "scale", addTimeAcc(ba.scales.size()),
                                   addFloatAcc(out, 3, "VEC3"));
                    }
                }
                if (channels.isEmpty()) continue;
                QJsonObject animObj{{"samplers", samplers}, {"channels", channels}};
                const QString nm = ci < animNames.size() ? animNames[ci]
                                                         : QStringLiteral("anim%1").arg(ci);
                if (!nm.isEmpty()) animObj["name"] = nm;
                animationsJson.append(animObj);
            }
        }

        QJsonObject root;
        root["asset"] = QJsonObject{{"version", "2.0"}, {"generator", "D4AssetBrowser"}};
        root["buffers"] = QJsonArray{QJsonObject{{"byteLength", 0}}};  // patched below
        root["bufferViews"] = bufferViews;
        root["accessors"] = accessors;
        if (!meshes.isEmpty()) root["meshes"] = meshes;     // omitted for armature-only clip libraries
        if (!materials.isEmpty()) root["materials"] = materials;
        if (!images.isEmpty())   { root["images"] = images; root["textures"] = textures; root["samplers"] = samplers; }
        root["nodes"] = nodes;
        root["skins"] = skins;
        if (!animationsJson.isEmpty()) root["animations"] = animationsJson;
        root["scenes"] = QJsonArray{QJsonObject{{"nodes", sceneNodes}}};
        root["scene"] = 0;
        if (usedEmisStrength) root["extensionsUsed"] = extensionsUsed;
        root["buffers"] = QJsonArray{QJsonObject{{"byteLength", bin.size()}}};

        QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
        while (json.size() % 4) json.append(' ');
        while (bin.size() % 4)  bin.append('\0');

        QByteArray glb;
        appendU32LE(glb, 0x46546C67u);
        appendU32LE(glb, 2u);
        appendU32LE(glb, quint32(12 + 8 + json.size() + 8 + bin.size()));
        appendU32LE(glb, quint32(json.size()));
        appendU32LE(glb, 0x4E4F534Au);
        glb.append(json);
        appendU32LE(glb, quint32(bin.size()));
        appendU32LE(glb, 0x004E4942u);
        glb.append(bin);

        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return false;
        return f.write(glb) == glb.size();
    }

    // ── Static (rigid) export ──
    QJsonObject root;
    root["asset"] = QJsonObject{{"version", "2.0"}, {"generator", "D4AssetBrowser"}};
    root["bufferViews"] = bufferViews;
    root["accessors"] = accessors;
    root["meshes"] = meshes;
    if (!materials.isEmpty())
        root["materials"] = materials;
    if (!images.isEmpty()) { root["images"] = images; root["textures"] = textures; root["samplers"] = samplers; }
    root["nodes"] = nodes;
    root["scenes"] = QJsonArray{QJsonObject{{"nodes", sceneNodes}}};
    root["scene"] = 0;
    if (usedEmisStrength) root["extensionsUsed"] = extensionsUsed;
    root["buffers"] = QJsonArray{QJsonObject{{"byteLength", bin.size()}}};

    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    while (json.size() % 4) json.append(' ');     // JSON chunk padded with spaces
    while (bin.size() % 4)  bin.append('\0');      // BIN chunk padded with zeros

    QByteArray glb;
    appendU32LE(glb, 0x46546C67u);                              // magic "glTF"
    appendU32LE(glb, 2u);                                       // version
    appendU32LE(glb, quint32(12 + 8 + json.size() + 8 + bin.size()));  // total length
    appendU32LE(glb, quint32(json.size()));
    appendU32LE(glb, 0x4E4F534Au);                             // "JSON"
    glb.append(json);
    appendU32LE(glb, quint32(bin.size()));
    appendU32LE(glb, 0x004E4942u);                             // "BIN\0"
    glb.append(bin);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(glb) == glb.size();
}

// Legacy convenience overload — forwards into the Options-based export.
bool ModelExporter::exportGlb(const ModelGeometry& geo, const QString& path,
                              const QVector<ExportMaterial>& materialDefs,
                              const QVector<AnimParser::DecodedAnim>& anims,
                              const QStringList& animNames,
                              bool reconstructNormalZ,
                              bool blenderFriendly)
{
    Options opt;
    opt.reconstructNormalZ = reconstructNormalZ;
    opt.blenderFriendly    = blenderFriendly;
    return exportGlb(geo, path, materialDefs, anims, animNames, opt);
}

// Resolve export options from QSettings, applying the target-engine preset
// (Settings ▸ Export ▸ "Retarget & modding"). Preset 0 (Custom) respects the
// individual toggles + retarget/unitScale; the named presets override them.
ModelExporter::Options ModelExporter::optionsFromSettings()
{
    const QSettings s;
    Options opt;
    opt.reconstructNormalZ = s.value(QStringLiteral("export/reconstructNormalZ"), true).toBool();
    opt.blenderFriendly    = s.value(QStringLiteral("export/blenderFriendly"), false).toBool();
    opt.xMirror            = s.value(QStringLiteral("export/xMirror"), true).toBool();
    switch (s.value(QStringLiteral("retarget/enginePreset"), 0).toInt()) {
    case 1:   // Blender — meters, OpenGL normals, Blender-friendly rig
        opt.blenderFriendly = true;
        opt.unitScale       = 1.0f;
        opt.flipNormalGreen = false;
        break;
    case 2:   // Unreal / Skyrim — cm pipelines (Blender→FBX), DirectX normals
        opt.blenderFriendly = true;
        opt.unitScale       = 100.0f;
        opt.flipNormalGreen = true;
        break;
    case 3:   // Unity — plain glTF (Y-up, meters, OpenGL normals)
        opt.blenderFriendly = false;
        opt.unitScale       = 1.0f;
        opt.flipNormalGreen = false;
        break;
    default:  // Custom
        opt.unitScale = float(s.value(QStringLiteral("retarget/unitScale"), 1.0).toDouble());
        break;
    }
    return opt;
}
