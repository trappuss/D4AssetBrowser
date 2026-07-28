#include "index/WardrobeAnimIndex.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QVector>
#include <thread>

namespace {

// A JSON sno reference is an object carrying "__targetFileName__" — the basename is the stem we
// want. Reading the name rather than the id keeps this working across a re-index, the same rule
// BackTrophyIndex follows.
QString refStem(const QJsonValue& v)
{
    if (!v.isObject()) return {};
    const QString p = v.toObject().value(QStringLiteral("__targetFileName__")).toString();
    if (p.isEmpty()) return {};
    QString base = p.section(QLatin1Char('/'), -1);
    const int dot = base.indexOf(QLatin1Char('.'));
    return dot > 0 ? base.left(dot) : base;
}

QJsonObject readJson(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

}  // namespace

WardrobeAnimIndex& WardrobeAnimIndex::instance()
{
    static WardrobeAnimIndex inst;
    return inst;
}

void WardrobeAnimIndex::reset()
{
    ++m_generation;
    m_classByType.clear();
    m_sets.clear();
    m_prefixes.clear();
    m_ready = false;
    m_building = false;
}

void WardrobeAnimIndex::install(QHash<QString, int>&& byType, QHash<QString, Entry>&& sets,
                                QSet<QString>&& prefixes)
{
    m_classByType = std::move(byType);
    m_sets        = std::move(sets);
    m_prefixes    = std::move(prefixes);
    m_ready       = true;
    m_building    = false;
    emit readyChanged();
}

WardrobeAnimIndex::Clips WardrobeAnimIndex::clipsFor(const QString& classPrefix, int weaponClass,
                                                     bool female) const
{
    const auto it = m_sets.constFind(key(classPrefix, weaponClass));
    if (it == m_sets.constEnd()) return {};
    const Clips& c = female ? it->female : it->male;
    // A set may bind the male anim and leave the female override empty; falling back is what the
    // game's own "override" wording means.
    if (female && c.idle.isEmpty() && c.unsheathe.isEmpty()) return it->male;
    return c;
}

void WardrobeAnimIndex::ensureBuilt(const QString& d4dataDir)
{
    if (m_ready || m_building || d4dataDir.isEmpty()) return;
    m_building = true;
    const int gen = m_generation;
    const QString root = d4dataDir;

    // Detached std::thread, matching BackTrophyIndex — the project links no Qt Concurrent module,
    // and the result is handed back with invokeMethod so nothing touches the index off the GUI
    // thread.
    std::thread([this, root, gen] {
        QHash<QString, int>   byType;
        QHash<QString, Entry> sets;
        QSet<QString>         prefixes;

        // ── ItemType -> eWeaponClass ──
        const QString itDir = QDir(root).filePath(QStringLiteral("json/base/meta/ItemType"));
        for (const QFileInfo& fi : QDir(itDir).entryInfoList({QStringLiteral("*.itt.json")}, QDir::Files)) {
            const QJsonObject o = readJson(fi.absoluteFilePath());
            if (o.isEmpty()) continue;
            const QJsonValue wc = o.value(QStringLiteral("eWeaponClass"));
            if (!wc.isDouble()) continue;
            const int v = wc.toInt(-1);
            if (v < 0) continue;
            QString stem = fi.fileName();
            stem.chop(9);   // ".itt.json"
            byType.insert(stem.toLower(), v);
        }

        // ── which wardrobe sets each character actually uses ──
        // Followed by REFERENCE rather than by file name: PlayerClass names its male and female
        // Actors, and each Actor lists the AnimSets it uses in arAnimSets. Globbing the AnimSet
        // directory and trusting the prefix would have been close but not safe — "war_" is WARLOCK,
        // not warrior, and nothing in the file name says so. Verified: all eight playable classes
        // resolve, and the male and female lists are identical (gender is handled INSIDE each set
        // by snoFemaleOverrideAnim, not by having separate sets).
        const QString asDir = QDir(root).filePath(QStringLiteral("json/base/meta/AnimSet"));
        const QString pcDir = QDir(root).filePath(QStringLiteral("json/base/meta/PlayerClass"));
        const QString acDir = QDir(root).filePath(QStringLiteral("json/base/meta/Actor"));
        QSet<QString> wardrobeSets;   // set stems this game's characters actually reference
        for (const QFileInfo& pf : QDir(pcDir).entryInfoList({QStringLiteral("*.pcl.json")}, QDir::Files)) {
            const QJsonObject pc = readJson(pf.absoluteFilePath());
            if (pc.isEmpty()) continue;
            for (const char* k : {"snoActorMale", "snoActorFemale"}) {
                const QString actor = refStem(pc.value(QLatin1String(k)));
                if (actor.isEmpty()) continue;
                const QJsonObject ac = readJson(QDir(acDir).filePath(actor + QStringLiteral(".acr.json")));
                for (const QJsonValue& av : ac.value(QStringLiteral("arAnimSets")).toArray()) {
                    const QString n = av.toObject().value(QStringLiteral("name")).toString();
                    if (n.endsWith(QLatin1String("_ui_wardrobe"), Qt::CaseInsensitive)) wardrobeSets.insert(n.toLower());
                }
            }
        }

        for (const QFileInfo& fi : QDir(asDir).entryInfoList({QStringLiteral("*_ui_wardrobe.ans.json")},
                                                             QDir::Files)) {
            {   // only the sets a real character references
                QString setStem = fi.fileName();
                setStem.chop(9);   // ".ans.json"
                if (!wardrobeSets.isEmpty() && !wardrobeSets.contains(setStem.toLower())) continue;
            }
            const QJsonObject o = readJson(fi.absoluteFilePath());
            if (o.isEmpty()) continue;
            QString stem = fi.fileName();
            stem.chop(14);   // "_ui_wardrobe.ans.json" minus the part we keep
            const QString prefix = fi.fileName().section(QLatin1Char('_'), 0, 0).toLower();
            if (prefix.isEmpty()) continue;
            prefixes.insert(prefix);

            Clips male, female;
            for (const QJsonValue& ev : o.value(QStringLiteral("ptPowerEntryList")).toArray()) {
                const QJsonObject e = ev.toObject();
                const QString power = refStem(e.value(QStringLiteral("snoPower")));
                const QString mAnim = refStem(e.value(QStringLiteral("snoAnim")));
                const QString fAnim = refStem(e.value(QStringLiteral("snoFemaleOverrideAnim")));
                // Matched case-insensitively: the data spells it ui_wardrobe_unSheathe with a
                // capital S in the middle, which no amount of staring at the file names predicts.
                if (power.compare(QLatin1String("ui_wardrobe_idle"), Qt::CaseInsensitive) == 0) {
                    male.idle = mAnim; female.idle = fAnim;
                } else if (power.compare(QLatin1String("ui_wardrobe_unSheathe"), Qt::CaseInsensitive) == 0) {
                    male.unsheathe = mAnim; female.unsheathe = fAnim;
                }
            }
            if (male.idle.isEmpty() && male.unsheathe.isEmpty()
                && female.idle.isEmpty() && female.unsheathe.isEmpty())
                continue;   // e.g. bar_1hshth, which binds only the loading-screen pose

            // An EMPTY ptWeaponClasses means "no particular class" — dru/rog/sor/war spell their
            // unarmed set that way where bar/nec/pal write 0 explicitly. Same meaning, so it is
            // registered as 0 rather than dropped.
            QVector<int> classes;
            for (const QJsonValue& wv : o.value(QStringLiteral("ptWeaponClasses")).toArray())
                if (wv.toInt(-1) >= 0) classes.push_back(wv.toInt());
            if (classes.isEmpty()) classes.push_back(0);
            for (int wc : classes) {
                const QString k = key(prefix, wc);
                // First writer wins. Several sets can claim one class (bar_oh_nw and bar_1hshth
                // both claim 1); the one that actually binds the wardrobe Powers is kept, and the
                // loading-screen-only set was already skipped above.
                if (!sets.contains(k)) sets.insert(k, Entry{male, female});
            }
        }

        QMetaObject::invokeMethod(this, [this, gen, byType = std::move(byType),
                                         sets = std::move(sets), prefixes = std::move(prefixes)]() mutable {
            if (gen != m_generation) { m_building = false; return; }   // d4data changed mid-build
            qInfo("wardrobe anims: %d item type(s) with a weapon class, %d (class,weapon) wardrobe "
                  "set(s) over %d character class(es)",
                  int(byType.size()), int(sets.size()), int(prefixes.size()));
            install(std::move(byType), std::move(sets), std::move(prefixes));
        }, Qt::QueuedConnection);
    }).detach();
}
