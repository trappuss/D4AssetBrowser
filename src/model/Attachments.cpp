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

bool ModelAttach::attachSubRig(ModelGeometry& childGeo,
                              const QVector<ModelJoint>& parentSkel,
                              const QHash<quint32, QPair<int, std::array<float, 16>>>& hpMap,
                              quint32 hpHash,
                              quint32 salt,
                              QVector<ModelJoint>* outPreSalt)
{
    if (childGeo.skeleton.isEmpty() || !hpHash || !hpMap.contains(hpHash)) return false;
    const QPair<int, std::array<float, 16>> bh = hpMap.value(hpHash);
    const int bone = bh.first;
    if (bone < 0 || bone >= parentSkel.size()) return false;
    const Mat4 hpMat = bh.second;

    // ALL NATIVE (z-up). jointWorldMat and the hardpoint transform are both native, and the
    // renderer applies the z-up→y-up swap itself when it rebuilds a bone from its rest TRS.
    // Conjugation is a homomorphism, so a native premultiply survives that swap intact.
    // Same authored-vs-identity rule seat() uses, so both paths agree on where the socket is.
    const Mat4 attachWorld0 = RigMath::jointWorldMat(parentSkel, bone);
    const Mat4 base = hpIsAuthored(hpMat)
                          ? Mat4{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}
                          : attachWorld0;
    return attachSubRigAt(childGeo, parentSkel, bone, RigMath::mat4mul(base, hpMat), salt, outPreSalt);
}

bool ModelAttach::attachSubRigAt(ModelGeometry& childGeo,
                                 const QVector<ModelJoint>& parentSkel,
                                 int bone,
                                 const std::array<float, 16>& Mz,
                                 quint32 salt,
                                 QVector<ModelJoint>* outPreSalt,
                                 bool physicsChain)
{
    if (childGeo.skeleton.isEmpty()) return false;
    if (bone < 0 || bone >= parentSkel.size()) return false;
    // A cage with no real particles claims no bones, so the cloth flag would only enrol them
    // unconstrained. Checked BEFORE the rig is rebuilt so the decision is on the authored data.
    bool hasUsableCage = false;
    for (const ClothSim& s : childGeo.clothSims)
        if (s.vertCount > 0 && !s.bindVerts.isEmpty() && !s.constraintLen.isEmpty())
            { hasUsableCage = true; break; }
    int chainBones = 0;
    if (outPreSalt) *outPreSalt = childGeo.skeleton;
    const Mat4 attachWorld = RigMath::jointWorldMat(parentSkel, bone);
    const Mat4 rootPre = RigMath::mat4mul(RigMath::invert(attachWorld), Mz);

    QVector<ModelJoint> out;
    out.reserve(childGeo.skeleton.size() + 2);

    // [0] the attach bone, ORIGINAL hash so mergeGeometries fuses it onto the parent's copy — that
    // fusion is what makes the child inherit parent animation.
    ModelJoint anchor = parentSkel[bone];
    anchor.parent = -1;
    anchor.cloth  = false;
    out.append(anchor);

    // [1] a dedicated PLACEMENT bone holding rootPre.
    //
    // This cannot live on the child's own roots. The renderer prefers a clip track over the rest
    // pose, and the child's clip is salted to bind to exactly those roots — so a root carrying the
    // placement in its rest TRS would have it overwritten the instant the clip played, dropping the
    // trophy back onto the raw spine bone. A bone the clip cannot address is the only place the
    // placement is safe.
    ModelJoint place;
    place.name     = QStringLiteral("bt_attach");
    place.nameHash = saltBoneHash(0xB7A77AC4u, salt);   // fixed sentinel: no clip can target it
    place.parent   = 0;
    place.cloth    = false;
    RigMath::decomposeTRS(rootPre, place.restT, place.restQ, place.restS);
    place.localMatrix = RigMath::mat4mul(RigMath::mat4mul(RigMath::kSwapZtoY, rootPre),
                                         RigMath::kSwapYtoZ);
    place.inverseBind = anchor.inverseBind;   // unused: no vertex is weighted to this bone
    out.append(place);

    for (int i = 0; i < childGeo.skeleton.size(); ++i) {
        ModelJoint j = childGeo.skeleton[i];
        const int p = j.parent;
        const bool isRoot = (p < 0 || p >= childGeo.skeleton.size());
        j.parent = isRoot ? 1 : p + 2;         // roots hang off the placement bone
        // Rest TRS deliberately UNCHANGED — the placement lives on the bone above, so a clip track
        // for this bone replaces only the child's own authored pose, never the attachment.
        j.nameHash = saltBoneHash(j.nameHash, salt);
        j.name = QStringLiteral("bt_") + j.name;
        // Keep the authored cloth flag ONLY when this attachment actually ships a cage the solver
        // can use — ClothSim::spaceBone (set below) tells it where that cage lives, so the
        // proximity match now succeeds in the child's own frame instead of failing silently metres
        // away. Without a cage the bones would enrol with no constraints and no pins: the
        // documented free-fall. Those stay animation-driven, which is what the bake did.
        //
        // A physics chain is the deliberate exception: no cage exists to claim these bones, but the
        // solver's cage-less path is exactly the treatment they want (pose tracking on a short
        // leash), so the flag goes ON for everything below the root rather than off.
        if (!hasUsableCage) j.cloth = (physicsChain && !isRoot);
        if (j.cloth && physicsChain && !hasUsableCage) { j.chain = true; ++chainBones; }
        out.append(j);
    }

    // Every index into the child's OWN skeleton shifts by TWO for the prepended anchor + placement.
    for (MeshPrimitive& prim : childGeo.primitives)
        for (MeshVertex& v : prim.vertices) {
            float wsum = 0.0f;
            for (int k = 0; k < 4; ++k) wsum += v.weights[k];
            if (wsum <= 0.0f) {
                // seat() baked unweighted verts into place; skinning skips them entirely, so they
                // would be left behind at the child's origin. Bind them rigidly to the placement
                // bone, which carries exactly the transform the bake would have applied.
                v.joints[0] = 1; v.joints[1] = v.joints[2] = v.joints[3] = 0;
                v.weights[0] = 1.0f; v.weights[1] = v.weights[2] = v.weights[3] = 0.0f;
                continue;
            }
            for (int k = 0; k < 4; ++k)
                v.joints[k] = (v.weights[k] > 0.0f) ? quint16(v.joints[k] + 2) : quint16(0);
        }
    for (ClothCapsule& c : childGeo.clothCapsules) if (c.boneIndex >= 0) c.boneIndex += 2;
    for (ClothSim& sim : childGeo.clothSims) {
        for (ClothPlane& pl : sim.planes) if (pl.boneIndex >= 0) pl.boneIndex += 2;
        for (int& b : sim.followerBone) if (b >= 0) b += 2;
        for (int& b : sim.drvBone)      if (b >= 0) b += 2;
        // The cage stays in the child's own coordinates; bone [1] is exactly the transform that
        // takes those coordinates to where the rig now sits, so the solver can do the conversion
        // itself rather than us rewriting every particle here.
        sim.spaceBone = 1;
    }
    if (physicsChain)
        qInfo("attach: physics chain — %d of %d bone(s) spring from the grip",
              chainBones, int(out.size()) - 2);
    for (ModelHardpoint& h : childGeo.hardpoints) {
        if (h.boneIndex >= 0) h.boneIndex += 2;
        if (h.boneHash) h.boneHash = saltBoneHash(h.boneHash, salt);
    }
    // Not consumed for an attached child today, but wrong the moment this is reused elsewhere.
    for (int& b : childGeo.pinnedBones) if (b >= 0) b += 2;
    if (childGeo.nBaseBones > 0) childGeo.nBaseBones += 2;

    childGeo.skeleton = out;
    return true;
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
