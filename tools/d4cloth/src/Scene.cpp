#include "Scene.h"

#include "model/ModelParser.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <cmath>

namespace d4cloth {

namespace {

// WardrobeTab2::fillClothSimTuning — verbatim logic, reading from the corpus cloth dir.
void fillClothSimTuning(const CorpusSource& corpus, ModelGeometry& geo)
{
    for (ClothSim& s : geo.clothSims) {
        if (s.name.isEmpty()) continue;
        const QJsonObject doc = corpus.clothTuning(s.name);
        if (doc.isEmpty()) continue;
        const QJsonObject t = doc.value(QStringLiteral("tClothTuning")).toObject();
        if (t.isEmpty()) continue;
        auto g = [&](const char* k, double def) { return t.value(QLatin1String(k)).toDouble(def); };
        s.boneTrack = qBound(0.0f, float(g("flBoneTrackingFactor", 0.5)), 1.0f);
        const QJsonObject gv = t.value(QStringLiteral("vGravity")).toObject();
        const double gmag = -gv.value(QStringLiteral("z")).toDouble(-20.0);
        s.gravScale = qBound(0.3f, float(gmag / 20.0), 3.0f);
        const QJsonObject sw = t.value(QStringLiteral("vSelfWind")).toObject();
        const double wf = g("flWindFactor", 1.0), sc = 0.0006;
        const double sx = sw.value(QStringLiteral("x")).toDouble(),
                     sy = sw.value(QStringLiteral("y")).toDouble(),
                     sz = sw.value(QStringLiteral("z")).toDouble();
        s.windX = float(sx * wf * sc); s.windY = float(sz * wf * sc); s.windZ = float(-sy * wf * sc);
        s.tuned = true;
    }
}

// WardrobeTab2::loadClothTuning — averaged authored tuning across the outfit's cloth
// pieces, discovered via each piece's .app.json snoCloth references.
GameClothTuning loadGameTuning(const CorpusSource& corpus, const QStringList& pieces)
{
    GameClothTuning out;
    static const QRegularExpression rxCloth(
        QStringLiteral("\"snoCloth\"\\s*:\\s*\\{[^}]*?\"name\"\\s*:\\s*\"([^\"]+)\""));
    QSet<QString> clothNames;
    for (const QString& pc : pieces) {
        QFile f(corpus.dir() + QStringLiteral("/appearance/") + pc + QStringLiteral(".app.json"));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QString raw = QString::fromUtf8(f.readAll());
        auto it = rxCloth.globalMatch(raw);
        while (it.hasNext()) clothNames.insert(it.next().captured(1));
    }
    double boneT=0, actorT=0, stretch=0, horiz=0, shear=0, bend=0, damp=0, attach=0, fric=0, dens=0;
    double drag=0, lift=0;
    int n = 0;
    for (const QString& cn : clothNames) {
        const QJsonObject doc = corpus.clothTuning(cn);   // tries cn and cn_sim + ci scan
        if (doc.isEmpty()) continue;
        const QJsonObject t = doc.value(QStringLiteral("tClothTuning")).toObject();
        if (t.isEmpty()) continue;
        auto g = [&](const char* k, double def) { return t.value(QLatin1String(k)).toDouble(def); };
        boneT  += g("flBoneTrackingFactor", 0.45);
        actorT += g("flActorTrackingFactor", 0.6);
        stretch+= g("flStretchingStiffness", 0.7);
        horiz  += g("flHorizontalStiffness", 0.5);
        shear  += g("flShearStiffness", 0.15);
        bend   += g("flBendingStiffness", 0.3);
        damp   += g("flDampingFactor", 0.25);
        attach += g("flAttachmentStiffness", 0.2);
        fric   += g("flFrictionScale", 1.0);
        dens   += g("flDensity", 2.0);
        drag   += g("flDragFactor", 0.0);
        lift   += g("flLiftFactor", 0.0);
        ++n;
    }
    if (n > 0) {
        const double inv = 1.0 / n;
        out.boneTrack  = qBound(0.0f, float(boneT  * inv), 1.0f);
        out.actorTrack = qBound(0.0f, float(actorT * inv), 1.0f);
        out.stretch    = qBound(0.0f, float(stretch* inv), 1.0f);
        out.horiz      = qBound(0.0f, float(horiz  * inv), 1.0f);
        out.shear      = qBound(0.0f, float(shear  * inv), 1.0f);
        out.bend       = qBound(0.0f, float(bend   * inv), 1.0f);
        out.damping    = qBound(0.0f, float(damp   * inv), 1.0f);
        out.attach     = qBound(0.0f, float(attach * inv), 1.0f);
        out.friction   = qBound(0.0f, float(fric   * inv / 3.0), 1.0f);   // data 0..3 → 0..1
        out.density    = qMax(0.1f, float(dens * inv));
        out.drag       = qBound(0.0f, float(drag * inv), 1.0f);
        out.lift       = qBound(0.0f, float(lift * inv), 1.0f);
        out.found = true;
    }
    return out;
}

} // namespace

Scene loadScene(const CorpusSource& corpus, const QString& caseName,
                const QStringList& pieces, bool applyGameTuning, const QString& animName)
{
    Scene sc;
    sc.caseName = caseName;
    sc.pieces = pieces;

    // ── parse pieces + merge (the app's own code paths) ──
    QVector<ModelGeometry> parsed;
    QVector<QVector<ClothDoc>> perPieceDocs;
    for (const QString& p : pieces) {
        const AssetBlob b = corpus.appearance(p);
        if (!b.ok) { sc.error = b.error; return sc; }
        ModelGeometry g = ModelParser::parseApp(b.meta, b.payload);
        if (!g.valid) { sc.error = QStringLiteral("parseApp failed for %1").arg(p); return sc; }
        parsed.push_back(g);
        QVector<ClothDoc> docs = parseClothDocs(b.meta, b.payload);
        for (ClothDoc& d : docs) d.pieceName = p;
        perPieceDocs.push_back(docs);
    }
    sc.geo = ModelParser::mergeGeometries(parsed);
    if (!sc.geo.valid) { sc.error = QStringLiteral("mergeGeometries produced no primitives"); return sc; }
    sc.baseBones = sc.geo.nBaseBones;
    fillClothSimTuning(corpus, sc.geo);

    // ── unified-arrays build (GLModelWidget::setGeometry equivalent) ──
    int vtotal = 0;
    for (const MeshPrimitive& pr : sc.geo.primitives) vtotal += pr.vertices.size();
    sc.bindVerts.reserve(vtotal * 11);
    sc.vJoints.reserve(vtotal); sc.vWeights.reserve(vtotal);
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    int voff = 0;
    for (int pi = 0; pi < sc.geo.primitives.size(); ++pi) {
        const MeshPrimitive& pr = sc.geo.primitives[pi];
        Scene::Part part;
        part.offset = sc.indices.size(); part.count = pr.indices.size();
        part.material = pr.materialName; part.pieceIdx = pi;
        sc.parts.push_back(part);
        for (const MeshVertex& v : pr.vertices) {
            sc.bindVerts << v.px << v.py << v.pz << v.nx << v.ny << v.nz
                         << v.u << v.v << 0.0f << 0.0f << 0.0f;
            sc.vJoints.push_back({{ v.joints[0], v.joints[1], v.joints[2], v.joints[3] }});
            sc.vWeights.push_back({{ v.weights[0], v.weights[1], v.weights[2], v.weights[3] }});
            mn[0] = qMin(mn[0], v.px); mn[1] = qMin(mn[1], v.py); mn[2] = qMin(mn[2], v.pz);
            mx[0] = qMax(mx[0], v.px); mx[1] = qMax(mx[1], v.py); mx[2] = qMax(mx[2], v.pz);
        }
        for (quint32 i : pr.indices) sc.indices.push_back(i + voff);
        voff += pr.vertices.size();
    }
    const float dx = mx[0]-mn[0], dy = mx[1]-mn[1], dz = mx[2]-mn[2];
    sc.radius = qMax(0.001f, 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz));
    sc.homeCenter = {{ (mn[0]+mx[0])*0.5f, (mn[1]+mx[1])*0.5f, (mn[2]+mx[2])*0.5f }};

    // ── rest globals (same compose+hierarchy as the app's builders, y-up) ──
    const int nb = sc.geo.skeleton.size();
    sc.restGlobal.resize(nb);
    for (int j = 0; j < nb; ++j) {
        const ModelJoint& jt = sc.geo.skeleton[j];
        const RigMath::Mat4 L = composeTRSSwapped(jt.restQ.data(), jt.restT.data(), jt.restS.data());
        const int p = jt.parent;
        sc.restGlobal[j] = (p >= 0 && p < j) ? RigMath::mat4mul(sc.restGlobal[p], L) : L;
    }

    // ── full ClothDocs with follower remap to the unified skeleton ──
    // mergeGeometries maps piece bones to unified indices by nameHash; rebuild that map.
    QHash<quint32,int> hashToUnified;
    for (int j = 0; j < nb; ++j)
        if (!hashToUnified.contains(sc.geo.skeleton[j].nameHash))
            hashToUnified.insert(sc.geo.skeleton[j].nameHash, j);
    for (int pi = 0; pi < perPieceDocs.size(); ++pi) {
        const QVector<ModelJoint>& pieceSkel = parsed[pi].skeleton;
        for (const ClothDoc& d : perPieceDocs[pi]) {
            QVector<int> follow(d.vertexCount, -1);
            for (int k = 0; k < d.followerIndices.size() && k < d.vertexCount; ++k) {
                const quint16 f = d.followerIndices[k];
                if (f == 0xFFFF || f >= pieceSkel.size()) continue;
                follow[k] = hashToUnified.value(pieceSkel[f].nameHash, -1);
            }
            // Driver → bone: ptDriverMap has boneCount entries; the working assumption
            // (validated at run time by the pinned-target check) is that entry b refers
            // to piece-skeleton bone b, value = driver index.
            QVector<int> drvBone(qMax(0, d.driverCount), -1);
            for (int b = 0; b < d.driverMap.size() && b < pieceSkel.size(); ++b) {
                const quint16 dv = d.driverMap[b];
                if (dv == 0xFFFF || dv >= drvBone.size()) continue;
                if (drvBone[dv] < 0)
                    drvBone[dv] = hashToUnified.value(pieceSkel[b].nameHash, -1);
            }
            // Full authored tuning for this doc.
            Scene::DocTuning dt;
            const QJsonObject tj = corpus.clothTuning(d.name)
                                       .value(QStringLiteral("tClothTuning")).toObject();
            if (!tj.isEmpty()) {
                auto g = [&](const char* k, double def) { return tj.value(QLatin1String(k)).toDouble(def); };
                dt.stretch = float(g("flStretchingStiffness", 0.7));
                dt.horiz   = float(g("flHorizontalStiffness", 0.5));
                dt.shear   = float(g("flShearStiffness", 0.15));
                dt.bend    = float(g("flBendingStiffness", 0.3));
                dt.dampingFactor  = float(g("flDampingFactor", 0.0));
                dt.dragFactor     = float(g("flDragFactor", 0.0));
                dt.attachStiffness= float(g("flAttachmentStiffness", 0.3));
                dt.nIterations    = int(g("nIterations", 1));
                dt.boneTrack      = float(g("flBoneTrackingFactor", 0.5));
                constexpr float dt2 = (1.0f/60.0f) * (1.0f/60.0f);
                const QJsonObject gv = tj.value(QStringLiteral("vGravity")).toObject();
                dt.gravPerStep = float(gv.value(QStringLiteral("z")).toDouble(-22.0)) * dt2;
                const QJsonObject sw = tj.value(QStringLiteral("vSelfWind")).toObject();
                const float wf = float(g("flWindFactor", 1.0));
                dt.windX = float(sw.value(QStringLiteral("x")).toDouble()) * wf * dt2;
                dt.windY = float(sw.value(QStringLiteral("z")).toDouble()) * wf * dt2;
                dt.windZ = float(-sw.value(QStringLiteral("y")).toDouble()) * wf * dt2;
                dt.found = true;
            }
            sc.docs.push_back(d);
            sc.docFollowerUnified.push_back(follow);
            sc.docDriverBoneUnified.push_back(drvBone);
            sc.docTuning.push_back(dt);
        }
    }

    // ── params: defaults + the app's game-driven overrides ──
    sc.gct = loadGameTuning(corpus, pieces);
    if (applyGameTuning && sc.gct.found) {
        ClothParams& p = sc.params;
        p.boneTracking     = sc.gct.boneTrack;
        p.actorTracking    = sc.gct.actorTrack;
        p.stretchStiffness = sc.gct.stretch;
        p.horizStiffness   = sc.gct.horiz;
        p.shearStiffness   = sc.gct.shear;
        p.bendStiffness    = sc.gct.bend;
        p.friction         = sc.gct.friction;
        p.attachStiffness  = sc.gct.attach;
        if (p.dragFactor <= 0.0f) p.dragFactor = sc.gct.drag;
        p.iterations       = qMax(p.iterations, 4);
    }

    // ── animation (WardrobeTab2::decodeAnimByName equivalent, corpus-backed) ──
    if (!animName.isEmpty()) {
        QFile jf(corpus.dir() + QStringLiteral("/anim/") + animName + QStringLiteral(".ani.json"));
        const AssetBlob ab = corpus.anim(animName);
        if (jf.open(QIODevice::ReadOnly) && ab.ok) {
            const QJsonObject root = QJsonDocument::fromJson(jf.readAll()).object();
            const QJsonArray perms = root.value(QStringLiteral("ptPermutations")).toArray();
            if (!perms.isEmpty()) {
                const QJsonObject perm = perms.first().toObject();
                const QJsonObject pv = perm.value(QStringLiteral("ptPayloadData")).toObject()
                                           .value(QStringLiteral("value")).toObject();
                const int offset = pv.value(QStringLiteral("dataOffset")).toInt();
                const int frames = perm.value(QStringLiteral("nKeyframeCount")).toInt();
                const int comp   = perm.value(QStringLiteral("flCompression")).toInt();
                const float fps  = float(perm.value(QStringLiteral("flFrameRate")).toDouble(30.0));
                if (frames > 0) {
                    QHash<quint32, AnimParser::RestTRS> rest;
                    for (const ModelJoint& j : sc.geo.skeleton) {
                        AnimParser::RestTRS t; t.q = j.restQ; t.t = j.restT; t.s = j.restS;
                        rest.insert(j.nameHash, t);
                    }
                    sc.anim = AnimParser::decode(ab.payload, offset, frames, comp, fps, rest);
                    if (sc.anim.valid) {
                        sc.animName = animName;
                        for (int i = 0; i < sc.anim.bones.size(); ++i)
                            sc.animByHash.insert(sc.anim.bones[i].boneHash, i);
                    } else sc.error = QStringLiteral("anim decode failed (non-fatal)");
                }
            }
        } else sc.error = QStringLiteral("anim '%1' not in corpus (non-fatal)").arg(animName);
    }

    sc.ok = true;
    return sc;
}

} // namespace d4cloth
