#include "deps/D4DataDownloader.h"

#include "app/AppPaths.h"
#include <QDir>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

namespace {
constexpr auto kRepoUrl = "https://github.com/DiabloTools/d4data.git";
const QStringList kSparse = {"json/base", "json/enUS_Text"};
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
        m_steps.append({"clone", "--depth", "1", "--filter=blob:none", "--sparse",
                        "--progress", QString::fromLatin1(kRepoUrl), dest});
        m_stepLabels.append(QStringLiteral("Downloading metadata"));
        m_steps.append(QStringList{"-C", dest, "sparse-checkout", "set"} + kSparse);
        // Named for what it costs, not for what it does. This step writes ~1 million small files
        // and takes minutes on an SSD, considerably longer on a hard drive — with the old label
        // and no output it read as a hang, which is the single most common "is it broken?" moment
        // in first-run setup.
        m_stepLabels.append(QStringLiteral("Extracting ~1 million files (several minutes)"));
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
