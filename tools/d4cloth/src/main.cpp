// d4cloth — standalone, text-first cloth-physics diagnostic harness for
// Diablo4AssetBrowser Native. No GUI, no OpenGL, no wall clock.
//
//   d4cloth extract  --casc <gameDir> --d4data <snapshotDir> --cases cases.json --out corpus/
//   d4cloth inspect  --corpus corpus/ [--case <name>] [--piece <appearanceName>]... [--out report.txt]
//   d4cloth version
//
// Every command prints a run header naming the binary build and the exact input bytes
// (FNV-1a hashes), so a result can always say what produced it.

#include "AssetSource.h"
#include "ClothDoc.h"
#include "Coverage.h"

namespace d4cloth { int cmdRun(const QStringList& args); }

#include "model/ModelParser.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QStringList>

#include <cstdio>

using namespace d4cloth;

namespace {

const char* kBuildStamp = __DATE__ " " __TIME__;

void printRunHeader(const QString& cmd)
{
    std::printf("# d4cloth %s | build %s | deterministic: no wall clock, no RNG\n",
                qPrintable(cmd), kBuildStamp);
}

int cmdInspect(const QStringList& args)
{
    QCommandLineParser p;
    p.addOptions({
        { QStringLiteral("corpus"), QStringLiteral("corpus dir"), QStringLiteral("dir") },
        { QStringLiteral("case"),   QStringLiteral("case name from corpus manifest"), QStringLiteral("name") },
        { QStringLiteral("piece"),  QStringLiteral("appearance name (repeatable)"), QStringLiteral("name") },
        { QStringLiteral("out"),    QStringLiteral("write report to file (default stdout)"), QStringLiteral("file") },
        { QStringLiteral("csv"),    QStringLiteral("also write machine-readable coverage CSV"), QStringLiteral("file") },
    });
    p.process(QStringList() << QStringLiteral("d4cloth") << args);
    const QString corpusDir = p.value(QStringLiteral("corpus"));
    if (corpusDir.isEmpty()) { std::fprintf(stderr, "inspect: --corpus is required\n"); return 2; }
    CorpusSource corpus(corpusDir);

    QStringList pieces = p.values(QStringLiteral("piece"));
    if (!p.value(QStringLiteral("case")).isEmpty()) {
        QString err;
        const QVector<TestCase> cases =
            loadCases(corpusDir + QStringLiteral("/manifest.json"), &err);
        bool found = false;
        for (const TestCase& tc : cases)
            if (tc.name == p.value(QStringLiteral("case"))) { pieces = tc.pieces; found = true; break; }
        if (!found) {
            std::fprintf(stderr, "inspect: case '%s' not in %s/manifest.json (%s)\n",
                         qPrintable(p.value(QStringLiteral("case"))), qPrintable(corpusDir),
                         qPrintable(err));
            return 2;
        }
    }
    if (pieces.isEmpty()) { std::fprintf(stderr, "inspect: nothing to inspect (--case or --piece)\n"); return 2; }

    printRunHeader(QStringLiteral("inspect"));
    QString report, csv;
    int clothDocs = 0;
    for (const QString& piece : pieces) {
        const AssetBlob b = corpus.appearance(piece);
        if (!b.ok) { std::fprintf(stderr, "inspect: %s\n", qPrintable(b.error)); return 2; }
        report += QStringLiteral("## piece %1  meta=%2 B fnv=%3  payload=%4 B fnv=%5\n")
                      .arg(piece).arg(b.meta.size())
                      .arg(QString::number(fnv1a64(b.meta), 16))
                      .arg(b.payload.size())
                      .arg(QString::number(fnv1a64(b.payload), 16));

        // Geometry via the app's own parser (for skeleton cross-analysis + a comparison
        // point: what the APP would see vs what is actually authored).
        const ModelGeometry geo = ModelParser::parseApp(b.meta, b.payload);
        report += QStringLiteral("   app parser: primitives=%1 bones=%2 nBaseBones=%3 "
                                 "clothSims=%4 capsules=%5\n")
                      .arg(geo.primitives.size()).arg(geo.skeleton.size()).arg(geo.nBaseBones)
                      .arg(geo.clothSims.size()).arg(geo.clothCapsules.size());
        for (const ClothSim& cs : geo.clothSims)
            report += QStringLiteral("   app ClothSim '%1': verts=%2 constraints=%3 attachLen=%4 planes=%5%6\n")
                          .arg(cs.name).arg(cs.vertCount).arg(cs.constraintLen.size())
                          .arg(cs.attachLen.size()).arg(cs.planes.size())
                          .arg(cs.attachLen.isEmpty()
                                   ? QStringLiteral("  <<< FALLBACK: app would synthesize")
                                   : QString());

        // Full authored parse + coverage.
        QVector<ClothDoc> docs = parseClothDocs(b.meta, b.payload);
        report += QStringLiteral("   full parse: %1 ClothData block(s)\n\n").arg(docs.size());
        for (ClothDoc& d : docs) {
            d.pieceName = piece;
            report += coverageReport(d, &geo);
            // Tuning cross-reference (.clt.json, both naming conventions).
            const QJsonObject tuning = corpus.clothTuning(d.name);
            report += tuning.isEmpty()
                          ? QStringLiteral("  tuning: NO .clt.json found for '%1' (tried '%1' and '%1_sim')\n\n").arg(d.name)
                          : QStringLiteral("  tuning: .clt.json FOUND for '%1' (sno %2)\n\n")
                                .arg(d.name)
                                .arg(qint64(tuning.value(QStringLiteral("__snoID__")).toDouble(0)));
            csv += coverageCsv(d);
            ++clothDocs;
        }
    }
    report += QStringLiteral("== inspected %1 piece(s), %2 ClothData block(s)\n")
                  .arg(pieces.size()).arg(clothDocs);

    const QString outPath = p.value(QStringLiteral("out"));
    if (outPath.isEmpty()) {
        std::printf("%s", qPrintable(report));
    } else {
        QFile f(outPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            std::fprintf(stderr, "inspect: cannot write %s\n", qPrintable(outPath)); return 2;
        }
        f.write(report.toUtf8());
        std::printf("inspect: report -> %s (%d piece(s), %d ClothData block(s))\n",
                    qPrintable(outPath), int(pieces.size()), clothDocs);
    }
    if (!p.value(QStringLiteral("csv")).isEmpty()) {
        QFile f(p.value(QStringLiteral("csv")));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) f.write(csv.toUtf8());
    }
    return 0;
}

int cmdExtract(const QStringList& args)
{
    QCommandLineParser p;
    p.addOptions({
        { QStringLiteral("casc"),   QStringLiteral("game install dir"), QStringLiteral("dir") },
        { QStringLiteral("d4data"), QStringLiteral("d4data snapshot dir"), QStringLiteral("dir") },
        { QStringLiteral("cases"),  QStringLiteral("cases.json"), QStringLiteral("file") },
        { QStringLiteral("out"),    QStringLiteral("corpus output dir"), QStringLiteral("dir") },
    });
    p.process(QStringList() << QStringLiteral("d4cloth") << args);
    for (const char* req : { "casc", "d4data", "cases", "out" })
        if (p.value(QLatin1String(req)).isEmpty()) {
            std::fprintf(stderr, "extract: --%s is required\n", req); return 2;
        }
    printRunHeader(QStringLiteral("extract"));
    return runExtract(p.value(QStringLiteral("casc")), p.value(QStringLiteral("d4data")),
                      p.value(QStringLiteral("cases")), p.value(QStringLiteral("out")));
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("d4cloth"));

    QStringList args = QCoreApplication::arguments();
    args.removeFirst();
    const QString cmd = args.isEmpty() ? QString() : args.takeFirst();

    if (cmd == QLatin1String("inspect")) return cmdInspect(args);
    if (cmd == QLatin1String("run"))     return d4cloth::cmdRun(args);
    if (cmd == QLatin1String("extract")) return cmdExtract(args);
    if (cmd == QLatin1String("version")) { std::printf("d4cloth build %s\n", kBuildStamp); return 0; }

    std::fprintf(stderr,
        "d4cloth — cloth physics diagnostic harness (text-first, deterministic, headless)\n"
        "usage:\n"
        "  d4cloth extract --casc <gameDir> --d4data <dir> --cases cases.json --out corpus/\n"
        "  d4cloth inspect --corpus corpus/ (--case <name> | --piece <appearance>...) [--out report.txt] [--csv cov.csv]\n  d4cloth run --corpus corpus/ --case <name> --scenario rest|spin|gravity-drop [--steps N]\n              [--dump bones,particles] [--render-every N] [--param k=v] [--trace b:327] [--report-bones 326,327,328]\n"
        "  d4cloth version\n");
    return 2;
}
