#include "deps/D4DataDownloader.h"

#include "app/AppPaths.h"
#include <QDir>
#include <QProcess>
#include "util/ProcQuiet.h"   // git/compact are console tools — suppress their console window
#include <QRegularExpression>
#include <QStandardPaths>

namespace {
constexpr auto kRepoUrl = "https://github.com/DiabloTools/d4data.git";
// The tool reads exactly TWENTY of d4data's 133 asset groups. Fetching all of json/base pulled
// 780,780 files; these twenty are 462,954 — so 317,826 files, ~41% of the extract time and a
// matching slice of the disk cost, were being spent on groups nothing ever opens.
//
// Derived by grepping every "json/base/meta/<Group>" literal in the source, not by judgement.
// If a new reader is added for another group, ADD IT HERE — a missing group is not fatal (the
// binary CASC path covers most of it) but it silently loses whatever the JSON provided.
//
// Cone mode also includes files sitting directly in the listed parents, so json/base/CoreTOC.dat.json
// arrives without being named — it is the only top-level file the code reads.
const QStringList kSparse = {
    "json/enUS_Text",
    "json/base/meta/Actor",         "json/base/meta/Anim",
    "json/base/meta/AnimSet",       "json/base/meta/Appearance",
    "json/base/meta/AppearanceSet", "json/base/meta/Cloth",
    "json/base/meta/Dye",           "json/base/meta/Emote",
    "json/base/meta/EyeColor",      "json/base/meta/FacialHair",
    "json/base/meta/HairColor",     "json/base/meta/Item",
    "json/base/meta/ItemType",      "json/base/meta/Makeup",
    "json/base/meta/MarkingColor",  "json/base/meta/MarkingShape",
    "json/base/meta/Material",      "json/base/meta/PlayerClass",
    "json/base/meta/StoreProduct",  "json/base/meta/Texture",
};
}

D4DataDownloader::D4DataDownloader(QObject* parent) : QObject(parent) {}

QString D4DataDownloader::gitPath()
{
    return QStandardPaths::findExecutable(QStringLiteral("git"));
}

QString D4DataDownloader::defaultDest()
{
    // Portable: download into the beside-exe data/ folder so the whole tool stays self-contained.
    return QDir(AppPaths::dataDir()).filePath(QStringLiteral("d4data"));
}

void D4DataDownloader::start(const QString& dest)
{
    m_dest = dest;
    m_cancelled = false;
    m_steps.clear();
    m_stepLabels.clear();
    m_stepIndex = 0;

    if (gitPath().isEmpty()) {
        emit finished(false, QStringLiteral(
            "git was not found on PATH. Install Git for Windows (https://git-scm.com) "
            "and reopen this dialog."));
        return;
    }

    const bool haveRepo = QDir(dest + QStringLiteral("/.git")).exists();
    if (haveRepo) {
        // Incremental update of an existing sparse checkout.
        m_steps.append({"-C", dest, "fetch", "--depth", "1", "--progress", "origin"});
        m_stepLabels.append(QStringLiteral("Checking for updates"));
        m_steps.append(QStringList{"-C", dest, "sparse-checkout", "set"} + kSparse);
        m_stepLabels.append(QStringLiteral("Selecting folders"));
        m_steps.append({"-C", dest, "reset", "--hard", "FETCH_HEAD"});
        m_stepLabels.append(QStringLiteral("Applying update"));
    } else {
        QDir().mkpath(dest);
        enableNtfsCompression(dest);
        m_steps.append({"clone", "--depth", "1", "--filter=blob:none", "--sparse",
                        "--progress", QString::fromLatin1(kRepoUrl), dest});
        m_stepLabels.append(QStringLiteral("Downloading metadata"));
        m_steps.append(QStringList{"-C", dest, "sparse-checkout", "set"} + kSparse);
        // Named for what it costs, not for what it does. This step writes ~1 million small files
        // and takes minutes on an SSD, considerably longer on a hard drive — with the old label
        // and no output it read as a hang, which is the single most common "is it broken?" moment
        // in first-run setup.
        m_stepLabels.append(QStringLiteral("Extracting ~460k files (several minutes)"));
    }
    runNext();
}

// Parse a git --progress stderr line ("Receiving objects:  45% (1234/2740), 5.20 MiB | 2.10 MiB/s")
// into a phase/percent/detail update. Non-progress lines are ignored (percent stays as-is).
void D4DataDownloader::parseGitProgress(const QString& line)
{
    static const QRegularExpression rx(
        QStringLiteral("^(?:remote:\\s*)?(?<phase>[A-Za-z][A-Za-z ]+?):\\s+(?<pct>\\d+)%\\s*"
                       "\\((?<cur>\\d+)/(?<tot>\\d+)\\)(?:,\\s*(?<detail>.+?))?\\.?\\s*$"));
    const QRegularExpressionMatch m = rx.match(line.trimmed());
    if (!m.hasMatch())
        return;
    QString detail = m.captured(QStringLiteral("detail")).trimmed();
    if (detail.isEmpty())
        detail = QStringLiteral("%1 / %2").arg(m.captured(QStringLiteral("cur")),
                                               m.captured(QStringLiteral("tot")));
    emit phaseProgress(m.captured(QStringLiteral("phase")).trimmed(),
                       m.captured(QStringLiteral("pct")).toInt(), detail);
}

// ── NTFS compression on the destination ─────────────────────────────────────────────────────────
// d4data is ~460,000 small JSON files. Two things make that far more expensive on disk than the
// logical size suggests: JSON is highly compressible (5-10x), and every file rounds up to a 4 KB
// NTFS cluster, so a 700-byte definition still costs 4 KB. Compressed NTFS files are allocated
// sparsely, which recovers most of that slack as well as the content.
//
// Set on the DIRECTORY before the clone, so git writes already-compressed files rather than us
// re-writing 460,000 of them afterwards. Decompression is transparent — nothing else in the tool
// needs to know, and reads of small files are typically FASTER compressed because there is less
// physical I/O.
//
// Best-effort by design: fails harmlessly on FAT32/exFAT/ReFS or a network share, where the
// attribute is unsupported. A failure costs disk space, never correctness, so it is logged and
// ignored rather than surfaced to the user as an error.
void D4DataDownloader::enableNtfsCompression(const QString& dir)
{
#ifdef Q_OS_WIN
    QProcess p;
    quietProcess(p);   // compact.exe is a console tool — no black flash
    // /c compress · /s recurse (so subdirs created later inherit) · /i ignore errors · /q quiet
    p.start(QStringLiteral("compact"),
            {QStringLiteral("/c"), QStringLiteral("/s:") + QDir::toNativeSeparators(dir),
             QStringLiteral("/i"), QStringLiteral("/q")});
    if (!p.waitForFinished(15000)) { p.kill(); return; }
    emit progress(p.exitCode() == 0
                      ? QStringLiteral("> NTFS compression enabled on %1 "
                                       "(JSON compresses ~5-10x; also recovers 4 KB-cluster slack)").arg(dir)
                      : QStringLiteral("> NTFS compression unavailable here — continuing uncompressed"));
#else
    Q_UNUSED(dir);
#endif
}

// ── Extraction progress, measured from the filesystem ───────────────────────────────────────────
// The 133 directories under json/base/meta are the asset GROUPS (Appearance, Texture, Material...),
// and git creates them as it writes. Counting those is one cheap readdir of ~133 entries — counting
// the ~1,000,000 FILES underneath would itself take longer than the UI refresh interval and make
// the freeze worse rather than better.
//
// The count is a floor, not a percentage of work: groups differ enormously in size (Texture ~141k
// files, most groups a few hundred), so it is reported as "N of 133 groups" alongside elapsed time
// rather than converted into a misleading percent.
void D4DataDownloader::startExtractWatch(const QString& dest)
{
    stopExtractWatch();
    m_extractClock.start();
    m_extractTimer = new QTimer(this);
    m_extractTimer->setInterval(1500);
    const QString metaDir = dest + QStringLiteral("/json/base/meta");
    connect(m_extractTimer, &QTimer::timeout, this, [this, metaDir] {
        const int groups = QDir(metaDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
        const qint64 secs = m_extractClock.elapsed() / 1000;
        const QString elapsed = QStringLiteral("%1:%2").arg(secs / 60)
                                    .arg(secs % 60, 2, 10, QLatin1Char('0'));
        // Percent is deliberately capped at 99: the last group finishing is not the step finishing,
        // and a bar that sits at 100% while the app is still busy is worse than one that sits at 99.
        const int pct = groups > 0 ? qMin(99, groups * 100 / 133) : 0;
        emit phaseProgress(QStringLiteral("Extracting"), pct,
                           groups > 0
                               ? QStringLiteral("%1 of ~133 asset groups · %2 elapsed")
                                     .arg(groups).arg(elapsed)
                               : QStringLiteral("writing ~1 million files · %1 elapsed").arg(elapsed));
    });
    m_extractTimer->start();
}

void D4DataDownloader::stopExtractWatch()
{
    if (!m_extractTimer) return;
    m_extractTimer->stop();
    m_extractTimer->deleteLater();
    m_extractTimer = nullptr;
}

void D4DataDownloader::runNext()
{
    if (m_cancelled) {
        emit finished(false, QStringLiteral("Cancelled."));
        return;
    }
    if (m_stepIndex >= m_steps.size()) {
        emit finished(true, QStringLiteral("d4data is ready:\n%1").arg(m_dest));
        return;
    }

    const QStringList args = m_steps[m_stepIndex];
    const QString label = m_stepLabels.value(m_stepIndex, QStringLiteral("Working"));
    emit stepChanged(m_stepIndex + 1, m_steps.size(), label);
    // sparse-checkout is the silent, slow one — watch the filesystem for the whole of it.
    stopExtractWatch();
    if (args.contains(QStringLiteral("sparse-checkout")) || label.startsWith(QStringLiteral("Applying")))
        startExtractWatch(m_dest);
    m_proc = new QProcess(this);
    quietProcess(*m_proc);   // git clone/fetch — progress is streamed into our own console widget
    m_proc->setProgram(gitPath());
    m_proc->setArguments(args);
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);

    connect(m_proc, &QProcess::readyReadStandardError, this, [this] {
        const QString s = QString::fromLocal8Bit(m_proc->readAllStandardError());
        // git rewrites the same progress line with '\r'; split on both so we catch every update.
        static const QRegularExpression sep(QStringLiteral("[\\r\\n]"));
        for (const QString& ln : s.split(sep, Qt::SkipEmptyParts)) {
            const QString t = ln.trimmed();
            if (!t.isEmpty()) { emit progress(t); parseGitProgress(t); }
        }
    });
    connect(m_proc, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus) {
                stopExtractWatch();   // whatever happens, the poller must not outlive the step
                if (m_proc) { m_proc->deleteLater(); m_proc = nullptr; }
                if (m_cancelled) { emit finished(false, QStringLiteral("Cancelled.")); return; }
                if (code != 0) {
                    emit finished(false, QStringLiteral("git step failed (exit %1).").arg(code));
                    return;
                }
                ++m_stepIndex;
                runNext();
            });

    emit progress(QStringLiteral("> git ") + args.join(QLatin1Char(' ')));
    m_proc->start();
}

void D4DataDownloader::cancel()
{
    m_cancelled = true;
    if (m_proc)
        m_proc->kill();
}
