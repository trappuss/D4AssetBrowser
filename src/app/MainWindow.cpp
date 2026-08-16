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
#include "index/StoreProductIndex.h"
#include "index/ItemHoverIndex.h"
#include "index/DadOverride.h"
#include <QStyle>
#include <QSystemTrayIcon>

#include "index/AnimActionIndex.h"
#include "model/MaterialDecode.h"
#include "index/IconAudit.h"
#include "model/FormatProbe.h"
#include "index/IconIndex.h"
#include "tabs/TexturesTab.h"
#include "tabs/ModelsTab.h"
#include "tabs/StableTab2.h"
#include "tabs/WardrobeTab2.h"
#include "tabs/BulkExtractorTab.h"
#include "tabs/CatalogueTab.h"
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
#include <QPointer>
#include <QMenuBar>
#include <QResizeEvent>
#include <QSettings>
#include <QDir>
#include <QDirIterator>   // Health check counts the snapshot's files to report coverage
#include <QFileInfo>
#include <QDateTime>
#include <QCheckBox>
#include <QMessageBox>
#include <QProgressDialog>
#include <QProcess>
#include "util/ProcQuiet.h"   // the d4dad curl fetch must not flash a console
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
#include "tex/TextureDefTable.h"
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
    setWindowTitle(QStringLiteral("D4AssetBrowser v%1")
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
    // Build every index now rather than waiting for the tab that needs each one. Useful right
    // after a patch, and before an audit or bulk export where a half-built index silently
    // produces a worse result. Settings ▸ Interface can do this automatically at startup.
    // A submenu rather than one action: each index also gets its own row showing whether it is
    // done, running (with a percentage) or not started, and can be built on its own.
    buildIndexMenu(file);
    // (Models-tab preferences now live in File > Settings > General > "Models tab".)
    // Hidden-ish diagnostic: cross-check every resolved icon against the diablo4.dad
    // DB and write data\icon_audit.txt (regression check after updates).
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
    m_actExportAllFiltered = exp->addAction(QStringLiteral("Export all matching…"),
                                            this, &MainWindow::exportActiveAllFiltered);
    m_actExportAllFilteredLast = exp->addAction(QStringLiteral("Export all matching last dir"),
                                                this, &MainWindow::exportActiveAllFilteredLast);
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
    // Coverage of the BUILT-IN Bulk Extract presets. They are 27 hard-coded queries against a
    // dataset that changes every patch: a family the game renames turns its preset into a silent
    // zero, and nothing in the UI distinguishes that from a preset nobody happened to click.
    // Measuring them is one pass, so it is a menu item rather than a thing to remember.
    // Defined below, after LazyTab — the lookup has to materialise the lazy tab, and LazyTab is
    // declared further down this file than buildMenu is.
    help->addAction(QStringLiteral("Audit bulk &presets…"), this, [this] { runPresetAudit(); });
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
            "D4AssetBrowser v%1 (built %2)\nQt %3 · %4\nGPU: %5\n"
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
            QStringLiteral("<b>D4AssetBrowser</b> v%1 &nbsp;<span style='color:#888'>"
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
        "<tr><td class='k'>Ctrl+1…6</td><td>Switch tab</td></tr>"
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
    // EVERY BrowserTab export virtual must be forwarded here. This proxy IS the tab as far as the
    // Export menu is concerned — activeTab() hands back the LazyTab, so a virtual that is not
    // forwarded silently resolves to BrowserTab's default and the feature is dead in the shipped
    // app while looking correct in the tab's own source. Adding a virtual to BrowserTab means
    // adding it here in the same edit.
    bool    hasExportAllFiltered() const override { return m_child && m_child->hasExportAllFiltered(); }
    QString exportAllFilteredLabel() const override {
        return m_child ? m_child->exportAllFilteredLabel() : BrowserTab::exportAllFilteredLabel(); }
    void    exportAllFiltered() override { if (m_child) m_child->exportAllFiltered(); }
    void    exportAllFilteredToLast() override { if (m_child) m_child->exportAllFilteredToLast(); }
    // The built child, for callers that need the concrete type (jumpTo → CatalogueTab::revealBundle).
    // Null until first shown; ensure() builds it.
    BrowserTab* built() const { return m_child; }
    BrowserTab* buildNow() { return ensure(); }
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
            [this](const QString& s) {
                if (s.isEmpty()) m_idxTabMsgs.remove(QStringLiteral("models"));
                else             m_idxTabMsgs.insert(QStringLiteral("models"), s);
                updateToast();
            });
    // A "Sold in" link in the INFO panel → that bundle in the Catalogue, with nav history so
    // Alt+Left returns to the model you came from. group 110 = StoreProduct.
    connect(models, &ModelsTab::revealBundleRequested, this,
            [this](int sno) { jumpTo(110, sno); });
    add(new LazyTab([](QWidget* p) { return new WardrobeTab2(p); }), QStringLiteral("Wardrobe"));
    add(new LazyTab([](QWidget* p) { return new StableTab2(p); }), QStringLiteral("Stable"));
    // Catalogue — the Cosmetics Shop (SNO group 110, StoreProduct). Like Bulk Extract it holds the
    // Models + Textures tabs so a bundle export goes through the SAME pipelines, and therefore
    // produces byte-identical files to exporting those SNOs by hand.
    add(new LazyTab([this, models, textures](QWidget* p) {
            auto* c = new CatalogueTab(models, textures, p);
            // Double-clicking a bundle item opens it in the Models tab, with nav history so
            // Alt+Left comes back. Connected inside the FACTORY because LazyTab builds the real
            // widget on first show and the wrapper has no signals of its own; `this` is MainWindow,
            // so the private jumpTo is in scope.
            connect(c, &CatalogueTab::revealModelRequested, this,
                    [this](int sno) { jumpTo(9, sno); });
            connect(c, &CatalogueTab::revealTextureRequested, this,
                    [this](int sno) { jumpTo(44, sno); });   // group 44 → the Textures tab
            // Its index build into the shared floating toast, keyed separately from the Models tab
            // so two startup scans cannot overwrite one another.
            connect(c, &CatalogueTab::scanStatus, this,
                    [this](const QString& s) {
                        if (s.isEmpty()) m_idxTabMsgs.remove(QStringLiteral("catalogue"));
                        else             m_idxTabMsgs.insert(QStringLiteral("catalogue"), s);
                        updateToast();
                    });
            return c;
        }), QStringLiteral("Catalogue"));
    add(new LazyTab([this, models, textures](QWidget* p) {
            auto* b = new BulkExtractorTab(models, textures, p);
            // Its "Export settings…" button opens the shared dialog — wired HERE, at construction,
            // because the tab is lazy and there is no other moment it is known to exist.
            connect(b, &BulkExtractorTab::exportSettingsRequested,
                    this, &MainWindow::openExportSettings);
            return b;
        }), QStringLiteral("Bulk Extract"));   // reuses Models + Textures export pipelines

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
    // "Export all matching" — the tab supplies the whole label because only it knows the live
    // match count, and a count that goes stale is worse than no count.
    const bool canAll = t && t->hasExportAllFiltered();
    if (m_actExportAllFiltered) {
        m_actExportAllFiltered->setVisible(canAll);
        m_actExportAllFiltered->setEnabled(canAll);
        if (canAll) m_actExportAllFiltered->setText(t->exportAllFilteredLabel() + QStringLiteral("…"));
    }
    if (m_actExportAllFilteredLast) {
        m_actExportAllFilteredLast->setVisible(canAll);
        m_actExportAllFilteredLast->setEnabled(canAll);
        if (canAll) m_actExportAllFilteredLast->setText(t->exportAllFilteredLabel()
                                                        + QStringLiteral(" last dir"));
    }
}

void MainWindow::exportActiveAllFiltered()
{
    if (BrowserTab* t = activeTab()) { if (t->hasExportAllFiltered()) t->exportAllFiltered(); }
}

void MainWindow::exportActiveAllFilteredLast()
{
    if (BrowserTab* t = activeTab()) { if (t->hasExportAllFiltered()) t->exportAllFilteredToLast(); }
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
// Returns true only when the file on disk actually changed, so the caller can re-derive the
// indexes that read it — and skip that work in the common 304 case.
//
// MUST NOT be called on the GUI thread: it blocks for up to 20 s (curl --max-time 15 plus the
// waitForFinished margin). It used to be, which is why the window froze for several seconds a
// couple of seconds after launch whenever the game or d4data fingerprint changed.
static bool refreshDadDb()
{
    const QString curl = QStandardPaths::findExecutable(QStringLiteral("curl"));
    if (curl.isEmpty()) {
        qInfo("d4dad: curl not on PATH — skipping diablo4.dad DB refresh");
        return false;
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
    quietProcess(p);   // no console window flash — this runs on every fingerprint change
    p.start(curl, args);
    if (!p.waitForFinished(20000)) {
        p.kill();
        p.waitForFinished(1000);
        QFile::remove(part);
        qInfo("d4dad: DB refresh timed out — keeping the cached copy");
        return false;
    }
    const QString http = QString::fromLatin1(p.readAllStandardOutput()).trimmed();
    const QFileInfo pf(part);
    if (p.exitCode() == 0 && http.startsWith(QLatin1Char('2')) && pf.isFile() && pf.size() > 0) {
        QFile::remove(dest);
        QFile::rename(part, dest);
        DadOverride::instance().reset();   // next ensureLoaded() re-parses
        qInfo().noquote() << "d4dad: DB updated —" << pf.size() << "bytes";
        return true;   // genuinely new bytes — the caller re-derives what depends on them
    }
    QFile::remove(part);
    // 304 is the COMMON case and not a failure: -z asked "only if newer than what I have".
    qInfo().noquote() << "d4dad: DB unchanged (HTTP" << http << ", exit"
                      << p.exitCode() << ") — keeping the cached copy";
    return false;
}

// Fire-and-forget wrapper: run the fetch on a worker, and only if it brought back NEW bytes, drop
// the two caches that read the DB so they re-derive with it. Detached because nothing waits on the
// result — the contract has always been "any failure just leaves the old copy".
//
// reset() is posted back to the GUI thread: these singletons are reset from MainWindow::reload on
// the GUI thread too, and resetting from a worker while a build is in flight is the race their
// generation counters exist to survive, not one to add deliberately.
static void startDadRefresh()
{
    std::thread([] {
        if (!refreshDadDb()) return;                      // 304 / offline / no curl → nothing to do
        if (!QCoreApplication::instance()) return;   // app already torn down (fast quit)
        QMetaObject::invokeMethod(qApp, [] {
            qInfo("d4dad: DB changed — re-deriving appearance icons against it");
            AppearanceMeta::instance().reset();            // the delta phase reads the DB
            IconIndex::instance().reset();
        }, Qt::QueuedConnection);
    }).detach();
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
    // buildVersion.txt CONTENT, not just an mtime. The three entries above all assume a git
    // checkout: Settings lets you point at a plain unzipped copy, and overwriting the files inside
    // such a folder changes neither .git (absent) nor json/base's own directory mtime — so the
    // fingerprint held steady across a snapshot swap and every cache survived it. The build string
    // is the snapshot's own identity and changes whenever it does.
    QFile bv(dir + QStringLiteral("/buildVersion.txt"));
    if (bv.open(QIODevice::ReadOnly | QIODevice::Text))
        sig += QLatin1Char(':') + QString::fromUtf8(bv.readAll()).trimmed();
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
    // The auto icon-audit worker is READING m_index and AppearanceMeta right now, and the block
    // below rebuilds both. Defer instead of racing — autoIconAudit's completion handler picks the
    // pending reload back up. The audit is bounded (a few seconds) and one-shot per launch, so
    // this can delay a reload but cannot starve it.
    if (m_iconAuditRunning) { m_reloadPending = true; return; }
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
        if (r.cascOk && index->loadFromCache(casc->buildAndKeySignature())) {
            r.idx = true; r.idxSrc = QStringLiteral("CoreTOC cache (build %1…)").arg(casc->buildId().left(10));
        } else if (r.cascOk && index->loadFromCasc(*casc)) {
            r.idx = true; r.idxSrc = QStringLiteral("CASC base/CoreTOC.dat");
            // BEFORE saveToCache: recovered names ride the existing per-build index cache, so the
            // payload scan happens once per game build rather than on every launch.
            //
            // Dict pass FIRST. It carries the asset's real authored name, so anything it resolves
            // must not be pre-empted by the cloth-string heuristic — which only reads a name that
            // happens to be embedded in a submesh, and can only ever reach cloth-bearing pieces.
            // Running it first also seeds the uniqueness set the heuristic checks against.
            index->applyEncryptedNameDicts(*casc);
            index->recoverEncryptedNames(*casc);
            // Signature includes the TACT key set: adding a key changes what the
            // EncryptedNameDict pass can name, so it must invalidate this cache.
            index->saveToCache(casc->buildAndKeySignature());
        } else if (index->loadFromD4data(d4)) {
            if (r.cascOk) index->applyEncryptedNameDicts(*casc);
            if (r.cascOk) index->recoverEncryptedNames(*casc);
            r.idx = true; r.idxSrc = QStringLiteral("d4data CoreTOC.dat.json");
        }
        // Snapshot/diff for the "Latest" filter, keyed by the product actually opened so retail
        // and PTR keep separate baselines instead of overwriting each other's. The human version
        // rides along so the build ledger can label entries "2.3.1.65956" rather than a hash.
        if (r.idx) index->updateLatest(casc->buildId(), casc->lineageKey(), casc->openedVersion());

        // ── Material name → SNO, from the LIVE index ────────────────────────────────────────
        // Lets the material decoder fall back to the game binary for NAMED materials, not just
        // encrypted "~unnamed_" ones. d4data is a community snapshot that lags the game: it ships
        // every RogF/RogM_stor273 material and no BarM_stor273 at all, so those parts rendered
        // untextured while the installed game had them the whole time.
        //
        // Built once here, on the reload worker, rather than searched per lookup: the Material
        // group is thousands of entries and a material miss can happen for every part of every
        // model. Lower-cased keys because .mat.json names and index names differ in case.
        if (r.idx) {
            // BOTH material groups. The name table calls 37 "Material" and 57 "Material (2)", and
            // the ones that matter are in 57 — PalM_stor188_LEG_mat and armor_skin_mat are both
            // there. Building from groupIdByName("Material") alone returns 37 and yields a map
            // that misses essentially every real material, which is a fix that silently does
            // nothing.
            auto map = std::make_shared<QHash<QString, qint64>>();
            for (const int g : {SnoIndex::groupIdByName(QStringLiteral("Material"), 37),
                                SnoIndex::groupIdByName(QStringLiteral("Material (2)"), 57)})
                for (const SnoEntry& e : index->entries(g))
                    if (!e.name.isEmpty()) map->insert(e.name.toLower(), e.snoId);
            r.nMatNames = map->size();
            MaterialDecode::setNameResolver([map](const QString& n) -> qint64 {
                return map->value(n.toLower(), 0);
            });
        }
        r.tIndex = et.restart();

        // Prewarm the clip→action map HERE, on this worker. It is a ~1700 ms walk of the AnimSet,
        // Emote and StringList trees, and it used to run on first use — which is inside
        // fillAnimList(), on the GUI thread, during the Wardrobe's first build. Measured, it was
        // 1701 ms of a 1702 ms populateAnims. Starting it now (~1 s in) means it is finished before
        // the idle prewarm reaches the Wardrobe (~4.5 s in); if the Wardrobe somehow gets there
        // first, ensure() blocks on the mutex instead of racing, which is no worse than before.
        {
            QElapsedTimer aiT; aiT.start();
            AnimActionIndex::instance().ensure(d4);
            r.tAnimAction = aiT.elapsed();
        }
        QMetaObject::invokeMethod(this, [this, r]() { finishReload(r); }, Qt::QueuedConnection);
    }).detach();
}

void MainWindow::finishReload(const ReloadResult& r)
{
    QElapsedTimer tailT; tailT.start();
    const bool cascOk = r.cascOk, idx = r.idx;
    if (r.nKeys > 0) qInfo("CASC: %d TACT keys registered before open()", r.nKeys);
    qInfo().noquote() << "reload: gameDir=" << Config::gameDir()
                      << "product=" << Config::cascProduct()
                      << "cascOk=" << cascOk << "d4data=" << Config::d4dataDir();
    if (!r.idxSrc.isEmpty()) qInfo().noquote() << "index source:" << r.idxSrc;
    else qWarning().noquote() << "index: neither source loaded (casc lastErr:"
                              << m_casc->lastError() << ")";
    // ── D4_VERIFY_KEYS=<key file>: is this list Diablo IV's? ────────────────────────────────────
    // A key is D4's, and ours, exactly when its EncryptedNameDict decodes to the 0xABCD4567 header.
    // That is a decidable test, so no key list ever has to be taken on faith — drop a candidate
    // file in and this reports which entries are real, which have no dict in this build, and which
    // have a dict but the wrong key value. Verified keys are registered as a side effect.
    //
    // m_casc, not a local: this is finishReload on the GUI thread, and `casc` is reload()'s
    // worker-thread local. Placed AFTER the index-source if/else above — dropped between them it
    // stole that `else`, so "neither source loaded" fired whenever the env var was simply unset.
    if (r.cascOk && m_casc && qEnvironmentVariableIsSet("D4_VERIFY_KEYS")) {
        const QString kf = qEnvironmentVariable("D4_VERIFY_KEYS");
        const QStringList rep = m_casc->verifyTactKeys(kf);
        QFile vf(AppPaths::file(QStringLiteral("tact_key_verify.txt")));
        if (vf.open(QIODevice::WriteOnly | QIODevice::Truncate))
            vf.write(rep.join(QLatin1Char('\n')).toUtf8());
    }

    // ── D4_DUMP_PRD=1: locate arBundledProducts inside the StoreProduct META binary ──────────────
    // Why this is needed: the Catalogue is built ENTIRELY from d4data's
    // json/base/meta/StoreProduct/*.prd.json — 7,496 files against 9,308 records in CoreTOC. The
    // ~1,800-product gap is encrypted and newly-patched content, and it is exactly the content
    // people look for: of the Doom (stor251) bundles only `dru` and `rog` ship a .prd.json, so the
    // other five classes' bundles cannot appear in the tab at all. The install is also two patches
    // ahead of the snapshot, so the gap widens every update.
    //
    // Reading those products from CASC needs the binary layout, and this measures it instead of
    // guessing: for a product that HAS json we already know its child SNOs, so their u32 values are
    // searched for in its meta blob and every hit offset is reported. A constant offset — or a
    // constant stride — across many samples is the array. Anything less is not an answer.
    //
    // Same discipline as D4_DUMP_MATSNO, which is how the material table was found.
    //
    // UNATTENDED, and it must WAIT. finishReload runs the moment CASC opens — long before
    // StoreProductIndex has crawled 7,496 .prd.json files. The first version fired right here and
    // wrote "product json count: 0 / sampled 0 bundle(s)": a report that reads like a FINDING
    // ("the binary holds no children") when it is only an empty index, and it left the window open
    // with nothing to say the run was over.
    //
    // So: kick the index, run on readyChanged, write, quit. Same shape as D4_HEALTH_AUDIT and
    // D4_CHAINTEST, which is exactly what makes those safe to drive from a .bat.
    if (r.cascOk && m_casc && qEnvironmentVariableIsSet("D4_DUMP_PRD")) {
        StoreProductIndex& spi = StoreProductIndex::instance();
        if (spi.ready()) {
            dumpPrdProbe();
        } else {
            qInfo("prd-probe: store-product index not built yet — waiting for it, then writing "
                  "the report and exiting. Leave the window alone.");
            connect(&spi, &StoreProductIndex::readyChanged, this, [this] { dumpPrdProbe(); });
            spi.ensureBuilt(Config::d4dataDir(), &m_index, m_casc.get());
        }
    }
    // ── D4_DUMP_TEX=<sno>[,<sno>…]: walk ONE texture through every stage of the decode ───────────
    // Why this exists: druM_stor235_HLM listed sixteen textures with correct SNOs, held the key for
    // them, and still rendered flat grey with an empty TEXTURE PREVIEW — while logging NOTHING.
    // Three separate fixes were aimed at call sites that, it turns out, cannot have been the ones
    // failing, because a failure in any of them would have printed a warning and none did.
    //
    // Guessing which stage breaks from the outside has now cost more rebuilds than it is worth, so
    // this walks the chain end to end with no UI involved and prints what each stage returned. The
    // first line that disagrees with expectation IS the bug, and no further speculation is needed.
    if (r.cascOk && m_casc && qEnvironmentVariableIsSet("D4_DUMP_TEX")) {
        const QStringList ids = qEnvironmentVariable("D4_DUMP_TEX")
                                    .split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& idStr : ids) {
            bool okNum = false;
            const int sno = idStr.trimmed().toInt(&okNum);
            if (!okNum || sno <= 0) continue;
            qInfo("dump-tex %d ---------------------------------------------", sno);

            // 1. What does the game say this asset IS?
            const QHash<int, QByteArray>& enc = m_casc->encryptedSnos();
            const auto eh = enc.constFind(sno);
            if (eh == enc.constEnd()) {
                qInfo("dump-tex %d: not listed in EncryptedSNOs (unencrypted)", sno);
            } else {
                qInfo("dump-tex %d: encrypted, key %s, key held = %s", sno,
                      eh.value().toHex().constData(),
                      m_casc->haveTactKey(eh.value()) ? "YES" : "NO");
            }

            // 2. Dimensions/format — the stage that decides whether decoding is even attempted.
            TextureDefTable::instance().ensureBuilt(m_casc.get());
            const TextureDefTable::Def d = TextureDefTable::instance().lookup(sno);
            qInfo("dump-tex %d: TextureDefTable -> %dx%d fmt %d (valid=%s)", sno,
                  d.width, d.height, d.format, d.valid() ? "yes" : "NO");

            // 3. Is the payload even in this install, and under which path?
            const CascReader::PayloadVariants pv = m_casc->payloadVariants(quint64(sno));
            qInfo("dump-tex %d: payload stored bytes — base/payload %llu, base/paylow %llu",
                  sno, pv.payload, pv.paylow);

            // 4. Does it actually read + decrypt?
            const QByteArray raw = m_casc->readPayloadBySno(quint64(sno));
            qInfo("dump-tex %d: readPayloadBySno -> %lld byte(s)", sno, qint64(raw.size()));

            // 5. And does the block decoder accept it? Called directly, so a null here is
            //    BcDecode's verdict alone, with no call-site or UI state in the way.
            if (d.valid() && !raw.isEmpty()) {
                const QImage img = BcDecode::decode(raw, d.width, d.height, d.format);
                qInfo("dump-tex %d: BcDecode -> %s", sno,
                      img.isNull() ? "NULL (format unsupported or data too short)"
                                   : qPrintable(QStringLiteral("%1x%2 ok").arg(img.width())
                                                    .arg(img.height())));
            }
            // 6. Finally the real entry point every call site uses, name-less as an encrypted
            //    texture genuinely is — if 1-5 pass and this returns null, the bug is in texture().
            const QImage viaApi = MaterialDecode::texture(m_casc.get(), Config::d4dataDir(),
                                                          QString(), sno);
            qInfo("dump-tex %d: MaterialDecode::texture -> %s", sno,
                  viaApi.isNull() ? "NULL" : qPrintable(QStringLiteral("%1x%2 ok")
                                                 .arg(viaApi.width()).arg(viaApi.height())));
        }
    }

    // ── D4_ICON_SWEEP=1: find the inventory-icon HANDLE offset in the appearance meta, unattended ─
    // The earlier per-model probe needed someone to click through thirty models, which is not a
    // measurement, it is a chore. This sweeps the corpus itself, writes a verdict, and quits.
    //
    // Ground truth: for a NAMED appearance AppearanceMeta already knows the handle, so the u32 is
    // searched for in that appearance's meta blob and the hit OFFSETS are tallied across hundreds of
    // samples. A field at a fixed offset shows up as one offset dominating; anything else is a
    // negative result, which is equally worth having — it kills the approach instead of leaving it
    // to be guessed at later.
    //
    // ANSWERED, 2026-08-03: 600 of 600 samples — handle ABSENT from the appearance meta entirely,
    // not even scattered hits. It is NOT stored there. The handle lives on the ITEM (tInvImages at
    // +368, see ItemDef), and the item -> appearance join is by NAME in AppearanceMeta's CASC phase.
    // That join was failing only because the cache signature counted appearances instead of noticing
    // they had been renamed; fixing it took encrypted-appearance icons from 1 to 106.
    //
    // Kept because it is cheap, env-gated and re-runnable after a patch changes the meta layout —
    // but do not expect it to say anything new today.
    if (r.cascOk && m_casc && qEnvironmentVariableIsSet("D4_ICON_SWEEP")) {
        AppearanceMeta::instance().ensureBuilt(Config::d4dataDir(), &m_index, m_casc.get());
        auto* poll = new QTimer(this);
        poll->setInterval(250);
        // waited lives in the lambda (mutable init-capture), not on the heap — the earlier `new int`
        // was a leak for no reason.
        connect(poll, &QTimer::timeout, this, [this, poll, waited = 0]() mutable {
            // AppearanceMeta builds on a worker; without it there is no ground truth to search for.
            if (!AppearanceMeta::instance().ready()) {
                if ((waited += 250) < 120000) return;
                qWarning("icon-sweep: AppearanceMeta never became ready — nothing to measure");
            }
            poll->stop();

            const int kMaxSamples = 600;
            const QVector<SnoEntry>& apps = m_index.entries(9);   // Appearance
            int sampled = 0, absent = 0, unreadable = 0;
            QHash<int, int> offsetHits;                            // meta offset -> how many samples
            for (const SnoEntry& e : apps) {
                if (sampled >= kMaxSamples) break;
                const quint32 want = AppearanceMeta::instance().iconFor(e.snoId);
                if (!want) continue;                               // no ground truth for this one
                const QByteArray meta = m_casc->readMetaBySno(quint64(e.snoId));
                if (meta.size() < 4) { ++unreadable; continue; }
                ++sampled;
                bool any = false;
                for (int off = 0; off + 4 <= meta.size(); ++off) {
                    const quint32 v = quint32(uchar(meta[off]))
                                    | quint32(uchar(meta[off + 1])) << 8
                                    | quint32(uchar(meta[off + 2])) << 16
                                    | quint32(uchar(meta[off + 3])) << 24;
                    if (v == want) { offsetHits[off] += 1; any = true; }
                }
                if (!any) ++absent;
            }

            QVector<QPair<int, int>> ranked;                       // (hits, offset)
            for (auto it = offsetHits.constBegin(); it != offsetHits.constEnd(); ++it)
                ranked.append({it.value(), it.key()});
            std::sort(ranked.begin(), ranked.end(),
                      [](const QPair<int, int>& a, const QPair<int, int>& b) { return a.first > b.first; });

            const int best = ranked.isEmpty() ? 0 : ranked.first().first;
            const double share = sampled ? double(best) / double(sampled) : 0.0;
            QString verdict;
            if (sampled == 0)
                verdict = QStringLiteral("NO SAMPLES — no named appearance had both a known handle "
                                         "and a readable meta blob.");
            else if (share >= 0.80)
                verdict = QStringLiteral("FIELD FOUND at offset 0x%1 — present in %2 of %3 samples "
                                         "(%4%). Encrypted appearances can read their icon handle "
                                         "from there.")
                              .arg(ranked.first().second, 0, 16).arg(best).arg(sampled)
                              .arg(int(share * 100));
            else if (absent > sampled / 2)
                verdict = QStringLiteral("DEAD END — the handle is absent from the meta blob in %1 "
                                         "of %2 samples. It is not stored there; this route is out.")
                              .arg(absent).arg(sampled);
            else
                verdict = QStringLiteral("NO CONSTANT OFFSET — best offset 0x%1 covers only %2 of %3 "
                                         "samples (%4%). Not a fixed field; do not build on it.")
                              .arg(ranked.isEmpty() ? 0 : ranked.first().second, 0, 16)
                              .arg(best).arg(sampled).arg(int(share * 100));

            QStringList rep;
            rep << QStringLiteral("D4AssetBrowser — inventory-icon handle sweep")
                << QStringLiteral("appearances with a known handle AND a readable meta: %1").arg(sampled)
                << QStringLiteral("handle absent from the blob entirely: %1").arg(absent)
                << QStringLiteral("meta unreadable (encrypted / missing): %1").arg(unreadable)
                << QString() << QStringLiteral("VERDICT: %1").arg(verdict) << QString()
                << QStringLiteral("top offsets (hits / %1 samples):").arg(sampled);
            for (int i = 0; i < ranked.size() && i < 20; ++i)
                rep << QStringLiteral("    0x%1   %2")
                           .arg(ranked[i].second, 0, 16).arg(ranked[i].first);

            const QString path = AppPaths::file(QStringLiteral("icon_handle_probe.txt"));
            QFile rf(path);
            if (rf.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                rf.write(rep.join(QLatin1Char('\n')).toUtf8());
            qInfo().noquote() << "icon-sweep:" << verdict;
            qInfo().noquote() << "icon-sweep: report written to" << path;
            QCoreApplication::quit();   // unattended: the whole point is that nobody has to sit here
        });
        poll->start();
    }

    qInfo().noquote() << "index loaded=" << idx << "total=" << m_index.totalCount()
                      << "groups=" << m_index.groups().size();
    qInfo("startup: tact-keys %lld ms · casc-open %lld ms · coretoc-index %lld ms · "
          "anim-actions %lld ms · material-names %d (worker thread)",
          r.tKeys, r.tOpen, r.tIndex, r.tAnimAction, r.nMatNames);

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
        // ── The TACT KEY COUNT belongs in the fingerprint ───────────────────────────────────────
        // Without it, "File ▸ Update TACT Keys" changed nothing visible. Every cache that recorded
        // "this asset could not be decoded" — appearance metadata, icons, texture info — stayed
        // valid, so newly-unlocked collab and seasonal content remained invisible until some
        // UNRELATED change (a game patch, a d4data pull) happened to move the fingerprint. The one
        // action whose entire purpose is to unlock content was the one action that did not trigger
        // a rebuild.
        //
        // The count is the right measure: keys are added over time and effectively never removed,
        // so a change in count means new content just became readable and everything derived from
        // "what can we decode" must be re-derived.
        const QString fp = m_casc->buildId() + QLatin1Char('|') + Config::d4dataDir()
                         + QLatin1Char('|') + d4dataSignature(Config::d4dataDir())
                         + QLatin1Char('|') + QString::number(m_casc->tactKeyCount());
        QSettings s;
        if (!fp.isEmpty() && s.value(QStringLiteral("index/fingerprint")).toString() != fp) {
            qInfo().noquote() << "data fingerprint changed — rebuilding icon/appearance/link indexes";
            // ── The diablo4.dad refresh runs OFF the GUI thread ─────────────────────────────────
            // It used to run here, synchronously, and blocked the window for up to 20 seconds a
            // second or two after launch — a network fetch on the UI thread, with no dialog and no
            // way to cancel. That is the freeze.
            //
            // Ordering was the reason it was first: the appearance crawl's delta phase fills icons
            // from this DB, so a stale copy means a worse rebuild. Rather than block for that, the
            // resets below run NOW against whatever copy exists, and the fetch re-triggers the two
            // indexes that actually read the DB — but only if the bytes really changed. In the
            // common case (HTTP 304, or no network) that costs nothing at all.
            startDadRefresh();
            IconIndex::instance().reset();
            AppearanceMeta::instance().reset();
            AssetLinks::instance().reset();
            ItemHoverIndex::instance().reset();   // hover metadata re-derives from the new snapshot
            BackTrophyIndex::instance().reset();  // Item→Actor→Appearance chain is snapshot-specific
            WardrobeAnimIndex::instance().reset();   // ItemType→weapon class + the wardrobe AnimSets
            // Clip→action labels ("Walk", "Get Hit", emote names). Memory-only and built once per
            // process, so without this a d4data change left every clip labelled from the previous
            // snapshot until the app was restarted — silently.
            AnimActionIndex::instance().reset();
            // Its cache validates on FILE COUNT alone and its path carries no d4data identity, so
            // a snapshot swap that keeps the count would serve the old catalogue forever.
            StoreProductIndex::instance().reset();
            // The menu/indicator state must be dropped with them: a percentage left over from the
            // previous snapshot's build would be displayed against an index that no longer has one.
            m_idxPct.clear();
            m_idxKicked.clear();
            // ── Caches that validate on a MAGIC NUMBER alone ────────────────────────────────────
            // stable_index_v* and tex_info_v* carry no build id, no d4data signature and no file
            // count, and neither tab's reset() deletes them. Once written they were frozen for the
            // life of the install: a mount added by a patch never appeared in the Stable tab, and a
            // new texture never gained a format/dimension entry, with nothing to tell the user why
            // or any UI to clear them. Deleted here rather than by teaching each one a signature —
            // the fingerprint is already the single authority for "the data underneath changed".
            {
                const QDir dd(AppPaths::dataDir());
                for (const QString& pat : {QStringLiteral("stable_index_v*.bin"),
                                           QStringLiteral("tex_info_v*.bin")})
                    for (const QString& fn : dd.entryList({pat}, QDir::Files))
                        QFile::remove(dd.filePath(fn));
            }
            QPixmapCache::clear();   // in-memory thumbnails (grid views) — stale art after a patch
            s.setValue(QStringLiteral("index/fingerprint"), fp);
        } else if (!QFileInfo::exists(DadOverride::defaultPath())) {
            startDadRefresh();   // first-run bootstrap of the diablo4.dad DB, also off-thread
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
    // It is NOT free, and it is not invisible: refresh() on a lazy tab BUILDS it, and building the
    // Wardrobe or Stable creates an OpenGL viewport and assembles a full character — ~800 ms of
    // synchronous GUI-thread work, plus a briefly-visible window as the GL widget is created. From
    // the user's side that is the app freezing and something flashing a couple of seconds after
    // launch, for no reason they asked for. Trading "slow first click" for "mystery stall" is a bad
    // trade when the stall is unattributable.
    //
    // So: still on by default (the first click really is much faster), but switchable, and each
    // shot now says in the log which tab it is about to build BEFORE it blocks — so the pause has
    // a name in any bug report instead of being a silent gap.
    // ── Index all on startup ────────────────────────────────────────────────────────────────────
    // Opt-in. Deferred, and deferred LONGER than the pre-warm was: this is strictly more work, so
    // firing it while the first tab is still settling would make the launch feel worse than the
    // thing it is meant to get ahead of. It also SUPERSEDES the pre-warm below — Index all already
    // refreshes every tab, and running both would build each one twice.
    const bool indexAllAtStart = QSettings().value(QStringLiteral("ui/indexAllOnStartup"), false).toBool();
    if (idx && indexAllAtStart)
        QTimer::singleShot(5000, this, [this] { if (!m_reloading) indexAll(); });

    if (idx && !indexAllAtStart
        && QSettings().value(QStringLiteral("ui/prewarmTabs"), true).toBool()) {
        int delayMs = 4000;   // was 2500 — clear of the first-tab refresh and the index toasts
        for (int i = 0; i < m_tabs->count(); ++i) {
            QWidget* w = m_tabs->widget(i);
            if (m_refreshed.contains(w)) continue;
            QTimer::singleShot(delayMs, this, [this, w] {
                if (m_reloading || m_refreshed.contains(w)) return;
                const QString name = m_tabs->tabText(m_tabs->indexOf(w));
                // BEFORE the work, not after: a line that only appears on completion tells you
                // nothing while you are staring at a frozen window.
                qInfo("prewarm: building %s in the background — the window may pause briefly",
                      qPrintable(name));
                QElapsedTimer pw; pw.start();
                m_refreshed.insert(w);
                static_cast<BrowserTab*>(w)->refresh();
                qInfo("prewarm: %s refreshed in %lld ms (idle)", qPrintable(name), pw.elapsed());
            });
            delayMs += 2500;   // stagger so pre-warms never stack on one event-loop turn
        }
    }

    m_reloading = false;
    if (m_reloadPending) {           // a reload was requested mid-flight (settings change) —
        m_reloadPending = false;     // honour it now that the storage is settled
        reload();
    }
}

// ── File ▸ Index ▸ Index all ────────────────────────────────────────────────────────────────────────────
// Kick EVERY background index at once instead of waiting for whichever tab happens to need one.
//
// Normally each index is built on demand: the Models tab starts the appearance crawl, the Textures
// tab the asset links, the Catalogue the shop products, and so on. That is right for a quick look
// at one asset and wrong for two cases this exists to serve — right after a game patch, when you
// want every cache rebuilt before you start work rather than hitting a pause per tab; and before an
// audit or a bulk export, where a half-built index quietly produces a worse result (the icon audit
// abstains, exports skip clips, the Catalogue shows fewer bundles).
//
// Every ensureBuilt() below is a no-op if that index is already built or in flight, so this is safe
// to invoke repeatedly and safe to run alongside whatever a tab has already started. The heavy work
// is on the indexes' own worker threads; the tab refreshes are the only GUI-thread cost, and they
// are what builds the lazy tabs and their per-tab caches.
// Help ▸ Audit bulk presets. Defined here rather than inline in buildMenu because it has to
// MATERIALISE the lazy Bulk Extract tab, and LazyTab is declared below buildMenu.
void MainWindow::runPresetAudit()
{
    // findChild alone would return null until the tab has been clicked once — the same LazyTab
    // trap that once left "Export all matching" dead in a shipped build while looking correct in
    // the tab's own source. Build it first, then look.
    BulkExtractorTab* bulk = nullptr;
    for (int i = 0; i < (m_tabs ? m_tabs->count() : 0); ++i) {
        QWidget* page = m_tabs->widget(i);
        // dynamic_cast, not qobject_cast: LazyTab is a plain QWidget subclass with no Q_OBJECT
        // (it declares no signals/slots of its own), and qobject_cast static-asserts on that.
        if (auto* lz = dynamic_cast<LazyTab*>(page)) lz->ensure();
        if ((bulk = page->findChild<BulkExtractorTab*>())) break;
    }
    if (!bulk) {
        setStatus(QStringLiteral("Preset audit: the Bulk Extract tab is unavailable."));
        return;
    }
    const QString msg = bulk->auditPresets();
    setStatus(msg);
    QMessageBox::information(this, QStringLiteral("Bulk preset audit"),
                             msg + QStringLiteral("\n\nFull report: data\\preset_audit.txt"));
}

// ── The index roster ────────────────────────────────────────────────────────────────────────────
// Every background index, in the order they appear in File ▸ Index. Rebuilt on each call rather
// than cached: the lambdas capture the CURRENT d4data dir and CascReader, both of which change on
// reload, and a stale capture is exactly how a menu ends up rebuilding into a directory the app
// has already moved off. Nine cheap std::functions per menu-open is not a cost worth caching.
QVector<MainWindow::IndexDesc> MainWindow::indexRoster()
{
    const QString d4  = Config::d4dataDir();
    const SnoIndex* ix = &m_index;
    CascReader* casc  = m_casc.get();
    QVector<IndexDesc> r;

    r.push_back({ QStringLiteral("Metadata"), QStringLiteral("item ▸ appearance ▸ icon map"),
        [] { return AppearanceMeta::instance().ready(); },
        [] { return AppearanceMeta::instance().building(); },
        [d4, ix, casc] { AppearanceMeta::instance().ensureBuilt(d4, ix, casc); },
        [] { AppearanceMeta::instance().reset(); }, true });

    r.push_back({ QStringLiteral("Icons"), QStringLiteral("inventory sprite atlases"),
        [] { return IconIndex::instance().ready(); },
        [] { return IconIndex::instance().building(); },
        [d4, casc] { IconIndex::instance().ensureBuilt(d4, casc); },
        [] { IconIndex::instance().reset(); }, true });

    r.push_back({ QStringLiteral("Asset links"), QStringLiteral("model ▸ texture ▸ material links"),
        [] { return AssetLinks::instance().ready(); },
        [] { return AssetLinks::instance().building(); },
        [d4] { AssetLinks::instance().ensureBuilt(d4); },
        [] { AssetLinks::instance().reset(); }, true });

    r.push_back({ QStringLiteral("Item hover"), QStringLiteral("names, rarity, flavour text"),
        [] { return ItemHoverIndex::instance().ready(); },
        [] { return ItemHoverIndex::instance().building(); },
        [d4] { ItemHoverIndex::instance().ensureBuilt(d4); },
        [] { ItemHoverIndex::instance().reset(); }, true });

    r.push_back({ QStringLiteral("Back trophies"), QStringLiteral("trophy actors + their clips"),
        [] { return BackTrophyIndex::instance().ready(); },
        [] { return BackTrophyIndex::instance().building(); },
        [d4] { BackTrophyIndex::instance().ensureBuilt(d4); },
        [] { BackTrophyIndex::instance().reset(); }, true });

    r.push_back({ QStringLiteral("Wardrobe animations"), QStringLiteral("per-loadout wardrobe AnimSets"),
        [] { return WardrobeAnimIndex::instance().ready(); },
        [] { return WardrobeAnimIndex::instance().building(); },
        [d4] { WardrobeAnimIndex::instance().ensureBuilt(d4); },
        [] { WardrobeAnimIndex::instance().reset(); }, true });

    r.push_back({ QStringLiteral("Store products"), QStringLiteral("shop bundles and their contents"),
        [] { return StoreProductIndex::instance().ready(); },
        [] { return StoreProductIndex::instance().building(); },
        [d4, ix, casc] { StoreProductIndex::instance().ensureBuilt(d4, ix, casc); },
        [] { StoreProductIndex::instance().reset(); }, true });

    // The last two are not QObjects, so they have no building() and no progress signal.
    // TextureDefTable's ensureBuilt is SYNCHRONOUS, so by the time start() returns it is either
    // ready or it failed — it needs no in-flight tracking at all. Animation actions does; see below.
    // The only entry that needs m_idxKicked: it runs on a detached thread AND cannot signal, so
    // nothing else can tell us it finished. The mark is set and cleared HERE rather than by the
    // callers, because a caller that sets it cannot know whether ensure() actually started — an
    // empty d4 makes it return instantly, and a mark set on that path would never be cleared and
    // would make "Index all" skip this index for the rest of the process.
    r.push_back({ QStringLiteral("Animation actions"), QStringLiteral("clip ▸ action labels (~1.7 s)"),
        [] { return AnimActionIndex::instance().built(); },
        nullptr,
        [this, d4] {
            const QString nm = QStringLiteral("Animation actions");
            m_idxKicked.insert(nm);
            // Versioned: Re-index everything can reset() and re-kick while an earlier thread is
            // still finishing, and an unversioned callback would then clear the NEW run's mark and
            // report "not started" while it was in fact rebuilding.
            const int gen = ++m_animKickGen;
            // QPointer, not a raw this: the thread is detached, so it can outlive the window on
            // shutdown and post into freed memory.
            QPointer<MainWindow> self(this);
            std::thread([self, d4, nm, gen] {
                AnimActionIndex::instance().ensure(d4);
                // qApp is null once QApplication has been destroyed, which a detached thread can
                // outlive on a fast quit; posting to it then is a null-receiver crash.
                if (!QCoreApplication::instance()) return;
                QMetaObject::invokeMethod(qApp, [self, nm, gen] {
                    if (!self || gen != self->m_animKickGen) return;   // superseded by a later kick
                    self->m_idxKicked.remove(nm);
                    self->refreshIndexMenu();
                    self->refreshIndexIndicator();
                }, Qt::QueuedConnection);
            }).detach();
        },
        [] { AnimActionIndex::instance().reset(); }, false });

    r.push_back({ QStringLiteral("Texture definitions"), QStringLiteral("texture format/size table"),
        [] { return TextureDefTable::instance().ready(); },
        nullptr,
        [casc] { TextureDefTable::instance().ensureBuilt(casc); },
        [] { TextureDefTable::instance().reset(); }, false });

    return r;
}

// Is this index working right now? building() is authoritative and set SYNCHRONOUSLY by every
// ensureBuilt() before it spawns its thread, so it is true the instant start() returns. Only the
// entries that cannot report (no building()) fall back to the m_idxKicked mark they maintain
// themselves — see the Animation actions entry above.
bool MainWindow::idxRunning(const IndexDesc& d) const
{
    return d.building ? d.building() : m_idxKicked.contains(d.name);
}

// ── File ▸ Index ────────────────────────────────────────────────────────────────────────────────
// Replaces a single flat "Index All" action. That action gave no way to see what had already been
// built, what was running, or how far along it was — the only feedback was one status-bar line and
// an indicator that watched three of these nine. A submenu that re-labels itself on aboutToShow
// answers all three questions in the place you go to trigger the work.
void MainWindow::buildIndexMenu(QMenu* fileMenu)
{
    m_indexMenu = fileMenu->addMenu(QStringLiteral("&Index"));

    m_indexMenu->addAction(QStringLiteral("Index &all"), this, &MainWindow::indexAll)
        ->setToolTip(QStringLiteral("Start every index that is not already built or running. "
                                    "Anything already done is left alone."));
    m_indexMenu->addAction(QStringLiteral("&Re-index everything…"), this, &MainWindow::indexRefreshAll)
        ->setToolTip(QStringLiteral("Drop every cache and rebuild from scratch. Minutes of work — "
                                    "use after a game patch if a cache looks wrong."));
    m_indexMenu->addSeparator();
    m_indexMenu->setToolTipsVisible(true);   // per-action tooltips are hidden in menus by default

    const auto roster = indexRoster();
    m_indexActions.clear();
    for (const IndexDesc& d : roster) {
        const QString name = d.name;
        QAction* a = m_indexMenu->addAction(name);   // real label set by refreshIndexMenu()
        a->setToolTip(d.what + QStringLiteral("\nClick to build it; if it is already done, "
                                              "this drops its cache and rebuilds just this one."));
        connect(a, &QAction::triggered, this, [this, name] {
            // Same guard as Index all / Re-index everything. Without it, clicking a row before
            // storage opens builds a DEGRADED index — AppearanceMeta accepts a null reader and an
            // empty SnoIndex, caches the near-empty result and marks itself ready, after which
            // every later "Index all" skips it as done.
            if (!m_casc || !m_casc->isReady() || !m_index.isLoaded()) {
                setStatus(name + QStringLiteral(": game storage isn't open yet — try again once it is."));
                return;
            }
            for (const IndexDesc& e : indexRoster()) {
                if (e.name != name) continue;
                if (idxRunning(e)) { setStatus(name + QStringLiteral(": already running.")); return; }
                if (e.ready && e.ready() && e.reset) e.reset();   // explicit click on a done index
                m_idxPct.remove(name);
                if (e.start) e.start();
                // Report what actually happened: ensureBuilt is a no-op when its own directory is
                // missing, and claiming "started" there leaves the row re-labelling to "not
                // started" a moment later with no explanation.
                setStatus(idxRunning(e) || (e.ready && e.ready())
                              ? name + QStringLiteral(": indexing started.")
                              : name + QStringLiteral(": nothing to index (check the d4data folder)."));
                break;
            }
            refreshIndexMenu();
            refreshIndexIndicator();
        });
        m_indexActions.push_back(a);
    }

    connect(m_indexMenu, &QMenu::aboutToShow, this, &MainWindow::refreshIndexMenu);
    refreshIndexMenu();
}

void MainWindow::refreshIndexMenu()
{
    const auto roster = indexRoster();
    for (int i = 0; i < roster.size() && i < m_indexActions.size(); ++i) {
        const IndexDesc& d = roster[i];
        const bool rdy = d.ready && d.ready();
        // An index that finished has nothing left in flight, so a stale "kicked" mark is cleared
        // here rather than needing a completion callback on the two that have no signals.
        if (rdy) m_idxKicked.remove(d.name);
        const bool bld = idxRunning(d);

        QString state;
        if (bld) {
            const int pct = m_idxPct.value(d.name, -1);
            state = (d.hasPct && pct >= 0) ? QStringLiteral("%1%").arg(qBound(0, pct, 100))
                                           : QStringLiteral("building…");
        } else {
            state = rdy ? QStringLiteral("done") : QStringLiteral("not started");
        }
        m_indexActions[i]->setText(QStringLiteral("%1  —  %2").arg(d.name, state));
    }
}

// Drop every cache and rebuild. Confirmed because it throws away work that costs minutes to redo,
// and because the button next to it ("Index all") is the one that is safe to press by reflex.
void MainWindow::indexRefreshAll()
{
    if (!m_casc || !m_casc->isReady() || !m_index.isLoaded()) {
        setStatus(QStringLiteral("Re-index: game storage isn't open yet — try again once it is."));
        return;
    }
    if (QMessageBox::question(
            this, QStringLiteral("Re-index everything"),
            QStringLiteral("Drop every index cache and rebuild all of them from scratch?\n\n"
                           "This discards work that takes several minutes to redo. Use \"Index all\" "
                           "instead if you only want to finish what has not been built yet."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    m_idxPct.clear();
    for (const IndexDesc& d : indexRoster()) {
        if (d.reset) d.reset();
        if (d.start) d.start();
    }
    // Per-tab scans (Models' animation + entity indexes, Textures' format scan, Stable's roster)
    // belong to their tab, not to a singleton, so they are re-driven the only way there is: clear
    // the refreshed set and refresh every built tab.
    m_refreshed.clear();
    for (int i = 0; i < m_tabs->count(); ++i) {
        QWidget* w = m_tabs->widget(i);
        m_refreshed.insert(w);
        static_cast<BrowserTab*>(w)->refresh();
    }
    setStatus(QStringLiteral("Re-index: every cache dropped, rebuilding — watch File ▸ Index."));
    qInfo("Re-index everything: dropped and restarted every index");
    refreshIndexMenu();
    refreshIndexIndicator();
}

void MainWindow::indexAll()
{
    if (!m_casc || !m_casc->isReady() || !m_index.isLoaded()) {
        setStatus(QStringLiteral("Index all: game storage isn't open yet — try again once it is."));
        return;
    }
    setStatus(QStringLiteral("Index all: starting everything not already built — watch File ▸ Index."));

    // Driven off the SAME roster the menu and the indicator read, so all three agree on WHICH
    // indexes exist. (The progress wiring in buildIndexIndicator still names them as string
    // literals, so a tenth index would need adding there too — the roster does not cover that.)
    // Order is not significant; each entry's start()
    // is ensureBuilt(), which is a no-op when that index is already built or in flight.
    for (const IndexDesc& d : indexRoster()) {
        if ((d.ready && d.ready()) || idxRunning(d)) continue;
        if (d.start) d.start();
    }

    // Then the per-tab indexes, which only exist once their tab does: refresh() builds a lazy tab
    // and starts its own scans (Models' animation + entity indexes, Textures' format scan, Stable's
    // roster). Marked refreshed so the idle pre-warm doesn't redo them.
    int built = 0;
    for (int i = 0; i < m_tabs->count(); ++i) {
        QWidget* w = m_tabs->widget(i);
        if (m_refreshed.contains(w)) continue;
        m_refreshed.insert(w);
        static_cast<BrowserTab*>(w)->refresh();
        ++built;
    }
    qInfo("Index all: kicked every shared index; %d tab(s) built on demand", built);
    refreshIndexMenu();
    refreshIndexIndicator();
}

void MainWindow::runIconAudit()
{
    // Synchronous on purpose: this is user-initiated, so a few seconds behind a wait cursor is
    // honest feedback rather than a mystery freeze. (The old reason given here — "CascReader
    // isn't guarded for concurrent readers" — was already untrue when written: readFile holds
    // m_mutex only to resolve index entries and does the archive read and BLTE inflate outside
    // it, which is what lets the parallel bulk-extract workers run at all. autoIconAudit relies
    // on that and runs the same call on a worker.)
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
//
// ON A WORKER THREAD, deliberately. This is a signal handler, so calling IconAudit::run()
// directly ran it on the GUI thread: measured, it walked 8,654 appearances with CASC reads and
// the window sat frozen for the duration with no wait cursor and no explanation. The manual
// File ▸ Icon audit at least sets Qt::WaitCursor; this path silently looked like a hang.
//
// Safe to background because CascReader::readFile resolves index entries under m_mutex and then
// does the archive read + BLTE inflate OUTSIDE it (each read opens its own QFile) — the parallel
// bulk-extract workers already depend on exactly that — and DadOverride::ensureLoaded documents
// itself as thread-safe for precisely this caller. AppearanceMeta, IconIndex and SnoIndex are
// read-only once ready.
//
// The one genuine hazard is reload(), which REBUILDS m_index and AppearanceMeta underneath the
// worker. reload() therefore defers while m_iconAuditRunning, and the completion handler below
// honours any reload that was deferred. Deferring rather than blocking keeps the GUI responsive,
// which is the entire point of the change.
void MainWindow::autoIconAudit()
{
    if (m_iconAuditRan) return;
    if (!AppearanceMeta::instance().ready() || !IconIndex::instance().ready()) return;
    if (m_reloading) return;   // index is mid-rebuild; readyChanged fires again when it settles
    m_iconAuditRan = true;
    m_iconAuditRunning = true;

    setStatus(QStringLiteral("Icon audit running in the background…"));

    // Snapshot everything the worker needs off the GUI thread. Config::d4dataDir() reads
    // QSettings, which must not be touched from two threads at once.
    CascReader*   casc  = m_casc.get();
    SnoIndex*     index = &m_index;
    const QString d4    = Config::d4dataDir();

    std::thread([this, casc, index, d4]() {
        QElapsedTimer t; t.start();
        const QString summary = IconAudit::run(d4, index, casc);
        const qint64  ms      = t.elapsed();
        QMetaObject::invokeMethod(this, [this, summary, ms]() {
            m_iconAuditRunning = false;
            qInfo("icon audit: %lld ms (background thread — GUI stayed responsive)", ms);
            setStatus(summary);
            qInfo().noquote() << summary;
            if (m_reloadPending) {   // a reload arrived while the audit held the index
                m_reloadPending = false;
                reload();
            }
        }, Qt::QueuedConnection);
    }).detach();
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
// The probe body, split out so it can run either immediately (index already built, e.g. a second
// launch off a warm cache) or from readyChanged on a cold one. Writes the report and QUITS —
// nothing else in the app should run after this, and the .bat's /wait returns as soon as it does.
void MainWindow::dumpPrdProbe()
{
    StoreProductIndex& spi = StoreProductIndex::instance();
    QStringList rep;
    rep << QStringLiteral("StoreProduct .prd binary probe — child-array offsets");
    rep << QStringLiteral("product json count: %1").arg(spi.count());
    // Says plainly when there is nothing to measure, rather than leaving a reader to infer it from
    // "sampled 0". An empty index and a binary with no child array look identical in the numbers.
    if (spi.count() == 0)
        rep << QStringLiteral("NO PRODUCTS INDEXED — this is not a finding about the binary. "
                              "Check that d4data is downloaded and json/base/meta/StoreProduct "
                              "exists.");
    int sampled = 0;
    for (int bsno : spi.bundles()) {
        if (sampled >= 40) break;
        const auto* p = spi.product(bsno);
        if (!p || p->children.size() < 2) continue;   // need several knowns to see a stride
        const QByteArray meta = m_casc ? m_casc->readMetaBySno(quint64(bsno)) : QByteArray();
        if (meta.size() < 8) continue;
        ++sampled;
        auto u32at = [&meta](int off) -> quint32 {
            return quint32(uchar(meta[off])) | quint32(uchar(meta[off + 1])) << 8
                 | quint32(uchar(meta[off + 2])) << 16 | quint32(uchar(meta[off + 3])) << 24;
        };
        QVector<int> hits;
        for (int c : p->children)
            for (int off = 0; off + 4 <= meta.size(); ++off)
                if (u32at(off) == quint32(c)) { hits << off; break; }
        std::sort(hits.begin(), hits.end());
        QString stride = QStringLiteral("n/a");
        if (hits.size() >= 2) {
            const int d0 = hits[1] - hits[0];
            bool uniform = true;
            for (int i = 2; i < hits.size(); ++i)
                if (hits[i] - hits[i - 1] != d0) { uniform = false; break; }
            stride = uniform ? QString::number(d0) : QStringLiteral("MIXED");
        }
        QStringList offs;
        for (int h : hits) offs << QStringLiteral("0x%1").arg(h, 0, 16);

        // ── PHASE 2 — where is the COUNT? ───────────────────────────────────────────────────────
        // Phase 1 established WHERE the children are. It cannot tell us HOW MANY there are for a
        // product we have no JSON for, which is the entire purpose of the exercise: reading until
        // the values stop looking like SNOs is not parsing, it is guessing with extra steps.
        //
        // The array data sits at a FIXED offset while the records vary in length, so the bytes
        // before it are a fixed-size header — and a serialized variable array is conventionally
        // described there by an (offset, size) pair. So scan the header for a u32 equal to the
        // child count, to count*4 (a byte size), or to the array's own start offset (a pointer).
        // A hit at the SAME header offset across every sample is the field; anything that moves is
        // a coincidence of that record's data.
        const int arrayAt = hits.isEmpty() ? 0 : hits.first();
        const int n = int(p->children.size());
        QStringList hdr;
        for (int off = 0; off + 4 <= arrayAt; off += 4) {   // 4-byte aligned: struct fields will be
            const quint32 v = u32at(off);
            if (v == quint32(n))          hdr << QStringLiteral("count@0x%1").arg(off, 0, 16);
            else if (v == quint32(n) * 4) hdr << QStringLiteral("bytes@0x%1").arg(off, 0, 16);
            else if (v == quint32(arrayAt)) hdr << QStringLiteral("ptr@0x%1").arg(off, 0, 16);
        }
        if (hdr.isEmpty()) hdr << QStringLiteral("(none)");

        rep << QStringLiteral("%1 [%2] meta=%3B children=%4 found=%5 stride=%6  @ %7  hdr: %8")
                   .arg(p->name).arg(bsno).arg(meta.size())
                   .arg(p->children.size()).arg(hits.size()).arg(stride)
                   .arg(offs.join(QLatin1Char(' ')), hdr.join(QLatin1Char(' ')));
    }
    rep << QStringLiteral("sampled %1 bundle(s)").arg(sampled);

    // ── PHASE 3 — VERIFY the derived layout against EVERY product we already know ────────────────
    // Phases 1 and 2 derived the layout from 40 samples. Forty agreeing samples is where a wrong
    // reading feels finished, so this checks the derivation against all ~7,500 products that ship
    // a .prd.json: parse each one from CASC alone, then compare with the answer the JSON already
    // gave. Anything short of a near-perfect match means the layout is not safe to depend on for
    // the ~1,800 products that have NO json — which are the only reason any of this exists.
    //
    // Note the sample bias this cannot remove: every record checked here HAS json. If the JSON-less
    // records are a different shape (a newer schema, say), this cannot tell. That is why the reader
    // below must validate every field it reads rather than trusting these offsets.
    {
        constexpr int kSizeAt  = 0x2c;    // u32 — byte length of arBundledProducts
        constexpr int kArrayAt = 0x260;   // packed u32 child SNOs, immediately after the header
        int exact = 0, orderDiff = 0, badCount = 0, badValues = 0, noMeta = 0, tooShort = 0;
        QStringList bad;
        for (int bsno : spi.bundles()) {
            const auto* p = spi.product(bsno);
            if (!p) continue;
            const QByteArray meta = m_casc ? m_casc->readMetaBySno(quint64(bsno)) : QByteArray();
            if (meta.isEmpty())            { ++noMeta;   continue; }
            if (meta.size() < kArrayAt + 4) { ++tooShort; continue; }
            auto u32 = [&meta](int off) -> quint32 {
                return quint32(uchar(meta[off])) | quint32(uchar(meta[off + 1])) << 8
                     | quint32(uchar(meta[off + 2])) << 16 | quint32(uchar(meta[off + 3])) << 24;
            };
            const quint32 bytes = u32(kSizeAt);
            const int n = int(bytes / 4);
            const int want = int(p->children.size());
            if ((bytes % 4) != 0 || n != want || kArrayAt + int(bytes) > meta.size()) {
                ++badCount;
                if (bad.size() < 20)
                    bad << QStringLiteral("  COUNT %1 [%2]: header says %3B (%4), json says %5")
                               .arg(p->name).arg(bsno).arg(bytes).arg(n).arg(want);
                continue;
            }
            QVector<int> got;
            for (int i = 0; i < n; ++i) got << int(u32(kArrayAt + i * 4));
            if (got == p->children) { ++exact; continue; }
            QVector<int> a = got, b = p->children;
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            if (a == b) { ++orderDiff; continue; }   // same set, different order — still usable
            ++badValues;
            if (bad.size() < 20)
                bad << QStringLiteral("  VALUES %1 [%2]: binary != json").arg(p->name).arg(bsno);
        }
        const int total = exact + orderDiff + badCount + badValues + noMeta + tooShort;
        rep << QString();
        rep << QStringLiteral("── VERIFICATION against every bundle that has json ──");
        rep << QStringLiteral("  size field 0x%1 (u32 bytes) · array 0x%2 (packed u32)")
                   .arg(kSizeAt, 0, 16).arg(kArrayAt, 0, 16);
        rep << QStringLiteral("  checked          %1").arg(total);
        rep << QStringLiteral("  exact match      %1").arg(exact);
        rep << QStringLiteral("  same set, order  %1").arg(orderDiff);
        rep << QStringLiteral("  WRONG count      %1").arg(badCount);
        rep << QStringLiteral("  WRONG values     %1").arg(badValues);
        rep << QStringLiteral("  no meta in CASC  %1").arg(noMeta);
        rep << QStringLiteral("  meta too short   %1").arg(tooShort);
        if (!bad.isEmpty()) { rep << QStringLiteral("  first mismatches:"); rep += bad; }
        rep << QStringLiteral("  VERDICT: %1")
                   .arg((badCount == 0 && badValues == 0 && exact + orderDiff > 0)
                            ? QStringLiteral("layout holds — safe to read JSON-less products with it")
                            : QStringLiteral("DO NOT PARSE — the layout does not reproduce known data"));
    }

    // ── PHASE 4 — where does the PAYLOAD sno live? ───────────────────────────────────────────────
    // Knowing a bundle's CHILDREN is only half of what the Catalogue needs. Each child is itself a
    // product, and what makes it showable is its payload — snoItemTransmog for armour and weapons,
    // snoMount, snoCompanion, snoHeadstone and the rest. Without that offset, a bundle recovered
    // from CASC lists children that resolve to nothing, which is a worse outcome than not listing
    // it: it looks broken rather than absent.
    //
    // Same method as phase 1, and the same standard of proof. In the JSON these are THIRTEEN
    // separate fields, not a union, so each kind is expected to occupy its own slot — the tally is
    // therefore kept per kind, and a kind whose offset is not unanimous has not been measured.
    {
        QHash<int, QHash<int, int>> offByKind;   // kind -> (offset -> how many products agreed)
        QHash<int, int> seenByKind;
        int scanned = 0;
        for (const SnoEntry& e : m_index.entries(SnoIndex::groupIdByName(
                                     QStringLiteral("StoreProduct"), 110))) {
            const auto* p = spi.product(e.snoId);
            if (!p || p->payloadSno <= 0) continue;
            // No cap. The first run stopped at 1500 and reached only the four COMMON kinds —
            // mounts, markings, jewelry, headstones, portals, hairstyles, facial hair, powers and
            // dyes never appeared, so nine of the thirteen slots went unmeasured while the report
            // said UNANIMOUS four times and looked complete. A sample that silently excludes the
            // rare cases is how you get a confident answer about the wrong population.
            ++scanned;
            const QByteArray meta = m_casc ? m_casc->readMetaBySno(quint64(e.snoId)) : QByteArray();
            if (meta.size() < 8) continue;
            ++seenByKind[int(p->kind)];
            for (int off = 0; off + 4 <= meta.size(); off += 4) {
                const quint32 v = quint32(uchar(meta[off])) | quint32(uchar(meta[off + 1])) << 8
                                | quint32(uchar(meta[off + 2])) << 16 | quint32(uchar(meta[off + 3])) << 24;
                if (v == quint32(p->payloadSno)) { offByKind[int(p->kind)][off] += 1; break; }
            }
        }
        rep << QString();
        rep << QStringLiteral("── PAYLOAD SNO offsets, per product kind ──");
        rep << QStringLiteral("  a kind is only USABLE when one offset accounts for ALL of its "
                              "products; anything less is not measured, do not parse it");
        for (auto it = seenByKind.constBegin(); it != seenByKind.constEnd(); ++it) {
            const QHash<int, int>& offs = offByKind.value(it.key());
            int bestOff = -1, bestN = 0;
            for (auto o = offs.constBegin(); o != offs.constEnd(); ++o)
                if (o.value() > bestN) { bestN = o.value(); bestOff = o.key(); }
            // ── The formula, tested rather than admired ─────────────────────────────────────────
            // The first four kinds measured did not land on four unrelated offsets:
            //     Transmog(1) 0x74 · Emote(3) 0x7c · Emblem(6) 0x88 · Companion(11) 0x9c
            // all satisfy  0x74 + (kind - Transmog) * 4  exactly. That is one contiguous block of
            // thirteen u32 slots at 0x74, in the order the JSON declares the payload fields — the
            // same order this Kind enum uses.
            //
            // Which is a PREDICTION about the nine kinds that run had no samples for, so it is
            // printed as a per-kind verdict instead of being quietly assumed. FITS on every kind is
            // the result that licenses reading all thirteen; a single BREAKS means the block theory
            // is wrong and only the individually-measured offsets may be used.
            const int predicted = 0x74 + (it.key() - int(StoreProductIndex::Transmog)) * 4;
            const bool unanimous = bestN == it.value() && bestN > 0;
            rep << QStringLiteral("  %1: %2 product(s), best 0x%3 on %4 (%5 distinct) %6  formula 0x%7 %8")
                       .arg(StoreProductIndex::kindLabel(StoreProductIndex::Kind(it.key())), -12)
                       .arg(it.value())
                       .arg(bestOff < 0 ? 0 : bestOff, 0, 16).arg(bestN).arg(offs.size())
                       .arg(unanimous ? QStringLiteral("UNANIMOUS") : QStringLiteral("<-- NOT unanimous"))
                       .arg(predicted, 0, 16)
                       .arg(bestOff == predicted ? QStringLiteral("FITS") : QStringLiteral("<-- BREAKS"));
        }
    }

    // ── PHASE 5 — where the ART HANDLES sit ─────────────────────────────────────────────────────
    // A recovered bundle with no tile art is a blank card in the list, so the handles matter almost
    // as much as the children. They cannot be tallied per slot the way payloads were: the index
    // SKIPS zero handles when it builds `art`, so position 0 in that vector is a different field
    // depending on which handles the product happens to set. What can be measured is WHERE the
    // values land — twelve contiguous u32 slots would show up as twelve clustered offsets with
    // high counts, and anything else tells us the theory is wrong.
    {
        QHash<int, int> hist;   // offset -> how many art handles were found there
        int scanned = 0;
        for (const SnoEntry& e : m_index.entries(SnoIndex::groupIdByName(
                                     QStringLiteral("StoreProduct"), 110))) {
            const auto* p = spi.product(e.snoId);
            if (!p || p->art.isEmpty()) continue;
            if (++scanned > 400) break;   // a histogram converges fast; this is not a unanimity test
            const QByteArray meta = m_casc ? m_casc->readMetaBySno(quint64(e.snoId)) : QByteArray();
            if (meta.size() < 16) continue;
            for (quint32 h : p->art)
                for (int off = 0; off + 4 <= meta.size(); off += 4) {
                    const quint32 v = quint32(uchar(meta[off])) | quint32(uchar(meta[off + 1])) << 8
                                    | quint32(uchar(meta[off + 2])) << 16 | quint32(uchar(meta[off + 3])) << 24;
                    if (v == h) { hist[off] += 1; break; }
                }
        }
        QVector<QPair<int, int>> rows;
        for (auto it = hist.constBegin(); it != hist.constEnd(); ++it) rows << qMakePair(it.key(), it.value());
        std::sort(rows.begin(), rows.end(), [](const QPair<int,int>& a, const QPair<int,int>& b) {
            return a.first < b.first; });
        rep << QString();
        rep << QStringLiteral("── ART HANDLE offsets (histogram over %1 products) ──").arg(scanned);
        QStringList cells;
        for (const auto& r : rows)
            if (r.second >= 5) cells << QStringLiteral("0x%1×%2").arg(r.first, 0, 16).arg(r.second);
        rep << (cells.isEmpty() ? QStringLiteral("  (nothing recurring)")
                                : QStringLiteral("  ") + cells.join(QLatin1Char(' ')));
    }

    // ── PHASE 5b — can we recover SHOP TITLES for products d4data does not describe? ────────────
    // The remaining gap in the CASC fallback: ~1,800 recovered products show their asset name
    // ("Bundle_HArmor_bar_stor251") because the shop's display text lives in d4data's string
    // tables and those bundles have none. Whether the GAME ships that text in a form we can read
    // is unmeasured, so measure it rather than assume either way.
    //
    // The question in three parts, answered against products whose title we ALREADY know from
    // d4data (so there is a right answer to check against):
    //   1. does a StringList sno named "StoreProduct_<name>" exist in CoreTOC at all?
    //   2. does it have a readable payload (or is it encrypted / absent)?
    //   3. does that payload visibly CONTAIN the known title as text?
    // Only if all three hold is a parser worth writing.
    {
        const int slGroup = SnoIndex::groupIdByName(QStringLiteral("StringList"), 42);
        QHash<QString, int> slByName;
        for (const SnoEntry& e : m_index.entries(slGroup))
            slByName.insert(e.name.toLower(), e.snoId);
        int checked = 0, named = 0, withPayload = 0, titleFound = 0, encrypted = 0;
        QStringList examples;
        for (int bsno : spi.bundles()) {
            const auto* p = spi.product(bsno);
            if (!p || p->title.isEmpty()) continue;      // need a known-good answer to compare to
            if (++checked > 400) break;
            const QString want = QStringLiteral("storeproduct_%1").arg(p->name.toLower());
            const int slSno = slByName.value(want, 0);
            if (!slSno) continue;
            ++named;
            const QByteArray pay = m_casc->readPayloadBySno(quint64(slSno));
            const QByteArray met = m_casc->readMetaBySno(quint64(slSno));
            const QByteArray blob = pay.isEmpty() ? met : pay;
            if (blob.isEmpty()) {
                if (!m_casc->tactKeyFor(quint64(slSno)).isEmpty()) ++encrypted;
                continue;
            }
            ++withPayload;
            // UTF-8 and UTF-16 both, because a string table could be either and "not found" for
            // the wrong encoding would read as "the text is not there".
            const bool hit = blob.contains(p->title.toUtf8())
                          || blob.contains(QByteArray(reinterpret_cast<const char*>(
                                 p->title.utf16()), p->title.size() * 2));
            if (hit) ++titleFound;
            if (examples.size() < 8)
                examples << QStringLiteral("    %1 → sl %2, %3 B, title %4")
                                .arg(p->name).arg(slSno).arg(blob.size())
                                .arg(hit ? QStringLiteral("FOUND") : QStringLiteral("not visible"));
        }
        rep << QString();
        rep << QStringLiteral("── SHOP TITLES from CASC StringList ──");
        rep << QStringLiteral("  checked %1 titled bundle(s)").arg(checked);
        rep << QStringLiteral("  StringList sno exists   %1").arg(named);
        rep << QStringLiteral("  readable blob           %1").arg(withPayload);
        rep << QStringLiteral("  encrypted, no key       %1").arg(encrypted);
        rep << QStringLiteral("  title visible in blob   %1").arg(titleFound);
        if (!examples.isEmpty()) { rep << QStringLiteral("  samples:"); rep += examples; }
        // The three outcomes are NOT interchangeable, and conflating them sent this enquiry down a
        // wrong path once already: "no readable blob" was reported as "the text is not in the
        // record", when the real cause was that we never indexed the archive holding it.
        const bool localeOn = QSettings().value(QStringLiteral("casc/includeLocalePacks"), false)
                                  .toBool();
        rep << QStringLiteral("  locale packs indexed: %1").arg(localeOn ? "YES" : "NO (default)");
        rep << QStringLiteral("  VERDICT: %1")
                   .arg(titleFound > 0
                            ? QStringLiteral("titles ARE in CASC — derive the .stl layout and read them")
                        : named == 0
                            ? QStringLiteral("no StoreProduct_* StringList in CoreTOC — the shop text "
                                             "is not in the client under that name")
                        : withPayload == 0 && !localeOn
                            ? QStringLiteral("the records EXIST but none has a readable blob, and "
                                             "locale packs are NOT indexed — localized text lives in "
                                             "those packs. Enable Settings > \"Include locale packs\", "
                                             "let it re-index, and re-run before concluding anything")
                        : withPayload == 0
                            ? QStringLiteral("the records exist, locale packs ARE indexed, and still "
                                             "no readable blob — the payload is elsewhere or gated")
                            : QStringLiteral("blobs are readable but the title is not a plain "
                                             "substring — needs the .stl layout"));
    }

    // ── PHASE 6 — raw hexdump of the header ─────────────────────────────────────────────────────
    // The point of dumping bytes rather than measuring one more field: everything above answers a
    // question I already knew to ask. This is here so the NEXT question can be answered from the
    // same run instead of another rebuild — class mask, flags, string offsets, whatever turns out
    // to matter. Three products of different kinds, because a field that is zero in one and set in
    // another is exactly how you spot it.
    {
        rep << QString();
        rep << QStringLiteral("── HEADER HEXDUMP (first 0x280 bytes) ──");
        rep << QStringLiteral("  known: 0x2c = child array byte-size · 0x74..0xa4 = 13 payload sno "
                              "slots · 0x260 = child array data");
        int dumped = 0;
        for (int bsno : spi.bundles()) {
            if (dumped >= 3) break;
            const auto* p = spi.product(bsno);
            if (!p) continue;
            const QByteArray meta = m_casc ? m_casc->readMetaBySno(quint64(bsno)) : QByteArray();
            if (meta.size() < 0x280) continue;
            ++dumped;
            rep << QString();
            rep << QStringLiteral("  %1 [%2] kind=%3 children=%4 art=%5")
                       .arg(p->name).arg(bsno)
                       .arg(StoreProductIndex::kindLabel(p->kind))
                       .arg(p->children.size()).arg(p->art.size());
            for (int off = 0; off < 0x280; off += 16) {
                QString hex, asc;
                for (int i = 0; i < 16; ++i) {
                    const uchar c = uchar(meta[off + i]);
                    hex += QStringLiteral("%1 ").arg(c, 2, 16, QLatin1Char('0'));
                    asc += (c >= 32 && c < 127) ? QChar(c) : QLatin1Char('.');
                }
                rep << QStringLiteral("    %1  %2 %3").arg(off, 4, 16, QLatin1Char('0')).arg(hex, asc);
            }
        }
    }

    const QString path = AppPaths::file(QStringLiteral("prd_probe.txt"));
    QFile pf(path);
    if (pf.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        pf.write(rep.join(QLatin1Char('\n')).toUtf8());
    qInfo().noquote() << "prd-probe: wrote" << path << "— exiting";
    QCoreApplication::exit(0);
}

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

    // 5b · What the snapshot is actually MISSING.
    //
    // The d4data row above says the versions differ; this says what that costs. CoreTOC comes from
    // the game and is always current, so the gap between "how many of these the game has" and "how
    // many the snapshot describes" IS the missing content — and it is the honest answer to the two
    // questions that otherwise arrive as bug reports: why a shop bundle is absent from the
    // Catalogue, and why a brand-new item has no icon.
    //
    // Counted, not estimated. Two directory scans on a manually-opened dialog that already sets a
    // wait cursor is an acceptable price for a number nobody has to guess at.
    if (d4Ok && idxOk) {
        auto snapCount = [](const char* sub, const char* glob) {
            int n = 0;
            QDirIterator c(Config::d4dataDir() + QStringLiteral("/json/base/meta/")
                               + QLatin1String(sub),
                           QStringList{QString::fromLatin1(glob)}, QDir::Files);
            while (c.hasNext()) { c.next(); ++n; }
            return n;
        };
        struct Probe { const char* label; const char* dir; const char* glob;
                       const char* group; int fallback; };
        static const Probe kProbe[] = {
            {"shop products", "StoreProduct", "*.prd.json", "StoreProduct", 110},
            {"items",         "Item",         "*.itm.json", "Item",          73},
        };
        QStringList parts;
        int worst = 0;
        for (const Probe& p : kProbe) {
            const int g    = SnoIndex::groupIdByName(QString::fromLatin1(p.group), p.fallback);
            const int live = m_index.entries(g).size();
            const int snap = snapCount(p.dir, p.glob);
            const int gap  = live - snap;
            if (gap > 0) worst = 1;
            parts << QStringLiteral("%1 %2 in game, %3 described%4")
                         .arg(QString::fromLatin1(p.label)).arg(live).arg(snap)
                         .arg(gap > 0 ? QStringLiteral(" — %1 NOT in the snapshot").arg(gap)
                                      : QString());
        }
        addRow(QStringLiteral("Snapshot coverage"), worst, parts.join(QStringLiteral("  ·  ")));
    }

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
    // Group 110 (StoreProduct) → the Catalogue. Found by NAME rather than by a hardcoded index:
    // the Catalogue is a LazyTab and sits after three others, so an index would break the moment
    // the tab order changed. setCurrentIndex realises the lazy widget, which is why the cast
    // happens after it and via currentWidget().
    if (group == 110) {
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (m_tabs->tabText(i) != QLatin1String("Catalogue")) continue;
            m_tabs->setCurrentIndex(i);
            QWidget* cw = m_tabs->currentWidget();
            // refresh() FIRST: for a lazy tab that also builds the real widget, which the lookup
            // below depends on.
            if (!m_refreshed.contains(cw)) {
                m_refreshed.insert(cw);
                static_cast<BrowserTab*>(cw)->refresh();
            }
            // The page widget is the LazyTab PROXY, not the tab itself — the real CatalogueTab is
            // a child of it. Casting the page directly always yielded null, so the jump switched
            // tabs and then quietly did nothing. Try both, so this keeps working if the tab is
            // ever added eagerly.
            CatalogueTab* cat = qobject_cast<CatalogueTab*>(cw);
            if (!cat) cat = cw->findChild<CatalogueTab*>();
            if (cat) cat->revealBundle(sno);
            else setStatus(QStringLiteral("Catalogue is still loading — try the link again."));
            return;
        }
        return;
    }
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

    // ── Desktop notification ────────────────────────────────────────────────────────────────
    // The toast above is inside the window, so it is invisible precisely when it matters most:
    // a bulk extract runs for minutes and people switch away.
    //
    // Fires whether or not the window is focused. It was originally gated on !isActiveWindow(),
    // reasoning that the in-window toast already covers the focused case — but that made the
    // feature look broken to anyone testing it with the app in front of them, and "did my export
    // finish?" is worth answering twice rather than zero times.
    if (!QSettings().value(QStringLiteral("export/osNotify"), true).toBool()) return;
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        qWarning("export notification: no system tray available on this desktop — skipped");
        return;
    }
    if (!m_tray) {
        // The icon must be non-null AND shown, or showMessage() is silently dropped on Windows.
        // windowIcon() can be null before the window is fully constructed, so fall back to a
        // standard icon rather than creating a tray entry that never appears.
        QIcon ic = windowIcon();
        if (ic.isNull()) ic = style()->standardIcon(QStyle::SP_DialogSaveButton);
        m_tray = new QSystemTrayIcon(ic, this);
        m_tray->setToolTip(QStringLiteral("D4AssetBrowser"));
        m_tray->show();   // created lazily, so turning the setting off means no tray entry ever
    }
    if (!m_tray->isVisible()) m_tray->show();
    m_tray->showMessage(QStringLiteral("D4AssetBrowser — export finished"), text,
                        QSystemTrayIcon::Information, 8000);
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

    // Every index that can report, wired the same way: progress lands in m_idxPct under the roster
    // name, readyChanged just re-reads. Previously only the first three were connected, so the
    // indicator's "Indexes ✓" meant "those three are idle" and said nothing about the other six.
    // Coalesced: seven indexes each emitting a whole-percent tick would otherwise rebuild the
    // roster and re-render two HTML tooltips ~1400 times over a full index. The percentage is
    // stored immediately; the repaint is batched onto the next 150 ms tick.
    auto tick = [this](const QString& name) {
        return [this, name](int p) {
            m_idxPct.insert(name, p);
            if (m_idxUiPending) return;
            m_idxUiPending = true;
            QTimer::singleShot(150, this, [this] {
                m_idxUiPending = false;
                // Only re-label the menu when it is actually on screen; otherwise aboutToShow does it.
                if (m_indexMenu && m_indexMenu->isVisible()) refreshIndexMenu();
                refreshIndexIndicator();
            });
        };
    };
    auto done = [this] { refreshIndexMenu(); refreshIndexIndicator(); };

    connect(&AppearanceMeta::instance(),    &AppearanceMeta::progress,    this, tick(QStringLiteral("Metadata")));
    connect(&AppearanceMeta::instance(),    &AppearanceMeta::readyChanged, this, done);
    connect(&IconIndex::instance(),         &IconIndex::progress,         this, tick(QStringLiteral("Icons")));
    connect(&IconIndex::instance(),         &IconIndex::readyChanged,     this, done);
    connect(&AssetLinks::instance(),        &AssetLinks::progress,        this, tick(QStringLiteral("Asset links")));
    connect(&AssetLinks::instance(),        &AssetLinks::readyChanged,    this, done);
    connect(&ItemHoverIndex::instance(),    &ItemHoverIndex::progress,    this, tick(QStringLiteral("Item hover")));
    connect(&ItemHoverIndex::instance(),    &ItemHoverIndex::readyChanged, this, done);
    connect(&BackTrophyIndex::instance(),   &BackTrophyIndex::progress,   this, tick(QStringLiteral("Back trophies")));
    connect(&BackTrophyIndex::instance(),   &BackTrophyIndex::readyChanged, this, done);
    connect(&WardrobeAnimIndex::instance(), &WardrobeAnimIndex::progress, this, tick(QStringLiteral("Wardrobe animations")));
    connect(&WardrobeAnimIndex::instance(), &WardrobeAnimIndex::readyChanged, this, done);
    connect(&StoreProductIndex::instance(), &StoreProductIndex::progress, this, tick(QStringLiteral("Store products")));
    connect(&StoreProductIndex::instance(), &StoreProductIndex::readyChanged, this, done);
    // Auto-regenerate icon_audit.txt once indexing completes (whichever of the two finishes last).
    connect(&AppearanceMeta::instance(), &AppearanceMeta::readyChanged, this, &MainWindow::autoIconAudit);
    connect(&IconIndex::instance(), &IconIndex::readyChanged, this, &MainWindow::autoIconAudit);
    refreshIndexIndicator();
}

void MainWindow::refreshIndexIndicator()
{
    if (!m_idxLabel) return;

    // Reads the same roster the menu does, so the indicator now covers every index instead of the
    // three it used to watch. "Indexes ✓" previously meant "those three are idle" — it appeared
    // while six others had never been built, which is the reason indexing state felt unknowable.
    const auto roster = indexRoster();
    QString rows;
    int running = 0, doneCount = 0;
    QString phase; int phasePct = -1;

    for (const IndexDesc& d : roster) {
        const bool rdy = d.ready && d.ready();
        if (rdy) m_idxKicked.remove(d.name);
        const bool bld = idxRunning(d);
        const int  pct = m_idxPct.value(d.name, -1);

        if (bld) {
            if (!running) { phase = d.name; phasePct = (d.hasPct ? pct : -1); }
            ++running;
        }
        if (rdy) ++doneCount;

        // ⟳ NN% while running (or a bare ⟳ when that index reports no percentage), ✓ when built,
        // a dash when it has not been started — the same three states the menu shows.
        QString state;
        if (bld)      state = (d.hasPct && pct >= 0) ? QStringLiteral("⟳ %1%").arg(qBound(0, pct, 100))
                                                     : QStringLiteral("⟳ building");
        else if (rdy) state = QStringLiteral("✓ done");
        else          state = QStringLiteral("— not started");
        rows += QStringLiteral("<tr><td><b>%1</b></td><td>&nbsp;%2&nbsp;</td>"
                               "<td style='color:#999'>%3</td></tr>").arg(d.name, state, d.what);
    }

    const QString tip = QStringLiteral(
        "<div style='max-width:380px'>"
        "<b>Background indexing</b> — %1 of %2 built<br>"
        "One-time scans that build the lookup tables every tab shares. They run "
        "after a game patch or a d4data update; the app is fully usable meanwhile.<br>"
        "Start or re-run any of them from <b>File ▸ Index</b>.<hr>"
        "<table cellspacing='0'>%3</table></div>")
        .arg(doneCount).arg(roster.size()).arg(rows);
    m_idxLabel->setToolTip(tip);

    if (running > 0) {
        const QString pctTxt = phasePct >= 0 ? QStringLiteral(" %1%").arg(qBound(0, phasePct, 100))
                                             : QString();
        // Name the extra ones rather than hiding them: "Icons 42%" alone read as the only work left.
        const QString more = running > 1 ? QStringLiteral(" (+%1 more)").arg(running - 1) : QString();
        m_idxGlobalMsg = QStringLiteral("Indexing %1%2%3").arg(phase, pctTxt, more);
    } else {
        m_idxGlobalMsg.clear();
    }

    if (m_idxStatus) {
        m_idxStatus->setToolTip(tip);
        if (running > 0) {
            m_idxStatus->setStyleSheet(QStringLiteral("color:#e8c46a;"));
            m_idxStatus->setText(QStringLiteral("⟳ %1").arg(m_idxGlobalMsg));
        } else if (doneCount == roster.size()) {
            m_idxStatus->setStyleSheet(QStringLiteral("color:#6f9f6f;"));
            m_idxStatus->setText(QStringLiteral("Indexes ✓"));
        } else {
            // Idle but incomplete — the state the old indicator could not express at all.
            m_idxStatus->setStyleSheet(QStringLiteral("color:#999;"));
            m_idxStatus->setText(QStringLiteral("Indexes %1/%2").arg(doneCount).arg(roster.size()));
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
    {   // sorted by key so the order does not shuffle as tabs come and go
        QStringList keys = m_idxTabMsgs.keys();
        keys.sort();
        for (const QString& k : keys)
            if (!m_idxTabMsgs.value(k).isEmpty()) parts << m_idxTabMsgs.value(k);
    }
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
