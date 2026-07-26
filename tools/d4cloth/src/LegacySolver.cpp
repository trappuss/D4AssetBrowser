#include "LegacySolver.h"

#include <QtGlobal>
#include <algorithm>
#include <cmath>

// Port source: GLModelWidget.cpp as of 2026-07-25 (fnv of the staged file recorded in
// FINDINGS.md context). Comments here are ONLY about port decisions; see the app source
// for the original rationale. Line references are to that file.

namespace d4cloth {

using Mat4 = RigMath::Mat4;

namespace {
// GLModelWidget rotFromTo (lines ~1404-1432): rotation matrix taking direction a to b.
void rotFromTo(const float a[3], const float b[3], float r[9])
{
    auto norm = [](const float v[3], float o[3]) {
        const float l = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        if (l < 1e-8f) { o[0]=0; o[1]=1; o[2]=0; return; }
        o[0]=v[0]/l; o[1]=v[1]/l; o[2]=v[2]/l;
    };
    float an[3], bn[3]; norm(a, an); norm(b, bn);
    const float ax=an[0], ay=an[1], az=an[2];
    const float vx = ay*bn[2]-az*bn[1], vy = az*bn[0]-ax*bn[2], vz = ax*bn[1]-ay*bn[0];
    const float c = ax*bn[0]+ay*bn[1]+az*bn[2];
    if (c > 0.9999f) { r[0]=1;r[1]=0;r[2]=0;r[3]=0;r[4]=1;r[5]=0;r[6]=0;r[7]=0;r[8]=1; return; }
    if (c < -0.9999f) {
        float px = (std::fabs(ax) < 0.9f) ? 1.0f : 0.0f, py = (px==0.0f) ? 1.0f : 0.0f;
        float kx=ay*0-az*py, ky=az*px-ax*0, kz=ax*py-ay*px;
        const float kl=std::sqrt(kx*kx+ky*ky+kz*kz); if (kl>1e-8f){kx/=kl;ky/=kl;kz/=kl;}
        r[0]=2*kx*kx-1; r[1]=2*ky*kx;   r[2]=2*kz*kx;
        r[3]=2*kx*ky;   r[4]=2*ky*ky-1; r[5]=2*kz*ky;
        r[6]=2*kx*kz;   r[7]=2*ky*kz;   r[8]=2*kz*kz-1;
        return;
    }
    const float k = 1.0f/(1.0f+c);
    r[0]=1+(-vz*vz-vy*vy)*k;  r[1]=vz+(vx*vy)*k;       r[2]=-vy+(vx*vz)*k;
    r[3]=-vz+(vx*vy)*k;       r[4]=1+(-vz*vz-vx*vx)*k; r[5]=vx+(vy*vz)*k;
    r[6]=vy+(vx*vz)*k;        r[7]=-vx+(vy*vz)*k;      r[8]=1+(-vy*vy-vx*vx)*k;
}
} // namespace

LegacySolver::LegacySolver(const Scene& s) : sc(s), P(s.params)
{
    nb = sc.geo.skeleton.size();
    animG = sc.restGlobal;          // no animation: the animated pose IS the rest pose
    global = animG;
}

void LegacySolver::tr(const char* phase, const QString& id, const float* p, const QString& note)
{
    if (!traceSink) return;
    traceSink(QStringLiteral("%1,%2,%3,%4,%5,%6,%7")
                  .arg(stepIndex).arg(QLatin1String(phase), id)
                  .arg(double(p[0]), 0, 'f', 6).arg(double(p[1]), 0, 'f', 6)
                  .arg(double(p[2]), 0, 'f', 6).arg(note));
}

// GLModelWidget::buildClothSim, authored-capsule path only (lines 2877-2953). The barF test
// matrix always carries authored capsules; the skin-fit fallback is not ported (asserted).
void LegacySolver::buildCapsules()
{
    colBoneA.clear(); colBoneB.clear();
    colP0Bind.clear(); colP1Bind.clear(); colR0.clear(); colR1.clear();
    colAuthored = !sc.geo.clothCapsules.isEmpty();
    if (!colAuthored) return;                       // skin-fit path NOT ported
    const QVector<Mat4>& restG = sc.restGlobal;
    for (const ClothCapsule& c : sc.geo.clothCapsules) {
        if (c.boneIndex < 0 || c.boneIndex >= nb) continue;
        const Mat4& m = restG[c.boneIndex];
        const float det = m[0]*(m[5]*m[10]-m[6]*m[9]) - m[4]*(m[1]*m[10]-m[2]*m[9])
                        + m[8]*(m[1]*m[6]-m[2]*m[5]);
        const float mir = (det < 0.0f) ? -1.0f : 1.0f;
        const float qx=c.localQ[0], qy=c.localQ[1], qz=c.localQ[2], qw=c.localQ[3];
        // capAxis: the app's live setting is 3 ("bone"), which the current implementation
        // does not branch on — it falls through to the X column. Ported as-is.
        float ax = 1-2*(qy*qy+qz*qz), ay = 2*(qx*qy+qw*qz), az = 2*(qx*qz-qw*qy);
        ax *= mir; ay *= mir; az *= mir;
        const float hh = c.height * 0.5f;
        const float lpx = c.localP[0]*mir, lpy = c.localP[1]*mir, lpz = c.localP[2]*mir;
        const float l0[3] = { lpx-ax*hh, lpy-ay*hh, lpz-az*hh };
        const float l1[3] = { lpx+ax*hh, lpy+ay*hh, lpz+az*hh };
        auto xf = [&](const float* in, float* o) {
            o[0] = m[0]*in[0] + m[4]*in[1] + m[8]*in[2] + m[12];
            o[1] = m[1]*in[0] + m[5]*in[1] + m[9]*in[2] + m[13];
            o[2] = m[2]*in[0] + m[6]*in[1] + m[10]*in[2] + m[14];
        };
        float p0[3], p1[3]; xf(l0, p0); xf(l1, p1);
        float r0 = c.radius1, r1 = c.radius2;
        if (p0[1] < p1[1]) std::swap(r0, r1);
        colBoneA.push_back(c.boneIndex); colBoneB.push_back(c.boneIndex);
        colP0Bind << p0[0] << p0[1] << p0[2];
        colP1Bind << p1[0] << p1[1] << p1[2];
        colR0.push_back(r0); colR1.push_back(r1);
    }
    colP0.fill(0.0f, colR0.size() * 3);
    colP1.fill(0.0f, colR0.size() * 3);
}

// GLModelWidget::buildSpringBones (lines 1436-1875), part flags all false.
void LegacySolver::buildSpringBones()
{
    sbSeeded = false;
    sbOrder.clear();
    sbIsCloth.fill(0, nb); sbChild.fill(-1, nb); sbLenParent.fill(0.0f, nb);
    sbSimHead.fill(0.0f, nb*3); sbPrevHead.fill(0.0f, nb*3);
    sbHair.fill(0, nb);
    cages.clear();
    sbAnchorPiece.fill(-1, nb); sbAnchorVert.fill(-1, nb); sbDriven.fill(0, nb);
    sbAnchorW.fill(0.0f, nb);
    sbContact.fill(0, nb);
    const QVector<ClothSim>& clothSims = sc.geo.clothSims;
    const int baseBones = sc.baseBones;
    if (nb <= 0) return;
    if (baseBones <= 0 && clothSims.isEmpty()) return;
    const bool noSplit = (baseBones <= 0 || baseBones >= nb);
    if (noSplit && clothSims.isEmpty()) return;
    constexpr int kMinBodyRig = 64;
    const bool badSplit = !noSplit && (nb - baseBones > nb / 2) && (baseBones < kMinBodyRig);
    if (badSplit && clothSims.isEmpty() && sc.geo.clothCapsules.isEmpty()) return;
    const int scanStart = noSplit ? 1 : baseBones;
    const QVector<Mat4>& restG = sc.restGlobal;

    if (!noSplit)
        for (int j = baseBones; j < nb; ++j) { sbIsCloth[j] = 1; sbOrder.push_back(j); }
    for (int j = scanStart; j < nb; ++j) {
        const int p = sc.geo.skeleton[j].parent;
        if (p >= 0 && p < nb) {
            if (sbChild[p] < 0) sbChild[p] = j;
            const float dx=restG[j][12]-restG[p][12], dy=restG[j][13]-restG[p][13], dz=restG[j][14]-restG[p][14];
            sbLenParent[j] = std::sqrt(dx*dx + dy*dy + dz*dz);
        }
    }
    // cage vert → bone map (≤3cm), pins, per-bone attachLen, constraints
    sbConA.clear(); sbConB.clear(); sbConRest.clear();
    sbPin.fill(0, nb);
    sbAttach.fill(1.0f, nb);
    sbSim.fill(-1, nb);
    for (int si = 0; si < clothSims.size(); ++si) {
        const ClothSim& cs = clothSims[si];
        QVector<int> v2b(cs.vertCount, -1);
        for (int k = 0; k < cs.vertCount; ++k) {
            const float* cp = cs.bindVerts.constData() + k*3;
            int best = -1; float bd = 0.03f*0.03f;
            for (int b = scanStart; b < nb; ++b) {
                const float dx=restG[b][12]-cp[0], dy=restG[b][13]-cp[1], dz=restG[b][14]-cp[2];
                const float d2 = dx*dx+dy*dy+dz*dz; if (d2 < bd) { bd = d2; best = b; }
            }
            v2b[k] = best;
            if (best >= 0) {
                sbSim[best] = si;
                if (k < cs.invMasses.size() && cs.invMasses[k] == 0.0f) sbPin[best] = 1;
                if (k < cs.attachLen.size()) sbAttach[best] = cs.attachLen[k];
            }
        }
        for (int e = 0; e < cs.constraintLen.size() && e*2+1 < cs.constraintIdx.size(); ++e) {
            const int a = cs.constraintIdx[e*2], b = cs.constraintIdx[e*2+1];
            if (a < cs.vertCount && b < cs.vertCount) {
                const int ba = v2b[a], bb = v2b[b];
                if (ba >= 0 && bb >= 0 && ba != bb) {
                    sbConA.push_back(ba); sbConB.push_back(bb); sbConRest.push_back(cs.constraintLen[e]);
                }
            }
        }
    }
    if (noSplit) {
        for (int j = scanStart; j < nb; ++j)
            if (sbSim[j] >= 0) { sbIsCloth[j] = 1; sbOrder.push_back(j); }
    } else if (badSplit) {
        QVector<int> kept;
        for (int j : sbOrder) {
            if (j < sbSim.size() && sbSim[j] >= 0) kept.push_back(j);
            else sbIsCloth[j] = 0;
        }
        sbOrder = kept;
    }
    // plane colliders: none in the barF matrix (planeCount=0 everywhere) — not ported yet.
    // hair flags: part flags are all false in the harness → no hair bones.
    for (int b : sc.geo.pinnedBones) if (b >= 0 && b < nb) sbPin[b] = 1;

    // ── cage runtime: borrow skinning from the nearest cloth-skinned render vert ──
    {
        const int vcount2 = sc.bindVerts.size() / 11;
        QVector<int> clothSkinned;
        for (int v = 0; v < vcount2 && v < sc.vJoints.size(); ++v) {
            const auto& J = sc.vJoints[v]; const auto& W = sc.vWeights[v];
            for (int k = 0; k < 4; ++k)
                if (W[k] > 0.0f && J[k] < nb && sbIsCloth[J[k]]) { clothSkinned.push_back(v); break; }
        }
        for (int si = 0; si < clothSims.size(); ++si) {
            const ClothSim& cs = clothSims[si];
            const int nv = cs.vertCount;
            if (nv <= 0 || cs.bindVerts.size() < nv*3) continue;
            if (cs.constraintLen.isEmpty()) continue;
            CageRt rt; rt.simIdx = si;
            rt.J.resize(nv); rt.W.resize(nv);
            rt.pos.fill(0.0f, nv*3); rt.prev.fill(0.0f, nv*3); rt.target.fill(0.0f, nv*3);
            for (int k = 0; k < nv; ++k) {
                const float* cp = cs.bindVerts.constData() + k*3;
                int bestV = -1; float bd = 0.20f*0.20f;
                for (int v : clothSkinned) {
                    const float* q = sc.bindVerts.constData() + v*11;
                    const float dx=q[0]-cp[0], dy=q[1]-cp[1], dz=q[2]-cp[2];
                    const float d2=dx*dx+dy*dy+dz*dz; if (d2 < bd) { bd = d2; bestV = v; }
                }
                if (bestV >= 0) { rt.J[k] = sc.vJoints[bestV]; rt.W[k] = sc.vWeights[bestV]; }
                else {
                    int bb = -1; float bbd = 1e30f;
                    for (int b = baseBones; b < nb; ++b) {
                        const float dx=restG[b][12]-cp[0], dy=restG[b][13]-cp[1], dz=restG[b][14]-cp[2];
                        const float d2=dx*dx+dy*dy+dz*dz; if (d2 < bbd) { bbd = d2; bb = b; }
                    }
                    rt.J[k] = {{quint16(qMax(bb, 0)), 0, 0, 0}};
                    rt.W[k] = {{bb >= 0 ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f}};
                }
            }
            cages.push_back(rt);
        }
        // cage span (world-space reach from the pinned edge)
        cageSpan.fill(0.0f, clothSims.size());
        for (int si = 0; si < clothSims.size(); ++si) {
            const ClothSim& cs3 = clothSims[si];
            const int nv3 = cs3.vertCount;
            if (nv3 <= 0 || nv3*3 > cs3.bindVerts.size()) continue;
            QVector<int> pins3;
            for (int k = 0; k < nv3 && k < cs3.invMasses.size(); ++k)
                if (cs3.invMasses[k] == 0.0f) pins3.push_back(k);
            float span = 0.0f;
            if (pins3.isEmpty()) {
                float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
                for (int k = 0; k < nv3; ++k)
                    for (int c = 0; c < 3; ++c) {
                        const float v = cs3.bindVerts[k*3+c];
                        mn[c] = qMin(mn[c], v); mx[c] = qMax(mx[c], v);
                    }
                const float dx = mx[0]-mn[0], dy = mx[1]-mn[1], dz = mx[2]-mn[2];
                span = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
            } else {
                for (int k = 0; k < nv3; ++k) {
                    const float* p = cs3.bindVerts.constData() + k*3;
                    float best = 1e30f;
                    for (int pi : pins3) {
                        const float* q = cs3.bindVerts.constData() + pi*3;
                        const float dx=p[0]-q[0], dy=p[1]-q[1], dz=p[2]-q[2];
                        best = qMin(best, dx*dx + dy*dy + dz*dz);
                    }
                    span = qMax(span, std::sqrt(best));
                }
            }
            cageSpan[si] = span;
        }
        // attachLen synthesis: authored arrays present on every matrix piece → not ported.
        // ── anchor each cloth bone to its nearest cage particle (the 10cm mechanism) ──
        for (int j = scanStart; j < nb; ++j) {
            if (sbPin[j] || !sbIsCloth[j]) continue;
            const bool hair = false;
            int bp = -1, bv = -1;
            float bd = hair ? 0.03f*0.03f : 0.10f*0.10f;
            const int pj       = sc.geo.skeleton[j].parent;
            const int prefCage = (pj >= 0 && pj < sbAnchorPiece.size()) ? sbAnchorPiece[pj] : -1;
            constexpr float kForeignPenalty = 4.0f;
            for (int ci = 0; ci < cages.size(); ++ci) {
                const ClothSim& cs = clothSims[cages[ci].simIdx];
                const float pen = (prefCage < 0 || ci == prefCage) ? 1.0f : kForeignPenalty;
                for (int k = 0; k < cs.vertCount; ++k) {
                    const float* cp = cs.bindVerts.constData() + k*3;
                    const float dx=restG[j][12]-cp[0], dy=restG[j][13]-cp[1], dz=restG[j][14]-cp[2];
                    const float d2=(dx*dx+dy*dy+dz*dz) * pen;
                    if (d2 < bd) { bd = d2; bp = ci; bv = k; }
                }
            }
            if (bp >= 0) {
                sbAnchorPiece[j] = bp; sbAnchorVert[j] = bv; sbDriven[j] = 1;
                const float d      = std::sqrt(qMax(0.0f, bd));
                constexpr float kFull = 0.03f, kFar = 0.10f, kMinW = 0.25f;
                float w = 1.0f;
                if (d > kFull) {
                    const float t = qBound(0.0f, (d - kFull) / (kFar - kFull), 1.0f);
                    w = 1.0f - (1.0f - kMinW) * t * t;
                }
                sbAnchorW[j] = qBound(kMinW, w, 1.0f);
            }
        }
    }
    m_built = true;
}

void LegacySolver::poseCapsules()
{
    auto capXf = [&](int b, const float* in, float* o) {
        if (b < 0 || b >= nb) { o[0]=in[0]; o[1]=in[1]; o[2]=in[2]; return; }
        const Mat4 m = RigMath::mat4mul(global[b], sc.geo.skeleton[b].inverseBind);
        o[0]=m[0]*in[0]+m[4]*in[1]+m[8]*in[2]+m[12];
        o[1]=m[1]*in[0]+m[5]*in[1]+m[9]*in[2]+m[13];
        o[2]=m[2]*in[0]+m[6]*in[1]+m[10]*in[2]+m[14];
    };
    const int nCap = colR0.size();
    for (int i=0;i<nCap;++i){ capXf(colBoneA[i], colP0Bind.constData()+i*3, colP0.data()+i*3);
                              capXf(colBoneB[i], colP1Bind.constData()+i*3, colP1.data()+i*3); }
}

// GLModelWidget::springBoneStep (lines 1937-2594), part-for-part.
void LegacySolver::springBoneStep()
{
    const QVector<ClothSim>& clothSims = sc.geo.clothSims;
    if (sbOrder.isEmpty() && cages.isEmpty()) return;
    if (!sbSeeded) {
        for (int j : sbOrder) {
            sbSimHead[j*3]=sbPrevHead[j*3]=animG[j][12];
            sbSimHead[j*3+1]=sbPrevHead[j*3+1]=animG[j][13];
            sbSimHead[j*3+2]=sbPrevHead[j*3+2]=animG[j][14];
        }
        sbSeeded = true; return;
    }
    const float stiff = qBound(0.0f, P.boneStiffness, 1.0f);
    const float keep  = qBound(0.0f, P.damping, 0.999f);
    const float grav  = P.gravity;
    const float kMargin = P.collisionMargin;
    kDivergeMax = qMax(0.001f, sc.radius) * 0.28f;
    const int kIters = qBound(6, P.iterations * 4, 30);
    const int   subSteps = qBound(1, P.subSteps, 4);
    const float subF     = 1.0f / float(subSteps);
    const float rScale = P.capsuleRadius;

    // user-spin (yaw-scripted)
    float spinOmega = 0.0f, spinAlpha = 0.0f;
    const bool spinOn = P.userSpin && P.userSpinForce > 0.0f;
    if (spinOn) {
        float d = m_yaw - m_spinPrevYaw;
        while (d >  3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        if (!m_spinSeeded) { d = 0.0f; m_spinSeeded = true; }
        m_spinPrevYaw = m_yaw;
        const float prevOmega = m_spinOmega;
        m_spinOmega = m_spinOmega * 0.80f + d * 0.20f;
        if (std::fabs(m_spinOmega) < 1e-5f) m_spinOmega = 0.0f;
        spinOmega = m_spinOmega;
        spinAlpha = m_spinOmega - prevOmega;
    } else {
        m_spinOmega = 0.0f; m_spinPrevYaw = m_yaw; m_spinSeeded = false;
    }
    const float spinIn = qBound(0.0f, P.userSpinForce, 5.0f);
    const float spinK = spinIn * spinIn;
    const float spinAccelCap = qMax(0.002f, sc.radius * 0.05f);
    const float spinDrag = 1.0f - qBound(0.0f, P.dragFactor, 1.0f) * 0.5f;
    const float spinCx = sc.homeCenter[0], spinCz = sc.homeCenter[2];
    const float spinMinR = qMax(0.05f, sc.radius * 0.25f);
    auto spinAccel = [&](const float* Pp, float& ax, float& az) {
        if (!spinOn || (spinOmega == 0.0f && spinAlpha == 0.0f)) return;
        float rx = Pp[0] - spinCx, rz = Pp[2] - spinCz;
        const float r = std::sqrt(rx*rx + rz*rz);
        if (r < spinMinR) {
            if (r > 1e-6f) { const float s = spinMinR / r; rx *= s; rz *= s; }
            else           { rx = spinMinR; rz = 0.0f; }
        }
        float sx = (-spinAlpha * rz) * spinK * 60.0f + (spinOmega * spinOmega * rx) * spinK * 15.0f;
        float sz = ( spinAlpha * rx) * spinK * 60.0f + (spinOmega * spinOmega * rz) * spinK * 15.0f;
        sx *= spinDrag; sz *= spinDrag;
        const float m2 = sx*sx + sz*sz;
        if (m2 > spinAccelCap*spinAccelCap) {
            const float s = spinAccelCap / std::sqrt(m2);
            sx *= s; sz *= s;
        }
        ax += sx; az += sz;
    };

    const float fricK = qBound(0.0f, P.friction, 1.0f);
    contactCount = 0; worstPen = 0.0f;
    float minColR = 1e30f;
    for (int i = 0; i < colR0.size(); ++i)
        minColR = qMin(minColR, qMin(colR0[i], colR1[i]) * rScale);
    const float maxStep = (minColR < 1e29f) ? qMax(0.01f, minColR * 0.75f) : 1e30f;
    auto clampStep = [maxStep](const float* from, float& nx, float& ny, float& nz) {
        const float dx=nx-from[0], dy=ny-from[1], dz=nz-from[2];
        const float d2 = dx*dx+dy*dy+dz*dz;
        if (d2 <= maxStep*maxStep || d2 < 1e-12f) return;
        const float s = maxStep / std::sqrt(d2);
        nx = from[0] + dx*s; ny = from[1] + dy*s; nz = from[2] + dz*s;
    };
    auto collide = [&](float* Pp, float* Q, bool fixVel) {
        for (int i = 0; i < colR0.size(); ++i) {
            const float* p0=colP0.constData()+i*3; const float* p1=colP1.constData()+i*3;
            const float sx=p1[0]-p0[0], sy=p1[1]-p0[1], sz=p1[2]-p0[2]; const float sl2=sx*sx+sy*sy+sz*sz;
            auto axisT = [&](const float* X) {
                float t = sl2 > 1e-8f ? ((X[0]-p0[0])*sx + (X[1]-p0[1])*sy + (X[2]-p0[2])*sz) / sl2 : 0.f;
                return qBound(0.f, t, 1.f);
            };
            const float t  = axisT(Pp);
            const float cx=p0[0]+sx*t, cy=p0[1]+sy*t, cz=p0[2]+sz*t;
            float dx=Pp[0]-cx, dy=Pp[1]-cy, dz=Pp[2]-cz;
            float d = std::sqrt(dx*dx+dy*dy+dz*dz);
            const float r = (colR0[i]+(colR1[i]-colR0[i])*t)*rScale + kMargin;
            if (d >= r) continue;
            if (Q) {
                const float qt = axisT(Q);
                const float qcx=p0[0]+sx*qt, qcy=p0[1]+sy*qt, qcz=p0[2]+sz*qt;
                float ex=Q[0]-qcx, ey=Q[1]-qcy, ez=Q[2]-qcz;
                const float el = std::sqrt(ex*ex+ey*ey+ez*ez);
                if (el > 1e-5f) {
                    ex/=el; ey/=el; ez/=el;
                    if (d <= 1e-5f || (dx*ex + dy*ey + dz*ez) < 0.0f) {
                        dx = ex; dy = ey; dz = ez; d = qMax(d, 1e-5f);
                    }
                }
            }
            if (d <= 1e-5f) { dx = 0.0f; dy = 1.0f; dz = 0.0f; d = 1e-5f; }
            const float nx=dx/d, ny=dy/d, nz=dz/d;
            const float pen = r - d;
            if (pen > worstPen) worstPen = pen;
            ++contactCount;
            Pp[0] = cx + nx*r; Pp[1] = cy + ny*r; Pp[2] = cz + nz*r;
            if (!Q || !fixVel) continue;
            float vx=Pp[0]-Q[0], vy=Pp[1]-Q[1], vz=Pp[2]-Q[2];
            const float vn = vx*nx + vy*ny + vz*nz;
            float tvx = vx - vn*nx, tvy = vy - vn*ny, tvz = vz - vn*nz;
            const float keepT = 1.0f - fricK;
            tvx *= keepT; tvy *= keepT; tvz *= keepT;
            const float outN = qMax(0.0f, vn);
            Q[0] = Pp[0] - (tvx + outN*nx);
            Q[1] = Pp[1] - (tvy + outN*ny);
            Q[2] = Pp[2] - (tvz + outN*nz);
        }
    };
    auto sweep = [&](float* Pp, float* Q) {
        const float mx=Pp[0]-Q[0], my=Pp[1]-Q[1], mz=Pp[2]-Q[2];
        const float mv2 = mx*mx+my*my+mz*mz;
        if (mv2 < 1e-8f) return;
        for (int i = 0; i < colR0.size(); ++i) {
            const float* p0=colP0.constData()+i*3; const float* p1=colP1.constData()+i*3;
            const float r = qMax(colR0[i], colR1[i])*rScale + kMargin;
            const float bcx=(p0[0]+p1[0])*0.5f, bcy=(p0[1]+p1[1])*0.5f, bcz=(p0[2]+p1[2])*0.5f;
            const float hx=p1[0]-bcx, hy=p1[1]-bcy, hz=p1[2]-bcz;
            const float bR = std::sqrt(hx*hx+hy*hy+hz*hz) + r;
            const float ex=Q[0]-bcx, ey=Q[1]-bcy, ez=Q[2]-bcz;
            const float far = std::sqrt(ex*ex+ey*ey+ez*ez) - std::sqrt(mv2) - bR;
            if (far > 0.0f) continue;
            {
                float qt = 0.f;
                const float qax=p1[0]-p0[0], qay=p1[1]-p0[1], qaz=p1[2]-p0[2];
                const float qal2=qax*qax+qay*qay+qaz*qaz;
                if (qal2 > 1e-8f) qt = qBound(0.f, ((Q[0]-p0[0])*qax+(Q[1]-p0[1])*qay+(Q[2]-p0[2])*qaz)/qal2, 1.f);
                const float qcx=p0[0]+qax*qt, qcy=p0[1]+qay*qt, qcz=p0[2]+qaz*qt;
                const float qdx=Q[0]-qcx, qdy=Q[1]-qcy, qdz=Q[2]-qcz;
                const float qr=(colR0[i]+(colR1[i]-colR0[i])*qt)*rScale + kMargin;
                if (qdx*qdx+qdy*qdy+qdz*qdz <= qr*qr) continue;
            }
            constexpr int kS = 4;
            for (int s = 1; s <= kS; ++s) {
                const float u = float(s) / float(kS);
                const float X[3] = { Q[0]+mx*u, Q[1]+my*u, Q[2]+mz*u };
                float tt = 0.f;
                const float ax=p1[0]-p0[0], ay=p1[1]-p0[1], az=p1[2]-p0[2];
                const float al2=ax*ax+ay*ay+az*az;
                if (al2 > 1e-8f) tt = qBound(0.f, ((X[0]-p0[0])*ax+(X[1]-p0[1])*ay+(X[2]-p0[2])*az)/al2, 1.f);
                const float ccx=p0[0]+ax*tt, ccy=p0[1]+ay*tt, ccz=p0[2]+az*tt;
                const float ddx=X[0]-ccx, ddy=X[1]-ccy, ddz=X[2]-ccz;
                const float rr=(colR0[i]+(colR1[i]-colR0[i])*tt)*rScale + kMargin;
                if (ddx*ddx+ddy*ddy+ddz*ddz < rr*rr) {
                    Pp[0]=X[0]; Pp[1]=X[1]; Pp[2]=X[2];
                    return;
                }
            }
        }
    };

    // ── cage-level sim ──
    cageLimitHits = 0; cageSafetyHits = 0; boneLimitHits = 0;
    QVector<Mat4> cagePal; QVector<quint8> cagePalDone;
    if (!cages.isEmpty()) { cagePal.resize(nb); cagePalDone.fill(0, nb); }
    auto cagePalGet = [&](int b) -> const Mat4& {
        if (!cagePalDone[b]) { cagePal[b] = RigMath::mat4mul(animG[b], sc.geo.skeleton[b].inverseBind); cagePalDone[b] = 1; }
        return cagePal[b];
    };
    for (CageRt& rt : cages) {
        if (rt.simIdx < 0 || rt.simIdx >= clothSims.size()) continue;
        const ClothSim& cs = clothSims[rt.simIdx];
        const int nv = cs.vertCount;
        if (nv <= 0 || rt.pos.size() < nv*3) continue;
        for (int k = 0; k < nv; ++k) {
            const float* bp = cs.bindVerts.constData() + k*3;
            float tx=0, ty=0, tz=0, ws=0;
            for (int t = 0; t < 4; ++t) {
                const float w = rt.W[k][t]; if (w <= 0.0f) continue;
                const int b = rt.J[k][t];   if (b < 0 || b >= nb) continue;
                const Mat4& m = cagePalGet(b);
                tx += w*(m[0]*bp[0]+m[4]*bp[1]+m[8]*bp[2] +m[12]);
                ty += w*(m[1]*bp[0]+m[5]*bp[1]+m[9]*bp[2] +m[13]);
                tz += w*(m[2]*bp[0]+m[6]*bp[1]+m[10]*bp[2]+m[14]);
                ws += w;
            }
            float* T = rt.target.data() + k*3;
            if (ws > 1e-6f) { T[0]=tx/ws; T[1]=ty/ws; T[2]=tz/ws; }
            else            { T[0]=bp[0]; T[1]=bp[1]; T[2]=bp[2]; }
        }
        if (!rt.seeded) { rt.pos = rt.target; rt.prev = rt.target; rt.seeded = true; continue; }
        const bool tuned = cs.tuned;
        auto pinnedAt = [&](int k) { return k < cs.invMasses.size() && cs.invMasses[k] == 0.0f; };
        const int cageIdx = int(&rt - cages.constData());
        for (int ss = 0; ss < subSteps; ++ss) {
        const float gj = grav * (tuned ? cs.gravScale : 1.0f) * subF;
        const float wx = cs.windX*subF, wy = cs.windY*subF, wz = cs.windZ*subF;
        for (int k = 0; k < nv; ++k) {
            float* Pp = rt.pos.data()+k*3; float* Q = rt.prev.data()+k*3;
            const float* T = rt.target.constData()+k*3;
            if (pinnedAt(k)) { Q[0]=Pp[0];Q[1]=Pp[1];Q[2]=Pp[2]; Pp[0]=T[0];Pp[1]=T[1];Pp[2]=T[2]; continue; }
            float sax = 0.0f, saz = 0.0f;
            spinAccel(Pp, sax, saz);
            sax *= subF; saz *= subF;
            float n0=Pp[0]+(Pp[0]-Q[0])*keep + wx + sax, n1=Pp[1]+(Pp[1]-Q[1])*keep + gj + wy, n2=Pp[2]+(Pp[2]-Q[2])*keep + wz + saz;
            n0+=(T[0]-n0)*stiff; n1+=(T[1]-n1)*stiff; n2+=(T[2]-n2)*stiff;
            clampStep(Pp, n0, n1, n2);
            Q[0]=Pp[0];Q[1]=Pp[1];Q[2]=Pp[2]; Pp[0]=n0;Pp[1]=n1;Pp[2]=n2;
            sweep(Pp, Q);
            if (watchedPart(cageIdx, k)) tr("integrate", QStringLiteral("p:%1/%2").arg(cageIdx).arg(k), Pp);
        }
        const float trk = tuned ? qBound(0.0f, cs.boneTrack, 1.0f)
                                : qBound(0.0f, P.boneTracking, 1.0f);
        const float tnorm = qBound(0.0f, (1.0f - trk) / 0.55f, 1.0f);
        const float span    = (rt.simIdx < cageSpan.size() && cageSpan[rt.simIdx] > 1e-4f)
                              ? cageSpan[rt.simIdx] : 1.0f;
        const float mdScale = P.maxDistance * qMax(0.05f, tnorm * tnorm) * span;
        for (int it = 0; it < kIters; ++it) {
            for (int e = 0; e < cs.constraintLen.size() && e*2+1 < cs.constraintIdx.size(); ++e) {
                const int a = cs.constraintIdx[e*2], b = cs.constraintIdx[e*2+1];
                if (a >= nv || b >= nv) continue;
                float* A = rt.pos.data()+a*3; float* B = rt.pos.data()+b*3;
                float dx=B[0]-A[0], dy=B[1]-A[1], dz=B[2]-A[2];
                const float len=std::sqrt(dx*dx+dy*dy+dz*dz); if (len < 1e-6f) continue;
                const float wa=pinnedAt(a)?0.f:1.f, wb=pinnedAt(b)?0.f:1.f, wsum=wa+wb;
                if (wsum < 1e-6f) continue;
                const float diff=(len-cs.constraintLen[e])/len, sa=diff*wa/wsum, sb=diff*wb/wsum;
                A[0]+=dx*sa;A[1]+=dy*sa;A[2]+=dz*sa; B[0]-=dx*sb;B[1]-=dy*sb;B[2]-=dz*sb;
            }
            for (int k = 0; k < nv; ++k) {
                if (pinnedAt(k)) continue;
                float* Pp = rt.pos.data()+k*3;
                const float* T = rt.target.constData()+k*3;
                const float al = (k < cs.attachLen.size()) ? cs.attachLen[k] : 1.0f;
                const float md = al * mdScale;
                const float dx=Pp[0]-T[0], dy=Pp[1]-T[1], dz=Pp[2]-T[2];
                const float d=std::sqrt(dx*dx+dy*dy+dz*dz);
                if (d > md && d > 1e-8f) {
                    const float s=md/d; Pp[0]=T[0]+dx*s; Pp[1]=T[1]+dy*s; Pp[2]=T[2]+dz*s;
                    if (it == kIters-1) ++cageLimitHits;
                    if (watchedPart(cageIdx, k))
                        tr("motion-limit", QStringLiteral("p:%1/%2").arg(cageIdx).arg(k), Pp,
                           QStringLiteral("md=%1 d=%2").arg(md).arg(d));
                }
            }
            // (plane colliders: none in the matrix)
            for (int k = 0; k < nv; ++k) {
                if (pinnedAt(k)) continue;
                collide(rt.pos.data()+k*3, rt.prev.data()+k*3, /*fixVel=*/false);
            }
        }
        for (int k = 0; k < nv; ++k) {
            if (pinnedAt(k)) continue;
            collide(rt.pos.data()+k*3, rt.prev.data()+k*3, /*fixVel=*/true);
        }
        for (int k = 0; k < nv; ++k) {
            if (pinnedAt(k)) continue;
            float* Pp = rt.pos.data()+k*3; float* Q = rt.prev.data()+k*3;
            const float* T = rt.target.constData()+k*3;
            const float al = (k < cs.attachLen.size()) ? cs.attachLen[k] : 1.0f;
            const float lim = qMin(al * mdScale * 1.5f, kDivergeMax) + kMargin;
            const float dx=Pp[0]-T[0], dy=Pp[1]-T[1], dz=Pp[2]-T[2];
            const float d2 = dx*dx+dy*dy+dz*dz;
            if (!std::isfinite(d2)) { Pp[0]=T[0];Pp[1]=T[1];Pp[2]=T[2]; Q[0]=T[0];Q[1]=T[1];Q[2]=T[2]; continue; }
            if (d2 > lim*lim && d2 > 1e-12f) {
                const float s = lim / std::sqrt(d2);
                const float vx=Pp[0]-Q[0], vy=Pp[1]-Q[1], vz=Pp[2]-Q[2];
                Pp[0]=T[0]+dx*s; Pp[1]=T[1]+dy*s; Pp[2]=T[2]+dz*s;
                Q[0]=Pp[0]-vx;   Q[1]=Pp[1]-vy;   Q[2]=Pp[2]-vz;
                ++cageSafetyHits;
                if (watchedPart(cageIdx, k))
                    tr("cage-safety", QStringLiteral("p:%1/%2").arg(cageIdx).arg(k), Pp,
                       QStringLiteral("lim=%1").arg(lim));
            }
        }
        }   // sub-step
    }
    // ── drive anchored bones from their cage particle ──
    for (int j : sbOrder) {
        if (j >= sbDriven.size() || !sbDriven[j]) continue;
        const int ci = sbAnchorPiece[j], kv = sbAnchorVert[j];
        if (ci < 0 || ci >= cages.size() || kv < 0) continue;
        const CageRt& rt = cages[ci];
        if (!rt.seeded || (kv+1)*3 > rt.pos.size()) continue;
        float* S=sbSimHead.data()+j*3; float* R=sbPrevHead.data()+j*3;
        R[0]=S[0];R[1]=S[1];R[2]=S[2];
        const float aw = (j < sbAnchorW.size() && sbAnchorW[j] > 0.0f) ? sbAnchorW[j] : 1.0f;
        S[0]=animG[j][12] + (rt.pos[kv*3]   - rt.target[kv*3])   * aw;
        S[1]=animG[j][13] + (rt.pos[kv*3+1] - rt.target[kv*3+1]) * aw;
        S[2]=animG[j][14] + (rt.pos[kv*3+2] - rt.target[kv*3+2]) * aw;
        if (watchedBone(j)) tr("cage-drive", QStringLiteral("b:%1").arg(j), S,
                               QStringLiteral("cage=%1 vert=%2 aw=%3").arg(ci).arg(kv).arg(aw));
    }
    // ── integrate free bones ──
    for (int j : sbOrder) {
        float* S=sbSimHead.data()+j*3; float* R=sbPrevHead.data()+j*3;
        if (j < sbDriven.size() && sbDriven[j]) continue;
        const bool animated = hasAnim() && j < sbAnimMoves.size() && sbAnimMoves[j];
        if (sbPin[j] || animated) { R[0]=S[0];R[1]=S[1];R[2]=S[2]; S[0]=animG[j][12]; S[1]=animG[j][13]; S[2]=animG[j][14]; continue; }
        const float ah0=animG[j][12], ah1=animG[j][13], ah2=animG[j][14];
        const int si = (j < sbSim.size()) ? sbSim[j] : -1;
        const bool tuned = (si >= 0 && si < clothSims.size() && clothSims[si].tuned);
        const float gj = grav * (tuned ? clothSims[si].gravScale : 1.0f);
        float wx=0,wy=0,wz=0;
        if (si>=0 && si<clothSims.size()) { wx=clothSims[si].windX; wy=clothSims[si].windY; wz=clothSims[si].windZ; }
        const bool hairBone = false;
        const float stiffJ = hairBone ? qMax(stiff, 0.55f) : stiff;
        const float keepJ  = hairBone ? qMin(keep, 0.45f)  : keep;
        const float gjJ    = hairBone ? gj * 0.35f          : gj;
        float sax = 0.0f, saz = 0.0f;
        spinAccel(S, sax, saz);
        float n0=S[0]+(S[0]-R[0])*keepJ + wx + sax, n1=S[1]+(S[1]-R[1])*keepJ + gjJ + wy, n2=S[2]+(S[2]-R[2])*keepJ + wz + saz;
        n0+=(ah0-n0)*stiffJ; n1+=(ah1-n1)*stiffJ; n2+=(ah2-n2)*stiffJ;
        clampStep(S, n0, n1, n2);
        R[0]=S[0];R[1]=S[1];R[2]=S[2]; S[0]=n0;S[1]=n1;S[2]=n2;
        sweep(S, R);
        if (watchedBone(j)) tr("bone-integrate", QStringLiteral("b:%1").arg(j), S);
    }
    // ── bone-level constraint network ──
    auto held = [&](int j) { return sbPin[j] || (j < sbDriven.size() && sbDriven[j]); };
    for (int it = 0; it < kIters; ++it) {
        for (int e = 0; e < sbConA.size(); ++e) {
            const int ia=sbConA[e], ib=sbConB[e];
            float* A=sbSimHead.data()+ia*3; float* B=sbSimHead.data()+ib*3;
            float dx=B[0]-A[0], dy=B[1]-A[1], dz=B[2]-A[2]; const float len=std::sqrt(dx*dx+dy*dy+dz*dz);
            if (len < 1e-6f) continue;
            const float wa=held(ia)?0.f:1.f, wb=held(ib)?0.f:1.f, ws=wa+wb; if (ws<1e-6f) continue;
            const float diff=(len-sbConRest[e])/len, sa=diff*wa/ws, sb=diff*wb/ws;
            A[0]+=dx*sa;A[1]+=dy*sa;A[2]+=dz*sa; B[0]-=dx*sb;B[1]-=dy*sb;B[2]-=dz*sb;
        }
        for (int j : sbOrder) if (!held(j)) {
            const int si2 = (j < sbSim.size()) ? sbSim[j] : -1;
            const bool tuned2 = (si2 >= 0 && si2 < clothSims.size() && clothSims[si2].tuned);
            float trk = tuned2 ? qBound(0.0f, clothSims[si2].boneTrack, 1.0f)
                               : qBound(0.0f, P.boneTracking, 1.0f);
            const float tnorm = qBound(0.0f, (1.0f - trk) / 0.55f, 1.0f);
            const float md = sbAttach[j] * P.maxDistance * qMax(0.05f, tnorm * tnorm);
            float* S=sbSimHead.data()+j*3;
            const float dx=S[0]-animG[j][12], dy=S[1]-animG[j][13], dz=S[2]-animG[j][14];
            const float d=std::sqrt(dx*dx+dy*dy+dz*dz);
            if (d > md) {
                const float s=md/d; S[0]=animG[j][12]+dx*s; S[1]=animG[j][13]+dy*s; S[2]=animG[j][14]+dz*s;
                if (it == kIters-1) ++boneLimitHits;
                if (watchedBone(j)) tr("bone-motion-limit", QStringLiteral("b:%1").arg(j), S,
                                       QStringLiteral("md=%1").arg(md));
            }
        }
        for (int j : sbOrder) if (!held(j)) {
            collide(sbSimHead.data()+j*3, sbPrevHead.data()+j*3, /*fixVel=*/false);
        }
    }
    for (int j : sbOrder) if (!held(j)) {
        const float before  = worstPen;
        const int   nBefore = contactCount;
        collide(sbSimHead.data()+j*3, sbPrevHead.data()+j*3, /*fixVel=*/true);
        if (j < sbContact.size()) {
            if (contactCount == nBefore)                 sbContact[j] = 0;
            else if (worstPen - before > kMargin * 4.0f) sbContact[j] = 2;
            else                                          sbContact[j] = 1;
        }
    }
    for (int j : sbOrder) if (!held(j)) {
        const int si2 = (j < sbSim.size()) ? sbSim[j] : -1;
        const bool tuned2 = (si2 >= 0 && si2 < clothSims.size() && clothSims[si2].tuned);
        float trk = tuned2 ? qBound(0.0f, clothSims[si2].boneTrack, 1.0f)
                           : qBound(0.0f, P.boneTracking, 1.0f);
        const float tnorm = qBound(0.0f, (1.0f - trk) / 0.55f, 1.0f);
        const float lim = qMin(sbAttach[j] * P.maxDistance * qMax(0.05f, tnorm * tnorm) * 1.5f,
                               kDivergeMax) + kMargin;
        float* S=sbSimHead.data()+j*3; float* R=sbPrevHead.data()+j*3;
        const float ax0=animG[j][12], ay0=animG[j][13], az0=animG[j][14];
        const float dx=S[0]-ax0, dy=S[1]-ay0, dz=S[2]-az0;
        const float d2=dx*dx+dy*dy+dz*dz;
        if (!std::isfinite(d2)) { S[0]=ax0;S[1]=ay0;S[2]=az0; R[0]=ax0;R[1]=ay0;R[2]=az0; continue; }
        if (d2 > lim*lim && d2 > 1e-12f) {
            const float s = lim/std::sqrt(d2);
            const float vx=S[0]-R[0], vy=S[1]-R[1], vz=S[2]-R[2];
            S[0]=ax0+dx*s; S[1]=ay0+dy*s; S[2]=az0+dz*s;
            R[0]=S[0]-vx;  R[1]=S[1]-vy;  R[2]=S[2]-vz;
            if (watchedBone(j)) tr("bone-safety", QStringLiteral("b:%1").arg(j), S,
                                   QStringLiteral("lim=%1").arg(lim));
        }
    }
    // ── hard divergence clamp ──
    {
        const float hardMax = kDivergeMax;
        divergeClamped = 0; worstDiv = 0.0f; worstDivBone = -1;
        for (int j : sbOrder) {
            if (sbPin[j]) continue;
            float* S = sbSimHead.data() + j*3;
            float* R = sbPrevHead.data() + j*3;
            const float ax = animG[j][12], ay = animG[j][13], az = animG[j][14];
            const float dx = S[0]-ax, dy = S[1]-ay, dz = S[2]-az;
            const float d2 = dx*dx + dy*dy + dz*dz;
            if (!std::isfinite(d2)) { S[0]=ax; S[1]=ay; S[2]=az; R[0]=ax; R[1]=ay; R[2]=az; continue; }
            const float d = std::sqrt(d2);
            if (d > worstDiv) { worstDiv = d; worstDivBone = j; }
            if (d2 > hardMax*hardMax) {
                const float s = hardMax/d;
                const float vx=S[0]-R[0], vy=S[1]-R[1], vz=S[2]-R[2];
                S[0]=ax+dx*s; S[1]=ay+dy*s; S[2]=az+dz*s;
                R[0]=S[0]-vx*0.25f; R[1]=S[1]-vy*0.25f; R[2]=S[2]-vz*0.25f;
                ++divergeClamped;
                if (watchedBone(j)) tr("diverge-clamp", QStringLiteral("b:%1").arg(j), S,
                                       QStringLiteral("cap=%1").arg(hardMax));
            }
        }
    }
    // ── pass 2: rebuild bone globals from simulated heads ──
    QVector<std::array<float,9>> swing(nb);
    for (int j : sbOrder) {
        if (hasAnim() && j < sbAnimMoves.size() && sbAnimMoves[j]) { global[j] = animG[j]; continue; }
        const int c = sbChild[j]; float r[9];
        if (c >= 0) {
            const float aD[3]={animG[c][12]-animG[j][12], animG[c][13]-animG[j][13], animG[c][14]-animG[j][14]};
            const float sD[3]={sbSimHead[c*3]-sbSimHead[j*3], sbSimHead[c*3+1]-sbSimHead[j*3+1], sbSimHead[c*3+2]-sbSimHead[j*3+2]};
            rotFromTo(aD, sD, r);
        } else {
            const int p = sc.geo.skeleton[j].parent;
            bool rolled = false;
            if (p >= 0 && p < nb) {
                const bool clothP = (p < sbIsCloth.size() && sbIsCloth[p] && (p + 1) * 3 <= sbSimHead.size());
                const float ppx = clothP ? sbSimHead[p*3]   : animG[p][12];
                const float ppy = clothP ? sbSimHead[p*3+1] : animG[p][13];
                const float ppz = clothP ? sbSimHead[p*3+2] : animG[p][14];
                const float aD[3]={animG[j][12]-animG[p][12], animG[j][13]-animG[p][13], animG[j][14]-animG[p][14]};
                const float sD[3]={sbSimHead[j*3]-ppx, sbSimHead[j*3+1]-ppy, sbSimHead[j*3+2]-ppz};
                const float al=aD[0]*aD[0]+aD[1]*aD[1]+aD[2]*aD[2], sl=sD[0]*sD[0]+sD[1]*sD[1]+sD[2]*sD[2];
                if (al > 1e-8f && sl > 1e-8f) { rotFromTo(aD, sD, r); rolled = true; }
            }
            if (!rolled) {
                if (p>=0 && p<nb && sbIsCloth[p]) { const auto& pr=swing[p]; for (int k=0;k<9;++k) r[k]=pr[k]; }
                else { r[0]=1;r[1]=0;r[2]=0;r[3]=0;r[4]=1;r[5]=0;r[6]=0;r[7]=0;r[8]=1; }
            }
        }
        for (int k=0;k<9;++k) swing[j][k]=r[k];
        const Mat4& a = animG[j]; Mat4 m;
        auto col = [&](int ci, int outBase) {
            const float v0=a[ci*4+0], v1=a[ci*4+1], v2=a[ci*4+2];
            m[outBase+0]=r[0]*v0+r[3]*v1+r[6]*v2;
            m[outBase+1]=r[1]*v0+r[4]*v1+r[7]*v2;
            m[outBase+2]=r[2]*v0+r[5]*v1+r[8]*v2; m[outBase+3]=0.0f;
        };
        col(0,0); col(1,4); col(2,8);
        m[12]=sbSimHead[j*3]; m[13]=sbSimHead[j*3+1]; m[14]=sbSimHead[j*3+2]; m[15]=1.0f;
        global[j]=m;
    }
}

// GLModelWidget::computeAnimMoves (lines 1884-1935): cloth bones whose track shows REAL
// motion follow the animation; static/rest tracks stay simulated.
void LegacySolver::computeAnimMoves()
{
    m_animMovesBuilt = true;
    sbAnimMoves.fill(0, nb);
    if (!sc.anim.valid) return;
    constexpr float kMoveT = 0.005f;
    constexpr float kMoveR = 0.9990f;
    for (int j = qMax(0, sc.baseBones); j < nb; ++j) {
        if (j < sbIsCloth.size() && !sbIsCloth[j]) continue;
        const int ai = sc.animByHash.value(sc.geo.skeleton[j].nameHash, -1);
        if (ai < 0 || ai >= sc.anim.bones.size()) continue;
        const auto& ba = sc.anim.bones[ai];
        bool moves = false;
        const int nt = ba.translations.size();
        if (nt > 1) {
            const auto& t0 = ba.translations[0];
            for (int f = 1; f < nt && !moves; ++f) {
                const auto& t = ba.translations[f];
                const float dx=t[0]-t0[0], dy=t[1]-t0[1], dz=t[2]-t0[2];
                if (dx*dx+dy*dy+dz*dz > kMoveT*kMoveT) moves = true;
            }
        }
        const int nr = ba.rotations.size();
        if (!moves && nr > 1) {
            const auto& q0 = ba.rotations[0];
            for (int f = 1; f < nr && !moves; ++f) {
                const auto& q = ba.rotations[f];
                const float d = std::fabs(q0[0]*q[0]+q0[1]*q[1]+q0[2]*q[2]+q0[3]*q[3]);
                if (d < kMoveR) moves = true;
            }
        }
        sbAnimMoves[j] = moves ? 1 : 0;
    }
}

// applySkinning's local/global build (lines 2619-2668): anim track (frame clamped per
// track) else rest TRS; hierarchy resolved by readiness.
void LegacySolver::setAnimFrame(int f)
{
    m_animFrame = f;
    if (f < 0 || !sc.anim.valid) { animG = sc.restGlobal; return; }
    QVector<Mat4> local(nb);
    for (int j = 0; j < nb; ++j) {
        const ModelJoint& jt = sc.geo.skeleton[j];
        const int ai = sc.animByHash.value(jt.nameHash, -1);
        if (ai >= 0 && ai < sc.anim.bones.size()) {
            const auto& ba = sc.anim.bones[ai];
            if (!ba.rotations.isEmpty() && !ba.translations.isEmpty() && !ba.scales.isEmpty()) {
                const int rf = qBound(0, f, int(ba.rotations.size())    - 1);
                const int tf = qBound(0, f, int(ba.translations.size()) - 1);
                const int sf = qBound(0, f, int(ba.scales.size())       - 1);
                local[j] = composeTRSSwapped(ba.rotations[rf].data(), ba.translations[tf].data(),
                                             ba.scales[sf].data());
                continue;
            }
        }
        local[j] = composeTRSSwapped(jt.restQ.data(), jt.restT.data(), jt.restS.data());
    }
    QVector<quint8> done(nb, 0);
    int remaining = 0;
    for (int j = 0; j < nb; ++j) {
        const int p = sc.geo.skeleton[j].parent;
        if (p < 0 || p >= nb)  { animG[j] = local[j];                             done[j] = 1; }
        else if (p < j)        { animG[j] = RigMath::mat4mul(animG[p], local[j]); done[j] = 1; }
        else                   { ++remaining; }
    }
    while (remaining > 0) {
        int progressed = 0;
        for (int j = 0; j < nb; ++j) {
            if (done[j]) continue;
            const int p = sc.geo.skeleton[j].parent;
            if (p >= 0 && p < nb && done[p]) {
                animG[j] = RigMath::mat4mul(animG[p], local[j]);
                done[j] = 1; --remaining; ++progressed;
            }
        }
        if (!progressed) {
            for (int j = 0; j < nb; ++j) if (!done[j]) { animG[j] = local[j]; done[j] = 1; }
            remaining = 0;
        }
    }
}

float LegacySolver::cageMdScale(int ci) const
{
    if (ci < 0 || ci >= cages.size()) return 0.0f;
    const int si = cages[ci].simIdx;
    if (si < 0 || si >= sc.geo.clothSims.size()) return 0.0f;
    const ClothSim& cs = sc.geo.clothSims[si];
    const float trk = cs.tuned ? qBound(0.0f, cs.boneTrack, 1.0f)
                               : qBound(0.0f, P.boneTracking, 1.0f);
    const float tnorm = qBound(0.0f, (1.0f - trk) / 0.55f, 1.0f);
    const float span = (si < cageSpan.size() && cageSpan[si] > 1e-4f) ? cageSpan[si] : 1.0f;
    return P.maxDistance * qMax(0.05f, tnorm * tnorm) * span;
}

void LegacySolver::step()
{
    ++stepIndex;
    if (!m_built) { buildCapsules(); buildSpringBones(); }
    if (!m_animMovesBuilt) computeAnimMoves();
    global = animG;                        // fresh animated (rest) globals each frame
    poseCapsules();
    springBoneStep();                      // modifies global[] for cloth bones (pass 2)
}

} // namespace d4cloth
