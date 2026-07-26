#include "model/Material.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
const QHash<int, QString>& slotRoles()
{
    static const QHash<int, QString> kRoles = {
        {1,  "BASE_COLOR"}, {3,  "NORMAL"}, {11, "BASE_COLOR"}, {13, "BASE_COLOR"},
        {19, "BASE_COLOR"}, {47, "NORMAL"}, {48, "NORMAL"}, {54, "DYE_MASK"},
        {56, "DYE_RAMP"}, {62, "ROUGHNESS"}, {63, "METALLIC"}, {81, "AO"},
        {86, "EMISSIVE"}, {96, "MASK_PRIMARY"}, {97, "NOISE_PROCEDURAL"},
        {104, "TRANSLUCENCY"}, {108, "DYE_MASK_2"}, {112, "ROUGHNESS"},
        {113, "ROUGHNESS"}, {145, "SKIN_MASK"}, {212, "DETAIL_NORMAL"},
        {213, "DETAIL_NORMAL"}, {214, "DETAIL_NORMAL"}, {218, "DETAIL_ROUGHNESS"},
        {219, "DETAIL_ROUGHNESS"}, {220, "DETAIL_ROUGHNESS"},   // Detail Map 3 roughness
    };
    return kRoles;
}
}

QString shaderSlotRole(int slot)
{
    auto it = slotRoles().constFind(slot);
    return it != slotRoles().constEnd() ? it.value() : QStringLiteral("SLOT_%1").arg(slot);
}

QVector<MatTexture> parseMaterialJson(const QByteArray& json)
{
    QVector<MatTexture> out;
    if (json.isEmpty())
        return out;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return out;

    const QJsonObject uber = doc.object().value(QStringLiteral("tUberMaterial")).toObject();
    const QJsonArray list = uber.value(QStringLiteral("ptMatTexList")).toArray();
    for (const QJsonValue& v : list) {
        const QJsonObject e = v.toObject();
        const QJsonObject mt = e.value(QStringLiteral("tMatTex")).toObject();
        const QJsonObject sno = mt.value(QStringLiteral("snoTex")).toObject();
        MatTexture t;
        t.slot    = e.value(QStringLiteral("eShaderTex")).toInt();
        t.role    = shaderSlotRole(t.slot);
        t.texName = sno.value(QStringLiteral("name")).toString();
        t.texSno  = qint64(sno.value(QStringLiteral("__raw__")).toDouble());
        // Per-map tiling (ptTexAnim[0].flUScale/flVScale). Detail maps carry the real scale
        // (e.g. 6-20); base colour/normal are 1. Default 1 when absent.
        const QJsonArray anim = mt.value(QStringLiteral("ptTexAnim")).toArray();
        for (const QJsonValue& av : anim) {
            const QJsonObject a = av.toObject();
            t.uScale = float(a.value(QStringLiteral("flUScale")).toDouble(1.0));
            t.vScale = float(a.value(QStringLiteral("flVScale")).toDouble(1.0));
            break;   // first entry only
        }
        if (!t.texName.isEmpty() || t.texSno != 0)
            out.append(t);
    }
    return out;
}

MaterialValues parseMaterialValues(const QByteArray& json)
{
    MaterialValues v;
    if (json.isEmpty())
        return v;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return v;

    const QJsonObject uber = doc.object().value(QStringLiteral("tUberMaterial")).toObject();
    const QJsonArray blocks = uber.value(QStringLiteral("ptRunTimeMaterialValues")).toArray();
    for (const QJsonValue& bv : blocks) {
        const QJsonObject b = bv.toObject();
        for (const char* arrName : {"arMaterialScalarValues", "arMaterialVectorValues"}) {
            const QJsonArray arr = b.value(QLatin1String(arrName)).toArray();
            for (const QJsonValue& sv : arr) {
                const QJsonObject tv = sv.toObject().value(QStringLiteral("tValue")).toObject();
                const quint64 sno = quint64(tv.value(QStringLiteral("snoMaterialValue"))
                                                .toObject().value(QStringLiteral("__raw__")).toDouble());
                const QJsonValue val = tv.value(QStringLiteral("value"));
                switch (sno) {
                    case 142332: v.metal = float(val.toDouble()); v.hasMetal = true; break;
                    case 142333: v.rough = float(val.toDouble()); v.hasRough = true; break;
                    case 455616: v.ao    = float(val.toDouble()); v.hasAO = true; break;
                    case 138947: v.emisMult = float(val.toDouble()); v.hasEmisMult = true; break;
                    case 204855: {
                        const QJsonObject c = val.toObject();
                        v.emisR = float(c.value(QStringLiteral("x")).toDouble());
                        v.emisG = float(c.value(QStringLiteral("y")).toDouble());
                        v.emisB = float(c.value(QStringLiteral("z")).toDouble());
                        v.hasEmisColor = true;
                        break;
                    }
                    default: break;
                }
            }
        }
    }
    v.valid = v.hasMetal || v.hasRough || v.hasAO || v.hasEmisMult || v.hasEmisColor;
    return v;
}
