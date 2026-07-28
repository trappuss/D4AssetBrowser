#pragma once
#include "model/ModelGeometry.h"

#include <QHash>
#include <QPair>
#include <QString>
#include <QVector>
#include <array>

// Reads the D4 "actor holds/attaches another model" system and seats a child mesh
// onto the parent's rig hardpoint.
//
// In game data an Actor attaches other models through ptMsgTriggeredEvents[]: each
// event has a dwMsgKey (WHEN it fires) and one or more ptTriggerEvent[] entries. A
// TriggerEventAddObject entry spawns/attaches a child Actor (snoname → its own
// snoAppearance = the mesh) at a hardpoint (tHardpointLinks[].tInfo.dwHash) using a
// grip transform (transform.q / transform.wp) and flScale.
//   dwMsgKey == 1000  → the on-spawn / permanent attach (a held weapon/shield/prop).
//   other keys        → combat/animation-triggered spawns (projectiles, effect actors).
namespace ModelAttach {

struct Attachment {
    QString parentActor;                     // actor that owns this attach event
    QString childActor;                      // attached child actor name
    int     childApprSno = -1;               // child actor's snoAppearance (the mesh) SNO
    QString childApprName;                   // child appearance short name (for display)
    quint32 hpHash = 0;                       // hardpoint hash (0 ⇒ unparented/at origin)
    QString hpName;                           // human-readable hardpoint (HP_rightWeapon…)
    std::array<float, 4> q {{0, 0, 0, 1}};   // grip rotation (x,y,z,w), D4-native
    std::array<float, 3> p {{0, 0, 0}};      // grip translation, D4-native
    float   scale = 1.0f;                     // flScale
    quint32 msgKey = 0;                       // dwMsgKey (trigger key)
    bool    permanent = false;                // msgKey == 1000 (held on spawn)
    bool    isMount = false;                  // this actor RIDES childActor (ptMonsterData.snoMount);
                                              // seat via the mount's own saddle hardpoint, not hpHash

    // Stable identity for UI rows / settings (parent+child+hardpoint+key).
    QString key() const;
    // Short "when" label derived from msgKey ("On spawn (held)", "Triggered #<key>").
    QString triggerLabel() const;
};

// Parse one actor's .acr.json (…/json/base/meta/Actor/<actorName>.acr.json) and return
// every TriggerEventAddObject that references a mesh-bearing child Actor. Child
// appearance SNO/name are resolved by reading the child actor's snoAppearance.
// `d4` is Config::d4dataDir(). Safe to call off the UI thread (pure file I/O).
QVector<Attachment> scanActor(const QString& d4, const QString& actorName);

// hardpoint hash → (parent bone index, bone-local matrix) for a parent appearance,
// read from <appJsonPath> tStructure.ptBoneData[0].ptHardpoints. Bone indices match
// the order ModelParser::parseApp produced the skeleton in.
QHash<quint32, QPair<int, std::array<float, 16>>> loadHardpointMap(const QString& appJsonPath);

// Bake `childGeo` onto the parent rig at attachment `a`'s hardpoint, IN PLACE:
// world = boneWorld · hpLocal · gripOffset (re-expressed in y-up mesh space), applied
// to the child's verts/normals; verts are pinned 100% to the attach bone so they track
// it during animation (parent skeleton merged first keeps the index valid). For a static
// parent (empty skeleton / bone out of range) the child is placed but left unweighted.
// Returns true if the hardpoint resolved; false ⇒ child left at its own origin (still usable).
bool seat(ModelGeometry& childGeo,
          const QVector<ModelJoint>& parentSkel,
          const QHash<quint32, QPair<int, std::array<float, 16>>>& hpMap,
          const Attachment& a);

// Bone-name hashes of an attached sub-rig are remapped through this so mergeGeometries — which
// unifies skeletons BY hash — cannot fuse them onto same-named parent bones. Shipped back trophies
// share up to 5 of their bones with the 293-bone character rig AND their clips drive exactly those,
// so fusing would let a trophy's idle drag the character's spine around.
//
// The SAME function must be applied to a clip's DecodedBone::boneHash before that clip can drive an
// attached sub-rig, because clips bind to bones by hash. Exposed for precisely that reason.
inline quint32 saltBoneHash(quint32 h, quint32 salt)
{
    return (h * 2654435761u) ^ (salt + 0x9E3779B9u);
}

// Attach `childGeo` at a hardpoint while KEEPING its own rig, IN PLACE — the counterpart to seat()
// for a child that has to keep moving after it is attached.
//
// seat() bakes the child's vertices into model space and clears its skeleton: right for a static
// prop, fatal for anything animatable. This instead re-expresses the child's ROOT bones relative to
// the attach bone, so at rest the mesh lands exactly where seat() would have put it while every
// bone survives and can still be driven.
//
// Entirely in D4-native (z-up) space: jointWorldMat and the hardpoint transform are both native,
// and the renderer applies the z-up→y-up swap itself when it rebuilds each bone from its rest TRS.
// Mixing the two spaces is the single easiest way to get this wrong.
//
//   worldWanted = Mz · worldOld,  worldNew = attachWorld · localNew
//     ⇒ localNew = attachWorld⁻¹ · Mz · localOld        (a root's local IS its world)
//
// Writes the rest TRS, not just localMatrix: the viewport recomposes every bone from restQ/restT/
// restS each frame and never reads localMatrix, so writing only the matrix places nothing.
//
// Returns false (childGeo untouched) when the child has no rig or the hardpoint does not resolve —
// the caller should fall back to seat().
// `outPreSalt`, when given, receives the child's skeleton as it was BEFORE re-hashing. A clip must
// be decoded against those original hashes: the decoder uses the rig only to supply a per-bone rest
// pose for channels the clip leaves empty (D4 authors rotation-only tracks routinely), and decoding
// against the salted rig silently substitutes a zero translation for every bone — collapsing the
// child's chain to a point the moment it plays.
// As attachSubRig, but with the placement SUPPLIED rather than derived from a hardpoint: `Mz` is
// the child's attach transform in D4-native (z-up) space and `bone` the parent bone it follows.
//
// Exists so a caller that already has proven placement logic can reuse it verbatim. Weapon seating
// resolves hand, grip offset, mirror, held-roll and an auto-upright correction before arriving at
// its matrix; re-deriving any of that here would be a second implementation to keep in step, and
// the placement could silently drift between the baked and rigged paths.
bool attachSubRigAt(ModelGeometry& childGeo,
                    const QVector<ModelJoint>& parentSkel,
                    int bone,
                    const std::array<float, 16>& Mz,
                    quint32 salt,
                    QVector<ModelJoint>* outPreSalt = nullptr);

bool attachSubRig(ModelGeometry& childGeo,
                  const QVector<ModelJoint>& parentSkel,
                  const QHash<quint32, QPair<int, std::array<float, 16>>>& hpMap,
                  quint32 hpHash,
                  quint32 salt,
                  QVector<ModelJoint>* outPreSalt = nullptr);

// Place a MOUNT mesh under its rider, IN PLACE: the rider stays at the origin and the mount is
// positioned so its own saddle hardpoint (HP_saddle) lands at the origin — i.e. the mount is
// transformed by inverse(saddleWorld). `mountHpMap`/`mountSkel` are the MOUNT's own hardpoint map
// and skeleton (not the rider's). Verts are baked static (the mount is a separate rig). Returns
// false if the mount has no saddle hardpoint (caller then skips it).
bool seatMount(ModelGeometry& mountGeo,
               const QHash<quint32, QPair<int, std::array<float, 16>>>& mountHpMap,
               const QVector<ModelJoint>& mountSkel);

}  // namespace ModelAttach
