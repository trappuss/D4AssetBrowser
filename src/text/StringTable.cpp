#include "text/StringTable.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

QVector<StringRow> parseStringTableJson(const QByteArray& json)
{
    QVector<StringRow> out;
    if (json.isEmpty())
        return out;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return out;

    const QJsonArray arr = doc.object().value(QStringLiteral("arStrings")).toArray();
    out.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        const QJsonObject e = v.toObject();
        StringRow row;
        row.label  = e.value(QStringLiteral("szLabel")).toString();
        row.text   = e.value(QStringLiteral("szText")).toString();
        row.hLabel = qint64(e.value(QStringLiteral("hLabel")).toDouble());
        out.append(row);
    }
    return out;
}
