#include "model/Hardpoints.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

// Verified hardpoint hash → name (D4_BoneHash_Research_Report.md; DJB2 seed-0 of the
// authored HP_* string). 118 entries mined from Diablo IV.exe + real model ptHardpoints.
struct HpName { quint32 hash; const char* name; };
const HpName kHardpointNames[] = {
        { 13337820u, "HP_bladeTip" },
        { 64762685u, "HP_leftShoulderPad" },
        { 127468149u, "HP_UI" },
        { 274763203u, "HP_rightBackSheath" },
        { 283867573u, "HP_ropeAllKnots" },
        { 290178321u, "HP_leftWeapon:HP_physics1" },
        { 290178322u, "HP_leftWeapon:HP_physics2" },
        { 290178323u, "HP_leftWeapon:HP_physics3" },
        { 290178324u, "HP_leftWeapon:HP_physics4" },
        { 372641935u, "HP_ropeSoundFollowListener" },
        { 390646174u, "HP_2HScytheSheath" },
        { 439492682u, "HP_bladeBase" },
        { 543141696u, "HP_leftHipSheath" },
        { 550415160u, "HP_explosion" },
        { 585752080u, "HP_leftBackSheath" },
        { 605037056u, "HP_rightAnkle" },
        { 609699182u, "HP_rightElbow" },
        { 720395180u, "HP_attach" },
        { 760191383u, "HP_leftBeam" },
        { 760340088u, "HP_leftFist" },
        { 760346490u, "HP_leftFoot" },
        { 760403069u, "HP_leftHand" },
        { 760524741u, "HP_leftKnee" },
        { 771461588u, "HP_leftScabbard" },
        { 780682296u, "HP_center" },
        { 825086260u, "HP_Unique_Invulnerable" },
        { 899481535u, "HP_chestBack" },
        { 924169363u, "HP_rightHipSheath" },
        { 926930060u, "HP_uniqueFX" },
        { 965785918u, "HP_trail_L" },
        { 965785924u, "HP_trail_R" },
        { 975883853u, "HP_health" },
        { 982636814u, "HP_trophy1" },
        { 982636815u, "HP_trophy2" },
        { 982636816u, "HP_trophy3" },
        { 1022868561u, "HP_interact" },
        { 1103621372u, "HP_playerLight" },
        { 1144786049u, "HP_lookAt" },
        { 1289372842u, "HP_pelvis" },
        { 1327944173u, "HP_shieldSheath" },
        { 1347375027u, "HP_Quiver" },
        { 1373172648u, "HP_back" },
        { 1373176940u, "HP_beam" },
        { 1373392553u, "HP_head" },
        { 1373619660u, "HP_nose" },
        { 1410062708u, "HP_sheath" },
        { 1410210544u, "HP_shield" },
        { 1433762267u, "HP_rightShoulder" },
        { 1448601124u, "HP_handleCenter" },
        { 1459763858u, "HP_leftWeapon:HP_uniqueFX" },
        { 1460921956u, "HP_trail1" },
        { 1460921957u, "HP_trail2" },
        { 1460921958u, "HP_trail3" },
        { 1460921959u, "HP_trail4" },
        { 1466758314u, "HP_playerCard_Mini" },
        { 1495610222u, "HP_Unique" },
        { 1537978424u, "HP_UIAnimated" },
        { 1644812451u, "HP_characterCreate_camera_hair" },
        { 1803764157u, "HP_2hSwordSheath" },
        { 1909880016u, "HP_bladeCenter" },
        { 1969125202u, "HP_conversation" },
        { 1970618122u, "HP_rightBeam" },
        { 1970766827u, "HP_rightFist" },
        { 1970773229u, "HP_rightFoot" },
        { 1970829808u, "HP_rightHand" },
        { 1970951480u, "HP_rightKnee" },
        { 2051394495u, "HP_payloadImpact" },
        { 2269064350u, "HP_handleBase" },
        { 2331521859u, "HP_leftExplosion" },
        { 2338619876u, "HP_poleSheath" },
        { 2366464462u, "HP_chest" },
        { 2377175503u, "HP_light" },
        { 2378592676u, "HP_mouth" },
        { 2385866104u, "HP_state" },
        { 2506348224u, "HP_characterCreate_camera" },
        { 2541639344u, "HP_handleTip" },
        { 2554626651u, "HP_damageText" },
        { 2567206852u, "HP_rightWeapon:HP_physics1" },
        { 2567206853u, "HP_rightWeapon:HP_physics2" },
        { 2567206854u, "HP_rightWeapon:HP_physics3" },
        { 2567206855u, "HP_rightWeapon:HP_physics4" },
        { 2606436928u, "HP_leftHipSheath_alt" },
        { 2687031632u, "HP_rightShoulderPad" },
        { 2712549526u, "HP_rightExplosion" },
        { 2727917030u, "HP_projectileImpact" },
        { 2732222941u, "HP_characterCreate_camera_beard" },
        { 2826306581u, "HP_attached" },
        { 2828868015u, "HP_manaGather" },
        { 2882817489u, "HP_emitter" },
        { 2888878652u, "HP_ropeKnots" },
        { 2995566791u, "HP_rightScabbard" },
        { 3003160993u, "HP_Follower" },
        { 3016501306u, "HP_leftEar" },
        { 3092546280u, "HP_staffSheath" },
        { 3162864563u, "HP_emitterFloor" },
        { 3471337311u, "HP_ropeHead" },
        { 3471764471u, "HP_ropeTail" },
        { 3483596046u, "HP_rightShield" },
        { 3504624360u, "HP_leftShoulder" },
        { 3610627629u, "HP_leftAnkle" },
        { 3615289755u, "HP_leftElbow" },
        { 3636304447u, "HP_rightWeapon" },
        { 3685380679u, "HP_Follower_Invulnerable" },
        { 3736792389u, "HP_rightWeapon:HP_uniqueFX" },
        { 3814428920u, "HP_characterCreate" },
        { 3824338910u, "HP_Invulnerable" },
        { 3883837147u, "HP_leftShield" },
        { 3918454775u, "HP_chestFront" },
        { 3964234573u, "HP_rightEar" },
        { 4036545548u, "HP_leftWeapon" },
        { 4052311819u, "HP_physics1" },
        { 4052311820u, "HP_physics2" },
        { 4052311821u, "HP_physics3" },
        { 4052311822u, "HP_physics4" },
        { 4054930511u, "HP_centroid" },
        { 4168202000u, "HP_spineMid" },
        { 4206448138u, "HP_top" },
        { 4230212172u, "HP_portrait" },
};

}  // namespace

QString Hardpoints::nameForHash(quint32 hash)
{
    static const QHash<quint32, QString> kMap = [] {
        QHash<quint32, QString> m;
        for (const HpName& e : kHardpointNames) m.insert(e.hash, QString::fromLatin1(e.name));
        return m;
    }();
    const auto it = kMap.constFind(hash);
    if (it != kMap.constEnd()) return it.value();
    return QStringLiteral("HP_%1").arg(hash, 8, 16, QLatin1Char('0'));
}

int Hardpoints::readInto(ModelGeometry& geo, const QString& appJsonPath)
{
    geo.hardpoints.clear();   // idempotent: never accumulate across repeated calls on one geometry
    const int nb = geo.skeleton.size();
    if (nb == 0) return 0;
    QFile f(appJsonPath);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonArray bd = root.value(QStringLiteral("tStructure")).toObject()
                              .value(QStringLiteral("ptBoneData")).toArray();
    if (bd.isEmpty()) return 0;
    const QJsonArray hps = bd.first().toObject().value(QStringLiteral("ptHardpoints")).toArray();
    int added = 0;
    for (const QJsonValue& hv : hps) {
        const QJsonObject h = hv.toObject();
        const int bone = h.value(QStringLiteral("nBoneIndex")).toInt(-1);
        if (bone < 0 || bone >= nb) continue;                  // unparented / out of range
        const quint32 hash = quint32(h.value(QStringLiteral("tInfo")).toObject()
                                      .value(QStringLiteral("dwHash")).toVariant().toULongLong());
        const QJsonObject tr = h.value(QStringLiteral("transform")).toObject();
        const QJsonObject q = tr.value(QStringLiteral("q")).toObject();
        const QJsonObject p = tr.value(QStringLiteral("wp")).toObject();
        ModelHardpoint hp;
        hp.name = Hardpoints::nameForHash(hash);
        hp.boneIndex = bone;
        hp.boneHash = geo.skeleton[bone].nameHash;             // survives later reorder
        hp.q = {{ float(q.value(QStringLiteral("x")).toDouble()),
                  float(q.value(QStringLiteral("y")).toDouble()),
                  float(q.value(QStringLiteral("z")).toDouble()),
                  float(q.value(QStringLiteral("w")).toDouble(1.0)) }};
        hp.t = {{ float(p.value(QStringLiteral("x")).toDouble()),
                  float(p.value(QStringLiteral("y")).toDouble()),
                  float(p.value(QStringLiteral("z")).toDouble()) }};
        geo.hardpoints.push_back(hp);
        ++added;
    }
    return added;
}

void Hardpoints::resolveBoneIndices(ModelGeometry& geo)
{
    if (geo.hardpoints.isEmpty()) return;
    QHash<quint32, int> byHash;
    for (int i = 0; i < geo.skeleton.size(); ++i)
        byHash.insert(geo.skeleton[i].nameHash, i);
    QVector<ModelHardpoint> kept;
    kept.reserve(geo.hardpoints.size());
    for (ModelHardpoint hp : geo.hardpoints) {
        const int idx = byHash.value(hp.boneHash, -1);
        if (idx < 0) continue;                 // parent bone gone (remap/collapse) → drop
        hp.boneIndex = idx;
        kept.push_back(hp);
    }
    geo.hardpoints = kept;
}
