#include "Solver.h"

#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace d4cloth {

using Mat4 = RigMath::Mat4;

void Solver::buildAuthored()
{
    m_authoredBuilt = true;
    acages.clear();
    // sbDriven/anchor state is repurposed for reporting: driven = "has a follower".
    sbDriven.fill(0, nb); sbAnchorPiece.fill(-1, nb); sbAnchorVert.fill(-1, nb);
    sbAnchorW.fill(0.0f, nb);

    for (int ci = 0; ci < cages.size(); ++ci) {
        const int si = cages[ci].simIdx;
        AuthoredCage ac;
        ac.docIdx = si;
        if (si < 0 || si >= sc.docs.size()) { acages.push_back(ac); continue; }
        const ClothDoc& d = sc.docs[si];
        const ClothSim& cs = sc.geo.clothSims[si];
        if (d.name != cs.name)
            qWarning("Solver: doc/sim name mismatch at %d ('%s' vs '%s')", si,
                     qPrintable(d.name), qPrintable(cs.name));
        ac.nv = d.vertexCount;
        const int nv = ac.nv;
        if (nv <= 0 || cs.bindVerts.size() < nv * 3) { acages.push_back(ac); continue; }

        // authored cage skinning
        if (d.driverInfluences.size() >= nv * 4 && d.weights.size() >= nv) {
            ac.inf.resize(nv); ac.w.resize(nv);
            for (int k = 0; k < nv; ++k) {
                for (int t = 0; t < 4; ++t) ac.inf[k][t] = d.driverInfluences[k * 4 + t];
                ac.w[k] = d.weights[k];
            }
        }
        ac.driverBone = (si < sc.docDriverBoneUnified.size()) ? sc.docDriverBoneUnified[si]
                                                              : QVector<int>();
        // constraints, filtered to real particles, stiffness by cluster class
        const Scene::DocTuning& tn = (si < sc.docTuning.size()) ? sc.docTuning[si]
                                                                 : Scene::DocTuning();
        const int pairs = d.constraintIndices.size() / 2;
        QVector<float> classStiff(pairs, tn.stretch);   // default if clusters absent
        auto mark = [&](const QVector<ClothDoc::Cluster>& cls, float st) {
            for (const ClothDoc::Cluster& c : cls)
                for (int e = c.start; e < c.end && e < pairs; ++e) classStiff[e] = st;
        };
        mark(d.warpClusters,  tn.stretch);
        mark(d.weftClusters,  tn.horiz);
        mark(d.shearClusters, tn.shear);
        mark(d.bendClusters,  tn.bend);
        for (int e = 0; e < pairs && e < d.constraintLengths.size(); ++e) {
            const int a = d.constraintIndices[e * 2], b = d.constraintIndices[e * 2 + 1];
            if (a >= nv || b >= nv || a == b) continue;   // padding / self-pairs dropped
            ac.con.push_back(a); ac.con.push_back(b);
            ac.conRest.push_back(d.constraintLengths[e]);
            ac.conStiff.push_back(qBound(0.05f, classStiff[e], 1.0f));
        }
        // tethers
        ac.tetherRoot.fill(-1, nv); ac.tetherLen.fill(0.0f, nv);
        if (d.kinematicRoots.size() >= nv && d.attachmentLengths.size() >= nv) {
            for (int k = 0; k < nv; ++k) {
                const quint16 r = d.kinematicRoots[k];
                if (r < nv) { ac.tetherRoot[k] = r; ac.tetherLen[k] = d.attachmentLengths[k]; }
            }
        }
        ac.pinned.fill(0, nv);
        for (int k = 0; k < nv && k < d.invMasses.size(); ++k)
            if (d.invMasses[k] == 0.0f) ac.pinned[k] = 1;

        ac.pos.fill(0.0f, nv * 3); ac.prev.fill(0.0f, nv * 3); ac.target.fill(0.0f, nv * 3);
        // usable = authored skinning fully resolved (all referenced drivers have a bone)
        ac.usable = !ac.inf.isEmpty() && !ac.driverBone.isEmpty();
        if (ac.usable)
            for (int k = 0; k < nv && ac.usable; ++k)
                for (int t = 0; t < 4; ++t) {
                    if (ac.w[k][t] <= 0.0f) continue;
                    const quint16 di = ac.inf[k][t];
                    if (di >= ac.driverBone.size() || ac.driverBone[di] < 0) { ac.usable = false; break; }
                }
        acages.push_back(ac);
        if (!ac.usable)
            qWarning("Solver: cage %d ('%s') authored skinning incomplete -> legacy borrowed "
                     "targets for this cage", ci, qPrintable(cs.name));
    }

    // follower map → repurposed sbDriven/sbAnchor state (only bones the legacy sim owns:
    // sbIsCloth; body-range chain bones the base rig owns stay rigid for now, documented)
    for (int ci = 0; ci < acages.size(); ++ci) {
        const int si = acages[ci].docIdx;
        if (si < 0 || si >= sc.docFollowerUnified.size()) continue;
        const QVector<int>& follow = sc.docFollowerUnified[si];
        for (int k = 0; k < follow.size(); ++k) {
            const int b = follow[k];
            if (b < 0 || b >= nb || !sbIsCloth[b]) continue;
            sbDriven[b] = 1; sbAnchorPiece[b] = ci; sbAnchorVert[b] = k; sbAnchorW[b] = 1.0f;
        }
    }
}

void Solver::step()
{
    ++stepIndex;
    if (!m_built) { buildCapsules(); buildSpringBones(); m_built = true; }
    if (!m_authoredBuilt) buildAuthored();
    if (!m_animMovesBuilt) computeAnimMoves();
    global = animG;
    poseCapsules();

    const QVector<ClothSim>& clothSims = sc.geo.clothSims;
    if (sbOrder.isEmpty() && acages.isEmpty()) return;
    if (!sbSeeded) {
        for (int j : sbOrder) {
            sbSimHead[j*3]=sbPrevHead[j*3]=animG[j][12];
            sbSimHead[j*3+1]=sbPrevHead[j*3+1]=animG[j][13];
            sbSimHead[j*3+2]=sbPrevHead[j*3+2]=animG[j][14];
        }
        sbSeeded = true;   // fall through: cages seed below on their first target build
    }

    const float keepBase = qBound(0.0f, P.damping, 0.999f);
    const float kMargin = P.collisionMargin;
    kDivergeMax = qMax(0.001f, sc.radius) * 0.28f;
    const int kIters = qBound(6, P.iterations * 4, 30);
    const int   subSteps = qBound(1, P.subSteps, 4);
    const float subF     = 1.0f / float(subSteps);
    const float rScale = P.capsuleRadius;
    const float fricK = qBound(0.0f, P.friction, 1.0f);
    contactCount = 0; worstPen = 0.0f; tetherHits = 0;
    cageLimitHits = 0; cageSafetyHits = 0; boneLimitHits = 0;
    worstPinnedTargetErr = 0.0f;

    // ── collision helpers (identical to the legacy port; see LegacySolver.cpp) ──
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

    // ── authored cage sim ──
    QVector<Mat4> pal(nb); QVector<quint8> palDone(nb, 0);
    auto palGet = [&](int b) -> const Mat4& {
        if (!palDone[b]) { pal[b] = RigMath::mat4mul(animG[b], sc.geo.skeleton[b].inverseBind); palDone[b] = 1; }
        return pal[b];
    };
    for (int ci = 0; ci < acages.size(); ++ci) {
        AuthoredCage& ac = acages[ci];
        const int nv = ac.nv;
        if (nv <= 0) continue;
        const ClothSim& cs = clothSims[cages[ci].simIdx];
        const Scene::DocTuning& tn = (ac.docIdx < sc.docTuning.size()) ? sc.docTuning[ac.docIdx]
                                                                        : Scene::DocTuning();
        // targets — authored skinning (fallback: legacy borrowed skinning if unresolved)
        for (int k = 0; k < nv; ++k) {
            const float* bp = cs.bindVerts.constData() + k*3;
            float tx=0, ty=0, tz=0, ws=0;
            if (ac.usable) {
                for (int t = 0; t < 4; ++t) {
                    const float w = ac.w[k][t]; if (w <= 0.0f) continue;
                    const int b = ac.driverBone[ac.inf[k][t]];
                    const Mat4& m = palGet(b);
                    tx += w*(m[0]*bp[0]+m[4]*bp[1]+m[8]*bp[2] +m[12]);
                    ty += w*(m[1]*bp[0]+m[5]*bp[1]+m[9]*bp[2] +m[13]);
                    tz += w*(m[2]*bp[0]+m[6]*bp[1]+m[10]*bp[2]+m[14]);
                    ws += w;
                }
            } else {
                const LegacySolver::CageRt& rt = cages[ci];
                for (int t = 0; t < 4; ++t) {
                    const float w = rt.W[k][t]; if (w <= 0.0f) continue;
                    const int b = rt.J[k][t];   if (b < 0 || b >= nb) continue;
                    const Mat4& m = palGet(b);
                    tx += w*(m[0]*bp[0]+m[4]*bp[1]+m[8]*bp[2] +m[12]);
                    ty += w*(m[1]*bp[0]+m[5]*bp[1]+m[9]*bp[2] +m[13]);
                    tz += w*(m[2]*bp[0]+m[6]*bp[1]+m[10]*bp[2]+m[14]);
                    ws += w;
                }
            }
            float* T = ac.target.data() + k*3;
            if (ws > 1e-6f) { T[0]=tx/ws; T[1]=ty/ws; T[2]=tz/ws; }
            else            { T[0]=bp[0]; T[1]=bp[1]; T[2]=bp[2]; }
        }
        // driver-table validation: pinned targets should agree with the legacy borrowed
        // skinning (which is body-derived and correct for pinned collar verts).
        if (ac.usable && cages[ci].seeded == false) { /* once-only cost is negligible; run every step anyway */ }
        for (int k = 0; k < nv; ++k) {
            if (!ac.pinned[k] || !ac.usable) continue;
            const LegacySolver::CageRt& rt = cages[ci];
            const float* bp = cs.bindVerts.constData() + k*3;
            float lx=0, ly=0, lz=0, ws=0;
            for (int t = 0; t < 4; ++t) {
                const float w = rt.W[k][t]; if (w <= 0.0f) continue;
                const int b = rt.J[k][t];   if (b < 0 || b >= nb) continue;
                const Mat4& m = palGet(b);
                lx += w*(m[0]*bp[0]+m[4]*bp[1]+m[8]*bp[2] +m[12]);
                ly += w*(m[1]*bp[0]+m[5]*bp[1]+m[9]*bp[2] +m[13]);
                lz += w*(m[2]*bp[0]+m[6]*bp[1]+m[10]*bp[2]+m[14]);
                ws += w;
            }
            if (ws > 1e-6f) {
                const float* T = ac.target.constData() + k*3;
                const float dx=T[0]-lx/ws, dy=T[1]-ly/ws, dz=T[2]-lz/ws;
                worstPinnedTargetErr = qMax(worstPinnedTargetErr,
                                            std::sqrt(dx*dx+dy*dy+dz*dz));
            }
        }
        if (!ac.seeded) { ac.pos = ac.target; ac.prev = ac.target; ac.seeded = true; continue; }

        const float keep = keepBase * (1.0f - qBound(0.0f, tn.dragFactor, 1.0f) * 0.1f);
        const float stiffPull = qBound(0.0f, P.boneStiffness, 1.0f);
        for (int ss = 0; ss < subSteps; ++ss) {
            const float gj = tn.gravPerStep * subF * (P.gravity == 0.0f ? 1.0f : 0.0f)
                           + P.gravity * subF;   // --param gravity overrides authored
            const float wx = tn.windX * subF, wy = tn.windY * subF, wz = tn.windZ * subF;
            for (int k = 0; k < nv; ++k) {
                float* Pp = ac.pos.data()+k*3; float* Q = ac.prev.data()+k*3;
                const float* T = ac.target.constData()+k*3;
                if (ac.pinned[k]) { Q[0]=Pp[0];Q[1]=Pp[1];Q[2]=Pp[2]; Pp[0]=T[0];Pp[1]=T[1];Pp[2]=T[2]; continue; }
                float n0=Pp[0]+(Pp[0]-Q[0])*keep + wx,
                      n1=Pp[1]+(Pp[1]-Q[1])*keep + gj + wy,
                      n2=Pp[2]+(Pp[2]-Q[2])*keep + wz;
                n0+=(T[0]-n0)*stiffPull; n1+=(T[1]-n1)*stiffPull; n2+=(T[2]-n2)*stiffPull;
                clampStep(Pp, n0, n1, n2);
                Q[0]=Pp[0];Q[1]=Pp[1];Q[2]=Pp[2]; Pp[0]=n0;Pp[1]=n1;Pp[2]=n2;
                if (watchedPart(ci, k)) tr("integrate", QStringLiteral("p:%1/%2").arg(ci).arg(k), Pp);
            }
            for (int it = 0; it < kIters; ++it) {
                // authored constraint network, per-class stiffness
                for (int e = 0; e * 2 + 1 < ac.con.size(); ++e) {
                    const int a = ac.con[e*2], b = ac.con[e*2+1];
                    float* A = ac.pos.data()+a*3; float* B = ac.pos.data()+b*3;
                    float dx=B[0]-A[0], dy=B[1]-A[1], dz=B[2]-A[2];
                    const float len=std::sqrt(dx*dx+dy*dy+dz*dz); if (len < 1e-6f) continue;
                    const float wa=ac.pinned[a]?0.f:1.f, wb=ac.pinned[b]?0.f:1.f, wsum=wa+wb;
                    if (wsum < 1e-6f) continue;
                    const float st = ac.conStiff[e];
                    const float diff=(len-ac.conRest[e])/len * st,
                                sa=diff*wa/wsum, sb=diff*wb/wsum;
                    A[0]+=dx*sa;A[1]+=dy*sa;A[2]+=dz*sa; B[0]-=dx*sb;B[1]-=dy*sb;B[2]-=dz*sb;
                }
                // authored TETHER: rope limit to the kinematic root's current position
                for (int k = 0; k < nv; ++k) {
                    if (ac.pinned[k]) continue;
                    const int r = ac.tetherRoot[k];
                    if (r < 0) continue;
                    float* Pp = ac.pos.data()+k*3;
                    const float* R = ac.pos.constData()+r*3;   // pinned root == its target
                    const float dx=Pp[0]-R[0], dy=Pp[1]-R[1], dz=Pp[2]-R[2];
                    const float d=std::sqrt(dx*dx+dy*dy+dz*dz);
                    const float lim = ac.tetherLen[k];
                    if (d > lim && d > 1e-8f) {
                        const float s=lim/d;
                        Pp[0]=R[0]+dx*s; Pp[1]=R[1]+dy*s; Pp[2]=R[2]+dz*s;
                        if (it == kIters-1) ++tetherHits;
                        if (watchedPart(ci, k))
                            tr("tether", QStringLiteral("p:%1/%2").arg(ci).arg(k), Pp,
                               QStringLiteral("lim=%1 d=%2").arg(lim).arg(d));
                    }
                }
                for (int k = 0; k < nv; ++k) {
                    if (ac.pinned[k]) continue;
                    collide(ac.pos.data()+k*3, ac.prev.data()+k*3, /*fixVel=*/false);
                }
            }
            for (int k = 0; k < nv; ++k) {
                if (ac.pinned[k]) continue;
                collide(ac.pos.data()+k*3, ac.prev.data()+k*3, /*fixVel=*/true);
            }
            // NaN net only — the tether is the real bound; clamps are not a mechanism here.
            for (int k = 0; k < nv; ++k) {
                float* Pp = ac.pos.data()+k*3; float* Q = ac.prev.data()+k*3;
                const float* T = ac.target.constData()+k*3;
                if (!std::isfinite(Pp[0]+Pp[1]+Pp[2])) {
                    Pp[0]=T[0];Pp[1]=T[1];Pp[2]=T[2]; Q[0]=T[0];Q[1]=T[1];Q[2]=T[2];
                    ++cageSafetyHits;
                }
            }
        }
    }

    // ── drive followed bones: the bone head IS its particle ──
    for (int j : sbOrder) {
        if (!sbDriven[j]) continue;
        const int ci = sbAnchorPiece[j], kv = sbAnchorVert[j];
        if (ci < 0 || ci >= acages.size() || kv < 0) continue;
        const AuthoredCage& ac = acages[ci];
        if (!ac.seeded || (kv+1)*3 > ac.pos.size()) continue;
        float* S=sbSimHead.data()+j*3; float* R=sbPrevHead.data()+j*3;
        R[0]=S[0];R[1]=S[1];R[2]=S[2];
        S[0]=ac.pos[kv*3]; S[1]=ac.pos[kv*3+1]; S[2]=ac.pos[kv*3+2];
        if (watchedBone(j)) tr("follower-drive", QStringLiteral("b:%1").arg(j), S,
                               QStringLiteral("cage=%1 vert=%2").arg(ci).arg(kv));
    }
    // ── unfollowed cloth bones: pre-cage-era pure spring path ──
    auto held = [&](int j) { return sbPin[j] || sbDriven[j]; };
    for (int j : sbOrder) {
        if (held(j)) { if (sbPin[j]) { float* S=sbSimHead.data()+j*3; float* R=sbPrevHead.data()+j*3;
                                        R[0]=S[0];R[1]=S[1];R[2]=S[2];
                                        S[0]=animG[j][12];S[1]=animG[j][13];S[2]=animG[j][14]; } continue; }
        const bool animated = hasAnim() && j < sbAnimMoves.size() && sbAnimMoves[j];
        float* S=sbSimHead.data()+j*3; float* R=sbPrevHead.data()+j*3;
        if (animated) { R[0]=S[0];R[1]=S[1];R[2]=S[2]; S[0]=animG[j][12];S[1]=animG[j][13];S[2]=animG[j][14]; continue; }
        const float ah0=animG[j][12], ah1=animG[j][13], ah2=animG[j][14];
        const float stiffJ = qBound(0.0f, P.boneStiffness, 1.0f);
        float n0=S[0]+(S[0]-R[0])*keepBase, n1=S[1]+(S[1]-R[1])*keepBase + P.gravity, n2=S[2]+(S[2]-R[2])*keepBase;
        n0+=(ah0-n0)*stiffJ; n1+=(ah1-n1)*stiffJ; n2+=(ah2-n2)*stiffJ;
        clampStep(S, n0, n1, n2);
        R[0]=S[0];R[1]=S[1];R[2]=S[2]; S[0]=n0;S[1]=n1;S[2]=n2;
    }
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
        // motion limit for unfollowed bones: the v2b-matched authored attachLen is an
        // ABSOLUTE tether length (FINDINGS F1) — applied unscaled around the anim pose.
        for (int j : sbOrder) if (!held(j)) {
            const float md = sbAttach[j];
            float* S=sbSimHead.data()+j*3;
            const float dx=S[0]-animG[j][12], dy=S[1]-animG[j][13], dz=S[2]-animG[j][14];
            const float d=std::sqrt(dx*dx+dy*dy+dz*dz);
            if (d > md && d > 1e-8f) {
                const float s=md/d;
                S[0]=animG[j][12]+dx*s; S[1]=animG[j][13]+dy*s; S[2]=animG[j][14]+dz*s;
                if (it == kIters-1) ++boneLimitHits;
            }
        }
        for (int j : sbOrder) if (!held(j))
            collide(sbSimHead.data()+j*3, sbPrevHead.data()+j*3, /*fixVel=*/false);
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
    // NaN net + divergence accounting (report-only bound for the unfollowed path)
    {
        divergeClamped = 0; worstDiv = 0.0f; worstDivBone = -1;
        for (int j : sbOrder) {
            if (sbPin[j]) continue;
            float* S = sbSimHead.data() + j*3;
            float* R = sbPrevHead.data() + j*3;
            const float ax = animG[j][12], ay = animG[j][13], az = animG[j][14];
            if (!std::isfinite(S[0]+S[1]+S[2])) { S[0]=ax;S[1]=ay;S[2]=az; R[0]=ax;R[1]=ay;R[2]=az; continue; }
            const float dx = S[0]-ax, dy = S[1]-ay, dz = S[2]-az;
            const float d = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (d > worstDiv) { worstDiv = d; worstDivBone = j; }
            if (!sbDriven[j] && d > kDivergeMax) {   // unfollowed bones keep the legacy net
                const float s = kDivergeMax/d;
                const float vx=S[0]-R[0], vy=S[1]-R[1], vz=S[2]-R[2];
                S[0]=ax+dx*s; S[1]=ay+dy*s; S[2]=az+dz*s;
                R[0]=S[0]-vx*0.25f; R[1]=S[1]-vy*0.25f; R[2]=S[2]-vz*0.25f;
                ++divergeClamped;
            }
        }
    }
    // ── pass 2: identical swing reconstruction (base-class geometry, same code path) ──
    // (duplicated from the legacy port so both solvers produce comparable globals)
    {
        QVector<std::array<float,9>> swing(nb);
        auto rotFromTo = [](const float a[3], const float b[3], float r[9]) {
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
        };
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
            auto col = [&](int cix, int outBase) {
                const float v0=a[cix*4+0], v1=a[cix*4+1], v2=a[cix*4+2];
                m[outBase+0]=r[0]*v0+r[3]*v1+r[6]*v2;
                m[outBase+1]=r[1]*v0+r[4]*v1+r[7]*v2;
                m[outBase+2]=r[2]*v0+r[5]*v1+r[8]*v2; m[outBase+3]=0.0f;
            };
            col(0,0); col(1,4); col(2,8);
            m[12]=sbSimHead[j*3]; m[13]=sbSimHead[j*3+1]; m[14]=sbSimHead[j*3+2]; m[15]=1.0f;
            global[j]=m;
        }
    }
}

} // namespace d4cloth
