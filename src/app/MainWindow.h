#pragma once
#include "index/SnoIndex.h"

#include <QMainWindow>
#include <QSet>
#include <memory>

class QTabWidget;
class QLabel;
class QWidget;
class QProgressBar;
class QTimer;
class QGraphicsOpacityEffect;
class QAction;
class QDialog;
class CascReader;
class BrowserTab;
class ConsoleWindow;

// Top-level window: a QTabWidget with the four core modules (Files, String Lists,
// Textures, Models), Ctrl+1..9 module shortcuts, a File menu (Settings / Reload),
// and a status bar — the same shape as the Python fork and d4analyzer.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;   // persist window geometry/state
    void resizeEvent(QResizeEvent* e) override;  // keep the floating indexing toast centred
    bool eventFilter(QObject* obj, QEvent* ev) override;   // Ctrl+K palette: arrows steer the list

private slots:
    void openSettings();
    void toggleConsole();
    void reload();                      // async: heavy I/O on a worker → finishReload on the GUI
    void onTabChanged(int index);
    void runIconAudit();   // hidden diagnostic: diablo4.dad cross-check → icon_audit.txt
    void autoIconAudit();  // one-shot: regenerate icon_audit.txt once indexing finishes (no dialog)

    // Export menu.
    void exportActiveSelection();       // active tab → export its selection (prompt)
    void exportActiveSelectionToLast(); // active tab → re-export to the last folder
    void exportActiveAnimations();      // active tab → rig-only animation export (Models)
    void exportActiveFramesSelected();      // active tab → export selected TexFrame(s) (Textures)
    void exportActiveFramesSelectedLast();  // active tab → selected TexFrame(s) → last dir
    void exportActiveFramesAll();           // active tab → export all TexFrames of the texture (Textures)
    void exportActiveFramesAllLast();       // active tab → all TexFrames → last dir
    void captureActiveImage();          // active preview → PNG/JPEG
    void captureActiveTurntable();      // active preview → 360° turntable GIF
    void captureActiveAnimLoop();       // active preview → looping GIF of the playing animation
    void openExportSettings();          // consolidated export options dialog
    void updateExportMenu();            // enable/disable actions for the active tab
    void applyHotkeys();                // (re)assign export/preview action shortcuts from settings

private:
    // Async reload plumbing (reload() itself is the slot above): the worker hands this back,
    // finishReload runs on the GUI thread. Plain members — moc must not see them in a slots
    // section (anything but a function declaration there breaks the moc parse).
    struct ReloadResult {
        bool  cascOk = false, idx = false;
        int   nKeys = 0;
        qint64 tKeys = 0, tOpen = 0, tIndex = 0;
        QString idxSrc;
    };
    void finishReload(const ReloadResult& r);   // GUI-thread tail: status, fingerprint, tabs
    bool m_reloading = false;           // a reload worker is in flight (guards re-entry + close)
    bool m_reloadPending = false;       // reload() called mid-flight → run again when done

    void buildMenu();
    void buildTabs();
    void buildShortcuts();
    void showShortcutSheet();           // Help ▸ Shortcuts (F1 / ?) — modeless cheat-sheet
    QDialog* m_shortcutSheet = nullptr; // open sheet (F1 toggles; WA_DeleteOnClose nulls this)
    void setStatus(const QString& msg);
    BrowserTab* activeTab() const;      // the currently-selected module tab (or null)
    void buildIndexIndicator();   // build the floating top-centre "Indexing…" toast (all tabs share it)
    void refreshIndexIndicator(); // recompute phase/percent from the index singletons
    void showExportToast(const QString& text, const QString& folder);  // unified export confirmation
    void positionExportToast();
    void updateStalenessWarning();   // game-vs-d4data build compare + missing TACT keys + tested-build
    void showHealthCheck();          // Help ▸ Health check — one screen: what works, what's stale

    // ── Global navigation: Ctrl+K jump palette + Alt+Left/Right history ──
    void showJumpPalette();          // Ctrl+K: search models+textures by name/SNO, jump on Enter
    void jumpTo(int group, int sno, bool record = true);   // switch tab + select; record = push history
    void navRecord();                // push the current (tab, sno) onto the back stack
    void navGo(bool back);           // Alt+Left (back) / Alt+Right (forward)
    struct NavLoc { int tab = -1; int sno = -1; };
    NavLoc currentNavLoc() const;    // (tab index, selected SNO on the addressable tabs)
    QVector<NavLoc> m_navBack, m_navFwd;
    class QFrame*      m_jump     = nullptr;   // the palette popup (lazy-built)
    class QLineEdit*   m_jumpEdit = nullptr;
    class QListWidget* m_jumpList = nullptr;
    void updateToast();           // compose global + active-tab scan messages → show/hide the toast
    void positionToast();         // re-centre the toast at the top of the window

    std::unique_ptr<CascReader> m_casc;
    SnoIndex    m_index;
    QTabWidget* m_tabs   = nullptr;
    QLabel*     m_status = nullptr;
    QLabel*     m_idxStatus = nullptr;    // persistent right-aligned index-build indicator (status bar)
    QLabel*     m_staleWarn = nullptr;    // data-staleness / missing-keys warning (status bar, gold)
    ConsoleWindow* m_console = nullptr;   // lazy-created log console (File ▸ Toggle)
    QSet<QWidget*> m_refreshed;

    // Floating, tab-independent indexing toast pinned to the top-centre of the window. It overlays
    // content (never reflows the UI) and merges every source: the global meta/icon/link build plus
    // the active tab's background scans (via BrowserTab scan-status signals).
    QLabel*              m_idxLabel = nullptr;
    QGraphicsOpacityEffect* m_idxFade = nullptr;
    QTimer*              m_idxHideTimer = nullptr;
    QString              m_idxGlobalMsg;   // meta/icon/link build status
    QString              m_idxTabMsg;      // active tab's scan status
    QLabel*              m_exportToast = nullptr;    // unified bottom-centre export confirmation
    QTimer*              m_exportToastTimer = nullptr;
    int m_metaPct = 0, m_iconPct = 0, m_linkPct = 0;
    bool m_iconAuditRan = false;   // guard: auto icon-audit runs once per launch
    bool m_setupPrompted = false;  // guard: first-run "open Settings?" prompt shows once per launch

    // Export-menu actions (enable state tracks the active tab's capabilities).
    QAction* m_actExportSel  = nullptr;
    QAction* m_actExportLast = nullptr;
    QAction* m_actSaveImg    = nullptr;
    QAction* m_actTurntable  = nullptr;
    QAction* m_actAnimLoop   = nullptr;
    QAction* m_actExportAnims = nullptr;
    QAction* m_actExportFramesSel = nullptr;      // Textures: export selected TexFrame(s)
    QAction* m_actExportFramesSelLast = nullptr;  // Textures: selected TexFrame(s) → last dir
    QAction* m_actExportFramesAll = nullptr;      // Textures: export all TexFrames of the texture
    QAction* m_actExportFramesAllLast = nullptr;  // Textures: all TexFrames → last dir
};
