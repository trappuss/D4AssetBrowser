#include "index/BackTrophyIndex.h"

#include "app/AppPaths.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <thread>

namespace {
constexpr int kCacheVersion = 1;

// The ItemType every back trophy points at. Matched against the sno reference's target path, not
// against the item's own filename — that distinction is the whole point of this index.
constexpr char kBackItemType[] = "ItemType/CosmeticBack";

// Basename of a d4data sno reference's "__targetFileName__", e.g.
// {"__targetFileName__": "base/meta/Actor/trophy_sor029_stor.acr"} → "trophy_sor029_stor".
QString refBaseName(const QJsonValue& v)
{
    if (!v.isObject()) return {};
    const QString t = v.toObject().value(QStringLiteral("__targetFileName__")).toString();
    if (t.isEmpty()) return {};
    return QFileInfo(t).completeBaseName();
}

// Localized item name. TransmogName is what the game's wardrobe shows when it differs from the
// plain Name, so it wins. Empty ⇒ caller falls back to the file stem rather than inventing text.
QString itemDisplayName(const QString& stlDir, const QString& stem)
{
    QFile f(stlDir + QStringLiteral("/Item_%1.stl.json").arg(stem));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).object()
                               .value(QStringLiteral("arStrings")).toArray();
    QString name, transmog;
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        const QString l = o.value(QStringLiteral("szLabel")).toString();
        if (l == QLatin1String("Name"))              name     = o.value(QStringLiteral("szText")).toString();
        else if (l == QLatin1String("TransmogName")) transmog = o.value(QStringLiteral("szText")).toString();
    }
    return transmog.isEmpty() ? name : transmog;
}
}   // namespace

BackTrophyIndex& BackTrophyIndex::instance()
{
    static BackTrophyIndex inst;
    return inst;
}

void BackTrophyIndex::install(QVector<Entry>&& e)
{
    m_entries  = std::move(e);
    m_ready    = true;
    m_building = false;
    emit readyChanged();
}

void BackTrophyIndex::reset()
{
    ++m_generation;      // orphan any in-flight build before clearing
    m_ready = false;
    m_building = false;
    m_entries.clear();
    QFile::remove(AppPaths::dataDir()
                  + QStringLiteral("/back_trophy_v%1.json").arg(kCacheVersion));
    emit readyChanged();
}

void BackTrophyIndex::ensureBuilt(const QString& d4dataDir)
{
    if (m_ready || m_building || d4dataDir.isEmpty()) return;
    const QString itemDir = d4dataDir + QStringLiteral("/json/base/meta/Item");
    if (!QDir(itemDir).exists()) return;
    m_building = true;

    const QString actorDir = d4dataDir + QStringLiteral("/json/base/meta/Actor");
    const QString stlDir   = d4dataDir + QStringLiteral("/json/enUS_Text/meta/StringList");
    const QString cachePath = AppPaths::dataDir()
                              + QStringLiteral("/back_trophy_v%1.json").arg(kCacheVersion);

    const int gen = m_generation;
    std::thread([this, gen, itemDir, actorDir, stlDir, cachePath, d4dataDir]() {
        // Signature: the counts of BOTH directories this index reads, plus the snapshot's build
        // stamp. Any patch / d4data commit changes at least one → automatic rebuild, never stale.
        QString sig;
        {
            int nItm = 0, nAcr = 0;
            { QDirIterator c(itemDir, {QStringLiteral("*.itm.json")}, QDir::Files); while (c.hasNext()) { c.next(); ++nItm; } }
            // Actor counts too: the appearance comes from Actor/*.acr.json, so a snoAppearance
            // retarget with an unchanged item count would otherwise serve a stale mapping.
            { QDirIterator c(actorDir, {QStringLiteral("*.acr.json")}, QDir::Files); while (c.hasNext()) { c.next(); ++nAcr; } }
            QString bv;
            QFile f(d4dataDir + QStringLiteral("/buildVersion.txt"));
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) bv = QString::fromUtf8(f.readAll()).trimmed();
            sig = QStringLiteral("%1|%2|%3").arg(nItm).arg(nAcr).arg(bv);
        }

        QVector<Entry> out;
        {   // Cache hit?
            QFile f(cachePath);
            if (f.open(QIODevice::ReadOnly)) {
                const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
                if (root.value(QStringLiteral("sig")).toString() == sig) {
                    for (const QJsonValue& v : root.value(QStringLiteral("trophies")).toArray()) {
                        const QJsonObject o = v.toObject();
                        Entry e;
                        e.appearance  = o.value(QStringLiteral("a")).toString();
                        e.itemStem    = o.value(QStringLiteral("i")).toString();
                        e.displayName = o.value(QStringLiteral("n")).toString();
                        if (!e.appearance.isEmpty()) out.push_back(e);
                    }
                    qInfo("back-trophy index: %d trophies (cached)", int(out.size()));
                    QMetaObject::invokeMethod(this, [this, gen, out]() mutable {
                        if (gen != generation()) return;      // d4data switched mid-build
                        install(std::move(out));
                    }, Qt::QueuedConnection);
                    return;
                }
            }
        }

        // Pass 1 — find the items. Every *.itm.json is read, but only as BYTES: the substring test
        // is far cheaper than parsing ~12k JSON documents, and only the handful that reference the
        // back ItemType are actually parsed. Deliberately not pre-filtered on the item's filename:
        // that is exactly the assumption that made the old "back_" scan miss every real trophy.
        QDirIterator it(itemDir, {QStringLiteral("*.itm.json")}, QDir::Files);
        while (it.hasNext()) {
            const QString path = it.next();
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QByteArray raw = f.readAll();
            if (!raw.contains(kBackItemType)) continue;

            const QJsonObject o = QJsonDocument::fromJson(raw).object();
            // Confirm on the PARSED reference: the byte test alone could match the string appearing
            // in some other field of a future item layout.
            if (refBaseName(o.value(QStringLiteral("snoItemType"))) != QLatin1String("CosmeticBack"))
                continue;
            const QString actor = refBaseName(o.value(QStringLiteral("snoActor")));
            if (actor.isEmpty()) continue;

            // Pass 2 — Actor → Appearance. Skipped entirely if unresolvable; a trophy with no
            // appearance is not something to guess a mesh for.
            QFile af(actorDir + QStringLiteral("/%1.acr.json").arg(actor));
            if (!af.open(QIODevice::ReadOnly)) continue;
            const QString appearance = refBaseName(
                QJsonDocument::fromJson(af.readAll()).object().value(QStringLiteral("snoAppearance")));
            if (appearance.isEmpty()) continue;

            Entry e;
            e.itemStem    = QFileInfo(path).fileName().section(QStringLiteral(".itm.json"), 0, 0);
            e.appearance  = appearance;
            e.displayName = itemDisplayName(stlDir, e.itemStem);
            out.push_back(e);
        }

        std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
            const QString an = a.displayName.isEmpty() ? a.itemStem : a.displayName;
            const QString bn = b.displayName.isEmpty() ? b.itemStem : b.displayName;
            const int c = an.compare(bn, Qt::CaseInsensitive);
            return c != 0 ? c < 0 : a.appearance.compare(b.appearance, Qt::CaseInsensitive) < 0;
        });

        QMetaObject::invokeMethod(this, [this, gen, out, sig, cachePath]() mutable {
            if (gen != generation()) return;                  // d4data switched mid-build
            QJsonArray arr;
            for (const Entry& e : out) {
                QJsonObject o{{QStringLiteral("a"), e.appearance}, {QStringLiteral("i"), e.itemStem}};
                if (!e.displayName.isEmpty()) o.insert(QStringLiteral("n"), e.displayName);
                arr.append(o);
            }
            QDir().mkpath(QFileInfo(cachePath).absolutePath());
            QFile f(cachePath);
            if (f.open(QIODevice::WriteOnly))
                f.write(QJsonDocument(QJsonObject{{QStringLiteral("sig"), sig},
                                                  {QStringLiteral("trophies"), arr}})
                            .toJson(QJsonDocument::Compact));
            qInfo("back-trophy index: %d trophies", int(out.size()));
            install(std::move(out));
        }, Qt::QueuedConnection);
    }).detach();
}
