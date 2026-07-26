#include "AssetSource.h"
#include "ClothDoc.h"

#include "casc/CascReader.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

namespace d4cloth {

namespace {

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeAll(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(data) == data.size();
}

// Resolve <group>/<name>.<ext>.json in the d4data snapshot; returns the parsed doc + snoID.
QJsonObject d4dataJson(const QString& d4dataDir, const QString& group, const QString& name,
                       const QString& ext, quint64* sno, QString* foundPath)
{
    // Filenames are case-sensitive on disk but authored case varies (BarF vs barF) — try
    // exact first, then a case-insensitive scan of the folder.
    const QString folder = d4dataDir + QStringLiteral("/json/base/meta/") + group;
    QString path = folder + QLatin1Char('/') + name + QLatin1Char('.') + ext + QStringLiteral(".json");
    if (!QFile::exists(path)) {
        QDirIterator it(folder, { QStringLiteral("*.") + ext + QStringLiteral(".json") }, QDir::Files);
        path.clear();
        const QString want = name.toLower() + QLatin1Char('.') + ext + QStringLiteral(".json");
        while (it.hasNext()) {
            const QString p = it.next();
            if (QFileInfo(p).fileName().toLower() == want) { path = p; break; }
        }
        if (path.isEmpty()) return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(readAll(path));
    if (!doc.isObject()) return {};
    const QJsonObject o = doc.object();
    if (sno) *sno = quint64(o.value(QStringLiteral("__snoID__")).toDouble(0));
    if (foundPath) *foundPath = path;
    return o;
}

} // namespace

QVector<TestCase> loadCases(const QString& casesJsonPath, QString* err)
{
    QVector<TestCase> out;
    const QByteArray raw = readAll(casesJsonPath);
    if (raw.isEmpty()) { if (err) *err = QStringLiteral("cannot read %1").arg(casesJsonPath); return out; }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
    if (!doc.isObject()) {
        if (err) *err = QStringLiteral("%1: %2").arg(casesJsonPath, pe.errorString());
        return out;
    }
    const QJsonObject cases = doc.object().value(QStringLiteral("cases")).toObject();
    for (auto it = cases.constBegin(); it != cases.constEnd(); ++it) {
        TestCase tc;
        tc.name = it.key();
        const QJsonObject o = it.value().toObject();
        for (const QJsonValue& v : o.value(QStringLiteral("pieces")).toArray()) tc.pieces << v.toString();
        for (const QJsonValue& v : o.value(QStringLiteral("anims")).toArray())  tc.anims  << v.toString();
        out.push_back(tc);
    }
    if (out.isEmpty() && err) *err = QStringLiteral("%1: no cases").arg(casesJsonPath);
    return out;
}

AssetBlob CorpusSource::appearance(const QString& name) const
{
    AssetBlob b; b.name = name;
    b.meta    = readAll(m_dir + QStringLiteral("/appearance/") + name + QStringLiteral(".meta.bin"));
    b.payload = readAll(m_dir + QStringLiteral("/appearance/") + name + QStringLiteral(".payload.bin"));
    b.ok = !b.meta.isEmpty() && !b.payload.isEmpty();
    if (!b.ok) b.error = QStringLiteral("missing corpus files for appearance '%1' under %2").arg(name, m_dir);
    return b;
}

AssetBlob CorpusSource::anim(const QString& name) const
{
    AssetBlob b; b.name = name;
    b.meta    = readAll(m_dir + QStringLiteral("/anim/") + name + QStringLiteral(".meta.bin"));
    b.payload = readAll(m_dir + QStringLiteral("/anim/") + name + QStringLiteral(".payload.bin"));
    b.ok = !b.payload.isEmpty();
    if (!b.ok) b.error = QStringLiteral("missing corpus files for anim '%1' under %2").arg(name, m_dir);
    return b;
}

QJsonObject CorpusSource::clothTuning(const QString& clothName) const
{
    // Both shipping conventions: Cloth/<name>.clt.json and Cloth/<name>_sim.clt.json.
    for (const QString& cand : { clothName, clothName + QStringLiteral("_sim") }) {
        const QByteArray raw = readAll(m_dir + QStringLiteral("/cloth/") + cand + QStringLiteral(".clt.json"));
        if (!raw.isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            if (doc.isObject()) return doc.object();
        }
    }
    // Case-insensitive fallback scan.
    QDirIterator it(m_dir + QStringLiteral("/cloth"), { QStringLiteral("*.clt.json") }, QDir::Files);
    const QString wantA = clothName.toLower() + QStringLiteral(".clt.json");
    const QString wantB = clothName.toLower() + QStringLiteral("_sim.clt.json");
    while (it.hasNext()) {
        const QString p = it.next();
        const QString fn = QFileInfo(p).fileName().toLower();
        if (fn == wantA || fn == wantB) {
            const QJsonDocument doc = QJsonDocument::fromJson(readAll(p));
            if (doc.isObject()) return doc.object();
        }
    }
    return {};
}

int runExtract(const QString& cascDir, const QString& d4dataDir,
               const QString& casesJsonPath, const QString& outDir)
{
    QString err;
    const QVector<TestCase> cases = loadCases(casesJsonPath, &err);
    if (cases.isEmpty()) { std::fprintf(stderr, "extract: %s\n", qPrintable(err)); return 2; }

    CascReader casc;
    if (!casc.open(cascDir)) {
        std::fprintf(stderr, "extract: CASC open failed for %s: %s\n",
                     qPrintable(cascDir), qPrintable(casc.lastError()));
        return 2;
    }
    // TACT keys: the d4data snapshot ships a community key file at its root.
    for (const QString& keys : { d4dataDir + QStringLiteral("/d4_tact_keys_clean.txt"), d4dataDir }) {
        if (QFile::exists(keys)) { casc.applyTactKeys(keys); break; }
    }
    std::printf("extract: CASC open, build %s, game %s\n",
                qPrintable(casc.buildId().left(12)),
                qPrintable(CascReader::gameVersion(cascDir)));

    QJsonObject manifest, manCases, manAssets;
    int failures = 0;

    auto pull = [&](const QString& group, const QString& ext, const QString& name,
                    const QString& outSub) -> bool {
        if (manAssets.contains(outSub + QLatin1Char('/') + name)) return true;   // already pulled
        quint64 sno = 0; QString jsonPath;
        const QJsonObject j = d4dataJson(d4dataDir, group, name, ext, &sno, &jsonPath);
        if (j.isEmpty() || sno == 0) {
            std::fprintf(stderr, "extract: FAIL %s/%s — no %s.json / __snoID__ in d4data\n",
                         qPrintable(group), qPrintable(name), qPrintable(ext));
            ++failures; return false;
        }
        const QByteArray meta = casc.readMetaBySno(sno);
        const QByteArray payload = casc.readPayloadBySno(sno);
        if (meta.isEmpty() && payload.isEmpty()) {
            std::fprintf(stderr, "extract: FAIL %s (sno %llu) — CASC returned nothing: %s\n",
                         qPrintable(name), (unsigned long long)sno, qPrintable(casc.lastError()));
            ++failures; return false;
        }
        writeAll(outDir + QLatin1Char('/') + outSub + QLatin1Char('/') + name + QStringLiteral(".meta.bin"), meta);
        writeAll(outDir + QLatin1Char('/') + outSub + QLatin1Char('/') + name + QStringLiteral(".payload.bin"), payload);
        // Keep the snapshot JSON beside the binaries (scalars like anim frameCount live there).
        writeAll(outDir + QLatin1Char('/') + outSub + QLatin1Char('/') + name + QLatin1Char('.') + ext
                     + QStringLiteral(".json"),
                 QJsonDocument(j).toJson(QJsonDocument::Compact));
        QJsonObject a;
        a.insert(QStringLiteral("sno"), double(sno));
        a.insert(QStringLiteral("metaBytes"), meta.size());
        a.insert(QStringLiteral("payloadBytes"), payload.size());
        a.insert(QStringLiteral("metaFnv1a64"), QString::number(fnv1a64(meta), 16));
        a.insert(QStringLiteral("payloadFnv1a64"), QString::number(fnv1a64(payload), 16));
        manAssets.insert(outSub + QLatin1Char('/') + name, a);
        std::printf("extract: %-10s %-40s sno=%-8llu meta=%d payload=%d\n",
                    qPrintable(outSub), qPrintable(name), (unsigned long long)sno,
                    int(meta.size()), int(payload.size()));
        return true;
    };

    // Copy every .clt.json whose name starts with a piece name (both conventions ship;
    // prefix match also captures per-anim overrides, which cost nothing to carry).
    auto pullCloth = [&](const QString& pieceName) {
        const QString folder = d4dataDir + QStringLiteral("/json/base/meta/Cloth");
        QDirIterator it(folder, { QStringLiteral("*.clt.json") }, QDir::Files);
        const QString pfx = pieceName.toLower();
        while (it.hasNext()) {
            const QString p = it.next();
            const QString fn = QFileInfo(p).fileName();
            if (!fn.toLower().startsWith(pfx)) continue;
            writeAll(outDir + QStringLiteral("/cloth/") + fn, readAll(p));
        }
    };

    for (const TestCase& tc : cases) {
        std::printf("extract: case '%s' (%d piece(s), %d anim(s))\n",
                    qPrintable(tc.name), int(tc.pieces.size()), int(tc.anims.size()));
        QJsonObject c;
        QJsonArray pieces, anims;
        for (const QString& p : tc.pieces) {
            pull(QStringLiteral("Appearance"), QStringLiteral("app"), p, QStringLiteral("appearance"));
            pullCloth(p);
            pieces.append(p);
        }
        for (const QString& a : tc.anims) {
            pull(QStringLiteral("Anim"), QStringLiteral("ani"), a, QStringLiteral("anim"));
            anims.append(a);
        }
        c.insert(QStringLiteral("pieces"), pieces);
        c.insert(QStringLiteral("anims"), anims);
        manCases.insert(tc.name, c);
    }

    manifest.insert(QStringLiteral("cases"), manCases);
    manifest.insert(QStringLiteral("assets"), manAssets);
    manifest.insert(QStringLiteral("gameVersion"), CascReader::gameVersion(cascDir));
    manifest.insert(QStringLiteral("cascBuild"), casc.buildId());
    writeAll(outDir + QStringLiteral("/manifest.json"),
             QJsonDocument(manifest).toJson(QJsonDocument::Indented));

    std::printf("extract: done, %d failure(s). Corpus at %s\n", failures, qPrintable(outDir));
    return failures ? 1 : 0;
}

} // namespace d4cloth
