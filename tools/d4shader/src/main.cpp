// d4shader — shader-reuse feasibility probe.
//
// ONE QUESTION, ANSWERED FROM BYTES: can Diablo IV's own compiled shaders be extracted and run,
// or does the game ship only shader DECLARATIONS (slot names, parameters) while the instructions
// live somewhere this tool can never reach?
//
// This matters because the app's renderer is hand-written GLSL 450 that REIMPLEMENTS what the
// game's shaders do, from the values the game authored. The comment in WardrobeTab2 says it
// outright — "the procedural Hero_Eye shader can't be run" — and everything downstream of that is
// a careful approximation. If the compiled programs are in CASC, the approximation can be replaced
// by the real thing. If they are not, the question closes permanently and the remaining fidelity
// gap is lighting and tonemap, not shading.
//
// Deliberately NOT a viewer. Nothing is rendered, nothing is decompiled. It reads, classifies and
// counts, and writes one report. A viewer built before this answer would have nothing to run.
//
// Reads the game install only. Does not touch the running game, does not attach to any process.

#include "casc/CascReader.h"
#include "index/CoreToc.h"
#include "model/Hardpoints.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QVector>
#include <QtEndian>

#include <algorithm>

namespace {

// SNO groups, from SnoIndex.cpp's table — not invented here.
constexpr int kGroupShader    = 107;
constexpr int kGroupShaderMap = 108;

QTextStream& out()
{
    static QTextStream s(stdout);
    return s;
}

// ── Which hardpoints does this appearance actually declare? ──────────────────────────────────────
// The Wardrobe seats an off-hand by looking up ONE hash on the body rig and leaving the mesh at the
// origin when it is absent — which is exactly what a floating shield looks like. Whether the rig
// carries HP_shield (1410210544) or only HP_leftWeapon (4036545548) is a fact in the appearance's
// own .app.json, and that file sits eight folders deep where nothing outside this machine can read
// it. Hash → name comes from Hardpoints::nameForHash, the verified table, never a second copy.
//
// Deliberately NOT Hardpoints::readInto: that one appends only hardpoints whose bone index resolves
// against a loaded skeleton, so with no geometry it would report nothing. The question here is what
// the file DECLARES, which is a level below that.
void dumpHardpoints(const QString& d4data, const QString& name, QTextStream& o)
{
    const QString path = QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4data, name);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { o << "   " << name << ": no .app.json\n"; return; }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonArray bones = root.value(QStringLiteral("tStructure")).toObject()
                                 .value(QStringLiteral("ptBoneData")).toArray();
    if (bones.isEmpty()) { o << "   " << name << ": no ptBoneData\n"; return; }
    const QJsonArray hps = bones.at(0).toObject().value(QStringLiteral("ptHardpoints")).toArray();
    o << "   " << name << ": " << hps.size() << " hardpoint(s)\n";
    for (const QJsonValue& v : hps) {
        const QJsonObject h = v.toObject();
        const quint32 hash = quint32(h.value(QStringLiteral("nNameHash")).toDouble());
        o << "      " << QStringLiteral("%1").arg(hash, 10) << "  "
          << Hardpoints::nameForHash(hash)
          << "   bone " << h.value(QStringLiteral("nBoneIndex")).toInt(-1) << "\n";
    }
}

// ── What is this blob? ──────────────────────────────────────────────────────────────────────────
// Reports what the FIRST BYTES say, and separately whether a known container appears anywhere
// inside. A shader record could legitimately be a small wrapper around an embedded program, so
// "does not start with DXBC" is not the same as "holds no DXBC" and the two are never conflated.
struct Verdict {
    QString head;        // classification of the first bytes
    QString embedded;    // container found later in the blob, or empty
};

QString hex(const QByteArray& b, int n)
{
    QString s;
    for (int i = 0; i < n && i < b.size(); ++i)
        s += QStringLiteral("%1 ").arg(quint8(b[i]), 2, 16, QLatin1Char('0'));
    return s.trimmed();
}

QString ascii(const QByteArray& b, int n)
{
    QString s;
    for (int i = 0; i < n && i < b.size(); ++i) {
        const char c = b[i];
        s += (c >= 32 && c < 127) ? QLatin1Char(c) : QLatin1Char('.');
    }
    return s;
}

Verdict classify(const QByteArray& b)
{
    Verdict v;
    if (b.isEmpty()) { v.head = QStringLiteral("(empty)"); return v; }

    if (b.startsWith("DXBC"))        v.head = QStringLiteral("DXBC — D3D compiled shader");
    else if (b.startsWith("DXIL"))   v.head = QStringLiteral("DXIL — D3D12 shader");
    else if (b.startsWith("BC\xC0\xDE")) v.head = QStringLiteral("LLVM bitcode (DXIL payload)");
    else if (b.size() >= 4 && qFromLittleEndian<quint32>(
                 reinterpret_cast<const uchar*>(b.constData())) == 0x07230203u)
        v.head = QStringLiteral("SPIR-V");
    else if (b.startsWith("\x07\x23\x02\x03")) v.head = QStringLiteral("SPIR-V (big-endian)");
    else if (b.startsWith("RIFF"))   v.head = QStringLiteral("RIFF container");
    else
        v.head = QStringLiteral("unknown — %1  |%2|").arg(hex(b, 8), ascii(b, 8));

    // Only search when the head did not already answer it, and only for containers whose presence
    // would change the conclusion.
    if (!v.head.startsWith(QLatin1String("DXBC")) && !v.head.startsWith(QLatin1String("DXIL"))) {
        for (const char* magic : {"DXBC", "DXIL", "SHEX", "SHDR"}) {
            const int at = b.indexOf(magic);
            if (at >= 0) {
                v.embedded = QStringLiteral("%1 at offset %2").arg(QLatin1String(magic)).arg(at);
                break;
            }
        }
    }
    return v;
}

QString sizeBucket(qint64 n)
{
    if (n == 0)      return QStringLiteral("0");
    if (n < 256)     return QStringLiteral("<256 B");
    if (n < 1024)    return QStringLiteral("256 B – 1 KB");
    if (n < 8192)    return QStringLiteral("1 – 8 KB");
    if (n < 65536)   return QStringLiteral("8 – 64 KB");
    return QStringLiteral(">64 KB");
}

struct Tally {
    int records = 0, withMeta = 0, withPayload = 0, neither = 0, encrypted = 0, keyMissing = 0;
    QMap<QString, int> headKinds, embeddedKinds, metaSizes, payloadSizes;
    qint64 metaBytes = 0, payloadBytes = 0;
};

void report(QTextStream& o, const QString& label, const Tally& t)
{
    o << "\n-- group " << label << " --\n";
    o << "   records in CoreTOC        : " << t.records << "\n";
    if (!t.records) {
        o << "   (nothing to read — this group is absent from the table)\n";
        return;
    }
    o << "   with a base/meta blob     : " << t.withMeta
      << "   (" << (t.metaBytes / 1024) << " KB total)\n";
    o << "   with a base/payload blob  : " << t.withPayload
      << "   (" << (t.payloadBytes / 1024) << " KB total)\n";
    o << "   with neither              : " << t.neither << "\n";
    o << "   TACT-encrypted            : " << t.encrypted
      << "   (of those, key missing: " << t.keyMissing << ")\n";

    auto dump = [&o](const char* title, const QMap<QString, int>& m) {
        if (m.isEmpty()) return;
        o << "   " << title << ":\n";
        QVector<QPair<int, QString>> v;
        for (auto i = m.constBegin(); i != m.constEnd(); ++i) v.append({i.value(), i.key()});
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (int i = 0; i < v.size() && i < 12; ++i)
            o << "     " << QStringLiteral("%1").arg(v[i].first, 6) << "  " << v[i].second << "\n";
        if (v.size() > 12) o << "     … and " << (v.size() - 12) << " more\n";
    };
    dump("first bytes say", t.headKinds);
    dump("container found inside", t.embeddedKinds);
    dump("meta size", t.metaSizes);
    dump("payload size", t.payloadSizes);
}

// Walk one group, reading every record. `dumpDir` non-empty writes the first `dumpN` blobs out
// whole, so a human can look at what the counts describe.
Tally sweep(CascReader& casc, int group, const QVector<SnoEntry>& entries,
            const QString& dumpDir, int dumpN, QTextStream& o, QTextStream* tsv)
{
    Tally t;
    t.records = int(entries.size());
    int dumped = 0;
    for (const SnoEntry& e : entries) {
        const quint64 sno = quint64(quint32(e.snoId));

        const QByteArray key = casc.tactKeyFor(sno);
        if (!key.isEmpty()) {
            ++t.encrypted;
            if (!casc.haveTactKey(key)) { ++t.keyMissing; continue; }
        }

        const QByteArray meta = casc.readMetaBySno(sno);
        const QByteArray pay  = casc.readPayloadBySno(sno);
        // Written for EVERY record, before any filtering. The group table in SnoIndex.cpp could
        // only ever be checked against the 23 folders the d4data sparse checkout carries; a group
        // with no folder was never measured at all, and its label is inherited rather than known.
        // This is the listing that lets any group be checked the way those 23 were.
        if (tsv) *tsv << group << '\t' << e.snoId << '\t' << e.name << '\t'
                      << meta.size() << '\t' << pay.size() << '\n';
        if (meta.isEmpty() && pay.isEmpty()) { ++t.neither; continue; }

        if (!meta.isEmpty()) {
            ++t.withMeta;
            t.metaBytes += meta.size();
            t.metaSizes[sizeBucket(meta.size())]++;
            const Verdict v = classify(meta);
            t.headKinds[QStringLiteral("meta: ") + v.head]++;
            if (!v.embedded.isEmpty()) t.embeddedKinds[QStringLiteral("meta: ") + v.embedded]++;
        }
        if (!pay.isEmpty()) {
            ++t.withPayload;
            t.payloadBytes += pay.size();
            t.payloadSizes[sizeBucket(pay.size())]++;
            const Verdict v = classify(pay);
            t.headKinds[QStringLiteral("payload: ") + v.head]++;
            if (!v.embedded.isEmpty()) t.embeddedKinds[QStringLiteral("payload: ") + v.embedded]++;
        }

        if (!dumpDir.isEmpty() && dumped < dumpN && (!meta.isEmpty() || !pay.isEmpty())) {
            ++dumped;
            const QString stem = QStringLiteral("%1/%2_%3").arg(dumpDir, QString::number(e.snoId),
                                                               QString(e.name).replace(QLatin1Char('/'), QLatin1Char('_')));
            if (!meta.isEmpty()) { QFile f(stem + QStringLiteral(".meta.bin"));
                                   if (f.open(QIODevice::WriteOnly)) f.write(meta); }
            if (!pay.isEmpty())  { QFile f(stem + QStringLiteral(".payload.bin"));
                                   if (f.open(QIODevice::WriteOnly)) f.write(pay); }
            o << "   dumped " << e.name << "  meta=" << meta.size()
              << "  payload=" << pay.size() << "\n";
        }
    }
    return t;
}

}   // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QString gameDir, product, outPath = QStringLiteral("shader_probe.txt"), dumpDir;
    int dumpN = 6;
    QVector<int> wanted;
    QString censusPath, d4data, hardpointNames;
    const QStringList args = QCoreApplication::arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString a = args[i];
        auto next = [&](QString* dst) { if (i + 1 < args.size()) *dst = args[++i]; };
        if      (a == QLatin1String("--casc"))    next(&gameDir);
        else if (a == QLatin1String("--product")) next(&product);
        else if (a == QLatin1String("--out"))     next(&outPath);
        else if (a == QLatin1String("--dump"))    next(&dumpDir);
        else if (a == QLatin1String("--dump-n")) { QString v; next(&v); dumpN = v.toInt(); }
        else if (a == QLatin1String("--census"))     next(&censusPath);
        else if (a == QLatin1String("--d4data"))     next(&d4data);
        else if (a == QLatin1String("--hardpoints")) next(&hardpointNames);
        else if (a == QLatin1String("--group"))  { QString v; next(&v);
                                                   for (const QString& g : v.split(QLatin1Char(',')))
                                                       if (const int n = g.trimmed().toInt()) wanted.append(n); }
    }
    // Hardpoints need only the d4data snapshot, so this runs before the CASC gate and can be the
    // whole job. With no --casc it is the whole job and we stop here.
    if (!hardpointNames.isEmpty() && !d4data.isEmpty()) {
        out() << "\n-- hardpoints declared by each appearance --\n";
        for (const QString& n : hardpointNames.split(QLatin1Char(','), Qt::SkipEmptyParts))
            dumpHardpoints(d4data, n.trimmed(), out());
        out() << "\n";
        out().flush();
        if (gameDir.isEmpty()) return 0;
    }
    if (gameDir.isEmpty()) {
        out() << "usage: d4shader --casc <game dir> [--product fenris] [--out report.txt]"
                 " [--dump <dir>] [--dump-n 6] [--group 107,108] [--census <tsv>]"
                 " [--d4data <dir> --hardpoints <appearance,appearance,…>]\n";
        out().flush();
        return 2;
    }

    CascReader casc;
    if (!casc.open(gameDir, product)) {
        out() << "could not open CASC at " << gameDir << " — " << casc.lastError() << "\n";
        out().flush();
        return 2;
    }
    out() << "CASC opened: product=" << casc.product() << " version=" << casc.openedVersion()
          << " tact keys=" << casc.tactKeyCount() << "\n";
    out().flush();

    const QByteArray toc = casc.readFile(QStringLiteral("base/CoreTOC.dat"));
    if (toc.isEmpty()) { out() << "CoreTOC.dat unreadable\n"; out().flush(); return 2; }
    const QHash<int, QVector<SnoEntry>> groups = parseCoreToc(toc);
    out() << "CoreTOC parsed: " << groups.size() << " groups\n\n";
    out().flush();

    if (!dumpDir.isEmpty()) QDir().mkpath(dumpDir);

    QString buf;
    QTextStream o(&buf);
    o << "=== d4shader — shader-reuse feasibility probe ===\n";
    o << "game    : " << gameDir << "\n";
    o << "product : " << casc.product() << "   version " << casc.openedVersion() << "\n";
    o << "\nThe question: are the game's compiled shader programs reachable, or does it ship only\n"
         "declarations? DXBC / DXIL / SPIR-V in the bytes below means reachable. Small records with\n"
         "no recognised container mean declaration-only, and the answer is no.\n";

    // ── Census: every group's record count and a sample of its names ────────────────────────────
    // Costs nothing — CoreTOC is already parsed, and this reads no asset at all. It exists because
    // SnoIndex.cpp's group table could only ever be verified against the 23 json/base/meta folders
    // the d4data sparse checkout carries; every group WITHOUT a folder was never measured, and its
    // label is inherited rather than known. That is how 107 came to be called "Shader" while
    // holding Waller/Teleporter/Vortex, and 108 "ShaderMap" while holding Paragon boards. This
    // makes any label in that table checkable against what the group actually contains.
    if (!censusPath.isEmpty()) {
        QFile cf(censusPath);
        if (cf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream c(&cf);
            c << "group\trecords\tsample_names\n";
            QVector<int> ids = groups.keys();
            std::sort(ids.begin(), ids.end());
            for (const int g : ids) {
                const QVector<SnoEntry>& v = groups.value(g);
                QStringList sample;
                for (int i = 0; i < v.size() && sample.size() < 12; ++i)
                    if (!v[i].name.isEmpty()) sample << v[i].name;
                c << g << '\t' << v.size() << '\t' << sample.join(QLatin1String(" | ")) << '\n';
            }
            c.flush();
            cf.close();

            // ── Every name, for every group small enough to audit ───────────────────────────────
            // A 12-name sample is enough to SPOT a wrong label; it is not enough to assign a right
            // one. The table's own rule is that a group is labelled only where every unambiguous
            // stem agrees, so fixing a label needs the whole list. Groups above the cap are the
            // ones already verified against a json/base/meta folder (Actor, Texture, Appearance,
            // Material, StringList …) — they are not in question and listing 148,000 texture names
            // would bury the ones that are.
            constexpr int kFullNameCap = 5000;
            const QString namesPath = censusPath + QStringLiteral(".names.tsv");
            QFile nf(namesPath);
            if (nf.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream n(&nf);
                n << "group\tsno\tname\n";
                int wrote = 0, skipped = 0;
                for (const int g : ids) {
                    const QVector<SnoEntry>& v = groups.value(g);
                    if (v.size() > kFullNameCap) { ++skipped; continue; }
                    for (const SnoEntry& e : v) { n << g << '\t' << e.snoId << '\t' << e.name << '\n'; ++wrote; }
                }
                n.flush();
                nf.close();
                out() << "names: " << wrote << " records over " << (ids.size() - skipped)
                      << " groups (" << skipped << " above the " << kFullNameCap
                      << "-record cap) -> " << namesPath << "\n";
            }
            out() << "census: " << ids.size() << " groups -> " << censusPath << "\n";
            out().flush();
        }
    }

    if (wanted.isEmpty()) wanted = {kGroupShader, kGroupShaderMap};

    // Every record name, for every group asked about. The TSV is the evidence; the report is the
    // summary of it.
    const QString tsvPath = outPath + QStringLiteral(".records.tsv");
    QFile tsvFile(tsvPath);
    QTextStream tsv;
    if (tsvFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        tsv.setDevice(&tsvFile);
        tsv << "group\tsno\tname\tmeta_bytes\tpayload_bytes\n";
    }

    Tally shaders, maps;
    for (const int g : wanted) {
        const Tally t = sweep(casc, g, groups.value(g), dumpDir, dumpN, out(),
                              tsvFile.isOpen() ? &tsv : nullptr);
        report(o, QString::number(g), t);
        if (g == kGroupShader)    shaders = t;
        if (g == kGroupShaderMap) maps    = t;
    }
    if (tsvFile.isOpen()) { tsv.flush(); tsvFile.close();
                            o << "\nEvery record name: " << tsvPath << "\n"; }

    o << "\n-- verdict --\n";
    const bool anyBytecode =
        std::any_of(shaders.headKinds.keyBegin(), shaders.headKinds.keyEnd(),
                    [](const QString& k) { return k.contains(QLatin1String("DXBC"))
                                               || k.contains(QLatin1String("DXIL"))
                                               || k.contains(QLatin1String("SPIR-V")); })
     || !shaders.embeddedKinds.isEmpty() || !maps.embeddedKinds.isEmpty();
    if (shaders.records == 0 && maps.records == 0)
        o << "   Neither group exists in this build's CoreTOC. Shaders are not shipped as SNO\n"
             "   records at all, so there is nothing here to extract.\n";
    else if (anyBytecode)
        o << "   Compiled shader bytecode IS present. Extraction is worth pursuing: next step is\n"
             "   one shader, decompiled and cross-compiled to GLSL, rendered on one material.\n";
    else
        o << "   No compiled shader container found in any record. On this evidence the game does\n"
             "   not ship runnable shader code here, and reimplementing in GLSL — what the app\n"
             "   already does — is the only path. Close the question and spend the effort on the\n"
             "   lighting rig and tonemap instead.\n";

    o.flush();
    out() << buf;
    out().flush();
    QFile f(outPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { f.write(buf.toUtf8()); f.close(); }
    out() << "\nWrote: " << outPath << "\n";
    out().flush();
    return 0;
}
