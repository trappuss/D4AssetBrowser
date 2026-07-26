#pragma once
// d4cloth — asset access. Two sources behind one interface:
//   · corpus dir : extracted meta/payload blobs + copied .clt.json / .ani.json
//                  (the portable test fixture the Linux build iterates against)
//   · live CASC  : the game install, via the app's own CascReader (extraction runs here)
//
// Name → SNO resolution uses the d4data JSON snapshot (Appearance/<name>.app.json and
// Anim/<name>.ani.json each carry their own __snoID__), so no CoreTOC parsing is needed.

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace d4cloth {

struct AssetBlob {
    QString    name;      // appearance / anim name
    quint64    sno = 0;
    QByteArray meta;      // base/meta/<sno>
    QByteArray payload;   // base/payload/<sno> (or paylow fallback)
    bool       ok = false;
    QString    error;
};

struct TestCase {
    QString     name;
    QStringList pieces;   // appearance names, merge order (body first)
    QStringList anims;    // anim names
};

// cases.json: { "cases": { "<caseName>": { "pieces": [...], "anims": [...] } } }
QVector<TestCase> loadCases(const QString& casesJsonPath, QString* err);

class CorpusSource {
public:
    explicit CorpusSource(const QString& dir) : m_dir(dir) {}
    AssetBlob appearance(const QString& name) const;   // reads appearance/<name>.{meta,payload}.bin
    AssetBlob anim(const QString& name) const;         // reads anim/<name>.{meta,payload}.bin
    QJsonObject clothTuning(const QString& clothName) const;  // cloth/<match>.clt.json (both naming conventions)
    QString dir() const { return m_dir; }
private:
    QString m_dir;
};

// Extraction (Windows side, one-time): pulls every asset the cases need out of the live
// CASC + d4data snapshot into `outDir`, writes a manifest with SNOs/sizes/hashes.
// Returns 0 on success; prints progress to stdout and failures to stderr.
int runExtract(const QString& cascDir, const QString& d4dataDir,
               const QString& casesJsonPath, const QString& outDir);

} // namespace d4cloth
