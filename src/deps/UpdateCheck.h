#pragma once
#include <QObject>
#include <QString>

// Notify-only "is an update available?" probe for the two external dependencies. NOTHING is
// downloaded or modified — it only reports, so you're told BEFORE choosing to update:
//   d4data    — a git checkout: compares local HEAD with the remote HEAD (git ls-remote). No fetch.
//   TACT keys — a raw file: conditional HEAD request (curl -I -z <local>): 304 = current, 200 = newer.
// Both probes are async (QProcess); `finished` fires once BOTH have answered. Self-deletes after.
class UpdateCheck : public QObject {
    Q_OBJECT
public:
    enum State { Unknown = 0, UpToDate, UpdateAvailable };

    static QString     tactKeysFile();  // configured path (file OR folder) → the actual key file ("" = none)

    explicit UpdateCheck(QObject* parent = nullptr);
    void start();

signals:
    // States are UpdateCheck::State values (int-typed so queued connections need no metatype).
    // d4dataId / tactKeysId identify the REMOTE version (d4data: commit SHA; keys: ETag or
    // Last-Modified). Callers use them to avoid re-nagging about an update already reported.
    void finished(int d4dataState, int tactKeysState, const QString& detail,
                  const QString& d4dataId, const QString& tactKeysId);

private:
    void checkD4Data();
    void checkTactKeys();
    void tryTactKeysUrl(const QString& curl, const QString& file, int urlIdx);   // candidate fallbacks
    void done(bool isTact, int state, const QString& note, const QString& id = QString());

    int     m_d4 = Unknown, m_tact = Unknown;
    bool    m_d4Done = false, m_tactDone = false;
    QString m_d4Note, m_tactNote;
    QString m_d4Id, m_tactId;
};
