#include "tabs/BulkExtractorTab.h"

#include "app/ExportNotifier.h"
#include "util/PanelPersist.h"
#include "util/QueryTerm.h"   // '|' OR-alternatives — shared with SnoListModel/ModelsTab
#include "app/AppPaths.h"     // preset_audit.txt lands in data\, never beside the exe
#include "index/ItemDef.h"    // canonical hero-class table — factory presets derive from it

#include <thread>

#include "tabs/ModelsTab.h"
#include "tabs/TexturesTab.h"
#include "casc/CascReader.h"
// Folder layout for a multi-model run — the same helper the Models tab batch paths use.
#include "util/ExportLayout.h"
// TextureCacheScope — one decode cache spanning every folder of a run.
#include "model/MaterialDecode.h"
#include "index/SnoIndex.h"
#include "index/AppearanceMeta.h"
#include "app/Config.h"
#include "tabs/BatchSink.h"

#include "util/ViewportPartMenu.h"
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTime>
#include <QElapsedTimer>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QDateTime>
#include <QThread>
#include <QEvent>
#include <QMouseEvent>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QStandardItemModel>   // per-item enable/disable in the layout combo
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QScrollArea>
#include <QSize>
#include <QToolButton>
#include <utility>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSplitter>
#include <QStringList>
#include <QTabWidget>
#include <QTextStream>
#include <QKeySequence>
#include <QShortcut>
#include <QUrl>
#include <QVBoxLayout>

namespace {
// Name-pinned group ids (fallbacks 9/44): survive a hypothetical SNO-group renumbering — only
// SnoIndex's name map would need correcting and these callsites follow automatically.
inline int kModelGroupId()   { static const int g = SnoIndex::groupIdByName(QStringLiteral("Appearance"), 9); return g; }
inline int kTextureGroupId() { static const int g = SnoIndex::groupIdByName(QStringLiteral("Texture"), 44); return g; }
#define kModelGroup kModelGroupId()
#define kTextureGroup kTextureGroupId()

// Folder name for a model in by-Model mode: the .glb's own stem, so the folder and the file inside
// it always agree. NameTemplate does its own path-illegal stripping, so the result is safe to use
// as a directory name; the raw index name is the fallback if a template resolves to nothing.
}

bool BulkExtractorTab::textureMode() const { return m_mode && m_mode->currentIndex() == 1; }

BulkExtractorTab::BulkExtractorTab(ModelsTab* models, TexturesTab* textures, QWidget* parent)
    : BrowserTab(parent), m_models(models), m_textures(textures)
{
    // ── Layout: four tight rows (what · query · where/how · options), then the run bar and the
    // console. Everything export-related is IN the tab — no trips to Settings mid-workflow.
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(6);

    // Row 1 — what to extract + presets.
    auto* modeRow = new QHBoxLayout();
    modeRow->setSpacing(6);
    m_mode = new QComboBox(this);
    m_mode->addItem(QStringLiteral("Models (.glb)"));
    m_mode->addItem(QStringLiteral("Textures (image)"));
    m_mode->setCurrentIndex(qBound(0, QSettings().value(QStringLiteral("bulk/mode"), 0).toInt(), 1));
    modeRow->addWidget(m_mode);
    // Funnel button — identical to the Models tab's: a painted funnel icon that opens ONE filter
    // popup (Special usage facets + grouped tag checkboxes). It sits right before the NAME box.
    m_tagBtn = new QToolButton(this);
    {
        QPixmap pm(14, 14);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(190, 180, 150));
        p.drawPolygon(QPolygonF({{1.5, 2.0}, {12.5, 2.0}, {8.5, 7.0}, {8.5, 12.0}, {5.5, 10.0}, {5.5, 7.0}}));
        p.end();
        m_tagBtn->setIcon(QIcon(pm));
    }
    m_tagBtn->setIconSize(QSize(14, 14));
    m_tagBtn->setFixedSize(28, kBarH);
    m_tagBtn->setStyleSheet(QString::fromLatin1(kIconBtnQss));
    m_tagBtn->setCursor(Qt::PointingHandCursor);
    m_tagBtn->setToolTip(QStringLiteral("Filter by categories & tags — select any number; "
                                        "results must match all (or any)"));
    modeRow->addWidget(m_tagBtn);
    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(QStringLiteral("NAME — space-separate terms (all match); prefix \"-\" to exclude"));
    m_name->setToolTip(QStringLiteral(
        "Filter by name. Space-separate terms (all must match); prefix a term with \"-\" to EXCLUDE it.\n"
        "\"|\" inside a term means OR:  barf_|barm_ _hlm|_trs  =  (barf_ or barm_) and (_hlm or _trs).\n"
        "e.g.  pandem -destroyed -pillar    (also matches tags / collection once loaded)"));
    m_name->setClearButtonEnabled(true);
    modeRow->addWidget(m_name, 1);
    m_preset = new QComboBox(this);
    m_preset->setMinimumWidth(140);
    m_preset->setToolTip(QStringLiteral("Saved query presets"));
    modeRow->addWidget(m_preset);
    auto* savePresetBtn = new QPushButton(QStringLiteral("Save…"), this);
    auto* delPresetBtn  = new QPushButton(QStringLiteral("✕"), this);
    delPresetBtn->setToolTip(QStringLiteral("Delete the selected preset"));
    delPresetBtn->setMaximumWidth(28);
    modeRow->addWidget(savePresetBtn);
    modeRow->addWidget(delPresetBtn);
    root->addLayout(modeRow);

    // (No separate filter row — the funnel button above opens the single unified filter panel,
    // exactly like the Models tab. Build it now, and refill its tag groups when the meta lands.)
    buildTagPanel();
    connect(m_tagBtn, &QToolButton::clicked, this, [this] {
        if (!m_tagPanel) return;
        refillTagPanel();
        m_tagPanel->adjustSize();
        m_tagPanel->move(m_tagBtn->mapToGlobal(QPoint(0, m_tagBtn->height() + 2)));
        m_tagPanel->show();
        m_tagPanel->raise();
    });
    if (m_models) connect(m_models, &ModelsTab::filtersChanged, this,
                          [this] { refillTagPanel(); updateCount(); });

    // Row 3 — the run's EXPORT OPTIONS, right here in the tab. These write the same settings
    // keys the export pipeline reads (Settings ▸ Export stays the home of the deeper options).
    auto* optRow = new QHBoxLayout();
    optRow->setSpacing(10);
    // Manual-pick controls live here (left of "Include:") to save a row. "Pick items manually" enables
    // selecting matches into the queue; Select all / none act on the left list.
    m_showList = new QCheckBox(QStringLiteral("Pick items manually"), this);
    m_showList->setChecked(QSettings().value(QStringLiteral("bulk/showList"), false).toBool());
    m_showList->setToolTip(QStringLiteral(
        "Off: every match is extracted.  On: select matches on the left to build the extract queue "
        "on the right (click, Ctrl/Shift-click, double-click to toggle, Ctrl+A for all)."));
    auto* allBtn  = new QPushButton(QStringLiteral("Select all"), this);
    auto* noneBtn = new QPushButton(QStringLiteral("Select none"), this);
    allBtn->setMaximumWidth(88); noneBtn->setMaximumWidth(94);
    optRow->addWidget(m_showList);
    optRow->addWidget(allBtn);
    optRow->addWidget(noneBtn);
    { auto* sep = new QLabel(QStringLiteral("│"), this); sep->setStyleSheet(QStringLiteral("color:#4a4a4a;")); optRow->addWidget(sep); }
    // One button instead of four checkboxes.
    //
    // "Textures", "All animations", "Pulled anims" and "Raw sources" were pure mirrors of
    // Settings ▸ Export keys — a second place to set the same thing, which meant two widgets could
    // disagree about one value and a run could be governed by whichever was touched last. The
    // deeper options (which animation SOURCES, base body/head, retarget, file-name templates)
    // never had a mirror here anyway, so half the export configuration was already only reachable
    // through Settings. Now all of it is, and this button is the shortcut.
    auto* exSetBtn = new QPushButton(QStringLiteral("Export settings…"), this);
    exSetBtn->setToolTip(QStringLiteral(
        "Open Settings ▸ Export — what goes into each exported file: textures, animation sources, "
        "base body/head, file-name templates, retarget options.\n\n"
        "Bulk Extract's own three preferences — loose textures, raw game buffers and the run "
        "report — are on that dialog's \"Wardrobe, Catalogue & Bulk\" page.\n\n"
        "What is left out here belongs to THIS run and nothing else: the worker count to the right, "
        "and the output folder, layout and Only-new/Overwrite choice on the row below."));
    connect(exSetBtn, &QPushButton::clicked, this, [this] { emit exportSettingsRequested(); });
    optRow->addWidget(exSetBtn);
    { auto* sep = new QLabel(QStringLiteral("│"), this); sep->setStyleSheet(QStringLiteral("color:#4a4a4a;")); optRow->addWidget(sep); }
    // "Buffers" and "Write report" moved to Settings ▸ Export ▸ Wardrobe, Catalogue & Bulk. Neither
    // was ever run-scoped — both persisted the instant you ticked them, so a checkbox sitting under
    // a "This run:" label was governing every future run too. Their keys (bulk/buffers, bulk/report)
    // are unchanged and still read below.
    //
    // "Loose textures" (bulk/coTextures) moved with them, and its PASS below is unchanged. It was
    // nearly cut as a duplicate of Settings ▸ Export ▸ Models ▸ "Also write textures to a textures
    // folder" — both write into <out>/textures/ — but they are not the same mechanism and the
    // export one cannot replace this one:
    //   • it selects by texture NAME PREFIX over the whole Texture group, so it catches recolour
    //     variants, atlases and sheets no default-look material binds; the export path writes only
    //     the maps the palette actually references;
    //   • it runs over every MATCH, so it still fills textures/ on an "Only new" re-run where every
    //     .glb is skipped and writeLooseTextures() is never reached at all;
    //   • it does not need export/includeTex, and it honours tex/format (JPEG), which the export
    //     path hardcodes to PNG;
    //   • it keeps the game's own texture name, so an output maps back to its SNO.
    // Four factory presets depend on it — "All Weapons" exists precisely because weapon textures
    // are named after their model and this prefix match is the only route the data supports.
    // The filenames differ (<texname> vs <model>_<material>_<role>), so both can be on at once.
    //
    // With the three checkboxes gone the row's "This run:" label went too: it introduced a list
    // that is now one combo, which already carries its own label. The genuinely run-scoped controls
    // — Only new / Overwrite and the Flat/Class/Type layout — were never on this row anyway; they
    // sit on the destination row below, next to the folder they act on.
    // Parallel decode workers (texture runs). Models stay serial — their pipeline shares more state.
    {
        auto* pLbl = new QLabel(QStringLiteral("Parallel:"), this);
        pLbl->setStyleSheet(QStringLiteral("color:#888;"));
        auto* par = new QComboBox(this);
        par->addItem(QStringLiteral("Auto"), -1);   // = CPU core count at run time
        for (int n : {1, 2, 4, 8, 16}) par->addItem(QString::number(n), n);
        par->setToolTip(QStringLiteral(
            "Concurrent workers for texture runs and (geometry-only) model runs. Auto = CPU core "
            "count. Archive reads + BLTE decompression + BC decode + file writes all run in "
            "parallel. Output is byte-identical to a single-threaded run."));
        const int saved = QSettings().value(QStringLiteral("bulk/parallel"), -1).toInt();
        { const int i = par->findData(saved); if (i >= 0) par->setCurrentIndex(i); }
        connect(par, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [par](int) {
            QSettings().setValue(QStringLiteral("bulk/parallel"), par->currentData().toInt());
        });
        optRow->addWidget(pLbl);
        optRow->addWidget(par);
    }
    optRow->addStretch(1);
    m_count = new QLabel(this);
    m_count->setStyleSheet(QStringLiteral("color:#d8a23a;font-weight:bold;"));
    optRow->addWidget(m_count);
    root->addLayout(optRow);

    // Row 4 — the destination. No Browse button: the "Extract to…" button below prompts for a
    // folder and remembers it; this field is just a read-only readout of that last folder.
    auto* whereRow = new QHBoxLayout();
    whereRow->setSpacing(6);
    whereRow->addWidget(new QLabel(QStringLiteral("To:"), this));
    m_outDir = new QLineEdit(QSettings().value(QStringLiteral("bulk/outDir")).toString(), this);
    m_outDir->setReadOnly(true);
    m_outDir->setPlaceholderText(QStringLiteral("no folder yet — use “Extract to…”"));
    m_outDir->setToolTip(QStringLiteral("The last folder you extracted to. Change it with “Extract to…”."));
    whereRow->addWidget(m_outDir, 1);
    m_organize = new QComboBox(this);
    m_organize->addItem(QStringLiteral("Flat"),                ExportLayout::kFlat());
    m_organize->addItem(QStringLiteral("Subfolders by Class"), ExportLayout::kClass());
    m_organize->addItem(QStringLiteral("Subfolders by Type"),  ExportLayout::kType());
    m_organize->addItem(QStringLiteral("Subfolders by Model"), ExportLayout::kModel());
    // Restored by VALUE, not by index. The key holds a stable id now (see ExportLayout), so adding
    // a mode no longer silently reinterprets everyone's saved choice — and findData returning -1
    // for an unknown value falls back to Flat, which is where the user pointed.
    syncLayoutCombo();
    m_organize->setToolTip(QStringLiteral(
        "Where the run's files go.\n\n"
        "Whichever you pick, each folder gets the SAME layout inside it: the models themselves, "
        "plus a textures\\, deps\\ and buffers\\ subfolder for whichever of those you have "
        "switched on. Flat is simply one folder — the one you chose.\n\n"
        "  Flat                 everything together\n"
        "  Subfolders by Class  barbarian\\, sorceress\\, …\n"
        "  Subfolders by Type   armor\\, weapon\\, …\n"
        "  Subfolders by Model  one folder per model\n\n"
        "By Model is the one to pick when you want each asset self-contained — its .glb, its "
        "textures and its raw sources all in one place, ready to move somewhere else.\n\n"
        "Two settings widen what a By Model folder holds, because they turn one requested model "
        "into several: Settings ▸ Export ▸ \"Also export the opposite gender\" adds the twin, and "
        "the set-aware option adds the rest of the armour set. Both land in the requesting model's "
        "folder.\n\n"
        "The run report stays in the folder you chose. The extraction ledger "
        "(_bulk_manifest.json) is written per output folder, so with any layout other than Flat "
        "each subfolder keeps its own — which is also why \"Only new\" compares against that "
        "subfolder rather than the whole run."));
    whereRow->addWidget(m_organize);
    m_onlyNew = new QRadioButton(QStringLiteral("Only new"), this);
    m_overwrite = new QRadioButton(QStringLiteral("Overwrite"), this);
    m_onlyNew->setToolTip(QStringLiteral(
        "Reads the folder's _bulk_manifest.json + existing files and exports only what's missing — "
        "re-running after a patch extracts just the new items."));
    m_overwrite->setToolTip(QStringLiteral("Re-extract everything that matches, overwriting existing files."));
    const bool wantNew = QSettings().value(QStringLiteral("bulk/onlyNew"), true).toBool();
    m_onlyNew->setChecked(wantNew);
    m_overwrite->setChecked(!wantNew);
    auto* exGroup = new QButtonGroup(this);   // exactly one selected
    exGroup->addButton(m_onlyNew);
    exGroup->addButton(m_overwrite);
    whereRow->addWidget(m_onlyNew);
    whereRow->addWidget(m_overwrite);
    root->addLayout(whereRow);

    // Extract buttons — directly under the destination path. The work set is your SELECTION in the
    // list below (or every match when nothing is selected). Two entry points:
    //   • Extract Selected → to the last folder shown above
    //   • Extract to…      → pick a folder (and remember it) then extract
    auto* btnRow = new QHBoxLayout();
    m_extractSelBtn = new QPushButton(QStringLiteral("Extract Selected"), this);
    m_extractSelBtn->setToolTip(QStringLiteral("Extract the selected rows below (or every match if none are selected), to the folder above."));
    m_extractSelBtn->setStyleSheet(QStringLiteral("font-weight:bold;"));
    m_extractToBtn = new QPushButton(QStringLiteral("Extract to…"), this);
    m_extractToBtn->setToolTip(QStringLiteral("Pick a folder, then extract the selection there (and remember it as the last folder)."));
    m_copyBtn = new QPushButton(QStringLiteral("Copy list"), this);
    m_copyBtn->setToolTip(QStringLiteral("Copy the matched names to the clipboard (one per line)."));
    m_openBtn = new QPushButton(QStringLiteral("Open folder"), this);
    m_openBtn->setToolTip(QStringLiteral("Open the current output folder in your file manager."));
    connect(m_openBtn, &QPushButton::clicked, this, [this] {
        const QString d = m_outDir ? m_outDir->text().trimmed() : QString();
        if (!d.isEmpty() && QDir(d).exists())
            QDesktopServices::openUrl(QUrl::fromLocalFile(d));
    });
    btnRow->addWidget(m_extractSelBtn, 1);
    btnRow->addWidget(m_extractToBtn);
    btnRow->addStretch();
    btnRow->addWidget(m_openBtn);
    btnRow->addWidget(m_copyBtn);
    root->addLayout(btnRow);

    m_lastRun = new QLabel(this);
    m_lastRun->setStyleSheet(QStringLiteral("color:#8fbf8f;"));
    m_lastRun->setWordWrap(true);
    m_lastRun->setVisible(false);
    root->addWidget(m_lastRun);

    // Split: LEFT = matches · RIGHT = queue. Both panes carry a centered title + counter, and the
    // splitter starts dead-centre (50/50). Selecting on the left mirrors into the queue; removing
    // from the queue (double-click / right-click) de-selects on the left.
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setMinimumHeight(240);

    auto* lWrap = new QWidget(split);
    auto* lLay = new QVBoxLayout(lWrap);
    lLay->setContentsMargins(0, 0, 0, 0);
    lLay->setSpacing(3);
    m_listLbl = new QLabel(QStringLiteral("Matches"), lWrap);
    m_listLbl->setStyleSheet(QString::fromLatin1(kSubHdrQss));
    m_listLbl->setAlignment(Qt::AlignHCenter);
    lLay->addWidget(m_listLbl);
    m_list = new QListWidget(lWrap);
    m_list->setUniformItemSizes(true);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->viewport()->installEventFilter(this);   // double-click = additive toggle (see eventFilter)
    lLay->addWidget(m_list, 1);
    split->addWidget(lWrap);

    auto* qWrap = new QWidget(split);
    auto* qLay = new QVBoxLayout(qWrap);
    qLay->setContentsMargins(0, 0, 0, 0);
    qLay->setSpacing(3);
    m_queueLbl = new QLabel(QStringLiteral("Queue"), qWrap);
    m_queueLbl->setStyleSheet(QString::fromLatin1(kSubHdrQss));
    m_queueLbl->setAlignment(Qt::AlignHCenter);
    qLay->addWidget(m_queueLbl);
    m_queue = new QListWidget(qWrap);
    m_queue->setUniformItemSizes(true);
    m_queue->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_queue->setContextMenuPolicy(Qt::CustomContextMenu);
    m_queue->setToolTip(QStringLiteral("Items queued for extraction. Double-click (or right-click) to remove one."));
    qLay->addWidget(m_queue, 1);
    split->addWidget(qWrap);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    split->setSizes({10000, 10000});   // start perfectly centred
    PanelPersist::bind(split, QStringLiteral("bulk/mainSplit"));   // remember the divider position
    root->addWidget(split, 2);

    connect(m_list, &QListWidget::itemSelectionChanged, this, [this] {
        if (m_syncingQueue) return;
        syncQueue();   // reconcile the visible selection into the persistent queue
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint& p) { showListMenu(p); });
    // Drop a queued sno from the persistent store (and de-select it on the left if visible).
    auto removeQueued = [this](int sno) {
        for (int i = 0; i < m_queued.size(); ++i)
            if (m_queued[i].first == sno) { m_queued.remove(i); break; }
        m_queuedSnos.remove(sno);
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem* li = m_list->item(i);
            if (li->data(Qt::UserRole).isValid() && li->data(Qt::UserRole).toInt() == sno) {
                QSignalBlocker b(m_list); li->setSelected(false); break;
            }
        }
        rebuildQueueWidget();
        saveQueue(m_mode ? m_mode->currentIndex() : 0);
        syncExtractButtons();
    };
    connect(m_queue, &QListWidget::itemDoubleClicked, this, [removeQueued](QListWidgetItem* it) {
        if (it && it->data(Qt::UserRole).isValid()) removeQueued(it->data(Qt::UserRole).toInt());
    });
    connect(m_queue, &QListWidget::customContextMenuRequested, this, [this, removeQueued](const QPoint& p) {
        QListWidgetItem* under = m_queue->itemAt(p);
        QMenu menu(this);
        QAction* rm  = menu.addAction(QStringLiteral("Remove from queue"));
        QAction* clr = menu.addAction(QStringLiteral("Clear queue"));
        rm->setEnabled(under != nullptr || !m_queue->selectedItems().isEmpty());
        QAction* act = menu.exec(m_queue->viewport()->mapToGlobal(p));
        if (act == rm) {
            QList<QListWidgetItem*> sel = m_queue->selectedItems();
            if (sel.isEmpty() && under) sel << under;
            QVector<int> snos;
            for (QListWidgetItem* s : sel) if (s->data(Qt::UserRole).isValid()) snos << s->data(Qt::UserRole).toInt();
            for (int sno : snos) removeQueued(sno);
        } else if (act == clr) {
            m_queued.clear(); m_queuedSnos.clear();
            { QSignalBlocker b(m_list); m_list->clearSelection(); }
            rebuildQueueWidget();
            saveQueue(m_mode ? m_mode->currentIndex() : 0);
            syncExtractButtons();
        }
    });
    // Toggle "Pick items manually": on = selection enabled (queue active); off = whole match set.
    auto applyManual = [this, allBtn, noneBtn](bool on) {
        m_list->setSelectionMode(on ? QAbstractItemView::ExtendedSelection
                                    : QAbstractItemView::NoSelection);
        allBtn->setEnabled(on);
        noneBtn->setEnabled(on);
        if (m_queue)    m_queue->setEnabled(on);
        if (m_queueLbl) m_queueLbl->setEnabled(on);
        reflectQueueInList();   // re-select visible queued rows (or clear the highlight when off)
        syncExtractButtons();
    };
    connect(m_showList, &QCheckBox::toggled, this, [this, applyManual](bool on) {
        QSettings().setValue(QStringLiteral("bulk/showList"), on);
        applyManual(on);
    });
    applyManual(m_showList->isChecked());

    // ── Live run console: every exported item and every failure WITH ITS REASON, in real time,
    // plus a progress bar and a Cancel that takes effect between items. Replaces the old modal
    // progress dialog, which hid the tab and told you nothing when something went wrong.
    auto* runRow = new QHBoxLayout();
    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(true);
    m_progress->setFormat(QStringLiteral("%v / %m"));
    m_progress->setVisible(false);
    m_cancelBtn = new QPushButton(QStringLiteral("Cancel"), this);
    m_cancelBtn->setVisible(false);
    connect(m_cancelBtn, &QPushButton::clicked, this, [this] {
        m_cancelRequested = true;
        m_cancelBtn->setText(QStringLiteral("Canceling…"));   // instant feedback; lands after the
        m_cancelBtn->setEnabled(false);                        // current item finishes writing
        logLine(QStringLiteral("Cancel requested — stopping after the current item…"));
    });
    // Esc = Cancel while a run is in flight (scoped to this tab, so it steals nothing elsewhere).
    {
        auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        esc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(esc, &QShortcut::activated, this, [this] {
            if (m_running && m_cancelBtn && m_cancelBtn->isEnabled()) m_cancelBtn->click();
        });
    }
    // Pause/Resume: the workers block inside their between-items cancel poll while paused, so
    // pausing is instant, costs nothing, and resumes exactly where it stopped.
    m_pauseBtn = new QPushButton(QStringLiteral("Pause"), this);
    m_pauseBtn->setVisible(false);
    connect(m_pauseBtn, &QPushButton::clicked, this, [this] {
        const bool pausing = !m_pauseRequested.load();
        m_pauseRequested = pausing;
        if (pausing) {
            m_pauseT0 = QDateTime::currentMSecsSinceEpoch();
            m_pauseBtn->setText(QStringLiteral("Resume"));
            logLine(QStringLiteral("Paused — finishing the current item, then holding."));
        } else {
            m_pausedMs += QDateTime::currentMSecsSinceEpoch() - m_pauseT0;
            m_pauseBtn->setText(QStringLiteral("Pause"));
            logLine(QStringLiteral("Resumed."));
        }
    });
    runRow->addWidget(m_progress, 1);
    runRow->addWidget(m_pauseBtn);
    runRow->addWidget(m_cancelBtn);
    root->addLayout(runRow);
    m_console = new QPlainTextEdit(this);
    m_console->setReadOnly(true);
    m_console->setMaximumBlockCount(5000);   // bounded — week-long sessions can't balloon it
    m_console->setMinimumHeight(140);
    {
        QFont mono = m_console->font();
        mono.setFamily(QStringLiteral("Consolas"));
        mono.setPointSizeF(9.0);
        m_console->setFont(mono);
    }
    m_console->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{background:#141414;color:#c8c8c8;border:1px solid #2a2a2a;}"));
    m_console->setPlaceholderText(QStringLiteral(
        "Run output appears here — every exported item, every failure with its reason."));
    root->addWidget(m_console, 1);

    connect(m_name, &QLineEdit::textChanged, this, [this](const QString&) { updateCount(); });
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this](int i) {
        QSettings().setValue(QStringLiteral("bulk/mode"), i);
        saveQueue(m_prevMode);      // persist the outgoing mode's queue…
        m_prevMode = i;
        loadQueueForMode(i);        // …then swap to the incoming mode's queue
        updateFunnelMode();         // swap the funnel to model tags ↔ texture categories
        updateTagBtn();             // active-filter tint reflects the current mode's selection
        updateCount();              // switches the SNO group (models 9 ↔ textures 44) + reflects queue
        updateLayoutModeAvailability();   // Class/Type mean nothing for textures — grey them out
    });
    connect(allBtn,  &QPushButton::clicked, this, [this] {   // queues the WHOLE match set, not just visible
        if (m_showList && m_showList->isChecked()) queueAllMatches();
        else m_list->selectAll();   // automatic mode ignores the queue; this is purely visual
        m_list->setFocus();
    });
    connect(noneBtn, &QPushButton::clicked, this, [this] {   // clears the WHOLE queue, not just visible
        m_queued.clear(); m_queuedSnos.clear();
        { QSignalBlocker b(m_list); m_list->clearSelection(); }
        rebuildQueueWidget();
        saveQueue(m_mode ? m_mode->currentIndex() : 0);
        syncExtractButtons();
    });
    connect(m_onlyNew, &QRadioButton::toggled, this,
            [this](bool on) { QSettings().setValue(QStringLiteral("bulk/onlyNew"), on); updateCount(); });
    connect(m_organize, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_syncingLayout) return;   // echo from syncLayoutCombo(), not a user choice
        QSettings().setValue(QStringLiteral("export/folderLayout"), m_organize->currentData().toString());
    });
    connect(m_extractSelBtn, &QPushButton::clicked, this, [this] { doExtract(/*promptDir*/false); });
    connect(m_extractToBtn,  &QPushButton::clicked, this, [this] { doExtract(/*promptDir*/true);  });
    connect(m_copyBtn, &QPushButton::clicked, this, [this] {
        const auto m = computeMatches();
        QStringList names; names.reserve(m.size());
        for (const auto& it : m) names << it.second;
        QGuiApplication::clipboard()->setText(names.join(QLatin1Char('\n')));
        if (m_count) m_count->setText(QStringLiteral("Copied %1 name(s) to the clipboard.").arg(names.size()));
    });
    connect(savePresetBtn, &QPushButton::clicked, this, [this] { savePreset(); });
    connect(delPresetBtn,  &QPushButton::clicked, this, [this] { deletePreset(); });
    connect(m_preset, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!m_loadingPreset) loadPreset(m_preset->currentData().toString());
    });
    connect(&AppearanceMeta::instance(), &AppearanceMeta::readyChanged, this,
            [this] { refillTagPanel(); updateCount(); });

    restoreFilterState();   // re-apply the saved funnel state (if "Remember filters" was on)
    m_prevMode = m_mode ? m_mode->currentIndex() : 0;
    loadQueueForMode(m_prevMode);   // restore this mode's persisted queue
    refreshPresets();
    loadFolderManifest();
    updateCount();          // builds the list + reflects the restored queue into it
}

void BulkExtractorTab::refresh() { refillTagPanel(); updateCount(); }

// The current UI state, resolved to matches. Thin wrapper over matchesFor so that the preset
// audit can evaluate a query WITHOUT driving the widgets — an audit that had to click through the
// UI to measure would be a second matcher, and a second matcher is how these drift.
QVector<QPair<int, QString>> BulkExtractorTab::computeMatches()
{
    return matchesFor(textureMode(), m_name ? m_name->text() : QString(), m_texCatSel,
                      m_catFacet, m_tagFilter, m_tagOrMode, m_hideUnrend);
}

QVector<QPair<int, QString>> BulkExtractorTab::matchesFor(
    bool texMode, const QString& nameText, const QSet<QString>& texCats,
    const QString& facet, const QSet<QString>& tags, bool tagOr, bool hideUnrend)
{
    if (!m_models) return {};
    // ── Textures mode: filter group-44 by NAME + the ticked TEXTURE categories (union). The model
    //    tag/class/gender/type filters don't apply to textures, so we don't route through them. ──
    if (texMode) {
        QVector<QPair<int, QString>> out;
        if (!m_index) return out;
        QStringList inc, exc;   // NAME box: space = AND include, leading '-' = exclude
        for (const QString& tok : nameText.trimmed()
                                      .split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            if (tok.startsWith(QLatin1Char('-')) && tok.size() > 1) exc << tok.mid(1);
            else if (!tok.startsWith(QLatin1Char('-')))             inc << tok;
        }
        static const QString kLatest = QStringLiteral("Latest (new this update)");
        const bool wantLatest = texCats.contains(kLatest);
        QStringList cats;
        for (const QString& c : texCats) if (c != kLatest) cats << c;
        const bool haveCat = wantLatest || !cats.isEmpty();
        for (const SnoEntry& e : m_index->entries(kTextureGroup)) {
            const QString nl = e.name.toLower();
            bool ok = true;
            for (const QString& t : inc) if (!QueryTerm::matches(nl, t)) { ok = false; break; }
            if (ok) for (const QString& t : exc) if (QueryTerm::matches(nl, t)) { ok = false; break; }
            if (!ok) continue;
            if (haveCat) {
                bool any = (wantLatest && m_index->isNew(e.snoId));
                if (!any) for (const QString& c : cats)
                    if (TexturesTab::bulkTexInCategory(nl, c)) { any = true; break; }
                if (!any) continue;
            }
            out.push_back({int(e.snoId), e.name});
        }
        return out;
    }
    ModelsTab::FilterSpec f;
    // Everything is driven through the funnel, exactly like the Models tab: the usage facet feeds
    // category; every ticked tag (Category/Class/Type/Gender/…) feeds the multi-tag selection; the
    // NAME box (which also matches tags + collection once loaded) covers name/collection search.
    f.category        = facet;
    f.nameSearch      = nameText;
    f.tagSel          = tags;
    f.tagOr           = tagOr;
    f.hideUnrenderable = hideUnrend;
    // Delegate to the Models tab's authoritative matcher so results are identical to its list.
    return m_models->queryEntries(kModelGroup, f);
}

// ── The filter funnel popup — a faithful copy of the Models tab's single filter panel ─────────
void BulkExtractorTab::buildTagPanel()
{
    // Kick the appearance-meta build so the tag groups (and matching) come online.
    if (!AppearanceMeta::instance().ready() && m_index)
        AppearanceMeta::instance().ensureBuilt(Config::d4dataDir(), m_index, m_reader);

    m_tagPanel = new QFrame(this, Qt::Popup);
    m_tagPanel->setObjectName(QStringLiteral("tagPanel"));
    m_tagPanel->setStyleSheet(QStringLiteral(
        "QFrame#tagPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
        "QLabel{color:#cccccc;} QCheckBox{color:#cccccc;}"));
    auto* tpl = new QVBoxLayout(m_tagPanel);
    tpl->setContentsMargins(10, 8, 10, 8);
    tpl->setSpacing(5);

    auto* topRow = new QHBoxLayout();
    m_tagSearch = new QLineEdit(m_tagPanel);
    m_tagSearch->setPlaceholderText(QStringLiteral("Search tags…"));
    m_tagSearch->setClearButtonEnabled(true);
    topRow->addWidget(m_tagSearch, 1);
    auto* clearBtn = new QPushButton(QStringLiteral("Clear"), m_tagPanel);
    clearBtn->setToolTip(QStringLiteral("Uncheck every tag"));
    topRow->addWidget(clearBtn);
    tpl->addLayout(topRow);

    m_tagOrChk = new QCheckBox(QStringLiteral("Match any tag (OR)"), m_tagPanel);
    m_tagOrChk->setToolTip(QStringLiteral("Off: results carry ALL selected tags (narrowing).\n"
                                          "On: results carry AT LEAST ONE (widening)."));
    connect(m_tagOrChk, &QCheckBox::toggled, this, [this](bool on) {
        m_tagOrMode = on; updateTagBtn(); updateCount();
    });
    tpl->addWidget(m_tagOrChk);

    m_hideChk = new QCheckBox(QStringLiteral("Hide un-renderable"), m_tagPanel);
    m_hideChk->setToolTip(QStringLiteral("Hide models that can't be displayed (no geometry — "
                                         "props, attachments, effect meshes)."));
    connect(m_hideChk, &QCheckBox::toggled, this, [this](bool on) { m_hideUnrend = on; updateCount(); });
    tpl->addWidget(m_hideChk);

    // ── Special usage facets (mutually exclusive) → the single category facet ──
    // Wrapped in m_specialBox so the whole MODEL-only group hides in texture mode.
    m_specialBox = new QWidget(m_tagPanel);
    {
        auto* sbl = new QVBoxLayout(m_specialBox);
        sbl->setContentsMargins(0, 0, 0, 0); sbl->setSpacing(5);
        auto* spLbl = new QLabel(QStringLiteral("Special"), m_specialBox);
        spLbl->setStyleSheet(QString::fromLatin1(kHdrQss));
        sbl->addWidget(spLbl);
        // Parity with the Models tab: Creature / Gear dropped (duplicate the Category tags
        // "Monster" / "Item"); the rest are unique animation/usage/update facets.
        static const struct { const char* label; const char* data; } kSpecial[] = {
            {"Latest (new this update)",         "__latest__"},
            {"Animated (owns / inherits clips)", "__animated__"},
            {"Rigged (on a base rig)",           "__rigged__"},
            {"Orphaned (no actor uses it)",      "__orphaned__"}};
        for (const auto& sp : kSpecial) {
            auto* c = new QCheckBox(QString::fromLatin1(sp.label), m_specialBox);
            const QString data = QString::fromLatin1(sp.data);
            m_specialChecks.insert(data, c);
            connect(c, &QCheckBox::toggled, this, [this, data](bool on) {
                if (on) {   // facets are mutually exclusive (like the Models category combo)
                    m_catFacet = data;
                    for (auto it = m_specialChecks.begin(); it != m_specialChecks.end(); ++it)
                        if (it.key() != data) { QSignalBlocker b(it.value()); it.value()->setChecked(false); }
                } else if (m_catFacet == data) {
                    m_catFacet.clear();
                }
                updateTagBtn(); updateCount();
            });
            sbl->addWidget(c);
        }
    }
    tpl->addWidget(m_specialBox);

    // ── Texture-mode categories (shown only in Textures mode) — the same facets the Textures tab
    //    uses, plus a "Latest" (new this update). A union: results match ANY ticked category.
    m_texFilterBox = new QWidget(m_tagPanel);
    {
        auto* tbl = new QVBoxLayout(m_texFilterBox);
        tbl->setContentsMargins(0, 0, 0, 0); tbl->setSpacing(4);
        auto* hdr = new QLabel(QStringLiteral("Texture categories"), m_texFilterBox);
        hdr->setStyleSheet(QString::fromLatin1(kHdrQss));
        tbl->addWidget(hdr);
        auto addCat = [&](const QString& cat) {
            auto* c = new QCheckBox(cat, m_texFilterBox);
            m_texCatChecks.insert(cat, c);
            connect(c, &QCheckBox::toggled, this, [this, cat](bool on) {
                if (on) m_texCatSel.insert(cat); else m_texCatSel.remove(cat);
                updateTagBtn(); updateCount();
            });
            tbl->addWidget(c);
        };
        addCat(QStringLiteral("Latest (new this update)"));   // special: SNO/isNew-based
        for (const QString& cat : TexturesTab::bulkTexCategories()) addCat(cat);
        tbl->addStretch(1);
    }
    tpl->addWidget(m_texFilterBox);

    m_rememberChk = new QCheckBox(QStringLiteral("Remember filters"), m_tagPanel);
    m_rememberChk->setToolTip(QStringLiteral("Restore the NAME box, ticked tags, match mode and "
                                             "facet exactly as you left them when the tool re-opens"));
    m_rememberChk->setChecked(QSettings().value(QStringLiteral("bulk/rememberFilter"), false).toBool());
    connect(m_rememberChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("bulk/rememberFilter"), on);
        if (on) saveFilterState();
    });
    tpl->addWidget(m_rememberChk);

    m_tagScroll = new QScrollArea(m_tagPanel);
    m_tagScroll->setWidgetResizable(true);
    m_tagScroll->setFrameShape(QFrame::NoFrame);
    m_tagScroll->setFixedHeight(320);
    m_tagScroll->setMinimumWidth(240);
    m_tagPanelBody = new QWidget(m_tagScroll);
    auto* bodyLay = new QVBoxLayout(m_tagPanelBody);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    bodyLay->setSpacing(3);
    bodyLay->addWidget(new QLabel(QStringLiteral("Tags load with the index…"), m_tagPanelBody));
    bodyLay->addStretch(1);
    m_tagScroll->setWidget(m_tagPanelBody);
    tpl->addWidget(m_tagScroll, 1);

    connect(clearBtn, &QPushButton::clicked, this, [this] {
        m_tagFilter.clear();
        for (QCheckBox* c : std::as_const(m_tagChecks)) { QSignalBlocker b(c); c->setChecked(false); }
        m_texCatSel.clear();   // clear texture categories too (Clear works in both modes)
        for (QCheckBox* c : std::as_const(m_texCatChecks)) { QSignalBlocker b(c); c->setChecked(false); }
        updateTagBtn(); updateCount();
    });
    connect(m_tagSearch, &QLineEdit::textChanged, this, [this](const QString& t) {
        const QString needle = t.trimmed();
        const QList<QWidget*> groups =
            m_tagPanelBody->findChildren<QWidget*>(QStringLiteral("tagGroup"), Qt::FindDirectChildrenOnly);
        for (QWidget* g : groups) {
            int vis = 0;
            for (QCheckBox* c : g->findChildren<QCheckBox*>()) {
                const bool hit = needle.isEmpty() || c->text().contains(needle, Qt::CaseInsensitive);
                c->setVisible(hit);
                if (hit) ++vis;
            }
            g->setVisible(vis > 0);
        }
    });
    refillTagPanel();
    updateFunnelMode();   // show model tags vs texture categories for the current mode
}

// Show the MODEL tag facets in Models mode, or the TEXTURE category checkboxes in Textures mode, so
// the funnel is always context-appropriate. The shared bits (Search box, Clear, Remember) stay.
void BulkExtractorTab::updateFunnelMode()
{
    const bool tex = textureMode();
    if (m_specialBox)  m_specialBox->setVisible(!tex);
    if (m_tagOrChk)    m_tagOrChk->setVisible(!tex);
    if (m_hideChk)     m_hideChk->setVisible(!tex);
    if (m_tagScroll)   m_tagScroll->setVisible(!tex);
    if (m_tagSearch)   m_tagSearch->setVisible(!tex);   // "Search tags…" filters model tag groups
    if (m_texFilterBox) m_texFilterBox->setVisible(tex);
    if (m_tagPanel && m_tagPanel->isVisible()) m_tagPanel->adjustSize();
}

// (Re)build the scrollable tag-group checkboxes from the Models tab's tag groups, preserving ticks.
void BulkExtractorTab::refillTagPanel()
{
    if (!m_tagPanelBody || !m_models) return;
    auto* bl = static_cast<QVBoxLayout*>(m_tagPanelBody->layout());
    while (QLayoutItem* it = bl->takeAt(0)) { if (it->widget()) it->widget()->deleteLater(); delete it; }
    m_tagChecks.clear();
    const auto groups = m_models->filterTagGroups();
    if (groups.isEmpty()) {
        bl->addWidget(new QLabel(QStringLiteral("Tags load with the index…"), m_tagPanelBody));
        bl->addStretch(1);
        updateTagBtn();
        return;
    }
    for (const auto& g : groups) {
        if (g.second.isEmpty()) continue;
        auto* gw = new QWidget(m_tagPanelBody);
        gw->setObjectName(QStringLiteral("tagGroup"));   // the search handler finds these
        auto* gl = new QVBoxLayout(gw);
        gl->setContentsMargins(0, 2, 0, 2);
        gl->setSpacing(2);
        auto* head = new QLabel(QStringLiteral("%1 (%2)").arg(g.first).arg(g.second.size()), gw);
        head->setStyleSheet(QString::fromLatin1(kHdrQss));
        gl->addWidget(head);
        QStringList vals = g.second;
        vals.removeDuplicates();
        vals.sort(Qt::CaseInsensitive);
        for (const QString& tv : vals) {
            auto* cb = new QCheckBox(tv, gw);
            cb->setChecked(m_tagFilter.contains(tv));
            m_tagChecks.insert(tv, cb);
            connect(cb, &QCheckBox::toggled, this, [this, tv](bool on) {
                if (on) m_tagFilter.insert(tv); else m_tagFilter.remove(tv);
                updateTagBtn();
                updateCount();
            });
            gl->addWidget(cb);
        }
        bl->addWidget(gw);
    }
    bl->addStretch(1);
    if (m_tagOrChk)  { QSignalBlocker b(m_tagOrChk);  m_tagOrChk->setChecked(m_tagOrMode); }
    if (m_hideChk)   { QSignalBlocker b(m_hideChk);   m_hideChk->setChecked(m_hideUnrend); }
    for (auto it = m_specialChecks.begin(); it != m_specialChecks.end(); ++it) {
        QSignalBlocker b(it.value());
        it.value()->setChecked(m_catFacet == it.key());
    }
    updateTagBtn();
}

// Gold-tint the funnel icon whenever any facet or tag filter is active (parity with Models).
void BulkExtractorTab::updateTagBtn()
{
    if (!m_tagBtn) return;
    const bool active = textureMode()
        ? !m_texCatSel.isEmpty()
        : (!m_tagFilter.isEmpty() || !m_catFacet.isEmpty() || m_hideUnrend);
    m_tagBtn->setStyleSheet(active
        ? QStringLiteral("QToolButton{padding:1px;border:1px solid #a07a1a;border-radius:3px;"
                         "background:#3a2f12;} QToolButton:hover{border-color:#b0453c;}")
        : QString::fromLatin1(kIconBtnQss));
    if (m_rememberChk && m_rememberChk->isChecked()) saveFilterState();
}

// ── Remember filters: persist / restore the funnel state ──
void BulkExtractorTab::saveFilterState()
{
    QSettings s;
    s.beginGroup(QStringLiteral("bulk/filter"));
    s.setValue(QStringLiteral("name"),   m_name ? m_name->text() : QString());
    s.setValue(QStringLiteral("facet"),  m_catFacet);
    s.setValue(QStringLiteral("tags"),   QStringList(m_tagFilter.values()));
    s.setValue(QStringLiteral("or"),     m_tagOrMode);
    s.setValue(QStringLiteral("hide"),   m_hideUnrend);
    s.setValue(QStringLiteral("texCats"), QStringList(m_texCatSel.values()));   // texture-mode categories
    s.endGroup();
}

void BulkExtractorTab::restoreFilterState()
{
    if (!QSettings().value(QStringLiteral("bulk/rememberFilter"), false).toBool()) return;
    QSettings s;
    s.beginGroup(QStringLiteral("bulk/filter"));
    if (m_name) m_name->setText(s.value(QStringLiteral("name")).toString());
    m_catFacet  = s.value(QStringLiteral("facet")).toString();
    const QStringList tags = s.value(QStringLiteral("tags")).toStringList();
    m_tagFilter = QSet<QString>(tags.begin(), tags.end());
    m_tagOrMode = s.value(QStringLiteral("or"), false).toBool();
    m_hideUnrend = s.value(QStringLiteral("hide"), false).toBool();
    const QStringList tc = s.value(QStringLiteral("texCats")).toStringList();
    m_texCatSel = QSet<QString>(tc.begin(), tc.end());
    for (auto it = m_texCatChecks.begin(); it != m_texCatChecks.end(); ++it)
        { QSignalBlocker b(it.value()); it.value()->setChecked(m_texCatSel.contains(it.key())); }
    s.endGroup();
    refillTagPanel();
    updateTagBtn();
}

void BulkExtractorTab::updateCount()
{
    const auto matches = computeMatches();
    const int n = matches.size();
    const QString noun = textureMode() ? QStringLiteral("texture") : QStringLiteral("model");
    auto present = [this](const QPair<int, QString>& it) {
        return m_folderDone.contains(it.first) || m_folderStems.contains(it.second);
    };
    int done = 0;
    for (const auto& it : matches) if (present(it)) ++done;
    QString txt = QStringLiteral("%1 %2(s) match").arg(n).arg(noun);
    if (done > 0) txt += QStringLiteral("   ·   %1 new · %2 already in folder").arg(n - done).arg(done);
    if (m_count) m_count->setText(txt);
    if (m_listLbl) m_listLbl->setText(QStringLiteral("Matches (%1)").arg(n));

    // The LEFT matches list is always visible now (capped so a huge match set doesn't stall the UI).
    // Items already in the output folder get a ✓ + dimmed colour; new ones are plain. Which rows show
    // as selected is derived from the PERSISTENT queue (reflectQueueInList) — not from prior widget state.
    if (m_list) {
        constexpr int kCap = 5000;
        const bool skip = m_onlyNew && m_onlyNew->isChecked();
        QSignalBlocker block(m_list);   // suppress the selection-changed storm during the rebuild
        m_list->clear();
        const int shown = qMin(n, kCap);
        for (int i = 0; i < shown; ++i) {
            const int sno = matches[i].first;
            const bool exists = present(matches[i]);
            auto* item = new QListWidgetItem((exists ? QStringLiteral("✓  ") : QString()) + matches[i].second);
            item->setForeground(exists ? QColor(120, 130, 120) : QColor(215, 215, 215));
            item->setData(Qt::UserRole,     sno);                 // sno (for the work set)
            item->setData(Qt::UserRole + 1, matches[i].second);   // clean name
            if (exists)
                item->setToolTip(QStringLiteral("Already in this folder — will be %1")
                                     .arg(skip ? QStringLiteral("skipped") : QStringLiteral("overwritten")));
            m_list->addItem(item);
        }
        // The cap is a DISPLAY limit only. Say so, because the bare "…and N more" left you to guess
        // whether the hidden rows were also excluded from the work — and it was itself selectable,
        // so Ctrl+A swept a row that carried no sno into the selection.
        if (n > kCap) {
            auto* more = new QListWidgetItem(
                QStringLiteral("…and %1 more — not listed, but still extracted").arg(n - kCap));
            more->setFlags(Qt::NoItemFlags);   // not selectable, not queueable
            more->setForeground(QColor(150, 150, 150));
            more->setToolTip(QStringLiteral(
                "Only the first %1 matches are listed, to keep the view responsive.\n"
                "\"All\" queues every match — all %2 — not just the ones shown.").arg(kCap).arg(n));
            m_list->addItem(more);
        }
    }

    reflectQueueInList();   // re-select visible queued rows + refresh the queue widget
    syncExtractButtons();
}

// Reconcile the VISIBLE left-list selection into the persistent queue: newly-selected visible rows
// are added, de-selected visible rows are removed. Queued items that aren't currently visible (a
// different filter / capped-out) are LEFT ALONE, so the queue survives filter tweaks.
void BulkExtractorTab::syncQueue()
{
    if (!m_list || !m_showList || !m_showList->isChecked()) return;
    bool changed = false;
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* it = m_list->item(i);
        const QVariant sv = it->data(Qt::UserRole);
        if (!sv.isValid()) continue;
        const int sno = sv.toInt();
        const bool sel = it->isSelected();
        const bool inQ = m_queuedSnos.contains(sno);
        if (sel && !inQ) {
            m_queued.append({sno, it->data(Qt::UserRole + 1).toString()});
            m_queuedSnos.insert(sno);
            changed = true;
        } else if (!sel && inQ) {
            for (int j = 0; j < m_queued.size(); ++j)
                if (m_queued[j].first == sno) { m_queued.remove(j); break; }
            m_queuedSnos.remove(sno);
            changed = true;
        }
    }
    rebuildQueueWidget();
    if (changed) saveQueue(m_mode ? m_mode->currentIndex() : 0);
    syncExtractButtons();
}

// "All" means every MATCH, not every visible row.
//
// The left list is capped at kCap rows so a huge filter doesn't stall the UI, and syncQueue only
// ever walks rows that exist as WIDGETS. So the old implementation — m_list->selectAll() — queued
// the first 5000 of a 12,000-match filter and dropped the other 7,000 with no error: the match
// label said 12000, the queue label said 5000, and nothing connected the two numbers. The "None"
// button already carried a comment saying it clears the WHOLE queue "not just visible"; this is
// the missing other half of that pair.
void BulkExtractorTab::queueAllMatches()
{
    const auto all = computeMatches();
    m_queued = all;
    m_queuedSnos.clear();
    m_queuedSnos.reserve(all.size());
    for (const auto& it : all) m_queuedSnos.insert(it.first);
    reflectQueueInList();   // selects the visible subset and rebuilds the queue widget
    saveQueue(m_mode ? m_mode->currentIndex() : 0);
    syncExtractButtons();
}

// Refill the queue list widget from the persistent store.
void BulkExtractorTab::rebuildQueueWidget()
{
    if (!m_queue) return;
    m_syncingQueue = true;
    m_queue->clear();
    for (const auto& it : m_queued) {
        auto* qi = new QListWidgetItem(it.second, m_queue);
        qi->setData(Qt::UserRole,     it.first);
        qi->setData(Qt::UserRole + 1, it.second);
    }
    if (m_queueLbl)
        m_queueLbl->setText(m_queued.isEmpty() ? QStringLiteral("Queue")
                                               : QStringLiteral("Queue (%1)").arg(m_queued.size()));
    m_syncingQueue = false;
}

// Show which visible rows are queued (manual mode) without disturbing the persistent store.
void BulkExtractorTab::reflectQueueInList()
{
    if (!m_list) return;
    const bool manual = m_showList && m_showList->isChecked();
    QSignalBlocker b(m_list);
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* it = m_list->item(i);
        const QVariant sv = it->data(Qt::UserRole);
        if (!sv.isValid()) continue;
        it->setSelected(manual && m_queuedSnos.contains(sv.toInt()));
    }
    rebuildQueueWidget();
}

void BulkExtractorTab::saveQueue(int mode) const
{
    QStringList l;
    l.reserve(m_queued.size());
    for (const auto& it : m_queued)
        l << QStringLiteral("%1\x1f%2").arg(it.first).arg(it.second);
    QSettings().setValue(mode == 1 ? QStringLiteral("bulk/queueTex")
                                   : QStringLiteral("bulk/queueModels"), l);
}

void BulkExtractorTab::loadQueueForMode(int mode)
{
    m_queued.clear();
    m_queuedSnos.clear();
    const QStringList l = QSettings().value(mode == 1 ? QStringLiteral("bulk/queueTex")
                                                      : QStringLiteral("bulk/queueModels")).toStringList();
    for (const QString& s : l) {
        const int sep = s.indexOf(QChar(0x1f));
        if (sep <= 0) continue;
        const int sno = s.left(sep).toInt();
        const QString name = s.mid(sep + 1);
        if (sno != 0 && !m_queuedSnos.contains(sno)) {
            m_queued.append({sno, name});
            m_queuedSnos.insert(sno);
        }
    }
}

// Per-run CSV: name · SNO · status (ok/failed/missing) · reason · bytes. Built from the folder
// manifest + this run's _bulk_failed.txt + on-disk file sizes — no pipeline changes needed.
void BulkExtractorTab::writeRunReport(const QString& dir, const QVector<QPair<int, QString>>& items) const
{
    // Failure reasons this run wrote (one "name — why" per line).
    QHash<QString, QString> failReason;
    QFile ff(QDir(dir).filePath(QStringLiteral("_bulk_failed.txt")));
    if (ff.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!ff.atEnd()) {
            const QString line = QString::fromUtf8(ff.readLine()).trimmed();
            const int sep = line.indexOf(QStringLiteral(" — "));
            if (sep > 0) failReason.insert(line.left(sep), line.mid(sep + 3));
        }
    }
    // File sizes by base name (one dir scan).
    QHash<QString, qint64> sizeByBase;
    for (const QFileInfo& fi : QDir(dir).entryInfoList(QDir::Files))
        sizeByBase.insert(fi.completeBaseName(), fi.size());

    QFile f(QDir(dir).filePath(QStringLiteral("_bulk_report.csv")));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) return;
    QTextStream out(&f);
    auto csv = [](QString s) {
        if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')) || s.contains(QLatin1Char('\n'))) {
            s.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            return QLatin1Char('"') + s + QLatin1Char('"');
        }
        return s;
    };
    out << "name,sno,status,reason,bytes\n";
    for (const auto& it : items) {
        const bool present = m_folderDone.contains(it.first) || m_folderStems.contains(it.second);
        const QString status = present ? QStringLiteral("ok")
                                        : (failReason.contains(it.second) ? QStringLiteral("failed")
                                                                          : QStringLiteral("missing"));
        out << csv(it.second) << ',' << it.first << ',' << status << ','
            << csv(failReason.value(it.second)) << ',' << sizeByBase.value(it.second, 0) << '\n';
    }
}

// Double-click = additive toggle of the row's selection. In ExtendedSelection the first press of a
// double-click already cleared the multi-selection, so we snapshot the selection at the START of a
// click sequence (a press more than one double-click interval after the previous one) and, on the
// double-click, restore that snapshot and flip just the clicked row.
bool BulkExtractorTab::eventFilter(QObject* obj, QEvent* ev)
{
    if (m_list && obj == m_list->viewport()) {
        if (ev->type() == QEvent::MouseButtonPress) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastPressMs > QApplication::doubleClickInterval()) {
                m_preClickSel.clear();
                for (QListWidgetItem* it : m_list->selectedItems())
                    if (it->data(Qt::UserRole).isValid()) m_preClickSel.insert(it->data(Qt::UserRole).toInt());
            }
            m_lastPressMs = now;
        } else if (ev->type() == QEvent::MouseButtonDblClick) {
            auto* me = static_cast<QMouseEvent*>(ev);
            QListWidgetItem* hit = m_list->itemAt(me->position().toPoint());
            if (hit && hit->data(Qt::UserRole).isValid()) {
                const int sno = hit->data(Qt::UserRole).toInt();
                QSignalBlocker block(m_list);
                for (int i = 0; i < m_list->count(); ++i) {   // restore the pre-click selection…
                    QListWidgetItem* it = m_list->item(i);
                    const QVariant sv = it->data(Qt::UserRole);
                    if (sv.isValid()) it->setSelected(m_preClickSel.contains(sv.toInt()));
                }
                hit->setSelected(!m_preClickSel.contains(sno));   // …then flip just this row
                syncExtractButtons();
                return true;   // swallow so the view doesn't re-select on top of us
            }
        }
    }
    return BrowserTab::eventFilter(obj, ev);
}

// The work set for a run: in manual mode the persistent QUEUE (the items you picked); else every match.
QVector<QPair<int, QString>> BulkExtractorTab::workSet()
{
    if (m_showList && m_showList->isChecked())
        return m_queued;   // may be empty — manual mode requires an explicit pick
    return computeMatches();
}

// Extract-button labels/enablement track the work set: queue size (manual) or the full match count.
void BulkExtractorTab::syncExtractButtons()
{
    const QString noun = textureMode() ? QStringLiteral("texture") : QStringLiteral("model");
    const bool manual = m_showList && m_showList->isChecked();
    int n;
    QString verb;
    if (manual) {
        n = m_queued.size();
        verb = QStringLiteral("Extract %1 queued").arg(n);
    } else {
        n = m_index ? computeMatches().size() : 0;
        verb = QStringLiteral("Extract %1 %2(s)").arg(n).arg(noun);
    }
    if (m_extractSelBtn) {
        m_extractSelBtn->setEnabled(n > 0 && !m_running);
        m_extractSelBtn->setText(n > 0 ? verb : QStringLiteral("Extract"));
    }
    if (m_extractToBtn) m_extractToBtn->setEnabled(n > 0 && !m_running);
}

// Right-click menu on the matches list — ported from the Models tab, plus cross-tab actions.
void BulkExtractorTab::showListMenu(const QPoint& pos)
{
    if (!m_list) return;
    QListWidgetItem* under = m_list->itemAt(pos);
    const QList<QListWidgetItem*> sel = m_list->selectedItems();
    QVector<QPair<int, QString>> items;
    QList<QListWidgetItem*> actItems;
    auto add = [&](QListWidgetItem* it) {
        if (it && it->data(Qt::UserRole).isValid()) {
            items.append({it->data(Qt::UserRole).toInt(), it->data(Qt::UserRole + 1).toString()});
            actItems << it;
        }
    };
    // Operate on the selection if the clicked row is part of it, otherwise just the clicked row.
    if (under && (sel.isEmpty() || !sel.contains(under))) add(under);
    else for (QListWidgetItem* it : sel) add(it);
    if (items.isEmpty()) return;

    const int n = items.size();
    AppearanceMeta& am = AppearanceMeta::instance();
    QMenu menu(this);
    auto copy = [](const QStringList& l) { QGuiApplication::clipboard()->setText(l.join(QLatin1Char('\n'))); };
    auto prev = [](const QString& s) { return s.size() > 30 ? s.left(29) + QChar(0x2026) : s; };

    // Export queue count.
    const QString exCount = QStringLiteral("%1 item%2").arg(n).arg(n == 1 ? QString() : QStringLiteral("s"));
    // The destination is on screen in m_outDir, but the menu claimed "last dir" without saying
    // which — the card and part menus name it. Same helper so they cannot drift again.
    const QString exLastDir = ViewportPartMenu::condensePath(
        m_outDir ? m_outDir->text().trimmed() : QString());
    // exportSet*, not kExportModel*: this list holds textures as well as models (textureMode()),
    // so the label has to name the counted SET — "Export 12 items…", not "Export model… — 12 items".
    if (!exLastDir.isEmpty())
        menu.addAction(MenuText::exportSetLast(exCount, exLastDir), this,
                       [this, items] { doExtract(false, items); });
    menu.addAction(MenuText::exportSetPrompt(exCount), this, [this, items] { doExtract(true, items); });
    menu.addSeparator();

    // Copy block (previews for a single row; counts for a multi-selection).
    QStringList snoL, fileL, nameL, collL;
    for (const auto& it : items) {
        snoL << QString::number(it.first);
        fileL << it.second;
        const QString t = am.titleFor(it.first);
        nameL << (t.isEmpty() ? it.second : t);
        collL << am.collectionFor(it.first);
    }
    if (n == 1) {
        menu.addAction(QStringLiteral("%1  (%2)").arg(MenuText::kCopySno).arg(snoL.first()), this, [snoL, copy] { copy(snoL); });
        menu.addAction(QStringLiteral("%1  (%2)").arg(MenuText::kCopyFileName).arg(prev(fileL.first())), this, [fileL, copy] { copy(fileL); });
        menu.addAction(QStringLiteral("%1  (%2)").arg(MenuText::kCopyName).arg(prev(nameL.first())), this, [nameL, copy] { copy(nameL); });
        QAction* aC = menu.addAction(QStringLiteral("%1  (%2)").arg(MenuText::kCopyCollection).arg(prev(collL.first().isEmpty() ? QStringLiteral("—") : collL.first())), this, [collL, copy] { copy(collL); });
        aC->setEnabled(!collL.first().isEmpty());
    } else {
        menu.addAction(QStringLiteral("%1  —  %2 rows").arg(MenuText::kCopySno).arg(n), this, [snoL, copy] { copy(snoL); });
        menu.addAction(QStringLiteral("%1  —  %2 rows").arg(MenuText::kCopyFileName).arg(n), this, [fileL, copy] { copy(fileL); });
        menu.addAction(QStringLiteral("%1  —  %2 rows").arg(MenuText::kCopyName).arg(n), this, [nameL, copy] { copy(nameL); });
        menu.addAction(QStringLiteral("%1  —  %2 rows").arg(MenuText::kCopyCollection).arg(n), this, [collL, copy] { copy(collL); });
    }

    // Cross-tab (single model-mode row).
    if (n == 1 && !textureMode() && m_models) {
        const int sno = items.first().first;
        const QString nm = items.first().second;
        menu.addSeparator();
        menu.addAction(QStringLiteral("Open in Models"), this, [this, sno] {
            for (QWidget* w = parentWidget(); w; w = w->parentWidget())
                if (auto* tw = qobject_cast<QTabWidget*>(w)) { tw->setCurrentWidget(m_models); break; }
            m_models->selectModelBySno(sno);
        });
        menu.addAction(MenuText::kShowDeps, this, [this, sno, nm] {
            m_models->showModelDependencies(sno, nm);
        });
    }

    // Selection (only meaningful in manual-selection mode).
    if (m_showList && m_showList->isChecked()) {
        menu.addSeparator();
        menu.addAction(n > 1 ? QStringLiteral("Select these %1").arg(n) : QStringLiteral("Select"),
                       this, [actItems] { for (QListWidgetItem* it : actItems) it->setSelected(true); });
        menu.addAction(n > 1 ? QStringLiteral("Unselect these %1").arg(n) : QStringLiteral("Unselect"),
                       this, [actItems] { for (QListWidgetItem* it : actItems) it->setSelected(false); });
        menu.addAction(QStringLiteral("Select all"), this, [this] {
            if (m_showList && m_showList->isChecked()) queueAllMatches();   // incl. rows past the cap
            else m_list->selectAll();
        });
        menu.addAction(QStringLiteral("Unselect all"), this, [this] { m_list->clearSelection(); });
    }
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

void BulkExtractorTab::doExtract(bool promptDir, const QVector<QPair<int, QString>>& explicitItems)
{
    if (m_running) return;

    // ── Work set: an explicit set (context menu) if given, else the queue/all-matches. Computed
    // first so an empty result short-circuits before we bother prompting for a folder.
    QVector<QPair<int, QString>> matches = explicitItems.isEmpty() ? workSet() : explicitItems;
    if (matches.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Bulk extract"),
                                QStringLiteral("Nothing to extract — widen the filters or queue some items."));
        return;
    }

    // ── Destination. "Extract to…" prompts; "Extract Selected" reuses the remembered last dir (and
    // falls back to a prompt the first time, when nothing has been extracted yet).
    QString dir = m_outDir ? m_outDir->text().trimmed() : QString();
    if (promptDir || dir.isEmpty()) {
        const QString start = !dir.isEmpty() ? dir : QDir::homePath();
        const QString picked = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Extract to folder"), start);
        if (picked.isEmpty()) return;   // user canceled
        dir = picked;
        if (m_outDir) m_outDir->setText(dir);
        QSettings().setValue(QStringLiteral("bulk/outDir"), dir);
        loadFolderManifest();           // refresh the "already extracted" set for the new folder
    }

    const bool onlyNew = m_onlyNew && m_onlyNew->isChecked();
    const QString noun = textureMode() ? QStringLiteral("texture") : QStringLiteral("model");

    // Size guard + rough size estimate on large runs.
    if (matches.size() > 500) {
        qint64 bytes = 0;
        if (m_reader) for (const auto& it : matches) bytes += qint64(m_reader->payloadSize(quint64(it.first)));
        const double gb = double(bytes) / (1024.0 * 1024.0 * 1024.0);
        const QString est = bytes > 0 ? QStringLiteral(" (~%1 GB source data)").arg(gb, 0, 'f', gb < 1.0 ? 2 : 1)
                                      : QString();
        if (QMessageBox::question(this, QStringLiteral("Bulk extract"),
                QStringLiteral("This will extract up to %1 %2(s)%3 to:\n%4\n\nContinue?")
                    .arg(matches.size()).arg(noun).arg(est).arg(dir),
                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
    }

    // ── Run guard + console wiring. One run at a time; controls freeze, Cancel + console stay
    // live (the pipelines pump the event loop between items, which is what lets both work).
    m_running = true;
    m_cancelRequested = false;
    if (m_extractSelBtn) m_extractSelBtn->setEnabled(false);
    if (m_extractToBtn)  m_extractToBtn->setEnabled(false);
    if (m_copyBtn) m_copyBtn->setEnabled(false);
    if (m_mode) m_mode->setEnabled(false);   // mode switch mid-run would desync the manifest view
    m_progress->setRange(0, matches.size());
    m_progress->setValue(0);
    m_progress->setFormat(QStringLiteral("%v / %m"));
    m_progress->setVisible(true);
    m_cancelBtn->setVisible(true);
    m_cancelBtn->setEnabled(true);
    m_pauseRequested = false;
    m_pausedMs = 0;
    if (m_pauseBtn) { m_pauseBtn->setText(QStringLiteral("Pause")); m_pauseBtn->setVisible(true); }
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    QElapsedTimer runT; runT.start();

    // ── Sink: called from the WORKER thread — every GUI touch is marshaled to the GUI thread
    // (queued), so the pipelines never need to pump the event loop and the UI never freezes.
    auto sink = std::make_shared<BatchSink>();
    sink->progress = [this, startMs](int done, int total) {
        QMetaObject::invokeMethod(this, [this, done, total, startMs] {
            m_progress->setMaximum(total);
            m_progress->setValue(done);
            // Throughput + ETA — active time only (pauses excluded). Shown once the rate settles.
            const qint64 pausedNow = m_pauseRequested.load()
                ? m_pausedMs + (QDateTime::currentMSecsSinceEpoch() - m_pauseT0) : m_pausedMs;
            const double activeS = double(QDateTime::currentMSecsSinceEpoch() - startMs - pausedNow) / 1000.0;
            if (done >= 5 && activeS > 2.0) {
                const double rate = done / activeS;
                const int remainS = rate > 0.01 ? int((total - done) / rate) : 0;
                m_progress->setFormat(QStringLiteral("%v / %m  ·  %1/s  ·  ~%2 left")
                    .arg(rate, 0, 'f', rate < 10.0 ? 1 : 0)
                    .arg(remainS >= 3600
                             ? QStringLiteral("%1h %2m").arg(remainS / 3600).arg((remainS % 3600) / 60)
                             : remainS >= 60 ? QStringLiteral("%1m %2s").arg(remainS / 60).arg(remainS % 60)
                                             : QStringLiteral("%1s").arg(remainS)));
            }
        }, Qt::QueuedConnection);
    };
    sink->log = [this](const QString& line) {
        QMetaObject::invokeMethod(this, [this, line] { logLine(line); }, Qt::QueuedConnection);
    };
    // The cancel poll doubles as the pause gate: while paused, workers sleep HERE (between items),
    // so pausing is instant and resume continues exactly where it stopped. Cancel breaks the hold.
    sink->canceled = [this] {
        while (m_pauseRequested.load() && !m_cancelRequested.load())
            QThread::msleep(100);
        return m_cancelRequested.load();
    };

    logLine(QStringLiteral("── %1 run: %2 match(es) → %3")
                .arg(textureMode() ? QStringLiteral("Texture") : QStringLiteral("Model"))
                .arg(matches.size()).arg(dir));
    logLine(QStringLiteral("   existing: %1 · layout: %2")
                .arg(onlyNew ? QStringLiteral("only new") : QStringLiteral("overwrite"),
                     m_organize && !m_organize->currentData().toString().isEmpty()
                         ? m_organize->currentText() : QStringLiteral("flat")));

    const int manBefore = m_folderDone.size();
    QDir().mkpath(dir);

    // Everything the worker needs, resolved NOW on the GUI thread (subfolder grouping reads
    // AppearanceMeta; the widget states can't be read from the worker).
    QString org = m_organize ? m_organize->currentData().toString() : QString();
    // Masked, not rewritten. A layout selected for model runs stays selected; it simply does not
    // apply to this one, and the log says so rather than producing a silent single "_misc" folder.
    if (textureMode() && !org.isEmpty()) {
        logLine(QStringLiteral("── layout: %1 groups by appearance, which textures are not — "
                               "this run is flat").arg(m_organize->currentText()));
        org.clear();
    }
    // Grouped HERE rather than inside exportModels(): the buffer and name-matched-texture passes
    // below need the same folders, so the split has to exist before any of them run. exportModels()
    // is told not to group again (applyLayout=false in bulkExport).
    const QVector<ExportLayout::Group> groups =
        org.isEmpty() ? QVector<ExportLayout::Group>() : ExportLayout::group(org, matches);
    const bool texMode  = textureMode();
    // Read from QSettings, not from a widget: the checkbox lives in Settings ▸ Export now.
    const bool wantCoTex = !texMode && QSettings().value(QStringLiteral("bulk/coTextures"), false).toBool()
                           && m_textures && m_index;
    const bool wantBuffers = QSettings().value(QStringLiteral("bulk/buffers"), false).toBool();
    const bool wantReport  = QSettings().value(QStringLiteral("bulk/report"), false).toBool();

    // ── Worker thread: the entire extract pipeline runs OFF the GUI thread, so the window stays
    // fully responsive during large runs and Cancel reacts instantly. Per-item SEH guards live in
    // the pipelines; sink callbacks marshal GUI updates back (queued).
    std::thread([this, sink, matches, dir, onlyNew, noun, org, groups, texMode,
                 wantCoTex, wantBuffers, wantReport, manBefore, runT]() mutable {
        auto runOne = [&](const QVector<QPair<int, QString>>& items, const QString& d) {
            if (texMode) { if (m_textures) m_textures->bulkExportTextures(items, d, onlyNew, sink.get()); }
            else         { if (m_models)   m_models->bulkExport(items, d, onlyNew, sink.get()); }
        };

        // ── One list of (folder, items), so every pass below follows the chosen layout ──────────
        // Previously only runOne() got the group folder; the co-texture and buffer passes used the
        // RUN ROOT, so no layout choice could reach them — every buffer from a by-Class run landed
        // in one buffers/ folder at the top while the models themselves were correctly split. Flat
        // is now just a single unnamed group, which removes the special case entirely.
        QVector<QPair<QString, QVector<QPair<int, QString>>>> passes;
        if (groups.isEmpty()) {
            passes.append({dir, matches});
        } else {
            for (const ExportLayout::Group& g : groups)
                passes.append({ExportLayout::folderFor(dir, g), g.items});
        }

        // Name-matched textures: which textures belong to which model, resolved ONCE for the whole
        // run and bucketed by the stem that matched. Doing the sweep inside the per-folder loop
        // instead would re-scan the entire texture group per folder — harmless for by-Class, but
        // by-Model has a folder per model, so a 500-model run would scan it 500 times.
        QHash<QString, QVector<QPair<int, QString>>> texByStem;
        if (!m_cancelRequested.load() && wantCoTex) {
            for (const SnoEntry& te : m_index->entries(kTextureGroup))
                for (const auto& it : matches)
                    if (te.name.startsWith(it.second, Qt::CaseInsensitive)) {
                        texByStem[it.second].append({te.snoId, te.name});
                        break;   // first match owns it, exactly as before
                    }
        }

        const QString bufExt = texMode ? QStringLiteral(".tex") : QStringLiteral(".app");

        // One texture-decode cache spanning every folder in the run. exportModels() opens its own,
        // but the pass loop calls it once per folder — so under "Subfolders by Model" that scope
        // would be created and destroyed once per model and share nothing across them, which is
        // precisely the case the cache exists for. Nesting is reference-counted, so the inner
        // scopes borrow this one and it is freed when the run ends.
        MaterialDecode::TextureCacheScope runCache;

        for (const auto& pass : passes) {
            if (sink->canceled()) break;   // also holds here while paused
            const QString& outDir = pass.first;
            const QVector<QPair<int, QString>>& items = pass.second;
            QDir().mkpath(outDir);
            // Logged whenever a layout is in force, not only when it produced several folders. A
            // one-group run is exactly when you most need telling — a by-Class run started before
            // AppearanceMeta is ready files EVERYTHING under _misc, and silence made that look like
            // a flat run that had simply gone to the wrong place.
            if (!groups.isEmpty() && sink->log)
                sink->log(QStringLiteral("── subfolder %1 (%2 item(s))")
                              .arg(QFileInfo(outDir).fileName()).arg(items.size()));
            runOne(items, outDir);

            // Name-matched textures for THIS folder's models, into its own textures/ — the same
            // subfolder writeLooseTextures() uses, which is deliberate: they are different views of
            // the same model's maps and their filenames cannot collide.
            if (!m_cancelRequested.load() && wantCoTex) {
                QVector<QPair<int, QString>> texJobs;
                for (const auto& it : items) texJobs += texByStem.value(it.second);
                if (!texJobs.isEmpty()) {
                    if (sink->log)
                        sink->log(QStringLiteral("   name-matched textures: %1 → textures/").arg(texJobs.size()));
                    const QString texDir = QDir(outDir).filePath(QStringLiteral("textures"));
                    QDir().mkpath(texDir);
                    m_textures->bulkExportTextures(texJobs, texDir, onlyNew, sink.get());
                }
            }

            // Raw payloads for THIS folder's items, into its own buffers/.
            if (!m_cancelRequested.load() && m_reader && m_reader->isReady() && wantBuffers) {
                const QString bufDir = QDir(outDir).filePath(QStringLiteral("buffers"));
                QDir().mkpath(bufDir);
                int wrote = 0;
                for (const auto& it : items) {
                    if (sink->canceled()) break;
                    const QByteArray raw = m_reader->readPayloadBySno(quint64(it.first));
                    if (raw.isEmpty()) continue;
                    QFile f(QDir(bufDir).filePath(it.second + bufExt));
                    if (f.open(QIODevice::WriteOnly)) { f.write(raw); ++wrote; }
                }
                // Reported even at zero: "I ticked buffers and got nothing" has to be
                // distinguishable from "the tick did not stick".
                if (sink->log)
                    sink->log(QStringLiteral("   buffers: %1 raw %2 file(s) → buffers/").arg(wrote).arg(bufExt));
            }
        }

        // ── Finish: back on the GUI thread for manifest reload, report, summary, toast + resets.
        QMetaObject::invokeMethod(this, [this, sink, dir, org, noun, manBefore, runT, matches, wantReport] {
            loadFolderManifest();
            // Report AFTER the manifest reload so its ok/missing statuses reflect THIS run.
            if (wantReport) {
                writeRunReport(dir, matches);
                logLine(QStringLiteral("── report: _bulk_report.csv written"));
            }
            const bool canceled = m_cancelRequested.load();
            QString summary;
            if (org.isEmpty()) {
                summary = QStringLiteral("Last run: %1 new %2(s) → %3")
                              .arg(qMax(0, m_folderDone.size() - manBefore)).arg(noun).arg(dir);
                if (QFileInfo::exists(QDir(dir).filePath(QStringLiteral("_bulk_failed.txt"))))
                    summary += QStringLiteral("   ·   some failed (see _bulk_failed.txt)");
            } else {
                summary = QStringLiteral("Last run: extracted to subfolders under %1").arg(dir);
            }
            if (canceled) summary += QStringLiteral("   ·   CANCELED");
            logLine(QStringLiteral("── done in %1 s.  %2")
                        .arg(runT.elapsed() / 1000.0, 0, 'f', 1).arg(summary));
            if (m_lastRun) { m_lastRun->setText(summary); m_lastRun->setVisible(true); }
            {
                QString shortMsg = org.isEmpty()
                    ? QStringLiteral("Extracted %1 new %2(s)").arg(qMax(0, m_folderDone.size() - manBefore)).arg(noun)
                    : QStringLiteral("Extracted to subfolders");
                if (canceled) shortMsg.prepend(QStringLiteral("Canceled — "));
                ExportNotifier::instance().notify(shortMsg, dir);
            }
            m_running = false;
            m_cancelRequested = false;
            m_pauseRequested = false;
            if (m_pauseBtn) { m_pauseBtn->setVisible(false); m_pauseBtn->setText(QStringLiteral("Pause")); }
            if (m_copyBtn) m_copyBtn->setEnabled(true);
            if (m_mode) m_mode->setEnabled(true);
            m_cancelBtn->setVisible(false);   // the bar stays, showing the final count
            m_cancelBtn->setText(QStringLiteral("Cancel"));
            m_cancelBtn->setEnabled(true);
            updateCount();   // re-enables the extract buttons + refreshes markers, PRESERVING ticks
        }, Qt::QueuedConnection);
    }).detach();
}

// Timestamped console append, pinned to the bottom so the newest line is always in view.
void BulkExtractorTab::logLine(const QString& s)
{
    if (!m_console) return;
    m_console->appendPlainText(QStringLiteral("[%1] %2")
        .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), s));
    m_console->verticalScrollBar()->setValue(m_console->verticalScrollBar()->maximum());
}

QString BulkExtractorTab::subfolderFor(int sno) const
{
    return ExportLayout::tagFolder(m_organize ? m_organize->currentData().toString() : QString(), sno);
}

// Point the combo at whatever export/folderLayout currently says. Called at construction and from
// onSettingsChanged(), because this control is now MIRRORED — the same key is editable in
// Settings ▸ Export ▸ Models, and two widgets on one key must never be able to disagree.
void BulkExtractorTab::syncLayoutCombo()
{
    if (!m_organize) return;
    m_syncingLayout = true;
    const int i = m_organize->findData(ExportLayout::mode());
    m_organize->setCurrentIndex(i >= 0 ? i : 0);
    m_syncingLayout = false;
    updateLayoutModeAvailability();
}

// Class, Type and Model all mean something only for appearances. Class/Type resolve through
// AppearanceMeta, which is keyed by APPEARANCE sno, so a texture sno matches nothing and the whole
// run lands in one "_misc" folder; by-Model would give a folder per texture holding one image, its
// name built by applying the MODEL name template to a texture. Grey all three out in Textures mode.
//
// Greyed out, and NOTHING ELSE. An earlier version also snapped the combo to Flat, which wrote the
// shared key — so merely switching the Bulk tab to Textures silently destroyed the layout choice
// for the Models tab too, and opening Settings while the Bulk tab sat in Textures mode reset the
// value the user had just picked. The selection is masked for the RUN instead (see doExtract), so
// the setting survives and comes back when the mode does.
void BulkExtractorTab::updateLayoutModeAvailability()
{
    if (!m_organize) return;
    const bool tex = textureMode();
    auto* model = qobject_cast<QStandardItemModel*>(m_organize->model());
    if (!model) return;   // a custom model would make the flags below meaningless; doExtract still masks
    for (int i = 0; i < m_organize->count(); ++i) {
        const QString id = m_organize->itemData(i).toString();
        const bool off = tex && !id.isEmpty();   // everything except Flat
        if (QStandardItem* item = model->item(i))
            item->setFlags(off ? item->flags() & ~Qt::ItemIsEnabled
                               : item->flags() | Qt::ItemIsEnabled);
        // Cleared again when it no longer applies — a stale "unavailable because…" on an enabled
        // item is worse than none.
        m_organize->setItemData(i, off ? QStringLiteral("Only Flat applies to a Textures run: the "
                                                        "other layouts group by appearance, and "
                                                        "textures are not appearances.")
                                       : QString(), Qt::ToolTipRole);
    }
}

void BulkExtractorTab::loadFolderManifest()
{
    m_folderDone.clear();
    m_folderStems.clear();
    const QString dir = m_outDir ? m_outDir->text().trimmed() : QString();
    if (dir.isEmpty()) return;
    // The root, THEN one level of subfolders. bulkExport writes its ledger into the folder it was
    // given, which under any layout other than Flat is a subfolder — so reading only the root found
    // nothing, every row of the run report came out "missing, 0 bytes", and no item was ever marked
    // already-extracted. One level is enough: layouts nest exactly one deep.
    QStringList roots{ dir };
    for (const QFileInfo& sub : QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        roots << sub.absoluteFilePath();
    for (const QString& r : roots) {
        QFile f(QDir(r).filePath(QStringLiteral("_bulk_manifest.json")));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonArray man = QJsonDocument::fromJson(f.readAll()).array();
            for (const QJsonValue& v : man) m_folderDone.insert(v.toObject().value(QStringLiteral("sno")).toInt());
        }
        // Also treat any file already on disk (any extension) as "present", matching the skip logic.
        for (const QFileInfo& fi : QDir(r).entryInfoList(QDir::Files))
            m_folderStems.insert(fi.completeBaseName());
    }
}

// ── Factory presets ─────────────────────────────────────────────────────────────────────────────
//
// Curated one-click extractions, generated from ItemDef's canonical class table so Paladin/Warlock
// (and any class after them) get their presets the moment the table gains a row.
//
// Vocabulary (measured against the game's own naming, see "Global & Base Appearance.txt"):
//   {cls}{f|m}_h##       hair                        jwl##_{cls}{f|m}   jewelry (all classes)
//   {cls}{f|m}_test999_* base "naked" armour slots   global_*           markings / facial hair
//   {cls}{f|m}_p##_(bod|hed)_*  skin textures        base_eyes/baseeye  eyes, MakeUp_ makeup
//
// Class/armor/weapon presets use TAGS, not name patterns — the same "Barbarian"/"armor"/"weapon"
// tags the Models tab's own Class and Category filters test, so a preset always agrees with what
// filtering by hand shows. Only the Global & Base pair needs name unions ('|'), because
// customization files span several naming families that no tag covers.
const QVector<BulkExtractorTab::FactoryPreset>& BulkExtractorTab::factoryPresets()
{
    static const QVector<FactoryPreset> v = [] {
        QVector<FactoryPreset> out;

        // Name unions across every class, built from the canonical table. All verified against
        // the d4data Appearance/Texture listings (2026-08 audit):
        //   hair      {cls}{f|m}_H00–H19 — "{c}f_h" collides with nothing else (checked)
        //   bodies    {cls}{F|M}_base00 EXACTLY — the naked body+head rig; base01+ are the
        //             slot-suffixed starter ARMOUR looks and are deliberately not matched
        //   skins     {cls}{f|m}_P0#_BOD/HED — plus a few genderless odd-outs ({cls}_P0#,
        //             druid{M|F}_P00) the per-class token also covers
        QStringList hairAlts, skinAlts, bodyAlts;
        for (int i = 0; i < ItemDef::HeroClassCount; ++i) {
            const QString c = QString::fromLatin1(ItemDef::heroClassCode(i));
            hairAlts << c + QStringLiteral("f_h") << c + QStringLiteral("m_h");
            skinAlts << c + QStringLiteral("f_p0") << c + QStringLiteral("m_p0")
                     << c + QStringLiteral("_p0");   // genderless sheets (rog_P00_HED_…)
            bodyAlts << c + QStringLiteral("f_base00") << c + QStringLiteral("m_base00");
        }
        // The one family named off-scheme: druidM_P00_HED_eyelash spells the class out in full.
        skinAlts << QStringLiteral("druidf_p0") << QStringLiteral("druidm_p0");

        // Models: body/head rigs + hair + jewelry + the test999 base-body armour slots.
        out.append({QStringLiteral("All Global & Base Appearance"), 0,
                    (QStringList() << bodyAlts << hairAlts
                                   << QStringLiteral("jwl") << QStringLiteral("test999"))
                        .join(QLatin1Char('|')),
                    {}, false, /*coTex*/ true});
        // Textures: everything the character creator draws — skins (P##_BOD/HED), hair sheets,
        // jewelry sheets, markings/facial hair/eyeshadow (global_*), eyes, teeth, makeup, lashes.
        out.append({QStringLiteral("All Global & Base Textures"), 1,
                    (QStringList() << skinAlts << hairAlts
                                   << QStringLiteral("global_") << QStringLiteral("jwl")
                                   << QStringLiteral("base_eyes") << QStringLiteral("baseeye")
                                   << QStringLiteral("base_teeth") << QStringLiteral("makeup_")
                                   << QStringLiteral("eyelash"))
                        .join(QLatin1Char('|')),
                    {}, false});

        // Per class: everything tagged with the class, the armour-only subset, and the class's
        // texture sheets. Textures (group 44) carry no tags, so that preset is name-based: every
        // class-owned sheet starts {cls}{f|m}_ (skins, hair, armour looks) — and because matching
        // is contains(), it also catches the class token mid-name (jwl00_barF_…). The bare code
        // without "_" is NOT used: "barm" would match unrelated names like "barmaid".
        for (int i = 0; i < ItemDef::HeroClassCount; ++i) {
            const QString n = QString::fromLatin1(ItemDef::heroClassName(i));
            const QString c = QString::fromLatin1(ItemDef::heroClassCode(i));
            out.append({QStringLiteral("All %1 Appearance").arg(n), 0, QString(), {n}, false,
                        /*coTex*/ true});
            out.append({QStringLiteral("All %1 Armor").arg(n), 0, QString(),
                        {n, QStringLiteral("armor")}, false, /*coTex*/ true});
            // "{c}_p0" adds the class's genderless skin sheets (rog_P00_HED_…); Druid also ships
            // sheets spelling the class in full (druidM_P00_HED_eyelash).
            QString texQ = c + QStringLiteral("f_|") + c + QStringLiteral("m_|")
                         + c + QStringLiteral("_p0");
            if (c == QLatin1String("dru")) texQ += QStringLiteral("|druidf_p0|druidm_p0");
            out.append({QStringLiteral("All %1 Textures").arg(n), 1, texQ, {}, false});
        }

        // Weapon TEXTURES have no class or category marker of their own — they are named after
        // their model. Loose textures is therefore the only route the data supports, which is why
        // there is no separate "All Weapons Textures" preset: this IS it.
        out.append({QStringLiteral("All Weapons"), 0, QString(), {QStringLiteral("weapon")}, false,
                    /*coTex*/ true});
        return out;
    }();
    return v;
}

void BulkExtractorTab::applyFactoryPreset(const FactoryPreset& p)
{
    m_loadingPreset = true;
    if (m_mode) m_mode->setCurrentIndex(qBound(0, p.mode, 1));
    if (m_name) m_name->setText(p.query);
    m_catFacet.clear();
    m_tagFilter = QSet<QString>(p.tags.begin(), p.tags.end());
    m_tagOrMode = p.tagOr;
    // Textures mode ignores this option entirely, so only assert it for MODEL presets — a texture
    // preset must not silently clear a setting it has no opinion about.
    //
    // Writes the key directly now that the checkbox lives in Settings ▸ Export. Unchanged in
    // effect: the old checkbox persisted on toggle, so choosing a preset always did rewrite this
    // preference — it is just visible in a different place afterwards.
    if (p.mode == 0 && p.coTex)
        QSettings().setValue(QStringLiteral("bulk/coTextures"), true);
    refillTagPanel();
    updateTagBtn();
    m_loadingPreset = false;
    updateCount();
}

QString BulkExtractorTab::auditPresets()
{
    if (!m_models || !m_index) return QStringLiteral("presets: index not ready");
    const auto& fp = factoryPresets();
    QFile f(AppPaths::dataDir() + QStringLiteral("/preset_audit.txt"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return QStringLiteral("presets: cannot write preset_audit.txt");
    QTextStream ts(&f);
    ts << "BULK EXTRACT — BUILT-IN PRESET COVERAGE\n"
       << "=======================================\n\n"
       << "Each preset resolved through the SAME matcher the tab uses. A count of 0 means the\n"
       << "query matches nothing (typo, or the game renamed that family); an implausibly large\n"
       << "count means a pattern is too loose. Both are invisible in the UI until you extract.\n\n";
    int empty = 0;
    for (const auto& p : fp) {
        const auto m = matchesFor(p.mode == 1, p.query, {}, QString(),
                                  QSet<QString>(p.tags.begin(), p.tags.end()), p.tagOr, false);
        if (m.isEmpty()) ++empty;
        ts << QStringLiteral("%1  %2  %3\n")
                  .arg(m.isEmpty() ? QStringLiteral("EMPTY ") : QStringLiteral("      "))
                  .arg(QString::number(m.size()).rightJustified(6))
                  .arg(p.name);
        // Three examples make a wrong-but-non-empty preset visible too: "All Warlock Armor"
        // returning barbarian pieces is a count that looks perfectly healthy.
        for (int i = 0; i < qMin(3, m.size()); ++i)
            ts << QStringLiteral("               · %1\n").arg(m[i].second);
    }
    ts << QStringLiteral("\n%1 preset(s), %2 empty.\n").arg(fp.size()).arg(empty);
    f.close();
    const QString sum = QStringLiteral("presets: %1 built-in, %2 empty — data\\preset_audit.txt")
                            .arg(fp.size()).arg(empty);
    qInfo().noquote() << sum;
    return sum;
}

void BulkExtractorTab::refreshPresets()
{
    if (!m_preset) return;
    QSettings s;
    s.beginGroup(QStringLiteral("bulk/presets"));
    const QStringList names = s.childGroups();
    s.endGroup();
    m_loadingPreset = true;
    m_preset->blockSignals(true);
    m_preset->clear();
    m_preset->addItem(QStringLiteral("(load preset…)"), QString());
    // Built-ins first, keyed "*<index>" — a namespace no user preset can occupy, because saved
    // presets are keyed by their (non-empty, QSettings-group) NAME. Then the user's own.
    const auto& fp = factoryPresets();
    for (int i = 0; i < fp.size(); ++i)
        m_preset->addItem(QStringLiteral("★ ") + fp[i].name, QStringLiteral("*%1").arg(i));
    if (!fp.isEmpty() && !names.isEmpty()) m_preset->insertSeparator(m_preset->count());
    for (const QString& n : names) m_preset->addItem(n, n);
    m_preset->setCurrentIndex(0);
    m_preset->blockSignals(false);
    m_loadingPreset = false;
}

void BulkExtractorTab::savePreset()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Save preset"),
        QStringLiteral("Preset name:"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    QSettings s;
    s.beginGroup(QStringLiteral("bulk/presets/") + name);
    s.setValue(QStringLiteral("mode"),     m_mode->currentIndex());
    s.setValue(QStringLiteral("name"),     m_name->text());
    s.setValue(QStringLiteral("facet"),    m_catFacet);
    s.setValue(QStringLiteral("tags"),     QStringList(m_tagFilter.values()));
    s.setValue(QStringLiteral("tagOr"),    m_tagOrMode);
    s.setValue(QStringLiteral("hide"),     m_hideUnrend);
    s.setValue(QStringLiteral("organize"), m_organize->currentData().toString());
    s.setValue(QStringLiteral("outDir"),   m_outDir ? m_outDir->text() : QString());   // remember the folder too
    s.endGroup();
    refreshPresets();
    const int i = m_preset->findData(name);
    if (i >= 0) { m_loadingPreset = true; m_preset->setCurrentIndex(i); m_loadingPreset = false; }
}

void BulkExtractorTab::deletePreset()
{
    const QString name = m_preset ? m_preset->currentData().toString() : QString();
    if (name.isEmpty()) return;
    if (name.startsWith(QLatin1Char('*'))) {   // factory presets are code, not settings
        if (m_count) m_count->setText(QStringLiteral("Built-in presets can't be deleted."));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("Delete preset"),
            QStringLiteral("Delete preset \"%1\"?").arg(name)) != QMessageBox::Yes) return;
    QSettings().remove(QStringLiteral("bulk/presets/") + name);
    refreshPresets();
}

void BulkExtractorTab::loadPreset(const QString& name)
{
    if (name.isEmpty()) return;
    if (name.startsWith(QLatin1Char('*'))) {   // "*<index>" = factory preset
        const int i = name.mid(1).toInt();
        const auto& fp = factoryPresets();
        if (i >= 0 && i < fp.size()) applyFactoryPreset(fp[i]);
        return;
    }
    QSettings s;
    s.beginGroup(QStringLiteral("bulk/presets/") + name);
    if (s.childKeys().isEmpty()) { s.endGroup(); return; }
    m_loadingPreset = true;
    m_mode->setCurrentIndex(qBound(0, s.value(QStringLiteral("mode"), 0).toInt(), 1));
    m_name->setText(s.value(QStringLiteral("name")).toString());
    m_catFacet = s.value(QStringLiteral("facet")).toString();
    const QStringList tags = s.value(QStringLiteral("tags")).toStringList();
    m_tagFilter = QSet<QString>(tags.begin(), tags.end());
    m_tagOrMode = s.value(QStringLiteral("tagOr"), false).toBool();
    m_hideUnrend = s.value(QStringLiteral("hide"), false).toBool();
    refillTagPanel();
    updateTagBtn();
    // Presets store the layout id, not an index — same reasoning as the key itself. A preset saved
    // by an older build holds the combo INDEX; accept that rather than silently dropping the choice.
    {
        const QVariant v = s.value(QStringLiteral("organize"));
        bool wasInt = false;
        const int legacy = v.toString().toInt(&wasInt);
        const QString id = wasInt ? (legacy == 1 ? ExportLayout::kClass()
                                   : legacy == 2 ? ExportLayout::kType()
                                   : legacy == 3 ? ExportLayout::kModel() : ExportLayout::kFlat())
                                  : v.toString();
        const int i = m_organize->findData(id);
        // Guarded: this combo is bound to a GLOBAL key now, so an unguarded setCurrentIndex would
        // make loading a bulk preset silently repoint every other tab's exports too.
        m_syncingLayout = true;
        m_organize->setCurrentIndex(i >= 0 ? i : 0);
        m_syncingLayout = false;
    }
    const QString od = s.value(QStringLiteral("outDir")).toString();
    if (!od.isEmpty() && m_outDir) {
        m_outDir->setText(od);
        QSettings().setValue(QStringLiteral("bulk/outDir"), od);   // becomes the new "last dir"
    }
    s.endGroup();
    m_loadingPreset = false;
    loadFolderManifest();
    updateCount();
}
