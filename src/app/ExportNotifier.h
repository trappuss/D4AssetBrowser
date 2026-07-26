#pragma once
#include <QObject>
#include <QString>

// App-wide export notifications. Any tab calls ExportNotifier::instance().notify(summary, folder)
// after a successful export; MainWindow listens and shows ONE consistent toast (with a "Show in
// folder" action) — so every tab reports exports the same way instead of a mix of modal dialogs,
// status-bar text and per-tab toasts.
class ExportNotifier : public QObject {
    Q_OBJECT
public:
    static ExportNotifier& instance() { static ExportNotifier n; return n; }
    // `folder` is the directory to reveal (omit for none). `text` is a short human summary.
    void notify(const QString& text, const QString& folder = QString()) { emit exported(text, folder); }

signals:
    void exported(const QString& text, const QString& folder);

private:
    ExportNotifier() = default;
};
