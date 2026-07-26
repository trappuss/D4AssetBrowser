// d4cloth — `run`: scenario driver with CSV dumps, SVG rendering, invariants, tracing.
//
//   d4cloth run --corpus corpus/ --case cape-outfit --scenario rest --steps 300 \
//       --out out/ [--dump bones,particles] [--every 10] [--render-every 50] \
//       [--param gravity=-0.004 ...] [--trace b:327 --trace p:5/68] [--report-bones 326,327,328]
//
// Scenarios (all deterministic, step-indexed, no wall clock):
//   rest         — no input at all; the sim settles from the seeded pose.
//   spin         — a scripted yaw flick: yaw ramps 0→pi over steps 60..120, then holds.
//   gravity-drop — gravity forced to -0.004/frame (unless --param overrides) from step 0.
//   anim         — plays the case's clip (or --anim <name>) at its authored frame rate
//                  against 60 Hz sim stepping, looping.

#include "AssetSource.h"
#include "LegacySolver.h"
#include "Solver.h"
#include "Scene.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QStringList>

#include <cmath>
#include <cstdio>

namespace d4cloth {

namespace {

bool applyParam(ClothParams& P, const QString& kv, QString* err)
{
    const int eq = kv.indexOf(QLatin1Char('='));
    if (eq <= 0) { *err = QStringLiteral("bad --param '%1' (want name=value)").arg(kv); return false; }
    const QString k = kv.left(eq).trimmed();
    bool ok = false;
    const double v = kv.mid(eq + 1).toDouble(&ok);
    if (!ok) { *err = QStringLiteral("bad --param value in '%1'").arg(kv); return false; }
    struct F { const char* n; float* p; };
    const F floats[] = {
        {"gravity", &P.gravity}, {"damping", &P.damping}, {"maxDistance", &P.maxDistance},
        {"bendStiffness", &P.bendStiffness}, {"stretchStiffness", &P.stretchStiffness},
        {"collisionMargin", &P.collisionMargin}, {"friction", &P.friction},
        {"capsuleRadius", &P.capsuleRadius}, {"boneTracking", &P.boneTracking},
        {"boneStiffness", &P.boneStiffness}, {"dragFactor", &P.dragFactor},
        {"windX", &P.windX}, {"windY", &P.windY}, {"windZ", &P.windZ},
        {"userSpinForce", &P.userSpinForce},
    };
    for (const F& f : floats)
        if (k == QLatin1String(f.n)) { *f.p = float(v); return true; }
    if (k == QLatin1String("iterations")) { P.iterations = int(v); return true; }
    if (k == QLatin1String("subSteps"))   { P.subSteps = int(v); return true; }
    if (k == QLatin1String("userSpin"))   { P.userSpin = v != 0; return true; }
    *err = QStringLiteral("unknown --param '%1'").arg(k);
    return false;
}

// Minimal SVG: front (x/y) + side (z/y) orthographic views of cage particles, cloth-bone
// chains and capsules, with the animated-target ghost. Legend baked in.
QString renderSvg(const LegacySolver& S)
{
    const Scene& sc = S.sc;
    const float cx = sc.homeCenter[0], cy = sc.homeCenter[1], cz = sc.homeCenter[2];
    const float R = qMax(0.001f, sc.radius);
    const float scale = 380.0f / (2.0f * R);           // one view = 800x860 px
    auto FX = [&](float x) { return 400.0f * 0.5f + (x - cx) * scale; };
    auto FY = [&](float y) { return 430.0f - (y - cy) * scale; };
    auto SX = [&](float z) { return 400.0f * 1.5f + (z - cz) * scale; };
    QString svg;
    svg += QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' width='800' height='880' "
                          "viewBox='0 0 800 880' style='background:#111'>\n");
    svg += QStringLiteral("<text x='8' y='16' fill='#888' font-size='12'>step %1 | left: front(x,y) "
                          "right: side(z,y) | grey=capsule blue=pinned orange=free red=drift>cap/2 "
                          "green=ghost(anim)</text>\n").arg(S.stepIndex);
    // capsules
    for (int i = 0; i < S.colR0.size(); ++i) {
        const float* p0 = S.colP0.constData() + i*3;
        const float* p1 = S.colP1.constData() + i*3;
        const float r0 = S.colR0[i] * S.P.capsuleRadius * scale;
        const float r1 = S.colR1[i] * S.P.capsuleRadius * scale;
        for (int view = 0; view < 2; ++view) {
            auto px = [&](const float* p) { return view == 0 ? FX(p[0]) : SX(p[2]); };
            svg += QStringLiteral("<line x1='%1' y1='%2' x2='%3' y2='%4' stroke='#444' stroke-width='1'/>"
                                  "<circle cx='%1' cy='%2' r='%5' fill='none' stroke='#444'/>"
                                  "<circle cx='%3' cy='%4' r='%6' fill='none' stroke='#444'/>\n")
                       .arg(px(p0)).arg(FY(p0[1])).arg(px(p1)).arg(FY(p1[1])).arg(r0).arg(r1);
        }
    }
    // cage particles + targets
    for (int ci = 0; ci < S.cages.size(); ++ci) {
        const LegacySolver::CageRt& rt = S.cages[ci];
        const ClothSim& cs = sc.geo.clothSims[rt.simIdx];
        for (int k = 0; k < cs.vertCount; ++k) {
            const float* p = rt.pos.constData() + k*3;
            const float* t = rt.target.constData() + k*3;
            const bool pinned = k < cs.invMasses.size() && cs.invMasses[k] == 0.0f;
            for (int view = 0; view < 2; ++view) {
                auto px = [&](const float* q) { return view == 0 ? FX(q[0]) : SX(q[2]); };
                svg += QStringLiteral("<circle cx='%1' cy='%2' r='1.4' fill='#2a5'/>\n")
                           .arg(px(t)).arg(FY(t[1]));
                svg += QStringLiteral("<%1 %2 fill='%3'/>\n")
                           .arg(pinned ? QStringLiteral("rect")
                                       : QStringLiteral("circle"),
                                pinned ? QStringLiteral("x='%1' y='%2' width='3' height='3'")
                                             .arg(px(p) - 1.5f).arg(FY(p[1]) - 1.5f)
                                       : QStringLiteral("cx='%1' cy='%2' r='1.8'")
                                             .arg(px(p)).arg(FY(p[1])),
                                pinned ? QStringLiteral("#48f") : QStringLiteral("#fa4"));
            }
        }
    }
    // cloth-bone chains, coloured by drift
    for (int j : S.sbOrder) {
        const int p = sc.geo.skeleton[j].parent;
        if (p < 0 || p >= S.nb) continue;
        const float* G = S.global[j].data();
        const float* Gp = S.global[p].data();
        const float dx = G[12]-S.animG[j][12], dy = G[13]-S.animG[j][13], dz = G[14]-S.animG[j][14];
        const float drift = std::sqrt(dx*dx + dy*dy + dz*dz);
        const bool bad = drift > S.kDivergeMax * 0.5f;
        for (int view = 0; view < 2; ++view) {
            const float x1 = view == 0 ? FX(Gp[12]) : SX(Gp[14]);
            const float x2 = view == 0 ? FX(G[12])  : SX(G[14]);
            svg += QStringLiteral("<line x1='%1' y1='%2' x2='%3' y2='%4' stroke='%5' stroke-width='%6'/>\n")
                       .arg(x1).arg(FY(Gp[13])).arg(x2).arg(FY(G[13]))
                       .arg(bad ? QStringLiteral("#f33") : QStringLiteral("#e90"))
                       .arg(bad ? 2.0 : 0.8);
            if (bad && view == 1)
                svg += QStringLiteral("<text x='%1' y='%2' fill='#f66' font-size='10'>%3</text>\n")
                           .arg(x2 + 3).arg(FY(G[13])).arg(j);
        }
    }
    svg += QStringLiteral("</svg>\n");
    return svg;
}

} // namespace

int cmdRun(const QStringList& args)
{
    QCommandLineParser p;
    p.addOptions({
        { QStringLiteral("corpus"),   QStringLiteral("corpus dir"), QStringLiteral("dir") },
        { QStringLiteral("case"),     QStringLiteral("case from manifest"), QStringLiteral("name") },
        { QStringLiteral("piece"),    QStringLiteral("explicit piece list (repeatable)"), QStringLiteral("name") },
        { QStringLiteral("scenario"), QStringLiteral("rest | spin | gravity-drop"), QStringLiteral("s"), QStringLiteral("rest") },
        { QStringLiteral("steps"),    QStringLiteral("step count"), QStringLiteral("n"), QStringLiteral("300") },
        { QStringLiteral("out"),      QStringLiteral("output dir"), QStringLiteral("dir"), QStringLiteral("out") },
        { QStringLiteral("dump"),     QStringLiteral("bones,particles (summary always)"), QStringLiteral("list"), QString() },
        { QStringLiteral("every"),    QStringLiteral("dump every N steps"), QStringLiteral("n"), QStringLiteral("10") },
        { QStringLiteral("render-every"), QStringLiteral("SVG every N steps (0=off)"), QStringLiteral("n"), QStringLiteral("0") },
        { QStringLiteral("param"),    QStringLiteral("override name=value (repeatable)"), QStringLiteral("kv") },
        { QStringLiteral("trace"),    QStringLiteral("b:<bone> or p:<cage>/<vert> (repeatable)"), QStringLiteral("id") },
        { QStringLiteral("report-bones"), QStringLiteral("comma list: print final positions"), QStringLiteral("list") },
        { QStringLiteral("no-game-tuning"), QStringLiteral("skip the .clt-driven param overrides") },
        { QStringLiteral("anim"), QStringLiteral("clip name for --scenario anim (default: the case's first)"), QStringLiteral("name") },
        { QStringLiteral("probe-cage"), QStringLiteral("print each cage vert's borrowed skinning (joints/weights)"), QStringLiteral("idx") },
        { QStringLiteral("solver"), QStringLiteral("legacy | authored"), QStringLiteral("which"), QStringLiteral("legacy") },
    });
    p.process(QStringList() << QStringLiteral("d4cloth") << args);
    const QString corpusDir = p.value(QStringLiteral("corpus"));
    if (corpusDir.isEmpty()) { std::fprintf(stderr, "run: --corpus required\n"); return 2; }
    CorpusSource corpus(corpusDir);

    QStringList pieces = p.values(QStringLiteral("piece"));
    QString caseName = p.value(QStringLiteral("case"));
    QString animName = p.value(QStringLiteral("anim"));
    if (!caseName.isEmpty()) {
        QString err;
        for (const TestCase& tc : loadCases(corpusDir + QStringLiteral("/manifest.json"), &err))
            if (tc.name == caseName) {
                pieces = tc.pieces;
                if (animName.isEmpty() && !tc.anims.isEmpty()) animName = tc.anims.first();
            }
    } else caseName = QStringLiteral("(pieces)");
    if (pieces.isEmpty()) { std::fprintf(stderr, "run: no pieces (--case/--piece)\n"); return 2; }

    const QString scenarioPeek = p.value(QStringLiteral("scenario"));
    Scene sc = loadScene(corpus, caseName, pieces, !p.isSet(QStringLiteral("no-game-tuning")),
                         scenarioPeek == QLatin1String("anim") ? animName : QString());
    if (scenarioPeek == QLatin1String("anim") && !sc.anim.valid) {
        std::fprintf(stderr, "run: anim scenario but clip '%s' failed to load: %s\n",
                     qPrintable(animName), qPrintable(sc.error));
        return 2;
    }
    if (!sc.ok) { std::fprintf(stderr, "run: %s\n", qPrintable(sc.error)); return 2; }

    const QString scenario = p.value(QStringLiteral("scenario"));
    if (scenario == QLatin1String("gravity-drop") && sc.params.gravity == 0.0f)
        sc.params.gravity = -0.004f;
    QString perr;
    for (const QString& kv : p.values(QStringLiteral("param")))
        if (!applyParam(sc.params, kv, &perr)) { std::fprintf(stderr, "run: %s\n", qPrintable(perr)); return 2; }

    const int steps = p.value(QStringLiteral("steps")).toInt();
    const int every = qMax(1, p.value(QStringLiteral("every")).toInt());
    const int renderEvery = p.value(QStringLiteral("render-every")).toInt();
    const QStringList dumps = p.value(QStringLiteral("dump")).split(QLatin1Char(','), Qt::SkipEmptyParts);
    const QString outDir = p.value(QStringLiteral("out"));
    QDir().mkpath(outDir);

    const bool authored = p.value(QStringLiteral("solver")) == QLatin1String("authored");
    Solver authoredS(sc);
    LegacySolver legacyS(sc);
    LegacySolver& S = authored ? static_cast<LegacySolver&>(authoredS) : legacyS;
    S.P = sc.params;

    // trace targets
    QFile traceFile(outDir + QStringLiteral("/trace.csv"));
    for (const QString& t : p.values(QStringLiteral("trace"))) {
        if (t.startsWith(QLatin1String("b:"))) S.traceBones << t.mid(2).toInt();
        else if (t.startsWith(QLatin1String("p:"))) {
            const QStringList cv = t.mid(2).split(QLatin1Char('/'));
            if (cv.size() == 2) S.traceParts << qMakePair(cv[0].toInt(), cv[1].toInt());
        }
    }
    if ((!S.traceBones.isEmpty() || !S.traceParts.isEmpty())
        && traceFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        traceFile.write("step,phase,id,x_wu,y_wu,z_wu,note\n");
        S.traceSink = [&traceFile](const QString& l) { traceFile.write((l + QLatin1Char('\n')).toUtf8()); };
    }

    // run header — inputs, params, units
    QString hdr;
    hdr += QStringLiteral("# d4cloth run | case=%1 scenario=%2 steps=%3 | units: wu (bind space, ~1m)\n")
               .arg(caseName, scenario).arg(steps);
    for (const QString& pc : pieces) {
        const AssetBlob b = corpus.appearance(pc);
        hdr += QStringLiteral("# piece %1 metaFnv=%2 payloadFnv=%3\n")
                   .arg(pc).arg(QString::number(fnv1a64(b.meta), 16), QString::number(fnv1a64(b.payload), 16));
    }
    hdr += QStringLiteral("# params: gravity=%1 damping=%2 maxDistance=%3 iterations=%4 subSteps=%5 "
                          "margin=%6 friction=%7 capsuleRadius=%8 boneTracking=%9 boneStiffness=%10 "
                          "gameTuning=%11\n")
               .arg(sc.params.gravity).arg(sc.params.damping).arg(sc.params.maxDistance)
               .arg(sc.params.iterations).arg(sc.params.subSteps).arg(sc.params.collisionMargin)
               .arg(sc.params.friction).arg(sc.params.capsuleRadius).arg(sc.params.boneTracking)
               .arg(sc.params.boneStiffness).arg(sc.gct.found ? 1 : 0);
    std::printf("%s", qPrintable(hdr));

    QFile fSummary(outDir + QStringLiteral("/summary.csv"));
    fSummary.open(QIODevice::WriteOnly | QIODevice::Text);
    fSummary.write(hdr.toUtf8());
    fSummary.write("step,worstDrift_wu,worstDriftBone,divergeCap_wu,worstPen_wu,contacts,"
                   "kineticE_wu2,cageLimitHits,cageSafetyHits,boneLimitHits,divergeClamped,nan,"
                   "tetherHits,worstPinnedTargetErr_wu\n");
    QFile fBones, fParts;
    if (dumps.contains(QStringLiteral("bones"))) {
        fBones.setFileName(outDir + QStringLiteral("/bones.csv"));
        fBones.open(QIODevice::WriteOnly | QIODevice::Text);
        fBones.write(hdr.toUtf8());
        fBones.write("step,bone,name,parent,px_wu,py_wu,pz_wu,animPx_wu,animPy_wu,animPz_wu,"
                     "drift_wu,cage,vert,driveW,driven,pinned,contact\n");
    }
    if (dumps.contains(QStringLiteral("particles"))) {
        fParts.setFileName(outDir + QStringLiteral("/particles.csv"));
        fParts.open(QIODevice::WriteOnly | QIODevice::Text);
        fParts.write(hdr.toUtf8());
        fParts.write("step,cage,vert,px_wu,py_wu,pz_wu,tx_wu,ty_wu,tz_wu,drift_wu,"
                     "attachLen_raw,md_wu,invMass,pinned\n");
    }

    // clamp-residency detector (invariant 7): consecutive steps at the cap per bone.
    QVector<int> capResidency(sc.geo.skeleton.size(), 0);
    int worstResidency = 0, worstResidencyBone = -1;

    for (int s = 0; s < steps; ++s) {
        // scenario input
        if (scenario == QLatin1String("anim") && sc.anim.valid && sc.anim.frameCount > 0) {
            // authored fps against 60 Hz stepping, looping — same cadence as the app's timer.
            const int frame = int(std::floor(double(s) * double(sc.anim.frameRate) / 60.0))
                              % sc.anim.frameCount;
            S.setAnimFrame(frame);
        }
        if (scenario == QLatin1String("spin")) {
            float yaw = 0.0f;
            if (s >= 60) yaw = qMin(1.0f, (s - 60) / 60.0f) * 3.14159265f;
            S.setYaw(yaw);
        }
        S.step();

        // energy + NaN scan
        double kin = 0; int nan = 0;
        for (int j : S.sbOrder) {
            const float vx = S.sbSimHead[j*3]-S.sbPrevHead[j*3],
                        vy = S.sbSimHead[j*3+1]-S.sbPrevHead[j*3+1],
                        vz = S.sbSimHead[j*3+2]-S.sbPrevHead[j*3+2];
            if (!std::isfinite(vx+vy+vz)) ++nan; else kin += vx*vx+vy*vy+vz*vz;
        }
        if (authored) {
            for (const Solver::AuthoredCage& ac : authoredS.acages)
                for (int i = 0; i < ac.pos.size(); i += 3) {
                    const float vx = ac.pos[i]-ac.prev[i], vy = ac.pos[i+1]-ac.prev[i+1],
                                vz = ac.pos[i+2]-ac.prev[i+2];
                    if (!std::isfinite(vx+vy+vz)) ++nan; else kin += vx*vx+vy*vy+vz*vz;
                }
        } else {
            for (const LegacySolver::CageRt& rt : S.cages)
                for (int i = 0; i < rt.pos.size(); i += 3) {
                    const float vx = rt.pos[i]-rt.prev[i], vy = rt.pos[i+1]-rt.prev[i+1],
                                vz = rt.pos[i+2]-rt.prev[i+2];
                    if (!std::isfinite(vx+vy+vz)) ++nan; else kin += vx*vx+vy*vy+vz*vz;
                }
        }
        if (nan) std::printf("INVARIANT nan step=%d count=%d\n", s, nan);

        // clamp residency
        for (int j : S.sbOrder) {
            const float dx = S.sbSimHead[j*3]-S.animG[j][12], dy = S.sbSimHead[j*3+1]-S.animG[j][13],
                        dz = S.sbSimHead[j*3+2]-S.animG[j][14];
            const float d = std::sqrt(dx*dx+dy*dy+dz*dz);
            if (S.kDivergeMax > 0 && d > S.kDivergeMax * 0.98f) {
                if (++capResidency[j] > worstResidency) { worstResidency = capResidency[j]; worstResidencyBone = j; }
            } else capResidency[j] = 0;
        }

        fSummary.write(QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13,%14\n")
            .arg(s).arg(double(S.worstDiv), 0, 'f', 6).arg(S.worstDivBone)
            .arg(double(S.kDivergeMax), 0, 'f', 6)
            .arg(double(S.worstPen), 0, 'f', 6).arg(S.contactCount)
            .arg(kin, 0, 'g', 8).arg(S.cageLimitHits).arg(S.cageSafetyHits)
            .arg(S.boneLimitHits).arg(S.divergeClamped).arg(nan)
            .arg(authored ? authoredS.tetherHits : 0)
            .arg(authored ? double(authoredS.worstPinnedTargetErr) : 0.0, 0, 'g', 5).toUtf8());

        const bool dumpNow = (s % every == 0) || s == steps - 1;
        if (dumpNow && fBones.isOpen()) {
            for (int j : S.sbOrder) {
                const float dx = S.sbSimHead[j*3]-S.animG[j][12], dy = S.sbSimHead[j*3+1]-S.animG[j][13],
                            dz = S.sbSimHead[j*3+2]-S.animG[j][14];
                fBones.write(QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13,%14,%15,%16,%17\n")
                    .arg(s).arg(j).arg(sc.geo.skeleton[j].name).arg(sc.geo.skeleton[j].parent)
                    .arg(double(S.sbSimHead[j*3]), 0, 'f', 6).arg(double(S.sbSimHead[j*3+1]), 0, 'f', 6)
                    .arg(double(S.sbSimHead[j*3+2]), 0, 'f', 6)
                    .arg(double(S.animG[j][12]), 0, 'f', 6).arg(double(S.animG[j][13]), 0, 'f', 6)
                    .arg(double(S.animG[j][14]), 0, 'f', 6)
                    .arg(double(std::sqrt(dx*dx+dy*dy+dz*dz)), 0, 'f', 6)
                    .arg(S.sbAnchorPiece[j]).arg(S.sbAnchorVert[j])
                    .arg(double(S.sbAnchorW[j]), 0, 'f', 3)
                    .arg(int(S.sbDriven[j])).arg(int(S.sbPin[j])).arg(int(S.sbContact[j])).toUtf8());
            }
        }
        if (dumpNow && fParts.isOpen()) {
            for (int ci = 0; ci < S.cages.size(); ++ci) {
                const LegacySolver::CageRt& rt = S.cages[ci];
                const bool useA = authored && ci < authoredS.acages.size()
                                  && authoredS.acages[ci].seeded;
                const Solver::AuthoredCage* ac = useA ? &authoredS.acages[ci] : nullptr;
                const ClothSim& cs = sc.geo.clothSims[rt.simIdx];
                const float mdScale = S.cageMdScale(ci);
                const int nvDump = useA ? ac->nv : cs.vertCount;
                for (int k = 0; k < nvDump; ++k) {
                    const float* pp = (useA ? ac->pos.constData() : rt.pos.constData())+k*3;
                    const float* tt = (useA ? ac->target.constData() : rt.target.constData())+k*3;
                    const float dx=pp[0]-tt[0], dy=pp[1]-tt[1], dz=pp[2]-tt[2];
                    const float al = (k < cs.attachLen.size()) ? cs.attachLen[k] : 1.0f;
                    const float im = (k < cs.invMasses.size()) ? cs.invMasses[k] : -1.0f;
                    fParts.write(QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13,%14\n")
                        .arg(s).arg(ci).arg(k)
                        .arg(double(pp[0]), 0, 'f', 6).arg(double(pp[1]), 0, 'f', 6).arg(double(pp[2]), 0, 'f', 6)
                        .arg(double(tt[0]), 0, 'f', 6).arg(double(tt[1]), 0, 'f', 6).arg(double(tt[2]), 0, 'f', 6)
                        .arg(double(std::sqrt(dx*dx+dy*dy+dz*dz)), 0, 'f', 6)
                        .arg(double(al), 0, 'f', 5).arg(double(al * mdScale), 0, 'f', 5)
                        .arg(double(im), 0, 'g', 4).arg(im == 0.0f ? 1 : 0).toUtf8());
                }
            }
        }
        if (renderEvery > 0 && (s % renderEvery == 0 || s == steps - 1)) {
            QFile f(outDir + QStringLiteral("/frame_%1.svg").arg(s, 5, 10, QLatin1Char('0')));
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) f.write(renderSvg(S).toUtf8());
        }
    }

    if (p.isSet(QStringLiteral("probe-cage"))) {
        const int ci = p.value(QStringLiteral("probe-cage")).toInt();
        if (ci >= 0 && ci < S.cages.size()) {
            const LegacySolver::CageRt& rt = S.cages[ci];
            const ClothSim& cs = sc.geo.clothSims[rt.simIdx];
            std::printf("probe cage %d ('%s', sim %d): vert -> borrowed skinning J(w) [c]=cloth bone\n",
                        ci, qPrintable(cs.name), rt.simIdx);
            for (int k = 0; k < cs.vertCount; ++k) {
                QString row;
                for (int t = 0; t < 4; ++t) {
                    if (rt.W[k][t] <= 0.0f) continue;
                    const int b = rt.J[k][t];
                    row += QStringLiteral("%1%2(%3) ").arg(b)
                               .arg(b < S.sbIsCloth.size() && S.sbIsCloth[b] ? QStringLiteral("[c]") : QString())
                               .arg(double(rt.W[k][t]), 0, 'f', 2);
                }
                const float* bp = cs.bindVerts.constData() + k*3;
                std::printf("  v%-3d bind(%.3f %.3f %.3f) %s\n", k, bp[0], bp[1], bp[2], qPrintable(row));
            }
        }
    }
    // ── build-time invariants + final report ──
    {   // anchor exclusivity (invariant 6)
        QHash<qint64, QVector<int>> byAnchor;
        for (int j : S.sbOrder)
            if (S.sbDriven[j])
                byAnchor[(qint64(S.sbAnchorPiece[j]) << 32) | quint32(S.sbAnchorVert[j])].append(j);
        int shared = 0;
        for (auto it = byAnchor.constBegin(); it != byAnchor.constEnd(); ++it)
            if (it.value().size() > 1) {
                ++shared;
                QString bones;
                for (int j : it.value()) bones += QStringLiteral("%1 ").arg(j);
                std::printf("INVARIANT shared-anchor cage=%d vert=%d bones= %s\n",
                            int(it.key() >> 32), int(it.key() & 0xFFFFFFFF), qPrintable(bones));
            }
        std::printf("run: %d shared anchor(s) among %d driven bone(s)\n",
                    shared, int(std::count(S.sbDriven.begin(), S.sbDriven.end(), quint8(1))));
    }
    if (worstResidency > 30)
        std::printf("INVARIANT clamp-residency bone=%d steps=%d (a clamp is acting as a mechanism)\n",
                    worstResidencyBone, worstResidency);

    for (const QString& rb : p.value(QStringLiteral("report-bones")).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const int j = rb.trimmed().toInt();
        if (j < 0 || j >= sc.geo.skeleton.size()) continue;
        const float dx = S.sbSimHead[j*3]-S.animG[j][12], dy = S.sbSimHead[j*3+1]-S.animG[j][13],
                    dz = S.sbSimHead[j*3+2]-S.animG[j][14];
        std::printf("bone %d '%s' parent=%d pos(%.3f %.3f %.3f) anim(%.3f %.3f %.3f) drift=%.3f wu "
                    "driven=%d cage=%d vert=%d driveW=%.2f pin=%d\n",
                    j, qPrintable(sc.geo.skeleton[j].name), sc.geo.skeleton[j].parent,
                    S.sbSimHead[j*3], S.sbSimHead[j*3+1], S.sbSimHead[j*3+2],
                    S.animG[j][12], S.animG[j][13], S.animG[j][14],
                    std::sqrt(dx*dx+dy*dy+dz*dz),
                    int(S.sbDriven[j]), S.sbAnchorPiece[j], S.sbAnchorVert[j],
                    S.sbAnchorW[j], int(S.sbPin[j]));
    }
    std::printf("run: done, %d step(s). cages=%d simBones=%d driven=%d capsules=%d -> %s/\n",
                steps, int(S.cages.size()), int(S.sbOrder.size()),
                int(std::count(S.sbDriven.begin(), S.sbDriven.end(), quint8(1))),
                int(S.colR0.size()), qPrintable(outDir));
    return 0;
}

} // namespace d4cloth
