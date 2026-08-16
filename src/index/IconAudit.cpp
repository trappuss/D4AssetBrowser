#include "index/IconAudit.h"

#include "app/AppPaths.h"
#include "casc/CascReader.h"
#include "index/AppearanceMeta.h"
#include "index/DadOverride.h"
#include "index/IconIndex.h"
#include "index/ItemDef.h"
#include "index/SnoIndex.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace {

constexpr int kGroupActor = 1;
constexpr int kGroupAppearance = 9;

// Same class/gender fallback rules as the crawl's pickHandle.
quint32 pickHandle(const QVector<QPair<quint32, quint32>>& inv, int classIdx, bool female)
{
    quint32 m = 0, f = 0;
    if (classIdx >= 0 && classIdx < inv.size()) {
        m = inv[classIdx].first;
        f = inv[classIdx].second;
    }
    if (!m && !f)
        for (const auto& e : inv)
            if (e.first || e.second) { m = e.first; f = e.second; break; }
    return female ? (f ? f : m) : (m ? m : f);
}

// snoActor.name straight out of a d4data item JSON (cheap text-free parse).
QString actorFromItemJson(const QString& d4dataDir, const QString& stem)
{
    QFile f(d4dataDir + QStringLiteral("/json/base/meta/Item/") + stem
            + QStringLiteral(".itm.json"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    return obj.value(QStringLiteral("snoActor")).toObject()
              .value(QStringLiteral("name")).toString();
}

}  // namespace

QString IconAudit::run(const QString& d4dataDir, const SnoIndex* index, CascReader* reader)
{
    const AppearanceMeta& am = AppearanceMeta::instance();
    if (!index || !index->isLoaded() || !am.ready())
        return QStringLiteral("Icon audit: appearance index not ready — wait for Indexing to finish.");
    DadOverride& dad = DadOverride::instance();
    if (!dad.ensureLoaded())
        return QStringLiteral("Icon audit: %1 not found/empty — it is fetched automatically on "
                              "the next data change, or place a d4dad.json there manually.")
            .arg(DadOverride::defaultPath());

    // Appearance name → sno and Actor sno → name, straight from the live index.
    QHash<QString, int> name2sno;
    QHash<int, QString> sno2name;
    for (const SnoEntry& e : index->entries(kGroupAppearance)) {
        name2sno.insert(e.name.toLower(), e.snoId);
        sno2name.insert(e.snoId, e.name);
    }
    QHash<quint32, QString> actorNames;
    for (const SnoEntry& a : index->entries(kGroupActor))
        actorNames.insert(quint32(a.snoId), a.name);

    static const QRegularExpression classGenderRe(QStringLiteral("^([a-z]{3})([fm])_"));

    // Pass 1 — expected handles per appearance, from every diablo4.dad item that
    // resolves to it via the shared derivation rules (multiple items may legally
    // claim one appearance; the tool's pick must match ANY of them).
    QHash<int, QSet<quint32>> expected;      // appearance sno → acceptable handles
    QHash<int, QString>       sourceItem;    // appearance sno → one contributing item (report)
    int itemsUsed = 0, actorFromCasc = 0;
    for (auto it = dad.items().constBegin(); it != dad.items().constEnd(); ++it) {
        const DadItem& di = it.value();
        // Restrict claims to the item's usable classes (empty mask = all). Without
        // this, class-specific uniques that share a style number (HLM_uniq101 …)
        // claim every class's appearance and the audit drowns in false DIFFs.
        QStringList classPrefs;   // empty = all 8
        if (!di.usable.isEmpty()) {
            const QStringList& all = AppearanceMeta::heroClassPrefixes();
            const int n = qMin(int(di.usable.size()), int(all.size()));
            for (int i = 0; i < n; ++i)
                if (di.usable.at(i)) classPrefs << all.at(i);
        }
        QStringList candNames = AppearanceMeta::cosmeticAppearanceNames(di.stem.toLower());
        if (candNames.isEmpty()) {
            QString actor = actorFromItemJson(d4dataDir, di.stem);
            if (actor.isEmpty() && reader && reader->isReady()) {
                const ItemDef::ItemInfo info =
                    ItemDef::parseItem(reader->readMetaBySno(quint64(it.key())));
                if (info.snoActor) {
                    actor = actorNames.value(info.snoActor);
                    ++actorFromCasc;
                }
            }
            candNames = AppearanceMeta::styleAppearanceNames(actor, classPrefs);
        }
        // Route 3. Until this was added the audit never entered a WEAPON appearance into its
        // expected-handle set, so it could neither confirm nor DIFF one: every weapon icon was
        // invisible to the audit and its reported coverage overstated itself.
        candNames = AppearanceMeta::withSelfName(candNames, di.stem.toLower());
        if (candNames.isEmpty())
            continue;
        bool used = false;
        for (const QString& nm : candNames) {
            const int s = name2sno.value(nm, 0);
            if (!s)
                continue;
            const auto gm = classGenderRe.match(nm);
            const bool female = gm.hasMatch() && gm.captured(2) == QLatin1String("f");
            const int ci = gm.hasMatch() ? ItemDef::heroClassIndex(gm.captured(1)) : -1;
            quint32 h = di.inv.isEmpty() ? 0 : pickHandle(di.inv, ci, female);
            if (!h) h = di.icon;
            if (!h)
                continue;
            expected[s].insert(h);
            if (!sourceItem.contains(s)) sourceItem.insert(s, di.stem);
            used = true;
        }
        if (used) ++itemsUsed;
    }

    // Reverse map: which appearance name(s) legitimately expect each handle. A DIFF whose
    // tool handle appears here (for a DIFFERENT appearance) is cross-wiring — the solver
    // borrowed another class/style's icon — as opposed to a handle that belongs to nothing.
    QHash<quint32, QStringList> handleOwners;
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it) {
        const QString onm = sno2name.value(it.key());
        for (quint32 h : it.value())
            if (handleOwners[h].size() < 4 && !handleOwners[h].contains(onm))
                handleOwners[h].append(onm);
    }

    // Pass 2 — compare against what the tool resolved.
    const IconIndex& ii = IconIndex::instance();
    const bool spriteCheck = ii.ready();
    QStringList lines;
    int missing = 0, diffs = 0, nosprite = 0, ok = 0;
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it) {
        const int s = it.key();
        const QString nm = sno2name.value(s);
        const quint32 actual = am.iconFor(s);
        if (!actual) {
            ++missing;
            lines << QStringLiteral("MISSING  %1 %2  expected=%3 (item %4)")
                         .arg(s).arg(nm).arg(*it.value().constBegin()).arg(sourceItem.value(s));
            continue;
        }
        if (!it.value().contains(actual)) {
            ++diffs;
            QStringList exp;
            for (quint32 h : it.value()) exp << QString::number(h);
            std::sort(exp.begin(), exp.end());
            // Root-cause hint: does the tool's handle belong to another appearance (cross-wiring)?
            const QStringList owners = handleOwners.value(actual);
            const QString why = owners.isEmpty()
                ? QStringLiteral("  [tool handle owned by no d4dad appearance — wrong item]")
                : QStringLiteral("  [tool handle belongs to: %1 — cross-wired]").arg(owners.join(QLatin1Char(',')));
            lines << QStringLiteral("DIFF     %1 %2  tool=%3 expected={%4} (item %5)%6")
                         .arg(s).arg(nm).arg(actual).arg(exp.join(QLatin1Char(',')),
                                                         sourceItem.value(s)).arg(why);
        } else {
            ++ok;
        }
        if (spriteCheck && !ii.has(actual)) {
            ++nosprite;
            // Distinguish "tool picked a spriteless variant when a good one exists" (fixable in
            // the solver) from "no expected handle has a sprite either" (sprite not in the local
            // atlas — a data/coverage issue, not a solver bug).
            bool expectedHasSprite = false;
            for (quint32 h : it.value()) if (ii.has(h)) { expectedHasSprite = true; break; }
            lines << QStringLiteral("NOSPRITE %1 %2  handle=%3 has no atlas frame%4")
                         .arg(s).arg(nm).arg(actual)
                         .arg(expectedHasSprite
                                  ? QStringLiteral("  [an expected handle DOES have a sprite — solver picked a spriteless variant]")
                                  : QStringLiteral("  [no expected handle has a sprite — likely not in the local atlas]"));
        }
    }
    std::sort(lines.begin(), lines.end());

    const QString summary =
        QStringLiteral("Icon audit: %1 appearances checked (%2 d4dad items, %3 via CASC actor) — "
                       "%4 ok, %5 missing, %6 diffs, %7 no-sprite%8")
            .arg(expected.size()).arg(itemsUsed).arg(actorFromCasc)
            .arg(ok).arg(missing).arg(diffs).arg(nosprite)
            .arg(spriteCheck ? QString() : QStringLiteral(" (sprite check skipped: icon index not ready)"));

    // Atomic write into data/ — readable while the app is still running (unlike the
    // truncating log). It used to go beside the EXE, which meant a downloaded copy grew an
    // icon_audit.txt next to D4AssetBrowser.exe the first time indexing finished, contradicting
    // the README's "everything the tool writes lives in data\". The release smoke test missed it
    // because the app was closed before indexing completed, so the audit never ran.
    const QString outPath = AppPaths::file(QStringLiteral("icon_audit.txt"));
    QSaveFile out(outPath);
    if (out.open(QIODevice::WriteOnly)) {
        QByteArray body = summary.toUtf8();
        body += "\n\n";
        body += lines.join(QLatin1Char('\n')).toUtf8();
        body += '\n';
        out.write(body);
        out.commit();
    }
    return summary + QStringLiteral("  →  %1").arg(outPath);
}
