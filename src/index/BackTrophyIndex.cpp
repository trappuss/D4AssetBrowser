#include "index/BackTrophyIndex.h"

#include "app/AppPaths.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <thread>

namespace {
constexpr int kCacheVersion = 2;   // v2 adds per-trophy clips

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

// Collapse leading zeros inside every digit run: "trophy_glo012_stor" -> "trophy_glo12_stor".
// The clip files are inconsistently padded against the appearance stems they belong to —
// trophy_glo12_stor_idle/_killstreak, trophy_glo13_*, trophy_glo14_* are the clips of appearances
// trophy_glo012/013/014_stor. Matching raw strings silently drops five of the ~25 shipped clips.
QString normStem(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (int i = 0; i < s.size(); ) {
        if (!s[i].isDigit()) { out += s[i++]; continue; }
        int j = i;
        while (j < s.size() && s[j].isDigit()) ++j;
        int k = i;
        while (k < j - 1 && s[k] == QLatin1Char('0')) ++k;   // keep at least one digit
        out += QStringView(s).mid(k, j - k);
        i = j;
    }
    return out.toLower();
}

// Keyframe count of a clip, read once at index time. 0 when absent — the UI just omits it.
int clipFrames(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    static const QRegularExpression rx(QStringLiteral("\"nKeyframeCount\":\\s*(\\d+)"));
    const auto m = rx.match(QString::fromUtf8(f.readAll()));
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

// One line describing an index result, used by BOTH the cache-hit and the fresh-build paths.
// They reported different things before: a warm start printed only the trophy count, so the
// clip coverage — the number the animation work actually turns on — was invisible unless the
// cache happened to be cold.
void logResult(const QVector<BackTrophyIndex::Entry>& e, const char* how)
{
    int withClips = 0, nClips = 0;
    for (const BackTrophyIndex::Entry& t : e) {
        if (!t.clips.isEmpty()) ++withClips;
        nClips += t.clips.size();
    }
    qInfo("back-trophy index (%s): %d trophies, %d with animation (%d clips)",
          how, int(e.size()), withClips, nClips);
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
    const QString animDir  = d4dataDir + QStringLiteral("/json/base/meta/Anim");
    const QString stlDir   = d4dataDir + QStringLiteral("/json/enUS_Text/meta/StringList");
    const QString cachePath = AppPaths::dataDir()
                              + QStringLiteral("/back_trophy_v%1.json").arg(kCacheVersion);

    const int gen = m_generation;
    std::thread([this, gen, itemDir, actorDir, animDir, stlDir, cachePath, d4dataDir]() {
        // Signature: the counts of BOTH directories this index reads, plus the snapshot's build
        // stamp. Any patch / d4data commit changes at least one → automatic rebuild, never stale.
        QString sig;
        {
            int nItm = 0, nAcr = 0, nAni = 0;
            { QDirIterator c(itemDir, {QStringLiteral("*.itm.json")}, QDir::Files); while (c.hasNext()) { c.next(); ++nItm; } }
            // Actor counts too: the appearance comes from Actor/*.acr.json, so a snoAppearance
            // retarget with an unchanged item count would otherwise serve a stale mapping.
            { QDirIterator c(actorDir, {QStringLiteral("*.acr.json")}, QDir::Files); while (c.hasNext()) { c.next(); ++nAcr; } }
            // Anim too, now that clips are indexed: a patch adding a trophy idle changes nothing else.
            { QDirIterator c(animDir, {QStringLiteral("*.ani.json")}, QDir::Files); while (c.hasNext()) { c.next(); ++nAni; } }
            QString bv;
            QFile f(d4dataDir + QStringLiteral("/buildVersion.txt"));
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) bv = QString::fromUtf8(f.readAll()).trimmed();
            sig = QStringLiteral("%1|%2|%3|%4").arg(nItm).arg(nAcr).arg(nAni).arg(bv);
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
                        for (const QJsonValue& cv : o.value(QStringLiteral("c")).toArray()) {
                            const QJsonObject co = cv.toObject();
                            Clip c;
                            c.name   = co.value(QStringLiteral("n")).toString();
                            c.frames = co.value(QStringLiteral("f")).toInt();
                            if (!c.name.isEmpty()) e.clips.push_back(c);
                        }
                        if (!e.appearance.isEmpty()) out.push_back(e);
                    }
                    logResult(out, "cached");
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

        // Pass 3 — the trophy's own clips. One directory listing, then a normalised stem match:
        // a clip belongs to a trophy when its name IS the appearance stem or extends it with a
        // "_<clip>" suffix (_idle, _killstreak, _anim). Anything that matches nothing is logged
        // rather than force-fitted — an unexplained clip is a signal, not a row to invent.
        {
            struct AniFile { QString stem, norm, path; };
            QVector<AniFile> anis;
            QDirIterator ai(animDir, {QStringLiteral("*.ani.json")}, QDir::Files);
            while (ai.hasNext()) {
                const QString fp = ai.next();
                AniFile a;
                a.stem = QFileInfo(fp).fileName().section(QStringLiteral(".ani.json"), 0, 0);
                if (!a.stem.startsWith(QLatin1String("trophy"), Qt::CaseInsensitive)) continue;
                a.norm = normStem(a.stem);
                a.path = fp;
                anis.push_back(a);
            }
            QSet<QString> claimed;
            for (Entry& e : out) {
                const QString en = normStem(e.appearance);
                for (const AniFile& a : anis) {
                    if (a.norm != en && !a.norm.startsWith(en + QLatin1Char('_'))) continue;
                    Clip c;
                    c.name   = a.stem;
                    c.frames = clipFrames(a.path);
                    e.clips.push_back(c);
                    claimed.insert(a.stem);
                }
            }
            for (const AniFile& a : anis)
                if (!claimed.contains(a.stem))
                    qInfo("back-trophy index: clip %s matches no trophy appearance",
                          qPrintable(a.stem));
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
                if (!e.clips.isEmpty()) {
                    QJsonArray ca;
                    for (const Clip& c : e.clips)
                        ca.append(QJsonObject{{QStringLiteral("n"), c.name}, {QStringLiteral("f"), c.frames}});
                    o.insert(QStringLiteral("c"), ca);
                }
                arr.append(o);
            }
            QDir().mkpath(QFileInfo(cachePath).absolutePath());
            QFile f(cachePath);
            if (f.open(QIODevice::WriteOnly))
                f.write(QJsonDocument(QJsonObject{{QStringLiteral("sig"), sig},
                                                  {QStringLiteral("trophies"), arr}})
                            .toJson(QJsonDocument::Compact));
            logResult(out, "built");
            install(std::move(out));
        }, Qt::QueuedConnection);
    }).detach();
}
