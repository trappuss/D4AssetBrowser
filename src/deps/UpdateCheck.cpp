#include "deps/UpdateCheck.h"

#include "deps/D4DataDownloader.h"
#include "app/Config.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include "util/ProcQuiet.h"   // no console-window flash for the startup probes
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>

namespace {
// The upstream repo moved the keys file (root/d4_tact_keys_clean.txt → keys/d4.keys) — which is
// exactly why this is a CANDIDATE LIST now: newest-known location first, older ones as fallbacks,
// so the next reorganisation degrades to a fallback hit instead of a hard "offline?".
// Only the PRIMARY list is probed for updates. The secondary source (CascLib's KeyService.cs,
// fetched alongside it by Settings ▸ Download) is a historical table: measured against a live
// build, none of its 361 Diablo IV keys overlap ours and only one is still usable by the current
// patch. Notifying on every commit to a WoW library would be noise, not news.
constexpr const char* kTactKeysUrls[] = {
    "https://raw.githubusercontent.com/HoldMyBeer-gg/rustydemon/main/keys/d4.keys",
    "https://raw.githubusercontent.com/HoldMyBeer-gg/rustydemon/main/d4_tact_keys_clean.txt",
};
constexpr auto kTactKeysFile = "d4_tact_keys_clean.txt";
}

// (tactKeysUrl accessor removed — kTactKeysUrl is used directly here.)

// The TACT-keys setting may hold either the key FILE or the FOLDER that contains it
// (the downloader points it at the folder), so resolve both.
QString UpdateCheck::tactKeysFile()
{
    const QFileInfo fi(Config::tactKeysPath().trimmed());
    if (fi.isFile()) return fi.absoluteFilePath();
    if (fi.isDir()) {
        const QString f = QDir(fi.absoluteFilePath()).filePath(QLatin1String(kTactKeysFile));
        if (QFileInfo::exists(f)) return f;
    }
    return QString();
}

UpdateCheck::UpdateCheck(QObject* parent) : QObject(parent) {}

void UpdateCheck::start()
{
    m_d4Done = m_tactDone = false;
    m_d4 = m_tact = Unknown;
    checkD4Data();
    checkTactKeys();
}

void UpdateCheck::done(bool isTact, int state, const QString& note, const QString& id)
{
    if (isTact) { m_tact = state; m_tactNote = note; m_tactId = id; m_tactDone = true; }
    else        { m_d4   = state; m_d4Note   = note; m_d4Id   = id; m_d4Done   = true; }
    if (!m_d4Done || !m_tactDone) return;
    const QString detail = QStringLiteral("d4data: %1   ·   TACT keys: %2").arg(m_d4Note, m_tactNote);
    // Record centrally so BOTH the manual and startup paths update it: powers the "Last checked"
    // line and the startup throttle (skip if we probed recently).
    QSettings s;
    s.setValue(QStringLiteral("updates/lastChecked"), QDateTime::currentDateTime());
    s.setValue(QStringLiteral("updates/lastResult"), detail);
    emit finished(m_d4, m_tact, detail, m_d4Id, m_tactId);
    deleteLater();   // one-shot probe
}

// d4data: local HEAD vs remote HEAD. `git ls-remote` talks to the server but fetches no objects.
void UpdateCheck::checkD4Data()
{
    const QString dir = Config::d4dataDir().trimmed();
    const QString git = D4DataDownloader::gitPath();
    if (dir.isEmpty() || !QFileInfo::exists(dir)) { done(false, Unknown, QStringLiteral("not set")); return; }
    if (git.isEmpty())                            { done(false, Unknown, QStringLiteral("git not found")); return; }
    if (!QFileInfo::exists(QDir(dir).filePath(QStringLiteral(".git")))) {
        done(false, Unknown, QStringLiteral("not a git checkout"));   // e.g. a manual/zip copy
        return;
    }
    auto* loc = new QProcess(this);
    quietProcess(*loc);   // background probe — never show a console for it
    loc->setProgram(git);
    loc->setArguments({QStringLiteral("-C"), dir, QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
    connect(loc, &QProcess::finished, this, [this, loc, git, dir](int code, QProcess::ExitStatus) {
        const QString local = QString::fromLatin1(loc->readAllStandardOutput()).trimmed();
        loc->deleteLater();
        if (code != 0 || local.isEmpty()) { done(false, Unknown, QStringLiteral("local HEAD unknown")); return; }
        auto* rem = new QProcess(this);
        quietProcess(*rem);
        rem->setProgram(git);
        rem->setArguments({QStringLiteral("-C"), dir, QStringLiteral("ls-remote"),
                           QStringLiteral("origin"), QStringLiteral("HEAD")});
        connect(rem, &QProcess::finished, this, [this, rem, local](int rc, QProcess::ExitStatus) {
            const QString out = QString::fromLatin1(rem->readAllStandardOutput()).trimmed();
            rem->deleteLater();
            if (rc != 0 || out.isEmpty()) { done(false, Unknown, QStringLiteral("offline?")); return; }
            const QString remote = out.section(QLatin1Char('\t'), 0, 0).trimmed();   // "<sha>\tHEAD"
            if (remote.isEmpty()) { done(false, Unknown, QStringLiteral("no remote HEAD")); return; }
            const bool same = remote.compare(local, Qt::CaseInsensitive) == 0;
            done(false, same ? UpToDate : UpdateAvailable,
                 same ? QStringLiteral("up to date") : QStringLiteral("UPDATE AVAILABLE"),
                 remote);   // remote commit = the version id, for nag-suppression
        });
        rem->start();
    });
    loc->start();
}

// TACT keys: compare CONTENT, not caching metadata.
//
// The obvious approach — HEAD + If-Modified-Since (curl -z) — does NOT work here:
// raw.githubusercontent.com serves ETag but no Last-Modified, so If-Modified-Since has nothing to
// match against and the CDN answers 200 unconditionally. That made this report "UPDATE AVAILABLE"
// forever, no matter how many times you'd actually updated.
//
// So we fetch the keys file (it's small — a plain text key list) and compare its hash with the local
// copy. Exact, stateless, and independent of any cache semantics. Your file is never written to;
// the fetched bytes live in memory and are discarded. The remote hash doubles as the version id for
// startup nag-suppression, which is a true content identity.
void UpdateCheck::checkTactKeys()
{
    const QString curl = QStandardPaths::findExecutable(QStringLiteral("curl"));
    if (curl.isEmpty()) { done(true, Unknown, QStringLiteral("curl not found")); return; }
    const QString file = tactKeysFile();
    if (file.isEmpty()) {
        // Nothing local at all — worth saying once; the sentinel id stops it repeating every launch.
        done(true, UpdateAvailable, QStringLiteral("no local keys file"), QStringLiteral("missing"));
        return;
    }
    // Try each candidate URL in order; only when ALL fail do we report "offline?".
    tryTactKeysUrl(curl, file, 0);
}

void UpdateCheck::tryTactKeysUrl(const QString& curl, const QString& file, int urlIdx)
{
    constexpr int kUrlCount = int(sizeof(kTactKeysUrls) / sizeof(kTactKeysUrls[0]));
    if (urlIdx >= kUrlCount) { done(true, Unknown, QStringLiteral("offline?")); return; }
    auto* p = new QProcess(this);
    quietProcess(*p);
    p->setProgram(curl);
    p->setArguments({QStringLiteral("-s"), QStringLiteral("-L"), QStringLiteral("-f"),
                     QString::fromLatin1(kTactKeysUrls[urlIdx])});   // body → stdout
    connect(p, &QProcess::finished, this, [this, p, curl, file, urlIdx](int code, QProcess::ExitStatus) {
        const QByteArray remote = p->readAllStandardOutput();
        p->deleteLater();
        if (code != 0 || remote.isEmpty()) { tryTactKeysUrl(curl, file, urlIdx + 1); return; }
        QFile lf(file);
        if (!lf.open(QIODevice::ReadOnly)) { done(true, Unknown, QStringLiteral("can't read local file")); return; }
        const QByteArray local = lf.readAll();
        lf.close();
        const QByteArray rh = QCryptographicHash::hash(remote, QCryptographicHash::Sha1);
        const QByteArray lh = QCryptographicHash::hash(local,  QCryptographicHash::Sha1);
        const bool same = (rh == lh);
        done(true, same ? UpToDate : UpdateAvailable,
             same ? QStringLiteral("up to date") : QStringLiteral("UPDATE AVAILABLE"),
             QString::fromLatin1(rh.toHex()));   // remote content hash = the version id
    });
    p->start();
}
