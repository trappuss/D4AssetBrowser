#pragma once
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QStringList>
#include <QVector>

class QProcess;

// Downloads / updates the d4data metadata repo, mirroring the Python original's
// preferred path: a shallow, blob-filtered, *sparse* git checkout of only the two
// folders the app reads — json/base (CoreTOC + Appearance/Material/Texture meta) and
// json/enUS_Text (StringList). Runs git via QProcess and streams its progress.
//
// Requires git on PATH (you already have it — vcpkg needs it). The Python tool also
// had a no-git tarball fallback; that needs a tar/gz extractor and is deferred here.
class D4DataDownloader : public QObject {
    Q_OBJECT
public:
    explicit D4DataDownloader(QObject* parent = nullptr);

    static QString gitPath();      // absolute path to git, or "" if not found
    static QString defaultDest();  // <AppData>/Diablo4AssetBrowser/.../d4data

    void start(const QString& dest);
    void cancel();

signals:
    void progress(const QString& line);
    // A new git step began (1-based `step` of `total`), with a human label like "Downloading".
    void stepChanged(int step, int total, const QString& label);
    // Parsed live progress from git: `phase` e.g. "Receiving objects", `percent` 0..100,
    // `detail` e.g. "5.20 MiB | 2.10 MiB/s". percent < 0 = indeterminate.
    void phaseProgress(const QString& phase, int percent, const QString& detail);
    void finished(bool ok, const QString& message);

private:
    void runNext();
    void parseGitProgress(const QString& line);   // emits phaseProgress from a git stderr line
    // "Extracting folders" is `git sparse-checkout set`, which materialises roughly a MILLION small
    // JSON files (Texture alone is ~141k, Material ~101k, Appearance ~67k, across 133 groups). git
    // prints checkout progress only to a TTY, and QProcess gives it a pipe, so that step emitted
    // NOTHING for several minutes and looked hung. These poll the filesystem instead, which works
    // regardless of what git chooses to print.
    void startExtractWatch(const QString& dest);
    void stopExtractWatch();
    QTimer*  m_extractTimer = nullptr;
    QElapsedTimer m_extractClock;

    QProcess*            m_proc = nullptr;
    QString              m_dest;
    QVector<QStringList> m_steps;
    QStringList          m_stepLabels;
    int                  m_stepIndex = 0;
    bool                 m_cancelled = false;
};
