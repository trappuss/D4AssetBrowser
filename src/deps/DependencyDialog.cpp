#include "deps/DependencyDialog.h"

#include "app/Config.h"
#include "casc/CascReader.h"
#include "deps/D4DataDownloader.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

DependencyDialog::DependencyDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Dependencies — d4data"));
    resize(620, 440);

    auto* lay = new QVBoxLayout(this);

    lay->addWidget(new QLabel(QStringLiteral(
        "<b>d4data</b> — community-maintained game metadata (asset names, item/appearance "
        "definitions, materials, textures and translated strings). The Textures, Models, "
        "Wardrobe and String-List tabs read it to name and resolve assets.<br><br>"
        "Only the two folders the app needs are fetched — <code>json/base</code> and "
        "<code>json/enUS_Text</code> — as a shallow, blob-filtered <i>sparse</i> git checkout, "
        "so it's a fraction of the full repo. Update it whenever the game patches."
        "<br><br>"
        "<b>Budget ~12 GB of disk space and 10-20 minutes.</b> The download itself is modest; "
        "what takes the time is writing roughly <b>460,000 small JSON files</b> "
        "(Texture ~141,000, Material ~101,000, Appearance ~67,000). Only the <b>20 asset groups "
        "this tool actually reads</b> are fetched, out of 133 in the repository — that alone "
        "skips 317,000 files. "
        "The <i>Extracting</i> step is therefore limited by your drive's file-creation rate, not "
        "by your connection — it is much slower on a hard drive than an SSD, and antivirus "
        "real-time scanning can double it. It is working even when it looks still; the progress "
        "line below counts asset groups as they appear."), this));

    m_status = new QLabel(this);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setWordWrap(true);
    lay->addWidget(m_status);

    m_btn = new QPushButton(this);
    lay->addWidget(m_btn);

    m_phase = new QLabel(this);
    m_phase->setWordWrap(true);
    m_phase->hide();
    lay->addWidget(m_phase);

    m_bar = new QProgressBar(this);
    m_bar->setRange(0, 0);     // starts indeterminate; switches to % once git reports one
    m_bar->setTextVisible(true);
    m_bar->hide();
    lay->addWidget(m_bar);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    lay->addWidget(m_log, 1);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);

    m_dl = new D4DataDownloader(this);
    connect(m_dl, &D4DataDownloader::progress, this,
            [this](const QString& line) { m_log->appendPlainText(line); });
    connect(m_dl, &D4DataDownloader::stepChanged, this,
            [this](int step, int total, const QString& label) {
        m_step = step; m_stepTotal = total; m_stepLabel = label;
        m_bar->setRange(0, 0);   // indeterminate until this step reports a percentage
        m_phase->setText(QStringLiteral("Step %1 of %2 · %3…").arg(step).arg(total).arg(label));
    });
    connect(m_dl, &D4DataDownloader::phaseProgress, this,
            [this](const QString& phase, int percent, const QString& detail) {
        if (percent >= 0) { m_bar->setRange(0, 100); m_bar->setValue(percent); }
        m_phase->setText(QStringLiteral("Step %1 of %2 · %3 — %4  (%5)")
                             .arg(m_step).arg(m_stepTotal).arg(m_stepLabel, phase, detail));
    });
    connect(m_dl, &D4DataDownloader::finished, this, &DependencyDialog::onFinished);

    connect(m_btn, &QPushButton::clicked, this, &DependencyDialog::onButton);

    refreshStatus();
}

void DependencyDialog::refreshStatus()
{
    const QString dir = Config::d4dataDir();
    const QString core = dir.isEmpty()
        ? QString()
        : QDir(dir).filePath(QStringLiteral("json/base/CoreTOC.dat.json"));
    const bool installed = !core.isEmpty() && QFile::exists(core);

    if (installed) {
        // buildVersion.txt is the build d4data was EXTRACTED FROM — not the installed game's build.
        // Labelling it "game build" read as "you are up to date" while the status bar simultaneously
        // warned "d4data behind game", because only the status bar actually compared the two. Do the
        // same comparison here so there is one answer, using the same last-dotted-component rule.
        QString d4Ver;
        QFile bv(QDir(dir).filePath(QStringLiteral("buildVersion.txt")));
        if (bv.open(QIODevice::ReadOnly)) d4Ver = QString::fromUtf8(bv.readAll()).trimmed().left(24);
        const QString gameVer = CascReader::gameVersion(Config::gameDir());
        auto buildNum = [](const QString& v) -> qlonglong {
            bool ok = false; const qlonglong n = v.section(QLatin1Char('.'), -1).toLongLong(&ok);
            return ok ? n : 0;
        };
        const qlonglong g = buildNum(gameVer), d = buildNum(d4Ver);
        QString detail;
        if (!d4Ver.isEmpty())  detail += QStringLiteral("\nd4data extracted from build %1").arg(d4Ver);
        if (!gameVer.isEmpty()) detail += QStringLiteral("\nGame installed build %1").arg(gameVer);
        if (g > 0 && d > 0)
            detail += (g > d)
                ? QStringLiteral("\n\n⚠ d4data is BEHIND the game — items added since build %1 may be "
                                 "missing names/icons. Update below.").arg(d4Ver)
                : QStringLiteral("\n\n✓ Up to date with the installed game.");
        m_status->setText(QStringLiteral("Installed: %1%2").arg(dir, detail));
        m_btn->setText(QStringLiteral("Update d4data"));
    } else {
        m_status->setText(QStringLiteral(
            "Not installed. It will be downloaded to:\n%1")
            .arg(D4DataDownloader::defaultDest()));
        m_btn->setText(QStringLiteral("Download d4data"));
    }

    if (D4DataDownloader::gitPath().isEmpty()) {
        m_status->setText(m_status->text() +
            QStringLiteral("\n\n⚠ git not found on PATH — install Git for Windows "
                           "(https://git-scm.com) to enable downloading."));
    }
}

void DependencyDialog::onButton()
{
    if (m_running) {        // acting as Cancel
        m_dl->cancel();
        m_log->appendPlainText(QStringLiteral("Cancelling…"));
        return;
    }
    m_running = true;
    m_btn->setText(QStringLiteral("Cancel"));
    m_phase->setText(QStringLiteral("Starting…"));
    m_phase->show();
    m_bar->setRange(0, 0);
    m_bar->show();
    m_log->clear();
    m_dl->start(D4DataDownloader::defaultDest());
}

void DependencyDialog::onFinished(bool ok, const QString& message)
{
    m_running = false;
    m_bar->hide();
    m_phase->setText(ok ? QStringLiteral("✔ Done.") : QStringLiteral("✖ %1").arg(message));
    m_log->appendPlainText(message);

    if (ok) {
        const QString dest = D4DataDownloader::defaultDest();
        Config::setD4dataDir(dest);
        emit d4dataInstalled(dest);
    }
    refreshStatus();
}
