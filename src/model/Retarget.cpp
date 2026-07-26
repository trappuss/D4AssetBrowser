#include "model/Retarget.h"

#include <QHash>
#include <QMatrix3x3>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QSettings>
#include <QVector3D>

#include <algorithm>
#include <cmath>

namespace {

// The 26 identified player-rig anchor bones (hash → export name). Same data as
// GLModelWidget::blenderizeSkeletonNames / d4_hash_tables.json "blenderBoneNames";
// derived from the game's own hardpoint + IK data (see D4_BoneHash_Research_Report.md).
struct Anchor { quint32 hash; const char* name; };
const Anchor kAnchors[] = {
    { 0xBF59F7AFu, "root"       }, { 0x98C6875Eu, "attach"     },
    { 0x20365219u, "center"     }, { 0xD2322EEBu, "chest"      },
    { 0xB0076FACu, "head"       }, { 0xD12DD5D1u, "mouth"      },
    { 0xF7A2423Bu, "uiAnimated" }, { 0x32FF39ADu, "pelvis"     },
    { 0xF51D6140u, "upperArm.R" }, { 0xF4B0CE3Au, "upperArm.L" },
    { 0x8E911E73u, "forearm.R"  }, { 0x8E248B6Du, "forearm.L"  },
    { 0xF54A1413u, "hand.R"     }, { 0xF4DD810Du, "hand.L"     },
    { 0x3536F3E9u, "weapon.R"   }, { 0x34CA60E3u, "weapon.L"   },
    { 0x50A78B5Bu, "handEnd.R"  }, { 0x503AF855u, "handEnd.L"  },
    { 0x1289E8B3u, "thigh.R"    }, { 0x121D55ADu, "thigh.L"    },
    { 0xB9CFD755u, "shin.R"     }, { 0xB963444Fu, "shin.L"     },
    { 0x9CAC595Du, "ankle.R"    }, { 0x9C3FC657u, "ankle.L"    },
    { 0x34EFB08Du, "foot.R"     }, { 0x34831D87u, "foot.L"     },
};

// Rewrite every vertex's JOINTS_0/WEIGHTS_0 through jointMap (old skeleton index →
// new skeleton index). Weights whose bones merged into the same target are summed;
// slots keep the 4 heaviest and are renormalized.
void rewriteWeights(ModelGeometry& geo, const QVector<int>& jointMap)
{
    for (MeshPrimitive& p : geo.primitives) {
        for (MeshVertex& v : p.vertices) {
            // Gather (target joint → summed weight).
            int   tj[4] = {0, 0, 0, 0};
            float tw[4] = {0, 0, 0, 0};
            int   n = 0;
            for (int k = 0; k < 4; ++k) {
                if (v.weights[k] <= 0.0f) continue;
                const int oj = v.joints[k];
                const int nj = (oj >= 0 && oj < jointMap.size()) ? jointMap[oj] : -1;
                if (nj < 0) continue;                     // bone dropped without a target
                int slot = -1;
                for (int s = 0; s < n; ++s) if (tj[s] == nj) { slot = s; break; }
                if (slot < 0 && n < 4) { slot = n++; tj[slot] = nj; tw[slot] = 0.0f; }
                if (slot >= 0) tw[slot] += v.weights[k];
            }
            // Sort descending by weight (n ≤ 4 — a couple of swaps is fine).
            for (int a = 0; a < n; ++a)
                for (int b = a + 1; b < n; ++b)
                    if (tw[b] > tw[a]) { std::swap(tw[a], tw[b]); std::swap(tj[a], tj[b]); }
            float sum = 0.0f;
            for (int s = 0; s < n; ++s) sum += tw[s];
            for (int k = 0; k < 4; ++k) {
                if (k < n && sum > 1e-8f) { v.joints[k] = quint16(tj[k]); v.weights[k] = tw[k] / sum; }
                else                      { v.joints[k] = 0;              v.weights[k] = 0.0f; }
            }
        }
    }
}

// Compose the D4-native (pre axis-swap) global rest matrix of every joint.
QVector<QMatrix4x4> globalRest(const QVector<ModelJoint>& skel)
{
    QVector<QMatrix4x4> global(skel.size());
    for (int i = 0; i < skel.size(); ++i) {
        const ModelJoint& j = skel[i];
        QMatrix4x4 local;
        local.translate(j.restT[0], j.restT[1], j.restT[2]);
        local.rotate(QQuaternion(j.restQ[3], j.restQ[0], j.restQ[1], j.restQ[2]));
        local.scale(j.restS[0], j.restS[1], j.restS[2]);
        const int p = j.parent;
        global[i] = (p >= 0 && p < i) ? global[p] * local : local;
    }
    return global;
}

// Decompose a rigid(ish) matrix into the joint's D4-native rest TRS and rebuild its
// glTF-space localMatrix (same Z-up→Y-up swap ModelParser applies: t (x,z,−y),
// q (x,z,−y,w), s (x,z,y)).
void setRestFromMatrix(ModelJoint& j, const QMatrix4x4& L)
{
    const QVector3D c0 = L.column(0).toVector3D();
    const QVector3D c1 = L.column(1).toVector3D();
    const QVector3D c2 = L.column(2).toVector3D();
    const QVector3D t  = L.column(3).toVector3D();
    const float sx = c0.length(), sy = c1.length(), sz = c2.length();
    const float rot[9] = {
        sx > 1e-9f ? c0.x()/sx : 1.0f, sy > 1e-9f ? c1.x()/sy : 0.0f, sz > 1e-9f ? c2.x()/sz : 0.0f,
        sx > 1e-9f ? c0.y()/sx : 0.0f, sy > 1e-9f ? c1.y()/sy : 1.0f, sz > 1e-9f ? c2.y()/sz : 0.0f,
        sx > 1e-9f ? c0.z()/sx : 0.0f, sy > 1e-9f ? c1.z()/sy : 0.0f, sz > 1e-9f ? c2.z()/sz : 1.0f,
    };
    const QQuaternion q = QQuaternion::fromRotationMatrix(QMatrix3x3(rot)).normalized();

    j.restT = {{t.x(), t.y(), t.z()}};
    j.restQ = {{q.x(), q.y(), q.z(), q.scalar()}};
    j.restS = {{sx, sy, sz}};

    // glTF-space local matrix (column-major), from the swapped TRS.
    QMatrix4x4 g;
    g.translate(t.x(), t.z(), -t.y());
    g.rotate(QQuaternion(q.scalar(), q.x(), q.z(), -q.y()).normalized());
    g.scale(sx, sz, sy);
    const float* d = g.constData();          // QMatrix4x4::constData() is column-major
    for (int k = 0; k < 16; ++k) j.localMatrix[k] = d[k];
}

}  // namespace

int Retarget::collapseClothChains(ModelGeometry& geo)
{
    const int nb = geo.skeleton.size();
    const int base = geo.nBaseBones;
    if (nb == 0 || base <= 0 || base >= nb)
        return 0;                                        // unknown boundary → safe no-op

    // Cloth bone → nearest kept (index < base) ancestor.
    QVector<int> jointMap(nb);
    for (int i = 0; i < nb; ++i) {
        int a = i;
        while (a >= base) {
            const int p = geo.skeleton[a].parent;
            if (p < 0 || p >= a) { a = 0; break; }       // broken chain → root
            a = p;
        }
        jointMap[i] = a;
    }
    rewriteWeights(geo, jointMap);
    geo.skeleton.resize(base);                           // cloth bones are appended after the base rig
    geo.clothCapsules.clear();                           // referenced the removed bones
    geo.clothSims.clear();
    return nb - base;
}

bool Retarget::remapToAnchors(ModelGeometry& geo)
{
    const int nb = geo.skeleton.size();
    if (nb == 0) return false;

    QHash<quint32, const Anchor*> byHash;
    for (const Anchor& a : kAnchors) byHash.insert(a.hash, &a);

    QVector<int> anchorIdx;                              // original indices of anchors, ascending
    QVector<const Anchor*> anchorDef;
    for (int i = 0; i < nb; ++i) {
        if (const Anchor* a = byHash.value(geo.skeleton[i].nameHash, nullptr)) {
            anchorIdx.append(i);
            anchorDef.append(a);
        }
    }
    // Player rigs carry all 26; require most of them so monsters/props pass through untouched.
    if (anchorIdx.size() < 16)
        return false;

    QVector<bool> isAnchor(nb, false);
    QVector<int>  newIdx(nb, -1);
    for (int k = 0; k < anchorIdx.size(); ++k) { isAnchor[anchorIdx[k]] = true; newIdx[anchorIdx[k]] = k; }

    // Every bone folds into its nearest anchor ancestor (anchors map to themselves).
    // The root bone is an anchor on player rigs, so the walk terminates; if a stray
    // branch reaches -1 without one, it folds into the first anchor (root-most).
    QVector<int> jointMap(nb);
    for (int i = 0; i < nb; ++i) {
        int a = i;
        while (a >= 0 && !isAnchor[a]) {
            const int p = geo.skeleton[a].parent;
            a = (p >= 0 && p < a) ? p : -1;
        }
        jointMap[i] = newIdx[a >= 0 ? a : anchorIdx.first()];
    }
    rewriteWeights(geo, jointMap);

    // Reduced skeleton: anchors only, world bind transforms preserved (inverse bind
    // matrices reused verbatim); locals recomputed against the anchor-only hierarchy.
    const QVector<QMatrix4x4> global = globalRest(geo.skeleton);
    QVector<ModelJoint> reduced;
    reduced.reserve(anchorIdx.size());
    for (int k = 0; k < anchorIdx.size(); ++k) {
        const int oi = anchorIdx[k];
        ModelJoint j = geo.skeleton[oi];
        j.name = QString::fromLatin1(anchorDef[k]->name);
        // Nearest anchor strict-ancestor becomes the new parent.
        int p = geo.skeleton[oi].parent;
        while (p >= 0 && !isAnchor[p]) p = geo.skeleton[p].parent;
        j.parent = p >= 0 ? newIdx[p] : -1;
        const QMatrix4x4 L = p >= 0 ? (global[p].inverted() * global[oi]) : global[oi];
        setRestFromMatrix(j, L);
        reduced.append(j);
    }
    geo.skeleton   = reduced;
    geo.nBaseBones = reduced.size();
    geo.clothCapsules.clear();
    geo.clothSims.clear();
    return true;
}

void Retarget::applyFromSettings(ModelGeometry& geo)
{
    const QSettings s;
    if (s.value(QStringLiteral("retarget/collapseCloth"), false).toBool())
        collapseClothChains(geo);
    if (s.value(QStringLiteral("retarget/remapWeights"), false).toBool())
        remapToAnchors(geo);
}

QVector<QVector3D> Retarget::restHeadsD4(const QVector<ModelJoint>& skeleton)
{
    const QVector<QMatrix4x4> global = globalRest(skeleton);
    QVector<QVector3D> pos(skeleton.size());
    for (int i = 0; i < skeleton.size(); ++i)
        pos[i] = global[i].column(3).toVector3D();
    return pos;
}

QVector<int> Retarget::mirrorPairs(const QVector<ModelJoint>& skeleton)
{
    const int nb = skeleton.size();
    QVector<int> partner(nb, -1);
    if (nb == 0) return partner;

    // 1. Curated pairs by hash (kAnchors names carry the side as a ".L"/".R" suffix).
    QHash<QString, int> sideIdx[2];                 // [0]=left(.L), [1]=right(.R): base → index
    QHash<quint32, const Anchor*> byHash;
    for (const Anchor& a : kAnchors) byHash.insert(a.hash, &a);
    for (int i = 0; i < nb; ++i) {
        const Anchor* a = byHash.value(skeleton[i].nameHash, nullptr);
        if (!a) continue;
        const QString nm = QString::fromLatin1(a->name);
        if (nm.endsWith(QStringLiteral(".L"))) sideIdx[0].insert(nm.left(nm.size()-2), i);
        else if (nm.endsWith(QStringLiteral(".R"))) sideIdx[1].insert(nm.left(nm.size()-2), i);
    }
    for (auto it = sideIdx[0].constBegin(); it != sideIdx[0].constEnd(); ++it) {
        const int r = sideIdx[1].value(it.key(), -1);
        if (r >= 0) { partner[it.value()] = r; partner[r] = it.value(); }
    }

    // 2. Geometric reciprocal nearest-mirror across D4 Y=0, skipping paired bones.
    const QVector<QVector3D> pos = restHeadsD4(skeleton);
    const float kCenterEps = 1e-3f, kPairTol = 6e-3f;
    auto nearestMirror = [&](int i) -> int {
        const QVector3D m(pos[i].x(), -pos[i].y(), pos[i].z());
        int best = -1; float bd = kPairTol;
        for (int k = 0; k < nb; ++k) {
            if (k == i || partner[k] >= 0) continue;
            const float d = (pos[k] - m).length();
            if (d < bd) { bd = d; best = k; }
        }
        return best;
    };
    for (int i = 0; i < nb; ++i) {
        if (partner[i] >= 0 || qAbs(pos[i].y()) < kCenterEps) continue;
        const int j = nearestMirror(i);
        if (j >= 0 && nearestMirror(j) == i) { partner[i] = j; partner[j] = i; }
    }
    return partner;
}
