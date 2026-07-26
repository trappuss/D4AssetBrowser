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
        m_stepLabels.append(QStringLiteral("Extracting folders"));
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
    emit stepChanged(m_stepIndex + 1, m_steps.size(),
                     m_stepLabels.value(m_stepIndex, QStringLiteral("Working")));
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
