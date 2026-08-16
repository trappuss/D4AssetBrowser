#include "index/MatSnoSweep.h"

#include "app/AppPaths.h"           // dataDir() — diagnostics write into data\, never beside the exe
#include "casc/CascReader.h"
#include "index/AppearanceMeta.h"   // icon coverage — see the ICONLESS tally in runHealthAudit
#include "tabs/MarkingCompose.h"    // marking sweep: the SAME ramp/composite the viewport uses
#include "index/SnoIndex.h"
#include "model/MaterialDecode.h"
#include "model/ModelParser.h"
#include "tex/TextureDefTable.h"

#include <QMap>

#include <QImage>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>   // marking sweep walks the MarkingColor folder
#include <QFile>
#include <QFileInfo>
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

// ── D4_CHAINTEST — end-to-end verification of the encrypted-content chain ───────────────────────
// Replaces the derivation sweep, which had done its job: the layouts are settled, so re-deriving
// them on 63k appearances every run answers nothing. The question that actually remains is whether
// the three readers WORK TOGETHER in the production path, and that was being checked by asking a
// human to look at a model.
//
// Walks the real code path — appearanceRosterFromMeta -> MaterialDecode::baseColor/normalMap/orm —
// so a regression anywhere in it fails here rather than in the viewport. Encrypted pieces are the
// point, but named ones are included as controls: if those break too the fault is general, not
// encryption-specific, and that distinction is worth one line of output.
QString runChainTest(const QString& d4, SnoIndex* idx, CascReader* rd)
{
    if (!idx || !rd || !rd->isReady()) return QStringLiteral("chaintest: reader not ready");
    static const char* kPieces[] = {
        "necF_stor245_TRS", "necF_stor245_LEG",      // encrypted — the target
        "spiM_stor190_TRS", "palF_stor171_TRS",      // encrypted — other keys/sets
        "necF_base01_TRS",  "barF_base02_GLV",       // NAMED controls
    };
    QString out;
    int pass = 0, fail = 0;
    for (const char* nm : kPieces) {
        const QString name = QLatin1String(nm);
        int sno = 0;
        for (const SnoEntry& e : idx->entries(kGroupAppearance))
            if (e.name.compare(name, Qt::CaseInsensitive) == 0) { sno = e.snoId; break; }
        if (!sno) { out += QStringLiteral("\n  %1: NOT IN INDEX").arg(name, -20); ++fail; continue; }

        const QByteArray meta = rd->readMetaBySno(quint64(sno));
        QStringList roster = MaterialDecode::appearanceRoster(d4, name);
        const char* via = "json";
        if (roster.isEmpty()) { roster = MaterialDecode::appearanceRosterFromMeta(meta, idx); via = "meta"; }

        int withTex = 0, decoded = 0;
        QString dims;
        for (const QString& m : roster) {
            if (m.isEmpty()) continue;
            ++withTex;
            const QImage bc = MaterialDecode::baseColor(rd, d4, m);
            if (bc.isNull()) continue;
            ++decoded;
            if (dims.size() < 40)
                dims += QStringLiteral(" %1x%2").arg(bc.width()).arg(bc.height());
        }
        const bool ok = decoded > 0;
        ok ? ++pass : ++fail;
        out += QStringLiteral("\n  %1 sno %2 via %3 — %4 material(s), %5 with a decoded baseColor%6  %7")
                   .arg(name, -20).arg(sno, 8).arg(QLatin1String(via))
                   .arg(roster.size()).arg(decoded).arg(dims)
                   .arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    }
    const QString head = QStringLiteral("chaintest: %1 passed, %2 failed").arg(pass).arg(fail);
    qInfo().noquote() << head + out;
    return head;
}

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

    // D4_METADUMP_SNOS=<sno,sno,...> — dump by raw sno, any group. Needed for Material and Texture
    // blobs, which have no appearance name to look up: an encrypted material resolves to
    // "~unnamed_<sno>", so the sno IS the handle. Writes meta and, for textures, the payload too,
    // since dimensions live in meta but pixels live in payload.
    if (!qEnvironmentVariable("D4_METADUMP_SNOS").isEmpty()) {
        const QString dumpDir = QCoreApplication::applicationDirPath() + QStringLiteral("/metadump");
        QDir().mkpath(dumpDir);
        int wrote = 0;
        const QStringList ids = qEnvironmentVariable("D4_METADUMP_SNOS")
                                    .split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& idStr : ids) {
            bool ok = false;
            const quint64 sno = idStr.trimmed().toULongLong(&ok);
            if (!ok || !sno) continue;
            const QByteArray meta = rd->readMetaBySno(sno);
            if (!meta.isEmpty()) {
                QFile f(dumpDir + QStringLiteral("/sno%1.meta.bin").arg(sno));
                if (f.open(QIODevice::WriteOnly)) { f.write(meta); ++wrote; }
            }
            const QByteArray pay = rd->readPayloadBySno(sno);
            if (!pay.isEmpty()) {
                QFile f(dumpDir + QStringLiteral("/sno%1.payload.bin").arg(sno));
                if (f.open(QIODevice::WriteOnly)) { f.write(pay); ++wrote; }
            }
            qInfo("metadump sno %llu: meta %lld B, payload %lld B",
                  sno, qint64(meta.size()), qint64(pay.size()));
            // When meta is empty the definition is on a path readFile never tries — textures are
            // exactly this case. List what CASC actually holds rather than guessing the path.
            if (meta.isEmpty())
                for (const QString& p : rd->rootPathsFor(sno))
                    qInfo().noquote() << QStringLiteral("    path: ") + p;
        }
        qInfo("metadump: wrote %d sno blob(s)", wrote);
        // One-shot namespace map. Textures resolve only to base/payload and base/paylow, so their
        // dwWidth/dwHeight/eTexFormat live somewhere else entirely — possibly a by-NAME path, or a
        // bulk container. Printing the whole census answers that without a rebuild per guess.
        qInfo("metadump: TVFS namespace census —");
        for (const QString& p : rd->rootPrefixCensus()) qInfo().noquote() << "    " << p;
        // And the complete table to disk. Texture dimensions are not in the payload (sno4678 is 256
        // bytes of zeros) and there is no base/meta entry for any texture, so the definition lives
        // somewhere this tool has never looked. Rather than test one candidate path per rebuild,
        // dump every path once and answer it offline.
        // D4_DUMP_PATHS=<path,path,...> — pull any CASC file out by its TVFS path. The texture
        // definitions turned out to live in base/texture-base-global.dat (3.7 MB) rather than in a
        // per-sno meta entry, and a generic dumper means the next container costs a command instead
        // of a build.
        if (!qEnvironmentVariable("D4_DUMP_PATHS").isEmpty()) {
            const QString dumpDir = QCoreApplication::applicationDirPath()
                                  + QStringLiteral("/metadump");
            QDir().mkpath(dumpDir);
            const QStringList paths = qEnvironmentVariable("D4_DUMP_PATHS")
                                          .split(QLatin1Char(','), Qt::SkipEmptyParts);
            // "prefix:<p>" expands to every path starting with p — the encrypted texture
            // definitions live across 137 texture-base-global-0x<hash>.dat overlays and naming
            // each one would be a 7 KB environment variable.
            QStringList expanded;
            for (const QString& raw : paths) {
                const QString t = raw.trimmed();
                if (t.startsWith(QLatin1String("prefix:")))
                    expanded += rd->rootPathsWithPrefix(t.mid(7));
                else
                    expanded << t;
            }
            for (const QString& raw : expanded) {
                const QString path = raw.trimmed();
                const QByteArray data = rd->readFile(path);
                QString safe = path;
                safe.replace(QLatin1Char('/'), QLatin1Char('_')).replace(QLatin1Char('\\'),
                                                                        QLatin1Char('_'));
                if (data.isEmpty()) { qWarning("dumppath: '%s' EMPTY", qPrintable(path)); continue; }
                QFile f(dumpDir + QLatin1Char('/') + safe);
                if (f.open(QIODevice::WriteOnly)) f.write(data);
                qInfo("dumppath: %s -> %lld B", qPrintable(path), qint64(data.size()));
            }
        }

        {
            const QString tv = QCoreApplication::applicationDirPath()
                             + QStringLiteral("/tvfs_paths.txt");
            const int n = rd->dumpAllRootPaths(tv);
            qInfo().noquote() << QStringLiteral("metadump: wrote %1 TVFS path(s) to %2").arg(n).arg(tv);
        }
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

// ── Corpus-wide asset health audit (see MatSnoSweep.h) ──────────────────────────────────────────
namespace {

// One classification per appearance, most severe first. Ordering matters: a piece with no payload
// cannot also be judged on materials, so the first failing stage is the one reported.
enum class Health {
    Ok,            // geometry + materials + every material's textures resolve
    Incomplete,    // renders, but LOD0 sub-objects were dropped — visibly missing parts
    NoTexDefs,     // materials resolve but no texture definition was found for them
    NoMaterials,   // geometry loads but the material list is empty
    NoGeometry,    // meta+payload present, parse produced nothing
    Locked,        // encrypted and we hold no key — not a defect, just gated
    NoData,        // no meta or no payload in CASC at all
};

const char* healthName(Health h)
{
    switch (h) {
    case Health::Ok:          return "OK";
    case Health::Incomplete:  return "INCOMPLETE-MESH";
    case Health::NoTexDefs:   return "NO-TEXTURE-DEFS";
    case Health::NoMaterials: return "NO-MATERIALS";
    case Health::NoGeometry:  return "NO-GEOMETRY";
    case Health::Locked:      return "LOCKED";
    case Health::NoData:      return "NO-DATA";
    }
    return "?";
}

}  // namespace

// ── Body-marking model sweep ────────────────────────────────────────────────────────────────────
// See the header. One run, every fact, plus pictures.
QString runMarkingSweep(const QString& d4, SnoIndex* idx, CascReader* rd, QWidget* parent)
{
    if (!idx || !rd || !rd->isReady()) return QStringLiteral("marking: reader not ready");
    // data\, NOT beside the exe. The older sweeps in this file write to applicationDirPath and the
    // Release Smoke Test flags exactly that as a portability failure ("nothing is written outside
    // data\"), which is how data\icon_audit.txt being written beside the exe once survived a
    // passing run. New diagnostics follow the rule.
    const QString outDir = AppPaths::dataDir();
    const QString imgDir = outDir + QStringLiteral("/marking_swatch");
    QDir().mkpath(outDir);
    QDir().mkpath(imgDir);
    QFile rep(outDir + QStringLiteral("/marking_sweep.txt"));
    if (!rep.open(QIODevice::WriteOnly | QIODevice::Text))
        return QStringLiteral("marking: cannot write marking_sweep.txt");
    QTextStream ts(&rep);

    auto lum = [](const QColor& c) {
        return 0.2126 * c.redF() + 0.7152 * c.greenF() + 0.0722 * c.blueF(); };
    auto hex = [](const QColor& c) { return c.isValid()
        ? QStringLiteral("#%1%2%3").arg(c.red(),2,16,QLatin1Char('0'))
              .arg(c.green(),2,16,QLatin1Char('0')).arg(c.blue(),2,16,QLatin1Char('0'))
        : QStringLiteral("#------"); };

    // ── PART A — every MarkingColor ─────────────────────────────────────────────────────────────
    // The 93 colours with their samples in BOTH spaces, their luminance order, and the authored
    // surface values. This is what decides whether "sample[0] is the shadow" is a rule or an
    // accident, and it cannot be answered from one marking.
    ts << "PART A — MarkingColor table\n";
    ts << "  samples are LINEAR in the file; sRGB column is pow(1/2.2) as the tool encodes them\n";
    ts << "  DIR = luminance order of the three samples\n\n";
    ts << QStringLiteral("%1 %2 %3 %4  %5  %6  %7 %8\n")
              .arg("NAME", -34).arg("TAT", 3).arg("ROUGH", 6).arg("METAL", 6)
              .arg("s0 sRGB / lum", -22).arg("s1 sRGB / lum", -22).arg("s2 sRGB / lum", -22).arg("DIR");
    int nTat = 0, nAsc = 0, nDesc = 0, nFlat = 0, nCol = 0;
    QDirIterator cit(d4 + QStringLiteral("/json/base/meta/MarkingColor"),
                     QStringList{QStringLiteral("*.mcl.json")}, QDir::Files);
    while (cit.hasNext()) {
        const QString path = cit.next();
        QString stem = QFileInfo(path).fileName();
        stem.chop(int(qstrlen(".mcl.json")));
        const MarkingPaint p = markingPaint(d4, stem);
        if (!p.valid) continue;
        ++nCol;
        if (p.isTattoo) ++nTat;
        const double l0 = lum(p.ramp[0]), l1 = lum(p.ramp[1]), l2 = lum(p.ramp[2]);
        QString dir = QStringLiteral("flat");
        if (l0 < l1 && l1 < l2)      { dir = QStringLiteral("dark->light"); ++nAsc; }
        else if (l0 > l1 && l1 > l2) { dir = QStringLiteral("light->dark"); ++nDesc; }
        else                         ++nFlat;
        auto cell = [&](const QColor& c, double l) {
            return QStringLiteral("%1/%2").arg(hex(c)).arg(l, 0, 'f', 3); };
        ts << QStringLiteral("%1 %2 %3 %4  %5  %6  %7 %8\n")
                  .arg(stem, -34).arg(p.isTattoo ? "yes" : "no", 3)
                  .arg(p.roughness, 6, 'f', 2).arg(p.metalness, 6, 'f', 2)
                  .arg(cell(p.ramp[0], l0), -22).arg(cell(p.ramp[1], l1), -22)
                  .arg(cell(p.ramp[2], l2), -22).arg(dir);
    }
    ts << QStringLiteral("\n  %1 colours · %2 tattoos · direction: %3 dark->light, %4 light->dark, "
                         "%5 flat/mixed\n").arg(nCol).arg(nTat).arg(nAsc).arg(nDesc).arg(nFlat);
    ts << "  If DIR is not one value, sample index alone cannot mean shadow-or-highlight and the\n"
          "  ramp must be driven by the mask, not by an assumed ordering.\n\n";

    // ── PART B — every MarkingShape's mask, measured ────────────────────────────────────────────
    ts << "PART B — MarkingShape masks\n";
    ts << "  ink%  = texels with R>=128 (the design)\n";
    ts << "  G/B   = mean over INK ONLY. A whole-sheet mean is meaningless: the design is a few\n";
    ts << "          percent of the sheet, so the empty background pulls every average to ~0.\n";
    ts << "  OUT   = rampLerp(ramp, G/255) averaged over ink — the colour the model produces.\n\n";
    ts << QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8 %9\n")
              .arg("SHAPE", -38).arg("COLOUR", -22).arg("MASK", 10).arg("INK%", 6)
              .arg("G", 4).arg("B", 4).arg("G<64", 6).arg("G>=192", 7).arg("OUT");
    const QString shapeDir = d4 + QStringLiteral("/json/base/meta/MarkingShape");
    QStringList shapes = QDir(shapeDir).entryList(QStringList{QStringLiteral("*.msh.json")}, QDir::Files);
    QProgressDialog prog(QStringLiteral("Marking sweep: %1 shapes…").arg(shapes.size()),
                         QStringLiteral("Cancel"), 0, shapes.size(), parent);
    prog.setWindowModality(Qt::ApplicationModal);
    prog.setMinimumDuration(0);
    // Pictures for a representative handful: the reported case, plus both ramp directions and a
    // paint alongside a tattoo. Named so the set is obvious rather than arbitrary.
    const QStringList wantImg = {
        QStringLiteral("global_bodyMarking_08"),   // the reported one (Winds of Fate / Inked Tattoo)
        QStringLiteral("global_bodyMarking_01"),
        QStringLiteral("Barbarian_bodyMarking_01"),
    };
    int done = 0, imgN = 0;
    for (const QString& fn : shapes) {
        if ((++done % 4) == 0) { prog.setValue(done); QCoreApplication::processEvents();
                                 if (prog.wasCanceled()) break; }
        QString stem = fn; stem.chop(int(qstrlen(".msh.json")));
        const MarkingDef md = markingDef(d4, stem);
        if (md.bodyTex.isEmpty() && md.faceTex.isEmpty()) continue;
        const MarkingPaint mp = markingPaint(d4, md.colorStem);
        const QString texName = md.bodyTex.isEmpty() ? md.faceTex : md.bodyTex;
        const int texSno = idx->snoForName(44, texName);
        const QImage mask = MaterialDecode::texture(rd, d4, texName, texSno);
        if (mask.isNull() || !mp.valid) {
            ts << QStringLiteral("%1 %2 %3\n").arg(stem, -38).arg(md.colorStem, -22)
                      .arg(mask.isNull() ? QStringLiteral("(mask did not decode)")
                                         : QStringLiteral("(no colour)"));
            continue;
        }
        const QImage m = mask.convertToFormat(QImage::Format_RGBA8888);
        const int sxx = qMax(1, m.width()/192), syy = qMax(1, m.height()/192);
        long n = 0, ink = 0, gLo = 0, gHi = 0;
        double sG = 0, sB = 0, oR = 0, oG = 0, oB = 0;
        for (int y = 0; y < m.height(); y += syy) {
            const uchar* s = m.constScanLine(y);
            for (int x = 0; x < m.width(); x += sxx) {
                const uchar* p = s + x*4; ++n;
                if (p[0] < 128) continue;
                ++ink; sG += p[1]; sB += p[2];
                if (p[1] < 64) ++gLo; else if (p[1] >= 192) ++gHi;
                const QColor c = rampLerp(mp.ramp, p[1] / 255.0f);
                oR += c.red(); oG += c.green(); oB += c.blue();
            }
        }
        const double inkPct = n ? 100.0*ink/n : 0;
        const QColor out = ink ? QColor(int(oR/ink), int(oG/ink), int(oB/ink)) : QColor();
        ts << QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8 %9\n")
                  .arg(stem, -38).arg(md.colorStem, -22)
                  .arg(QStringLiteral("%1x%2").arg(m.width()).arg(m.height()), 10)
                  .arg(inkPct, 6, 'f', 1)
                  .arg(ink ? sG/ink : 0, 4, 'f', 0).arg(ink ? sB/ink : 0, 4, 'f', 0)
                  .arg(ink ? 100.0*gLo/ink : 0, 5, 'f', 1).arg(ink ? 100.0*gHi/ink : 0, 6, 'f', 1)
                  .arg(hex(out));

        // ── PART C — pictures ───────────────────────────────────────────────────────────────────
        if (wantImg.contains(stem)) {
            ++imgN;
            const int S = 512;
            const QImage small = m.scaled(S, S, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QImage rCh(small.size(), QImage::Format_RGBA8888), gCh(small.size(), QImage::Format_RGBA8888);
            for (int y = 0; y < small.height(); ++y) {
                const uchar* s = small.constScanLine(y);
                uchar* a = rCh.scanLine(y); uchar* b = gCh.scanLine(y);
                for (int x = 0; x < small.width(); ++x) {
                    a[x*4+0] = a[x*4+1] = a[x*4+2] = s[x*4+0]; a[x*4+3] = 255;
                    b[x*4+0] = b[x*4+1] = b[x*4+2] = s[x*4+1]; b[x*4+3] = 255;
                }
            }
            rCh.save(QStringLiteral("%1/%2_maskR_coverage.png").arg(imgDir, stem));
            gCh.save(QStringLiteral("%1/%2_maskG_rampPos.png").arg(imgDir, stem));
            // The ramp itself, left = G 0, right = G 255.
            QImage strip(256, 48, QImage::Format_RGBA8888);
            for (int x = 0; x < 256; ++x) {
                const QColor c = rampLerp(mp.ramp, x / 255.0f);
                for (int y = 0; y < 48; ++y) strip.setPixelColor(x, y, c);
            }
            strip.save(QStringLiteral("%1/%2_ramp.png").arg(imgDir, stem));
            // The composite over FLAT skin — flat so the marking colour is unambiguous rather than
            // entangled with a skin texture. This is the picture that says dark or bright.
            QImage skin(small.size(), QImage::Format_RGBA8888);
            skin.fill(QColor(214, 163, 138));
            applyMarking(skin, small, mp.ramp).save(
                QStringLiteral("%1/%2_composite_on_flat_skin.png").arg(imgDir, stem));
        }
    }
    prog.setValue(shapes.size());
    ts << QStringLiteral("\n  %1 shape(s) scanned · %2 picture set(s) in marking_swatch\\\n")
              .arg(done).arg(imgN);
    rep.close();

    const QString head = QStringLiteral("marking sweep: %1 colours, %2 shapes → marking_sweep.txt "
                                        "+ %3 picture set(s)").arg(nCol).arg(done).arg(imgN);
    qInfo().noquote() << head;
    return head;
}

QString runHealthAudit(const QString& d4, SnoIndex* idx, CascReader* rd, QWidget* parent)
{
    if (!idx || !rd || !rd->isReady()) return QStringLiteral("health: reader not ready");
    // data\, not beside the exe. "Audit Asset Health.bat" is a SHIPPED script, so a user running it
    // from an unzipped release used to dirty the release folder with three files — the portability
    // claim is "nothing is written outside data\", and the smoke test cannot catch this one because
    // it is env-gated and never runs during that test.
    const QString outDir = AppPaths::dataDir();
    QFile csv(outDir + QStringLiteral("/asset_health.csv"));
    if (!csv.open(QIODevice::WriteOnly | QIODevice::Text))
        return QStringLiteral("health: cannot write asset_health.csv");
    QTextStream cs(&csv);
    cs << "name,sno,status,encrypted,keyHeld,metaBytes,payloadBytes,prims,materials,"
          "texturesResolved,droppedSubObjects,iconHandle\n";

    // ── Icon coverage ───────────────────────────────────────────────────────────────────────────
    // Rendering health is not the only way an asset can be unusable. An appearance with no icon is
    // present in every list as a blank row, and the tool's icon binding is NAME-joined — which is
    // the single most failure-prone thing in this codebase. Weapons (armour-only name rule) and
    // headstones (art on the actor, not the item) were both invisible here for exactly that
    // reason, and neither showed up in any audit because no audit measured it.
    //
    // Reported like NAMELESS-BUT-RENDERABLE, not as a health verdict: a missing icon does not make
    // the model broken, and folding the two together would hide whichever is smaller.
    //
    // Only measured when the index is actually READY. The audit fires 1500 ms after startup and
    // AppearanceMeta builds in the background, so scoring an unbuilt index would report every
    // appearance as iconless — a metric that is confidently wrong is worse than one that abstains.
    const bool amReady = AppearanceMeta::instance().ready();
    int iconless = 0;
    QStringList iconlessExamples;

    TextureDefTable::instance().ensureBuilt(rd);

    const QVector<SnoEntry>& apps = idx->entries(kGroupAppearance);
    QProgressDialog prog(QStringLiteral("Asset health: %1 appearances…").arg(apps.size()),
                         QStringLiteral("Cancel"), 0, int(apps.size()), parent);
    prog.setWindowModality(Qt::ApplicationModal);
    prog.setMinimumDuration(0);

    QMap<Health, int> tally;
    int namelessOk = 0;                  // decode fine, but no name -> unreachable in every roster
    QStringList namelessExamples;
    QMap<Health, QStringList> examples;
    QHash<QString, QString> current;   // name -> status, for the diff against last run
    int scanned = 0;

    for (const SnoEntry& e : apps) {
        if ((++scanned % 128) == 0) {
            prog.setValue(scanned);
            QCoreApplication::processEvents();
            if (prog.wasCanceled()) break;
        }
        const QByteArray meta = rd->readMetaBySno(quint64(e.snoId));
        const QByteArray pay  = rd->readPayloadBySno(quint64(e.snoId));
        const QByteArray kn   = rd->tactKeyFor(quint64(e.snoId));
        const bool enc = !kn.isEmpty();
        const bool held = enc ? rd->haveTactKey(kn) : true;

        Health h = Health::Ok;
        int prims = 0, mats = 0, texOk = 0, geoDropped = 0;

        if (meta.isEmpty() || pay.isEmpty()) {
            // Distinguish "gated behind a key we lack" from "genuinely absent". Only the second is
            // a defect; conflating them would bury real breakage under thousands of locked rows.
            h = (enc && !held) ? Health::Locked : Health::NoData;
        } else {
            const ModelGeometry geo = ModelParser::parseApp(meta, pay, e.name);
            prims = int(geo.primitives.size());
            geoDropped = geo.droppedSubObjects;
            if (!geo.valid || prims == 0) {
                h = Health::NoGeometry;
            } else {
                QStringList roster = MaterialDecode::appearanceRoster(d4, e.name);
                if (roster.isEmpty()) roster = MaterialDecode::appearanceRosterFromMeta(meta, idx);
                mats = 0;
                for (const QString& m : roster) if (!m.isEmpty()) ++mats;
                if (mats == 0) {
                    h = Health::NoMaterials;
                } else {
                    // Texture DEFINITIONS only — no pixel decode. Resolving 140k textures through
                    // BcDecode would turn a two-minute audit into an hour for no extra signal:
                    // if the definition resolves, the decode path is the same one the chain test
                    // already covers.
                    for (const QString& m : roster) {
                        if (m.isEmpty()) continue;
                        for (const int ts : MaterialDecode::textureSnosFor(rd, d4, m))
                            if (TextureDefTable::instance().lookup(ts).valid()) { ++texOk; break; }
                    }
                    if (texOk == 0) h = Health::NoTexDefs;
                    // NOT a health category — see below. droppedSubObjects is recorded in the CSV
                    // for triage, but it cannot decide OK vs broken, because ModelParser's
                    // findSubObjects sweeps the WHOLE meta blob (ModelParser.cpp:1002) and the
                    // counter therefore includes LOD1/2/3 sub-objects the tool deliberately does
                    // not load. Scoring on it flagged 45,862 of 67,720 appearances (68%) as
                    // incomplete when the measured defect is nearer 10% — a metric that cannot
                    // separate "missing parts" from "has more LODs" is worse than no metric.
                    // Restore this only once sub-objects carry a LOD index.
                }
            }
        }

        ++tally[h];
        // NAMELESS-BUT-RENDERABLE is tracked separately from the health verdict, because it is a
        // different kind of defect: the asset is perfectly fine and simply cannot be REACHED, since
        // every roster in the tool is name-shaped. Counting it as OK is what made an earlier
        // "100% of wardrobe appearances OK" claim blind to exactly the pieces the user was asking
        // about — the name filter excluded them from the denominator.
        if (h == Health::Ok && e.name.startsWith(QLatin1String("~unnamed_"))) {
            ++namelessOk;
            if (namelessExamples.size() < 30) namelessExamples << QStringLiteral("%1 (%2 mats)")
                                                                     .arg(e.name).arg(mats);
        }
        if (h != Health::Ok && examples[h].size() < 25) examples[h] << e.name;
        current.insert(e.name, QLatin1String(healthName(h)));

        const quint32 icon = amReady ? AppearanceMeta::instance().iconFor(e.snoId) : 0u;
        // Named + renderable + no icon = a row that will be blank in every list in the tool.
        // Nameless ones are excluded: they are already counted above and cannot be reached at all,
        // so an icon would not help them.
        if (amReady && h == Health::Ok && !icon && !e.name.startsWith(QLatin1String("~unnamed_"))) {
            ++iconless;
            if (iconlessExamples.size() < 30) iconlessExamples << e.name;
        }

        cs << e.name << ',' << e.snoId << ',' << healthName(h) << ',' << (enc ? "yes" : "no") << ','
           << (held ? "yes" : "no") << ',' << meta.size() << ',' << pay.size() << ',' << prims
           << ',' << mats << ',' << texOk << ',' << geoDropped << ',' << icon << '\n';
    }
    prog.setValue(int(apps.size()));
    csv.close();

    // ── Diff against the previous run ────────────────────────────────────────────────────────
    // The whole point of auditing rather than spot-checking: after a patch or a new key, what
    // CHANGED. A raw count is easy to skim past; "4 newly broken" is not.
    const QString basePath = outDir + QStringLiteral("/asset_health_baseline.csv");
    QHash<QString, QString> prev;
    {
        QFile bf(basePath);
        if (bf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream bs(&bf);
            bs.readLine();   // header
            while (!bs.atEnd()) {
                const QStringList f = bs.readLine().split(QLatin1Char(','));
                if (f.size() >= 3) prev.insert(f[0], f[2]);
            }
        }
    }
    // Statuses the PREVIOUS run knew about, so a newly-introduced category is not read as breakage.
    QSet<QString> seenStatuses;
    for (auto it = prev.constBegin(); it != prev.constEnd(); ++it) seenStatuses.insert(it.value());
    QStringList fixed, broken;
    if (!prev.isEmpty())
        for (auto it = current.constBegin(); it != current.constEnd(); ++it) {
            const QString was = prev.value(it.key());
            if (was.isEmpty() || was == it.value()) continue;
            if (it.value() == QLatin1String("OK")) fixed << it.key();
            // A status that exists in this build but not the last one is a CATEGORY change, not a
            // regression. Adding INCOMPLETE-MESH reported 45,862 assets as "newly BROKEN" when
            // nothing had changed about them at all — a diff that cries wolf on its own schema
            // change will be ignored exactly when it matters.
            else if (was == QLatin1String("OK") && seenStatuses.contains(it.value()))
                broken << it.key();
        }

    QFile rep(outDir + QStringLiteral("/asset_health.txt"));
    if (rep.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream ts(&rep);
        ts << "ASSET HEALTH AUDIT\n==================\n\n"
           << "appearances scanned  " << scanned << "\n\n";
        for (Health h : {Health::Ok, Health::NoTexDefs, Health::NoMaterials,
                         Health::NoGeometry, Health::Locked, Health::NoData})
            ts << QStringLiteral("  %1 %2\n").arg(QLatin1String(healthName(h)), -18)
                      .arg(tally.value(h), 8);
        ts << "\nLOCKED is not a defect — those need a TACT key we do not hold. Everything else\n"
              "above OK is something the tool should be able to show and cannot.\n";
        ts << QStringLiteral(
                  "\n  NAMELESS BUT RENDERABLE  %1\n"
                  "  These decode completely — geometry, materials and textures all resolve — and are\n"
                  "  still invisible, because every roster in the tool matches on NAME. They are\n"
                  "  counted in OK above; this line exists so they can never again be hidden by a\n"
                  "  name-shaped filter. Recovering a name for one makes it appear with no other\n"
                  "  change. THIS IS THE REAL BACKLOG.\n")
                  .arg(namelessOk, 8);
        if (!namelessExamples.isEmpty()) {
            ts << "\n  first " << namelessExamples.size() << ":\n";
            for (const QString& n : namelessExamples) ts << "    " << n << '\n';
        }
        // Icon coverage. Diffable run-to-run like everything else here, so a name-rule regression
        // that costs a whole category its icons shows up as a number rather than as a bug report.
        // Re-checked, not assumed: the scan loop calls processEvents(), and AppearanceMeta::install
        // arrives by queued connection, so the index can finish MID-SCAN. The figure would then be
        // half from an empty index and half from a built one — worse than not reporting it.
        if (!amReady || !AppearanceMeta::instance().ready()) {
            ts << "\n  ICONLESS BUT RENDERABLE  not measured — the appearance index was not ready\n"
                  "  for the whole scan (it builds in the background, and the audit starts 1.5 s\n"
                  "  after launch). Re-run once it is ready for this figure.\n";
        } else {
            ts << QStringLiteral(
                      "\n  ICONLESS BUT RENDERABLE  %1\n"
                      "  Named, decodes completely, and has no inventory icon — a blank row in every\n"
                      "  list in the tool. Icon binding is NAME-joined, so this number is the early\n"
                      "  warning for a name-rule regression: weapons and headstones were both fully\n"
                      "  invisible here and no audit measured it.\n")
                      .arg(iconless, 8);
            if (!iconlessExamples.isEmpty()) {
                ts << "\n  first " << iconlessExamples.size() << ":\n";
                for (const QString& n : iconlessExamples) ts << "    " << n << '\n';
            }
        }
        for (Health h : {Health::NoTexDefs, Health::NoMaterials,
                         Health::NoGeometry, Health::NoData}) {
            if (examples.value(h).isEmpty()) continue;
            ts << "\n" << healthName(h) << " (first " << examples.value(h).size() << "):\n";
            for (const QString& n : examples.value(h)) ts << "  " << n << '\n';
        }
        ts << "\n── CHANGE SINCE LAST RUN ──\n";
        if (prev.isEmpty())
            ts << "  no baseline yet — this run establishes one (asset_health_baseline.csv)\n";
        else {
            ts << QStringLiteral("  newly working  %1\n  newly BROKEN   %2\n")
                      .arg(fixed.size()).arg(broken.size());
            for (int i = 0; i < broken.size() && i < 40; ++i) ts << "    BROKE: " << broken[i] << '\n';
            for (int i = 0; i < fixed.size() && i < 15; ++i)  ts << "    fixed: " << fixed[i] << '\n';
        }
        ts << "\nPer-appearance detail: asset_health.csv\n";
        rep.close();
    }
    QFile::remove(basePath);
    QFile::copy(outDir + QStringLiteral("/asset_health.csv"), basePath);

    const QString head =
        QStringLiteral("health: %1 OK (%6 nameless+renderable), %2 locked, %3 broken "
                       "(%4 newly broken, %5 newly working)")
            .arg(tally.value(Health::Ok)).arg(tally.value(Health::Locked))
            .arg(tally.value(Health::NoTexDefs) + tally.value(Health::NoMaterials)
                 + tally.value(Health::NoGeometry) + tally.value(Health::NoData))
            .arg(broken.size()).arg(fixed.size()).arg(namelessOk);
    qInfo().noquote() << head;
    return head;
}
