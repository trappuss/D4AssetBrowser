#pragma once
#include <QDialog>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class D4DataDownloader;

// File ▸ Dependencies… — shows d4data status and downloads/updates it with one
// click (git sparse checkout under the hood). Mirrors the Python original's
// Dependencies dialog. Emits d4dataInstalled() so the main window can reload.
class DependencyDialog : public QDialog {
    Q_OBJECT
public:
    explicit DependencyDialog(QWidget* parent = nullptr);

signals:
    void d4dataInstalled(const QString& path);

private:
    void refreshStatus();
    void onButton();
    void onFinished(bool ok, const QString& message);

    QLabel*          m_status = nullptr;
    QLabel*          m_phase  = nullptr;   // live "Step 1/2 · Downloading — 45% (5 MiB | 2 MiB/s)"
    QPlainTextEdit*  m_log    = nullptr;
    QProgressBar*    m_bar    = nullptr;
    QPushButton*     m_btn    = nullptr;
    D4DataDownloader* m_dl    = nullptr;
    bool             m_running = false;
    int              m_step = 0, m_stepTotal = 0;
    QString          m_stepLabel;
};
