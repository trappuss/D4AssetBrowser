#pragma once
#include "tabs/BrowserTab.h"
#include <QVector>
#include <QPair>
#include <QSet>
#include <QHash>
#include <QString>
#include <atomic>

class ModelsTab;
class TexturesTab;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QRadioButton;
class QLabel;
class QListWidget;
class QPushButton;
class QToolButton;
class QFrame;
class QScrollArea;
class QPlainTextEdit;
class QProgressBar;

// A focused "extract all of X" tab: filter the appearance index by name / class / item-type / gender /
// tag (real game-data tags), preview the live match count, then bulk-export every match to a folder.
// Export is delegated to ModelsTab (reusing its .glb pipeline + current Export settings). Incremental
// "only new" skipping uses each folder's _bulk_manifest.json ledger, so re-running a query after a
// game patch extracts just the new items.
class BulkExtractorTab : public BrowserTab {
    Q_OBJECT
public:
    explicit BulkExtractorTab(ModelsTab* models, TexturesTab* textures, QWidget* parent = nullptr);
    void refresh() override;   // (re)populate the tag dropdowns + count when the appearance meta lands

signals:
    // "Export settings…" — MainWindow opens the shared dialog on its Export tab. Same route the
    // Textures tab's "Options…" takes, so there is one dialog and one set of keys.
    void exportSettingsRequested();

protected:
    // Makes double-click a true *additive* toggle of a row's selection (see the .cpp for why the
    // pre-click snapshot is needed) without disturbing the rest of the Ctrl/Shift selection.
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    bool textureMode() const;   // true = extract textures (group 44), false = models (.glb, group 9)
    // Non-const: delegates to ModelsTab::queryEntries, which lazily builds indexes + caches.
    QVector<QPair<int, QString>> computeMatches();
    void updateCount();
    void buildTagPanel();       // the Blender-style filter funnel popup (parity with Models)
    void refillTagPanel();      // (re)populate its tag-group checkboxes from the Models tab
    void updateFunnelMode();    // show model tags vs texture categories per the Models/Textures mode
    void updateTagBtn();        // funnel tints while any facet/tag filter is active
    void saveFilterState();     // persist the funnel state (when "Remember filters" is on)
    void restoreFilterState();  // re-apply it at construction
    // promptDir = ask for the destination folder (else reuse the last one). The work set is the
    // queued rows in manual mode, otherwise every match — unless explicitItems is given (context menu).
    void doExtract(bool promptDir, const QVector<QPair<int, QString>>& explicitItems = {});
    QVector<QPair<int, QString>> workSet();          // queued rows (manual) else all matches
    void syncExtractButtons();                      // label + enablement from the current work set
    void syncQueue();                               // reconcile the visible selection into the queue
    void queueAllMatches();                         // queue EVERY match, including rows past the list cap
    void rebuildQueueWidget();                      // refill the queue list widget from m_queued
    void reflectQueueInList();                      // re-select the visible rows that are queued
    void saveQueue(int mode) const;                 // persist a mode's queue to settings
    // Built-in query presets (class sets, weapons, global customization). Generated from
    // ItemDef's hero-class table at runtime, so a new class extends them with no edit here.
    struct FactoryPreset {
        QString name;      // combo label
        int     mode;      // 0 = models, 1 = textures
        QString query;     // NAME-box text (may use the '|' OR syntax)
        QStringList tags;  // funnel tag selection (ANDed unless tagOr)
        bool    tagOr = false;
        // Models mode only: also decode each model's matching textures into a "textures"
        // subfolder. On for every MODEL preset — "all the files related to X" is the whole
        // point of a preset, and a preset that silently depended on a checkbox you last
        // touched days ago is not self-contained. (The checkbox now lives in Settings ▸ Export;
        // applyFactoryPreset writes the key directly, exactly as toggling it used to.)
        bool    coTex = false;
    };
    static const QVector<FactoryPreset>& factoryPresets();
    void applyFactoryPreset(const FactoryPreset& p);
    // Resolve an explicit query to matches. computeMatches() is this called with the current UI
    // state; auditPresets() is this called once per built-in. One matcher, two callers.
    QVector<QPair<int, QString>> matchesFor(bool texMode, const QString& nameText,
                                            const QSet<QString>& texCats, const QString& facet,
                                            const QSet<QString>& tags, bool tagOr, bool hideUnrend);

public:
    // Writes data\preset_audit.txt: one line per built-in preset with its match count. A preset
    // that returns 0 (a typo in a pattern) or an implausible count (a pattern too loose) is
    // otherwise indistinguishable from a working one until you extract it. Returns a one-line
    // summary for the caller to log.
    QString auditPresets();

private:
    void loadQueueForMode(int mode);                // load a mode's persisted queue into m_queued
    void writeRunReport(const QString& dir, const QVector<QPair<int, QString>>& items) const;
    void showListMenu(const QPoint& pos);           // right-click menu on the matches list
    void loadFolderManifest();               // <outDir>/_bulk_manifest.json → m_folderDone (delta)
    void refreshPresets();                   // rebuild the preset combo from settings
    void savePreset();
    void deletePreset();
    void loadPreset(const QString& name);
    QString subfolderFor(int sno) const;
    // export/folderLayout is mirrored in Settings ▸ Export ▸ Models, so this combo has to be
    // re-pointed whenever the dialog changes it — two widgets, one key, never disagreeing.
    // Re-point the mirrored layout combo when the Settings dialog changes export/folderLayout.
    void onSettingsChanged() override { syncLayoutCombo(); }
    void syncLayoutCombo();
    void updateLayoutModeAvailability();
    bool m_syncingLayout = false;   // suppress the write-back while syncLayoutCombo() sets the index     // organize: the item's tag value for the chosen group ("" = flat)

    ModelsTab*   m_models = nullptr;
    TexturesTab* m_textures = nullptr;
    QComboBox*   m_mode = nullptr;      // Models / Textures
    QLineEdit*   m_name = nullptr;      // NAME box (space = AND include, leading '-' = exclude)
    // ── The filter funnel — a faithful copy of the Models tab's single filter popup ──────────────
    QToolButton* m_tagBtn = nullptr;       // the funnel icon button (tints while active)
    QFrame*      m_tagPanel = nullptr;     // the popup (Search · OR · Hide · Special · tag groups)
    QWidget*     m_tagPanelBody = nullptr; // its scroll body (grouped tag checkboxes)
    QScrollArea* m_tagScroll = nullptr;    // scroll wrapping the model tag body (hidden in texture mode)
    QWidget*     m_specialBox = nullptr;   // MODEL-only facets (Special/Hide/OR) — hidden in texture mode
    QWidget*     m_texFilterBox = nullptr; // TEXTURE-mode category checkboxes (shown in texture mode)
    QHash<QString, QCheckBox*> m_texCatChecks;   // texture category → its checkbox
    QSet<QString> m_texCatSel;                   // selected texture categories (union; "" Latest special)
    QLineEdit*   m_tagSearch = nullptr;    // "Search tags…" — live-filters the checkbox list
    QCheckBox*   m_tagOrChk = nullptr;     // match ANY (OR) vs ALL (AND) selected tags
    QCheckBox*   m_hideChk = nullptr;      // hide un-renderable
    QCheckBox*   m_rememberChk = nullptr;  // persist + restore the filter state across sessions
    QSet<QString> m_tagFilter;             // selected tag-group tags (Category/Class/Type/Gender/…)
    QString      m_catFacet;               // the single active usage facet ("__animated__"…) or ""
    bool         m_tagOrMode = false;      // false = ALL (narrow) · true = ANY (widen)
    bool         m_hideUnrend = false;     // hide un-renderable models
    QHash<QString, QCheckBox*> m_tagChecks;      // tag value → its checkbox (Clear / restore)
    QHash<QString, QCheckBox*> m_specialChecks;  // facet data → its checkbox (mutual exclusion)
    QComboBox*   m_organize = nullptr;  // (none) / by Class / by Type → subfolders
    QComboBox*   m_preset = nullptr;    // saved query presets
    QLineEdit*   m_outDir = nullptr;
    QRadioButton* m_onlyNew = nullptr;   // skip already-extracted (incremental)
    QRadioButton* m_overwrite = nullptr; // re-extract + overwrite existing
    QCheckBox*   m_showList = nullptr;  // "Pick items manually": enables selection into the queue
    QListWidget* m_list = nullptr;      // LEFT: every matched name (select to queue in manual mode)
    QListWidget* m_queue = nullptr;     // RIGHT: the queued items to extract
    QLabel*      m_listLbl = nullptr;   // "Matches (N)" header (centered)
    QLabel*      m_queueLbl = nullptr;  // "Queue (N)" header (centered)
    bool         m_syncingQueue = false;   // guards the list↔queue mirror against re-entry
    // The queue is a PERSISTENT store (per mode), not just a mirror of the transient selection — so it
    // survives filter tweaks, mode switches and reopening the tool.
    QVector<QPair<int, QString>> m_queued;   // ordered (sno, name)
    QSet<int>    m_queuedSnos;                // membership index for m_queued
    int          m_prevMode = 0;             // to persist the outgoing mode's queue on a mode switch
    QPushButton* m_openBtn = nullptr;        // "Open folder" (last output dir)
    QLabel*      m_count = nullptr;
    QLabel*      m_lastRun = nullptr;   // persistent last-run summary
    QPushButton* m_extractSelBtn = nullptr;  // primary: extract the selection (→ last dir)
    QPushButton* m_extractToBtn = nullptr;   // extract the selection → prompt for a folder
    QPushButton* m_copyBtn = nullptr;
    QSet<int>     m_folderDone;          // snos already in the current output folder's ledger
    QSet<QString> m_folderStems;         // base names of files already in the folder (any extension)
    bool         m_loadingPreset = false;
    QSet<int>    m_preClickSel;          // selection snapshot captured at the start of a click sequence
    qint64       m_lastPressMs = 0;      // to tell a fresh press from the 2nd press of a double-click
    // ── Live run console (real-time progress + reasoned failures + working Cancel) ──
    void logLine(const QString& s);      // timestamped append + auto-scroll
    QPlainTextEdit* m_console   = nullptr;
    QProgressBar*   m_progress  = nullptr;
    QPushButton*    m_cancelBtn = nullptr;
    QPushButton*    m_pauseBtn  = nullptr;         // Pause/Resume toggle (workers block while paused)
    bool m_running = false;                        // an extract run is in flight (guards re-entry)
    std::atomic<bool> m_cancelRequested{false};    // Cancel clicked — polled by the worker between items
    std::atomic<bool> m_pauseRequested{false};     // Pause on — workers block in the canceled-poll
    qint64 m_pausedMs   = 0;                       // accumulated pause time (excluded from the ETA)
    qint64 m_pauseT0    = 0;                       // msecs-since-epoch when the current pause began
};
