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

// Place a MOUNT mesh under its rider, IN PLACE: the rider stays at the origin and the mount is
// positioned so its own saddle hardpoint (HP_saddle) lands at the origin — i.e. the mount is
// transformed by inverse(saddleWorld). `mountHpMap`/`mountSkel` are the MOUNT's own hardpoint map
// and skeleton (not the rider's). Verts are baked static (the mount is a separate rig). Returns
// false if the mount has no saddle hardpoint (caller then skips it).
bool seatMount(ModelGeometry& mountGeo,
               const QHash<quint32, QPair<int, std::array<float, 16>>>& mountHpMap,
               const QVector<ModelJoint>& mountSkel);

}  // namespace ModelAttach
