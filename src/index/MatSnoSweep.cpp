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
// The ORDERED list the tool actually needs: one sno per ptAppearanceMaterials entry, in array
// order, because that is what MeshPrimitive::materialIndex indexes into. Preference order matches
// MaterialDecode::appearanceRoster so the comparison is against the behaviour we must reproduce,
// not against an idealised reading of the JSON.
QVector<int> jsonMaterialList(const QString& d4, const QString& name)
{
    QVector<int> out;
    QFile f(QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4, name));
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonObject ro = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue& mv : ro.value(QStringLiteral("ptAppearanceMaterials")).toArray()) {
        const QJsonArray soas = mv.toObject().value(QStringLiteral("ptSOAs")).toArray();
        int pick = 0;
        if (!soas.isEmpty()) {
            const QJsonObject so = soas[0].toObject();
            for (const char* k : {"snoOverrideMaterial", "snoMaterial", "snoCloth",
                                  "snoHighQualityClothOverride"}) {
                const int r = so.value(QLatin1String(k)).toObject()
                                .value(QStringLiteral("__raw__")).toInt();
                if (r > 0) { pick = r; break; }
            }
        }
        out.push_back(pick);
    }
    return out;
}

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
// The second field is the array's TOTAL BYTE SIZE, not the element size — so it is 72 for one
// material, 144 for two, and so on, and the COUNT is size/72. The first sweep hardcoded '== 72' and
// therefore matched only single-material arrays, which is the whole of its 32180 NO-DESCRIPTOR
// result. The tell was already in the hand-clicked run: BarM_stor258_TRS reported a 144-byte stride.
// Returns the descriptor offset and writes the record count out.
// Selection must not depend on the JSON: encrypted appearances have none, and a rule tuned against
// ground truth we will not have at runtime is worthless. So candidates are validated against the SNO
// INDEX — every record's +20 field must be a real Material sno — which is available for encrypted
// assets too.
//
// Taking the FIRST forward match was the bug behind 31670 count mismatches, overwhelmingly
// 'declared < json' (decl=1/json=2 alone accounts for 8105): a smaller sub-array sits earlier in the
// blob and anchored fine. Now every candidate is scored and the LONGEST fully-valid one wins, since
// ptAppearanceMaterials is the outermost array and any sub-array is necessarily shorter.
int findDescriptor(const QByteArray& meta, const QSet<int>& matSnos, const QSet<int>& anySno,
                   int* countOut)
{
    int bestOff = -1, bestCount = 0;
    for (int off = 0; off + 8 <= meta.size(); off += 4) {
        const quint32 bytes = u32at(meta, off + 4);
        if (bytes == 0 || bytes % quint32(kRecSize) != 0 || bytes > 64u * quint32(kRecSize)) continue;
        const int dataOff = int(u32at(meta, off));
        const int n = int(bytes) / kRecSize;
        if (dataOff <= 0 || dataOff + n * kRecSize > meta.size()) continue;
        if (n <= bestCount) continue;                    // cannot beat what we already have
        // A record's sno need NOT be a Material: ptAppearanceMaterials entries also reference
        // snoCloth (group 11), and an entry can be empty (0). Demanding group 57/37 for EVERY
        // record rejected the real array whenever one slot was cloth or blank, and the search then
        // settled for a 1-record sub-array — which is the whole 'decl=1, json=2/3/4' population
        // (8108 + 4237 + 1790 + ... of 31218 count mismatches).
        //
        // So: every record must be 0 or a sno the index knows, and at least one must be a real
        // Material. The anchor keeps this from matching an arbitrary array of valid snos; the
        // tolerance stops one cloth slot disqualifying a correct table.
        bool allValid = true, anyMaterial = false;
        for (int r = 0; r < n && allValid; ++r) {
            const int v = int(u32at(meta, dataOff + r * kRecSize + kSnoAtRec));
            if (v == 0) continue;                        // empty slot is legal
            if (matSnos.contains(v)) { anyMaterial = true; continue; }
            allValid = anySno.contains(v);
        }
        if (!allValid || !anyMaterial) continue;
        bestOff = off; bestCount = n;
    }
    if (bestOff >= 0 && countOut) *countOut = bestCount;
    return bestOff;
}

}  // namespace

QString runMatSnoSweep(const QString& d4, SnoIndex* idx, CascReader* rd, QWidget* parent)
{
    if (!idx || !rd || !rd->isReady()) return QStringLiteral("matsno sweep: index/reader not ready");

    // ── D4_METADUMP_NAMES: write raw meta blobs for OFFLINE format derivation ────────────────
    // The build-run-read loop is the slow part of deriving the material table; the d4analyzer
    // reference extraction gives ground truth for ENCRYPTED appearances (GLB embeds the ordered
    // material list with snos), and d4data JSON gives it for named ones. With the raw blobs on
    // disk, the layout can be solved offline in one sitting instead of one hypothesis per rebuild.
    if (!qEnvironmentVariable("D4_METADUMP_NAMES").isEmpty()) {
        const QString dumpDir = QCoreApplication::applicationDirPath() + QStringLiteral("/metadump");
        QDir().mkpath(dumpDir);
        int wrote = 0;
        const QStringList names = qEnvironmentVariable("D4_METADUMP_NAMES")
                                      .split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& nm : names) {
            int sno = 0;
            for (const SnoEntry& e : idx->entries(kGroupAppearance))
                if (e.name.compare(nm.trimmed(), Qt::CaseInsensitive) == 0) { sno = e.snoId; break; }
            if (!sno) { qWarning("metadump: '%s' not in index", qPrintable(nm.trimmed())); continue; }
            const QByteArray meta = rd->readMetaBySno(quint64(sno));
            if (meta.isEmpty()) { qWarning("metadump: '%s' meta empty", qPrintable(nm.trimmed())); continue; }
            QFile bf(dumpDir + QStringLiteral("/%1_%2.bin").arg(nm.trimmed()).arg(sno));
            if (bf.open(QIODevice::WriteOnly)) { bf.write(meta); ++wrote; }
        }
        qInfo("metadump: wrote %d blob(s) to %s", wrote, qPrintable(dumpDir));
    }
    const QString outDir = QCoreApplication::applicationDirPath();
    QFile csv(outDir + QStringLiteral("/matsno_sweep.csv"));
    if (!csv.open(QIODevice::WriteOnly | QIODevice::Text))
        return QStringLiteral("matsno sweep: cannot write matsno_sweep.csv");
    QTextStream cs(&csv);
    cs << "appearance,sno,metaBytes,jsonMats,descOff,dataOff,declaredCount,walkHits,"
          "walkFirstMiss,contiguous,coversJson,countMatches,pattern\n";

    // Every Material sno in the index — the JSON-free validator the descriptor search runs on.
    // Both material groups: 57 is "Material (2)" and 37 the older "Material" (SnoIndex.cpp:31).
    QSet<int> matSnos;
    for (int g : {57, 37})
        for (const SnoEntry& m : idx->entries(g)) matSnos.insert(m.snoId);
    // Cloth and every other group a material slot may legally point at.
    QSet<int> anySno;
    for (int g : idx->groups())
        for (const SnoEntry& m : idx->entries(g)) anySno.insert(m.snoId);
    qInfo("matsno sweep: validator knows %d material sno(s), %d sno(s) overall",
          int(matSnos.size()), int(anySno.size()));

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
        const QSet<int>   want  = jsonMaterialSnos(d4, e.name);      // anchor set (all sno kinds)
        const QVector<int> order = jsonMaterialList(d4, e.name);     // what materialIndex indexes
        if (want.isEmpty()) continue;
        ++withJson;

        const QByteArray meta = rd->readMetaBySno(quint64(e.snoId));
        if (meta.size() < 64) continue;
        int declaredCount = 0;
        const int desc = findDescriptor(meta, matSnos, anySno, &declaredCount);
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
        for (int r = 0; r < qMax(declaredCount, 1) && r < 64; ++r) {
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
        // THE TEST THAT MATTERS: does record i equal ptAppearanceMaterials[i]? That is the mapping
        // MeshPrimitive::materialIndex needs. The previous 'covers' compared against a SET built
        // from four sno fields, so any appearance carrying cloth as well as materials scored as a
        // failure while being perfectly correct — 42.8% was measuring my superset, not the data.
        bool ordered = (declaredCount == order.size());
        if (ordered)
            for (int r = 0; r < order.size(); ++r) {
                const int rec = dataOff + r * kRecSize;
                if (rec + kSnoAtRec + 4 > meta.size()
                    || int(u32at(meta, rec + kSnoAtRec)) != order[r]) { ordered = false; break; }
            }
        const bool covers = ordered;
        if (contiguous) ++contiguousOk;
        if (covers) ++coversAll;
        if (contiguous && covers) ++exact;
        if (firstMiss == want.size() || (firstMiss < 0 && hits == want.size()))
            ++lengthByCount[want.size()];
        ++patternTally[shape.left(12)];

        cs << e.name << ',' << e.snoId << ',' << meta.size() << ',' << want.size() << ','
           << QStringLiteral("0x%1").arg(desc, 0, 16) << ','
           << QStringLiteral("0x%1").arg(dataOff, 0, 16) << ',' << declaredCount << ',' << hits
           << ',' << firstMiss << ',' << (contiguous ? "yes" : "no") << ','
           << (covers ? "yes" : "no") << ',' << (declaredCount == want.size() ? "yes" : "no") << ','
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
           << "record[i]==jsonMats[i]    " << coversAll << "   <- the mapping materialIndex needs\n"
           << "BOTH (clean derivation)   " << exact << '\n';
        const double pct = descFound ? 100.0 * exact / descFound : 0.0;
        ts << QStringLiteral("\nNOTE: the rate below is conditional on the descriptor being FOUND.\n"
                            "The first sweep divided by all appearances and read 41%% as 'the model\n"
                            "is wrong', when the model held in 99%% of located cases and the search\n"
                            "was what failed. Search coverage is reported separately above.\n");
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
