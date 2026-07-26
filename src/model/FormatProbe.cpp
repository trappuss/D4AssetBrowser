#include "model/FormatProbe.h"

#include "casc/CascReader.h"
#include "index/CoreToc.h"
#include "index/SnoIndex.h"
#include "model/ModelParser.h"

namespace {
constexpr int kGroupAppearance = 9;
// A parsed player body should have well over this many bones (real barM_base00 has 318;
// any humanoid rig has 150+). Below it ⇒ the parser found garbage / the format moved.
constexpr int kMinPlausibleBones = 80;
}

FormatProbe::Result FormatProbe::run(CascReader* reader, const SnoIndex* index)
{
    Result r;
    if (!reader || !reader->isReady() || !index)
        return r;   // ran = false: nothing to probe against

    // Prefer barM_base00 (the rig everything was validated on); fall back to any *_base00.
    int sno = -1;
    QString name;
    const QVector<SnoEntry>& apps = index->entries(kGroupAppearance);
    for (const SnoEntry& e : apps)
        if (e.name.compare(QStringLiteral("barM_base00"), Qt::CaseInsensitive) == 0) {
            sno = e.snoId; name = e.name; break;
        }
    if (sno < 0)
        for (const SnoEntry& e : apps)
            if (e.name.endsWith(QStringLiteral("_base00"), Qt::CaseInsensitive)
                && !e.name.contains(QLatin1Char('/'))) {
                sno = e.snoId; name = e.name; break;
            }
    if (sno < 0)
        return r;   // ran = false: no known model in this build's index

    r.ran = true;
    r.probedName = name;

    const QByteArray meta = reader->readMetaBySno(quint64(sno));
    const QByteArray payload = reader->readPayloadBySno(quint64(sno));
    if (meta.isEmpty() || payload.isEmpty()) {
        r.summary = QStringLiteral("Format probe: could not read %1 (encrypted or missing) — skipped.")
                        .arg(name);
        return r;   // ok=false but no warning: a single unreadable model isn't proof of a format break
    }

    const ModelGeometry geo = ModelParser::parseApp(meta, payload);
    r.boneCount = geo.skeleton.size();
    if (geo.valid && r.boneCount >= kMinPlausibleBones) {
        r.ok = true;
        r.summary = QStringLiteral("Format probe: %1 parsed OK (%2 bones).").arg(name).arg(r.boneCount);
    } else {
        r.summary = QStringLiteral("Format probe: %1 parsed abnormally (valid=%2, bones=%3).")
                        .arg(name).arg(geo.valid ? QStringLiteral("yes") : QStringLiteral("no"))
                        .arg(r.boneCount);
        r.warning = QStringLiteral(
            "The known model \"%1\" did not parse as expected (%2 bones, expected 80+).\n\n"
            "A Diablo IV patch may have changed the model/skeleton format. Exports and the 3D "
            "preview may be wrong until the parser is updated. If you just updated the game, this "
            "is likely why.").arg(name).arg(r.boneCount);
    }
    return r;
}
