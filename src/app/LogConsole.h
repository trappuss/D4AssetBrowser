#pragma once
#include <QDialog>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>

class QPlainTextEdit;
class QLineEdit;

// Thread-safe broadcaster of log lines. The global qInstallMessageHandler feeds
// every message here (in addition to the log file); the console window listens.
class LogBuffer : public QObject {
    Q_OBJECT
public:
    static LogBuffer& instance();
    void    append(const QString& line);   // safe to call from any thread
    QString contents() const;              // the buffered backlog (newline-joined)

signals:
    void appended(const QString& line);

private:
    explicit LogBuffer(QObject* parent = nullptr) : QObject(parent) {}
    mutable QMutex m_mtx;
    QStringList    m_lines;
};

// Non-modal console window that mirrors the live log output (File ▸ Toggle console).
class ConsoleWindow : public QDialog {
    Q_OBJECT
public:
    explicit ConsoleWindow(QWidget* parent = nullptr);

private:
    void applyFilter();
    QPlainTextEdit* m_text   = nullptr;
    QLineEdit*      m_filter = nullptr;
};
