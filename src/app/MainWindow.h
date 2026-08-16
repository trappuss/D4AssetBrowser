#pragma once
#include "index/SnoIndex.h"

#include <QHash>
#include <QMainWindow>
#include <QSet>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

class QTabWidget;
class QLabel;
class QWidget;
class QProgressBar;
class QTimer;
class QGraphicsOpacityEffect;
class QAction;
class QMenu;
class QDialog;
class QSystemTrayIcon;
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
    void indexAll();                        // File ▸ Index ▸ Index all — kick every background index at once
    void exportActiveAllFiltered();         // active tab → everything its filter matches (Catalogue)
    void exportActiveAllFilteredLast();     // …to the remembered folder
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
        qint64 tKeys = 0, tOpen = 0, tIndex = 0, tAnimAction = 0;
        int    nMatNames = 0;   // material name->SNO entries wired into MaterialDecode
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

    // ── File ▸ Index ────────────────────────────────────────────────────────────────────────────
    // ONE roster drives both the submenu and the status-bar indicator, so the two can never
    // disagree about what is running. The old indicator watched three of the nine indexes and
    // showed "Indexes ✓" whenever those three happened to be idle — whether or not anything had
    // actually been built — which is why indexing state was impossible to trust.
    struct IndexDesc {
        QString name;                     // menu label and m_idxPct key — must be unique
        QString what;                     // one-line description, shown in the indicator tooltip
        std::function<bool()> ready;      // built and usable
        std::function<bool()> building;   // null if the index cannot report it (see m_idxKicked)
        std::function<void()> start;      // ensureBuilt — a no-op if ready or already in flight
        std::function<void()> reset;      // drop memory + disk cache; null if it cannot be dropped
        bool hasPct = false;              // whether it emits progress(); false → "building…"
    };
    QVector<IndexDesc> indexRoster();
    bool idxRunning(const IndexDesc& d) const;   // building(), or the m_idxKicked fallback
    void buildIndexMenu(QMenu* fileMenu);
    void refreshIndexMenu();          // re-label every row from live state (on aboutToShow)
    void indexRefreshAll();           // File ▸ Index ▸ Re-index everything (drops caches first)
    QMenu*             m_indexMenu = nullptr;
    QVector<QAction*>  m_indexActions;   // parallel to indexRoster(), same order
    QHash<QString,int> m_idxPct;         // roster name → last reported %, -1/absent = unknown
    // Indexes with no building() of their own (they are not QObjects): we started them, so we know.
    QSet<QString>      m_idxKicked;
    bool               m_idxUiPending = false;   // a coalesced indicator repaint is queued
    // Bumped on every Animation-actions kick so a completion callback from an older run cannot
    // clear a newer run's in-flight mark (reachable via Index all → Re-index everything).
    int                m_animKickGen = 0;
    void showExportToast(const QString& text, const QString& folder);  // unified export confirmation
    void positionExportToast();
    void updateStalenessWarning();   // game-vs-d4data build compare + missing TACT keys + tested-build
    void showHealthCheck();          // Help ▸ Health check — one screen: what works, what's stale
    void runPresetAudit();           // Help ▸ Audit bulk presets — match count per built-in preset
    // D4_DUMP_PRD=1 — measure the StoreProduct binary's child-array layout, write
    // data\prd_probe.txt, then exit. Needs StoreProductIndex ready, so finishReload may defer it
    // to readyChanged. Unattended: "Dump StoreProduct Layout.bat" waits on the process.
    void dumpPrdProbe();

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
    QString              m_idxGlobalMsg;   // whichever roster index is building
    // Per-SOURCE scan status, not one shared string. It was a single QString while only the Models
    // tab fed it; the Catalogue's index build now feeds it too, and two tabs scanning at startup
    // would have taken turns overwriting each other's message.
    QHash<QString, QString> m_idxTabMsgs;   // source key -> its scan status
    QLabel*              m_exportToast = nullptr;    // unified bottom-centre export confirmation
    QTimer*              m_exportToastTimer = nullptr;
    // Desktop notification for exports that finish while the window is in the background. Created
    // on first use only, so turning the setting off means no tray entry ever appears.
    QSystemTrayIcon*     m_tray = nullptr;
    // (m_metaPct/m_iconPct/m_linkPct retired — every index's percent now lives in m_idxPct,
    //  keyed by roster name, so adding an index does not mean adding another member here.)
    bool m_iconAuditRan = false;   // guard: auto icon-audit runs once per launch
    // The auto icon-audit runs on a worker thread and READS m_index + AppearanceMeta, both of
    // which reload() rebuilds. Atomic because reload() (GUI thread) tests it while the worker
    // clears it. reload() defers rather than racing; see MainWindow::reload.
    std::atomic<bool> m_iconAuditRunning{false};
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
    QAction* m_actExportAllFiltered = nullptr;     // Catalogue: every bundle the filter matches
    QAction* m_actExportAllFilteredLast = nullptr; // …to the remembered folder
};
