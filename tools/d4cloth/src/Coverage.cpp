#include "Coverage.h"

#include "model/ModelGeometry.h"
#include "model/RigMath.h"

#include <QHash>
#include <QSet>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>

namespace d4cloth {
namespace {

// ── consumption status of each array in the APP as of 2026-07-25 ─────────────────────
// Audited from ModelParser::parseClothCapsules + GLModelWidget (buildSpringBones /
// springBoneStep / buildClothSim). Update when the app changes.
struct Consumption { const char* name; const char* status; };
constexpr Consumption kConsumption[] = {
    {"ptBindVertices",       "CONSUMED  (cage rest positions: buildSpringBones v2b match, cage targets, anchors)"},
    {"ptBindNormals",        "UNPARSED"},
    {"ptInvMasses",          "CONSUMED  (pins: cage kinematics + bone m_sbPin)"},
    {"ptBlendWeights",       "UNPARSED"},
    {"ptAnimBlendFractions", "UNPARSED"},
    {"ptDeltaFrames",        "UNPARSED  (element type unknown)"},
    {"ptLevels",             "UNPARSED"},
    {"ptAttachmentLengths",  "PARTIAL   (parsed ONLY when size/4==vertexCount, else silently empty -> synthesized fallback; consumed as motion limit al*maxDistance*tnorm^2*cageSpan)"},
    {"ptParentIndices",      "UNPARSED"},
    {"ptKinematicRoots",     "UNPARSED"},
    {"ptTangentIndices",     "UNPARSED"},
    {"ptWeights",            "UNPARSED"},
    {"ptDriverInfluences",   "UNPARSED"},
    {"ptFollowerIndices",    "UNPARSED"},
    {"ptTriangles",          "PARSED-UNUSED (stored in ClothSim::triangles; no runtime reader found)"},
    {"ptConstraintIndices",  "CONSUMED  (distance-constraint network, cage + bone paths)"},
    {"ptConstraintLengths",  "CONSUMED  (rest lengths)"},
    {"unk_8ecbb2b",          "UNPARSED  (unknown u16 array)"},
    {"unk_9f71907",          "UNPARSED  (unknown f32 array)"},
    {"ptWarpClusters",       "UNPARSED  (per-class stiffness never applied)"},
    {"ptWeftClusters",       "UNPARSED"},
    {"ptShearClusters",      "UNPARSED"},
    {"ptBendClusters",       "UNPARSED"},
    {"ptCapsuleDefs",        "CONSUMED  (collision capsules; fields scale/solver/hide UNREAD)"},
    {"ptPlaneDefs",          "CONSUMED  (plane colliders; normal axis DERIVED, not authored)"},
    {"ptDriverBindPose",     "UNPARSED  (driver frames — candidate authored cage->bone/mesh driving)"},
    {"ptDriverMap",          "UNPARSED"},
};

const char* consumption(const QString& n)
{
    for (const Consumption& c : kConsumption)
        if (n == QLatin1String(c.name)) return c.status;
    return "?";
}

struct Stats { double mn = 0, mx = 0, mean = 0; int n = 0; int nan = 0; };
Stats stats(const QVector<float>& v)
{
    Stats s; s.n = v.size();
    if (v.isEmpty()) return s;
    s.mn = std::numeric_limits<double>::max(); s.mx = -s.mn;
    double sum = 0; int cnt = 0;
    for (float f : v) {
        if (!std::isfinite(f)) { ++s.nan; continue; }
        s.mn = qMin(s.mn, double(f)); s.mx = qMax(s.mx, double(f)); sum += f; ++cnt;
    }
    if (cnt) s.mean = sum / cnt; else { s.mn = s.mx = 0; }
    return s;
}
Stats statsU16(const QVector<quint16>& v)
{
    Stats s; s.n = v.size();
    if (v.isEmpty()) return s;
    s.mn = 65535; s.mx = 0; double sum = 0;
    for (quint16 u : v) { s.mn = qMin(s.mn, double(u)); s.mx = qMax(s.mx, double(u)); sum += u; }
    s.mean = sum / v.size();
    return s;
}

double dist3(const std::array<float,4>& a, const std::array<float,4>& b)
{
    const double dx = a[0]-b[0], dy = a[1]-b[1], dz = a[2]-b[2];
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

// Pearson correlation of two equal-length series.
double pearson(const QVector<double>& a, const QVector<double>& b)
{
    const int n = qMin(a.size(), b.size());
    if (n < 3) return 0.0;
    double ma = 0, mb = 0;
    for (int i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= n; mb /= n;
    double num = 0, da = 0, db = 0;
    for (int i = 0; i < n; ++i) {
        num += (a[i]-ma)*(b[i]-mb); da += (a[i]-ma)*(a[i]-ma); db += (b[i]-mb)*(b[i]-mb);
    }
    return (da > 1e-12 && db > 1e-12) ? num / std::sqrt(da*db) : 0.0;
}

QString hexDump(const QByteArray& b, int maxBytes)
{
    QString out;
    const int n = qMin(int(b.size()), maxBytes);
    for (int i = 0; i < n; i += 16) {
        out += QStringLiteral("      %1  ").arg(i, 6, 16, QLatin1Char('0'));
        QString ascii;
        for (int j = i; j < qMin(i + 16, n); ++j) {
            out += QStringLiteral("%1 ").arg(quint8(b[j]), 2, 16, QLatin1Char('0'));
            const char c = b[j];
            ascii += (c >= 32 && c < 127) ? QLatin1Char(c) : QLatin1Char('.');
        }
        out += QStringLiteral("  |%1|\n").arg(ascii);
    }
    if (b.size() > maxBytes) out += QStringLiteral("      ... (%1 more bytes)\n").arg(b.size() - maxBytes);
    return out;
}

// Interpret the same bytes as f32 and print a few — helps identify unknown element types.
QString peekF32(const QByteArray& b, int count)
{
    QString out;
    const int n = qMin(int(b.size() / 4), count);
    for (int i = 0; i < n; ++i) {
        float f; std::memcpy(&f, b.constData() + i * 4, 4);
        out += QStringLiteral("%1 ").arg(double(f), 0, 'g', 5);
    }
    return out;
}

} // namespace

QString coverageReport(const ClothDoc& doc, const ModelGeometry* geo)
{
    QString r;
    auto line = [&](const QString& s) { r += s + QLatin1Char('\n'); };

    line(QStringLiteral("=== ClothData '%1'  (piece '%2', block @%3 in %4) ===")
             .arg(doc.name, doc.pieceName)
             .arg(doc.blockOffset)
             .arg(doc.blockInPayload ? QStringLiteral("payload") : QStringLiteral("meta")));

    // ── header ──
    line(QStringLiteral("  header: vertexCount=%1 vertexCapacity=%2 kinematicCount=%3 triangleCount=%4")
             .arg(doc.vertexCount).arg(doc.vertexCapacity).arg(doc.kinematicCount).arg(doc.triangleCount));
    line(QStringLiteral("          clusters warp=%1 weft=%2 shear=%3 bend=%4  constraintCount=%5")
             .arg(doc.warpClusterCount).arg(doc.weftClusterCount)
             .arg(doc.shearClusterCount).arg(doc.bendClusterCount).arg(doc.constraintCount));
    line(QStringLiteral("          maxLevel=%1 boneCount=%2 driverCount=%3 capsuleCount=%4 planeCount=%5")
             .arg(doc.maxLevel).arg(doc.boneCount).arg(doc.driverCount)
             .arg(doc.capsuleCount).arg(doc.planeCount));
    line(QStringLiteral("          density=%1 [unitless?]  unk_9460e91=%2")
             .arg(double(doc.density), 0, 'g', 6).arg(doc.unk_9460e91));

    // ── per-array table ──
    line(QStringLiteral("  arrays (27):"));
    const int nv = doc.vertexCount;
    for (const ArraySlot& s : doc.arrays) {
        QString sz;
        if (!s.present) sz = QStringLiteral("ABSENT");
        else {
            sz = QStringLiteral("%1 B").arg(s.dataSize);
            QStringList div;
            if (nv > 0 && s.dataSize % nv == 0) div << QStringLiteral("%1 B/vert").arg(s.dataSize / nv);
            if (doc.constraintCount > 0 && s.dataSize % doc.constraintCount == 0)
                div << QStringLiteral("%1 B/constraint").arg(s.dataSize / doc.constraintCount);
            if (doc.driverCount > 0 && s.dataSize % doc.driverCount == 0)
                div << QStringLiteral("%1 B/driver").arg(s.dataSize / doc.driverCount);
            if (doc.kinematicCount > 0 && s.dataSize % doc.kinematicCount == 0)
                div << QStringLiteral("%1 B/kinematic").arg(s.dataSize / doc.kinematicCount);
            if (doc.boneCount > 0 && s.dataSize % doc.boneCount == 0)
                div << QStringLiteral("%1 B/bone").arg(s.dataSize / doc.boneCount);
            if (!div.isEmpty()) sz += QStringLiteral("  [") + div.join(QStringLiteral(", ")) + QLatin1Char(']');
        }
        line(QStringLiteral("    %1 %2").arg(s.name, -22).arg(sz));
        line(QStringLiteral("      app: %1").arg(QLatin1String(consumption(s.name))));
    }

    // ── numeric stats for the typed views ──
    line(QStringLiteral("  values:"));
    auto fstat = [&](const char* n, const QVector<float>& v, const char* unitNote) {
        if (v.isEmpty()) return;
        const Stats s = stats(v);
        line(QStringLiteral("    %1 n=%2 min=%3 max=%4 mean=%5 %6%7")
                 .arg(QLatin1String(n), -22).arg(s.n)
                 .arg(s.mn, 0, 'g', 6).arg(s.mx, 0, 'g', 6).arg(s.mean, 0, 'g', 6)
                 .arg(QLatin1String(unitNote))
                 .arg(s.nan ? QStringLiteral("  NAN=%1 <<<").arg(s.nan) : QString()));
    };
    auto ustat = [&](const char* n, const QVector<quint16>& v, int validMax, const char* refName) {
        if (v.isEmpty()) return;
        const Stats s = statsU16(v);
        int oob = 0;
        if (validMax > 0) for (quint16 u : v) if (u != 0xFFFF && u >= validMax) ++oob;
        line(QStringLiteral("    %1 n=%2 min=%3 max=%4 (valid range: <%5 = %6)%7")
                 .arg(QLatin1String(n), -22).arg(s.n).arg(int(s.mn)).arg(int(s.mx))
                 .arg(validMax).arg(QLatin1String(refName))
                 .arg(oob ? QStringLiteral("  OUT-OF-RANGE=%1 <<<").arg(oob) : QString()));
    };
    fstat("invMasses",          doc.invMasses,          "[1/mass]");
    fstat("blendWeights",       doc.blendWeights,       "[?]");
    fstat("animBlendFractions", doc.animBlendFractions, "[?]");
    fstat("attachmentLengths",  doc.attachmentLengths,  "[UNITS UNDER TEST: normalized 0..1 vs world]");
    fstat("constraintLengths",  doc.constraintLengths,  "[wu, bind-space]");
    fstat("unk_9f71907",        doc.unkF576,            "[?]");
    ustat("levels",             doc.levels,             0,               "-");
    ustat("parentIndices",      doc.parentIndices,      doc.vertexCount, "vertexCount");
    ustat("kinematicRoots",     doc.kinematicRoots,     doc.vertexCount, "vertexCount");
    ustat("tangentIndices",     doc.tangentIndices,     doc.vertexCount, "vertexCount");
    ustat("driverInfluences",   doc.driverInfluences,   qMax(doc.driverCount, 1), "driverCount");
    ustat("followerIndices",    doc.followerIndices,    doc.vertexCount, "vertexCount");
    ustat("driverMap",          doc.driverMap,          qMax(doc.vertexCount, doc.boneCount), "max(vertexCount,boneCount)");
    ustat("triangles",          doc.triangles,          doc.vertexCount, "vertexCount");
    ustat("constraintIndices",  doc.constraintIndices,  doc.vertexCount, "vertexCount");
    ustat("unk_8ecbb2b",        doc.unkU560,            0,               "-");

    // ── consistency checks ──
    line(QStringLiteral("  checks:"));
    { int pinned = 0, pinnedPad = 0;
      for (int k = 0; k < doc.invMasses.size(); ++k)
          if (doc.invMasses[k] == 0.0f) { if (k < nv) ++pinned; else ++pinnedPad; }
      line(QStringLiteral("    pinned(invMass==0, real verts)=%1 vs kinematicCount=%2 %3%4")
               .arg(pinned).arg(doc.kinematicCount)
               .arg(pinned == doc.kinematicCount ? QStringLiteral("MATCH")
                                                 : QStringLiteral("MISMATCH <<<"))
               .arg(pinnedPad ? QStringLiteral("  (+%1 pinned padding verts)").arg(pinnedPad) : QString())); }
    if (!doc.constraintIndices.isEmpty()) {
        const int pairs = doc.constraintIndices.size() / 2;
        line(QStringLiteral("    constraint pairs=%1 vs constraintCount=%2 %3  lengths=%4 %5")
                 .arg(pairs).arg(doc.constraintCount)
                 .arg(pairs == doc.constraintCount ? QStringLiteral("MATCH") : QStringLiteral("MISMATCH <<<"))
                 .arg(doc.constraintLengths.size())
                 .arg(doc.constraintLengths.size() == pairs ? QStringLiteral("MATCH")
                                                            : QStringLiteral("MISMATCH <<<")));
        // Rest lengths vs bind distances — a systematic offset flags a space/units bug.
        if (!doc.bindVertices.isEmpty() && !doc.constraintLengths.isEmpty()) {
            double worst = 0, sum = 0; int n = 0;
            for (int e = 0; e < pairs && e < doc.constraintLengths.size(); ++e) {
                const int a = doc.constraintIndices[e*2], b = doc.constraintIndices[e*2+1];
                if (a >= nv || b >= nv) continue;
                const double d = dist3(doc.bindVertices[a], doc.bindVertices[b]);
                const double res = d - double(doc.constraintLengths[e]);
                worst = qMax(worst, std::abs(res)); sum += res; ++n;
            }
            if (n) line(QStringLiteral("    restLen vs bindDist [wu]: meanResidual=%1 worst=%2 over %3 pairs %4")
                            .arg(sum / n, 0, 'g', 4).arg(worst, 0, 'g', 4).arg(n)
                            .arg(worst > 0.02 ? QStringLiteral("<<< check space/units") : QString()));
        }
    }

    // ── H3: do the cluster ranges partition the constraint set? ──
    if (!doc.warpClusters.isEmpty() || !doc.weftClusters.isEmpty()
        || !doc.shearClusters.isEmpty() || !doc.bendClusters.isEmpty()) {
        line(QStringLiteral("  clusters (H3 — per-class constraint ranges):"));
        auto clusterLine = [&](const char* n, const QVector<ClothDoc::Cluster>& cs) {
            if (cs.isEmpty()) return;
            QString ranges; int total = 0; int lo = INT_MAX, hi = -1; bool asc = true; int prevEnd = -1;
            for (const ClothDoc::Cluster& c : cs) {
                ranges += QStringLiteral("[%1..%2) ").arg(c.start).arg(c.end);
                total += qMax(0, int(c.end) - int(c.start));
                lo = qMin(lo, int(c.start)); hi = qMax(hi, int(c.end));
                if (int(c.start) < prevEnd) asc = false;
                prevEnd = c.end;
            }
            // Per-class rest-length stats, if the ranges index into constraintLengths.
            QString lenStat;
            if (!doc.constraintLengths.isEmpty() && hi <= doc.constraintLengths.size()) {
                QVector<float> lens;
                for (const ClothDoc::Cluster& c : cs)
                    for (int e = c.start; e < c.end && e < doc.constraintLengths.size(); ++e)
                        lens.push_back(doc.constraintLengths[e]);
                const Stats s = stats(lens);
                lenStat = QStringLiteral("  restLen[wu] min=%1 max=%2 mean=%3")
                              .arg(s.mn, 0, 'g', 4).arg(s.mx, 0, 'g', 4).arg(s.mean, 0, 'g', 4);
            }
            line(QStringLiteral("    %1 %2 ranges, %3 constraints, span [%4..%5)%6 %7")
                     .arg(QLatin1String(n), -14).arg(cs.size()).arg(total).arg(lo).arg(hi)
                     .arg(asc ? QString() : QStringLiteral(" NON-MONOTONIC"))
                     .arg(lenStat));
            if (ranges.size() < 300) line(QStringLiteral("      %1").arg(ranges));
        };
        clusterLine("warp",  doc.warpClusters);
        clusterLine("weft",  doc.weftClusters);
        clusterLine("shear", doc.shearClusters);
        clusterLine("bend",  doc.bendClusters);
        int total = 0;
        for (const auto* cs : { &doc.warpClusters, &doc.weftClusters, &doc.shearClusters, &doc.bendClusters })
            for (const ClothDoc::Cluster& c : *cs) total += qMax(0, int(c.end) - int(c.start));
        line(QStringLiteral("    sum(cluster spans)=%1 vs constraintCount=%2 %3")
                 .arg(total).arg(doc.constraintCount)
                 .arg(total == doc.constraintCount ? QStringLiteral("PARTITION CONFIRMED")
                                                   : QStringLiteral("NOT A PARTITION")));
    }

    // ── padding / extra particles (vertexCount..capacity-1) ──
    const int cap = qMax(doc.vertexCount, doc.vertexCapacity);
    if (cap > nv && !doc.bindVertices.isEmpty()) {
        line(QStringLiteral("  extra particles beyond vertexCount (%1..%2):").arg(nv).arg(cap - 1));
        for (int k = nv; k < cap && k < doc.bindVertices.size(); ++k) {
            int refs = 0;
            for (quint16 ci : doc.constraintIndices) if (ci == k) ++refs;
            line(QStringLiteral("    v%1: bind=(%2 %3 %4) invMass=%5 attachLen=%6 constraintRefs=%7")
                     .arg(k)
                     .arg(double(doc.bindVertices[k][0]), 0, 'g', 4)
                     .arg(double(doc.bindVertices[k][1]), 0, 'g', 4)
                     .arg(double(doc.bindVertices[k][2]), 0, 'g', 4)
                     .arg(double(doc.invMasses.value(k, -1)), 0, 'g', 4)
                     .arg(double(doc.attachmentLengths.value(k, -1)), 0, 'g', 4)
                     .arg(refs));
        }
    }

    // ── H2a: cage skinning — driverInfluences (4 x u16/vert) + weights (vec4/vert) ──
    if (!doc.driverInfluences.isEmpty() && doc.driverInfluences.size() % 4 == 0) {
        int oobInf = 0; quint16 maxInf = 0;
        for (quint16 di : doc.driverInfluences) {
            if (di != 0xFFFF) { maxInf = qMax(maxInf, di); if (doc.driverCount > 0 && di >= doc.driverCount) ++oobInf; }
        }
        double wsMin = 1e30, wsMax = -1e30;
        for (int k = 0; k < nv && k < doc.weights.size(); ++k) {
            const double ws = doc.weights[k][0] + doc.weights[k][1] + doc.weights[k][2] + doc.weights[k][3];
            wsMin = qMin(wsMin, ws); wsMax = qMax(wsMax, ws);
        }
        line(QStringLiteral("  cage skinning (H2a): driverInfluences=4xu16/vert maxIdx=%1 vs driverCount=%2%3; "
                            "weight row sums [%4..%5]%6")
                 .arg(maxInf).arg(doc.driverCount)
                 .arg(oobInf ? QStringLiteral(" OUT-OF-RANGE=%1 <<<").arg(oobInf) : QString())
                 .arg(wsMin, 0, 'g', 4).arg(wsMax, 0, 'g', 4)
                 .arg((doc.weights.isEmpty()) ? QStringLiteral(" (no weights)") : QString()));
    }

    // ── H2b: followerIndices — the authored particle→bone driving map ──
    if (!doc.followerIndices.isEmpty()) {
        int none = 0, valid = 0; quint16 mn = 0xFFFF, mx = 0;
        for (int k = 0; k < doc.followerIndices.size(); ++k) {
            const quint16 f = doc.followerIndices[k];
            if (f == 0xFFFF) { ++none; continue; }
            ++valid; mn = qMin(mn, f); mx = qMax(mx, f);
        }
        line(QStringLiteral("  followers (H2b): %1 entries — %2 with a follower bone [%3..%4], %5 without "
                            "(0xFFFF: pinned particles and bone-less fabric verts); kinematicCount=%6")
                 .arg(doc.followerIndices.size()).arg(valid).arg(mn).arg(mx).arg(none)
                 .arg(doc.kinematicCount));
        if (geo && !geo->skeleton.isEmpty() && !doc.bindVertices.isEmpty()) {
            // Distance from each followed bone's rest position to its cage vert — the authored
            // version of the app's 3cm/10cm nearest-anchor search. Both in D4-native space.
            const int nb = geo->skeleton.size();
            QVector<RigMath::Mat4> restG(nb);
            for (int j = 0; j < nb; ++j) {
                const ModelJoint& jt = geo->skeleton[j];
                const RigMath::Mat4 L = RigMath::composeTRS(jt.restT, jt.restQ, jt.restS);
                const int p = jt.parent;
                restG[j] = (p >= 0 && p < j) ? RigMath::mat4mul(restG[p], L) : L;
            }
            double worst = 0, sum = 0; int n = 0, over3cm = 0;
            QSet<quint16> seen; int dupBones = 0;
            for (int k = 0; k < doc.followerIndices.size() && k < doc.bindVertices.size(); ++k) {
                const quint16 f = doc.followerIndices[k];
                if (f == 0xFFFF || f >= nb) continue;
                if (seen.contains(f)) ++dupBones; else seen.insert(f);
                const double dx = restG[f][12] - doc.bindVertices[k][0];
                const double dy = restG[f][13] - doc.bindVertices[k][1];
                const double dz = restG[f][14] - doc.bindVertices[k][2];
                const double d = std::sqrt(dx*dx + dy*dy + dz*dz);
                worst = qMax(worst, d); sum += d; ++n; if (d > 0.03) ++over3cm;
            }
            if (n) line(QStringLiteral("    follower bone rest ↔ cage vert distance [wu]: mean=%1 worst=%2 "
                                       ">3cm=%3/%4  bonesFollowedTwice=%5")
                            .arg(sum / n, 0, 'g', 4).arg(worst, 0, 'g', 4)
                            .arg(over3cm).arg(n).arg(dupBones));
        }
    }

    // ── H2c: driverMap (per-bone, boneCount entries) ──
    if (!doc.driverMap.isEmpty()) {
        const Stats s = statsU16(doc.driverMap);
        line(QStringLiteral("  driverMap (H2c): n=%1 (boneCount=%2) min=%3 max=%4 — candidate bone→particle/driver map")
                 .arg(doc.driverMap.size()).arg(doc.boneCount).arg(int(s.mn)).arg(int(s.mx)));
        QString head;
        for (int i = 0; i < qMin(24, int(doc.driverMap.size())); ++i)
            head += QStringLiteral("%1 ").arg(doc.driverMap[i] == 0xFFFF ? QStringLiteral("-")
                                                                          : QString::number(doc.driverMap[i]));
        line(QStringLiteral("    first entries: %1").arg(head));
    }

    // ── H1: what does attachmentLengths measure? ──
    if (!doc.attachmentLengths.isEmpty() && !doc.bindVertices.isEmpty()) {
        line(QStringLiteral("  attachmentLengths (H1 — candidate references, all distances in wu, bind space; "
                            "real verts 0..%1 only):").arg(nv - 1));
        QVector<int> pins;
        for (int k = 0; k < doc.invMasses.size() && k < nv; ++k)
            if (doc.invMasses[k] == 0.0f) pins.push_back(k);
        QVector<double> al, dNearPin, dParent, dChainRoot, dChainSum, dKinRoot;
        const bool hasParents = doc.parentIndices.size() >= nv;
        const bool hasKin     = doc.kinematicRoots.size() >= nv;
        for (int k = 0; k < nv; ++k) {
            al.push_back(doc.attachmentLengths[k]);
            double best = 0;
            if (!pins.isEmpty()) {
                best = std::numeric_limits<double>::max();
                for (int p : pins) best = qMin(best, dist3(doc.bindVertices[k], doc.bindVertices[p]));
            }
            dNearPin.push_back(best);
            if (hasParents) {
                const int p = doc.parentIndices[k];
                dParent.push_back(p < doc.bindVertices.size() ? dist3(doc.bindVertices[k], doc.bindVertices[p]) : 0.0);
                int cur = k; double sum = 0; int guard = 0; int root = k;
                while (guard++ < 4096) {
                    const int pp = doc.parentIndices[cur];
                    if (pp >= doc.bindVertices.size() || pp == cur) break;
                    sum += dist3(doc.bindVertices[cur], doc.bindVertices[pp]);
                    cur = pp; root = pp;
                }
                dChainRoot.push_back(dist3(doc.bindVertices[k], doc.bindVertices[root]));
                dChainSum.push_back(sum);
            }
            if (hasKin) {
                const int r = doc.kinematicRoots[k];
                dKinRoot.push_back(r < doc.bindVertices.size() ? dist3(doc.bindVertices[k], doc.bindVertices[r]) : 0.0);
            }
        }
        auto corr = [&](const char* n, const QVector<double>& b) {
            if (b.isEmpty()) return;
            double mr = 0; int cnt = 0;
            for (int i = 0; i < al.size() && i < b.size(); ++i)
                if (b[i] > 1e-6) { mr += al[i] / b[i]; ++cnt; }
            line(QStringLiteral("    vs %1 r=%2 mean(attachLen/candidate)=%3")
                     .arg(QLatin1String(n), -28)
                     .arg(pearson(al, b), 0, 'f', 4)
                     .arg(cnt ? mr / cnt : 0.0, 0, 'g', 4));
        };
        corr("distToNearestPinned",     dNearPin);
        corr("distToParent",            dParent);
        corr("directDistToChainRoot",   dChainRoot);
        corr("chainSumToRoot",          dChainSum);
        corr("distToKinematicRoot",     dKinRoot);   // the authored LRA/tether anchor
        line(QStringLiteral("    sample verts (vert: attachLen | dNearPin | dKinRoot | level | parent | kinRoot | invMass):"));
        for (int k = 0; k < nv; k += qMax(1, nv / 10)) {
            line(QStringLiteral("      v%1: %2 | %3 | %4 | %5 | %6 | %7 | %8")
                     .arg(k)
                     .arg(double(doc.attachmentLengths[k]), 0, 'g', 5)
                     .arg(dNearPin.value(k), 0, 'g', 5)
                     .arg(dKinRoot.value(k, -1), 0, 'g', 5)
                     .arg(doc.levels.size() > k ? QString::number(doc.levels[k]) : QStringLiteral("-"))
                     .arg(hasParents ? QString::number(doc.parentIndices[k]) : QStringLiteral("-"))
                     .arg(hasKin ? QString::number(doc.kinematicRoots[k]) : QStringLiteral("-"))
                     .arg(double(doc.invMasses.value(k, -1)), 0, 'g', 4));
        }
    } else if (doc.attachmentLengths.isEmpty()) {
        const ArraySlot* s = doc.slot(QStringLiteral("ptAttachmentLengths"));
        if (s && s->present)
            line(QStringLiteral("  attachmentLengths: PRESENT (%1 B) but does NOT divide as f32/vert "
                                "(vertexCount=%2 -> expected %3 B) <<< the app silently drops this")
                     .arg(s->dataSize).arg(nv).arg(nv * 4));
        else
            line(QStringLiteral("  attachmentLengths: ABSENT (app synthesizes a fallback)"));
    }

    // ── levels vs parent-chain depth ──
    if (doc.levels.size() == nv && doc.parentIndices.size() == nv) {
        int match = 0;
        for (int k = 0; k < nv; ++k) {
            int cur = k, depth = 0, guard = 0;
            while (guard++ < 4096) {
                const int pp = doc.parentIndices[cur];
                if (pp >= nv || pp == cur) break;
                ++depth; cur = pp;
            }
            if (depth == doc.levels[k]) ++match;
        }
        line(QStringLiteral("  levels vs parent-chain depth: %1/%2 match  maxLevel(header)=%3")
                 .arg(match).arg(nv).arg(doc.maxLevel));
    }

    // ── kinematic roots: are they pinned? ──
    if (!doc.kinematicRoots.isEmpty() && !doc.invMasses.isEmpty()) {
        int pinnedRoots = 0, inRange = 0;
        for (quint16 kr : doc.kinematicRoots) {
            if (kr < doc.invMasses.size()) { ++inRange; if (doc.invMasses[kr] == 0.0f) ++pinnedRoots; }
        }
        line(QStringLiteral("  kinematicRoots: n=%1 inRange=%2 pinned=%3")
                 .arg(doc.kinematicRoots.size()).arg(inRange).arg(pinnedRoots));
    }

    // ── H2: driver arrays vs the piece skeleton ──
    if (geo && !geo->skeleton.isEmpty() && !doc.driverBindPose.isEmpty()) {
        line(QStringLiteral("  drivers (H2 — driver frames vs piece skeleton, %1 bones):")
                 .arg(geo->skeleton.size()));
        // Bone rest world positions (compose local TRS down the hierarchy, D4-native).
        const int nb = geo->skeleton.size();
        QVector<RigMath::Mat4> restG(nb);
        for (int j = 0; j < nb; ++j) {
            const ModelJoint& jt = geo->skeleton[j];
            const RigMath::Mat4 L = RigMath::composeTRS(jt.restT, jt.restQ, jt.restS);
            const int p = jt.parent;
            restG[j] = (p >= 0 && p < j) ? RigMath::mat4mul(restG[p], L) : L;
        }
        int near1cm = 0, near3cm = 0;
        double worstBest = 0;
        for (const ClothDoc::Frame& f : doc.driverBindPose) {
            // driver p is D4-native z-up; bone rests composed from restQ/restT are in the
            // same native space (pre-swap), so compare directly AND with the y-up swap —
            // report whichever matches better.
            double best = std::numeric_limits<double>::max();
            for (int j = 0; j < nb; ++j) {
                const double dx = restG[j][12] - f.p[0], dy = restG[j][13] - f.p[1],
                             dz = restG[j][14] - f.p[2];
                best = qMin(best, std::sqrt(dx*dx + dy*dy + dz*dz));
                float sx = f.p[0], sy = f.p[1], sz = f.p[2];
                zUpToYUp(sx, sy, sz);
                const double dx2 = restG[j][12] - sx, dy2 = restG[j][13] - sy, dz2 = restG[j][14] - sz;
                best = qMin(best, std::sqrt(dx2*dx2 + dy2*dy2 + dz2*dz2));
            }
            if (best < 0.01) ++near1cm;
            if (best < 0.03) ++near3cm;
            worstBest = qMax(worstBest, best);
        }
        line(QStringLiteral("    driverBindPose frames=%1 (driverCount=%2): within 1cm of a bone rest=%3, "
                            "within 3cm=%4, worst nearest=%5 wu")
                 .arg(doc.driverBindPose.size()).arg(doc.driverCount)
                 .arg(near1cm).arg(near3cm).arg(worstBest, 0, 'g', 4));
        // Scale sanity of driver frames.
        { QVector<float> sc;
          for (const ClothDoc::Frame& f : doc.driverBindPose) { sc << f.s[0] << f.s[1] << f.s[2]; }
          const Stats s = stats(sc);
          line(QStringLiteral("    driver scales: min=%1 max=%2 mean=%3")
                   .arg(s.mn, 0, 'g', 4).arg(s.mx, 0, 'g', 4).arg(s.mean, 0, 'g', 4)); }
    }
    if (!doc.driverMap.isEmpty()) {
        QHash<quint16, int> mult;
        for (quint16 m : doc.driverMap) ++mult[m];
        int dupTargets = 0;
        for (auto it = mult.constBegin(); it != mult.constEnd(); ++it) if (it.value() > 1) ++dupTargets;
        line(QStringLiteral("  driverMap: n=%1 distinct=%2 targetsReferencedMoreThanOnce=%3")
                 .arg(doc.driverMap.size()).arg(mult.size()).arg(dupTargets));
    }

    // ── raw peeks at the undecoded arrays ──
    for (const char* n : { "ptDeltaFrames", "unk_8ecbb2b", "unk_9f71907" }) {
        const ArraySlot* s = doc.slot(QString::fromLatin1(n));
        if (!s || !s->present) continue;
        line(QStringLiteral("  raw %1 (%2 B; first floats: %3):")
                 .arg(s->name).arg(s->dataSize).arg(peekF32(s->raw, 8)));
        r += hexDump(s->raw, 96);
    }

    return r;
}

QString coverageCsv(const ClothDoc& doc)
{
    QString r;
    r += QStringLiteral("# piece,cloth,array,present,bytes,bytesPerVert,appStatus\n");
    for (const ArraySlot& s : doc.arrays) {
        const int bpv = (doc.vertexCount > 0 && s.dataSize % doc.vertexCount == 0)
                            ? s.dataSize / doc.vertexCount : -1;
        QString status = QLatin1String(consumption(s.name));
        status = status.left(status.indexOf(QLatin1String("  ")) > 0
                                 ? status.indexOf(QLatin1String("  ")) : status.size());
        r += QStringLiteral("%1,%2,%3,%4,%5,%6,%7\n")
                 .arg(doc.pieceName, doc.name, s.name)
                 .arg(int(s.present)).arg(s.dataSize).arg(bpv).arg(status);
    }
    return r;
}

} // namespace d4cloth
