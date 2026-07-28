#include "index/WardrobeAnimIndex.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
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

        // ── wardrobe AnimSets -> per (class prefix, weapon class) clips ──
        // Selected by the "_ui_wardrobe" suffix, which is what every one of them carries; the
        // weapon classes come from the file's own ptWeaponClasses rather than from its name, so a
        // set covering several (rog_dw covers 1, 2, 9, 26, 29 and 30) registers under all of them.
        const QString asDir = QDir(root).filePath(QStringLiteral("json/base/meta/AnimSet"));
        for (const QFileInfo& fi : QDir(asDir).entryInfoList({QStringLiteral("*_ui_wardrobe.ans.json")},
                                                             QDir::Files)) {
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

            for (const QJsonValue& wv : o.value(QStringLiteral("ptWeaponClasses")).toArray()) {
                const int wc = wv.toInt(-1);
                if (wc < 0) continue;
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
