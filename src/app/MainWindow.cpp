#include "app/MainWindow.h"

#include "app/Config.h"
#include "app/Hotkeys.h"
#include "app/LogConsole.h"
#include "app/SettingsDialog.h"
#include "casc/CascReader.h"
#include <QIcon>
#include <QColor>
#include "index/AppearanceMeta.h"
#include "app/ExportNotifier.h"
#include "index/AssetLinks.h"
#include "index/BackTrophyIndex.h"
#include "index/WardrobeAnimIndex.h"
#include "index/ItemHoverIndex.h"
#include "index/DadOverride.h"
#include "index/IconAudit.h"
#include "model/FormatProbe.h"
#include "index/IconIndex.h"
#include "tabs/TexturesTab.h"
#include "tabs/ModelsTab.h"
#include "tabs/StableTab2.h"
#include "tabs/WardrobeTab2.h"
#include "tabs/BulkExtractorTab.h"
#include "deps/UpdateCheck.h"
#include "tabs/BrowserTab.h"
#include "gl/GLModelWidget.h"
#include "app/ExportCapture.h"

#include <QElapsedTimer>
#include <QMouseEvent>
#include <QPainter>
#include <QClipboard>
#include <QGuiApplication>
#include <QDesktopServices>
#include <QSysInfo>
#include <QUrl>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "app/AppPaths.h"
#include <QVBoxLayout>
#include <functional>
#include <thread>

#include <QApplication>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QImageWriter>
#include <QFormLayout>
#include <QKeySequence>
#include <QLabel>
#include <utility>
#include <QAction>
#include <QCloseEvent>
#include <QMenu>
#include <QMenuBar>
#include <QResizeEvent>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QCheckBox>
#include <QMessageBox>
#include <QProgressDialog>
#include <QProcess>
#include <QStandardPaths>
#include <QPushButton>
#include <QShortcut>
#include <QCryptographicHash>
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmapCache>
#include <QStatusBar>
#include <QTimer>
#include "index/SnoIndex.h"
#include "tex/BcDecode.h"
#include "tex/TexMeta.h"
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QTabWidget>

// Defined later in this file (used by finishReload's deferred probe / scaffold step).
static int goldenSampleCheck(CascReader* casc, SnoIndex* index, QString* detail);
static void ensureOverrideScaffold();

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_casc(std::make_unique<CascReader>())
{
    QElapsedTimer ctorT; ctorT.start();   // window-construction timing → the log
    setWindowTitle(QStringLiteral("Diablo4AssetBrowserNative v%1")
                       .arg(QApplication::applicationVersion()));
    resize(1280, 800);

    buildMenu();
    buildTabs();
    qInfo("startup: menu+tabs      %5lld ms  (Textures+Models eager; Wardrobe/Stable/Bulk lazy)",
          ctorT.elapsed());
    buildShortcuts();

    // Restore the last window size / position / maximized-fullscreen state.
    QSettings s;
    const QByteArray geom = s.value(QStringLiteral("window/geometry")).toByteArray();
    const QByteArray state = s.value(QStringLiteral("window/state")).toByteArray();
    if (!geom.isEmpty()) restoreGeometry(geom);
    if (!state.isEmpty()) restoreState(state);

    // ── Crash recovery ───────────────────────────────────────────────────────
    // A wardrobe "_loading" breadcrumb that survived means the previous run crashed while loading
    // that outfit. Log the culprit, clear ONLY that tab's remembered model selection (so it can't
    // crash-loop on restore), and fall through to the safe first tab. The remember feature stays on.
    QString crashCulprit;
    for (const QString& prefix : {QStringLiteral("wardrobe"), QStringLiteral("wardrobe2")}) {
        const QString key = prefix + QStringLiteral("/_loading");
        const QString crumb = s.value(key).toString();
        if (crumb.isEmpty()) continue;
        crashCulprit = crumb;
        qWarning("CRASH RECOVERY: previous run crashed while loading [%s] %s",
                 qPrintable(prefix), qPrintable(crumb));
        for (int i = 0; i < 5; ++i) s.remove(prefix + QStringLiteral("/slot/%1").arg(i));
        for (int i = 0; i < 9; ++i) s.remove(prefix + QStringLiteral("/creator/%1").arg(i));
        for (const QString& k : {QStringLiteral("/weaponType"), QStringLiteral("/weapon"),
                                 QStringLiteral("/weaponType2"), QStringLiteral("/weapon2"),
                                 QStringLiteral("/trophy"), QStringLiteral("/anim")})
            s.remove(prefix + k);
        s.remove(key);
    }
    s.sync();

    // Reopen the last tab — unless we just recovered from a crash, in which case open the safe
    // first tab (Textures) so the wardrobe's auto-restore can't immediately re-crash.
    if (!crashCulprit.isEmpty()) {
        m_tabs->setCurrentIndex(0);
    } else if (s.value(QStringLiteral("view/rememberTab"), false).toBool()) {
        const int t = s.value(QStringLiteral("view/lastTab"), 0).toInt();
        if (t >= 0 && t < m_tabs->count()) m_tabs->setCurrentIndex(t);
    }

    m_status = new QLabel(this);
    // A long status line (an icon-audit summary, a full export path) must never widen the window.
    // A default QLabel reports its full text width as its sizeHint, which propagates into the status
    // bar's and then the WINDOW's minimum width — so one long message could shove the entire layout.
    // `Ignored` means the hint can't drive the layout: the label takes whatever room is there and
    // clips, with the complete text on hover.
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_status->setMinimumWidth(1);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusBar()->addWidget(m_status, 1);
    buildIndexIndicator();

    // Unified export confirmation: any tab emits ExportNotifier → one consistent toast here.
    connect(&ExportNotifier::instance(), &ExportNotifier::exported, this,
            [this](const QString& text, const QString& folder) { showExportToast(text, folder); });

    // Open the game storage: reload() is ASYNC (worker thread does the seconds of CASC/TVFS +
    // CoreTOC work), so the window is up and fully responsive immediately, with an honest
    // "Opening game storage…" status until finishReload lands.
    if (!Config::gameDir().isEmpty())
        reload();
    else
        setStatus(QStringLiteral("Not configured — File ▸ Settings to set your Diablo IV folder."));

    if (!crashCulprit.isEmpty())
        setStatus(QStringLiteral("Recovered from a crash while loading: %1  —  cleared that wardrobe selection.").arg(crashCulprit));

    // Optional startup update check (Settings ▸ Directories ▸ "Check at startup"). Notify-only —
    // nothing is downloaded. Deferred so it never delays the window, and silent unless something
    // is genuinely newer, so it only speaks up when you'd want to know.
    // Throttled: the keys probe fetches a (small) file, so don't re-run it on every launch —
    // once a day is plenty for data that moves this slowly. A manual check resets the window.
    const QDateTime lastChk = s.value(QStringLiteral("updates/lastChecked")).toDateTime();
    const int everyHrs = s.value(QStringLiteral("updates/checkEveryHours"), 24).toInt();
    const bool dueForCheck = !lastChk.isValid()
                             || lastChk.secsTo(QDateTime::currentDateTime()) >= qint64(everyHrs) * 3600;
    if (s.value(QStringLiteral("updates/checkAtStartup"), false).toBool() && dueForCheck) {
        QTimer::singleShot(1500, this, [this] {
            auto* uc = new UpdateCheck(this);
            connect(uc, &UpdateCheck::finished, this,
                    [this](int d4, int tact, const QString& detail,
                           const QString& d4Id, const QString& tactId) {
                // Only speak up about a version we haven't already reported. Once you've been told
                // about a given d4data commit / keys revision, stay quiet until it actually changes
                // (or you update, after which the check simply reports "up to date"). Without this
                // the dialog would fire every launch until you gave in — and you'd just switch it off.
                QSettings st;
                static const auto kD4Key   = QStringLiteral("updates/notifiedD4Id");
                static const auto kTactKey = QStringLiteral("updates/notifiedTactId");
                QStringList what;
                const bool d4New   = d4   == UpdateCheck::UpdateAvailable && !d4Id.isEmpty()
                                     && st.value(kD4Key).toString() != d4Id;
                const bool tactNew = tact == UpdateCheck::UpdateAvailable && !tactId.isEmpty()
                                     && st.value(kTactKey).toString() != tactId;
                if (d4New)   what << QStringLiteral("d4data");
                if (tactNew) what << QStringLiteral("TACT keys");
                if (what.isEmpty()) return;   // up to date, undeterminable, or already reported → quiet
                if (d4New)   st.setValue(kD4Key, d4Id);       // remember what we've now told them about
                if (tactNew) st.setValue(kTactKey, tactId);
                const QString who = what.join(QStringLiteral(" and "));
                setStatus(QStringLiteral("Update available: %1  —  File ▸ Settings ▸ Directories to download.").arg(who));
                QMessageBox mb(QMessageBox::Information, QStringLiteral("Update available"),
                    QStringLiteral("A newer version is available for: %1.\n\n%2\n\n"
                                   "(You won't be reminded about this version again.)").arg(who, detail),
                    QMessageBox::Close, this);
                // One click straight to the Download buttons, rather than "go find Settings yourself".
                QPushButton* now = mb.addButton(QStringLiteral("Update now"), QMessageBox::AcceptRole);
                mb.exec();
                if (mb.clickedButton() == now) openSettings();
            });
            uc->start();
        });
    }
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* e)
{
    // Don't tear down while the reload worker still holds CascReader/SnoIndex — destroying them
    // under it is a use-after-free. Rare (only a close within the first seconds); pump the loop
    // until finishReload lands, bounded so a wedged open can't trap the user.
    if (m_reloading) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        QElapsedTimer w; w.start();
        while (m_reloading && w.elapsed() < 20000)
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
        QApplication::restoreOverrideCursor();
    }
    QSettings s;
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
    s.setValue(QStringLiteral("window/state"), saveState());
    s.setValue(QStringLiteral("view/lastTab"), m_tabs->currentIndex());
    // Flush each tab's camera/FOV now: hideEvent only fires on tab-switch, not when the window is
    // closed with a tab still visible, so without this the current tab's view would be lost.
    if (m_tabs)
        for (int i = 0; i < m_tabs->count(); ++i)
            static_cast<BrowserTab*>(m_tabs->widget(i))->persistView();
    QMainWindow::closeEvent(e);
}

void MainWindow::toggleConsole()
{
    if (!m_console)
        m_console = new ConsoleWindow(this);
    if (m_console->isVisible()) {
        m_console->hide();
    } else {
        m_console->show();
        m_console->raise();
        m_console->activateWindow();
    }
}

void MainWindow::buildMenu()
{
    QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(QStringLiteral("&Settings…"), QKeySequence(QStringLiteral("Ctrl+,")),
                    this, &MainWindow::openSettings);
    file->addAction(QStringLiteral("&Reload"), QKeySequence(Qt::Key_F5),
                    this, &MainWindow::reload);
    // (Models-tab preferences now live in File > Settings > General > "Models tab".)
    // Hidden-ish diagnostic: cross-check every resolved icon against the diablo4.dad
    // DB and write icon_audit.txt next to the exe (regression check after updates).
    file->addAction(QStringLiteral("Icon &audit (write report)"), this, &MainWindow::runIconAudit);
    file->addAction(QStringLiteral("Toggle &console window"),
                    QKeySequence(QStringLiteral("Ctrl+`")), this, &MainWindow::toggleConsole);
    file->addSeparator();
    // (Export/Import settings profile moved to Settings ▸ "Settings profile".)
    file->addAction(QStringLiteral("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    // ── Export menu: quick access to per-tab export + preview capture + settings ──
    QMenu* exp = menuBar()->addMenu(QStringLiteral("E&xport"));
    m_actExportSel = exp->addAction(QStringLiteral("Export selection…"),
                                    this, &MainWindow::exportActiveSelection);
    m_actExportLast = exp->addAction(QStringLiteral("Export selection last dir"),
                                     this, &MainWindow::exportActiveSelectionToLast);
    m_actExportAnims = exp->addAction(QStringLiteral("Export animations only (.glb)…"),
                                      this, &MainWindow::exportActiveAnimations);
    m_actExportFramesSel = exp->addAction(QStringLiteral("Export selected TexFrame/s…"),
                                          this, &MainWindow::exportActiveFramesSelected);
    m_actExportFramesSelLast = exp->addAction(QStringLiteral("Export selected TexFrame/s last dir"),
                                              this, &MainWindow::exportActiveFramesSelectedLast);
    m_actExportFramesAll = exp->addAction(QStringLiteral("Export all TexFrames…"),
                                          this, &MainWindow::exportActiveFramesAll);
    m_actExportFramesAllLast = exp->addAction(QStringLiteral("Export all TexFrames last dir"),
                                              this, &MainWindow::exportActiveFramesAllLast);
    exp->addSeparator();
    m_actSaveImg = exp->addAction(QStringLiteral("Save preview &image…"),
                                  this, &MainWindow::captureActiveImage);
    m_actTurntable = exp->addAction(QStringLiteral("&Turntable GIF…"),
                                    this, &MainWindow::captureActiveTurntable);
    m_actAnimLoop = exp->addAction(QStringLiteral("&Animation loop GIF…"),
                                   this, &MainWindow::captureActiveAnimLoop);
    exp->addSeparator();
    exp->addAction(QStringLiteral("Export &settings…"), this, &MainWindow::openExportSettings);
    connect(exp, &QMenu::aboutToShow, this, &MainWindow::updateExportMenu);
    applyHotkeys();   // assign user-configured shortcuts to the export/preview actions

    QMenu* help = menuBar()->addMenu(QStringLiteral("&Help"));
    // Shortcut cheat-sheet — the viewport keys are invisible unless something teaches them.
    auto* shortcuts = help->addAction(QStringLiteral("&Shortcuts…"), this,
                                      [this] { showShortcutSheet(); });
    shortcuts->setShortcuts({QKeySequence(Qt::Key_F1), QKeySequence(QStringLiteral("Shift+/"))});
    help->addSeparator();
    // Post-patch triage: one screen that verifies storage, keys, d4data freshness, indexes and
    // live format probes — "what broke?" answered in seconds after a game update.
    help->addAction(QStringLiteral("&Health check…"), this, [this] { showHealthCheck(); });
    // Same buffer as Export log, straight to the clipboard — pasting a log into a bug report or a
    // chat is the common case, and writing a file first only to attach it is pure friction.
    help->addAction(QStringLiteral("&Copy log to clipboard"), this, [this] {
        const QString text = LogBuffer::instance().contents();
        QGuiApplication::clipboard()->setText(text);
        setStatus(QStringLiteral("Log copied to clipboard — %1 lines, %2 KB")
                      .arg(text.count(QLatin1Char('\n')) + 1)
                      .arg((text.toUtf8().size() + 512) / 1024));
    });
    help->addAction(QStringLiteral("&Export log…"), this, [this] {
        const QString suggested = QStringLiteral("d4browser_log_%1.txt")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export log"),
                                                          suggested, QStringLiteral("Text (*.txt)"));
        if (path.isEmpty()) return;
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            f.write(LogBuffer::instance().contents().toUtf8());
            setStatus(QStringLiteral("Log exported — attach it to a bug report: %1").arg(path));
        }
    });
    help->addSeparator();
    help->addAction(QStringLiteral("Open &data folder"), this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(AppPaths::dataDir()));
    });
    help->addAction(QStringLiteral("Open &log folder"), this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QCoreApplication::applicationDirPath()));
    });
    help->addAction(QStringLiteral("&Copy diagnostic info"), this, [this] {
        const QString diag = QStringLiteral(
            "Diablo4AssetBrowserNative v%1 (built %2)\nQt %3 · %4\nGPU: %5\n"
            "CASC: %6 · build %7\nGame: %8\nd4data: %9\nIndex: %10 assets / %11 groups")
            .arg(QApplication::applicationVersion(), QStringLiteral(__DATE__),
                 QString::fromLatin1(qVersion()), QSysInfo::prettyProductName(),
                 GLModelWidget::glInfo().isEmpty() ? QStringLiteral("(viewport not initialized yet)")
                                                   : GLModelWidget::glInfo(),
                 m_casc && m_casc->isReady() ? QStringLiteral("open") : QStringLiteral("closed"),
                 m_casc ? m_casc->buildId().left(18) : QString(),
                 Config::gameDir(), Config::d4dataDir())
            .arg(m_index.totalCount()).arg(m_index.groups().size());
        QApplication::clipboard()->setText(diag);
        setStatus(QStringLiteral("Diagnostic info copied — paste it into your bug report."));
    });
    help->addSeparator();
    help->addAction(QStringLiteral("&About"), this, [this] {
        QDialog dlg(this);
        dlg.setWindowTitle(QStringLiteral("About"));
        auto* lay = new QFormLayout(&dlg);
        lay->addRow(new QLabel(
            QStringLiteral("<b>Diablo4AssetBrowserNative</b> v%1 &nbsp;<span style='color:#888'>"
                           "(built %2 · Qt %3)</span><br><br>"
                           "Native C++/Qt6 Diablo IV asset browser:<br>"
                           "Qt 6 Widgets · OpenGL 4.5 · native CASC (TVFS) · fastgltf · tinygltf.<br><br>"
                           "<span style='color:#888'>Portable: settings, caches and d4data live in "
                           "the <i>data</i> folder beside the .exe — no registry, no AppData.<br>"
                           "Not affiliated with or endorsed by Blizzard. For personal use with a "
                           "copy of Diablo IV you own.</span>")
                .arg(QApplication::applicationVersion(), QStringLiteral(__DATE__),
                     QString::fromLatin1(qVersion())), &dlg));
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
        QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        lay->addRow(bb);
        dlg.exec();
    });
}

// ── Shortcut cheat-sheet (Help ▸ Shortcuts, F1 or ?) ─────────────────────────
void MainWindow::showShortcutSheet()
{
    if (m_shortcutSheet) {   // toggle: F1 again (or Esc) closes it
        m_shortcutSheet->close();
        return;
    }
    auto* dlg = new QDialog(this);
    m_shortcutSheet = dlg;
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QObject::destroyed, this, [this] { m_shortcutSheet = nullptr; });
    dlg->setWindowTitle(QStringLiteral("Keyboard & mouse shortcuts"));
    auto* lay = new QVBoxLayout(dlg);
    auto* label = new QLabel(dlg);
    label->setTextFormat(Qt::RichText);
    label->setText(QStringLiteral(
        "<style>td{padding:1px 10px 1px 0;} .k{color:#d8a23a;white-space:nowrap;}"
        " .h{color:#dedede;font-weight:bold;} .s{color:#888;}</style>"
        "<table>"
        "<tr><td class='h' colspan='2'>Everywhere</td></tr>"
        "<tr><td class='k'>Ctrl+1…5</td><td>Switch tab</td></tr>"
        "<tr><td class='k'>Ctrl+K</td><td>Jump to any model / texture by name or SNO</td></tr>"
        "<tr><td class='k'>Alt+Left / Alt+Right</td><td>Back / forward through jumps</td></tr>"
        "<tr><td class='k'>F1 &nbsp;/&nbsp; ?</td><td>This sheet</td></tr>"
        "<tr><td class='s' colspan='2'>Export/capture hotkeys are rebindable in Settings ▸ Hotkeys.</td></tr>"
        "<tr><td colspan='2'>&nbsp;</td></tr>"
        "<tr><td class='h' colspan='2'>3D viewport (Models · Wardrobe)</td></tr>"
        "<tr><td class='k'>Drag / right-drag / wheel</td><td>Orbit · pan · zoom</td></tr>"
        "<tr><td class='k'>Middle-click</td><td>Re-frame the model</td></tr>"
        "<tr><td class='k'>Double-click</td><td>Select part (same part / empty space deselects; "
        "camera snap is a Camera-panel option)</td></tr>"
        "<tr><td class='k'>Esc</td><td>Deselect part · exit fullscreen</td></tr>"
        "<tr><td class='k'>F</td><td>Fullscreen (maximize in place)</td></tr>"
        "<tr><td class='k'>H · Shift+H · Alt+H</td><td>Hide selected · solo selected · show all</td></tr>"
        "<tr><td class='k'>Gizmo click / double-click</td><td>Snap to axis view · toggle orthographic</td></tr>"
        "<tr><td class='k'>Wheel on shading ⌄</td><td>Cycle the view channel</td></tr>"
        "<tr><td colspan='2'>&nbsp;</td></tr>"
        "<tr><td class='h' colspan='2'>Timeline (Models)</td></tr>"
        "<tr><td class='k'>Wheel on slider</td><td>Step one frame</td></tr>"
        "<tr><td class='k'>Shift+wheel</td><td>Playback speed</td></tr>"
        "<tr><td colspan='2'>&nbsp;</td></tr>"
        "<tr><td class='h' colspan='2'>Models search</td></tr>"
        "<tr><td class='k'>c:name</td><td>Collection filter</td></tr>"
        "<tr><td class='k'>#tag</td><td>Tag filter (funnel button = full list)</td></tr>"
        "<tr><td class='k'>digits</td><td>SNO lookup</td></tr>"
        "</table>"));
    lay->addWidget(label);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    connect(bb, &QDialogButtonBox::clicked, dlg, &QDialog::close);
    lay->addWidget(bb);
    dlg->show();   // modeless: keep it open beside the app while learning
}

// Assign the user-configurable shortcuts (Settings ▸ Hotkeys) to the export/preview
// actions. Called once after the menu is built and again whenever settings change, so
// rebindings take effect without a restart.
void MainWindow::applyHotkeys()
{
    QAction* acts[] = { m_actExportSel, m_actExportLast, m_actExportAnims,
                        m_actSaveImg,   m_actTurntable,  m_actAnimLoop };
    constexpr int kActs = int(sizeof(acts) / sizeof(acts[0]));
    const QVector<Hotkeys::Def> d = Hotkeys::defs();
    for (int i = 0; i < d.size() && i < kActs; ++i)
        if (acts[i]) acts[i]->setShortcut(Hotkeys::seq(d[i].key, d[i].def));
}

// ── Lazy tab host ────────────────────────────────────────────────────────────
// Constructing every tab up front made startup pay for ALL of them — the Wardrobe alone builds
// thousands of widgets nobody sees until it's clicked. This host is a BrowserTab (so every
// static_cast<BrowserTab*> page walk in this file stays valid) that builds its real tab on
// FIRST refresh() — which is exactly when the tab first becomes visible (onTabChanged) or the
// storage loads while it's current. Until then every virtual is a cheap no-op/default.
namespace {
class LazyTab : public BrowserTab {
public:
    // The factory takes the PARENT and must construct the tab as its child. Building the tab
    // parentless made it a top-level window for its whole (heavy) constructor — on Windows the
    // GL viewport/popup panels force native-handle creation mid-ctor, which flashed a phantom
    // window on the first click. Parented from birth, there is nothing to flash.
    explicit LazyTab(std::function<BrowserTab*(QWidget*)> make) : m_make(std::move(make))
    {
        auto* l = new QVBoxLayout(this);
        l->setContentsMargins(0, 0, 0, 0);
    }
    BrowserTab* ensure()
    {
        if (!m_child) {
            m_child = m_make(this);
            m_child->setReader(m_reader);   // hand down what setReader/setIndex stored on us
            m_child->setIndex(m_index);
            layout()->addWidget(m_child);
        }
        return m_child;
    }
    // Creation triggers: becoming visible (refresh) — everything else forwards only if built.
    void refresh() override { ensure()->refresh(); }
    void reset() override { if (m_child) m_child->reset(); }
    void onSettingsChanged() override { if (m_child) m_child->onSettingsChanged(); }   // an unbuilt
    void onSettingsLiveChanged(bool r) override { if (m_child) m_child->onSettingsLiveChanged(r); }
    void persistView() override { if (m_child) m_child->persistView(); }   // …tab reads QSettings at build
    GLModelWidget* previewWidget() override { return m_child ? m_child->previewWidget() : nullptr; }
    bool hasExportSelection() const override { return m_child && m_child->hasExportSelection(); }
    void exportSelection() override { if (m_child) m_child->exportSelection(); }
    void exportSelectionToLast() override { if (m_child) m_child->exportSelectionToLast(); }
    QString exportNoun() const override { return m_child ? m_child->exportNoun() : BrowserTab::exportNoun(); }
    bool    hasAnimExport() const override { return m_child && m_child->hasAnimExport(); }
    QString animExportLabel() const override { return m_child ? m_child->animExportLabel() : BrowserTab::animExportLabel(); }
    void    exportAnimations() override { if (m_child) m_child->exportAnimations(); }
    bool    hasFrameExport() const override { return m_child && m_child->hasFrameExport(); }
    void    exportFramesSelected() override { if (m_child) m_child->exportFramesSelected(); }
    void    exportFramesSelectedToLast() override { if (m_child) m_child->exportFramesSelectedToLast(); }
    void    exportFramesAll() override { if (m_child) m_child->exportFramesAll(); }
    void    exportFramesAllToLast() override { if (m_child) m_child->exportFramesAllToLast(); }
private:
    std::function<BrowserTab*(QWidget*)> m_make;
    BrowserTab* m_child = nullptr;
};
}   // namespace

void MainWindow::buildTabs()
{
    m_tabs = new QTabWidget(this);
    m_tabs->setTabPosition(QTabWidget::North);

    auto add = [this](BrowserTab* tab, const QString& label) {
        tab->setReader(m_casc.get());
        tab->setIndex(&m_index);
        m_tabs->addTab(tab, label);
    };
    // Textures + Models are EAGER: one of them is (almost always) the visible startup tab, and
    // the cross-tab wiring below wants their concrete pointers. Everything after is LAZY —
    // built the first time its tab is opened.
    auto* textures = new TexturesTab; add(textures, QStringLiteral("Textures"));
    auto* models   = new ModelsTab;   add(models,   QStringLiteral("Models"));
    // Route the Models tab's background-scan progress into the shared floating toast.
    connect(models, &ModelsTab::scanStatus, this,
            [this](const QString& s) { m_idxTabMsg = s; updateToast(); });
    add(new LazyTab([](QWidget* p) { return new WardrobeTab2(p); }), QStringLiteral("Wardrobe"));
    add(new LazyTab([](QWidget* p) { return new StableTab2(p); }), QStringLiteral("Stable"));
    add(new LazyTab([models, textures](QWidget* p) { return new BulkExtractorTab(models, textures, p); }),
        QStringLiteral("Bulk Extract"));   // reuses Models + Textures export pipelines

    // Double-clicking an .app in the Textures tab's Associated Models jumps to the
    // Models tab and loads that model.
    connect(textures, &TexturesTab::revealModelRequested, this,
            [this](int sno) { jumpTo(9, sno); });   // records nav history (Alt+Left returns here)
    // Textures "Options…" now opens the shared Export settings dialog (single source of truth).
    connect(textures, &TexturesTab::exportSettingsRequested, this, &MainWindow::openExportSettings);

    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    setCentralWidget(m_tabs);
}

void MainWindow::buildShortcuts()
{
    // Ctrl+1 … Ctrl+9 jump straight to a module (d4analyzer parity).
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto* sc = new QShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(i + 1)), this);
        connect(sc, &QShortcut::activated, this, [this, i] { m_tabs->setCurrentIndex(i); });
    }
    // Ctrl+K — global jump: type a name or SNO, land on the asset in its owning tab.
    connect(new QShortcut(QKeySequence(QStringLiteral("Ctrl+K")), this),
            &QShortcut::activated, this, [this] { showJumpPalette(); });
    // Alt+Left / Alt+Right — walk the cross-tab jump history.
    connect(new QShortcut(QKeySequence(QStringLiteral("Alt+Left")), this),
            &QShortcut::activated, this, [this] { navGo(true); });
    connect(new QShortcut(QKeySequence(QStringLiteral("Alt+Right")), this),
            &QShortcut::activated, this, [this] { navGo(false); });
}

void MainWindow::onTabChanged(int index)
{
    // Lazy: refresh a tab the first time it becomes visible.
    auto* w = m_tabs->widget(index);
    if (!w || m_refreshed.contains(w))
        return;
    m_refreshed.insert(w);
    // Every page in m_tabs is a BrowserTab (added only via the add() lambda).
    static_cast<BrowserTab*>(w)->refresh();
}

// ── Export menu ──────────────────────────────────────────────────────────────
BrowserTab* MainWindow::activeTab() const
{
    QWidget* w = m_tabs ? m_tabs->currentWidget() : nullptr;
    return w ? static_cast<BrowserTab*>(w) : nullptr;   // every page is a BrowserTab
}

// Enable each action only where it applies to the current tab (called on menu open).
void MainWindow::updateExportMenu()
{
    BrowserTab* t = activeTab();
    const bool canSel  = t && t->hasExportSelection();
    GLModelWidget* pv  = t ? t->previewWidget() : nullptr;
    const bool hasView = pv != nullptr;
    const bool hasAnim = hasView && pv->animFrameCount() > 0;
    const QString noun = t ? t->exportNoun() : QStringLiteral("selection");
    if (m_actExportSel)  { m_actExportSel->setText(QStringLiteral("Export %1…").arg(noun));
                           m_actExportSel->setEnabled(canSel); }
    if (m_actExportLast) { m_actExportLast->setText(QStringLiteral("Export %1 last dir").arg(noun));
                           m_actExportLast->setEnabled(canSel); }
    if (m_actSaveImg)    m_actSaveImg->setEnabled(hasView);
    if (m_actTurntable)  m_actTurntable->setEnabled(hasView);
    if (m_actAnimLoop)   m_actAnimLoop->setEnabled(hasAnim);
    if (m_actExportAnims) {   // rig-only anim export — shown only for tabs that support it (Models)
        const bool canAnim = t && t->hasAnimExport();
        m_actExportAnims->setVisible(canAnim);
        m_actExportAnims->setEnabled(canAnim);
        if (canAnim) m_actExportAnims->setText(t->animExportLabel());
    }
    // TexFrame export — shown only for tabs that support it (Textures), and only when the selected
    // texture actually has frames.
    const bool canFrames = t && t->hasFrameExport();
    for (QAction* a : {m_actExportFramesSel, m_actExportFramesSelLast, m_actExportFramesAll, m_actExportFramesAllLast})
        if (a) { a->setVisible(canFrames); a->setEnabled(canFrames); }
}

void MainWindow::exportActiveAnimations()
{
    if (BrowserTab* t = activeTab()) { if (t->hasAnimExport()) t->exportAnimations(); }
}

void MainWindow::exportActiveFramesSelected()
{
    if (BrowserTab* t = activeTab()) { if (t->hasFrameExport()) t->exportFramesSelected(); }
}

void MainWindow::exportActiveFramesSelectedLast()
{
    if (BrowserTab* t = activeTab()) { if (t->hasFrameExport()) t->exportFramesSelectedToLast(); }
}

void MainWindow::exportActiveFramesAll()
{
    if (BrowserTab* t = activeTab()) { if (t->hasFrameExport()) t->exportFramesAll(); }
}

void MainWindow::exportActiveFramesAllLast()
{
    if (BrowserTab* t = activeTab()) { if (t->hasFrameExport()) t->exportFramesAllToLast(); }
}

void MainWindow::exportActiveSelection()
{
    if (BrowserTab* t = activeTab()) { if (t->hasExportSelection()) t->exportSelection(); }
}

void MainWindow::exportActiveSelectionToLast()
{
    if (BrowserTab* t = activeTab()) { if (t->hasExportSelection()) t->exportSelectionToLast(); }
}

// Shared save-path helper for the capture actions. Starts in (and remembers) the last folder a
// capture was saved to — Pictures is only the first-run default.
static QString captureSavePath(QWidget* parent, const QString& stem, const QString& ext,
                               const QString& title, const QString& filter)
{
    QSettings s;
    QString dir = s.value(QStringLiteral("export/captureDir")).toString();
    if (dir.isEmpty() || !QDir(dir).exists())
        dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString ts  = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString suggest = QDir(dir).filePath(QStringLiteral("%1_%2.%3").arg(stem, ts, ext));
    const QString path = QFileDialog::getSaveFileName(parent, title, suggest, filter);
    if (!path.isEmpty())
        s.setValue(QStringLiteral("export/captureDir"), QFileInfo(path).absolutePath());
    return path;
}

void MainWindow::captureActiveImage()
{
    BrowserTab* t = activeTab();
    GLModelWidget* pv = t ? t->previewWidget() : nullptr;
    if (!pv) return;
    // Default to the configured container, but keep every option in the filter so the dialog can
    // still override it per-save.
    const QString fmt = ExportCapture::imageFormat();
    const QString pngF  = QStringLiteral("PNG image (*.png)");
    const QString jpgF  = QStringLiteral("JPEG (*.jpg)");
    const bool haveWebp = QImageWriter::supportedImageFormats().contains(QByteArrayLiteral("webp"));
    const QString webpF = haveWebp ? QStringLiteral(";;WebP (*.webp)") : QString();
    const QString filter = fmt == QLatin1String("jpg")  ? jpgF + QStringLiteral(";;") + pngF + webpF
                         : fmt == QLatin1String("webp") ? QStringLiteral("WebP (*.webp);;") + pngF
                                                          + QStringLiteral(";;") + jpgF
                                                        : pngF + QStringLiteral(";;") + jpgF + webpF;
    const QString path = captureSavePath(this, QStringLiteral("preview"), fmt,
                                         QStringLiteral("Save preview image"), filter);
    if (path.isEmpty()) return;
    setStatus(ExportCapture::saveImage(pv, path) ? QStringLiteral("Saved %1").arg(path)
                                                 : QStringLiteral("Could not save %1").arg(path));
}

// Cloth physics + GIF export don't get along: the sim is stepped once per exported frame, and the
// result reads as jitter in the finished GIF no matter how the stepping is tuned. Rather than let
// people discover that after a long export, say so up front and offer the one-click way out.
// Returns false = user cancelled. `disableForExport` = export this one with cloth switched off.
// KISS on purpose: no per-tab setting paths in the text, because all three tabs (Models, Wardrobe,
// Stable) expose the same "Physics" panel with the same master switch.
// RAII: switch cloth off for one export and restore it afterwards, on EVERY exit path (success,
// failure, cancel, early return). Passing nullptr is a no-op, so call sites stay branch-free.
struct ClothOff {
    GLModelWidget* v = nullptr;
    bool prev = false;
    explicit ClothOff(GLModelWidget* w) : v(w) {
        if (!v) return;
        prev = v->clothEnabled();
        v->setClothEnabled(false);
    }
    ~ClothOff() { if (v) v->setClothEnabled(prev); }
    ClothOff(const ClothOff&) = delete;
    ClothOff& operator=(const ClothOff&) = delete;
};

static bool confirmGifPhysics(QWidget* parent, GLModelWidget* pv, bool& disableForExport)
{
    disableForExport = false;
    if (!pv || !pv->clothEnabled()) return true;
    if (!QSettings().value(QStringLiteral("export/warnGifPhysics"), true).toBool()) return true;

    QMessageBox box(parent);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Cloth physics is on"));
    box.setText(QStringLiteral("<b>Cloth physics usually looks jittery in an exported GIF.</b>"));
    box.setInformativeText(QStringLiteral(
        "The simulation advances one step per exported frame, which does not reproduce the smooth, "
        "continuously-settled motion you see in the live preview.<br><br>"
        "<b>Recommended:</b> export with physics off — the garment is drawn in its skinned pose and "
        "the animation stays clean.<br><br>"
        "To turn it off yourself for all exports, open the <b>Physics</b> panel in this tab and "
        "uncheck <b>Enable cloth simulation</b>."));
    QPushButton* without = box.addButton(QStringLiteral("Export without physics"), QMessageBox::AcceptRole);
    QPushButton* anyway  = box.addButton(QStringLiteral("Export anyway"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(without);

    QCheckBox* mute = new QCheckBox(QStringLiteral("Don't warn me again"), &box);
    box.setCheckBox(mute);
    box.exec();

    if (mute->isChecked()) QSettings().setValue(QStringLiteral("export/warnGifPhysics"), false);
    if (box.clickedButton() == without) { disableForExport = true; return true; }
    if (box.clickedButton() == anyway)  return true;
    return false;   // Cancel (or dialog closed)
}

// Run a multi-frame GIF capture behind the tool's standard progress dialog (linear bar + Cancel) —
// same UI as the Models tab's batch export. grabFramebuffer() renders the GL widget's own buffer, so
// an overlapping dialog never appears in the recorded frames.
static bool runGifWithProgress(GLModelWidget* pv,
                               const std::function<bool(const ExportCapture::ProgressFn&)>& run,
                               bool& canceled)
{
    QProgressDialog dlg(QStringLiteral("Recording GIF…"), QStringLiteral("Cancel"), 0, 100,
                        pv ? pv->window() : nullptr);
    dlg.setWindowTitle(QStringLiteral("Exporting GIF"));
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setMinimumDuration(0);
    dlg.setAutoClose(false);
    dlg.setAutoReset(false);
    dlg.setValue(0);
    const bool ok = run([&dlg](int done, int total) {
        dlg.setMaximum(qMax(1, total));
        dlg.setValue(done);
        QApplication::processEvents();
        return !dlg.wasCanceled();
    });
    canceled = dlg.wasCanceled();
    return ok;
}

void MainWindow::captureActiveTurntable()
{
    BrowserTab* t = activeTab();
    GLModelWidget* pv = t ? t->previewWidget() : nullptr;
    if (!pv) return;
    bool noPhys = false;
    if (!confirmGifPhysics(this, pv, noPhys)) return;   // ask BEFORE the save dialog
    const QString path = captureSavePath(this, QStringLiteral("turntable"), QStringLiteral("gif"),
                                         QStringLiteral("Save turntable GIF"),
                                         QStringLiteral("Animated GIF (*.gif)"));
    if (path.isEmpty()) return;
    ClothOff clothOff(noPhys ? pv : nullptr);   // restores on every exit path
    bool canceled = false;
    const bool ok = runGifWithProgress(pv,
        [pv, &path](const ExportCapture::ProgressFn& p) { return ExportCapture::turntableGif(pv, path, p); },
        canceled);
    setStatus(canceled ? QStringLiteral("Turntable export canceled.")
                       : ok ? QStringLiteral("Saved %1").arg(path)
                            : QStringLiteral("Turntable export failed."));
}

void MainWindow::captureActiveAnimLoop()
{
    BrowserTab* t = activeTab();
    GLModelWidget* pv = t ? t->previewWidget() : nullptr;
    if (!pv) return;
    if (pv->animFrameCount() <= 0) { setStatus(QStringLiteral("No animation is playing to record.")); return; }
    bool noPhys = false;
    if (!confirmGifPhysics(this, pv, noPhys)) return;   // ask BEFORE the save dialog
    const QString path = captureSavePath(this, QStringLiteral("anim"), QStringLiteral("gif"),
                                         QStringLiteral("Save animation loop GIF"),
                                         QStringLiteral("Animated GIF (*.gif)"));
    if (path.isEmpty()) return;
    ClothOff clothOff(noPhys ? pv : nullptr);   // restores on every exit path
    bool canceled = false;
    const bool ok = runGifWithProgress(pv,
        [pv, &path](const ExportCapture::ProgressFn& p) { return ExportCapture::animLoopGif(pv, path, p); },
        canceled);
    setStatus(canceled ? QStringLiteral("Animation export canceled.")
                       : ok ? QStringLiteral("Saved %1").arg(path)
                            : QStringLiteral("Animation export failed."));
}

// The Export menu's "Export settings…" (and the Textures tab's "Options…") now open the single
// consolidated Settings dialog focused on its Export tab — one source of truth. Options persist
// live; tabs are kept in sync via settingsChanged(). No reindex here (export options never affect
// the CASC index), unlike File > Settings which reloads on OK.
void MainWindow::openExportSettings()
{
    SettingsDialog dlg(this);
    connect(&dlg, &SettingsDialog::settingsChanged, this, [this] {
        if (!m_tabs) return;
        for (int i = 0; i < m_tabs->count(); ++i)
            static_cast<BrowserTab*>(m_tabs->widget(i))->onSettingsChanged();   // sync mirrored Tex/Anim etc.
        applyHotkeys();   // pick up any rebindings from the Hotkeys tab immediately
    });
    connect(&dlg, &SettingsDialog::wardrobeLiveChanged, this, [this](bool rebuild) {
        if (!m_tabs) return;
        for (int i = 0; i < m_tabs->count(); ++i)
            static_cast<BrowserTab*>(m_tabs->widget(i))->onSettingsLiveChanged(rebuild);
    });
    dlg.showExportTab();
    dlg.exec();
}

// Refresh the cached diablo4.dad DB (<AppData>/d4dad.json) with a conditional curl
// fetch — same external-tool pattern as the d4data/TACT-key downloads (no QtNetwork
// in this qtbase). Called when the game/d4data fingerprint changes (and on first run),
// BEFORE the index caches rebuild, so the crawl's delta phase sees fresh data. -z makes
// the server answer 304 when our copy is current, so the common case is a header
// round-trip. Synchronous with a hard timeout; any failure just leaves the old copy.
static void refreshDadDb()
{
    const QString curl = QStandardPaths::findExecutable(QStringLiteral("curl"));
    if (curl.isEmpty()) {
        qInfo("d4dad: curl not on PATH — skipping diablo4.dad DB refresh");
        return;
    }
    const QString dest = DadOverride::defaultPath();
    QDir().mkpath(QFileInfo(dest).absolutePath());
    const QString part = dest + QStringLiteral(".part");
    QFile::remove(part);
    QStringList args{QStringLiteral("-s"), QStringLiteral("-L"), QStringLiteral("-f"),
                     QStringLiteral("--max-time"), QStringLiteral("15"),
                     QStringLiteral("-w"), QStringLiteral("%{http_code}"),
                     QStringLiteral("-o"), part};
    if (QFileInfo::exists(dest))
        args << QStringLiteral("-z") << dest;   // If-Modified-Since our local copy
    args << QStringLiteral("https://diablo4.dad/d4dad.json");

    QProcess p;
    p.start(curl, args);
    if (!p.waitForFinished(20000)) {
        p.kill();
        p.waitForFinished(1000);
        QFile::remove(part);
        qInfo("d4dad: DB refresh timed out — keeping the cached copy");
        return;
    }
    const QString http = QString::fromLatin1(p.readAllStandardOutput()).trimmed();
    const QFileInfo pf(part);
    if (p.exitCode() == 0 && http.startsWith(QLatin1Char('2')) && pf.isFile() && pf.size() > 0) {
        QFile::remove(dest);
        QFile::rename(part, dest);
        DadOverride::instance().reset();   // next ensureLoaded() re-parses
        qInfo().noquote() << "d4dad: DB updated —" << pf.size() << "bytes";
    } else {
        QFile::remove(part);
        qInfo().noquote() << "d4dad: DB refresh skipped (HTTP" << http << ", exit"
                          << p.exitCode() << ") — keeping the cached copy";
    }
}

// A cheap signature of the d4data export that changes when it's updated/re-cloned (the
// downloader rewrites .git/FETCH_HEAD on every fetch), so the disk caches also rebuild on a
// d4data change — not only on a game patch.
static QString d4dataSignature(const QString& dir)
{
    if (dir.isEmpty()) return QString();
    QString sig;
    for (const char* p : { "/.git/FETCH_HEAD", "/.git/HEAD", "/json/base" }) {
        const QFileInfo fi(dir + QLatin1String(p));
        if (fi.exists()) sig += QLatin1Char(':') + QString::number(fi.lastModified().toMSecsSinceEpoch());
    }
    return sig;
}

// Async: the heavy I/O (TACT keys, CASC open with its 1.1M-path TVFS expansion, the 800k-entry
// CoreTOC parse) measured ~6.2 s and ran ON THE GUI THREAD — the window painted, then froze
// solid until it finished. It's pure data work against objects nothing else touches while the
// index is unloaded (every tab's refresh() early-returns on !isLoaded()), so it now runs on a
// detached worker; finishReload() picks up on the GUI thread with widgets and settings.
void MainWindow::reload()
{
    if (m_reloading) { m_reloadPending = true; return; }   // coalesce mid-flight requests
    m_reloading = true;
    setStatus(QStringLiteral("Opening game storage…"));

    CascReader* casc = m_casc.get();
    SnoIndex*   index = &m_index;
    const QString keysPath = Config::tactKeysPath();
    const QString gameDir  = Config::gameDir();
    const QString product  = Config::cascProduct();
    const QString d4       = Config::d4dataDir();
    std::thread([this, casc, index, keysPath, gameDir, product, d4]() {
        ReloadResult r;
        QElapsedTimer et; et.start();
        // TACT keys MUST be loaded BEFORE open(): open() expands the nested TVFS manifests,
        // and encrypted container manifests only decode when their key is already registered.
        r.nKeys = casc->applyTactKeys(keysPath);
        r.tKeys = et.restart();
        r.cascOk = casc->open(gameDir, product);
        r.tOpen = et.restart();
        QMetaObject::invokeMethod(this, [this] {
            setStatus(QStringLiteral("Reading asset index…"));
        }, Qt::QueuedConnection);
        // Index: the per-build disk cache first (skips the CASC read + 820k-entry parse + sorts),
        // then the live game's binary CoreTOC from CASC, then d4data's pre-parsed JSON.
        if (r.cascOk && index->loadFromCache(casc->buildId())) {
            r.idx = true; r.idxSrc = QStringLiteral("CoreTOC cache (build %1…)").arg(casc->buildId().left(10));
        } else if (r.cascOk && index->loadFromCasc(*casc)) {
            r.idx = true; r.idxSrc = QStringLiteral("CASC base/CoreTOC.dat");
            // BEFORE saveToCache: recovered names ride the existing per-build index cache, so the
            // payload scan happens once per game build rather than on every launch.
            index->recoverEncryptedNames(*casc);
            index->saveToCache(casc->buildId());
        } else if (index->loadFromD4data(d4)) {
            if (r.cascOk) index->recoverEncryptedNames(*casc);
            r.idx = true; r.idxSrc = QStringLiteral("d4data CoreTOC.dat.json");
        }
        if (r.idx) index->updateLatest(casc->buildId());   // snapshot/diff for the "Latest" filter
        r.tIndex = et.restart();
        QMetaObject::invokeMethod(this, [this, r]() { finishReload(r); }, Qt::QueuedConnection);
    }).detach();
}

// The window/taskbar icon, taken from the game's own ui_placeholder_square_mask (sno 1286628,
// 256x256, eTexFormat 41 = BC4). Decoded from CASC at startup rather than shipped as a .ico,
// because the tool already has the whole path — readPayloadBySno + BcDecode — and a baked copy
// would be one more thing to regenerate when the game updates.
//
// BC4 is single-channel, so the decode is a greyscale MASK, not a picture: used directly it would
// be a white square. Alpha comes from the mask and the RGB is left white, which is what makes it
// read as a shape against both light and dark taskbars.
static void applyAppIcon(CascReader* casc)
{
    if (!casc || !casc->isReady()) return;
    constexpr int kIconSno = 1286628;
    const QByteArray payload = casc->readPayloadBySno(kIconSno);
    if (payload.isEmpty()) {
        qInfo("app icon: ui_placeholder_square_mask (sno %d) not readable — keeping the default",
              kIconSno);
        return;
    }
    QImage mask = BcDecode::decode(payload, 256, 256, 41);
    if (mask.isNull()) {
        qInfo("app icon: sno %d failed to decode — keeping the default", kIconSno);
        return;
    }
    mask = mask.convertToFormat(QImage::Format_RGBA8888);
    QImage icon(mask.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < mask.height(); ++y)
        for (int x = 0; x < mask.width(); ++x)
            // Red carries the BC4 value; treat it as coverage and keep the fill white.
            icon.setPixelColor(x, y, QColor(255, 255, 255, qRed(mask.pixel(x, y))));
    QIcon ic;
    // Several sizes so Windows picks a good one for the taskbar, alt-tab and the title bar
    // instead of scaling 256 down each time.
    for (int sz : {16, 24, 32, 48, 64, 128, 256})
        ic.addPixmap(QPixmap::fromImage(
            icon.scaled(sz, sz, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    QApplication::setWindowIcon(ic);
    qInfo("app icon: set from ui_placeholder_square_mask (sno %d)", kIconSno);
}

void MainWindow::finishReload(const ReloadResult& r)
{
    QElapsedTimer tailT; tailT.start();
    const bool cascOk = r.cascOk, idx = r.idx;
    if (cascOk) applyAppIcon(m_casc);
    if (r.nKeys > 0) qInfo("CASC: %d TACT keys registered before open()", r.nKeys);
    qInfo().noquote() << "reload: gameDir=" << Config::gameDir()
                      << "product=" << Config::cascProduct()
                      << "cascOk=" << cascOk << "d4data=" << Config::d4dataDir();
    if (!r.idxSrc.isEmpty()) qInfo().noquote() << "index source:" << r.idxSrc;
    else qWarning().noquote() << "index: neither source loaded (casc lastErr:"
                              << m_casc->lastError() << ")";
    qInfo().noquote() << "index loaded=" << idx << "total=" << m_index.totalCount()
                      << "groups=" << m_index.groups().size();
    qInfo("startup: tact-keys %lld ms · casc-open %lld ms · coretoc-index %lld ms (worker thread)",
          r.tKeys, r.tOpen, r.tIndex);

    // Patch-resilience probe: confirm a known model still parses (catches a game update that
    // changed the binary format before it silently corrupts exports). Deferred — the warning
    // is exactly as useful at T+4s as at T+0.
    if (idx && cascOk) {
        QTimer::singleShot(4000, this, [this] {
            if (m_reloading) return;   // a newer reload owns the reader — its probe will run
            const FormatProbe::Result fp = FormatProbe::run(m_casc.get(), &m_index);
            if (fp.ran) {
                qInfo().noquote() << fp.summary;
                if (!fp.warning.isEmpty())
                    QMessageBox::warning(this, QStringLiteral("Model format check"), fp.warning);
            }
            // Golden decode samples: same-build hash drift or new decode failures surface as a
            // status warning immediately after startup, not when a user notices wrong pixels.
            QString gd;
            const int gs = goldenSampleCheck(m_casc.get(), &m_index, &gd);
            qInfo().noquote() << QStringLiteral("golden samples: %1").arg(gd);
            if (gs == 1 || gs == 2)
                setStatus(QStringLiteral("⚠ Decode regression check: %1  (Help ▸ Health check for details)").arg(gd));
        });
    }

    if (idx) {
        setStatus(QStringLiteral("Ready — %1 assets across %2 groups%3")
                      .arg(m_index.totalCount())
                      .arg(m_index.groups().size())
                      .arg(cascOk ? QString() : QStringLiteral("   (no game folder — previews/exports disabled)")));
    } else if (Config::gameDir().isEmpty() && Config::d4dataDir().isEmpty()) {
        setStatus(QStringLiteral("Not configured — open File ▸ Settings: set your Diablo IV "
                                 "folder and Download d4data."));
    } else {
        setStatus(QStringLiteral("No asset index found — Download d4data in File ▸ Settings "
                                 "(CASC: %1).").arg(cascOk ? QStringLiteral("open")
                                                           : m_casc->lastError()));
    }

    // Guided first-run: nothing loaded (bad/missing paths) → a status line is easy to miss, so offer
    // to open Settings directly. Once per session (no nagging on repeated reloads) and deferred so it
    // never blocks startup construction. If the user configures things, the next reload has idx=true.
    if (!idx && !m_setupPrompted) {
        m_setupPrompted = true;
        QTimer::singleShot(0, this, [this] {
            const auto r = QMessageBox::question(this, QStringLiteral("Welcome — set up Diablo IV Asset Browser"),
                QStringLiteral(
                    "No game data is loaded yet. Two quick things in Settings:\n\n"
                    "  1.  Diablo IV folder — point it at your game install (for live CASC textures/models).\n"
                    "  2.  d4data — the extracted metadata. Click Download to fetch it automatically\n"
                    "        (needs 'git' on PATH), or Browse to an existing copy.\n\n"
                    "Everything the tool writes — settings, caches, and the downloaded d4data — stays in "
                    "the portable 'data' folder next to the .exe. Nothing touches the registry or AppData.\n\n"
                    "Open Settings now?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (r == QMessageBox::Yes) openSettings();
        });
    }

    // Stale-cache guard. The icon / appearance / asset-link indexes persist to disk but only
    // self-validate on a file count (AssetLinks: not at all), so after a game patch or a d4data
    // update they would serve stale metadata against the new build — blank/wrong icons and
    // broken texture↔model links. Fingerprint the data on the CASC build id (the TVFS root
    // hash, which changes every patch) + the d4data signature; on any change, wipe those caches
    // so they rebuild fresh. This keeps the tool working across game/d4data updates.
    {
        const QString fp = m_casc->buildId() + QLatin1Char('|') + Config::d4dataDir()
                         + QLatin1Char('|') + d4dataSignature(Config::d4dataDir());
        QSettings s;
        if (!fp.isEmpty() && s.value(QStringLiteral("index/fingerprint")).toString() != fp) {
            qInfo().noquote() << "data fingerprint changed — rebuilding icon/appearance/link indexes";
            // Pull a fresh diablo4.dad DB FIRST so the appearance crawl's delta phase
            // (fill icons the stale d4data snapshot misses) rebuilds against current data.
            refreshDadDb();
            IconIndex::instance().reset();
            AppearanceMeta::instance().reset();
            AssetLinks::instance().reset();
            ItemHoverIndex::instance().reset();   // hover metadata re-derives from the new snapshot
            BackTrophyIndex::instance().reset();  // Item→Actor→Appearance chain is snapshot-specific
            WardrobeAnimIndex::instance().reset();   // ItemType→weapon class + the wardrobe AnimSets
            QPixmapCache::clear();   // in-memory thumbnails (grid views) — stale art after a patch
            s.setValue(QStringLiteral("index/fingerprint"), fp);
        } else if (!QFileInfo::exists(DadOverride::defaultPath())) {
            refreshDadDb();   // first-run bootstrap of the diablo4.dad DB
        }
    }

    updateStalenessWarning();      // game-vs-d4data drift, missing keys, untested build
    ensureOverrideScaffold();      // user-editable post-patch correction points (idempotent)

    // New storage: drop every tab's cached state, then refresh the visible one now
    // and the rest lazily on first view.
    const qint64 tFinger = tailT.elapsed();
    for (int i = 0; i < m_tabs->count(); ++i)
        static_cast<BrowserTab*>(m_tabs->widget(i))->reset();
    const qint64 tReset = tailT.elapsed();
    m_refreshed.clear();
    if (auto* w = m_tabs->currentWidget()) {
        m_refreshed.insert(w);
        static_cast<BrowserTab*>(w)->refresh();
    }
    qInfo("startup: finish (GUI) %lld ms — fingerprint %lld · tab resets %lld · first-tab refresh %lld",
          tailT.elapsed(), tFinger, tReset - tFinger, tailT.elapsed() - tReset);

    // ── Idle pre-warm: quietly refresh() the not-yet-visited tabs a few seconds after startup,
    // one at a time, so the first CLICK on a heavy tab (Wardrobe/Stable assemble a full model)
    // costs nothing — the work already happened while the app sat idle. Each shot re-checks that
    // the user hasn't visited the tab meanwhile and that no reload is in flight.
    if (idx) {
        int delayMs = 2500;
        for (int i = 0; i < m_tabs->count(); ++i) {
            QWidget* w = m_tabs->widget(i);
            if (m_refreshed.contains(w)) continue;
            QTimer::singleShot(delayMs, this, [this, w] {
                if (m_reloading || m_refreshed.contains(w)) return;
                QElapsedTimer pw; pw.start();
                m_refreshed.insert(w);
                static_cast<BrowserTab*>(w)->refresh();
                qInfo("prewarm: %s refreshed in %lld ms (idle)",
                      qPrintable(m_tabs->tabText(m_tabs->indexOf(w))), pw.elapsed());
            });
            delayMs += 1800;   // stagger so pre-warms never stack on one event-loop turn
        }
    }

    m_reloading = false;
    if (m_reloadPending) {           // a reload was requested mid-flight (settings change) —
        m_reloadPending = false;     // honour it now that the storage is settled
        reload();
    }
}

void MainWindow::runIconAudit()
{
    // Synchronous on purpose: CascReader isn't guarded for concurrent readers, and
    // this is a hidden diagnostic — a few seconds with a wait cursor is fine.
    if (!AppearanceMeta::instance().ready()) {
        QMessageBox::information(this, QStringLiteral("Icon audit"),
            QStringLiteral("The appearance index is still building — wait for "
                           "“Indexing” to finish (Models tab), then retry."));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    setStatus(QStringLiteral("Icon audit running…"));
    const QString summary = IconAudit::run(Config::d4dataDir(), &m_index, m_casc.get());
    QApplication::restoreOverrideCursor();
    setStatus(summary);
    qInfo().noquote() << summary;
    QMessageBox::information(this, QStringLiteral("Icon audit"), summary);
}

// Auto-regenerate icon_audit.txt once BOTH the appearance crawl and the icon-sprite index
// finish — so the report always reflects the current build/crawl without a manual File ▸ Icon
// audit. One-shot per launch, no dialog (just the status line + log).
void MainWindow::autoIconAudit()
{
    if (m_iconAuditRan) return;
    if (!AppearanceMeta::instance().ready() || !IconIndex::instance().ready()) return;
    m_iconAuditRan = true;
    const QString summary = IconAudit::run(Config::d4dataDir(), &m_index, m_casc.get());
    setStatus(summary);
    qInfo().noquote() << summary;
}

void MainWindow::openSettings()
{
    SettingsDialog dlg(this);
    // Apply each Wardrobe/panel toggle to every tab the instant it changes (real-time),
    // not only when the dialog is accepted.
    connect(&dlg, &SettingsDialog::wardrobeLiveChanged, this, [this](bool rebuild) {
        for (int i = 0; i < m_tabs->count(); ++i)
            static_cast<BrowserTab*>(m_tabs->widget(i))->onSettingsLiveChanged(rebuild);
    });
    // Live-persisted Export/Models changes: keep the tabs' mirrored checkboxes in sync as they change.
    connect(&dlg, &SettingsDialog::settingsChanged, this, [this] {
        for (int i = 0; i < m_tabs->count(); ++i)
            static_cast<BrowserTab*>(m_tabs->widget(i))->onSettingsChanged();
        applyHotkeys();   // pick up any rebindings from the Hotkeys tab immediately
    });
    if (dlg.exec() == QDialog::Accepted) {
        for (int i = 0; i < m_tabs->count(); ++i)
            static_cast<BrowserTab*>(m_tabs->widget(i))->onSettingsChanged();
        reload();
    }
}

void MainWindow::setStatus(const QString& msg)
{
    if (!m_status) return;
    m_status->setText(msg);
    // The label clips rather than widening the window (see the size policy in the constructor), so
    // keep the full message reachable on hover.
    m_status->setToolTip(msg);
}

// ── Override scaffold (future-proofing) ─────────────────────────────────────────────────────────
// Ship the user-editable correction points EMPTY but VISIBLE: when a patch breaks icon UVs or
// naming heuristics, fixes are a dropped-in file — no rebuild. Creates data/icon_overrides/ (+ a
// README) and documents data/category_rules.json. Cheap and idempotent; runs once per launch.
static void ensureOverrideScaffold()
{
    const QString base = AppPaths::dataDir();
    QDir().mkpath(base + QStringLiteral("/icon_overrides"));
    const QString readme = base + QStringLiteral("/icon_overrides/_README.txt");
    if (!QFileInfo::exists(readme)) {
        QFile f(readme);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
            f.write("Post-patch correction folder — files dropped here override game data, no rebuild needed.\n"
                    "\n"
                    "ICON OVERRIDES (this folder):\n"
                    "  When a game patch re-lays-out an icon atlas, d4data's frame UVs go stale and icons\n"
                    "  miscrop. Export the frames with d4analyzer (Export > TexFrames) or this tool's\n"
                    "  Textures > TexFrames export, then drop the PNGs here. Naming (produced automatically):\n"
                    "      <atlasName> [<atlasSno>] - <frameIdx> <frameName>.png\n"
                    "  Both the frame-icon lookup and atlas dimensions self-correct from these files.\n"
                    "\n"
                    "CATEGORY RULES (../category_rules.json):\n"
                    "  Extends the Textures tab's name-based category filters without a recompile:\n"
                    "      { \"categories\": [ { \"name\": \"Store\", \"prefixes\": [\"shop\"],\n"
                    "                           \"contains\": [\"marketplace\"] } ] }\n"
                    "  A known name widens that category; a new name adds a category to the filter funnel.\n");
    }
}

// ── Golden regression samples (future-proofing) ─────────────────────────────────────────────────
// A tiny known-good sample set that decodes on demand and compares against a stored baseline:
// same build + changed hashes (or new failures) ⇒ a decode regression surfaced immediately, not
// by users. On a NEW build the baseline legitimately changes, so it's rewritten (failures still
// count — a texture that no longer decodes right after a patch is exactly the alarm we want).
// Returns 0 = ok, 1 = drift (hash changed, same build), 2 = decode failures, 3 = baseline written.
static int goldenSampleCheck(CascReader* casc, SnoIndex* index, QString* detail)
{
    auto say = [&](const QString& s) { if (detail) *detail = s; };
    if (!casc || !casc->isReady() || !index || !index->isLoaded()) { say(QStringLiteral("CASC or index not loaded — skipped")); return 2; }
    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty()) { say(QStringLiteral("no d4data — skipped")); return 2; }

    auto decodeHash = [&](int sno, const QString& name) -> QString {
        QFile f(QStringLiteral("%1/json/base/meta/Texture/%2.tex.json").arg(d4, name));
        if (!f.open(QIODevice::ReadOnly)) return {};
        const TexMeta meta = parseTexMetaJson(f.readAll());
        if (!meta.valid) return {};
        const QByteArray payload = casc->readPayloadBySno(quint64(sno));
        if (payload.isEmpty()) return {};
        const QImage img = BcDecode::decode(payload, meta.width, meta.height, meta.eTexFormat);
        if (img.isNull()) return {};
        QCryptographicHash h(QCryptographicHash::Md5);
        h.addData(QByteArrayView(reinterpret_cast<const char*>(img.constBits()), img.sizeInBytes()));
        return QString::fromLatin1(h.result().toHex());
    };

    const QString path = AppPaths::dataDir() + QStringLiteral("/golden_baseline.json");
    QJsonObject root;
    { QFile f(path); if (f.open(QIODevice::ReadOnly)) root = QJsonDocument::fromJson(f.readAll()).object(); }

    if (root.value(QStringLiteral("build")).toString() == casc->buildId()) {
        int drift = 0, fail = 0, checked = 0;
        for (const QJsonValue& v : root.value(QStringLiteral("samples")).toArray()) {
            const QJsonObject o = v.toObject();
            const QString h = decodeHash(o.value(QStringLiteral("sno")).toInt(),
                                         o.value(QStringLiteral("name")).toString());
            ++checked;
            if (h.isEmpty()) ++fail;
            else if (h != o.value(QStringLiteral("hash")).toString()) ++drift;
        }
        if (fail)  { say(QStringLiteral("%1 of %2 golden samples no longer decode").arg(fail).arg(checked)); return 2; }
        if (drift) { say(QStringLiteral("%1 of %2 golden samples decode DIFFERENTLY (regression?)").arg(drift).arg(checked)); return 1; }
        say(QStringLiteral("%1 golden samples verified").arg(checked));
        return 0;
    }

    // New build (or no baseline): pick the first few textures that decode and store their hashes.
    QJsonArray samples;
    const int texGroup = SnoIndex::groupIdByName(QStringLiteral("Texture"), 44);
    for (const SnoEntry& e : index->entries(texGroup)) {
        const QString h = decodeHash(e.snoId, e.name);
        if (h.isEmpty()) continue;
        samples.append(QJsonObject{{QStringLiteral("sno"), e.snoId},
                                   {QStringLiteral("name"), e.name},
                                   {QStringLiteral("hash"), h}});
        if (samples.size() >= 5) break;
    }
    if (samples.isEmpty()) { say(QStringLiteral("no texture in the index decodes — format change?")); return 2; }
    QJsonObject out{{QStringLiteral("build"), casc->buildId()}, {QStringLiteral("samples"), samples}};
    QDir().mkpath(AppPaths::dataDir());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(out).toJson(QJsonDocument::Compact));
    say(QStringLiteral("baseline written for this build (%1 samples)").arg(samples.size()));
    return 3;
}

// The newest game build this tool release was verified against (#13). Bump when validating a patch.
// Newer game builds usually still work (the probes below verify the formats live), but the marker
// tells the user whether their combination is known-good or uncharted.
static const char* kTestedGameBuild = "2.3.x";
static const int   kTestedBuildNum  = 0;   // 0 = accept any (set to a build number to gate)

// ── Data-staleness banner (future-proofing) ─────────────────────────────────────────────────────
// Three independent checks, merged into one persistent gold status-bar warning:
//   1. d4data behind the game: compare the numeric build (last dotted component) of the game's
//      .build.info Version vs d4data/buildVersion.txt → new items would be missing/mis-iconed.
//   2. TACT keys stale: CASC skipped encrypted container manifests for lack of keys.
//   3. Untested build: the game is newer than what this tool release was verified against.
void MainWindow::updateStalenessWarning()
{
    if (!m_staleWarn) return;
    auto buildNum = [](const QString& v) -> qlonglong {
        const QString last = v.section(QLatin1Char('.'), -1);
        bool ok = false; const qlonglong n = last.toLongLong(&ok);
        return ok ? n : 0;
    };
    const QString gameVer = CascReader::gameVersion(Config::gameDir());
    QString d4Ver;
    {
        QFile f(Config::d4dataDir() + QStringLiteral("/buildVersion.txt"));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text))
            d4Ver = QString::fromUtf8(f.readAll()).trimmed();
    }
    QStringList warns, tips;
    const qlonglong gameB = buildNum(gameVer), d4B = buildNum(d4Ver);
    if (gameB > 0 && d4B > 0 && gameB > d4B) {
        warns << QStringLiteral("d4data behind game");
        tips << QStringLiteral("Game is build %1 but d4data was extracted from %2 — items added since "
                               "then may be missing names/icons. File ▸ Settings ▸ Download d4data.")
                    .arg(gameVer, d4Ver);
    }
    if (m_casc && m_casc->missingKeyCount() > 0) {
        warns << QStringLiteral("%1 encrypted (missing keys)").arg(m_casc->missingKeyCount());
        tips << QStringLiteral("%1 encrypted container(s) couldn't be opened — the TACT keys file "
                               "may be outdated. File ▸ Settings ▸ Download TACT keys, then Reload.")
                    .arg(m_casc->missingKeyCount());
    }
    if (kTestedBuildNum > 0 && gameB > kTestedBuildNum) {
        warns << QStringLiteral("untested build");
        tips << QStringLiteral("This tool release was verified against build %1; your game (%2) is newer. "
                               "Most things should work — report anything that renders wrong.")
                    .arg(QLatin1String(kTestedGameBuild), gameVer);
    }
    if (warns.isEmpty()) { m_staleWarn->hide(); return; }
    m_staleWarn->setText(QStringLiteral("⚠ %1").arg(warns.join(QStringLiteral(" · "))));
    m_staleWarn->setToolTip(tips.join(QStringLiteral("\n\n")));
    m_staleWarn->show();
}

// ── Health check (Help ▸ Health check) ──────────────────────────────────────────────────────────
// One screen that answers "what broke?" after a game patch in seconds: storage, keys, d4data
// freshness, every background index, a live model-format probe and the golden decode samples.
void MainWindow::showHealthCheck()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Health check"));
    auto* lay = new QGridLayout(&dlg);
    lay->setHorizontalSpacing(14);
    lay->setVerticalSpacing(6);
    int row = 0;
    auto addRow = [&](const QString& name, int state, const QString& detail) {
        // state: 0 ok · 1 warn · 2 fail · 3 info
        static const char* kGlyph[4] = {"✓", "⚠", "✗", "•"};
        static const char* kColor[4] = {"#6f9f6f", "#e8c46a", "#c05050", "#9a9a9a"};
        auto* g = new QLabel(QString::fromUtf8(kGlyph[qBound(0, state, 3)]), &dlg);
        g->setStyleSheet(QStringLiteral("color:%1;font-weight:bold;").arg(QLatin1String(kColor[qBound(0, state, 3)])));
        auto* n = new QLabel(name, &dlg);
        n->setStyleSheet(QStringLiteral("font-weight:bold;"));
        auto* d = new QLabel(detail, &dlg);
        d->setWordWrap(true);
        d->setTextInteractionFlags(Qt::TextSelectableByMouse);
        lay->addWidget(g, row, 0);
        lay->addWidget(n, row, 1);
        lay->addWidget(d, row, 2);
        ++row;
    };
    QApplication::setOverrideCursor(Qt::WaitCursor);

    // 1 · Game folder + version.
    const QString gameVer = CascReader::gameVersion(Config::gameDir());
    const bool gameOk = QFileInfo::exists(Config::gameDir() + QStringLiteral("/.build.info"));
    addRow(QStringLiteral("Game folder"), gameOk ? 0 : 2,
           gameOk ? QStringLiteral("%1  ·  version %2").arg(Config::gameDir(), gameVer.isEmpty() ? QStringLiteral("?") : gameVer)
                  : QStringLiteral("no .build.info at %1 — set it in File ▸ Settings").arg(
                        Config::gameDir().isEmpty() ? QStringLiteral("(unset)") : Config::gameDir()));

    // 2 · CASC storage.
    const bool cascOk = m_casc && m_casc->isReady();
    addRow(QStringLiteral("CASC storage"), cascOk ? 0 : 2,
           cascOk ? QStringLiteral("open  ·  build %1…").arg(m_casc->buildId().left(12))
                  : QStringLiteral("not open%1").arg(m_casc ? QStringLiteral("  ·  ") + m_casc->lastError() : QString()));

    // 3 · TACT keys.
    const int keys = m_casc ? m_casc->tactKeyCount() : 0;
    const int missing = m_casc ? m_casc->missingKeyCount() : 0;
    addRow(QStringLiteral("TACT keys"), missing ? 1 : (keys ? 0 : 1),
           QStringLiteral("%1 registered%2").arg(keys)
               .arg(missing ? QStringLiteral("  ·  %1 container(s) still encrypted — keys may be stale "
                                             "(Settings ▸ Download TACT keys)").arg(missing) : QString()));

    // 4 · d4data snapshot + freshness.
    QString d4Ver;
    { QFile f(Config::d4dataDir() + QStringLiteral("/buildVersion.txt"));
      if (f.open(QIODevice::ReadOnly | QIODevice::Text)) d4Ver = QString::fromUtf8(f.readAll()).trimmed(); }
    const bool d4Ok = !d4Ver.isEmpty();
    auto bnum = [](const QString& v) { bool ok = false; qlonglong n = v.section(QLatin1Char('.'), -1).toLongLong(&ok); return ok ? n : 0; };
    const bool behind = d4Ok && bnum(gameVer) > 0 && bnum(gameVer) > bnum(d4Ver);
    addRow(QStringLiteral("d4data"), d4Ok ? (behind ? 1 : 0) : 2,
           d4Ok ? QStringLiteral("version %1%2").arg(d4Ver, behind
                      ? QStringLiteral("  ·  BEHIND game (%1) — new items may be missing (Settings ▸ Download d4data)").arg(gameVer)
                      : QString())
                : QStringLiteral("missing — Settings ▸ Download d4data"));

    // 5 · Asset index.
    const bool idxOk = m_index.isLoaded() && m_index.totalCount() > 0;
    addRow(QStringLiteral("Asset index"), idxOk ? 0 : 2,
           idxOk ? QStringLiteral("%1 assets across %2 groups").arg(m_index.totalCount()).arg(m_index.groups().size())
                 : QStringLiteral("not loaded"));

    // 6 · Background metadata indexes.
    auto idxState = [](bool ready, bool building) { return ready ? 0 : (building ? 1 : 2); };
    auto idxText  = [](bool ready, bool building) {
        return ready ? QStringLiteral("ready") : (building ? QStringLiteral("building…") : QStringLiteral("not built")); };
    AppearanceMeta& am = AppearanceMeta::instance();
    IconIndex& ii = IconIndex::instance();
    AssetLinks& al = AssetLinks::instance();
    addRow(QStringLiteral("Metadata indexes"),
           qMax(qMax(idxState(am.ready(), am.building()), idxState(ii.ready(), ii.building())),
                idxState(al.ready(), al.building())),
           QStringLiteral("appearance meta %1  ·  icons %2  ·  asset links %3")
               .arg(idxText(am.ready(), am.building()), idxText(ii.ready(), ii.building()),
                    idxText(al.ready(), al.building())));

    // 7 · Model format probe (live parse of a known player body).
    if (cascOk && idxOk) {
        const FormatProbe::Result fp = FormatProbe::run(m_casc.get(), &m_index);
        addRow(QStringLiteral("Model format"), fp.ran ? (fp.ok ? 0 : 2) : 1,
               fp.ran ? fp.summary : QStringLiteral("couldn't probe (model not found in index)"));
    } else {
        addRow(QStringLiteral("Model format"), 3, QStringLiteral("skipped — needs CASC + index"));
    }

    // 8 · Golden decode samples (regression baseline).
    {
        QString detail;
        const int st = goldenSampleCheck(m_casc.get(), &m_index, &detail);
        addRow(QStringLiteral("Golden samples"), st == 3 ? 3 : st, detail);
    }

    // 9 · Tested-build marker.
    addRow(QStringLiteral("Tested against"), 3,
           QStringLiteral("this tool release was verified on game build %1%2")
               .arg(QLatin1String(kTestedGameBuild),
                    gameVer.isEmpty() ? QString() : QStringLiteral("  ·  yours is %1").arg(gameVer)));

    QApplication::restoreOverrideCursor();
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    lay->addWidget(bb, row, 0, 1, 3);
    dlg.setMinimumWidth(680);
    dlg.exec();
}

// ── Global jump (Ctrl+K) + back/forward history ────────────────────────────────────────────────
// The palette searches the two SNO-addressable tabs (Models = Appearance, Textures) by name
// substring or SNO digits, and jumps to the picked asset in its owning tab. Every jump (palette,
// Associated-Models reveal, history walk) goes through jumpTo(); Alt+Left/Right retrace.

// Current location = (tab index, selected SNO on the two addressable tabs).
MainWindow::NavLoc MainWindow::currentNavLoc() const
{
    NavLoc loc;
    loc.tab = m_tabs->currentIndex();
    if (loc.tab == 0)      loc.sno = static_cast<TexturesTab*>(m_tabs->widget(0))->currentSno();
    else if (loc.tab == 1) loc.sno = static_cast<ModelsTab*>(m_tabs->widget(1))->currentSno();
    return loc;
}

void MainWindow::navRecord()
{
    const NavLoc loc = currentNavLoc();
    if (!m_navBack.isEmpty() && m_navBack.last().tab == loc.tab && m_navBack.last().sno == loc.sno)
        return;   // no duplicate consecutive entries
    m_navBack.append(loc);
    if (m_navBack.size() > 50) m_navBack.removeFirst();
    m_navFwd.clear();   // a new jump invalidates the forward chain (browser semantics)
}

void MainWindow::jumpTo(int group, int sno, bool record)
{
    if (sno <= 0) return;
    if (record) navRecord();
    const int tab = (group == 44) ? 0 : 1;   // Textures : Models
    QWidget* w = m_tabs->widget(tab);
    m_tabs->setCurrentIndex(tab);
    if (!m_refreshed.contains(w)) { m_refreshed.insert(w); static_cast<BrowserTab*>(w)->refresh(); }
    if (tab == 0) static_cast<TexturesTab*>(w)->selectBySno(sno);
    else          static_cast<ModelsTab*>(w)->selectModelBySno(sno);
}

void MainWindow::navGo(bool back)
{
    QVector<NavLoc>& from = back ? m_navBack : m_navFwd;
    QVector<NavLoc>& to   = back ? m_navFwd : m_navBack;
    if (from.isEmpty()) { setStatus(back ? QStringLiteral("No further back history.")
                                         : QStringLiteral("No forward history.")); return; }
    to.append(currentNavLoc());
    if (to.size() > 50) to.removeFirst();
    const NavLoc loc = from.takeLast();
    if (loc.sno > 0) jumpTo(loc.tab == 0 ? 44 : 9, loc.sno, /*record=*/false);
    else if (loc.tab >= 0) m_tabs->setCurrentIndex(loc.tab);
}

void MainWindow::showJumpPalette()
{
    if (!m_jump) {
        m_jump = new QFrame(this, Qt::Popup);
        m_jump->setObjectName(QStringLiteral("jumpPalette"));
        m_jump->setStyleSheet(QStringLiteral(
            "QFrame#jumpPalette{background:#232323;border:1px solid #5a5a5a;border-radius:6px;}"
            "QLineEdit{background:#2b2b2b;border:1px solid #555;border-radius:3px;padding:4px 8px;color:#eee;}"
            "QListWidget{background:#262626;border:none;color:#ccc;}"
            "QListWidget::item{padding:3px 8px;}"
            "QListWidget::item:selected{background:#8a1414;color:#fff;}"));
        auto* lay = new QVBoxLayout(m_jump);
        lay->setContentsMargins(8, 8, 8, 8);
        lay->setSpacing(6);
        m_jumpEdit = new QLineEdit(m_jump);
        m_jumpEdit->setPlaceholderText(QStringLiteral("Jump to model or texture…   name or SNO"));
        m_jumpList = new QListWidget(m_jump);
        m_jumpList->setUniformItemSizes(true);
        m_jumpList->setFixedHeight(320);
        lay->addWidget(m_jumpEdit);
        lay->addWidget(m_jumpList);
        m_jump->setFixedWidth(560);

        auto doActivate = [this](QListWidgetItem* item) {
            if (!item) item = m_jumpList->currentItem();
            if (!item && m_jumpList->count() > 0) item = m_jumpList->item(0);
            if (!item) return;
            m_jump->hide();
            jumpTo(item->data(Qt::UserRole + 1).toInt(), item->data(Qt::UserRole).toInt());
        };
        connect(m_jumpEdit, &QLineEdit::returnPressed, this, [doActivate] { doActivate(nullptr); });
        connect(m_jumpList, &QListWidget::itemActivated, this,
                [doActivate](QListWidgetItem* it) { doActivate(it); });
        m_jumpEdit->installEventFilter(this);   // Up/Down steer the list from the edit

        connect(m_jumpEdit, &QLineEdit::textChanged, this, [this](const QString& raw) {
            m_jumpList->clear();
            const QString q = raw.trimmed();
            if (q.size() < 2) return;
            bool digits = true;
            for (const QChar c : q) if (!c.isDigit()) { digits = false; break; }
            int budget = 50;
            auto scan = [&](int group, const QString& kind) {
                for (const SnoEntry& e : m_index.entries(group)) {
                    if (budget <= 0) return;
                    const bool hit = digits ? QString::number(e.snoId).startsWith(q)
                                            : e.name.contains(q, Qt::CaseInsensitive);
                    if (!hit) continue;
                    auto* it = new QListWidgetItem(QStringLiteral("%1    ·  %2  ·  %3")
                                                       .arg(e.name).arg(e.snoId).arg(kind));
                    it->setData(Qt::UserRole, e.snoId);
                    it->setData(Qt::UserRole + 1, group);
                    m_jumpList->addItem(it);
                    --budget;
                }
            };
            scan(9,  QStringLiteral("Model"));
            scan(44, QStringLiteral("Texture"));
            if (m_jumpList->count() > 0) m_jumpList->setCurrentRow(0);
        });
    }
    // Centre under the menu bar, like the indexing toast.
    m_jumpEdit->clear();
    m_jumpList->clear();
    const int x = (width() - m_jump->width()) / 2;
    m_jump->move(mapToGlobal(QPoint(qMax(0, x), (menuBar() ? menuBar()->height() : 0) + 12)));
    m_jump->show();
    m_jumpEdit->setFocus();
}

// Up/Down (and PageUp/PageDown) pressed in the palette's edit steer the result list.
bool MainWindow::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_jumpEdit && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        const int k = ke->key();
        if (k == Qt::Key_Down || k == Qt::Key_Up || k == Qt::Key_PageDown || k == Qt::Key_PageUp) {
            if (m_jumpList->count() > 0) {
                int row = m_jumpList->currentRow();
                const int step = (k == Qt::Key_PageDown) ? 10 : (k == Qt::Key_PageUp) ? -10
                                : (k == Qt::Key_Down) ? 1 : -1;
                m_jumpList->setCurrentRow(qBound(0, row + step, m_jumpList->count() - 1));
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, ev);
}

// Unified export confirmation — a bottom-centre toast with an optional "Show in folder" link.
// Every tab routes its export result through ExportNotifier so the confirmation looks identical
// everywhere (replacing the old mix of modal dialogs, status-bar text and per-tab toasts).
void MainWindow::showExportToast(const QString& text, const QString& folder)
{
    if (!m_exportToast) {
        m_exportToast = new QLabel(this);
        m_exportToast->setTextFormat(Qt::RichText);
        m_exportToast->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
        m_exportToast->setStyleSheet(QStringLiteral(
            "QLabel{ background:rgba(26,26,28,0.96); color:#e8e8e8; border:1px solid #4a4a4a;"
            " border-radius:6px; padding:8px 14px; }"));
        connect(m_exportToast, &QLabel::linkActivated, this, [](const QString& href) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(href));
        });
        m_exportToastTimer = new QTimer(this);
        m_exportToastTimer->setSingleShot(true);
        connect(m_exportToastTimer, &QTimer::timeout, this,
                [this] { if (m_exportToast) m_exportToast->hide(); });
    }
    QString html = QStringLiteral("<b>✓</b>  %1").arg(text.toHtmlEscaped());
    if (!folder.isEmpty())
        html += QStringLiteral("     <a style='color:#e8c46a; text-decoration:none;' href=\"%1\">"
                               "Show in folder</a>").arg(folder.toHtmlEscaped());
    m_exportToast->setText(html);
    m_exportToast->adjustSize();
    positionExportToast();
    m_exportToast->show();
    m_exportToast->raise();
    m_exportToastTimer->start(6000);
}

void MainWindow::positionExportToast()
{
    if (!m_exportToast) return;
    const QSize s = m_exportToast->size();
    m_exportToast->move((width() - s.width()) / 2, height() - s.height() - 44);
}

// One persistent indexing indicator for the whole app, pinned to the status bar so it's visible on
// every tab. Aggregates the three background index singletons into "<phase> NN%" + a bar, then shows
// "Ready ✓" and fades out when all are done.
void MainWindow::buildIndexIndicator()
{
    // A floating "⟳ <phase> NN%" toast pinned to the top-centre of the window. It sits ABOVE the
    // content (mouse-transparent, not in any layout) so ticking progress never reflows the UI.
    m_idxLabel = new QLabel(this);
    m_idxLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_idxLabel->setStyleSheet(QStringLiteral(
        "QLabel{ background:rgba(26,26,28,0.92); color:#e8c46a; border:1px solid #4a4a4a;"
        " padding:4px 12px; font-weight:bold; }"));
    m_idxFade = new QGraphicsOpacityEffect(m_idxLabel);
    m_idxLabel->setGraphicsEffect(m_idxFade);
    m_idxLabel->hide();

    // Persistent, compact indicator pinned to the RIGHT of the status bar — always visible (unlike the
    // floating toast, which fades). Shows the running phase + % while building, then a subtle "✓".
    m_idxStatus = new QLabel(this);
    m_idxStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusBar()->addPermanentWidget(m_idxStatus);

    // Data-staleness banner: game build vs d4data snapshot, missing TACT keys, untested build.
    // Hidden when everything is current; gold and persistent (with a detailed tooltip) when not.
    m_staleWarn = new QLabel(this);
    m_staleWarn->setStyleSheet(QStringLiteral("color:#e8c46a;font-weight:bold;"));
    m_staleWarn->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_staleWarn->hide();
    statusBar()->addPermanentWidget(m_staleWarn);

    m_idxHideTimer = new QTimer(this);
    m_idxHideTimer->setSingleShot(true);
    connect(m_idxHideTimer, &QTimer::timeout, this, [this] {
        auto* anim = new QPropertyAnimation(m_idxFade, "opacity", this);
        anim->setDuration(500);
        anim->setStartValue(1.0);
        anim->setEndValue(0.0);
        connect(anim, &QPropertyAnimation::finished, this, [this] {
            m_idxLabel->hide();
            if (m_idxFade) m_idxFade->setOpacity(1.0);
        });
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    });

    connect(&AppearanceMeta::instance(), &AppearanceMeta::progress, this,
            [this](int p) { m_metaPct = p; refreshIndexIndicator(); });
    connect(&AppearanceMeta::instance(), &AppearanceMeta::readyChanged, this,
            [this] { refreshIndexIndicator(); });
    connect(&IconIndex::instance(), &IconIndex::progress, this,
            [this](int p) { m_iconPct = p; refreshIndexIndicator(); });
    connect(&IconIndex::instance(), &IconIndex::readyChanged, this,
            [this] { refreshIndexIndicator(); });
    connect(&AssetLinks::instance(), &AssetLinks::progress, this,
            [this](int p) { m_linkPct = p; refreshIndexIndicator(); });
    connect(&AssetLinks::instance(), &AssetLinks::readyChanged, this,
            [this] { refreshIndexIndicator(); });
    // Auto-regenerate icon_audit.txt once indexing completes (whichever of the two finishes last).
    connect(&AppearanceMeta::instance(), &AppearanceMeta::readyChanged, this, &MainWindow::autoIconAudit);
    connect(&IconIndex::instance(), &IconIndex::readyChanged, this, &MainWindow::autoIconAudit);
    refreshIndexIndicator();
}

void MainWindow::refreshIndexIndicator()
{
    if (!m_idxLabel) return;
    const bool meta = AppearanceMeta::instance().building();
    const bool icon = IconIndex::instance().building();
    const bool link = AssetLinks::instance().building();

    // Per-stage line for the hover tooltip: a spinner while running, ✓ when done.
    auto stageLine = [](const QString& name, bool running, int pct, const QString& what) {
        const QString state = running ? QStringLiteral("⟳ %1%").arg(qBound(0, pct, 100))
                                      : QStringLiteral("✓ done");
        return QStringLiteral("<tr><td><b>%1</b></td><td>&nbsp;%2&nbsp;</td>"
                              "<td style='color:#999'>%3</td></tr>")
            .arg(name, state, what);
    };
    const QString tip = QStringLiteral(
        "<div style='max-width:340px'>"
        "<b>Background indexing</b><br>"
        "One-time scans that build the lookup tables every tab shares. They run "
        "after a game patch or a d4data update; the app is fully usable meanwhile.<hr>"
        "<table cellspacing='0'>%1%2%3</table></div>")
        .arg(stageLine(QStringLiteral("Metadata"), meta, m_metaPct,
                       QStringLiteral("item ▸ appearance ▸ icon map")),
             stageLine(QStringLiteral("Icons"),    icon, m_iconPct,
                       QStringLiteral("inventory sprite atlases")),
             stageLine(QStringLiteral("Links"),    link, m_linkPct,
                       QStringLiteral("model ▸ texture ▸ material links")));
    m_idxLabel->setToolTip(tip);

    if (meta || icon || link) {
        QString phase; int pct = 0;
        if (meta)      { phase = QStringLiteral("Indexing metadata"); pct = m_metaPct; }
        else if (icon) { phase = QStringLiteral("Loading icons");     pct = m_iconPct; }
        else           { phase = QStringLiteral("Linking assets");    pct = m_linkPct; }
        m_idxGlobalMsg = QStringLiteral("%1 %2%").arg(phase).arg(qBound(0, pct, 100));
    } else {
        m_idxGlobalMsg.clear();
    }
    // Mirror into the persistent status-bar indicator (shares the detailed hover tooltip).
    if (m_idxStatus) {
        m_idxStatus->setToolTip(tip);
        if (meta || icon || link) {
            m_idxStatus->setStyleSheet(QStringLiteral("color:#e8c46a;"));
            m_idxStatus->setText(QStringLiteral("⟳ %1").arg(m_idxGlobalMsg));
        } else {
            m_idxStatus->setStyleSheet(QStringLiteral("color:#6f9f6f;"));
            m_idxStatus->setText(QStringLiteral("Indexes ✓"));
        }
    }
    updateToast();
}

// Merge the global build status with the active tab's scan status into the floating toast; show it
// centred at the top when anything is running, otherwise flash "✓ Ready" and fade out.
void MainWindow::updateToast()
{
    if (!m_idxLabel) return;
    QStringList parts;
    if (!m_idxGlobalMsg.isEmpty()) parts << m_idxGlobalMsg;
    if (!m_idxTabMsg.isEmpty())    parts << m_idxTabMsg;
    const QString text = parts.join(QStringLiteral("     ·     "));
    if (text.isEmpty()) {
        if (m_idxLabel->isVisible() && !m_idxHideTimer->isActive()) {
            m_idxLabel->setText(QStringLiteral("✓ Ready"));
            m_idxLabel->adjustSize();
            positionToast();
            m_idxHideTimer->start(1200);
        }
        return;
    }
    m_idxHideTimer->stop();
    if (m_idxFade) m_idxFade->setOpacity(1.0);
    m_idxLabel->setText(QStringLiteral("⟳  %1").arg(text));
    m_idxLabel->adjustSize();
    positionToast();
    m_idxLabel->show();
    m_idxLabel->raise();
}

void MainWindow::positionToast()
{
    if (!m_idxLabel) return;
    const int top = (menuBar() ? menuBar()->height() : 0) + 8;
    m_idxLabel->move(qMax(0, (width() - m_idxLabel->width()) / 2), top);
}

void MainWindow::resizeEvent(QResizeEvent* e)
{
    QMainWindow::resizeEvent(e);
    if (m_idxLabel && m_idxLabel->isVisible()) positionToast();
}
