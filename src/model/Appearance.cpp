#include "model/Appearance.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

AppearanceInfo parseAppearanceJson(const QByteArray& json)
{
    AppearanceInfo info;
    if (json.isEmpty())
        return info;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return info;
    const QJsonObject root = doc.object();

    // Materials: ptAppearanceMaterials[].ptSOAs[].snoMaterial
    const QJsonArray mats = root.value(QStringLiteral("ptAppearanceMaterials")).toArray();
    for (const QJsonValue& mv : mats) {
        const QJsonObject m = mv.toObject();
        const qint64 hash = qint64(m.value(QStringLiteral("dwMaterialHash")).toDouble());
        const QJsonArray soas = m.value(QStringLiteral("ptSOAs")).toArray();
        for (const QJsonValue& sv : soas) {
            const QJsonObject s = sv.toObject();
            const QJsonObject sm = s.value(QStringLiteral("snoMaterial")).toObject();
            AppMaterial am;
            am.name  = sm.value(QStringLiteral("name")).toString();
            am.sno   = qint64(sm.value(QStringLiteral("__raw__")).toDouble());
            am.hash  = hash;
            am.flags = s.value(QStringLiteral("dwFlags")).toInt();
            // Skip empty bindings (no material assigned to this slot).
            if (!am.name.isEmpty() || am.sno != 0)
                info.materials.append(am);
        }
    }

    // Looks: ptAppearanceLooks[].szLookName — usually a string, but many files
    // store it as a look-name HASH (int). Show the name when present, else the hash.
    // (A later milestone can resolve hashes via LookNames.txt, like the Python fork.)
    const QJsonArray looks = root.value(QStringLiteral("ptAppearanceLooks")).toArray();
    for (const QJsonValue& lv : looks) {
        const QJsonValue ln = lv.toObject().value(QStringLiteral("szLookName"));
        QString nm;
        if (ln.isString())
            nm = ln.toString();
        else if (ln.isDouble())
            nm = QString::number(qint64(ln.toDouble()));
        if (!nm.isEmpty())
            info.looks.append(nm);
    }

    info.valid = !info.materials.isEmpty() || !info.looks.isEmpty();
    return info;
}
