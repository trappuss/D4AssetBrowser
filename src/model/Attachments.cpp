#include "model/Attachments.h"

#include "model/Hardpoints.h"
#include "model/RigMath.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <cmath>

using RigMath::Mat4;

namespace {

// Is a hardpoint transform AUTHORED (meaningfully non-identity)? Authored transforms are
// model-space rest placements; identity means "use the bone" (see the semantics note in seat()).
bool hpIsAuthored(const Mat4& m)
{
    // Translation part…
    if (std::fabs(m[12]) > 1e-4f || std::fabs(m[13]) > 1e-4f || std::fabs(m[14]) > 1e-4f) return true;
    // …or a rotation part that deviates from identity (diagonal ≠ 1 / off-diagonal ≠ 0).
    return std::fabs(m[0] - 1) > 1e-3f || std::fabs(m[5] - 1) > 1e-3f || std::fabs(m[10] - 1) > 1e-3f
        || std::fabs(m[1]) > 1e-3f || std::fabs(m[4]) > 1e-3f || std::fabs(m[8]) > 1e-3f;
}

QString actorJsonPath(const QString& d4, const QString& actorName)
{
    return d4 + QStringLiteral("/json/base/meta/Actor/") + actorName + QStringLiteral(".acr.json");
}

QJsonObject readJson(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return {};
    return doc.object();
}

// short name from a "…/Appearance/<name>.app" targetFileName
QString apprNameOf(const QJsonObject& ref)
{
    QString nm = ref.value(QStringLiteral("__targetFileName__")).toString().section(QLatin1Char('/'), -1);
    if (nm.endsWith(QLatin1String(".app"))) nm.chop(4);
    return nm;
}

// Resolve a child actor name → (appearance SNO, appearance short name). Cached per call.
struct ChildAppr { int sno = -1; QString name; };
ChildAppr resolveChildAppearance(const QString& d4, const QString& childActor,
                                 QHash<QString, ChildAppr>& cache)
{
    const auto it = cache.constFind(childActor);
    if (it != cache.constEnd()) return it.value();
    ChildAppr r;
    const QJsonObject o = readJson(actorJsonPath(d4, childActor));
    if (!o.isEmpty()) {
        const QJsonObject ap = o.value(QStringLiteral("snoAppearance")).toObject();
        const int sno = ap.value(QStringLiteral("__raw__")).toInt(-1);
        if (sno > 0) { r.sno = sno; r.name = apprNameOf(ap); }
    }
    cache.insert(childActor, r);
    return r;
}

std::array<float, 4> readQuat(const QJsonObject& q)
{
    return {{ float(q.value(QStringLiteral("x")).toDouble()),
              float(q.value(QStringLiteral("y")).toDouble()),
              float(q.value(QStringLiteral("z")).toDouble()),
              float(q.value(QStringLiteral("w")).toDouble(1.0)) }};
}
std::array<float, 3> readVec3(const QJsonObject& v)
{
    return {{ float(v.value(QStringLiteral("x")).toDouble()),
              float(v.value(QStringLiteral("y")).toDouble()),
              float(v.value(QStringLiteral("z")).toDouble()) }};
}

}  // namespace

QString ModelAttach::Attachment::key() const
{
    return parentActor + QLatin1Char('|') + childActor + QLatin1Char('|')
         + QString::number(hpHash) + QLatin1Char('|') + QString::number(msgKey);
}

QString ModelAttach::Attachment::triggerLabel() const
{
    if (isMount)   return QStringLiteral("Mount");
    if (permanent) return QStringLiteral("On spawn (held)");
    return QStringLiteral("Triggered #%1").arg(msgKey);
}

QVector<ModelAttach::Attachment> ModelAttach::scanActor(const QString& d4, const QString& actorName)
{
    QVector<Attachment> out;
    if (d4.isEmpty() || actorName.isEmpty()) return out;
    const QJsonObject o = readJson(actorJsonPath(d4, actorName));
    if (o.isEmpty()) return out;

    QHash<QString, ChildAppr> childCache;
    const QJsonArray events = o.value(QStringLiteral("ptMsgTriggeredEvents")).toArray();
    for (const QJsonValue& ev : events) {
        const QJsonObject e = ev.toObject();
        const quint32 msgKey = quint32(e.value(QStringLiteral("dwMsgKey")).toVariant().toULongLong());
        for (const QJsonValue& tv : e.value(QStringLiteral("ptTriggerEvent")).toArray()) {
            const QJsonObject te = tv.toObject();
            if (te.value(QStringLiteral("__type__")).toString() != QLatin1String("TriggerEventAddObject"))
                continue;
            const QJsonObject sn = te.value(QStringLiteral("snoname")).toObject();
            if (sn.value(QStringLiteral("groupName")).toString() != QLatin1String("Actor"))
                continue;
            const QString childActor = sn.value(QStringLiteral("name")).toString();
            if (childActor.isEmpty()) continue;

            const ChildAppr ca = resolveChildAppearance(d4, childActor, childCache);
            if (ca.sno <= 0) continue;   // child has no mesh → nothing to show/attach

            Attachment a;
            a.parentActor = actorName;
            a.childActor  = childActor;
            a.childApprSno = ca.sno;
            a.childApprName = ca.name;
            a.msgKey = msgKey;
            a.permanent = (msgKey == 1000u);

            // First non-zero hardpoint link is the socket it snaps to.
            for (const QJsonValue& hv : te.value(QStringLiteral("tHardpointLinks")).toArray()) {
                const quint32 h = quint32(hv.toObject().value(QStringLiteral("tInfo")).toObject()
                                            .value(QStringLiteral("dwHash")).toVariant().toULongLong());
                if (h) { a.hpHash = h; break; }
            }
            a.hpName = a.hpHash ? Hardpoints::nameForHash(a.hpHash) : QStringLiteral("(root)");

            const QJsonObject tr = te.value(QStringLiteral("transform")).toObject();
            a.q = readQuat(tr.value(QStringLiteral("q")).toObject());
            a.p = readVec3(tr.value(QStringLiteral("wp")).toObject());
            const double sc = te.value(QStringLiteral("flScale")).toDouble(1.0);
            a.scale = sc > 1e-4 ? float(sc) : 1.0f;

            out.push_back(a);
        }
    }

    // Mount / rider: a mounted NPC references the mount actor it rides via
    // ptMonsterData[0].snoMount. Emit it as a mount attachment (seated via the mount's own
    // saddle hardpoint, not a hardpoint on this rider).
    const QJsonArray md = o.value(QStringLiteral("ptMonsterData")).toArray();
    if (!md.isEmpty()) {
        const QJsonObject mnt = md.first().toObject().value(QStringLiteral("snoMount")).toObject();
        const QString mountActor = mnt.value(QStringLiteral("name")).toString();
        if (!mountActor.isEmpty()
            && mnt.value(QStringLiteral("groupName")).toString() == QLatin1String("Actor")) {
            const ChildAppr ca = resolveChildAppearance(d4, mountActor, childCache);
            if (ca.sno > 0) {
                Attachment a;
                a.parentActor  = actorName;
                a.childActor   = mountActor;
                a.childApprSno = ca.sno;
                a.childApprName = ca.name;
                a.isMount   = true;
                a.permanent = true;                       // a mount is a persistent relationship
                a.hpName    = QStringLiteral("saddle");
                out.push_back(a);
            }
        }
    }
    return out;
}

QHash<quint32, QPair<int, std::array<float, 16>>>
ModelAttach::loadHardpointMap(const QString& appJsonPath)
{
    QHash<quint32, QPair<int, std::array<float, 16>>> out;
    const QJsonObject o = readJson(appJsonPath);
    if (o.isEmpty()) return out;
    const QJsonArray bd = o.value(QStringLiteral("tStructure")).toObject()
                              .value(QStringLiteral("ptBoneData")).toArray();
    if (bd.isEmpty()) return out;
    const QJsonArray hps = bd.at(0).toObject().value(QStringLiteral("ptHardpoints")).toArray();
    for (const QJsonValue& hv : hps) {
        const QJsonObject h = hv.toObject();
        const int bone = h.value(QStringLiteral("nBoneIndex")).toInt(-1);
        if (bone < 0) continue;
        const quint32 hash = quint32(h.value(QStringLiteral("tInfo")).toObject()
                                         .value(QStringLiteral("dwHash")).toVariant().toULongLong());
        if (!hash) continue;
        const QJsonObject t  = h.value(QStringLiteral("transform")).toObject();
        const std::array<float, 4> qa = readQuat(t.value(QStringLiteral("q")).toObject());
        const std::array<float, 3> pa = readVec3(t.value(QStringLiteral("wp")).toObject());
        out.insert(hash, qMakePair(bone, RigMath::quatPosMat(qa, pa)));
    }
    return out;
}

bool ModelAttach::seat(ModelGeometry& childGeo,
                       const QVector<ModelJoint>& parentSkel,
                       const QHash<quint32, QPair<int, std::array<float, 16>>>& hpMap,
                       const Attachment& a)
{
    // Grip offset from the trigger transform.
    Mat4 Toff = RigMath::quatPosMat(a.q, a.p);
    // The right-weapon hardpoint (HP_rightWeapon) is identity-local on the shared rigs, so a
    // weapon seated there lands rolled 180° about its grip axis (upside down) versus the
    // (correct) left-hand mirror — the difference is the hand-bone roll. Apply a half-turn about
    // Y so the main-hand attachment is upright. Same fix the wardrobe applies to main-hand weapons.
    if (a.hpHash == 3636304447u) {
        static const Mat4 kHeld{{ -1, 0, 0, 0,  0, 1, 0, 0,  0, 0, -1, 0,  0, 0, 0, 1 }};
        Toff = RigMath::mat4mul(Toff, kHeld);
    }

    int bone = -1;
    Mat4 hpMat{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    bool resolved = false;
    if (a.hpHash && hpMap.contains(a.hpHash)) {
        const QPair<int, std::array<float, 16>> bh = hpMap.value(a.hpHash);
        bone = bh.first;
        hpMat = bh.second;
        resolved = true;
    }

    const bool haveBone = resolved && bone >= 0 && bone < parentSkel.size();
    // Hardpoint-transform semantics (verified against d4data — mnt_stor061_horse: saddle
    // (0.008,0,1.544), mouth (1.49,0,1.55), four hooves at z≈0, HP_UI (0,0,1.8), perfect L/R
    // mirror symmetry): an AUTHORED (non-identity) transform is the hardpoint's rest placement
    // in MODEL space, and nBoneIndex is only the bone it FOLLOWS during animation. An IDENTITY
    // transform (player rigs: HP_rightWeapon…) means "the bone itself". Multiplying boneWorld ×
    // modelSpaceTransform double-transformed every mount socket (the scattered-gizmos bug).
    const bool authored = hpIsAuthored(hpMat);
    const Mat4 world = (haveBone && !authored)
                           ? RigMath::jointWorldMat(parentSkel, bone)
                           : Mat4{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    const Mat4 Mz = RigMath::mat4mul(RigMath::mat4mul(world, hpMat), Toff);   // z-up attach
    const Mat4 M  = RigMath::mat4mul(RigMath::mat4mul(RigMath::kSwapZtoY, Mz), RigMath::kSwapYtoZ);  // → y-up

    const float s = a.scale;
    for (MeshPrimitive& prim : childGeo.primitives)
        for (MeshVertex& v : prim.vertices) {
            // Uniform scale about the child's own origin, then place at the hardpoint.
            const float x = v.px * s, y = v.py * s, z = v.pz * s;
            v.px = M[0] * x + M[4] * y + M[8]  * z + M[12];
            v.py = M[1] * x + M[5] * y + M[9]  * z + M[13];
            v.pz = M[2] * x + M[6] * y + M[10] * z + M[14];
            const float nx = v.nx, ny = v.ny, nz = v.nz;
            float rx = M[0] * nx + M[4] * ny + M[8]  * nz;
            float ry = M[1] * nx + M[5] * ny + M[9]  * nz;
            float rz = M[2] * nx + M[6] * ny + M[10] * nz;
            const float len = std::sqrt(rx * rx + ry * ry + rz * rz);
            if (len > 1e-8f) { rx /= len; ry /= len; rz /= len; }
            v.nx = rx; v.ny = ry; v.nz = rz;
            if (haveBone) {
                v.joints[0] = quint16(bone); v.joints[1] = v.joints[2] = v.joints[3] = 0;
                v.weights[0] = 1.0f; v.weights[1] = v.weights[2] = v.weights[3] = 0.0f;
            } else {
                // Static: zero the joints too (the child's own rig is cleared below, so its
                // original indices would be out of range against the merged base skeleton).
                v.joints[0] = v.joints[1] = v.joints[2] = v.joints[3] = 0;
                v.weights[0] = v.weights[1] = v.weights[2] = v.weights[3] = 0.0f;
            }
        }
    // Its verts now reference the parent bone index directly (or are static), so drop the
    // child's own rig — the merge must keep those indices as-is.
    childGeo.skeleton.clear();
    return resolved;
}

bool ModelAttach::seatMount(ModelGeometry& mountGeo,
                            const QHash<quint32, QPair<int, std::array<float, 16>>>& mountHpMap,
                            const QVector<ModelJoint>& mountSkel)
{
    constexpr quint32 kSaddle = 1401728324u;   // HP_saddle — the rider seat on mount rigs
    if (!mountHpMap.contains(kSaddle)) return false;   // no seat → caller skips the mount
    const QPair<int, std::array<float, 16>> bh = mountHpMap.value(kSaddle);
    const int bone = bh.first;
    // Saddle world in the mount's own (z-up) space. An authored (non-identity) transform IS the
    // model-space rest placement (see seat()); only an identity transform means "the bone itself".
    const Mat4 saddle = hpIsAuthored(bh.second)
                            ? bh.second
                            : ((bone >= 0 && bone < mountSkel.size())
                                   ? RigMath::jointWorldMat(mountSkel, bone) : bh.second);
    // Move the mount so the saddle lands at the origin (where the rider stands), y-up mesh space.
    const Mat4 place = RigMath::invertRigid(saddle);
    const Mat4 M = RigMath::mat4mul(RigMath::mat4mul(RigMath::kSwapZtoY, place), RigMath::kSwapYtoZ);
    for (MeshPrimitive& prim : mountGeo.primitives)
        for (MeshVertex& v : prim.vertices) {
            const float x = v.px, y = v.py, z = v.pz;
            v.px = M[0] * x + M[4] * y + M[8]  * z + M[12];
            v.py = M[1] * x + M[5] * y + M[9]  * z + M[13];
            v.pz = M[2] * x + M[6] * y + M[10] * z + M[14];
            const float nx = v.nx, ny = v.ny, nz = v.nz;
            float rx = M[0] * nx + M[4] * ny + M[8]  * nz;
            float ry = M[1] * nx + M[5] * ny + M[9]  * nz;
            float rz = M[2] * nx + M[6] * ny + M[10] * nz;
            const float len = std::sqrt(rx * rx + ry * ry + rz * rz);
            if (len > 1e-8f) { rx /= len; ry /= len; rz /= len; }
            v.nx = rx; v.ny = ry; v.nz = rz;
            v.joints[0] = v.joints[1] = v.joints[2] = v.joints[3] = 0;   // static (separate rig)
            v.weights[0] = v.weights[1] = v.weights[2] = v.weights[3] = 0.0f;
        }
    mountGeo.skeleton.clear();
    return true;
}
