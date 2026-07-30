#include "index/MatSnoSweep.h"

#include "casc/CascReader.h"
#include "index/SnoIndex.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProgressDialog>
#include <QSet>
#include <QTextStream>
#include <QWidget>

namespace {

constexpr int kGroupAppearance = 9;
constexpr int kSnoAtRec        = 20;   // measured: material sno sits 20 bytes into the record
constexpr int kRecSize         = 72;   // measured twice: sno-hit spacing, and the descriptor's size

quint32 u32at(const QByteArray& b, int off)
{
    if (off < 0 || off + 4 > b.size()) return 0;
    return quint32(uchar(b[off])) | quint32(uchar(b[off + 1])) << 8
         | quint32(uchar(b[off + 2])) << 16 | quint32(uchar(b[off + 3])) << 24;
}

// The material snos the JSON says this appearance uses — ground truth for scoring the walk.
// Mirrors what ModelsTab reads: snoMaterial, snoOverrideMaterial and snoCloth across every SOA.
QSet<int> jsonMaterialSnos(const QString& d4, const QString& name)
{
    QSet<int> out;
    QFile f(QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4, name));
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonObject ro = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue& mv : ro.value(QStringLiteral("ptAppearanceMaterials")).toArray())
        for (const QJsonValue& sv : mv.toObject().value(QStringLiteral("ptSOAs")).toArray()) {
            const QJsonObject so = sv.toObject();
            for (const char* k : {"snoMaterial", "snoOverrideMaterial", "snoCloth",
                                  "snoHighQualityClothOverride"}) {
                const int r = so.value(QLatin1String(k)).toObject()
                                .value(QStringLiteral("__raw__")).toInt();
                if (r > 0) out.insert(r);
            }
        }
    return out;
}

// Locate the array descriptor: the '... [dataOffset] [72] ...' pair, where dataOffset points at a
// record whose +20 field is one of the snos we expect. Anchoring on a KNOWN sno is what keeps this
// from matching an unrelated 72-sized array elsewhere in the blob.
int findDescriptor(const QByteArray& meta, const QSet<int>& want)
{
    for (int off = 0; off + 8 <= meta.size(); off += 4) {
        if (u32at(meta, off + 4) != quint32(kRecSize)) continue;
        const int dataOff = int(u32at(meta, off));
        if (dataOff <= 0 || dataOff + kSnoAtRec + 4 > meta.size()) continue;
        if (want.contains(int(u32at(meta, dataOff + kSnoAtRec)))) return off;
    }
    return -1;
}

}  // namespace

QString runMatSnoSweep(const QString& d4, SnoIndex* idx, CascReader* rd, QWidget* parent)
{
    if (!idx || !rd || !rd->isReady()) return QStringLiteral("matsno sweep: index/reader not ready");
    const QString outDir = QCoreApplication::applicationDirPath();
    QFile csv(outDir + QStringLiteral("/matsno_sweep.csv"));
    if (!csv.open(QIODevice::WriteOnly | QIODevice::Text))
        return QStringLiteral("matsno sweep: cannot write matsno_sweep.csv");
    QTextStream cs(&csv);
    cs << "appearance,sno,metaBytes,jsonMats,descOff,dataOff,walkHits,walkFirstMiss,"
          "contiguous,coversJson,pattern\n";

    const QVector<SnoEntry>& apps = idx->entries(kGroupAppearance);
    QProgressDialog prog(QStringLiteral("Material-sno sweep: %1 appearances…").arg(apps.size()),
                         QStringLiteral("Cancel"), 0, int(apps.size()), parent);
    prog.setWindowModality(Qt::ApplicationModal);
    prog.setMinimumDuration(0);

    int named = 0, withJson = 0, descFound = 0, contiguousOk = 0, coversAll = 0, exact = 0;
    QMap<int, int> lengthByCount;      // jsonMats -> how often walkHits equalled it
    QMap<QString, int> patternTally;   // ok/miss shape -> occurrences
    int scanned = 0;

    for (const SnoEntry& e : apps) {
        if ((++scanned % 64) == 0) {
            prog.setValue(scanned);
            QCoreApplication::processEvents();
            if (prog.wasCanceled()) break;
        }
        // Named only: the JSON is the ground truth this whole sweep scores against, so an encrypted
        // appearance has nothing to check the walk with and would only add noise.
        if (e.name.startsWith(QLatin1String("~unnamed_"))) continue;
        ++named;
        const QSet<int> want = jsonMaterialSnos(d4, e.name);
        if (want.isEmpty()) continue;
        ++withJson;

        const QByteArray meta = rd->readMetaBySno(quint64(e.snoId));
        if (meta.size() < 64) continue;
        const int desc = findDescriptor(meta, want);
        if (desc < 0) {
            cs << e.name << ',' << e.snoId << ',' << meta.size() << ',' << want.size()
               << ",,,,,,,NO-DESCRIPTOR\n";
            ++patternTally[QStringLiteral("NO-DESCRIPTOR")];
            continue;
        }
        ++descFound;
        const int dataOff = int(u32at(meta, desc));

        // Walk until the record leaves the blob. Record which entries the JSON vouches for; the
        // SHAPE of that hit/miss sequence is the finding — a leading run of hits means one
        // contiguous array and gives the length rule.
        QString shape;
        int hits = 0, firstMiss = -1;
        QSet<int> seen;
        for (int r = 0; r < 64; ++r) {
            const int rec = dataOff + r * kRecSize;
            if (rec + kSnoAtRec + 4 > meta.size()) break;
            const int v = int(u32at(meta, rec + kSnoAtRec));
            const bool ok = want.contains(v);
            if (ok) { ++hits; seen.insert(v); }
            else if (firstMiss < 0) firstMiss = r;
            shape += ok ? QLatin1Char('o') : QLatin1Char('.');
        }
        // "contiguous" = every hit precedes every miss, i.e. the array is a clean prefix.
        const bool contiguous = !shape.contains(QStringLiteral(".o"));
        const bool covers = (seen.size() == want.size());
        if (contiguous) ++contiguousOk;
        if (covers) ++coversAll;
        if (contiguous && covers) ++exact;
        if (firstMiss == want.size() || (firstMiss < 0 && hits == want.size()))
            ++lengthByCount[want.size()];
        ++patternTally[shape.left(12)];

        cs << e.name << ',' << e.snoId << ',' << meta.size() << ',' << want.size() << ','
           << QStringLiteral("0x%1").arg(desc, 0, 16) << ','
           << QStringLiteral("0x%1").arg(dataOff, 0, 16) << ',' << hits << ',' << firstMiss << ','
           << (contiguous ? "yes" : "no") << ',' << (covers ? "yes" : "no") << ','
           << shape.left(24) << '\n';
    }
    prog.setValue(int(apps.size()));
    csv.close();

    QFile rep(outDir + QStringLiteral("/matsno_sweep.txt"));
    QString summary;
    if (rep.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream ts(&rep);
        ts << "MATERIAL-SNO TABLE SWEEP\n========================\n\n"
           << "Assumptions under test (measured earlier from ~32 hand-picked appearances):\n"
           << "  material sno at record +" << kSnoAtRec << ", record size " << kRecSize
           << " bytes, descriptor is (dataOffset, " << kRecSize << ").\n\n"
           << "appearances in index      " << apps.size() << '\n'
           << "named                     " << named << '\n'
           << "with JSON materials       " << withJson << '\n'
           << "descriptor located        " << descFound << '\n'
           << "walk contiguous (o*.*)    " << contiguousOk << '\n'
           << "walk covered all JSON     " << coversAll << '\n'
           << "BOTH (clean derivation)   " << exact << '\n';
        const double pct = withJson ? 100.0 * exact / withJson : 0.0;
        ts << QStringLiteral("\nclean derivation rate     %1%\n").arg(pct, 0, 'f', 1);
        ts << "\nVERDICT\n";
        if (pct >= 95.0)
            ts << "  CONFIRMED. The offsets reproduce the JSON across the corpus. A reader can be\n"
                  "  written: walk from dataOffset, stride " << kRecSize << ", sno at +" << kSnoAtRec
               << ", stopping\n  at the first record whose sno is not a valid Material sno.\n";
        else if (pct >= 50.0)
            ts << "  PARTIAL. The shape is right but a subset disagrees — read the pattern tally and\n"
                  "  the 'no' rows in the CSV before writing a reader. Do NOT ship the majority rule.\n";
        else
            ts << "  REJECTED. The offsets do not generalise beyond the hand-picked sample. The\n"
                  "  earlier conclusion was drawn from too narrow a set; re-derive before building.\n";
        ts << "\nWALK PATTERNS (o = JSON lists this sno, . = it does not)\n";
        QList<QPair<int, QString>> pats;
        for (auto it = patternTally.constBegin(); it != patternTally.constEnd(); ++it)
            pats << qMakePair(it.value(), it.key());
        std::sort(pats.begin(), pats.end(), [](const QPair<int, QString>& a,
                                               const QPair<int, QString>& b) { return a.first > b.first; });
        for (int i = 0; i < pats.size() && i < 25; ++i)
            ts << QStringLiteral("  %1  %2\n").arg(pats[i].first, 6).arg(pats[i].second);
        ts << "\nLENGTH RULE — walk length matched the JSON material count this often:\n";
        for (auto it = lengthByCount.constBegin(); it != lengthByCount.constEnd(); ++it)
            ts << QStringLiteral("  %1 material(s): %2 appearance(s)\n").arg(it.key()).arg(it.value());
        ts << "\nPer-appearance detail: matsno_sweep.csv\n";
        rep.close();
    }
    summary = QStringLiteral("matsno sweep: %1 appearances with JSON materials, %2 clean (%3%)")
                  .arg(withJson).arg(exact)
                  .arg(withJson ? 100.0 * exact / withJson : 0.0, 0, 'f', 1);
    qInfo().noquote() << summary;
    return summary;
}
