#include "index/DadOverride.h"

#include "app/AppPaths.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QStandardPaths>

DadOverride& DadOverride::instance()
{
    static DadOverride inst;
    return inst;
}

QString DadOverride::defaultPath()
{
    return AppPaths::dataDir()
           + QStringLiteral("/d4dad.json");
}

bool DadOverride::ensureLoaded()
{
    QMutexLocker lock(&m_mutex);
    if (m_parsed)
        return !m_items.isEmpty();
    m_parsed = true;

    QFile f(defaultPath());
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();

    // d4dad.json "items" mixes real items with emblems/emotes/etc. — keep only
    // entries backed by a base/meta/Item/ definition (those carry tInvImages).
    static const QString kItemPrefix = QStringLiteral("base/meta/Item/");
    const QJsonArray items = root.value(QStringLiteral("items")).toArray();
    m_items.reserve(items.size());
    for (const QJsonValue& v : items) {
        const QJsonObject o = v.toObject();
        const QString fn = o.value(QStringLiteral("filename")).toString();
        if (!fn.startsWith(kItemPrefix))
            continue;
        DadItem it;
        it.stem = fn.mid(kItemPrefix.size());
        if (it.stem.endsWith(QLatin1String(".itm")))
            it.stem.chop(4);
        it.icon = quint32(o.value(QStringLiteral("icon")).toDouble());
        for (const QJsonValue& iv : o.value(QStringLiteral("invImages")).toArray()) {
            const QJsonArray p = iv.toArray();
            it.inv.append(qMakePair(quint32(p.at(0).toDouble()),
                                    quint32(p.at(1).toDouble())));
        }
        for (const QJsonValue& uv : o.value(QStringLiteral("usableByClass")).toArray())
            it.usable.append(quint8(uv.toInt()));
        if (it.icon || !it.inv.isEmpty())
            m_items.insert(o.value(QStringLiteral("id")).toInt(), it);
    }
    return !m_items.isEmpty();
}

void DadOverride::reset()
{
    QMutexLocker lock(&m_mutex);
    m_parsed = false;
    m_items.clear();
}
