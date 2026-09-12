#include "tabs/StableTab2.h"
#include "util/CameraOrbitRow.h"
#include "util/LookIcon.h"
#include "util/ViewportPartMenu.h"

#include "app/ExportNotifier.h"
#include "util/HoverInfo.h"
#include "util/AnimExportScope.h"   // which animation sources an export embeds
#include "util/PanelPersist.h"

#include <QElapsedTimer>

#include "app/AppPaths.h"

#include "app/Config.h"
#include "app/SehGuard.h"
#include "casc/CascReader.h"
#include "gl/GLModelWidget.h"
#include "index/AppearanceMeta.h"
#include "index/IconIndex.h"
#include "index/SnoIndex.h"
#include "model/Attachments.h"
#include "model/Hardpoints.h"
#include "model/Material.h"          // parseMaterialJson / MatTexture (raw-source count)
#include "model/MaterialDecode.h"
#include "model/MaterialReport.h"
#include "util/TextReportDialog.h"
#include "model/ModelParser.h"
#include "model/Retarget.h"
#include "tabs/HintBar.h"
#include "tabs/IconBadge.h"
#include "tabs/PanelBox.h"
#include "tabs/ViewGlyphs.h"   // shadeBallGlyph · stripGlyph · overlayGlyph (shared toolbar icons)

#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDataStream>
#include <QDateTime>

#include <cmath>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QHideEvent>
#include <QIcon>
#include <QInputDialog>
#include <QMenu>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMatrix4x4>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QSet>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidgetAction>

#include <algorithm>
#include <functional>
#include <thread>

namespace {
constexpr int kGroupAppearance = 9;

// Card selection borders — same visual language as the Wardrobe/Models picker grids.
inline constexpr const char* kCardBaseQss =
    "QToolButton{border:1px solid #444;border-radius:4px;background:#2b2b2b;color:#cfcfcf;}"
    "QToolButton:hover{border-color:#b0453c;}"
    "QToolButton:checked{border:2px solid #ffffff;border-radius:4px;}";

// Responsive card grid metrics (mirrors WardrobeTab2::cardMetrics): columns from available
// width, a bounded card width, and the same portrait aspect the other tabs use.
void cardMetrics(int availW, int& cols, int& cardW, int& cardH, int& iconW)
{
    const int spacing = 4, prefW = 100;   // 100px cards → a default column fits ~4 across
    if (availW < 80) availW = 440;
    cols  = qMax(1, (availW + spacing) / (prefW + spacing));
    cardW = qBound(92, (availW - spacing * (cols + 1)) / cols, 150);
    cardH = cardW * 150 / 132;
    iconW = cardW - 16;
}

// Species tokens that appear in mount appearance names (mnt_<...>_<token>). The internal
// data name for Basilisks is "chimera" — the UI always shows the in-game label "Basilisk".
struct Species { const char* label; const char* token; };
const Species kSpecies[] = { { "Horse", "horse" }, { "Cat", "cat" }, { "Basilisk", "chimera" } };

// Exclude FX / sub-mesh / simulation-fragment appearances from the pickable lists.
bool looksLikeFxFragment(const QString& lower)
{
    for (const char* bad : { "_fx", "burst", "projection", "sprint", "trail", "_proj", "_glow" })
        if (lower.contains(QLatin1String(bad))) return true;
    return false;
}

// The three viewport enums this tab persists are the RENDERER's contract values, not widget
// positions — GLModelWidget::setEnvironment and setViewChannel document their own numbering, and
// the Models and Wardrobe tabs store the identical values under their own prefixes. So the stored
// int is right and needs no migration. What was missing is validation: a value outside the range
// (a hand-edited INI, a profile written by a newer build) went straight to a shader uniform, while
// setCurrentIndex() on that same int silently yielded -1 — a blank combo showing nothing while the
// viewport rendered whatever the bad number meant. Read every one of them through these.
int envOrDefault(int v)         { return (v >= 0 && v <= 3) ? v : 1; }   // 0 Studio 1 Outdoor 2 Dungeon 3 Night
int lightPresetOrDefault(int v) { return (v >= 0 && v <= 2) ? v : 1; }   // 1 = Hero Direct
int channelOrDefault(int v)     { return (v >= 0 && v <= 8) ? v : 0; }   // 0 shaded … 8 dye zones

// Select the entry whose userData is `value`. The combos below carry their enum value as DATA
// rather than relying on their position matching it: the binding was implicit, so inserting one
// item would have silently re-pointed every saved value AND every setEnvironment/setViewChannel
// call. findData is inert to order; falls back to `def`, then to the first row.
void selectByValue(QComboBox* cb, int value, int def)
{
    if (!cb) return;
    int i = cb->findData(value);
    if (i < 0) i = cb->findData(def);
    cb->setCurrentIndex(i < 0 ? 0 : i);
}

// The ONE place an Appearance .app.json path is built in this tab. Consolidated so the direct-JSON
// read count verify-src tracks reflects real read SITES rather than repeated string building, and
// so the eventual move to the CASC meta binary (which is what encrypted appearances need) has a
// single path to change instead of four.
QString apprJsonPath(const QString& d4, const QString& appr)
{
    return QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4, appr);
}

// Trailing "_token" of a lowercased appearance name.
QString lastTok(const QString& lower)
{
    const int u = lower.lastIndexOf(QLatin1Char('_'));
    return (u >= 0 && u + 1 < lower.size()) ? lower.mid(u + 1) : QString();
}

// Species category of a mount name: the trailing token with its variant number stripped
// (e.g. "mnt_base00_horse26" → "horse", "mnt_base00_cat" → "cat").
QString catOf(const QString& lower)
{
    QString t = lastTok(lower);
    while (!t.isEmpty() && t.back().isDigit()) t.chop(1);
    return t;
}

// Is this a real species token (not a structural name segment)?
bool isSpeciesTok(const QString& tok)
{
    static const QStringList kSkip = { QStringLiteral("base"), QStringLiteral("amor"),
                                       QStringLiteral("armor"), QStringLiteral("trophy"),
                                       QStringLiteral("mnt") };
    return tok.size() >= 3 && !tok.at(0).isDigit() && !kSkip.contains(tok);
}

// Category display order: known species first (kSpecies order), other species next, pets last.
int catRank(const QString& cat)
{
    if (cat == QLatin1String("pet")) return 100;
    for (int i = 0; i < int(sizeof(kSpecies) / sizeof(kSpecies[0])); ++i)
        if (cat == QLatin1String(kSpecies[i].token)) return i;
    return 50;
}

// Localized Name/Description from a StringList
// (enUS_Text/meta/StringList/<stem>.stl.json → arStrings[]{szLabel, szText}); e.g. stem
// "Item_MountReins_DarkHorse" or "Actor_Mount_DarkHorse".
void readStrings(const QString& d4, const QString& stem, QString& name, QString& desc)
{
    QFile f(QStringLiteral("%1/json/enUS_Text/meta/StringList/%2.stl.json").arg(d4, stem));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).object()
                               .value(QStringLiteral("arStrings")).toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        const QString label = o.value(QStringLiteral("szLabel")).toString();
        if (label == QLatin1String("Name"))        name = o.value(QStringLiteral("szText")).toString();
        else if (label == QLatin1String("Description")) desc = o.value(QStringLiteral("szText")).toString();
    }
}
}  // namespace

QString StableTab2::typeToken(int type)
{
    switch (type) {
    case 0: return QStringLiteral("horse");
    case 1: return QStringLiteral("cat");
    case 2: return QStringLiteral("chimera");
    default: return QString();
    }
}

QString StableTab2::typeLabel(const QString& token)
{
    if (token == QLatin1String("chimera")) return QStringLiteral("Basilisk");   // in-game name
    if (token == QLatin1String("pet"))     return QStringLiteral("Pet");
    if (token.isEmpty()) return token;
    return token.left(1).toUpper() + token.mid(1);
}

StableTab2::StableTab2(QWidget* parent) : BrowserTab(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    if (QWidget* hint = makeHintBar(this,
            QStringLiteral("Tip: pick a Mount, then Mount Armor/Trophy · Basilisks have no armor · "
                           "double-click a part to isolate · ⛶ fullscreen"),
            "hints/stable"))
        root->addWidget(hint);
    auto* split = new QSplitter(Qt::Horizontal, this);
    root->addWidget(split, 1);

    // ── Left: controls ───────────────────────────────────────────────────────
    auto* left = new QWidget;
    auto* ll = new QVBoxLayout(left);
    ll->setContentsMargins(8, 8, 8, 8);
    ll->setSpacing(5);

    ll->addWidget(new QLabel(QStringLiteral("MOUNT")));
    // (Category combo removed — the grid shows ALL mounts and pets at once, grouped under
    //  Horse / Cat / Basilisk / Pet section headers.)

    // Three slot cells: Mount body / Barding / Trophy.
    m_slotCellGroup = new QButtonGroup(this);
    m_slotCellGroup->setExclusive(true);
    auto* cellRow = new QHBoxLayout();
    cellRow->setSpacing(4);
    static const char* kSlotLabels[SlotCount] = { "Mount", "Mount Armor", "Trophy" };
    for (int i = 0; i < SlotCount; ++i) {
        auto* b = new QToolButton;
        b->setCheckable(true);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setIconSize(QSize(40, 40));
        b->setFixedSize(66, 72);   // matches the Wardrobe creator cells
        b->setStyleSheet(QStringLiteral("QToolButton{font-size:9px;}"));
        b->setText(QString::fromLatin1(kSlotLabels[i]));
        b->setToolTip(QStringLiteral("Select the %1 slot").arg(QString::fromLatin1(kSlotLabels[i])));
        m_slotCell[i] = b;
        // Same menu the Wardrobe slot cells carry, for the same reason: a slot cell is a picture
        // of one equipped item. Clear first — it is the action about the SLOT rather than the item
        // in it, and the one people reach for.
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(b, &QWidget::customContextMenuRequested, this, [this, i, b](const QPoint& p) {
            QMenu menu;
            const int sno = m_slotSel[i];
            QAction* aClear = menu.addAction(QStringLiteral("Clear"), this, [this, i] {
                pushUndo();
                m_slotSel[i] = 0; m_slotName[i].clear(); m_slotDisp[i].clear();
                m_slotDesc[i].clear(); m_slotLook[i] = 0;
                refreshSlotCells(); fillGrid(); scheduleRebuild();
            });
            aClear->setEnabled(sno > 0);
            if (sno > 0) {
                const QString appr = m_slotName[i];
                const QString disp = m_slotDisp[i].isEmpty()
                                       ? AppearanceMeta::instance().titleFor(sno) : m_slotDisp[i];
                const QString coll = AppearanceMeta::instance().collectionFor(sno);
                auto clip = [](const QString& s) { QGuiApplication::clipboard()->setText(s); };
                auto prev = [](const QString& s) { return s.size() > 30 ? s.left(29) + QChar(0x2026) : s; };
                // Image pair first, then export, then copy — the order Wardrobe's slot cells use.
                // These were missing here purely because the icon helper was a file-static inside
                // WardrobeTab2.cpp; a Stable slot cell and a Wardrobe slot cell are the same idea.
                LookIcon::addActions(menu, this, slotIcon(sno), appr);
                menu.addSeparator();
                const QString exDir = ViewportPartMenu::condensePath(
                    QSettings().value(QStringLiteral("stable2/exportDir")).toString());
                if (!exDir.isEmpty())
                    menu.addAction(ViewportPartMenu::withValue(MenuText::kExportModelLast, exDir),
                                   this, [this, sno, appr] { exportAppearanceModel(sno, appr, true); });
                menu.addAction(ViewportPartMenu::prompts(MenuText::kExportModel), this,
                               [this, sno, appr] { exportAppearanceModel(sno, appr, false); });
                menu.addSeparator();
                menu.addAction(QStringLiteral("%1  (%2)").arg(MenuText::kCopySno).arg(sno), this,
                               [sno, clip] { clip(QString::number(sno)); });
                menu.addAction(QStringLiteral("%1  (%2)").arg(MenuText::kCopyFileName).arg(prev(appr)), this,
                               [appr, clip] { clip(appr); });
                menu.addAction(QStringLiteral("%1  (%2)").arg(MenuText::kCopyName).arg(prev(disp)), this,
                               [disp, clip] { clip(disp); });
                QAction* aColl = menu.addAction(
                    QStringLiteral("%1  (%2)").arg(MenuText::kCopyCollection).arg(prev(coll.isEmpty() ? QStringLiteral("—") : coll)),
                    this, [coll, clip] { clip(coll); });
                aColl->setEnabled(!coll.isEmpty());
            }
            menu.exec(b->mapToGlobal(p));
        });
        m_slotCellGroup->addButton(b, i);
        cellRow->addWidget(b);
    }
    cellRow->addStretch(1);
    m_slotCell[SlotMount]->setChecked(true);
    ll->addLayout(cellRow);
    connect(m_slotCellGroup, &QButtonGroup::idClicked, this, [this](int id) { selectSlot(id); });

    // Selected mount's localized name + description (like the in-game stable screen).
    m_infoLbl = new QLabel;
    m_infoLbl->setWordWrap(true);
    m_infoLbl->setTextFormat(Qt::RichText);
    m_infoLbl->setStyleSheet(QStringLiteral("padding:2px 2px 4px 2px;"));
    m_infoLbl->setVisible(false);
    ll->addWidget(m_infoLbl);

    // Search + collection filter for the active slot's browser.
    auto* fRow = new QHBoxLayout();
    m_search = new QLineEdit;
    m_search->setPlaceholderText(QStringLiteral("Search…"));
    m_search->setClearButtonEnabled(true);
    m_collFilter = new QComboBox;
    m_collFilter->addItem(QStringLiteral("All collections"), QString());
    fRow->addWidget(m_search, 1);
    fRow->addWidget(m_collFilter, 1);
    ll->addLayout(fRow);
    connect(m_search, &QLineEdit::textChanged, this, [this] { fillGrid(); });
    connect(m_collFilter, &QComboBox::currentIndexChanged, this, [this] { fillGrid(); });

    // Card grid browser.
    m_gridScroll = new QScrollArea;
    m_gridScroll->setWidgetResizable(true);
    m_gridScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_gridContent = new QWidget;
    m_gridLayout = new QGridLayout(m_gridContent);
    m_gridLayout->setContentsMargins(2, 2, 2, 2);
    m_gridLayout->setSpacing(4);
    m_gridLayout->setAlignment(Qt::AlignTop);
    m_gridScroll->setWidget(m_gridContent);
    ll->addWidget(m_gridScroll, 1);
    // Cards are responsive: reflow (debounced) when the panel width changes the column count,
    // so the grid fills the column like the Wardrobe/Models pickers instead of forcing it wide.
    m_gridReflow = new QTimer(this);
    m_gridReflow->setSingleShot(true);
    m_gridReflow->setInterval(60);
    connect(m_gridReflow, &QTimer::timeout, this, [this] { fillGrid(); });
    m_gridScroll->viewport()->installEventFilter(this);
    // Lazy card-thumbnail renderer (mounts have no inventory icons).
    m_thumbTimer = new QTimer(this);
    m_thumbTimer->setInterval(30);
    connect(m_thumbTimer, &QTimer::timeout, this, [this] { processThumbs(); });

    // (Saved "Stables" loadouts removed — not needed for a browser.)

    // Animation player. The clip LIST is registered as a right-sidebar PanelBox in buildSidebar()
    // (so the strip can toggle it, as in the Wardrobe and Models tabs); the TRANSPORT is added
    // under the viewport further down. Neither belongs in this left browser column, which is a
    // picker — the list sat here with no toggle at all, and stable2/showAnims was read once and
    // written by nothing, so there was no way to put it away.
    buildAnimPanel();
    if (m_resetBtn) ll->addWidget(m_resetBtn);   // whole-mount reset — never inside a hideable panel

    // Parts tree — created here, but LIVES in the right sidebar (wardrobe-parity PanelBox). Two
    // columns (Part · Tris) like the Wardrobe/Models PARTS panel.
    m_partTree = new QTreeWidget;
    m_partTree->setColumnCount(2);
    m_partTree->setHeaderLabels({ QStringLiteral("Part"), QStringLiteral("Tris") });
    m_partTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_partTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_partTree->header()->setStretchLastSection(false);
    m_partTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_partTree->setMouseTracking(true);
    m_partTree->viewport()->setMouseTracking(true);
    m_partTree->setToolTip(QStringLiteral("Uncheck to hide a submesh · hover/select to highlight · Esc clears"));
    connect(m_partTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem*, int) { recomputePartVisibility(); });
    connect(m_partTree, &QTreeWidget::itemSelectionChanged, this, &StableTab2::syncPartSelection);
    connect(m_partTree, &QTreeWidget::itemEntered, this, [this](QTreeWidgetItem* it, int) {
        if (!m_view) return;
        QList<int> hot = selectedParts(); hot += primitivesOf(it);
        m_view->setHighlightParts(hot);
    });
    m_partTree->viewport()->installEventFilter(this);
    m_partTree->installEventFilter(this);
    // Parts panel gets the SAME menu as the viewport (copy/export/isolate), not just "Copy name".
    m_partTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_partTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* it = m_partTree->itemAt(pos);
        const int idx = it ? it->data(0, Qt::UserRole).toInt() : -1;
        // Group headers store -1. Hand over a representative child so "Export Model" means THAT
        // appearance rather than the whole assembly — the tree has one group per equipped piece.
        const int group = (idx < 0 && it && it->childCount() > 0)
                            ? it->child(0)->data(0, Qt::UserRole).toInt() : -1;
        showPartContextMenu(idx, m_partTree->viewport()->mapToGlobal(pos), group);
    });

    m_status = new QLabel(QStringLiteral("Pick a mount."));   // lives in the sidebar's INFO panel
    m_status->setStyleSheet(QStringLiteral("color:#888;"));
    m_status->setWordWrap(true);

    // ── Center: toolbar + 3D view ────────────────────────────────────────────
    auto* center = new QWidget;
    auto* cl = new QVBoxLayout(center);
    cl->setContentsMargins(4, 4, 4, 4);
    cl->setSpacing(4);

    m_toolbarW = new QWidget(center);   // wrapped so fullscreen can hide the whole row
    auto* tb = new QHBoxLayout(m_toolbarW);
    tb->setContentsMargins(0, 2, 0, 2);
    tb->setSpacing(3);
    // Shared toolbar language (matches Models/Wardrobe): checkable QToolButtons, kToolBtnQss,
    // kBarH height, VLine dividers, an inline-styled dropdown.
    auto mkToggle = [&](const QString& text, const QString& tip, bool checked,
                        std::function<void(bool)> slot) {
        auto* b = new QToolButton(center);
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setChecked(checked);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QLatin1String(kToolBtnQss));
        b->setFixedHeight(kBarH);
        connect(b, &QToolButton::toggled, this, slot);
        tb->addWidget(b);
        return b;
    };
    auto sep = [&] {
        auto* f = new QFrame(center); f->setFrameShape(QFrame::VLine);
        f->setStyleSheet(QStringLiteral("color:#444;")); tb->addWidget(f);
    };
    m_wire = new QCheckBox(center);  m_wire->hide();   // hidden state carriers for existing wiring
    m_grid = new QCheckBox(center);  m_grid->hide();
    m_fxChk = new QCheckBox(center); m_fxChk->hide();
    m_simChk = new QCheckBox(center); m_simChk->hide();
    m_fxChk->setChecked(QSettings().value(QStringLiteral("stable2/showFx"), true).toBool());
    m_simChk->setChecked(QSettings().value(QStringLiteral("stable2/showSim"), true).toBool());
    // ── Shading mode: Blender's four spheres (Wire · Flat · Shaded · Rendered) — the shared
    // ViewGlyphs balls the Models/Wardrobe toolbars use. "Rendered" turns the post pipeline on. ──
    {
        auto* shadeGroup = new QButtonGroup(this);
        shadeGroup->setExclusive(true);
        static const char* const kShadeTip[4] = {
            "Wireframe", "Flat: base colour only", "Shaded: PBR, post off", "Rendered: PBR + IBL/shadows/SSAO/tonemap" };
        for (int m = 0; m < 4; ++m) {
            auto* b = new QToolButton(center);
            b->setIcon(QIcon(shadeBallGlyph(m)));
            b->setIconSize(QSize(20, 20));
            b->setToolTip(QString::fromLatin1(kShadeTip[m]));
            b->setCheckable(true);
            b->setCursor(Qt::PointingHandCursor);
            b->setFixedSize(28, kBarH);
            b->setStyleSheet(QLatin1String(kIconBtnQss));
            shadeGroup->addButton(b, m);
            tb->addWidget(b);
        }
        const int saved = qBound(0, QSettings().value(QStringLiteral("stable2/view/shadeMode"), 3).toInt(), 3);
        if (QAbstractButton* b = shadeGroup->button(saved)) b->setChecked(true);
        auto applyShade = [this](int id) {
            QSettings s;
            s.setValue(QStringLiteral("stable2/view/shadeMode"), id);
            if (!m_view) return;
            m_view->setWireframe(id == 0);
            m_view->setPbr(id >= 2);
            if (id >= 2) {   // Shaded/Rendered own the post pipeline (mirror the Graphics keys)
                const bool post = (id == 3);
                for (const char* k : { "ibl", "shadow", "ssao", "tonemap" })
                    s.setValue(QStringLiteral("stable2/gfx/") + QLatin1String(k), post);
                m_view->setFeatureIbl(post); m_view->setShadowEnabled(post);
                m_view->setSsaoEnabled(post); m_view->setFeatureTonemap(post);
            }
        };
        connect(shadeGroup, &QButtonGroup::idClicked, this, applyShade);
        QTimer::singleShot(0, this, [applyShade, saved] { applyShade(saved); });   // apply on load

        // ── Shading "⌄": a popover holding the Channel combo, placed immediately AFTER the four
        // shading balls (wardrobe parity — m_shadeMoreBtn + m_channelCombo). Wheel over the arrow
        // cycles the channel live; the ◆ glyph flags a non-default view. ──
        m_channelCombo = new QComboBox(center);
        {   // Value = GLModelWidget's own channel numbering, carried as data so the list can be
            // reordered or extended without re-pointing a single stored value.
            const char* const kChan[9] = { "Shaded", "Base Color", "Normal", "Roughness",
                                           "Metallic", "AO", "Emissive", "Detail maps",
                                           "Dye zones" };
            for (int c = 0; c < 9; ++c) m_channelCombo->addItem(QString::fromLatin1(kChan[c]), c);
        }
        m_channelCombo->setToolTip(QStringLiteral("View the lit result or one raw material channel (↑/↓ to scroll)"));
        m_channelCombo->setCursor(Qt::PointingHandCursor);
        m_channelCombo->setStyleSheet(QStringLiteral(
            "QComboBox{padding:2px 8px;border:1px solid #555;border-radius:3px;background:#2b2b2b;color:#bbb;}"
            "QComboBox:hover{border-color:#b0453c;}"
            "QComboBox QAbstractItemView{background:#2b2b2b;color:#dddddd;"
            "selection-background-color:#8a1414;selection-color:#ffffff;}"));
        selectByValue(m_channelCombo,
                      channelOrDefault(QSettings().value(QStringLiteral("stable2/view/channel"), 0).toInt()), 0);
        connect(m_channelCombo, &QComboBox::currentIndexChanged, this, [this](int) {
            const int c = channelOrDefault(m_channelCombo->currentData().toInt());
            QSettings().setValue(QStringLiteral("stable2/view/channel"), c);
            if (m_view) m_view->setViewChannel(c);
        });
        auto* shadeMore = new QToolButton(center);
        m_shadeMoreBtn = shadeMore;
        shadeMore->setText(QStringLiteral("⌄"));
        shadeMore->setPopupMode(QToolButton::InstantPopup);
        shadeMore->setFixedSize(18, kBarH);
        shadeMore->setCursor(Qt::PointingHandCursor);
        shadeMore->setStyleSheet(QLatin1String(kArrowBtnQss));
        shadeMore->installEventFilter(this);   // wheel → cycle channel
        {
            auto* sm2 = new QMenu(shadeMore);
            auto* row = new QWidget(sm2);
            auto* rl2 = new QHBoxLayout(row);
            rl2->setContentsMargins(10, 4, 10, 4);
            rl2->setSpacing(6);
            rl2->addWidget(new QLabel(QStringLiteral("Channel"), row));
            rl2->addWidget(m_channelCombo, 1);   // reparents; its connect lives on
            auto* wa = new QWidgetAction(sm2);
            wa->setDefaultWidget(row);
            sm2->addAction(wa);
            shadeMore->setMenu(sm2);
        }
        tb->addWidget(shadeMore);
        auto syncChannelBtn = [this]() {
            if (!m_shadeMoreBtn || !m_channelCombo) return;
            // currentData(), not currentIndex(): the ◆ flags "not the default channel", which is
            // channel VALUE 0 — position 0 only happens to be the same today.
            const int c = channelOrDefault(m_channelCombo->currentData().toInt());
            m_shadeMoreBtn->setText(c == 0 ? QStringLiteral("⌄") : QStringLiteral("◆"));
            m_shadeMoreBtn->setToolTip(QStringLiteral(
                "Channel: %1\nScroll here to flip channels · click for the list").arg(m_channelCombo->currentText()));
        };
        syncChannelBtn();
        connect(m_channelCombo, &QComboBox::currentIndexChanged, this,
                [syncChannelBtn](int) { syncChannelBtn(); });
        // The ONLY path by which the saved channel reaches the renderer at startup — both
        // currentIndexChanged lambdas are connected after the restore above, so neither fires
        // during construction. Reads the DATA, so a reordered list still resolves.
        QTimer::singleShot(0, this, [this] {   // apply the saved channel on load
            if (m_view) m_view->setViewChannel(channelOrDefault(m_channelCombo->currentData().toInt()));
        });
    }
    sep();
    // ── Overlays: Blender's split control (wardrobe parity). A SPHERE toggle (master on/off for
    // every guide) + an ARROW opening a persistent QFrame popup (grid / axes / skeleton / physics
    // bones / per-bone axes / bone names). Mostly stable2/ovl/*; the axis gizmo and coloured grid
    // axes are the APP-WIDE viewer/* keys every tab shares. ──
    {
        auto* ovBtn = new QToolButton(center);
        m_overlayBtn = ovBtn;
        ovBtn->setIcon(QIcon(overlayGlyph()));
        ovBtn->setIconSize(QSize(20, 20));
        ovBtn->setToolTip(QStringLiteral("Show overlays (grid, axes, skeleton…) — master toggle"));
        ovBtn->setCursor(Qt::PointingHandCursor);
        ovBtn->setCheckable(true);
        ovBtn->setChecked(QSettings().value(QStringLiteral("stable2/view/overlays"), true).toBool());
        ovBtn->setFixedSize(28, kBarH);
        ovBtn->setStyleSheet(QLatin1String(kIconBtnQss));
        tb->addWidget(ovBtn);

        m_overlayPanel = new QFrame(this, Qt::Popup);
        m_overlayPanel->setObjectName(QStringLiteral("stableOvPanel"));
        m_overlayPanel->setStyleSheet(QStringLiteral(
            "QFrame#stableOvPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
            "QLabel{color:#cccccc;} QCheckBox{color:#cccccc;}"));
        auto* opl = new QVBoxLayout(m_overlayPanel);
        opl->setContentsMargins(12, 10, 12, 10);
        opl->setSpacing(4);
        auto* ovHdr = new QLabel(QStringLiteral("Viewport Overlays"), m_overlayPanel);
        ovHdr->setStyleSheet(QLatin1String(kHdrQss));
        opl->addWidget(ovHdr);
        auto ovSection = [&](const QString& t) {
            auto* l = new QLabel(t, m_overlayPanel);
            l->setStyleSheet(QStringLiteral("%1margin-top:6px;").arg(QLatin1String(kSubHdrQss)));
            opl->addWidget(l);
        };
        // Persist the key, push to GL, gate on the master (off → GL stays dark; re-applied on master on).
        // `key` is the FULL settings key, not a suffix — as in the Wardrobe. Two of these overlays
        // are APP-WIDE (viewer/axisGizmo, viewer/gridAxisColors): GLModelWidget seeds itself from
        // those, and the Models and Wardrobe tabs write them, so a private stable2/ovl/ copy meant
        // the axis gizmo silently disagreed between tabs and the shared toggle never reached here.
        auto addOverlay = [&](const QString& label, const QString& key, bool def, bool indent,
                              const QString& tip, std::function<void(bool)> apply) {
            auto* cb = new QCheckBox(label, m_overlayPanel);
            if (indent) cb->setStyleSheet(QStringLiteral("QCheckBox{color:#cccccc;margin-left:16px;}"));
            if (!tip.isEmpty()) cb->setToolTip(tip);
            cb->setChecked(QSettings().value(key, def).toBool());
            connect(cb, &QCheckBox::toggled, this, [this, key, apply](bool on) {
                QSettings().setValue(key, on);
                if (m_overlaysOn) apply(on);
            });
            opl->addWidget(cb);
            m_overlayChks.append({ cb, apply });
            return cb;
        };
        ovSection(QStringLiteral("Guides"));
        addOverlay(QStringLiteral("Ground grid"), QStringLiteral("stable2/ovl/grid"), false, false,
                   QStringLiteral("Ground plane grid."),
                   [this](bool on) { if (m_view) m_view->setShowGrid(on); });
        addOverlay(QStringLiteral("Axis gizmo"), QStringLiteral("viewer/axisGizmo"), true, false,
                   QStringLiteral("Clickable X/Y/Z orientation ball in the viewport corner."),
                   [this](bool on) { if (m_view) m_view->setShowAxisGizmo(on); });
        addOverlay(QStringLiteral("Colored grid axes"), QStringLiteral("viewer/gridAxisColors"), true, true,
                   QStringLiteral("Tint the grid's world axes: X red, Z blue."),
                   [this](bool on) { if (m_view) m_view->setGridAxisColors(on); });
        ovSection(QStringLiteral("Skeleton"));
        addOverlay(QStringLiteral("Skeleton"), QStringLiteral("stable2/ovl/skel"), false, false,
                   QStringLiteral("Draw the bone hierarchy."),
                   [this](bool on) { if (m_view) m_view->setShowSkeleton(on); });
        {   // Collision model. Deliberately NOT via addOverlay: the Physics panel already owns a
            // "Show collision models" box on stable2/cloth/showColliders, and addOverlay would mint
            // a second key under stable2/ovl/. One state, one key — the two boxes are linked below.
            auto* cb = new QCheckBox(QStringLiteral("Collision model"), m_overlayPanel);
            cb->setToolTip(QStringLiteral("Draw the cloth collision model — the authored capsules and "
                                          "plane colliders the cloth is solved against. Use it to see "
                                          "whether a garment is clipping because the capsules don't "
                                          "match the body."));
            cb->setChecked(QSettings().value(QStringLiteral("stable2/cloth/showColliders"), false).toBool());
            connect(cb, &QCheckBox::toggled, this, [this](bool on) {
                QSettings().setValue(QStringLiteral("stable2/cloth/showColliders"), on);
                if (m_overlaysOn && m_view) m_view->setShowColliders(on);
            });
            opl->addWidget(cb);
            m_overlayChks.append({ cb, [this](bool on) { if (m_view) m_view->setShowColliders(on); } });
            m_ovlChkColliders = cb;
            linkColliderToggles();
        }
        addOverlay(QStringLiteral("Physics bones"), QStringLiteral("stable2/ovl/phys"), false, false,
                   QStringLiteral("Overlay the cloth/physics bones (anchored grey, simulated orange)."),
                   [this](bool on) { if (m_view) m_view->setShowPhysBones(on); });
        addOverlay(QStringLiteral("Axis gizmos (per-bone)"), QStringLiteral("stable2/ovl/physaxes"), true, true,
                   QStringLiteral("Per-bone XYZ rotation gizmo (R/G/B)."),
                   [this](bool on) { if (m_view) m_view->setShowPhysAxes(on); });
        addOverlay(QStringLiteral("Hardpoints"), QStringLiteral("stable2/ovl/hardpoints"), false, false,
                   QStringLiteral("Draw the mount's attach sockets (saddle, HP_trophy1/2/3, reins…) as "
                                  "labeled XYZ gizmos — where the trophy and rider snap on."),
                   [this](bool on) { if (m_view) m_view->setShowHardpoints(on); });
        addOverlay(QStringLiteral("Bone names"), QStringLiteral("stable2/ovl/bnm"), false, false,
                   QStringLiteral("Label each bone at its position in the viewport."),
                   [this](bool on) { if (m_view) m_view->setShowBoneNames(on); });
        addOverlay(QStringLiteral("Translated names"), QStringLiteral("stable2/ovl/bnmtrans"), false, true,
                   QStringLiteral("Readable labels from verified D4 hardpoint/IK data; others keep bone_<hash>."),
                   [this](bool on) { if (m_view) m_view->setBoneNamesTranslated(on); });
        addOverlay(QStringLiteral("Hide unnamed bones"), QStringLiteral("stable2/ovl/bnmhide"), false, true,
                   QStringLiteral("Only label bones with a known/translated name."),
                   [this](bool on) { if (m_view) m_view->setBoneNamesHideUnknown(on); });

        // Master toggle: all guides off at once, remembering each box's own state.
        m_overlaysOn = ovBtn->isChecked();
        connect(ovBtn, &QToolButton::toggled, this, [this](bool on) {
            m_overlaysOn = on;
            QSettings().setValue(QStringLiteral("stable2/view/overlays"), on);
            reapplyOverlays();   // off = force-off; on = restore each box (and the cloth flags)
            if (m_overlayPanel) m_overlayPanel->setEnabled(on);
        });
        m_overlayPanel->setEnabled(m_overlaysOn);

        // ⌄ — opens/closes the overlay settings panel.
        auto* ovArrow = new QToolButton(center);
        ovArrow->setText(QStringLiteral("⌄"));
        ovArrow->setToolTip(QStringLiteral("Overlay settings"));
        ovArrow->setCursor(Qt::PointingHandCursor);
        ovArrow->setFixedSize(18, kBarH);
        ovArrow->setStyleSheet(QLatin1String(kArrowBtnQss));
        connect(ovArrow, &QToolButton::clicked, this, [this, ovArrow]() {
            if (!m_overlayPanel) return;
            if (m_overlayPanel->isVisible()) { m_overlayPanel->hide(); return; }
            m_overlayPanel->adjustSize();
            m_overlayPanel->move(ovArrow->mapToGlobal(QPoint(0, ovArrow->height() + 2)));
            m_overlayPanel->show();
            m_overlayPanel->raise();
        });
        tb->addWidget(ovArrow);
    }
    sep();
    // FX/SIM drive the hidden state carriers (downstream visibility wiring unchanged).
    mkToggle(QStringLiteral("FX"), QStringLiteral("Show FX submeshes"), m_fxChk->isChecked(),
             [this](bool on) { m_fxChk->setChecked(on); });
    mkToggle(QStringLiteral("SIM"), QStringLiteral("Show cloth-sim submeshes"), m_simChk->isChecked(),
             [this](bool on) { m_simChk->setChecked(on); });
    tb->addStretch(1);
    // (Export .glb button removed — export runs from the top Export menu via previewWidget/exportSelection.)
    cl->addWidget(m_toolbarW);

    m_view = new GLModelWidget;
    m_view->setMinimumSize(360, 360);
    m_view->setFocusPolicy(Qt::StrongFocus);   // for the H-family hide hotkeys / Esc
    cl->addWidget(m_view, 1);
    // partFocused (double-click) is deliberately NOT connected here any more. It used to be the
    // only way to select a part from the viewport; a single left-click owns that now, and the
    // first click of a double-click has already done it. Re-selecting on the double-click achieved
    // nothing on a plain one and destroyed the selection on a Ctrl one. The camera move itself
    // lives in GLModelWidget, gated on viewer/framePartOnPick, so double-click still frames.
    // Single left-click → select in the PARTS tree; Ctrl or Shift adds or toggles; empty space
    // clears. See the Wardrobe's copy of this for why Shift is additive rather than a range.
    connect(m_view, &GLModelWidget::partClicked, this, [this](int part, Qt::KeyboardModifiers mods) {
        if (!m_partTree) return;
        const bool add = mods & (Qt::ControlModifier | Qt::ShiftModifier);
        QTreeWidgetItem* hit = itemForPart(part);
        // One sync, not two — the selection handler here rebuilds the TEXTURE PREVIEW tiles, which
        // is six smooth QImage rescales, so a plain click firing it twice is worth avoiding.
        {
            const bool was = m_partTree->blockSignals(true);
            if (!add) m_partTree->clearSelection();
            if (hit) {
                if (add && hit->isSelected()) {
                    hit->setSelected(false);
                } else {
                    if (hit->parent()) hit->parent()->setExpanded(true);
                    hit->setSelected(true);
                    m_partTree->scrollToItem(hit);
                }
            }
            m_partTree->blockSignals(was);
        }
        syncPartSelection();   // the one sync the blocked edit above deliberately suppressed
    });
    // Right-click a part in the viewport → hide/show it + copy its material name.
    connect(m_view, &GLModelWidget::partRightClicked, this,
            [this](int part, const QPoint& gp) { showPartContextMenu(part, gp); });
    // Transport (Play · scrub · speed · loop) under the viewport, where the Models and Wardrobe
    // tabs put theirs. It hides itself until a clip is actually playing.
    if (m_timeline) cl->addWidget(m_timeline);
    buildVpStrip();   // Reset · Camera · Lighting · Fullscreen pinned to the viewport edge

    left->setMinimumWidth(230);
    left->setMaximumWidth(500);   // a picker column, not a canvas — bounds the responsive card grid
    split->addWidget(left);
    split->addWidget(center);
    buildSidebar(split);   // wardrobe-parity right sidebar: PARTS · MATERIALS · TEXTURES · INFO · ANIMATIONS
    m_mainSplit = split;
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 0);
    split->setChildrenCollapsible(false);
    split->setSizes({ 460, 600, 230 });   // left wide enough for a default row of 4 cards

    // Remember the column widths across sessions (parity with Wardrobe): restore the saved
    // split, then persist any user drag of the dividers.
    if (PanelPersist::enabled()) {
        const QVariantList sv = QSettings().value(QStringLiteral("stable2/splitSizes")).toList();
        if (sv.size() == 3) {
            QList<int> sizes;
            for (const QVariant& v : sv) sizes << qMax(0, v.toInt());
            if (sizes[0] > 0 && sizes[1] > 0) split->setSizes(sizes);
        }
    }
    connect(split, &QSplitter::splitterMoved, this, [this, split](int, int) {
        if (m_restoring || !PanelPersist::enabled()) return;
        QVariantList sv;
        for (int s : split->sizes()) sv << s;
        QSettings().setValue(QStringLiteral("stable2/splitSizes"), sv);
    });

    // Esc leaves fullscreen (armed only while it's active).
    m_fsEsc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    m_fsEsc->setContext(Qt::WidgetWithChildrenShortcut);
    m_fsEsc->setEnabled(false);
    connect(m_fsEsc, &QShortcut::activated, this, [this] {
        if (m_fsBtn) m_fsBtn->setChecked(false);   // → toggleFullscreen(false)
    });
    // F toggles fullscreen (parity with the Wardrobe/Models F key).
    auto* fKey = new QShortcut(QKeySequence(Qt::Key_F), this);
    fKey->setContext(Qt::WidgetWithChildrenShortcut);
    connect(fKey, &QShortcut::activated, this, [this] { if (m_fsBtn) m_fsBtn->toggle(); });
    // Ctrl+Z undoes a mount/barding/trophy/look change.
    auto* undoSc = new QShortcut(QKeySequence::Undo, this);
    undoSc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(undoSc, &QShortcut::activated, this, [this] { undo(); });

    // ── Wiring ───────────────────────────────────────────────────────────────
    connect(m_wire, &QCheckBox::toggled, this, [this](bool on) { if (m_view) m_view->setWireframe(on); });
    connect(m_grid, &QCheckBox::toggled, this, [this](bool on) { if (m_view) m_view->setShowGrid(on); });
    connect(m_fxChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("stable2/showFx"), on); recomputePartVisibility();
    });
    connect(m_simChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("stable2/showSim"), on); recomputePartVisibility();
    });
}

// ── Viewport N-strip (wardrobe-parity): Reset · Camera · Lighting · Fullscreen, pinned to the
// viewport's right edge. Popups open LEFTward so they never spill off-screen. ─────────────────
void StableTab2::buildVpStrip()
{
    if (!m_view || m_vpStrip) return;
    m_vpStrip = new QWidget(m_view);
    m_vpStrip->setAttribute(Qt::WA_StyledBackground);
    m_vpStrip->setStyleSheet(QStringLiteral(
        "QWidget{background:rgba(30,30,32,190);border:1px solid #3c3c3f;border-radius:5px;}"));
    auto* v = new QVBoxLayout(m_vpStrip);
    v->setContentsMargins(3, 4, 3, 4);
    v->setSpacing(3);
    auto mk = [&](const QPixmap& icon, const QString& text, const QString& tip, bool checkable) {
        auto* b = new QToolButton(m_vpStrip);
        if (!icon.isNull()) { b->setIcon(QIcon(icon)); b->setIconSize(QSize(16, 16)); }
        else                  b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(checkable);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(30, 24);
        b->setStyleSheet(QStringLiteral(
            "QToolButton{border:1px solid transparent;border-radius:3px;background:transparent;color:#cfcfcf;}"
            "QToolButton:hover{border-color:#b0453c;}"
            "QToolButton:checked{background:#8a1414;border-color:#a01818;}"));
        v->addWidget(b);
        return b;
    };
    m_sideArrow   = mk(QPixmap(), QStringLiteral("»"), QStringLiteral("Hide the right panels"), false);
    auto* bReset  = mk(QPixmap(), QStringLiteral("⟲"), QStringLiteral("Reset view"), false);
    auto* bGfx    = mk(stripGlyph(0),   QString(), QStringLiteral("Graphics — IBL · shadows · SSAO · tonemap · backdrop"), false);
    auto* bCam    = mk(stripGlyph(2),   QString(), QStringLiteral("Camera — FOV · angles · turntable"), false);
    auto* bLight  = mk(stripGlyph(3),   QString(), QStringLiteral("Lighting — three-point rig"), false);
    auto* bShade  = mk(stripGlyph(4),   QString(), QStringLiteral("Shaders — fur/mane shell + mesh FX"), false);
    auto* bDetail = mk(stripGlyph(5),   QString(), QStringLiteral("Detail maps — detail-map selection (global)"), false);
    auto* bPhys   = mk(stripGlyph(7),   QString(), QStringLiteral("Physics — live cloth/mane sim tuning"), false);
    m_fsBtn       = mk(QPixmap(), QStringLiteral("⛶"), QStringLiteral("Fullscreen — viewport fills the tab (Esc/F restores)"), true);
    connect(m_sideArrow, &QToolButton::clicked, this, [this] { setSideCollapsed(!m_sideCollapsed); });
    connect(bReset, &QToolButton::clicked, this, [this] { if (m_view) m_view->resetView(); });
    connect(bGfx, &QToolButton::clicked, this, [this, bGfx] {
        if (!m_gfxPanel) buildGraphicsPanel();
        showPopup(m_gfxPanel, bGfx);
    });
    connect(bCam, &QToolButton::clicked, this, [this, bCam] {
        if (!m_camPanel) buildCameraPanel();
        if (m_camOrbitSync) m_camOrbitSync();   // camera may have been orbited since the last open
        showPopup(m_camPanel, bCam);
    });
    connect(bLight, &QToolButton::clicked, this, [this, bLight] {
        if (!m_lightPanel) buildLightingPanel();
        showPopup(m_lightPanel, bLight);
    });
    connect(bShade, &QToolButton::clicked, this, [this, bShade] {
        if (!m_shaderPanel) buildShaderPanel();
        showPopup(m_shaderPanel, bShade);
    });
    connect(bDetail, &QToolButton::clicked, this, [this, bDetail] {
        if (!m_detailPanel) buildDetailPanel();
        showPopup(m_detailPanel, bDetail);
    });
    connect(bPhys, &QToolButton::clicked, this, [this, bPhys] {
        if (!m_physPanel) buildPhysicsPanel();
        showPopup(m_physPanel, bPhys);
    });
    connect(m_fsBtn, &QToolButton::toggled, this, [this](bool on) { toggleFullscreen(on); });
    m_view->installEventFilter(this);   // reposition the strip on viewport resize
    positionVpStrip();
    m_vpStrip->show();
    m_vpStrip->raise();
}

void StableTab2::positionVpStrip()
{
    if (!m_vpStrip || !m_view) return;
    m_vpStrip->adjustSize();
    // Right edge, below the axis gizmo (top-right ~88px).
    m_vpStrip->move(m_view->width() - m_vpStrip->width() - 8, 100);
    m_vpStrip->raise();
}

// ── Right sidebar: PanelBox stack (PARTS · MATERIALS · TEXTURES · INFO · ANIMATIONS), wardrobe-style — a vertical icon strip of
// checkable toggles beside a QSplitter of titled panels; shown/hidden state persists. ─────────
void StableTab2::buildSidebar(QSplitter* mainSplit)
{
    m_sidebarW = new QWidget;
    auto* sb = new QHBoxLayout(m_sidebarW);
    sb->setContentsMargins(2, 6, 4, 6);
    sb->setSpacing(3);
    m_rstripW = new QWidget(m_sidebarW);
    QWidget* stripW = m_rstripW;
    auto* stripLay = new QVBoxLayout(stripW);
    stripLay->setContentsMargins(0, 0, 0, 0);
    stripLay->setSpacing(3);
    m_rsplit = new QSplitter(Qt::Vertical, m_sidebarW);
    m_rsplit->setChildrenCollapsible(false);
    sb->addWidget(stripW);
    sb->addWidget(m_rsplit, 1);

    // `id` is the STABLE token the layout is stored under — never the title, which is a label and
    // may grow a live count later. They happen to be equal today; keeping them separate is what
    // stops a cosmetic rename from orphaning every saved layout.
    auto section = [&](const QString& id, const QString& title, QWidget* content,
                       const QPixmap& icon, const QString& tip, bool defOn) {
        const int page = m_rsections.size();
        auto* box = new PanelBox(title, content, m_rsplit);
        box->hide();
        m_rsplit->addWidget(box);
        m_rsections.append(box);
        m_rids.append(id);
        m_rdefOn.append(defOn);
        connect(box->up,   &QToolButton::clicked, this, [this, page] { moveSidePanel(page, -1); });
        connect(box->down, &QToolButton::clicked, this, [this, page] { moveSidePanel(page, +1); });
        auto* b = new QToolButton(stripW);
        b->setIcon(QIcon(icon));               // shared outliner glyphs, like the Models sidebar
        b->setIconSize(QSize(16, 16));
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(24, 24);
        b->setStyleSheet(QStringLiteral(
            "QToolButton{border:1px solid transparent;border-radius:3px;background:transparent;color:#cfcfcf;}"
            "QToolButton:hover{border-color:#b0453c;}"
            "QToolButton:checked{background:#8a1414;border-color:#a01818;}"));
        stripLay->addWidget(b);
        m_rpageBtns.append(b);
        connect(b, &QToolButton::toggled, this, [this, page](bool on) { showSidePanel(page, on); });
        connect(box->close, &QToolButton::clicked, this, [this, page] {
            if (page < m_rpageBtns.size()) m_rpageBtns[page]->setChecked(false);
        });
        // NO restore here: which panels are up is now an ORDERED list replayed once after every
        // registration (see the block at the end of this function). Restoring per-section would
        // fix the order to registration order, which is the thing the reorder buttons exist to
        // change.
    };

    // MATERIALS panel — # · material · tris table; selecting a row highlights that part.
    m_matTable = new QTreeWidget;
    m_matTable->setColumnCount(3);
    m_matTable->setHeaderLabels({ QStringLiteral("#"), QStringLiteral("Material"), QStringLiteral("Tris") });
    m_matTable->setRootIsDecorated(false);
    m_matTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_matTable->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_matTable->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_matTable->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_matTable->header()->setStretchLastSection(false);
    installCopyMenu(m_matTable, 1);   // right-click → Copy material name / Copy all
    connect(m_matTable, &QTreeWidget::itemSelectionChanged, this, [this] {
        QList<int> parts;
        for (QTreeWidgetItem* it : m_matTable->selectedItems()) parts << it->data(0, Qt::UserRole).toInt();
        if (m_view) m_view->setHighlightParts(parts);
        if (!parts.isEmpty()) updateTexTiles(parts.first());
    });

    // TEXTURE PREVIEW panel — five PBR-channel tiles for the selected part.
    auto* texW = new QWidget;
    auto* tgl = new QGridLayout(texW);
    tgl->setContentsMargins(4, 4, 4, 4);
    tgl->setSpacing(3);
    static const char* const kTexCaps[6] = { "COLOR", "ROUGH", "METAL", "NORMAL", "ALPHA", "EMIS" };
    for (int c = 0; c < 6; ++c) {
        auto* col = new QVBoxLayout();
        col->setSpacing(1);
        auto* tile = new QLabel;
        tile->setFixedSize(44, 44);
        tile->setAlignment(Qt::AlignCenter);
        tile->setStyleSheet(QStringLiteral("QLabel{background:#1b1b1c;border:1px solid #3a3a3d;color:#666;font-size:7px;}"));
        tile->setText(QString::fromLatin1(kTexCaps[c]));
        m_texTile[c] = tile;
        auto* cap = new QLabel(QString::fromLatin1(kTexCaps[c]));
        cap->setAlignment(Qt::AlignCenter);
        cap->setStyleSheet(QStringLiteral("color:#8a8a8a;font-size:7px;"));
        col->addWidget(tile);
        col->addWidget(cap);
        tgl->addLayout(col, c / 3, c % 3);   // 3 per row → fits the narrow sidebar
    }
    tgl->setColumnStretch(3, 1);

    // INFO panel content: the status line (parts/verts/tris + trophy seat debug).
    auto* infoW = new QWidget;
    auto* iv = new QVBoxLayout(infoW);
    iv->setContentsMargins(4, 2, 4, 2);
    iv->setSpacing(4);
    iv->addWidget(m_status);

    using K = ModelOutlinerModel;
    section(QStringLiteral("PARTS"), QStringLiteral("PARTS"), m_partTree, K::kindIcon(K::Part),
            QStringLiteral("Parts — submesh visibility (uncheck to hide, hover to highlight)"), true);
    section(QStringLiteral("MATERIALS"), QStringLiteral("MATERIALS"), m_matTable, K::kindIcon(K::Material),
            QStringLiteral("Materials — one row per submesh (select to highlight)"), true);
    section(QStringLiteral("TEXTURES"), QStringLiteral("TEXTURES"), texW, K::kindIcon(K::TexGroup),
            QStringLiteral("Texture preview — PBR channels of the selected part"), false);
    section(QStringLiteral("INFO"), QStringLiteral("INFO"), infoW, K::kindIcon(K::ValueGroup),
            QStringLiteral("Info — assembly stats for the current mount"), false);
    // ANIMATIONS — the clip list, in the right column like the Models and Wardrobe tabs (the
    // transport stays under the viewport). Defaults ON: it was always visible before the move, so
    // anything else would read as the panel having gone missing.
    if (m_animPanel)
        section(QStringLiteral("ANIMATIONS"), QStringLiteral("ANIMATIONS"), m_animPanel, K::kindIcon(K::Anim),
                QStringLiteral("Animations — the mount's clip list (search, select to play)"), true);
    stripLay->addStretch(1);

    // ── Replay the layout: which panels are up, in what ORDER, at what heights ──────────────────
    // Names + heights rather than QSplitter::saveState(), which is positional — hidden panels
    // still occupy splitter slots, so index N would mean a different panel between runs.
    {
        QSettings st;
        QStringList shown;
        if (st.contains(QStringLiteral("stable2/panels/shown"))) {
            shown = st.value(QStringLiteral("stable2/panels/shown")).toStringList();
        } else {
            // Migrate the old per-panel booleans once. They were a second store for the same
            // state and carried no order at all, so registration order is the only reading.
            for (int i = 0; i < m_rids.size(); ++i) {
                const QString legacy = QStringLiteral("stable2/panel/") + m_rids[i];
                if (st.value(legacy, m_rdefOn.value(i)).toBool()) shown << m_rids[i];
                st.remove(legacy);   // migrated; leaving it is a dead key
            }
            // Persist the migrated list HERE. saveSidePanelLayout() is suppressed for the whole
            // replay below, and after it only a strip toggle, a reorder or a handle drag writes —
            // so a user who upgrades and never touches the sidebar would have had their layout
            // read from legacy keys once, those keys deleted, and every later launch fall back to
            // defaults with nothing left to recover from.
            st.setValue(QStringLiteral("stable2/panels/shown"), shown);
        }
        const QStringList heights = st.value(QStringLiteral("stable2/panels/sizes")).toStringList();
        const int shownCount = [&] {   // how many of `shown` this build still has
            int n = 0;
            for (const QString& id : shown) if (m_rids.indexOf(id) >= 0) ++n;
            return n;
        }();

        m_panelRestore = true;   // don't let these toggles write a half-applied layout back out
        int slot = 0;
        for (const QString& id : shown) {
            const int page = m_rids.indexOf(id);
            if (page < 0) continue;              // a panel this build no longer has
            m_rsplit->insertWidget(slot++, m_rsections[page]);
            m_rpageBtns[page]->setChecked(true); // → showSidePanel(page, true)
        }
        m_panelRestore = false;
        // Compare against the number of SHOWN panels — the same quantity saveSidePanelLayout()
        // produced. m_rsplit->count() is every registered panel, because a hidden PanelBox keeps
        // its splitter slot, so the two match only when all five are up: with the shipped defaults
        // (three up) the guard never fired and stable2/panels/sizes was write-only.
        //
        // And patch the LIVE full-length size list rather than building one from `heights`: setSizes
        // expects one entry per splitter child, so a short list would size only the leading slots.
        if (!heights.isEmpty() && heights.size() == shownCount) {
            QList<int> sizes = m_rsplit->sizes();
            int k = 0;
            for (int i = 0; i < m_rsplit->count() && k < heights.size(); ++i)
                if (!m_rsplit->widget(i)->isHidden()) sizes[i] = heights[k++].toInt();
            m_rsplit->setSizes(sizes);
        }
        updateSidebarCollapse();
    }
    connect(m_rsplit, &QSplitter::splitterMoved, this,
            [this](int, int) { saveSidePanelLayout(); });

    mainSplit->addWidget(m_sidebarW);
}

// Move one panel up or down among the panels that are UP (hidden ones keep their splitter slots
// but must not be counted, or a ▲ would appear to do nothing while it swapped two invisibles).
void StableTab2::moveSidePanel(int page, int delta)
{
    if (!m_rsplit || page < 0 || page >= m_rsections.size()) return;
    PanelBox* box = m_rsections[page];
    QVector<int> vis;   // isHidden, not isVisible: the latter is false for every child while the
                        // tab itself is unshown, which would make this a no-op on a background tab
    for (int i = 0; i < m_rsplit->count(); ++i)
        if (!m_rsplit->widget(i)->isHidden()) vis << i;
    const int cur = vis.indexOf(m_rsplit->indexOf(box));
    const int tgt = cur + delta;
    if (cur < 0 || tgt < 0 || tgt >= vis.size()) return;   // already at an end
    const QList<int> sizes = m_rsplit->sizes();
    m_rsplit->insertWidget(vis[tgt], box);                 // moves the existing child
    m_rsplit->setSizes(sizes);                             // insertWidget resets sizes — restore
    saveSidePanelLayout();
}

// Which panels are up, in what order, at what heights.
void StableTab2::saveSidePanelLayout()
{
    if (!m_rsplit || m_panelRestore) return;   // never write while the replay above is running
    const QList<int> sizes = m_rsplit->sizes();
    QStringList shown, heights;
    for (int i = 0; i < m_rsplit->count(); ++i) {
        QWidget* w = m_rsplit->widget(i);
        if (w->isHidden()) continue;
        const int page = m_rsections.indexOf(static_cast<PanelBox*>(w));
        if (page < 0) continue;
        shown   << m_rids.value(page);
        heights << QString::number(sizes.value(i));
    }
    QSettings s;
    s.setValue(QStringLiteral("stable2/panels/shown"), shown);
    s.setValue(QStringLiteral("stable2/panels/sizes"), heights);
}

// With no panels up the column shrinks to just the icon strip — which must stay reachable, since
// it is the only way to bring a panel back. Hiding the column outright is a different thing
// (setSideCollapsed), so these widths only ever apply while it is visible.
void StableTab2::updateSidebarCollapse()
{
    if (!m_sidebarW || !m_rstripW) return;
    bool any = false;
    for (PanelBox* b : m_rsections)
        if (b && !b->isHidden()) { any = true; break; }
    if (any) {
        m_sidebarW->setMinimumWidth(230);
        m_sidebarW->setMaximumWidth(QWIDGETSIZE_MAX);
    } else {
        m_sidebarW->setMinimumWidth(0);
        m_sidebarW->setMaximumWidth(m_rstripW->sizeHint().width() + 10);
    }
}

void StableTab2::showSidePanel(int page, bool on)
{
    if (page < 0 || page >= m_rsections.size()) return;
    PanelBox* box = m_rsections[page];
    const bool was = !box->isHidden();
    box->setVisible(on);
    if (on && !was) panelBoxArrive(m_rsplit, box);
    saveSidePanelLayout();     // one store for "which panels, in what order, how tall"
    updateSidebarCollapse();   // last panel down → the column shrinks to the strip
}

void StableTab2::toggleFullscreen(bool on)
{
    m_fullscreen = on;
    if (m_mainSplit && m_mainSplit->count() > 0 && m_mainSplit->widget(0))
        m_mainSplit->widget(0)->setVisible(!on);   // left controls
    if (m_sidebarW) m_sidebarW->setVisible(!on && !m_sideCollapsed);   // honor an existing collapse
    if (m_toolbarW) m_toolbarW->setVisible(!on);   // toolbar row
    // The transport lived in the left column before it moved under the viewport, so fullscreen hid
    // it for free. It is chrome; keep hiding it rather than letting the move change what fullscreen
    // means. (Only re-show it if a clip is actually loaded — that is its own resting state.)
    if (m_timeline) m_timeline->setVisible(!on && m_curAnim.valid);
    if (m_fsEsc) m_fsEsc->setEnabled(on);
    positionVpStrip();   // the viewport just changed size
}

// Coalesce rapid interactive changes into ONE rebuild. rebuildMount() parses up to three
// appearances, merges them, BC-decodes every texture and uploads it — all on the GUI thread — so
// clicking down the card grid used to pay that once per card the pointer passed through. A 35 ms
// single-shot restart means only the selection the user settles on is ever built.
//
// No setting gates this: an unconditional 35 ms debounce is imperceptible, and a QSettings flag
// with no UI to write it would be exactly the dead key this tab has been carrying elsewhere.
//
// Deliberately NOT used by the startup restore or by undo(): both run code on the very next line
// that assumes the rebuild has already happened (restoreCameraState() re-frames the new model;
// undo clears m_restoring, and a deferred rebuild would then see it false and re-snapshot).
void StableTab2::scheduleRebuild()
{
    if (!m_rebuildTimer) {
        m_rebuildTimer = new QTimer(this);
        m_rebuildTimer->setSingleShot(true);
        m_rebuildTimer->setInterval(35);
        connect(m_rebuildTimer, &QTimer::timeout, this, [this] { rebuildMount(); });
    }
    m_rebuildTimer->start();   // restart on each change
}

// Reload / game-build change. Everything below is keyed to the BUILD and would otherwise be
// answered from the previous one. This used to clear only the material-decode caches, which was
// not enough on three counts:
//   m_petReady   ensurePetIndex() early-returns on it, and MainWindow DELETES stable_index_v6.bin
//                on a fingerprint change (that cache carries no signature) — so the roster was
//                never rebuilt and the deleted cache's contents stayed on screen all session
//   m_loaded     refresh() early-returns on it, so the tab never repopulated at all
//   m_petGen     bumped so a scan already IN FLIGHT discards itself instead of installing the old
//                install's roster over the new one (the pattern BackTrophyIndex.h documents)
//
// The rendered card portraits are disk-backed as well as in-memory, and queueThumb() reads the
// disk copy first — so dropping m_thumbs alone would force a round-trip and hand back the same
// stale image. The directory goes too. (Nothing else clears it: MainWindow's fingerprint cleanup
// covers stable_index_v*.bin and tex_info_v*.bin only.)
void StableTab2::reset()
{
    m_cBase.clear(); m_cNorm.clear(); m_cOrm.clear();
    m_cEmis.clear(); m_cMask.clear(); m_cTrans.clear();
    m_clipTok.clear();
    m_clipDiskLoaded = false;   // re-attempt the load; a stale signature rejects itself

    ++m_petGen;
    m_petReady = false; m_petBuilding = false;
    m_mounts.clear(); m_armorItems.clear(); m_trophyItems.clear(); m_petItems.clear();
    m_pets.clear(); m_iconByApp.clear(); m_gridEntries.clear();
    m_themesBuilt = false; m_themeArmor.clear(); m_themeTrophy.clear();
    m_atlasBuilt = false; m_atlasIdx.clear();

    m_thumbs.clear(); m_thumbQueue.clear(); m_thumbQueued.clear(); m_thumbAppr.clear();
    if (m_thumbTimer) m_thumbTimer->stop();
    // A rebuild queued against the OLD build must not fire after the reset.
    if (m_rebuildTimer) m_rebuildTimer->stop();
    QDir(AppPaths::dataDir() + QStringLiteral("/stable_icons")).removeRecursively();

    // The assembled mount belongs to the old build too. hasExportSelection() reads m_lastGeo, so
    // leaving it would keep the Export menu enabled and writing the PRE-reload mesh; and an undo
    // snapshot holds appearance SNOs that may not resolve any more.
    m_lastGeo = ModelGeometry();
    m_exportMats.clear();
    m_undo.clear();

    m_loaded = false;
}

// ── Refresh / discovery ───────────────────────────────────────────────────────
void StableTab2::refresh()
{
    if (m_loaded || !m_index || !m_index->isLoaded()) return;
    m_loaded = true;
    QElapsedTimer refT; refT.start();
    // Icons need both the appearance→handle map (AppearanceMeta) and the atlas decoder
    // (IconIndex); build them if some other tab hasn't already, and repaint when ready.
    const QString d4 = Config::d4dataDir();
    AppearanceMeta::instance().ensureBuilt(d4, m_index, m_reader);
    IconIndex::instance().ensureBuilt(d4, m_reader);
    // ONCE for the life of the tab, not once per refresh(). reset() now clears m_loaded so the
    // tab repopulates on a new game build, which means refresh() runs again — and these are
    // lambdas on two SINGLETONS that outlive the tab, so Qt::UniqueConnection cannot dedupe them.
    // Without this flag every reload appended another pair and each readyChanged then ran the
    // full fillGrid() one more time than the last.
    if (!m_signalsWired) {
        m_signalsWired = true;
        connect(&IconIndex::instance(), &IconIndex::readyChanged, this,
                [this] { refreshSlotCells(); fillGrid(); });
        connect(&AppearanceMeta::instance(), &AppearanceMeta::readyChanged, this,
                [this] { refreshSlotCells(); fillGrid(); });
    }
    restoreCurrent();        // BEFORE the index: a cache-hit scan auto-selects + saves, which
    ensurePetIndex();        // would otherwise clobber the persisted selection being restored
    selectSlot(SlotMount);
    refreshSlotCells();
    // Default to the first mount so the viewport isn't empty on first open. Item-driven when
    // ready (sets appearance + look + name); appearance-name fallback otherwise.
    if (m_slotSel[SlotMount] == 0) {
        const QVector<StableEntry> ents = entriesFor(SlotMount);
        if (!ents.isEmpty()) {
            const StableEntry& e = ents.first();
            m_slotSel[SlotMount] = e.apprSno;  m_slotName[SlotMount] = e.appr;
            m_slotDisp[SlotMount] = e.name;    m_slotDesc[SlotMount] = e.desc;
            m_slotLook[SlotMount] = e.look;    m_mountType = e.type;
            refreshSlotCells(); fillGrid();
        } else {
            const auto c = candidatesFor(SlotMount);
            if (!c.isEmpty()) {
                m_slotSel[SlotMount] = c.first().second; m_slotName[SlotMount] = c.first().first;
                refreshSlotCells(); fillGrid();
            }
        }
    }
    if (m_view) {
        m_view->setEnvironment(envOrDefault(QSettings().value(QStringLiteral("stable2/env"), 1).toInt()));
        applyLightRig();
        applyGraphics();   // IBL/shadows/SSAO/tonemap/features from stable2/gfx/*
        applyFur();        // fur/mane shell + mesh-FX settings from stable2/fur/* · stable2/fx/*
        applyClothParams();// live cloth/mane sim from stable2/cloth/*
        applyDetailConfig();// detail-map selection from stable2/detail/*
        // Persisted overlay guides, gated by the master toggle (grid / axes / skeleton / bones / physics).
        QSettings ov;
        const bool om = m_overlaysOn;
        m_view->setShowGrid(om && ov.value(QStringLiteral("stable2/ovl/grid"), false).toBool());
        m_view->setShowAxisGizmo(om && ov.value(QStringLiteral("viewer/axisGizmo"), true).toBool());
        m_view->setGridAxisColors(om && ov.value(QStringLiteral("viewer/gridAxisColors"), true).toBool());
        m_view->setShowSkeleton(om && ov.value(QStringLiteral("stable2/ovl/skel"), false).toBool());
        m_view->setShowPhysBones(om && ov.value(QStringLiteral("stable2/ovl/phys"), false).toBool());
        m_view->setShowPhysAxes(om && ov.value(QStringLiteral("stable2/ovl/physaxes"), true).toBool());
        m_view->setShowHardpoints(om && ov.value(QStringLiteral("stable2/ovl/hardpoints"), false).toBool());
        m_view->setShowBoneNames(om && ov.value(QStringLiteral("stable2/ovl/bnm"), false).toBool());
        m_view->setBoneNamesTranslated(ov.value(QStringLiteral("stable2/ovl/bnmtrans"), false).toBool());
        m_view->setBoneNamesHideUnknown(ov.value(QStringLiteral("stable2/ovl/bnmhide"), false).toBool());
        // Re-apply persisted view toggles not covered by the camera state.
        m_view->setAutoSpin(QSettings().value(QStringLiteral("stable2/spin"), false).toBool());
        m_view->setSpinSpeed(QSettings().value(QStringLiteral("stable2/spinSpeed"), 0.025f).toFloat());
    }
    if (QSettings().value(QStringLiteral("stable2/sideCollapsed"), false).toBool())
        setSideCollapsed(true);
    rebuildMount();
    restoreCameraState();   // after the first auto-frame, snap back to the remembered view
    qInfo("startup: stable refresh — %lld ms total (restore + pet index kick + initial mount build)", refT.elapsed());
}

// Background scan of Item/*.itm.json → the AUTHORITATIVE stable rosters, exactly how the game
// models them (see D4 data): MountItem.snoMount → mount Actor (ptMountData.eMountType 0 Horse /
// 1 Cat / 2 Basilisk, snoAppearance = the shared species base mesh, tDefaultLook.dwLookHash =
// the colour variant); HorseArmor/CatArmor items (appearance = the item's own name; there is NO
// ChimeraArmor — Basilisks take no armor, only different versions); Trophy items (snoActor →
// appearance); CompanionItem.snoCompanion → pets. Localized names/descriptions come from the
// enUS_Text StringLists. Cached to disk per d4data build.
void StableTab2::ensurePetIndex()
{
    if (m_petReady || m_petBuilding || !m_index) return;
    m_petBuilding = true;
    const int gen = m_petGen;   // reset() bumps this; a stale build must not install (see below)
    const QString cacheBase = AppPaths::dataDir();
    const QString cachePath = cacheBase + QStringLiteral("/stable_index_v6.bin");
    constexpr quint32 kMagic = 0x7E410061u;   // v6: unk_75d565b inventory-icon handles

    // Field-wise (de)serialization of the entry vectors (StableEntry is a private nested type).
    auto writeVec = [](QDataStream& ds, const QVector<StableEntry>& v) {
        ds << qint32(v.size());
        for (const StableEntry& e : v)
            ds << e.item << e.name << e.desc << e.appr << qint32(e.apprSno) << e.look << qint32(e.type);
    };
    auto readVec = [](QDataStream& ds, QVector<StableEntry>& v) {
        qint32 n = 0; ds >> n;
        if (n < 0 || n > 100000) { v.clear(); return; }
        v.resize(n);
        for (StableEntry& e : v) {
            qint32 sno = 0, ty = -1;
            ds >> e.item >> e.name >> e.desc >> e.appr >> sno >> e.look >> ty;
            e.apprSno = sno; e.type = ty;
        }
    };

    if (QFile::exists(cachePath)) {
        QFile f(cachePath);
        if (f.open(QIODevice::ReadOnly)) {
            QDataStream ds(&f);
            quint32 magic = 0; ds >> magic;
            if (magic == kMagic) {
                QVector<QPair<QString, int>> v; QHash<int, quint32> ic;
                QVector<StableEntry> mounts, armor, trophies, pets;
                ds >> v >> ic;
                readVec(ds, mounts); readVec(ds, armor); readVec(ds, trophies); readVec(ds, pets);
                if (ds.status() == QDataStream::Ok && !ic.isEmpty()) {
                    m_pets = std::move(v); m_iconByApp = std::move(ic);
                    m_mounts = std::move(mounts); m_armorItems = std::move(armor);
                    m_trophyItems = std::move(trophies); m_petItems = std::move(pets);
                    m_petReady = true; m_petBuilding = false;
                    onPetsReady();
                    return;
                }
            }
        }
    }
    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty()) { m_petBuilding = false; return; }
    QHash<QString, int> appByName;
    for (const SnoEntry& e : m_index->entries(kGroupAppearance)) appByName.insert(e.name.toLower(), e.snoId);

    std::thread([this, gen, d4, cacheBase, cachePath, kMagic, appByName, writeVec]() {
        static const QRegularExpression rxType(QStringLiteral("\"snoItemType\"\\s*:\\s*\\{[^}]*?/ItemType/([^.\"/]+)"));
        static const QRegularExpression rxActor(QStringLiteral("\"snoActor\"\\s*:\\s*\\{[^}]*?/Actor/([^.\"/]+)"));
        static const QRegularExpression rxMount(QStringLiteral("\"snoMount\"\\s*:\\s*\\{[^}]*?/Actor/([^.\"/]+)"));
        static const QRegularExpression rxCompanion(QStringLiteral("\"snoCompanion\"\\s*:\\s*\\{[^}]*?/Actor/([^.\"/]+)"));
        static const QRegularExpression rxIcon(QStringLiteral("\"hDefaultImage\"\\s*:\\s*(\\d+)"));
        // The AUTHORITATIVE mount/pet/gear inventory-icon handle: `unk_75d565b`. Verified against
        // the atlases — e.g. cmp_stor105_dogLarge → 2055034561 = frame 0 of the Companion atlas,
        // mnt_stor032_horse → 256596751 = a frame of 2DInventory_Bundle_HMount_stor032. This handle
        // resolves through IconIndex (which indexes every 2D* atlas frame), so no rendering is needed.
        static const QRegularExpression rxInvIcon(QStringLiteral("\"unk_75d565b\"\\s*:\\s*(\\d+)"));
        static const QRegularExpression rxAppr(QStringLiteral("\"snoAppearance\"\\s*:\\s*\\{[^}]*?/Appearance/([^.\"/]+)"));
        static const QRegularExpression rxMountType(QStringLiteral("\"eMountType\"\\s*:\\s*(\\d+)"));
        static const QRegularExpression rxLook(QStringLiteral("\"dwLookHash\"\\s*:\\s*(\\d+)"));

        QVector<StableEntry> mounts, armor, trophies, pets;
        QHash<int, quint32> icons;                    // appearance SNO → inventory-icon handle
        QHash<QString, QString> actorApprCache;       // mount actor name → appearance name (dedupe reads)

        auto entryIcon = [&](const QString& raw, int apprSno) {
            if (apprSno <= 0 || icons.contains(apprSno)) return;
            // Prefer a real hDefaultImage (equipment-style); mounts/pets have it 0, so fall back to
            // the unk_75d565b inventory-icon handle (the authoritative source for stable cosmetics).
            auto mi = rxIcon.globalMatch(raw);
            while (mi.hasNext()) { const quint32 h = mi.next().captured(1).toUInt(); if (h) { icons.insert(apprSno, h); return; } }
            const auto iv = rxInvIcon.match(raw);
            if (iv.hasMatch()) { const quint32 h = iv.captured(1).toUInt(); if (h) icons.insert(apprSno, h); }
        };

        // Items dropped because their appearance name would not join to a SNO. Each branch below
        // ends in a bare `continue`, so the item simply never reaches its picker and the user sees
        // a shorter list with no way to tell that anything was lost. Counted per item type, because
        // "9 mounts missing" and "9 trophies missing" point at completely different causes.
        //
        // Reported on the COLD build only: the result is cached (stable_index_v6.bin) and the
        // cache-hit path returns before this loop runs. That is the right time to hear it — the
        // counts can only change when the index is rebuilt, which is when d4data changed.
        int dropMount = 0, dropArmor = 0, dropTrophy = 0, dropPet = 0;
        QDir dir(d4 + QStringLiteral("/json/base/meta/Item"));
        for (const QString& fn : dir.entryList(QStringList{ QStringLiteral("*.itm.json") }, QDir::Files)) {
            QFile f(dir.filePath(fn));
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QString raw = QString::fromUtf8(f.readAll());
            const auto mt = rxType.match(raw);
            if (!mt.hasMatch()) continue;
            const QString itype = mt.captured(1).toLower();
            const QString base = fn.left(fn.size() - 9);   // strip ".itm.json"

            if (itype == QLatin1String("mountitem")) {
                const auto mm = rxMount.match(raw);
                if (!mm.hasMatch()) { ++dropMount; continue; }   // no snoMount reference at all
                const QString actor = mm.captured(1);
                StableEntry e; e.item = base;
                // The ridable actor carries the appearance + type + colour look — one read,
                // ~150 mount items total (each actor is unique per item, so no cache needed;
                // actorApprCache still dedupes the rare shared-actor case).
                QString apprName = actorApprCache.value(actor);
                {
                    QFile af(d4 + QStringLiteral("/json/base/meta/Actor/") + actor + QStringLiteral(".acr.json"));
                    if (af.open(QIODevice::ReadOnly)) {
                        const QString raw2 = QString::fromUtf8(af.readAll());
                        if (apprName.isNull()) {
                            const auto ap = rxAppr.match(raw2);
                            apprName = ap.hasMatch() ? ap.captured(1) : QStringLiteral("");
                            actorApprCache.insert(actor, apprName);
                        }
                        const auto tm = rxMountType.match(raw2);
                        if (tm.hasMatch()) e.type = tm.captured(1).toInt();
                        const auto lm = rxLook.match(raw2);
                        if (lm.hasMatch()) e.look = lm.captured(1).toUInt();
                    }
                }
                if (apprName.isEmpty()) { ++dropMount; continue; }
                e.appr = apprName;
                e.apprSno = appByName.value(apprName.toLower(), 0);
                if (e.apprSno <= 0) { ++dropMount; continue; }
                readStrings(d4, QStringLiteral("Item_") + base, e.name, e.desc);
                if (e.name.isEmpty()) {   // base colour variants carry their name on the ACTOR instead
                    QString d2;
                    readStrings(d4, QStringLiteral("Actor_") + actor, e.name, d2);
                    if (e.desc.isEmpty()) e.desc = d2;
                }
                if (e.name.isEmpty()) e.name = base;
                entryIcon(raw, e.apprSno);
                mounts.append(e);
            } else if (itype == QLatin1String("horsearmor") || itype == QLatin1String("catarmor")) {
                StableEntry e; e.item = base; e.appr = base;
                e.apprSno = appByName.value(base.toLower(), 0);
                if (e.apprSno <= 0) { ++dropArmor; continue; }   // no worn mesh shipped
                e.type = itype == QLatin1String("horsearmor") ? 0 : 1;
                readStrings(d4, QStringLiteral("Item_") + base, e.name, e.desc);
                if (e.name.isEmpty()) e.name = base;
                entryIcon(raw, e.apprSno);
                armor.append(e);
            } else if (itype == QLatin1String("trophy")) {
                StableEntry e; e.item = base; e.appr = base;
                e.apprSno = appByName.value(base.toLower(), 0);
                if (e.apprSno <= 0) {                          // fallback: item's drop actor name
                    const auto ma = rxActor.match(raw);
                    if (ma.hasMatch()) {
                        e.appr = ma.captured(1);
                        e.apprSno = appByName.value(e.appr.toLower(), 0);
                    }
                }
                if (e.apprSno <= 0) { ++dropTrophy; continue; }
                readStrings(d4, QStringLiteral("Item_") + base, e.name, e.desc);
                if (e.name.isEmpty()) e.name = base;
                entryIcon(raw, e.apprSno);
                trophies.append(e);
            } else if (itype == QLatin1String("companionitem")) {
                const auto mc = rxCompanion.match(raw);
                QString actor = mc.hasMatch() ? mc.captured(1) : QString();
                if (actor.isEmpty()) {
                    const auto ma = rxActor.match(raw);
                    if (ma.hasMatch()) actor = ma.captured(1);
                }
                if (actor.isEmpty()) { ++dropPet; continue; }
                StableEntry e; e.item = base; e.appr = actor;
                e.apprSno = appByName.value(actor.toLower(), 0);
                if (e.apprSno <= 0) { ++dropPet; continue; }
                readStrings(d4, QStringLiteral("Item_") + base, e.name, e.desc);
                if (e.name.isEmpty()) e.name = actor;
                entryIcon(raw, e.apprSno);
                pets.append(e);
            }
        }
        if (dropMount || dropArmor || dropTrophy || dropPet)
            qInfo("stable index: %d item(s) skipped — no appearance resolved "
                  "(mounts %d, barding %d, trophies %d, pets %d); kept %d/%d/%d/%d",
                  dropMount + dropArmor + dropTrophy + dropPet,
                  dropMount, dropArmor, dropTrophy, dropPet,
                  int(mounts.size()), int(armor.size()), int(trophies.size()), int(pets.size()));
        auto byName2 = [](const StableEntry& a, const StableEntry& b) { return a.name.toLower() < b.name.toLower(); };
        std::sort(mounts.begin(), mounts.end(), byName2);
        std::sort(armor.begin(), armor.end(), byName2);
        std::sort(trophies.begin(), trophies.end(), byName2);
        std::sort(pets.begin(), pets.end(), byName2);
        // Legacy (name, sno) pet list — several call sites key off it.
        QVector<QPair<QString, int>> petPairs;
        for (const StableEntry& e : pets) petPairs.append({ e.name, e.apprSno });

        QMetaObject::invokeMethod(this, [this, gen, cacheBase, cachePath, kMagic, writeVec,
                                         petPairs, icons, mounts, armor, trophies, pets]() mutable {
            // The data dir changed under this scan — discard it. Deliberately does NOT clear
            // m_petBuilding: reset() already did, and a newer build may be running by now, so
            // clearing it here would let a third start alongside it.
            if (gen != m_petGen) return;
            // The cache is WRITTEN HERE, on the GUI thread, after the generation check — not on
            // the worker before it. This cache carries no build signature, so MainWindow deletes
            // it on a fingerprint change; a stale worker that wrote it after that deletion would
            // put the OLD build's roster back on disk, where the next launch would load it happily
            // and forever. Writing past the check also serialises the two workers reset() can
            // leave running, and temp+rename keeps a torn file from ever being the real one.
            QDir().mkpath(cacheBase);
            const QString tmp = cachePath + QStringLiteral(".tmp");
            QFile out(tmp);
            if (out.open(QIODevice::WriteOnly)) {
                QDataStream ds(&out);
                ds << kMagic << petPairs << icons;
                writeVec(ds, mounts); writeVec(ds, armor); writeVec(ds, trophies); writeVec(ds, pets);
                out.flush();
                const bool ok = ds.status() == QDataStream::Ok;
                out.close();
                if (ok) { QFile::remove(cachePath); QFile::rename(tmp, cachePath); }
                else    { QFile::remove(tmp); }
            }
            m_pets = std::move(petPairs); m_iconByApp = std::move(icons);
            m_mounts = std::move(mounts); m_armorItems = std::move(armor);
            m_trophyItems = std::move(trophies); m_petItems = std::move(pets);
            m_petReady = true; m_petBuilding = false;
            onPetsReady();
        }, Qt::QueuedConnection);
    }).detach();
}

bool StableTab2::petMode() const
{
    return mountCategory() == QLatin1String("pet");   // the SELECTED mount is a pet
}

// The item scan finished (mounts + gear + pets + icon map): repaint the grid + slot cells so the
// localized names and icons appear. The grid is unfiltered (all mounts + pets, grouped by header).
void StableTab2::onPetsReady()
{
    refreshSlotCells();
    fillGrid();
}

// Item-driven candidates for a slot (empty until the background item scan is ready). The Mount
// grid is unfiltered — ALL mounts and pets at once, grouped by fillGrid's section headers.
QVector<StableTab2::StableEntry> StableTab2::entriesFor(int slot) const
{
    QVector<StableEntry> out;
    if (!m_petReady) return out;

    if (slot == SlotMount) {
        out += m_mounts;
        out += m_petItems;
        QSet<QString> petKeys;
        for (const StableEntry& p : m_petItems) petKeys.insert(p.item);
        std::sort(out.begin(), out.end(), [&petKeys](const StableEntry& a, const StableEntry& b) {
            auto tokOf = [&petKeys](const StableEntry& e) {
                if (petKeys.contains(e.item)) return QStringLiteral("pet");
                const QString t = typeToken(e.type);
                return t.isEmpty() ? catOf(e.appr.toLower()) : t;
            };
            const int ra = catRank(tokOf(a)), rb = catRank(tokOf(b));
            return ra != rb ? ra < rb : a.name.toLower() < b.name.toLower();
        });
        return out;
    }
    if (slot == SlotBarding) {
        const QString cat = mountCategory();
        // Pets have no gear; Basilisks take NO armor (there is no ChimeraArmor item type in the
        // game data — Basilisks come as whole different versions instead).
        if (cat.isEmpty() || cat == QLatin1String("pet") || cat == QLatin1String("chimera")) return out;
        const int want = cat == QLatin1String("horse") ? 0 : cat == QLatin1String("cat") ? 1 : -2;
        for (const StableEntry& e : m_armorItems)
            if (e.type == want) out.append(e);
        return out;
    }
    if (slot == SlotTrophy) {
        if (mountCategory() == QLatin1String("pet")) return out;
        out = m_trophyItems;
    }
    return out;
}

// The category token of the currently-selected mount: "pet" if it's a companion, else the
// authoritative eMountType token (horse/cat/chimera) when known, else the trailing species
// token of its appearance name. Empty if no mount.
QString StableTab2::mountCategory() const
{
    const int sno = m_slotSel[SlotMount];
    if (sno <= 0) return QString();
    for (const auto& p : m_pets) if (p.second == sno) return QStringLiteral("pet");
    const QString t = typeToken(m_mountType);
    return t.isEmpty() ? catOf(m_slotName[SlotMount].toLower()) : t;
}

QVector<QPair<QString, int>> StableTab2::candidatesFor(int slot) const
{
    QVector<QPair<QString, int>> out;
    if (!m_index || !m_index->isLoaded()) return out;
    // Item-driven when the scan is ready: real unlockable items with localized names.
    if (m_petReady && !m_mounts.isEmpty()) {
        for (const StableEntry& e : entriesFor(slot)) out.append({ e.name, e.apprSno });
        return out;
    }
    if (slot == SlotMount) {
        // ALL base mounts + pets at once (no category filter) — fillGrid groups them under headers.
        QSet<int> petSnos;
        for (const auto& p : m_pets) petSnos.insert(p.second);
        for (const SnoEntry& e : m_index->entries(kGroupAppearance)) {
            const QString lower = e.name.toLower();
            if (!lower.startsWith(QLatin1String("mnt_")) || looksLikeFxFragment(lower)) continue;
            if (!lower.contains(QLatin1String("base"))) continue;
            const QString cat = catOf(lower);
            if (!isSpeciesTok(cat)) continue;
            out.append({ e.name, e.snoId });
        }
        out += m_pets;   // companions
        std::sort(out.begin(), out.end(), [&](const auto& a, const auto& b) {
            const QString ca = petSnos.contains(a.second) ? QStringLiteral("pet") : catOf(a.first.toLower());
            const QString cb = petSnos.contains(b.second) ? QStringLiteral("pet") : catOf(b.first.toLower());
            const int ra = catRank(ca), rb = catRank(cb);
            return ra != rb ? ra < rb : a.first < b.first;
        });
        return out;
    }
    if (slot == SlotBarding) {
        // Mount-specific: a horse only lists horse bardings, cat→cat, etc. Pets have none.
        const QString cat = mountCategory();
        if (cat.isEmpty() || cat == QLatin1String("pet")) return out;
        for (const SnoEntry& e : m_index->entries(kGroupAppearance)) {
            const QString lower = e.name.toLower();
            if (!lower.startsWith(QLatin1String("mnt_")) || looksLikeFxFragment(lower)) continue;
            if (!(lower.contains(QLatin1String("amor")) || lower.contains(QLatin1String("armor")))) continue;
            if (!lower.contains(QLatin1String("_") + cat)) continue;
            out.append({ e.name, e.snoId });
        }
    } else if (slot == SlotTrophy) {
        for (const SnoEntry& e : m_index->entries(kGroupAppearance)) {
            const QString lower = e.name.toLower();
            if (!lower.startsWith(QLatin1String("mnt_")) || looksLikeFxFragment(lower)) continue;
            if (lower.contains(QLatin1String("trophy"))) out.append({ e.name, e.snoId });
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

// ── Slot cells + card grid ─────────────────────────────────────────────────────
void StableTab2::selectSlot(int slot)
{
    m_activeSlot = qBound(0, slot, SlotCount - 1);
    if (m_slotCell[m_activeSlot]) { QSignalBlocker b(m_slotCellGroup); m_slotCell[m_activeSlot]->setChecked(true); }
    rebuildCollections();
    fillGrid();
}

QImage StableTab2::slotIcon(int sno) const
{
    if (sno <= 0) return QImage();
    quint32 h = m_iconByApp.value(sno, 0);            // mount/barding/trophy/pet icon (our crawl)
    if (!h) h = AppearanceMeta::instance().iconFor(sno);   // equipment-style fallback
    if (!h) return QImage();
    return IconIndex::instance().iconImage(h, m_reader);
}

void StableTab2::refreshSlotCells()
{
    static const char* kSlotLabels[SlotCount] = { "Mount", "Mount Armor", "Trophy" };
    const QString cat = mountCategory();
    const bool pet = cat == QLatin1String("pet");
    const bool basilisk = cat == QLatin1String("chimera");
    // Pets have no gear at all; Basilisks take no armor (no ChimeraArmor exists in game data —
    // they come as whole different versions instead) but DO take trophies.
    m_slotCell[SlotBarding]->setEnabled(!pet && !basilisk);
    m_slotCell[SlotTrophy]->setEnabled(!pet);
    m_slotCell[SlotBarding]->setToolTip(
        basilisk ? QStringLiteral("Basilisks don't take armor — they come as different versions instead.")
        : pet    ? QStringLiteral("Pets have no gear.")
                 : QStringLiteral("Select the Mount Armor slot"));
    // Selected mount's localized name + description (the in-game stable shows both).
    if (m_infoLbl) {
        if (m_slotSel[SlotMount] > 0 && !m_slotDisp[SlotMount].isEmpty()) {
            QString html = QStringLiteral("<b>%1</b>").arg(m_slotDisp[SlotMount].toHtmlEscaped());
            if (!m_slotDesc[SlotMount].isEmpty())
                html += QStringLiteral("<br><span style='color:#999'>%1</span>")
                            .arg(m_slotDesc[SlotMount].toHtmlEscaped());
            m_infoLbl->setText(html);
            m_infoLbl->setVisible(true);
        } else {
            m_infoLbl->clear();
            m_infoLbl->setVisible(false);
        }
    }
    for (int i = 0; i < SlotCount; ++i) {
        if (!m_slotCell[i]) continue;
        const QImage ic = slotIcon(m_slotSel[i]);
        m_slotCell[i]->setIcon(ic.isNull() ? QIcon() : QIcon(QPixmap::fromImage(ic)));
        QString label = (i == SlotMount && pet) ? QStringLiteral("Pet") : QString::fromLatin1(kSlotLabels[i]);
        if (m_slotSel[i] > 0 && !m_slotName[i].isEmpty()) {
            QString title = m_slotDisp[i];   // localized item name (item-driven path)
            if (title.isEmpty()) title = AppearanceMeta::instance().titleFor(m_slotSel[i]);
            QString n = title.isEmpty() ? m_slotName[i] : title;
            if (n.size() > 12) n = n.left(11) + QChar(0x2026);
            m_slotCell[i]->setText(label + QStringLiteral("\n") + n);
            m_slotCell[i]->setToolTip(title.isEmpty() ? m_slotName[i] : title + QStringLiteral("\n") + m_slotName[i]);
        } else {
            m_slotCell[i]->setText(label);
            if (i != SlotBarding) m_slotCell[i]->setToolTip(QString());   // barding keeps its rule tip
        }
    }
}

void StableTab2::rebuildCollections()
{
    if (!m_collFilter) return;
    QSignalBlocker b(m_collFilter);
    const QString cur = m_collFilter->currentData().toString();
    m_collFilter->clear();
    m_collFilter->addItem(QStringLiteral("All collections"), QString());
    QSet<QString> seen;
    AppearanceMeta& am = AppearanceMeta::instance();
    for (const auto& it : candidatesFor(m_activeSlot)) {
        const QString c = am.collectionFor(it.second);
        if (c.isEmpty() || seen.contains(c)) continue;
        seen.insert(c);
    }
    QStringList colls(seen.constBegin(), seen.constEnd());
    colls.sort();
    for (const QString& c : colls) m_collFilter->addItem(c, c);
    const int idx = cur.isEmpty() ? 0 : m_collFilter->findData(cur);
    m_collFilter->setCurrentIndex(idx >= 0 ? idx : 0);
    m_collFilter->setVisible(!colls.isEmpty());
}

void StableTab2::fillGrid()
{
    if (!m_gridLayout) return;
    // Clear the previous cards + button group.
    while (QLayoutItem* it = m_gridLayout->takeAt(0)) { if (it->widget()) it->widget()->deleteLater(); delete it; }
    delete m_gridGroup;
    m_gridGroup = new QButtonGroup(this);
    m_gridGroup->setExclusive(true);

    const QString filter = m_search ? m_search->text().trimmed().toLower() : QString();
    const QString coll = m_collFilter ? m_collFilter->currentData().toString() : QString();
    AppearanceMeta& am = AppearanceMeta::instance();

    const bool itemsReady = m_petReady && !m_mounts.isEmpty();
    const bool optional = (m_activeSlot != SlotMount);   // barding/trophy get a "(none)" card
    const bool grouped = (m_activeSlot == SlotMount);    // mounts get category header rows

    // Responsive card sizing (matches the Wardrobe/Models picker grids).
    int cols = 2, cardW = 128, cardH = 146, iconW = 112;
    const int availW = m_gridScroll ? m_gridScroll->viewport()->width() : 300;
    cardMetrics(availW, cols, cardW, cardH, iconW);
    m_gridCols = cols;   // remember so the resize handler reflows only on an actual column change

    int row = 0, col = 0;
    auto addHeader = [&](const QString& text) {
        if (col != 0) { col = 0; ++row; }
        auto* h = new QLabel(text);
        h->setStyleSheet(QStringLiteral("color:#bbb; font-weight:bold; padding:6px 2px 2px 2px;"));
        m_gridLayout->addWidget(h, row, 0, 1, cols);
        ++row;
    };
    auto makeCard = [&](const QString& disp0, const QString& tip, int iconSno,
                        const QString& apprName, bool checked) {
        auto* b = new QToolButton;
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setIconSize(QSize(iconW, iconW));
        b->setFixedSize(cardW, cardH);
        b->setStyleSheet(QLatin1String(kCardBaseQss));   // white border when equipped, like the other tabs
        QString disp = disp0;
        if (disp.size() > 22) disp = disp.left(21) + QChar(0x2026);
        b->setText(disp);
        b->setToolTip(tip);
        b->setProperty("sno", iconSno);   // so a resolved icon can find its card
        if (iconSno > 0) {
            if (m_thumbs.contains(iconSno)) b->setIcon(QIcon(badgeIcon(iconSno, m_thumbs.value(iconSno))));  // cached portrait
            else queueThumb(iconSno, apprName);   // resolve the ORIGINAL 2D portrait (deferred, no render)
        }
        b->setChecked(checked);
        m_gridLayout->addWidget(b, row, col);
        if (++col >= cols) { col = 0; ++row; }
        return b;
    };

    if (itemsReady) {
        // ── Item-driven path: cards are the game's actual unlockable items (localized names +
        // descriptions, colour-variant looks, authoritative Horse/Cat/Basilisk grouping). ──
        m_gridEntries = entriesFor(m_activeSlot);
        QSet<QString> petKeys;
        for (const StableEntry& p : m_petItems) petKeys.insert(p.item);
        if (optional) {
            QToolButton* none = makeCard(QStringLiteral("(none)"), QString(), 0,
                                         QString(), m_slotSel[m_activeSlot] == 0);
            none->setProperty("eidx", -1);
            // Every other card in this grid answers a right-click; this one silently did not.
            none->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(none, &QWidget::customContextMenuRequested, this, [this, none](const QPoint& p) {
                const int slot = m_activeSlot;
                QMenu menu;
                QAction* a = menu.addAction(QStringLiteral("Clear"), this, [this, slot] {
                    pushUndo();
                    m_slotSel[slot] = 0; m_slotName[slot].clear(); m_slotDisp[slot].clear();
                    m_slotDesc[slot].clear(); m_slotLook[slot] = 0;
                    refreshSlotCells(); fillGrid(); scheduleRebuild();
                });
                a->setEnabled(m_slotSel[slot] > 0);
                menu.exec(none->mapToGlobal(p));
            });
            m_gridGroup->addButton(none);
        }
        QString curCat;
        for (int i = 0; i < m_gridEntries.size(); ++i) {
            const StableEntry& e = m_gridEntries[i];
            if (!filter.isEmpty() && !e.name.toLower().contains(filter)
                && !e.item.toLower().contains(filter)) continue;
            if (!coll.isEmpty() && am.collectionFor(e.apprSno) != coll) continue;
            if (grouped) {
                QString tok = petKeys.contains(e.item) ? QStringLiteral("pet") : typeToken(e.type);
                if (tok.isEmpty()) tok = catOf(e.appr.toLower());
                const QString cat = typeLabel(tok);   // "chimera" header reads "Basilisk"
                if (cat != curCat) { addHeader(cat); curCat = cat; }
            }
            // Card tooltip — lines per Settings ▸ General ▸ On-hover ▸ Stable tab, colour-coded to
            // match the other tabs (name white, appearance file grey, series gold, flavour parchment).
            const bool colour = HoverInfo::colourCode();
            auto tint = [colour](const QString& s, const char* hex) {
                return colour ? QStringLiteral("<span style='color:%1'>%2</span>")
                                    .arg(QLatin1String(hex), s.toHtmlEscaped()) : s.toHtmlEscaped();
            };
            QStringList tl;
            if (HoverInfo::on("st/name") && !e.name.isEmpty())
                tl << QStringLiteral("<b>%1</b>").arg(tint(e.name, HoverInfo::Col::kName));
            if (HoverInfo::on("st/desc") && !e.desc.isEmpty())
                tl << QStringLiteral("<i>%1</i>").arg(tint(e.desc, HoverInfo::Col::kFlavor));
            if (HoverInfo::on("st/collType")) {
                QString ct = typeLabel(typeToken(e.type));
                const QString cl = am.collectionFor(e.apprSno);
                if (!cl.isEmpty()) ct += (ct.isEmpty() ? QString() : QStringLiteral(" · ")) + cl;
                if (!ct.isEmpty()) tl << tint(ct, HoverInfo::Col::kSeries);
            }
            tl << tint(QStringLiteral("(") + e.appr + QLatin1Char(')'), HoverInfo::Col::kFile);
            const QString tip = tl.join(QStringLiteral("<br>"));
            const bool checked = e.apprSno == m_slotSel[m_activeSlot]
                              && e.look == m_slotLook[m_activeSlot]
                              && (m_activeSlot != SlotMount || e.appr == m_slotName[SlotMount]);
            QToolButton* b = makeCard(e.name, tip, e.apprSno, e.appr, checked);
            b->setProperty("eidx", i);
            m_gridGroup->addButton(b);

            // Right-click card menu (Wardrobe parity): equip / theme / export / copy.
            const bool isPet = petKeys.contains(e.item);
            const StableEntry entry = e;   // capture by value (m_gridEntries is rebuilt each fill)
            const int slot = m_activeSlot;
            b->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(b, &QWidget::customContextMenuRequested, this,
                    [this, b, entry, isPet, slot](const QPoint& p) {
                        QMenu menu;
                        menu.addAction(QStringLiteral("Equip"), this, [this, slot, entry] { equipEntry(slot, entry); });
                        // Theme actions only make sense on a real mount card.
                        if (slot == SlotMount && !isPet) {
                            StableEntry armor, trophy;
                            const bool haveA = matchingSetPiece(entry, SlotBarding, armor);
                            const bool haveT = matchingSetPiece(entry, SlotTrophy, trophy);
                            if (haveA || haveT) {
                                menu.addSeparator();
                                QAction* thm = menu.addAction(
                                    QStringLiteral("Equip theme  (%1)").arg(themeItemCount(entry)),
                                    this, [this, entry] { equipMountTheme(entry); });
                                thm->setEnabled(true);
                                QAction* aA = menu.addAction(
                                    QStringLiteral("Equip Armor  (%1)").arg(haveA ? armor.name : QStringLiteral("None")),
                                    this, [this, armor] { equipEntry(SlotBarding, armor); });
                                aA->setEnabled(haveA);
                                QAction* aT = menu.addAction(
                                    QStringLiteral("Equip Trophy  (%1)").arg(haveT ? trophy.name : QStringLiteral("None")),
                                    this, [this, trophy] { equipEntry(SlotTrophy, trophy); });
                                aT->setEnabled(haveT);
                            }
                        }
                        menu.addSeparator();
                        const QString exSuffix = exportMenuSuffix(entry.appr, isPet);
                        // Match the Wardrobe/viewport wording: destination + size, not "1 model".
                        {
                            const QString exDir = ViewportPartMenu::condensePath(
                                QSettings().value(QStringLiteral("stable2/exportDir")).toString());
                            QStringList ex = exSuffix.split(QStringLiteral(" + "), Qt::SkipEmptyParts);
                            for (int i = ex.size() - 1; i >= 0; --i)
                                if (ex[i].trimmed() == QLatin1String("1 model")) ex.removeAt(i);
                            const QString extra = ex.isEmpty()
                                ? QString() : QStringLiteral("  —  %1").arg(ex.join(QStringLiteral(" + ")));
                            if (!exDir.isEmpty())
                                menu.addAction(ViewportPartMenu::withValue(
                                                   MenuText::kExportModelLast, exDir) + extra, this,
                                               [this, entry] { exportAppearanceModel(entry.apprSno, entry.appr, true); });
                            menu.addAction(ViewportPartMenu::prompts(MenuText::kExportModel + extra), this,
                                           [this, entry] { exportAppearanceModel(entry.apprSno, entry.appr, false); });
                        }
                        LookIcon::addActions(menu, this, slotIcon(entry.apprSno), entry.appr);
                        menu.addSeparator();
                        auto clip = [](const QString& s) { QGuiApplication::clipboard()->setText(s); };
                        auto prev = [](const QString& s) { return s.size() > 30 ? s.left(29) + QChar(0x2026) : s; };
                        const QString coll = AppearanceMeta::instance().collectionFor(entry.apprSno);
                        menu.addAction(QStringLiteral("%1  (%2)").arg(MenuText::kCopySno).arg(entry.apprSno), this,
                                       [entry, clip] { clip(QString::number(entry.apprSno)); });
                        menu.addAction(QStringLiteral("%1  (%2)").arg(MenuText::kCopyFileName).arg(prev(entry.appr)), this,
                                       [entry, clip] { clip(entry.appr); });
                        menu.addAction(QStringLiteral("%1  (%2)").arg(MenuText::kCopyName).arg(prev(entry.name)), this,
                                       [entry, clip] { clip(entry.name); });
                        QAction* aColl = menu.addAction(QStringLiteral("%1  (%2)").arg(MenuText::kCopyCollection).arg(prev(coll.isEmpty() ? QStringLiteral("—") : coll)), this,
                                       [coll, clip] { clip(coll); });
                        aColl->setEnabled(!coll.isEmpty());
                        menu.exec(b->mapToGlobal(p));
                    });
        }
        if (m_gridLayout->count() == 0)
            m_gridLayout->addWidget(new QLabel(QStringLiteral("  (no items)")), 0, 0);

        connect(m_gridGroup, &QButtonGroup::buttonClicked, this, [this](QAbstractButton* btn) {
            pushUndo();   // snapshot before the change (Ctrl+Z)
            const int slot = m_activeSlot;
            const int eidx = btn->property("eidx").toInt();
            if (eidx < 0) {   // "(none)"
                m_slotSel[slot] = 0; m_slotName[slot].clear();
                m_slotDisp[slot].clear(); m_slotDesc[slot].clear(); m_slotLook[slot] = 0;
            } else if (eidx < m_gridEntries.size()) {
                const StableEntry& e = m_gridEntries[eidx];
                m_slotSel[slot] = e.apprSno;
                m_slotName[slot] = e.appr;
                m_slotDisp[slot] = e.name;
                m_slotDesc[slot] = e.desc;
                m_slotLook[slot] = e.look;
                if (slot == SlotMount) m_mountType = e.type;
            }
            if (slot == SlotMount) {
                // Category may have changed: pets take no gear, Basilisks take no armor, and a
                // barding from another species no longer fits — drop what no longer applies.
                const QString cat = mountCategory();
                const bool pet = cat == QLatin1String("pet");
                const bool noArmor = pet || cat == QLatin1String("chimera");
                const int wantArmor = cat == QLatin1String("horse") ? 0
                                     : cat == QLatin1String("cat") ? 1 : -2;
                bool bardingOk = !noArmor && m_slotSel[SlotBarding] > 0;
                if (bardingOk) {
                    bardingOk = false;
                    for (const StableEntry& a : m_armorItems)
                        if (a.apprSno == m_slotSel[SlotBarding] && a.type == wantArmor) { bardingOk = true; break; }
                }
                if (!bardingOk) {
                    m_slotSel[SlotBarding] = 0; m_slotName[SlotBarding].clear();
                    m_slotDisp[SlotBarding].clear(); m_slotDesc[SlotBarding].clear(); m_slotLook[SlotBarding] = 0;
                }
                if (pet) {
                    m_slotSel[SlotTrophy] = 0; m_slotName[SlotTrophy].clear();
                    m_slotDisp[SlotTrophy].clear(); m_slotDesc[SlotTrophy].clear(); m_slotLook[SlotTrophy] = 0;
                }
            }
            refreshSlotCells();
            scheduleRebuild();
        });
        return;
    }

    // ── Pre-scan fallback: appearance-name driven (the original path). ──
    m_gridEntries.clear();
    QVector<QPair<QString, int>> items = candidatesFor(m_activeSlot);
    QSet<int> petSnos;
    for (const auto& p : m_pets) petSnos.insert(p.second);

    auto addCard = [&](const QString& name, int sno) {
        const QString title = (sno > 0) ? am.titleFor(sno) : QString();
        QToolButton* b = makeCard(title.isEmpty() ? name : title,
                                  title.isEmpty() ? name : title + QStringLiteral("\n") + name,
                                  sno, name, sno == m_slotSel[m_activeSlot]);
        b->setProperty("appName", name);   // raw appearance name — never the tooltip (title breaks roster)
        m_gridGroup->addButton(b, sno);
    };

    if (optional) addCard(QStringLiteral("(none)"), 0);
    QString curCat;
    for (const auto& it : items) {
        if (!filter.isEmpty() && !it.first.toLower().contains(filter)) continue;
        if (!coll.isEmpty() && am.collectionFor(it.second) != coll) continue;
        if (grouped) {
            const QString c = petSnos.contains(it.second) ? QStringLiteral("pet") : catOf(it.first.toLower());
            const QString cat = typeLabel(c);
            if (cat != curCat) { addHeader(cat); curCat = cat; }
        }
        addCard(it.first, it.second);
    }
    if (m_gridLayout->count() == 0) {
        const bool scanning = m_activeSlot == SlotMount && !m_petReady;
        m_gridLayout->addWidget(new QLabel(scanning ? QStringLiteral("  (scanning…)")
                                                     : QStringLiteral("  (no items)")), 0, 0);
    }

    connect(m_gridGroup, &QButtonGroup::idClicked, this, [this](int sno) {
        pushUndo();   // snapshot before the change (Ctrl+Z)
        const int slot = m_activeSlot;
        m_slotSel[slot] = sno;
        if (auto* b = qobject_cast<QToolButton*>(m_gridGroup->checkedButton()))
            m_slotName[slot] = (sno > 0) ? b->property("appName").toString() : QString();
        m_slotDisp[slot].clear(); m_slotDesc[slot].clear(); m_slotLook[slot] = 0;
        if (slot == SlotMount) {
            m_mountType = -1;
            // Picking a mount can change the category: pets have no barding/trophy, and a
            // barding from another species no longer applies — drop what no longer fits.
            const bool pet = petMode();
            m_slotCell[SlotBarding]->setEnabled(!pet);
            m_slotCell[SlotTrophy]->setEnabled(!pet);
            const QString cat = mountCategory();
            if (pet || m_slotName[SlotBarding].isEmpty()
                || !m_slotName[SlotBarding].toLower().contains(QLatin1String("_") + cat)) {
                m_slotSel[SlotBarding] = 0; m_slotName[SlotBarding].clear();
            }
            if (pet) { m_slotSel[SlotTrophy] = 0; m_slotName[SlotTrophy].clear(); }
        }
        refreshSlotCells();
        scheduleRebuild();
    });
}

// Seat the trophy rigidly on the mount at the hardpoint the TROPHY authoritatively names
// (ptItemData[0].tAttachmentHardpointLink.tInfo.dwHash — usually HP_trophy1, ~30% HP_trophy3, a few
// HP_trophy2). ModelAttach::seat bakes the trophy verts at the true model-space socket and pins them
// 100% to the follow bone so it rides the animation. (This is the proven placement; the skeletal
// physics-preserving version had positioning issues, so trophies are static for now.)
static QString seatTrophyOnMount(ModelGeometry& trophy, const QVector<ModelJoint>& mountSkel,
                                 const QString& d4, const QString& mountAppr, const QString& trophyAppr)
{
    if (mountSkel.isEmpty() || trophy.primitives.isEmpty() || d4.isEmpty() || mountAppr.isEmpty())
        return QString();
    const auto hpMap = ModelAttach::loadHardpointMap(apprJsonPath(d4, mountAppr));
    if (hpMap.isEmpty()) return QString();
    quint32 authored = 0;
    if (!trophyAppr.isEmpty()) {
        QFile af(d4 + QStringLiteral("/json/base/meta/Actor/") + trophyAppr + QStringLiteral(".acr.json"));
        if (af.open(QIODevice::ReadOnly)) {
            const QJsonArray items = QJsonDocument::fromJson(af.readAll()).object()
                                         .value(QStringLiteral("ptItemData")).toArray();
            if (!items.isEmpty())
                authored = quint32(items.first().toObject()
                                       .value(QStringLiteral("tAttachmentHardpointLink")).toObject()
                                       .value(QStringLiteral("tInfo")).toObject()
                                       .value(QStringLiteral("dwHash")).toVariant().toULongLong());
        }
    }
    QVector<quint32> order;
    if (authored) order << authored;
    order << 982636814u /*HP_trophy1*/ << 982636816u /*HP_trophy3*/ << 982636815u /*HP_trophy2*/
          << 1401728324u /*HP_saddle*/ << 899481535u /*HP_chestBack*/ << 1373172648u /*HP_back*/;
    for (quint32 hash : order) {
        if (!hash || !hpMap.contains(hash)) continue;
        ModelAttach::Attachment a;
        a.hpHash = hash;
        ModelAttach::seat(trophy, mountSkel, hpMap, a);   // bakes verts + pins to the attach bone
        return Hardpoints::nameForHash(hash);
    }
    return QString();
}

// Material roster for a specific colour-variant LOOK. Mount colour variants share one base
// appearance; the mount actor's tDefaultLook.dwLookHash names the look, which selects a
// different material per sub-object (ptSOAs[lookIndex]). lookHash 0 (or not found) = default.
// reader/meta/sno/idx are carried through purely so the three fallbacks below can reach the meta
// binary: a look table only exists in the .app.json, so an appearance without one has no looks to
// choose between and wants its default roster — which for encrypted content lives in the binary.
static QStringList rosterForLook(CascReader* reader, const QString& d4, const QString& appName,
                                 const QByteArray& meta, int sno, const SnoIndex* idx,
                                 quint32 lookHash)
{
    const auto dflt = [&] {
        return MaterialDecode::appearanceRosterAny(reader, d4, appName, meta, sno, idx);
    };
    if (!lookHash) return dflt();
    QFile f(apprJsonPath(d4, appName));
    if (!f.open(QIODevice::ReadOnly)) return dflt();
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    // Find the look's SOA index by matching szLookName (a hash) against dwLookHash.
    int lookIdx = -1;
    const QJsonArray looks = root.value(QStringLiteral("ptAppearanceLooks")).toArray();
    for (int i = 0; i < looks.size(); ++i) {
        const quint32 h = quint32(looks.at(i).toObject()
                                      .value(QStringLiteral("szLookName")).toVariant().toULongLong());
        if (h == lookHash) { lookIdx = i; break; }
    }
    if (lookIdx <= 0) return dflt();   // default look
    QStringList out;
    for (const QJsonValue& mv : root.value(QStringLiteral("ptAppearanceMaterials")).toArray()) {
        const QJsonArray soas = mv.toObject().value(QStringLiteral("ptSOAs")).toArray();
        const QJsonObject s = soas.at(qMin(lookIdx, soas.size() - 1)).toObject();
        // Per-look material: override beats base; cloth is the fallback (same rule the Models tab uses).
        QString nm = s.value(QStringLiteral("snoOverrideMaterial")).toObject()
                         .value(QStringLiteral("name")).toString();
        if (nm.isEmpty()) nm = s.value(QStringLiteral("snoMaterial")).toObject()
                                   .value(QStringLiteral("name")).toString();
        if (nm.isEmpty()) nm = s.value(QStringLiteral("snoCloth")).toObject()
                                   .value(QStringLiteral("name")).toString();
        out << nm;
    }
    return out;
}

// The base/physics-bone split for an appearance, read from its authored data:
// Appearance/<name>.app.json → tStructure.ptBoneData[0].nBaseBoneCount. Bones at index
// ≥ nBaseBoneCount are the game's simulated cloth/mane/tail/physics bones. A self-contained
// creature/mount skeleton carries its OWN physics bones, so without this the merge heuristic
// ("everything after the first piece is cloth") wrongly counts them all as base → no sim runs.
// Returns 0 when unavailable (caller then falls back to the heuristic).
static int baseBoneCountFor(const QString& d4, const QString& appr)
{
    if (d4.isEmpty() || appr.isEmpty()) return 0;
    QFile f(apprJsonPath(d4, appr));
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonArray bd = root.value(QStringLiteral("tStructure")).toObject()
                              .value(QStringLiteral("ptBoneData")).toArray();
    if (bd.isEmpty()) return 0;
    return bd.first().toObject().value(QStringLiteral("nBaseBoneCount")).toInt(0);
}

// The lowercased shader-map name authored on a material (…/Material/<name>.mat.json →
// tUberMaterial.snoShaderMap.name). This is D4's real shader assignment and the authoritative
// signal for classifying a part (FX / hair / opaque) — far more reliable than the material NAME.
// Empty when the material or its shader is missing.
static QString stableShaderName(const QString& d4, const QString& matName)
{
    if (d4.isEmpty() || matName.isEmpty()) return QString();
    QFile mf(d4 + QStringLiteral("/json/base/meta/Material/") + matName + QStringLiteral(".mat.json"));
    if (!mf.open(QIODevice::ReadOnly)) return QString();
    return QJsonDocument::fromJson(mf.readAll()).object()
        .value(QStringLiteral("tUberMaterial")).toObject()
        .value(QStringLiteral("snoShaderMap")).toObject()
        .value(QStringLiteral("name")).toString().toLower();
}

// Is a shader a visual-effect shader? D4 mount FX (energy manes, particle trails, lightning,
// distortion) all route through vfx_/particle_/…_blend_uber_unlit shaders; opaque body/hair
// shaders (mount_opaque*, mount_hair, actor_opaque_pbr) contain none of these tokens. Verified
// against the full mount material set. Mirrors ModelsTab::matIsFxByShader.
static bool stableShaderIsFx(const QString& sm)
{
    if (sm.isEmpty()) return false;
    static const char* const kTok[] = { "vfx", "particle", "distort", "refract", "glow",
                                        "flipbook", "dissolve", "emissiveflow", "blend_uber_unlit",
                                        "trail", "lightning", "_fx", "fxmesh" };
    for (const char* t : kTok) if (sm.contains(QLatin1String(t))) return true;
    return false;
}

// ── Assemble + texture ─────────────────────────────────────────────────────────
void StableTab2::rebuildMount()
{
    // Cancel any pending debounce. Every synchronous caller (undo, equip theme, equip, reset-to-
    // default, the startup restore) would otherwise be followed 35 ms later by a redundant rebuild
    // of the state it just built. Here rather than at each call site so a new caller cannot forget.
    if (m_rebuildTimer) m_rebuildTimer->stop();

    if (!m_view) return;
    if (!m_reader || !m_reader->isReady()) { if (m_status) m_status->setText(QStringLiteral("CASC not ready.")); return; }
    QElapsedTimer buildT; buildT.start();   // stage timing → "stable: rebuild …" log line
    qint64 tGeom = 0;
    const QString prevAnim = m_playingAnim;   // preserve the playing clip across gear/look changes
    clearAnim();   // the rig is about to change — stop any playing clip
    const QString d4 = Config::d4dataDir();

    // Parse each selected slot into its own appearance geometry (roster-named), in
    // draw order: mount body → barding → trophy.
    QVector<ModelGeometry> parts;
    QVector<int> pieceSlot;          // parallel to `parts`: which slot each piece came from
    QStringList loaded;
    QVector<ModelJoint> mountSkel;   // captured from the base mount, to seat the trophy
    QString trophyDbg;
    for (int s = 0; s < SlotCount; ++s) {
        const int sno = m_slotSel[s];
        if (sno <= 0) continue;
        const QByteArray meta = m_reader->readMetaBySno(quint64(sno));
        const QByteArray payload = m_reader->readPayloadBySno(quint64(sno));
        if (meta.isEmpty() || payload.isEmpty()) continue;
        // Guard the parse: a malformed mesh would otherwise access-violate and kill the process
        // (Stable had no crash guard, unlike Models/Wardrobe). A faulted piece is just skipped.
        ModelGeometry geo;
        const bool parsed = seh::runGuarded("stableParse",
            [&]() { geo = ModelParser::parseApp(meta, payload); });
        if (!parsed || !geo.valid || geo.primitives.isEmpty()) continue;
        // Authored base/physics-bone split: mounts & pets carry their mane/tail/physics bones
        // inside their OWN skeleton, so mark the boundary here (the merge heuristic can't see it).
        // Bones ≥ nBaseBones then get spring-bone physics in the viewport (mane/tail sway).
        const int bbc = baseBoneCountFor(d4, m_slotName[s]);
        if (bbc > 0 && bbc < geo.skeleton.size()) geo.nBaseBones = bbc;
        // Colour-variant look: mounts sharing one base mesh differ only by the look's materials.
        const QStringList roster = rosterForLook(m_reader, d4, m_slotName[s], meta, sno, m_index,
                                                m_slotLook[s]);
        for (MeshPrimitive& p : geo.primitives) p.materialName = roster.value(p.materialIndex);
        if (s == SlotMount) mountSkel = geo.skeleton;
        if (s == SlotTrophy) {
            // Rigidly seat the trophy on the mount's authored trophy hardpoint (bakes its verts at
            // the true model-space socket, pins them to the follow bone so it rides the animation).
            // Proven-correct placement; the skeletal physics-preserving variant mis-positioned, so
            // the trophy is static for now (physics deferred to a visually-verified experiment).
            const QString bone = seatTrophyOnMount(geo, mountSkel, d4, m_slotName[SlotMount], m_slotName[s]);
            trophyDbg = bone.isEmpty() ? QStringLiteral(" · trophy: no attach bone (left at origin)")
                                       : QStringLiteral(" · trophy → %1").arg(bone);
        }
        parts.append(geo);
        pieceSlot.append(s);
        loaded << m_slotName[s];
    }
    if (parts.isEmpty()) {
        m_view->clearGeometry();
        m_lastGeo = ModelGeometry();
        m_partSource.clear(); m_partSourceSno.clear(); m_partSourceSlot.clear();
        if (m_status) m_status->setText(QStringLiteral("No mount selected."));
        return;
    }

    ModelGeometry geo;
    const bool merged = seh::runGuarded("stableMerge",
        [&]() { geo = (parts.size() == 1) ? parts[0] : ModelParser::mergeGeometries(parts); });
    if (!merged || !geo.valid || geo.primitives.isEmpty()) {
        m_view->clearGeometry(); m_lastGeo = ModelGeometry();
        m_partSource.clear(); m_partSourceSno.clear(); m_partSourceSlot.clear();
        if (m_status) m_status->setText(QStringLiteral("Assembly failed."));
        return;
    }
    // Reindex materials sequentially so the exporter's per-material list is unambiguous
    // after the multi-appearance merge (indices can otherwise collide across pieces).
    for (int i = 0; i < geo.primitives.size(); ++i) geo.primitives[i].materialIndex = i;

    // Trace each merged primitive back to the appearance it came from. mergeGeometries concatenates
    // primitives in piece order, so expanding the per-piece counts reproduces the mapping — the same
    // approach WardrobeTab2 uses. If the totals ever disagree the assumption is broken, so fall back
    // to attributing everything to the mount rather than mislabelling parts.
    m_partSource.clear(); m_partSourceSno.clear(); m_partSourceSlot.clear();
    int pieceTotal = 0;
    for (const ModelGeometry& g : parts) pieceTotal += int(g.primitives.size());
    if (pieceTotal == geo.primitives.size()) {
        for (int k = 0; k < parts.size(); ++k) {
            const int sl = pieceSlot.value(k, SlotMount);
            for (int j = 0; j < parts[k].primitives.size(); ++j) {
                m_partSource     << m_slotName[sl];
                m_partSourceSno  << m_slotSel[sl];
                m_partSourceSlot << sl;
            }
        }
    } else {
        for (int i = 0; i < geo.primitives.size(); ++i) {
            m_partSource     << m_slotName[SlotMount];
            m_partSourceSno  << m_slotSel[SlotMount];
            m_partSourceSlot << int(SlotMount);
        }
    }

    m_lastGeo = geo;
    // Guard the GPU upload (flatten + VBO/IBO + cloth build) — the stage that most often faults.
    const bool gpuOk = seh::runGuarded("stableGpu", [&]() { m_view->setGeometry(geo, m_framed); });
    if (!gpuOk) {
        m_view->clearGeometry(); m_lastGeo = ModelGeometry();
        m_partSource.clear(); m_partSourceSno.clear(); m_partSourceSlot.clear();
        if (m_status) m_status->setText(QStringLiteral("Couldn't display this model — skipped."));
        return;
    }
    m_framed = true;
    m_view->setBackfaceCull(false);   // double-sided by default (parity with Wardrobe/Models)
    // Rig hardpoints (attach sockets) for the viewport overlay — read from the MOUNT appearance.
    // The mount is the first merged piece, so its bone indices are unchanged in the merged skeleton.
    m_lastGeo.hardpoints.clear();
    if (!m_slotName[SlotMount].isEmpty())
        Hardpoints::readInto(m_lastGeo, apprJsonPath(d4, m_slotName[SlotMount]));
    m_view->setHardpoints(m_lastGeo.hardpoints);

    const int n = geo.primitives.size();
    QVector<QImage> tex(n), norm(n), orm(n), emis(n), mask(n), trans(n);
    QVector<QImage> detN[3], detR[3];
    for (int k = 0; k < 3; ++k) { detN[k].resize(n); detR[k].resize(n); }
    QVector<float> metal(n), rough(n), emisMul(n, 1.0f), dNInt(n, 1.0f), dRInt(n, 1.0f), dROff(n, 0.0f);
    // 3 floats per part. Never filled before, so the widget fell back to white and a monochrome
    // EMISSIVE mask — which is what D4 mostly authors — lost its colour entirely. emisMul was
    // filled with a hard 1.0 for the same reason: nothing read the material's authored value.
    QVector<float> emisCol(n * 3, 1.0f);
    QVector<QVector3D> dScale(n, QVector3D(8, 8, 8));
    QVector<int> dMetalLayer(n, -1);
    QVector<int> hair(n, 0), skin(n, 0), cloth(n, 0);
    m_partFx = QVector<int>(n, 0); m_partSim = QVector<int>(n, 0);
    m_partHidden = QVector<int>(n, 0);
    m_exportMats = QVector<ModelExporter::ExportMaterial>(n);

    // Per-material caches — SESSION-wide members now (many primitives share one material, and many
    // mounts share materials across rebuilds). Raw decodes are deterministic, so reuse is lossless.
    // Bounded: past ~160 distinct materials the whole pool clears (typical mounts use 10–30).
    if (m_cBase.size() > 160) {
        m_cBase.clear(); m_cNorm.clear(); m_cOrm.clear();
        m_cEmis.clear(); m_cMask.clear(); m_cTrans.clear();
    }
    QHash<QString, QImage>& cBase = m_cBase; QHash<QString, QImage>& cNorm = m_cNorm;
    QHash<QString, QImage>& cOrm  = m_cOrm;  QHash<QString, QImage>& cEmis = m_cEmis;
    QHash<QString, QImage>& cMask = m_cMask; QHash<QString, QImage>& cTrans = m_cTrans;
    QHash<QString, QString> cShader;   // material → its shader-map name (FX/hair classification)
    struct DetailC { QVector<QImage> nrm, rgh; float nInt = 1, rInt = 1, rOff = 0; QVector3D scale{8,8,8}; int metal = -1; };
    QHash<QString, DetailC> cDetail;
    qint64 totalV = 0, totalT = 0;

    // Cutout detection for Blender export: if a base-colour texture carries a meaningful
    // amount of transparency it's an alpha-tested card (mane / feather / tassel / cloth
    // edge), so export it as glTF MASK (double-sided) rather than opaque.
    auto hasCutout = [](const QImage& img) -> bool {
        if (img.isNull() || !img.hasAlphaChannel()) return false;
        const int sx = qMax(1, img.width() / 64), sy = qMax(1, img.height() / 64);
        int transp = 0, total = 0;
        for (int y = 0; y < img.height(); y += sy)
            for (int x = 0; x < img.width(); x += sx) {
                ++total; if (qAlpha(img.pixel(x, y)) < 200) ++transp;
            }
        return total > 0 && transp * 100 / total >= 3;   // ≥3% transparent texels
    };

    tGeom = buildT.elapsed();   // geometry parse/merge done; the texture decode pass follows
    // Guard the whole texture-decode pass (CASC reads + BC decode): a corrupt texture would
    // otherwise fault unguarded and crash. On fault the model still shows, just untextured.
    seh::runGuarded("stableTex", [&]() {
    for (int i = 0; i < n; ++i) {
        const MeshPrimitive& p = geo.primitives[i];
        const QString& m = p.materialName;
        auto cached = [&](QHash<QString, QImage>& c, auto fn) -> QImage {
            auto it = c.constFind(m); if (it != c.constEnd()) return it.value();
            const QImage img = fn(); c.insert(m, img); return img;
        };
        const QImage base = cached(cBase, [&] { return MaterialDecode::baseColor(m_reader, d4, m); });
        const QImage nrm = cached(cNorm, [&] { return MaterialDecode::normalMap(m_reader, d4, m); });
        const QImage orw = cached(cOrm, [&] { return MaterialDecode::orm(m_reader, d4, m); });
        const QImage emi = cached(cEmis, [&] { return MaterialDecode::byRole(m_reader, d4, m, "EMISSIVE"); });
        // MASK_PRIMARY + TRANSLUCENCY: the hair shader cuts the mane/tail cards by the mask —
        // without it they render as opaque rectangles (the untextured-card bug).
        const QImage msk = cached(cMask, [&] { return MaterialDecode::byRole(m_reader, d4, m, "MASK_PRIMARY"); });
        const QImage trn = cached(cTrans, [&] { return MaterialDecode::byRole(m_reader, d4, m, "TRANSLUCENCY"); });
        tex[i] = base; norm[i] = nrm; orm[i] = orw; emis[i] = emi; mask[i] = msk; trans[i] = trn;

        // Detail maps (leather / metal micro-surface on barding).
        DetailC dc;
        auto dit = cDetail.constFind(m);
        if (dit != cDetail.constEnd()) { dc = dit.value(); }
        else {
            const float nI[3] = { 1, 1, 1 }, rI[3] = { 1, 1, 1 }, rO[3] = { 0, 0, 0 };
            QVector<QImage> outN, outR; float sN = 1, sR = 1, sO = 0; float sc[3] = { 8, 8, 8 }; int ml = -1;
            MaterialDecode::detailMapsSeparate(m_reader, d4, m, nI, rI, rO, outN, outR, sN, sR, sO, sc, ml);
            outN.resize(3); outR.resize(3);
            dc.nrm = outN; dc.rgh = outR; dc.nInt = sN; dc.rInt = sR; dc.rOff = sO;
            dc.scale = QVector3D(sc[0], sc[1], sc[2]); dc.metal = ml;
            cDetail.insert(m, dc);
        }
        for (int k = 0; k < 3; ++k) { detN[k][i] = dc.nrm.value(k); detR[k][i] = dc.rgh.value(k); }
        dNInt[i] = dc.nInt; dRInt[i] = dc.rInt; dROff[i] = dc.rOff; dScale[i] = dc.scale; dMetalLayer[i] = dc.metal;

        float mt = 0, rg = 1; MaterialDecode::factors(m_reader, d4, m, mt, rg);
        metal[i] = mt; rough[i] = rg;

        const QString ml2 = m.toLower();
        // Shader-driven classification (data, not name guessing). Cache one shader read per material.
        QString shd;
        { auto it = cShader.constFind(m);
          if (it != cShader.constEnd()) shd = it.value();
          else { shd = stableShaderName(d4, m); cShader.insert(m, shd); } }
        // Collision proxies (mnt_globalCapsule*, *_capsule) are invisible physics volumes — never render.
        const bool isCapsule = ml2.contains(QLatin1String("capsule"));
        // FX: authored via a vfx/particle/unlit shader (energy manes, trails, lightning). Fall back to
        // the material name only when the material has no readable shader.
        const bool isFx = shd.isEmpty() ? (ml2.contains(QLatin1String("_fx")) || ml2.contains(QLatin1String("energy"))
                                           || ml2.contains(QLatin1String("effect")) || ml2.contains(QLatin1String("glow")))
                                        : stableShaderIsFx(shd);
        // Hair: the mount_hair shader (or a mane/tail/fur material) → the wispy anisotropic hair path.
        const bool isHair = shd.contains(QLatin1String("hair"))
                            || ml2.contains(QLatin1String("mane")) || ml2.contains(QLatin1String("_tail"))
                            || ml2.contains(QLatin1String("fur"));
        // SIM: the simulation submesh is the invisible low-poly cage that DRIVES the cloth — it has
        // NO material with textures (verified: mnt_*_sim submeshes have no Material/*.mat.json at all,
        // while the visible render mane mnt_*_mane_mat has a real mount_hair* shader). So a part is
        // "sim" exactly when it carries no shader/material (empty shd) — confirmed by the "_sim" name.
        // The physics-DRIVEN render mane (textured, sways) is NOT a sim cage and must stay visible.
        // FX and collision proxies never count.
        const bool isSim = !isFx && !isCapsule
                           && (shd.isEmpty() || ml2.contains(QLatin1String("_sim")));
        hair[i] = isHair ? 1 : 0;
        cloth[i] = (isSim && !isHair) ? 1 : 0;   // hair cards use the hair path, not cloth sim
        m_partSim[i] = isSim ? 1 : 0;
        m_partFx[i] = isFx ? 1 : 0;
        m_partHidden[i] = isCapsule ? 1 : 0;     // collision capsules are force-hidden

        totalV += p.vertices.size(); totalT += p.indices.size() / 3;

        ModelExporter::ExportMaterial em;
        em.name = m;
        em.baseColor = base; em.normal = nrm; em.orm = orw;
        em.hasMetal = true; em.metal = mt; em.hasRough = true; em.rough = rg;
        emisMul[i] = MaterialDecode::materialScalar(d4, m, "emissive multiplier", 1.0f);
        {
            const QColor ec = MaterialDecode::emissiveTint(d4, m, emi, base);
            emisCol[i*3+0] = float(ec.redF());
            emisCol[i*3+1] = float(ec.greenF());
            emisCol[i*3+2] = float(ec.blueF());
        }
        if (!emi.isNull()) {
            em.emissive = emi; em.hasEmissive = true; em.emisMult = emisMul[i];
            em.emisR = emisCol[i*3+0]; em.emisG = emisCol[i*3+1]; em.emisB = emisCol[i*3+2];
        }
        const bool cut = hasCutout(base);
        em.alphaCutout = cut;
        em.alphaCutoff = 0.35f;
        em.doubleSided = p.doubleSided || cut;   // alpha cards read from both sides
        m_exportMats[i] = em;
    }
    }); // end stableTex guard

    m_view->setPartTextures(tex);
    m_view->setPartNormals(norm);
    m_view->setPartOrm(orm);
    m_view->setPartEmissive(emis);
    m_view->setPartMask(mask);
    m_view->setPartTranslucency(trans);
    m_view->setPartEmissiveMult(emisMul);
    m_view->setPartEmissiveColor(emisCol);
    m_view->setPartDetailNormals(detN[0], detN[1], detN[2]);
    m_view->setPartDetailRoughs(detR[0], detR[1], detR[2]);
    m_view->setPartDetailIntensity(dNInt, dRInt);
    m_view->setPartDetailROffset(dROff);
    m_view->setPartDetailScales(dScale);
    m_view->setPartDetailMetalLayer(dMetalLayer);
    m_view->setPartFactors(metal, rough);
    m_view->setPartFlags(hair, skin, cloth);
    m_view->setFeatureDye(false);
    m_view->setFeatureIbl(true);
    m_view->setFeatureTonemap(true);

    rebuildPartList();
    recomputePartVisibility();
    m_view->setEnvironment(envOrDefault(QSettings().value(QStringLiteral("stable2/env"), 1).toInt()));
    applyLightRig();
    // New geometry means a new rig and a rebuilt cloth sim. Re-push EVERY overlay (this also calls
    // applyClothParams for the mane/tail/cloth sim), so toggles survive a mount swap.
    reapplyOverlays();
    populateAnims();      // discover clips for the (possibly new) rig
    // Auto-play the model's nav-idle (like in-game). If a clip was already playing and the new rig
    // still has it (gear/look swap), keep that instead of resetting to idle.
    {
        const QStringList rows = currentClipRows();
        auto clipOf = [](const QString& r) { return r.section(QStringLiteral("  ·  "), 0, 0); };
        QString toPlay;
        if (!prevAnim.isEmpty())
            for (const QString& r : rows) if (clipOf(r) == prevAnim) { toPlay = prevAnim; break; }
        if (toPlay.isEmpty())            // prefer *_nav_idle, then any *idle*
            for (const QString& r : rows) if (clipOf(r).toLower().contains(QLatin1String("nav_idle"))) { toPlay = clipOf(r); break; }
        if (toPlay.isEmpty())
            for (const QString& r : rows) if (clipOf(r).toLower().contains(QLatin1String("idle"))) { toPlay = clipOf(r); break; }
        if (!toPlay.isEmpty()) playAnimByName(toPlay);
    }
    saveCurrent();

    if (m_status)
        m_status->setText(QStringLiteral("%1 · %2 parts · %3 verts · %4 tris%5")
                              .arg(loaded.join(QStringLiteral(" + "))).arg(n).arg(totalV).arg(totalT)
                              .arg(trophyDbg));
    qInfo("stable: rebuild %lld ms — geometry %lld · textures+apply %lld (%d parts, %d cached materials)",
          buildT.elapsed(), tGeom, buildT.elapsed() - tGeom, n, int(m_cBase.size()));
}

// Persist / restore the live (unnamed) selection so the tab reopens where you left it.
void StableTab2::saveCurrent()
{
    QSettings s;
    s.setValue(QStringLiteral("stable2/cur/mountSno"), m_slotSel[SlotMount]);
    s.setValue(QStringLiteral("stable2/cur/bardingSno"), m_slotSel[SlotBarding]);
    s.setValue(QStringLiteral("stable2/cur/trophySno"), m_slotSel[SlotTrophy]);
    s.setValue(QStringLiteral("stable2/cur/mountName"), m_slotName[SlotMount]);
    s.setValue(QStringLiteral("stable2/cur/bardingName"), m_slotName[SlotBarding]);
    s.setValue(QStringLiteral("stable2/cur/trophyName"), m_slotName[SlotTrophy]);
    for (int i = 0; i < SlotCount; ++i) {
        s.setValue(QStringLiteral("stable2/cur/look%1").arg(i), m_slotLook[i]);
        s.setValue(QStringLiteral("stable2/cur/disp%1").arg(i), m_slotDisp[i]);
        s.setValue(QStringLiteral("stable2/cur/desc%1").arg(i), m_slotDesc[i]);
    }
    s.setValue(QStringLiteral("stable2/cur/mountType"), m_mountType);
}

void StableTab2::saveCameraState()
{
    // The Camera panel's "Remember camera on relaunch" box wrote stable2/rememberCam and NOTHING
    // read it, so the camera was always saved and always restored and the checkbox did nothing.
    // Gated in both directions now, as the Wardrobe does.
    if (!m_view || !QSettings().value(QStringLiteral("stable2/rememberCam"), true).toBool()) return;
    const GLModelWidget::CamState c = m_view->cameraState();
    if (!c.valid) return;
    QSettings s;
    s.setValue(QStringLiteral("stable2/cam/yaw"), c.yaw);
    s.setValue(QStringLiteral("stable2/cam/pitch"), c.pitch);
    s.setValue(QStringLiteral("stable2/cam/dist"), c.dist);
    s.setValue(QStringLiteral("stable2/cam/fov"), c.fov);
    s.setValue(QStringLiteral("stable2/cam/cx"), c.cx);
    s.setValue(QStringLiteral("stable2/cam/cy"), c.cy);
    s.setValue(QStringLiteral("stable2/cam/cz"), c.cz);
    s.setValue(QStringLiteral("stable2/cam/ortho"), c.ortho);
}

void StableTab2::restoreCameraState()
{
    if (!m_view) return;
    QSettings s;
    if (!s.value(QStringLiteral("stable2/rememberCam"), true).toBool()) return;
    if (!s.contains(QStringLiteral("stable2/cam/yaw"))) return;
    GLModelWidget::CamState c;
    c.yaw = s.value(QStringLiteral("stable2/cam/yaw")).toFloat();
    c.pitch = s.value(QStringLiteral("stable2/cam/pitch")).toFloat();
    c.dist = s.value(QStringLiteral("stable2/cam/dist")).toFloat();
    c.fov = s.value(QStringLiteral("stable2/cam/fov"), 45.0f).toFloat();
    c.cx = s.value(QStringLiteral("stable2/cam/cx")).toFloat();
    c.cy = s.value(QStringLiteral("stable2/cam/cy")).toFloat();
    c.cz = s.value(QStringLiteral("stable2/cam/cz")).toFloat();
    c.ortho = s.value(QStringLiteral("stable2/cam/ortho"), false).toBool();
    c.valid = true;
    m_view->setCameraState(c);
}

void StableTab2::hideEvent(QHideEvent* ev)
{
    saveCameraState();
    QWidget::hideEvent(ev);
}

void StableTab2::restoreCurrent()
{
    QSettings s;
    m_slotSel[SlotMount] = s.value(QStringLiteral("stable2/cur/mountSno"), 0).toInt();
    m_slotSel[SlotBarding] = s.value(QStringLiteral("stable2/cur/bardingSno"), 0).toInt();
    m_slotSel[SlotTrophy] = s.value(QStringLiteral("stable2/cur/trophySno"), 0).toInt();
    m_slotName[SlotMount] = s.value(QStringLiteral("stable2/cur/mountName")).toString();
    m_slotName[SlotBarding] = s.value(QStringLiteral("stable2/cur/bardingName")).toString();
    m_slotName[SlotTrophy] = s.value(QStringLiteral("stable2/cur/trophyName")).toString();
    for (int i = 0; i < SlotCount; ++i) {
        m_slotLook[i] = s.value(QStringLiteral("stable2/cur/look%1").arg(i), 0u).toUInt();
        m_slotDisp[i] = s.value(QStringLiteral("stable2/cur/disp%1").arg(i)).toString();
        m_slotDesc[i] = s.value(QStringLiteral("stable2/cur/desc%1").arg(i)).toString();
    }
    m_mountType = s.value(QStringLiteral("stable2/cur/mountType"), -1).toInt();
    if (petMode()) { m_slotSel[SlotBarding] = m_slotSel[SlotTrophy] = 0;
                     m_slotName[SlotBarding].clear(); m_slotName[SlotTrophy].clear(); }
}

// ── Parts tree ──────────────────────────────────────────────────────────────────
void StableTab2::rebuildPartList()
{
    if (!m_partTree) return;
    QSignalBlocker block(m_partTree);
    m_partTree->clear();
    // A new model just loaded — drop any part selection/highlight from the PREVIOUS model, or the
    // stale part indices would re-highlight an unrelated part in the new mesh.
    m_partTree->clearSelection();
    if (m_view) m_view->setHighlightParts({});
    // One group per equipped appearance (mount / barding / trophy), matching Wardrobe's tree, so a
    // part's owning item is visible at a glance and "Export Model" has an obvious meaning. With a
    // bare mount that is a single group and looks exactly as it did before.
    static const char* kSlotLabel[SlotCount] = { "mount", "barding", "trophy" };
    QTreeWidgetItem* root = nullptr;
    int rootSlot = -2;
    qint64 rootT = 0;
    auto closeGroup = [&] { if (root) root->setText(1, QString::number(rootT)); };
    for (int i = 0; i < m_lastGeo.primitives.size(); ++i) {
        const int sl = m_partSourceSlot.value(i, SlotMount);
        if (sl != rootSlot) {
            closeGroup();
            rootSlot = sl; rootT = 0;
            const QString appName = m_partSource.value(i);
            const QString label = (sl >= 0 && sl < SlotCount)
                ? (appName.isEmpty() ? QString::fromLatin1(kSlotLabel[sl])
                                     : QStringLiteral("%1  ·  %2").arg(QString::fromLatin1(kSlotLabel[sl]), appName))
                : QStringLiteral("parts");
            root = new QTreeWidgetItem(m_partTree, QStringList{ label, QString() });
            root->setData(0, Qt::UserRole, -1);
            root->setFlags(root->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            root->setCheckState(0, Qt::Checked);
        }
        QString name = m_lastGeo.primitives[i].materialName;
        if (name.isEmpty()) name = QStringLiteral("part %1").arg(i);
        if (i < m_partFx.size() && m_partFx[i]) name += QStringLiteral("  [FX]");
        if (i < m_partSim.size() && m_partSim[i]) name += QStringLiteral("  [SIM]");
        const int tris = int(m_lastGeo.primitives[i].indices.size() / 3);
        rootT += tris;
        auto* child = new QTreeWidgetItem(root, QStringList{ name, QString::number(tris) });
        child->setData(0, Qt::UserRole, i);
        child->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
        child->setCheckState(0, Qt::Checked);
    }
    closeGroup();
    m_partTree->expandAll();
    fillMaterialsPanel();
    updateTexTiles(-1);
}

// MATERIALS panel — one row per submesh: # · material name · tris (mirrors the Wardrobe/Models
// "Materials" table). Selecting a row highlights the matching part(s) in the viewport.
void StableTab2::fillMaterialsPanel()
{
    if (!m_matTable) return;
    QSignalBlocker block(m_matTable);
    m_matTable->clear();
    for (int i = 0; i < m_lastGeo.primitives.size(); ++i) {
        const MeshPrimitive& p = m_lastGeo.primitives[i];
        QString mat = p.materialName;
        if (mat.isEmpty()) mat = QStringLiteral("part %1").arg(i);
        const int tris = int(p.indices.size() / 3);
        auto* it = new QTreeWidgetItem(m_matTable,
            QStringList{ QString::number(i), mat, QString::number(tris) });
        it->setData(0, Qt::UserRole, i);
        it->setTextAlignment(0, Qt::AlignRight | Qt::AlignVCenter);
        it->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
    }
}

// Right-click → Copy name / Copy all, for a QTreeWidget (nameCol) or QListWidget (uses the
// clip-name UserRole). Wired onto the PARTS / MATERIALS / ANIMATIONS panels.
void StableTab2::installCopyMenu(QWidget* view, int nameCol)
{
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QWidget::customContextMenuRequested, this, [this, view, nameCol](const QPoint& p) {
        // For item views (QAbstractScrollArea), customContextMenuRequested delivers `p` in VIEWPORT
        // coordinates — feed it straight to itemAt (mapping through the frame shifted it by the header
        // height, so the copied row was one off / "inaccurate"). Exec the menu at the viewport point.
        QWidget* vp = view;
        if (auto* sa = qobject_cast<QAbstractScrollArea*>(view)) vp = sa->viewport();
        const QPoint local = p;
        const QPoint g = vp->mapToGlobal(p);
        QString cur; QStringList all;
        if (auto* tw = qobject_cast<QTreeWidget*>(view)) {
            if (QTreeWidgetItem* it = tw->itemAt(local)) cur = it->text(nameCol);
            std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* it) {
                if (!it->text(nameCol).isEmpty()) all << it->text(nameCol);
                for (int i = 0; i < it->childCount(); ++i) walk(it->child(i));
            };
            for (int i = 0; i < tw->topLevelItemCount(); ++i) walk(tw->topLevelItem(i));
        } else if (auto* lw = qobject_cast<QListWidget*>(view)) {
            auto disp = [](QListWidgetItem* it) {
                const QString n = it->data(Qt::UserRole).toString();
                return n.isEmpty() ? it->text() : n;
            };
            if (QListWidgetItem* it = lw->itemAt(local)) cur = disp(it);
            for (int i = 0; i < lw->count(); ++i)
                if (lw->item(i)->flags() != Qt::NoItemFlags) all << disp(lw->item(i));
        }
        QMenu m;
        QAction* aCopy = m.addAction(QStringLiteral("Copy"));  aCopy->setEnabled(!cur.isEmpty());
        QAction* aAll  = m.addAction(QStringLiteral("Copy all"));   aAll->setEnabled(!all.isEmpty());
        QAction* chosen = m.exec(g);
        if (chosen == aCopy && !cur.isEmpty()) QGuiApplication::clipboard()->setText(cur);
        else if (chosen == aAll) QGuiApplication::clipboard()->setText(all.join(QLatin1Char('\n')));
    });
}

// ── Themed sets ("Equip matching set") ─────────────────────────────────────────────────────────
// D4 sells a mount together with its matching Mount Armor + Trophy as a StoreProduct bundle
// (…/StoreProduct/Bundle_*Mount*.prd.json → arBundledProducts[]). The bundled product stems ARE
// the appearance/item stems Stable already indexes, so classifying them by name is enough to link
// a mount to its set. Scanned once, lazily.
void StableTab2::buildThemeMap()
{
    if (m_themesBuilt) return;
    m_themesBuilt = true;
    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty()) return;
    QDir dir(d4 + QStringLiteral("/json/base/meta/StoreProduct"));
    if (!dir.exists()) return;
    auto classify = [](const QString& stem) -> int {   // 0 mount · 1 armor · 2 trophy · -1 other
        const QString s = stem.toLower();
        if (s.contains(QLatin1String("trophy"))) return 2;
        if (s.contains(QLatin1String("amor")) || s.contains(QLatin1String("armor"))) return 1;
        if (s.contains(QLatin1String("_horse")) || s.contains(QLatin1String("_cat"))
            || s.contains(QLatin1String("_chimera"))) return 0;
        return -1;
    };
    // Only bundles that carry a mount (name contains "Mount") — skips the armor-/weapon-only packs.
    const QStringList files = dir.entryList(QStringList{ QStringLiteral("Bundle_*Mount*.prd.json") }, QDir::Files);
    for (const QString& fn : files) {
        QFile f(dir.filePath(fn));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonArray prods = QJsonDocument::fromJson(f.readAll()).object()
                                     .value(QStringLiteral("arBundledProducts")).toArray();
        QString mount, armor; QStringList trophies;
        for (const QJsonValue& pv : prods) {
            QString tgt = pv.toObject().value(QStringLiteral("__targetFileName__")).toString();
            if (tgt.isEmpty()) continue;
            QString stem = tgt.section(QLatin1Char('/'), -1);   // basename
            if (stem.endsWith(QLatin1String(".prd"))) stem.chop(4);
            switch (classify(stem)) {
                case 0: if (mount.isEmpty()) mount = stem; break;
                case 1: if (armor.isEmpty()) armor = stem; break;
                case 2: trophies << stem; break;
                default: break;
            }
        }
        if (mount.isEmpty()) continue;
        const QString key = mount.toLower();
        if (!armor.isEmpty() && !m_themeArmor.contains(key)) m_themeArmor.insert(key, armor);
        if (!trophies.isEmpty() && !m_themeTrophy.contains(key)) m_themeTrophy.insert(key, trophies);
    }
}


// Equip a mount together with the matching Mount Armor + Trophy from its bundle. Basilisks take
// no armor (armor slot left empty); the first bundled trophy is used.
void StableTab2::equipMountTheme(const StableEntry& mount)
{
    buildThemeMap();
    pushUndo();   // snapshot before the change (Ctrl+Z)
    auto clearSlot = [&](int s) {
        m_slotSel[s] = 0; m_slotName[s].clear(); m_slotDisp[s].clear();
        m_slotDesc[s].clear(); m_slotLook[s] = 0;
    };
    auto setSlot = [&](int s, const StableEntry& e) {
        m_slotSel[s] = e.apprSno; m_slotName[s] = e.appr; m_slotDisp[s] = e.name;
        m_slotDesc[s] = e.desc; m_slotLook[s] = e.look;
    };
    // 1. the mount itself.
    setSlot(SlotMount, mount);
    m_mountType = mount.type;
    // The bundle keys on the mount's item/appearance stem — try both.
    const QString key = m_themeArmor.contains(mount.item.toLower()) || m_themeTrophy.contains(mount.item.toLower())
                            ? mount.item.toLower() : mount.appr.toLower();
    // Match a bundled stem against a roster entry by item OR appearance name (trophies sometimes
    // carry a fallback appearance name, so item is the reliable key).
    auto matches = [](const StableEntry& e, const QString& stem) {
        return e.item.compare(stem, Qt::CaseInsensitive) == 0
            || e.appr.compare(stem, Qt::CaseInsensitive) == 0;
    };
    // 2. matching Mount Armor (skip for Basilisks — none exists).
    clearSlot(SlotBarding);
    int nArmor = 0;
    if (mount.type != 2) {
        const QString armorStem = m_themeArmor.value(key);
        if (!armorStem.isEmpty())
            for (const StableEntry& a : m_armorItems)
                if (matches(a, armorStem)) { setSlot(SlotBarding, a); ++nArmor; break; }
    }
    // 3. matching Trophy (first of the bundle that we actually have indexed).
    clearSlot(SlotTrophy);
    int nTrophy = 0;
    for (const QString& tStem : m_themeTrophy.value(key)) {
        bool found = false;
        for (const StableEntry& t : m_trophyItems)
            if (matches(t, tStem)) { setSlot(SlotTrophy, t); found = true; ++nTrophy; break; }
        if (found) break;
    }
    refreshSlotCells();
    fillGrid();          // re-highlight the now-selected cards
    rebuildMount();
    saveCurrent();
    if (m_status)
        m_status->setText(QStringLiteral("Equipped set: %1%2%3")
                              .arg(mount.name.isEmpty() ? mount.appr : mount.name,
                                   nArmor ? QStringLiteral(" + armor") : QString(),
                                   nTrophy ? QStringLiteral(" + trophy") : QString()));
}

// The matching set piece (armor or trophy) for a mount, from its bundle. Returns false if none
// is indexed. `slot` is SlotBarding (armor) or SlotTrophy.
bool StableTab2::matchingSetPiece(const StableEntry& mount, int slot, StableEntry& out)
{
    buildThemeMap();
    const QString ik = mount.item.toLower(), ak = mount.appr.toLower();
    const QString key = (m_themeArmor.contains(ik) || m_themeTrophy.contains(ik)) ? ik : ak;
    auto matches = [](const StableEntry& e, const QString& stem) {
        return e.item.compare(stem, Qt::CaseInsensitive) == 0
            || e.appr.compare(stem, Qt::CaseInsensitive) == 0;
    };
    if (slot == SlotBarding) {
        if (mount.type == 2) return false;   // Basilisks take no armor
        const QString stem = m_themeArmor.value(key);
        if (stem.isEmpty()) return false;
        for (const StableEntry& a : m_armorItems) if (matches(a, stem)) { out = a; return true; }
        return false;
    }
    if (slot == SlotTrophy) {
        for (const QString& stem : m_themeTrophy.value(key))
            for (const StableEntry& t : m_trophyItems) if (matches(t, stem)) { out = t; return true; }
    }
    return false;
}

// How many set pieces (mount + matching armor + trophy) equipping the theme would apply.
int StableTab2::themeItemCount(const StableEntry& mount)
{
    int n = 1;   // the mount itself
    StableEntry e;
    if (matchingSetPiece(mount, SlotBarding, e)) ++n;
    if (matchingSetPiece(mount, SlotTrophy, e))  ++n;
    return n;
}

// Equip a single item into a slot (right-click "Equip"), mirroring the left-click path including
// the mount-category cleanup (pets take no gear; Basilisks no armor; cross-species barding drops).
void StableTab2::equipEntry(int slot, const StableEntry& e)
{
    pushUndo();
    m_slotSel[slot] = e.apprSno; m_slotName[slot] = e.appr;
    m_slotDisp[slot] = e.name;  m_slotDesc[slot] = e.desc; m_slotLook[slot] = e.look;
    if (slot == SlotMount) {
        m_mountType = e.type;
        const QString cat = mountCategory();
        const bool pet = cat == QLatin1String("pet");
        const bool noArmor = pet || cat == QLatin1String("chimera");
        const int wantArmor = cat == QLatin1String("horse") ? 0 : cat == QLatin1String("cat") ? 1 : -2;
        bool bardingOk = !noArmor && m_slotSel[SlotBarding] > 0;
        if (bardingOk) {
            bardingOk = false;
            for (const StableEntry& a : m_armorItems)
                if (a.apprSno == m_slotSel[SlotBarding] && a.type == wantArmor) { bardingOk = true; break; }
        }
        if (!bardingOk) { m_slotSel[SlotBarding] = 0; m_slotName[SlotBarding].clear();
                          m_slotDisp[SlotBarding].clear(); m_slotDesc[SlotBarding].clear(); m_slotLook[SlotBarding] = 0; }
        if (pet) { m_slotSel[SlotTrophy] = 0; m_slotName[SlotTrophy].clear();
                   m_slotDisp[SlotTrophy].clear(); m_slotDesc[SlotTrophy].clear(); m_slotLook[SlotTrophy] = 0; }
    }
    refreshSlotCells();
    fillGrid();
    rebuildMount();
    saveCurrent();
}

// Export ONE item's mesh (right-click "Export model"): parse the appearance, decode its PBR
// textures the same way rebuildMount does, and write a .glb. `toLast` skips the dialog and reuses
// the last export directory.
void StableTab2::exportAppearanceModel(int sno, const QString& appr, bool toLast)
{
    if (sno <= 0 || !m_reader || !m_reader->isReady()) {
        QMessageBox::information(this, QStringLiteral("Export"), QStringLiteral("Item not ready to export.")); return;
    }
    const QString d4 = Config::d4dataDir();
    const QByteArray meta = m_reader->readMetaBySno(quint64(sno));
    const QByteArray payload = m_reader->readPayloadBySno(quint64(sno));
    ModelGeometry geo;
    const bool ok = seh::runGuarded("stableExportParse",
        [&]() { geo = ModelParser::parseApp(meta, payload); });
    if (!ok || !geo.valid || geo.primitives.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Export"), QStringLiteral("Couldn't read this item's model.")); return;
    }
    const int bbc = baseBoneCountFor(d4, appr);
    if (bbc > 0 && bbc < geo.skeleton.size()) geo.nBaseBones = bbc;
    const QStringList roster =
        MaterialDecode::appearanceRosterAny(m_reader, d4, appr, meta, sno, m_index);
    for (MeshPrimitive& p : geo.primitives) {
        const QString rn = roster.value(p.materialIndex);
        if (!rn.isEmpty()) p.materialName = rn;
    }
    // Per-material PBR decode → ExportMaterial list.
    const bool bakeDetail_ = QSettings().value(QStringLiteral("export/bakeDetail"), false).toBool();
    // One decode cache for this model's materials, same as the batch paths.
    MaterialDecode::TextureCacheScope texCache;
    QVector<ModelExporter::ExportMaterial> mats(geo.primitives.size());
    QHash<QString, ModelExporter::ExportMaterial> cache;
    for (int i = 0; i < geo.primitives.size(); ++i) {
        const QString m = geo.primitives[i].materialName;
        auto it = cache.constFind(m);
        if (it != cache.constEnd()) { mats[i] = it.value(); geo.primitives[i].materialIndex = i; continue; }
        ModelExporter::ExportMaterial em; em.name = m;
        seh::runGuarded("stableExportTex", [&]() {
            em.baseColor = MaterialDecode::baseColor(m_reader, d4, m);
            em.normal    = MaterialDecode::normalMap(m_reader, d4, m);
            em.orm       = MaterialDecode::orm(m_reader, d4, m);
            float mt = 0, rg = 1; MaterialDecode::factors(m_reader, d4, m, mt, rg);
            em.hasMetal = true; em.metal = mt; em.hasRough = true; em.rough = rg;
            const QImage emi = MaterialDecode::byRole(m_reader, d4, m, "EMISSIVE");
            // The material's OWN multiplier, not a hard 1.0. glTF writes emissiveStrength from
            // this, so a hard-coded 1.0 silently flattened every authored glow in the export the
            // same way the missing colour flattened it in the viewport.
            if (!emi.isNull()) {
                em.emissive = emi; em.hasEmissive = true;
                em.emisMult = MaterialDecode::materialScalar(d4, m, "emissive multiplier", 1.0f);
                // The colour too, from the same helper the viewport uses. This path set NEITHER
                // before, so a mount's glow exported with emissiveFactor [0,0,0] — the map present
                // and multiplied to black. Three tabs had three different answers here.
                const QColor ec = MaterialDecode::emissiveTint(d4, m, emi, em.baseColor);
                em.emisR = float(ec.redF());
                em.emisG = float(ec.greenF());
                em.emisB = float(ec.blueF());
            }
            // The third export path that was ignoring export/bakeDetail. exportMount and the Models
            // roster builder were both wired up when the bake became shared; this one — right-click
            // ▸ Export model on a stable item — builds its own materials and was missed, so the
            // setting was still a lie here alone. Cached with the material, like everything above it.
            if (bakeDetail_)
                MaterialDecode::bakeDetailForMaterial(m_reader, d4, m, em.normal, em.orm);
        });
        cache.insert(m, em); mats[i] = em; geo.primitives[i].materialIndex = i;
    }
    const QString base = appr.isEmpty() ? QStringLiteral("model") : appr;
    QString path;
    if (toLast) {
        const QString dir = QSettings().value(QStringLiteral("stable2/exportDir"), QDir::homePath()).toString();
        path = dir + QStringLiteral("/") + base + QStringLiteral(".glb");
    } else {
        const QString dir = QSettings().value(QStringLiteral("stable2/exportDir"), QDir::homePath()).toString();
        path = QFileDialog::getSaveFileName(this, QStringLiteral("Export model"),
                   dir + QStringLiteral("/") + base + QStringLiteral(".glb"), QStringLiteral("glTF Binary (*.glb)"));
        if (path.isEmpty()) return;
    }
    if (!path.endsWith(QStringLiteral(".glb"), Qt::CaseInsensitive)) path += QStringLiteral(".glb");

    // Animations per Settings ▸ Export (matches the count shown in the menu): all of the item's
    // clips, or just the one playing in preview. Both the CHOOSING and the DECODING are shared with
    // every other export path now. This site used to re-derive the carrier inline with no fallback,
    // so a species that does not name its rig mnt_base00_<tok> exported zero clips while the panel
    // beside it listed them.
    QVector<AnimParser::DecodedAnim> anims; QStringList animNames;
    if (QSettings().value(QStringLiteral("export/includeAnim"), false).toBool()) {
        const bool pet = appr.toLower().startsWith(QLatin1String("cmp_"))
                      || appr.toLower().contains(QLatin1String("companion"));
        collectExportAnims(geo, exportClipNames(appr, sno, pet), anims, animNames);
    }
    const int nAnim = anims.size();

    ModelExporter::Options opt = ModelExporter::optionsFromSettings();
    // Hardpoint empties + bone naming + the re-index after any rename. All three were missing from
    // every Stable export path: a mount exported with no HP_saddle/HP_trophy* sockets in Blender,
    // and with raw bone_<hash> names unless "Blender friendly" happened to be on. Same block the
    // Wardrobe uses, in the same order — readInto BEFORE retarget, resolveBoneIndices AFTER any
    // rename, because a hardpoint stores a bone INDEX that a rename or reduction invalidates.
    if (QSettings().value(QStringLiteral("export/hardpointEmpties"), false).toBool())
        Hardpoints::readInto(geo, apprJsonPath(d4, appr));
    Retarget::applyFromSettings(geo);
    if (opt.blenderFriendly) GLModelWidget::blenderizeSkeletonNames(geo.skeleton);
    else if (QSettings().value(QStringLiteral("export/boneNamesTranslated"), false).toBool())
        GLModelWidget::translateSkeletonNames(geo.skeleton);
    Hardpoints::resolveBoneIndices(geo);
    const bool wrote = ModelExporter::exportGlb(geo, path, mats, anims, animNames, opt);
    QSettings().setValue(QStringLiteral("stable2/exportDir"), QFileInfo(path).absolutePath());

    // Raw source files (.app + distinct .tex) into a "deps" folder beside the .glb, when enabled.
    // Folder name and rule match the Models/Bulk path — export/withDeps is one setting and had
    // grown two layouts, which is the thing that makes a shared option unpredictable.
    int nRaw = 0;
    int nUnnamedMat = 0;   // roster slots with no material name — their sources cannot be written
    if (wrote && QSettings().value(QStringLiteral("export/withDeps"), false).toBool()) {
        const QString depDir = QFileInfo(path).dir().filePath(QStringLiteral("deps"));
        QDir().mkpath(depDir);
        QFile af(depDir + QLatin1Char('/') + base + QStringLiteral(".app"));
        if (af.open(QIODevice::WriteOnly)) { af.write(payload); af.close(); ++nRaw; }
        QSet<qint64> doneTex;
        for (const QString& mn : roster) {
            // An EMPTY roster slot is not a failure — appearanceRoster() pads the table so its
            // indices line up with materialIndex, and a slot with no ptSOAs legitimately has no
            // material. Counting those reported "2 materials unresolved" on a complete export.
            if (mn.isEmpty()) continue;
            QFile mf(d4 + QStringLiteral("/json/base/meta/Material/") + mn + QStringLiteral(".mat.json"));
            if (!mf.open(QIODevice::ReadOnly)) {
                // "~unnamed_<sno>" is an ENCRYPTED material: it has a real sno and renders fine, it
                // just has no name and therefore no .mat.json — by far the common case (d4data ships
                // ~1079 of them). Only a NAMED material with no readable .mat.json is a real miss.
                if (!mn.startsWith(QLatin1String("~unnamed_"))) ++nUnnamedMat;
                continue;
            }
            for (const MatTexture& mt : parseMaterialJson(mf.readAll())) {
                if (mt.texSno == 0 || doneTex.contains(mt.texSno)) continue;
                doneTex.insert(mt.texSno);
                const QByteArray tb = m_reader->readPayloadBySno(quint64(mt.texSno));
                if (tb.isEmpty()) continue;
                const QString tn = mt.texName.isEmpty() ? QStringLiteral("tex_%1").arg(mt.texSno) : mt.texName;
                QFile tf(depDir + QLatin1Char('/') + tn + QStringLiteral(".tex"));
                if (tf.open(QIODevice::WriteOnly)) { tf.write(tb); tf.close(); ++nRaw; }
            }
        }
    }

    QString extra;
    if (nAnim) extra += QStringLiteral("  + %1 animation%2").arg(nAnim).arg(nAnim == 1 ? QString() : QStringLiteral("s"));
    if (nRaw)  extra += QStringLiteral("  + %1 raw source file%2").arg(nRaw).arg(nRaw == 1 ? QString() : QStringLiteral("s"));
    // Materials the roster could not name: their .mat/.tex sources are simply absent from _deps.
    // Reported for the same reason ModelsTab_Export logs an empty palette — a short deps folder is
    // otherwise indistinguishable from a complete one.
    if (nUnnamedMat) extra += QStringLiteral("  ·  %1 material%2 unresolved")
                                  .arg(nUnnamedMat).arg(nUnnamedMat == 1 ? QString() : QStringLiteral("s"));
    if (wrote)
        ExportNotifier::instance().notify(
            QStringLiteral("Exported %1%2%3").arg(QFileInfo(path).fileName(), extra,
                                                  ExportNotifier::glbOptionsLine(opt)),
            QFileInfo(path).absolutePath());
    else
        QMessageBox::information(this, QStringLiteral("Export"), QStringLiteral("Export failed."));
}

// TEXTURE PREVIEW — the selected part's five PBR channels, pulled from the maps decoded during
// rebuildMount (no re-decode). -1 clears the tiles.
void StableTab2::updateTexTiles(int partIndex)
{
    static const char* const kCaps[6] = { "COLOR", "ROUGH", "METAL", "NORMAL", "ALPHA", "EMIS" };
    if (!m_texTile[0]) return;
    QImage chans[6];
    if (partIndex >= 0 && partIndex < m_exportMats.size()) {
        const ModelExporter::ExportMaterial& em = m_exportMats[partIndex];
        chans[0] = em.baseColor;
        if (!em.orm.isNull()) {   // ORM = AO(R) · Roughness(G) · Metal(B)
            chans[1] = em.orm.convertToFormat(QImage::Format_RGBA8888);
            chans[2] = chans[1];
        }
        chans[3] = em.normal;
        if (!em.baseColor.isNull() && em.baseColor.hasAlphaChannel()) chans[4] = em.baseColor;
        chans[5] = em.emissive;
    }
    for (int c = 0; c < 6; ++c) {
        QLabel* t = m_texTile[c];
        if (!t) continue;
        const int sz = qMax(24, t->width() - 2);
        if (!chans[c].isNull()) {
            // Scale to the tile FIRST, then (for single-channel tiles) extract the channel on the
            // small image — cheap even for 2K source maps.
            QImage img = chans[c].scaled(sz, sz, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            if (c == 1 || c == 2 || c == 4) {
                img = img.convertToFormat(QImage::Format_RGBA8888);
                for (int y = 0; y < img.height(); ++y)
                    for (int x = 0; x < img.width(); ++x) {
                        const QRgb px = img.pixel(x, y);
                        const int v = c == 1 ? qGreen(px) : c == 2 ? qBlue(px) : qAlpha(px);
                        img.setPixel(x, y, qRgb(v, v, v));
                    }
            }
            t->setPixmap(QPixmap::fromImage(img));
            t->setText(QString());
        } else {
            t->setPixmap(QPixmap());
            t->setText(QString::fromLatin1(kCaps[c]));
        }
        t->setToolTip(QString::fromLatin1(kCaps[c]));
    }
}

// ── Card icons — ORIGINAL 2D portraits only (no 3D rendering) ─────────────────────
// Mounts/pets have no inventory icon HANDLE on hDefaultImage, but the item's unk_75d565b field
// IS the inventory-icon handle (verified against the 2DInventory_Bundle_* atlases), and it
// resolves through IconIndex like any equipment icon. Cards are filled lazily from that handle
// (or, as a backup, a name-matched atlas), cached to memory + disk.
QString StableTab2::thumbPath(int sno) const
{
    // New dir (was stable_thumbs) so any old 3D-rendered pngs are never loaded again.
    return AppPaths::dataDir() + QStringLiteral("/stable_icons/%1.png").arg(sno);
}

// Find + decode the game's baked 2D portrait for an appearance. Mounts/pets have no icon handle,
// but the game ships name-addressed atlases (2DInventory_Bundle_Companion_stor105_dogLarge,
// 2DInventory_Bundle_HMount_cat_stor024, …). Match by the appearance's variant token (storNNN /
// amorNNN / baseNNN / dluxNNN / lunarNNN / eventNNN) plus a descriptor (species / doglarge / …),
// prefer the shortest 2DInventory match, then decode the atlas. Null when no baked portrait exists.
QImage StableTab2::resolveOriginalIcon(const QString& appr)
{
    if (appr.isEmpty() || !m_index || !m_reader) return {};
    if (!m_atlasBuilt) {
        m_atlasBuilt = true;
        for (const SnoEntry& t : m_index->entries(44)) {   // 44 = Texture
            const QString ln = t.name.toLower();
            if (ln.startsWith(QLatin1String("2dinventory")) || ln.startsWith(QLatin1String("2dui")))
                m_atlasIdx.append({ t.name, t.snoId });
        }
    }
    const QString a = appr.toLower();
    static const QRegularExpression rxNum(QStringLiteral("(stor|amor|dlux|lunar|hib|event|base)\\d+"));
    const QString numTok = rxNum.match(a).captured(0);
    if (numTok.isEmpty()) return {};   // no strong discriminator → caller falls back to a render
    const QStringList toks = a.split(QLatin1Char('_'), Qt::SkipEmptyParts);
    QString descTok;
    for (const QString& t : { QStringLiteral("cat"), QStringLiteral("horse"),
                              QStringLiteral("chimera"), QStringLiteral("trophy") })
        if (toks.contains(t)) { descTok = t; break; }
    if (descTok.isEmpty())   // last meaningful token (e.g. "doglarge", "wyvern")
        for (int i = toks.size() - 1; i >= 0; --i) {
            const QString& t = toks[i];
            if (t == QLatin1String("mnt") || t == QLatin1String("cmp") || t == QLatin1String("stor")
                || t.contains(rxNum)) continue;
            descTok = t; break;
        }
    int bestSno = 0; double bestScore = -1e9; QString bestName;
    for (const auto& e : m_atlasIdx) {
        const QString ln = e.first.toLower();
        if (!ln.contains(numTok)) continue;                 // must carry the variant id
        double score = 0;
        if (!descTok.isEmpty() && ln.contains(descTok)) score += 3;
        if (ln.startsWith(QLatin1String("2dinventory"))) score += 1;
        score -= ln.size() * 0.01;                          // prefer the shortest (most specific)
        if (score > bestScore) { bestScore = score; bestSno = e.second; bestName = e.first; }
    }
    if (bestSno <= 0 || (!descTok.isEmpty() && bestScore < 3)) return {};   // require the descriptor to match
    return MaterialDecode::texture(m_reader, Config::d4dataDir(), bestName, bestSno);
}

void StableTab2::queueThumb(int sno, const QString& appr)
{
    if (sno <= 0 || m_thumbs.contains(sno) || m_thumbQueued.contains(sno)) return;
    QPixmap pm;
    if (pm.load(thumbPath(sno)) && !pm.isNull()) {   // disk cache → reuse across sessions
        m_thumbs.insert(sno, pm);
        setCardIcon(sno, pm);
        return;
    }
    m_thumbAppr.insert(sno, appr);
    m_thumbQueued.insert(sno);
    m_thumbQueue.append(sno);
    if (m_thumbTimer && !m_thumbTimer->isActive()) m_thumbTimer->start();
}

// Overlay the model-presence badge (✓ has mesh / ✗ icon-only) on a card thumbnail, per the
// per-tab "stable" settings. Cheap: payloadSize is an O(1) lookup.
QPixmap StableTab2::badgeIcon(int sno, const QPixmap& pm) const
{
    if (pm.isNull() || !IconBadge::anyEnabled(QStringLiteral("stable"))) return pm;
    int st = 0;
    if (m_reader && m_reader->isReady()) st = m_reader->payloadSize(quint64(sno)) > 0 ? 1 : -1;
    return IconBadge::withBadge(pm, st, IconBadge::showPresent(QStringLiteral("stable")),
                                IconBadge::showMissing(QStringLiteral("stable")));
}

void StableTab2::setCardIcon(int sno, const QPixmap& pm)
{
    if (!m_gridGroup) return;
    for (QAbstractButton* b : m_gridGroup->buttons())
        if (b->property("sno").toInt() == sno) { b->setIcon(QIcon(badgeIcon(sno, pm))); break; }
}

// Resolve queued card icons — ORIGINAL 2D portraits only, decoded off the viewport. Primary
// source is the item's inventory-icon handle (unk_75d565b) via IconIndex; secondary is a
// name-matched 2DInventory atlas. NO 3D rendering. Runs a few per tick so the grid never stalls.
void StableTab2::processThumbs()
{
    if (m_thumbQueue.isEmpty()) { m_thumbTimer->stop(); return; }
    if (!m_reader || !m_reader->isReady()) { m_thumbTimer->stop(); return; }
    QDir().mkpath(AppPaths::dataDir() + QStringLiteral("/stable_icons"));
    int done = 0;
    while (!m_thumbQueue.isEmpty() && done < 8) {
        const int sno = m_thumbQueue.takeFirst();
        m_thumbQueued.remove(sno);
        const QString appr = m_thumbAppr.take(sno);
        ++done;
        QImage img;                                          // guarded: CASC read + BC decode of the atlas
        seh::runGuarded("stableIcon", [&]() {
            img = slotIcon(sno);                             // handle-based portrait (IconIndex)
            if (img.isNull()) img = resolveOriginalIcon(appr); // name-matched 2DInventory atlas
        });
        if (img.isNull()) continue;                          // no baked portrait → leave blank
        const QPixmap pm = QPixmap::fromImage(
            img.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        m_thumbs.insert(sno, pm);
        pm.save(thumbPath(sno), "PNG");
        setCardIcon(sno, pm);
    }
    if (m_thumbQueue.isEmpty()) m_thumbTimer->stop();
}

void StableTab2::recomputePartVisibility()
{
    if (!m_view) return;
    const bool showFx = !m_fxChk || m_fxChk->isChecked();
    const bool showSim = !m_simChk || m_simChk->isChecked();
    QHash<int, bool> checked;
    if (m_partTree)
        for (int r = 0; r < m_partTree->topLevelItemCount(); ++r) {
            QTreeWidgetItem* root = m_partTree->topLevelItem(r);
            for (int c = 0; c < root->childCount(); ++c) {
                QTreeWidgetItem* it = root->child(c);
                checked[it->data(0, Qt::UserRole).toInt()] = (it->checkState(0) == Qt::Checked);
            }
        }
    const int cnt = qMax(m_partFx.size(), m_partSim.size());
    for (int i = 0; i < cnt; ++i) {
        const bool isFx = i < m_partFx.size() && m_partFx[i];
        const bool isSim = i < m_partSim.size() && m_partSim[i];
        const bool hidden = i < m_partHidden.size() && m_partHidden[i];   // collision proxy → never show
        m_view->setPartVisible(i, !hidden && checked.value(i, true) && !(isFx && !showFx) && !(isSim && !showSim));
    }
    m_view->update();
}

QList<int> StableTab2::primitivesOf(QTreeWidgetItem* it) const
{
    QList<int> out;
    if (!it) return out;
    const int prim = it->data(0, Qt::UserRole).toInt();
    if (prim >= 0) out << prim;
    else for (int c = 0; c < it->childCount(); ++c) out += primitivesOf(it->child(c));
    return out;
}

// The parts-tree row for a merged primitive index, or null. Mirrors the Wardrobe's — it was a
// lambda local to showPartContextMenu until the viewport's click-select needed the same lookup.
void StableTab2::syncPartSelection()
{
    // selectedParts() walks the whole tree, so it is called ONCE and reused — it was being called
    // twice here for the two consumers.
    const QList<int> sel = selectedParts();
    if (m_view) m_view->setHighlightParts(sel);
    updateTexTiles(sel.isEmpty() ? -1 : sel.first());   // fill the TEXTURE PREVIEW tiles
}

QTreeWidgetItem* StableTab2::itemForPart(int part) const
{
    if (!m_partTree || part < 0) return nullptr;
    for (int r = 0; r < m_partTree->topLevelItemCount(); ++r) {
        QTreeWidgetItem* root = m_partTree->topLevelItem(r);
        for (int c = 0; c < root->childCount(); ++c)
            if (root->child(c)->data(0, Qt::UserRole).toInt() == part) return root->child(c);
    }
    return nullptr;
}

QList<int> StableTab2::selectedParts() const
{
    QList<int> out;
    if (m_partTree)
        for (QTreeWidgetItem* it : m_partTree->selectedItems()) out += primitivesOf(it);
    return out;
}

bool StableTab2::eventFilter(QObject* obj, QEvent* ev)
{
    const QEvent::Type t = ev->type();
    // Wheel over the shading "⌄" cycles the material channel live (wardrobe parity).
    if (m_shadeMoreBtn && obj == m_shadeMoreBtn && t == QEvent::Wheel && m_channelCombo) {
        const int dir = static_cast<QWheelEvent*>(ev)->angleDelta().y() > 0 ? -1 : 1;
        const int n = m_channelCombo->count();
        // Clamped, not wrapped — see ModelsTab: scrolling up must stop at "Shaded", not wrap to the
        // bottom of the list.
        if (n > 0) m_channelCombo->setCurrentIndex(qBound(0, m_channelCombo->currentIndex() + dir, n - 1));
        return true;
    }
    if (m_view && obj == m_view && (t == QEvent::Resize || t == QEvent::Show)) {
        positionVpStrip();   // keep the N-strip pinned to the viewport's right edge
        if (t == QEvent::Show && !m_thumbQueue.isEmpty() && m_thumbTimer && !m_thumbTimer->isActive())
            m_thumbTimer->start();   // GL is initialized now → render any deferred thumbnails
    }
    // Viewport Esc → clear the part selection (fullscreen-exit Esc is the m_fsEsc shortcut,
    // active only while maximized, so the two never fight).
    if (m_view && obj == m_view && t == QEvent::KeyPress && !m_fullscreen
        && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_Escape) {
        if (m_partTree) m_partTree->clearSelection();
        m_view->setHighlightParts({});
        return true;
    }
    // Alt+H is also the menubar's &Help mnemonic — claim the key back for the hide hotkey.
    if ((obj == m_view || obj == m_partTree) && t == QEvent::ShortcutOverride
        && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_H) { ev->accept(); return true; }
    // Part-visibility hotkeys (viewport or PARTS tree): H hide · Shift+H solo · Alt+H show all.
    if ((obj == m_view || obj == m_partTree) && t == QEvent::KeyPress && m_partTree
        && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_H) {
        const auto* ke = static_cast<QKeyEvent*>(ev);
        const QList<int> sel = selectedParts();
        const bool alt = ke->modifiers() & Qt::AltModifier, shift = ke->modifiers() & Qt::ShiftModifier;
        if (!alt && sel.isEmpty()) return BrowserTab::eventFilter(obj, ev);   // H with nothing selected
        const bool was = m_partTree->blockSignals(true);
        for (int r = 0; r < m_partTree->topLevelItemCount(); ++r) {
            QTreeWidgetItem* root = m_partTree->topLevelItem(r);
            for (int c = 0; c < root->childCount(); ++c) {
                QTreeWidgetItem* it = root->child(c);
                const int prim = it->data(0, Qt::UserRole).toInt();
                if (alt)                     it->setCheckState(0, Qt::Checked);
                else if (shift)              it->setCheckState(0, sel.contains(prim) ? Qt::Checked : Qt::Unchecked);
                else if (sel.contains(prim)) it->setCheckState(0, Qt::Unchecked);
            }
        }
        m_partTree->blockSignals(was);
        recomputePartVisibility();
        return true;
    }
    if (m_gridScroll && obj == m_gridScroll->viewport() && t == QEvent::Resize) {
        int cols = 0, cw = 0, ch = 0, iw = 0;
        cardMetrics(m_gridScroll->viewport()->width(), cols, cw, ch, iw);
        if (cols != m_gridCols && m_gridReflow) m_gridReflow->start();   // only when the layout changes
    }
    if (m_partTree && obj == m_partTree && t == QEvent::KeyPress
        && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_Escape) {
        m_partTree->clearSelection();
        return true;
    }
    if (m_partTree && obj == m_partTree->viewport()) {
        if (t == QEvent::Leave) {
            if (m_view) m_view->setHighlightParts(selectedParts());
        } else if (t == QEvent::MouseButtonPress) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            const QPoint p = me->position().toPoint();
            QTreeWidgetItem* it = m_partTree->itemAt(p);
            if (!it) m_partTree->clearSelection();
            else if (me->button() == Qt::LeftButton && me->modifiers() == Qt::NoModifier && it->isSelected()) {
                const QRect r = m_partTree->visualItemRect(it);
                if (p.x() > r.left() + 24) { it->setSelected(false); return true; }
            }
        }
    }
    return BrowserTab::eventFilter(obj, ev);
}

// (Saved "Stables" loadouts were removed here — a browser does not need saved loadouts.
//  The disabled implementation lived in this file until it was deleted; recover it from
//  git history rather than rewriting it if the Stable tab ever wants ensembles again.)

// ── Animations ──────────────────────────────────────────────────────────────────
void StableTab2::buildAnimPanel()
{
    m_animPanel = new QWidget;
    // PanelBox reads the CONTENT's own vertical policy to decide whether to let it fill the panel
    // or wrap it in a scroll area under a trailing stretch. A bare QWidget is Preferred, and a
    // widget does not inherit Expanding from its children — without this the clip list would stop
    // growing partway down a dragged panel and scroll inside a scroll area. WardrobeTab2 sets the
    // same policy on its equivalent for the same reason.
    m_animPanel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* v = new QVBoxLayout(m_animPanel);
    v->setContentsMargins(0, 0, 0, 0); v->setSpacing(3);
    // No title label: the PanelBox header supplies "ANIMATIONS" once this is in the sidebar.

    // Parentless — the caller adds it under the viewport, not into this panel.
    m_timeline = new QWidget;
    auto* tl = new QHBoxLayout(m_timeline);
    tl->setContentsMargins(0, 0, 0, 0); tl->setSpacing(4);
    m_playBtn = new QPushButton(QStringLiteral("Play"));
    m_animSlider = new QSlider(Qt::Horizontal);
    m_speedCombo = new QComboBox;
    m_speedCombo->addItems({ QStringLiteral("0.25x"), QStringLiteral("0.5x"), QStringLiteral("1x"), QStringLiteral("2x") });
    m_speedCombo->setCurrentText(QStringLiteral("1x"));
    m_loopCheck = new QCheckBox(QStringLiteral("Loop"));
    m_loopCheck->setChecked(true);
    tl->addWidget(m_playBtn); tl->addWidget(m_animSlider, 1); tl->addWidget(m_speedCombo); tl->addWidget(m_loopCheck);
    m_timeline->setVisible(false);

    m_animSearch = new QLineEdit;
    m_animSearch->setPlaceholderText(QStringLiteral("Filter animations…"));
    m_animSearch->setClearButtonEnabled(true);
    v->addWidget(m_animSearch);
    m_anims = new QListWidget;
    // Extended selection, as in the Models and Wardrobe animation panels. Left at the default
    // SingleSelection this list could never hand the animation-library export more than one clip,
    // and fillAnimList's setCurrentItem always leaves exactly one row selected — so the export
    // would have silently shipped the playing clip and called it a library.
    m_anims->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // Minimum only. A MAXIMUM height on PanelBox content is the one thing its header tells you
    // never to do: inside a splitter it turns extra dragged height into dead space.
    m_anims->setMinimumHeight(220);
    v->addWidget(m_anims, 1);
    // Play + Copy file name, then the shared Copy/Copy all. This list could only be played by
    // LEFT-clicking a row (:3142) while its context menu offered copy alone — the Models and
    // Wardrobe animation panels both expose the action their tab actually performs, and this one
    // did not. Installed before installCopyMenu so the copy pair stays at the bottom, matching the
    // ordering everywhere else.
    m_anims->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_anims, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        QListWidgetItem* hit = m_anims->itemAt(p);
        QMenu menu(this);
        if (hit) {
            const QString clip = hit->data(Qt::UserRole).toString();
            if (!clip.isEmpty()) {
                menu.addAction(QStringLiteral("Play"), this, [this, clip] { playAnimByName(clip); });
                menu.addSeparator();
                menu.addAction(QStringLiteral("Copy file name  (%1)")
                                   .arg(clip.size() > 30 ? clip.left(29) + QChar(0x2026) : clip),
                               this, [clip] { QGuiApplication::clipboard()->setText(clip); });
            }
        }
        if (!menu.isEmpty()) menu.addSeparator();
        menu.addAction(QStringLiteral("Copy all"), this, [this] {
            QStringList all;
            for (int i = 0; i < m_anims->count(); ++i) all << m_anims->item(i)->text().trimmed();
            QGuiApplication::clipboard()->setText(all.join(QLatin1Char('\n')));
        });
        menu.exec(m_anims->viewport()->mapToGlobal(p));
    });

    // NOT an animation control despite living next to them: it resets the whole mount selection
    // (base horse, no barding, no trophy) and only then the clip. It stays in the always-visible
    // left picker column — the caller adds it — because the ANIMATIONS panel can now be toggled
    // off, and this is the only way back to a known-good mount.
    m_resetBtn = new QPushButton(QStringLiteral("Reset to default"));
    m_resetBtn->setToolTip(QStringLiteral("Reset to the base horse mount with no armor or trophy, "
                                          "playing the default idle (1× speed, looping)."));
    connect(m_resetBtn, &QPushButton::clicked, this, [this] { resetAnimToDefault(); });

    m_animTimer = new QTimer(this);
    connect(m_animTimer, &QTimer::timeout, this, &StableTab2::tickAnimation);
    connect(m_anims, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        if (it) playAnimByName(it->data(Qt::UserRole).toString());
    });
    connect(m_playBtn, &QPushButton::clicked, this, [this] {
        if (!m_view) return;
        if (m_animTimer->isActive()) { m_animTimer->stop(); m_playBtn->setText(QStringLiteral("Play")); }
        else if (m_view->animFrameCount() > 0) { m_animTimer->start(); m_playBtn->setText(QStringLiteral("Pause")); }
    });
    connect(m_animSlider, &QSlider::valueChanged, this, [this](int f) { if (m_view) m_view->setFrame(f); });
    connect(m_speedCombo, &QComboBox::currentIndexChanged, this, [this](int) { applyAnimSpeed(); });
    connect(m_animSearch, &QLineEdit::textChanged, this, [this] { fillAnimList(); });
}

// Species token used to narrow the Anim/ scan: a pet's own name prefix, else the mount's species
// (the authoritative eMountType token when the caller has one, otherwise the name's trailing token).
QString StableTab2::animTokenFor(const QString& appr, bool pet, const QString& catHint)
{
    if (!pet) return catHint.isEmpty() ? catOf(appr.toLower()) : catHint;

    // A pet's token is its SPECIES — the LAST segment of cmp_<variant>_<species>, not the first
    // two. Measured against CoreTOC (48 companion appearances, 45,681 clips): the game names pet
    // clips two ways, and the old first-two-segments token could only ever see one of them.
    //
    //   stem-named     cmp_stor100_murloc_idle      belongs to that one appearance   (28 of 48)
    //   species-named  CMP_dogLarge_nav_idle        shared by every dogLarge variant (the rest)
    //
    // The species-named family carries no variant at all, so `cmp_stor105` matched none of it and
    // the filename filter in clipBuckets threw every one of those clips away before ownership was
    // ever consulted. 13 of 48 pets could not see a single one of their clips; 15 saw only some.
    // The species token sees 100% of both families for all 48, with nothing extra.
    const QString lower = appr.toLower();
    // catOf, as the mount side uses, so a future cmp_base000_dogLarge2 still resolves to
    // "doglarge" instead of missing the whole CMP_dogLarge_* family — which is this very bug,
    // reintroduced by variant numbering. isSpeciesTok is the existing test for "is this a real
    // species token, not a structural segment"; it rejects base/amor/armor/trophy/mnt and anything
    // under three characters or starting with a digit. Falling back to the WHOLE name is safe: it
    // is long and specific, so clipBuckets' filter collapses to mnt/mount only and finds nothing
    // rather than everything.
    const QString last = catOf(lower);
    return isSpeciesTok(last) ? last : lower;
}

// Scan Anim/*.ani.json once (narrowed by the species/mount token) and bucket every clip under the
// appearance that OWNS it. Rows are "<name>  ·  <frames> frames", sorted within each bucket.
//
// Bucketing FIRST and choosing the carrier after is the whole point. The previous code asked
// "which clips belong to mnt_base00_<species>", which silently returns nothing for a species that
// does not use that name — Basilisks own their clips through mnt_stor001_chimera and there is no
// mnt_base00_chimera in the data at all. A scan that answers "who owns clips here" cannot fail
// that way, and costs one directory walk per token instead of one per carrier guess.
// Does this clip row NAME the token as a whole segment? A bare substring test is unsafe here and
// the reason is structural: clipBuckets admits any file carrying "mnt"/"mount" regardless of token,
// so the bucket set for EVERY token — every pet species included — already holds the entire mount
// corpus. Species tokens are short, so unanchored matching hands "deer" every reindeer clip, "fox"
// every foxglove, "bird" every blackbird. Segment boundaries are '_' or the ends of the name.
bool rowNamesToken(const QString& row, const QString& tok)
{
    if (tok.isEmpty()) return false;
    const QString name = row.section(QStringLiteral("  ·  "), 0, 0).toLower();
    for (int from = 0;;) {
        const int i = name.indexOf(tok, from);
        if (i < 0) return false;
        const int end = i + tok.size();
        const bool leftOk  = (i == 0)            || name.at(i - 1) == QLatin1Char('_');
        const bool rightOk = (end == name.size()) || name.at(end)  == QLatin1Char('_');
        if (leftOk && rightOk) return true;
        from = i + 1;
    }
}

// Signature for the clip cache: the d4data build stamp plus the Anim directory's own mtime.
// Deliberately NOT a file count — counting IS the expensive operation this cache exists to avoid,
// so a count-based signature would pay the very cost it removes. Both inputs are one stat, and both
// move when the snapshot updates (git rewrites the tree).
QString StableTab2::animCacheSig()
{
    const QString d4 = Config::d4dataDir();
    const QString bvPath = d4 + QStringLiteral("/buildVersion.txt");
    QString bv;
    QFile bf(bvPath);
    if (bf.open(QIODevice::ReadOnly | QIODevice::Text)) bv = QString::fromUtf8(bf.readAll()).trimmed();
    // buildVersion.txt's MTIME as well as its contents: a directory's mtime moves when entries are
    // added, removed or renamed, but NOT when an existing .ani.json is rewritten in place. A
    // re-dump that truncates in place and leaves the stamp alone would otherwise be invisible, and
    // nothing else deletes this cache (MainWindow's fingerprint sweep covers stable_index_v*.bin
    // and tex_info_v*.bin only), so the signature is the only defence. One extra stat.
    return QStringLiteral("%1|%2|%3").arg(bv)
               .arg(QFileInfo(d4 + QStringLiteral("/json/base/meta/Anim")).lastModified().toMSecsSinceEpoch())
               .arg(QFileInfo(bvPath).lastModified().toMSecsSinceEpoch());
}

// Fill m_clipTok from disk, once per session. The scan this replaces walks 45,549 files, and it ran
// once per SPECIES TOKEN rather than once per session — browsing a horse, a cat, a Basilisk and two
// pets was five full walks on the GUI thread, each a hard freeze, repeated every launch. Rows are
// short strings, so every token together is a couple of hundred KB.
void StableTab2::loadClipCache()
{
    if (m_clipDiskLoaded) return;
    m_clipDiskLoaded = true;
    QFile cf(AppPaths::file(QStringLiteral("stable_anims_v2.json")));
    if (!cf.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(cf.readAll()).object();
    if (root.value(QStringLiteral("sig")).toString() != animCacheSig()) return;
    const QJsonObject toks = root.value(QStringLiteral("tokens")).toObject();
    for (auto t = toks.constBegin(); t != toks.constEnd(); ++t) {
        QHash<int, QStringList> buckets;
        const QJsonObject owners = t.value().toObject();
        for (auto o = owners.constBegin(); o != owners.constEnd(); ++o) {
            const int sno = o.key().toInt();
            if (sno <= 0) continue;               // a key that is not a sno is a corrupt file
            QStringList rows;
            for (const QJsonValue& v : o.value().toArray()) rows << v.toString();
            if (!rows.isEmpty()) buckets.insert(sno, rows);
        }
        if (!buckets.isEmpty()) m_clipTok.insert(t.key(), buckets);
    }
    if (!m_clipTok.isEmpty())
        qInfo("stable anims: %d token(s) from disk cache — Anim folder not scanned",
              int(m_clipTok.size()));
}

// Written after each newly scanned token rather than at shutdown: the whole point is that a crash
// or a kill mid-session must not cost the walk again, and the file is small enough that rewriting
// it a handful of times per session is free.
void StableTab2::saveClipCache()
{
    QJsonObject toks;
    for (auto t = m_clipTok.constBegin(); t != m_clipTok.constEnd(); ++t) {
        QJsonObject owners;
        for (auto o = t.value().constBegin(); o != t.value().constEnd(); ++o) {
            QJsonArray rows;
            for (const QString& r : o.value()) rows.append(r);
            owners.insert(QString::number(o.key()), rows);
        }
        toks.insert(t.key(), owners);
    }
    QJsonObject root;
    root.insert(QStringLiteral("sig"), animCacheSig());
    root.insert(QStringLiteral("tokens"), toks);
    // Temp + rename, and mkpath first — the same standard stable_index_v6.bin now holds. Writing
    // in place would destroy the previous good cache on a crash mid-write (a torn file always
    // fails the signature check, so it is never mis-loaded — but it is needlessly lost), and
    // AppPaths::file() does not create the directory.
    const QString path = AppPaths::file(QStringLiteral("stable_anims_v2.json"));
    QDir().mkpath(AppPaths::dataDir());
    const QString tmp = path + QStringLiteral(".tmp");
    QFile wf(tmp);
    if (!wf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    const QByteArray blob = QJsonDocument(root).toJson(QJsonDocument::Compact);
    const bool ok = wf.write(blob) == blob.size();
    wf.close();
    if (ok) { QFile::remove(path); QFile::rename(tmp, path); }
    else    { QFile::remove(tmp); }
}

const QHash<int, QStringList>& StableTab2::clipBuckets(const QString& tok)
{
    // No token means the species is unknown. Fail closed rather than walking the whole Anim tree
    // for a filter that cannot narrow anything: an unknown family must expand NOTHING.
    static const QHash<int, QStringList> kNone;
    if (tok.isEmpty()) return kNone;
    loadClipCache();   // once per session; the lookup below then usually hits
    const auto cached = m_clipTok.constFind(tok);
    if (cached != m_clipTok.constEnd()) return cached.value();

    QHash<int, QStringList> buckets;
    const QString d4 = Config::d4dataDir();
    static const QRegularExpression rxApp(
        QStringLiteral("\"snoAppearance\":\\s*\\{[^{}]*?\"__raw__\":\\s*(\\d+)"));
    static const QRegularExpression rxFrames(QStringLiteral("\"nKeyframeCount\":\\s*(\\d+)"));
    QDirIterator di(d4 + QStringLiteral("/json/base/meta/Anim"),
                    QStringList{ QStringLiteral("*.ani.json") }, QDir::Files);
    while (di.hasNext()) {
        const QString fp = di.next();
        const QString base = di.fileName();
        const QString low = base.toLower();
        // tok is never empty here — clipBuckets returns kNone for an empty token before this loop.
        if (!(low.contains(QLatin1String("mnt")) || low.contains(QLatin1String("mount"))
              || low.contains(tok))) continue;
        QFile jf(fp);
        if (!jf.open(QIODevice::ReadOnly)) continue;
        const QString raw = QString::fromUtf8(jf.readAll());
        const auto m = rxApp.match(raw);
        if (!m.hasMatch()) continue;
        const int owner = m.captured(1).toInt();
        if (owner <= 0) continue;
        const QString nm = base.left(base.size() - 9);   // strip ".ani.json"
        const auto fm = rxFrames.match(raw);
        // operator[] inserting a default is intended here — a first clip creates its owner's bucket.
        buckets[owner] << (fm.hasMatch()
                               ? QStringLiteral("%1  ·  %2 frames").arg(nm, fm.captured(1)) : nm);
    }
    for (auto b = buckets.begin(); b != buckets.end(); ++b) b.value().sort();
    const QHash<int, QStringList>& out = *m_clipTok.insert(tok, buckets);
    saveClipCache();   // reads m_clipTok only — never inserts, so `out` stays valid
    return out;
}

// The appearance SNO that owns this item's clips — resolved in stages for both families. For a mount it is the
// species' clip-owning base, chosen by MEASUREMENT rather than by name: the conventional
// mnt_base00_<species> wins when it actually owns clips, then any mnt_base* of that species, and
// failing both, whichever appearance of the species owns the most clips. A name that owns nothing
// is not a carrier however it is spelled, which is what makes this survive the next species.
int StableTab2::animCarrierFor(const QString& appr, int apprSno, bool pet, const QString& catHint)
{
    const QString tok = animTokenFor(appr, pet, catHint);
    // Both guards come BEFORE clipBuckets(): that call walks ~45k Anim files, and with no index
    // every branch below is unreachable, so the old pure-lookup early-outs have to stay early.
    if (tok.isEmpty() || !m_index) return apprSno;
    const QHash<int, QStringList>& buckets = clipBuckets(tok);
    if (buckets.isEmpty()) return apprSno;

    // ── Stage 1: the family's conventional owner, IF it actually owns clips ──────────────────────
    // The two families differ here, and not arbitrarily — it is what the shipped data does.
    //
    //   PETS   author clips per APPEARANCE (cmp_stor100_murloc_idle), so the pet's own appearance
    //          is the right first answer: 28 of 48 companions ship a full set named after their
    //          own stem.
    //   MOUNTS do the opposite. Every horse skin plays the base rig's clips, and what a skin owns
    //          in its OWN name is a handful of FX extras — mnt_stor052_horse_wings_flap,
    //          mnt_amor124_cat_stor_wings_idle. Preferring "its own" for a mount would hand the
    //          user one wing-flap in place of thirty gaits, so the base rig is asked first.
    if (pet) {
        if (!buckets.value(apprSno).isEmpty()) return apprSno;
    } else {
        int exact = 0, anyBase = 0;
        const QString want = QStringLiteral("mnt_base00_") + tok;
        for (const SnoEntry& e : m_index->entries(kGroupAppearance)) {
            const QString lower = e.name.toLower();
            if (lower == want) { exact = e.snoId; if (anyBase) break; continue; }
            if (!anyBase && lower.startsWith(QLatin1String("mnt_base")) && catOf(lower) == tok) {
                anyBase = e.snoId;
                if (exact) break;
            }
        }
        if (exact   > 0 && !buckets.value(exact).isEmpty())   return exact;     // the convention, verified
        if (anyBase > 0 && !buckets.value(anyBase).isEmpty()) return anyBase;   // any base of the species
    }

    // ── Stage 2: the family's base rig for a pet whose own appearance owns nothing ───────────────
    if (pet) {
        for (const SnoEntry& e : m_index->entries(kGroupAppearance)) {
            const QString lower = e.name.toLower();
            if (!lower.startsWith(QLatin1String("cmp_base"))) continue;
            if (catOf(lower) != tok) continue;   // same derivation animTokenFor uses
            if (!buckets.value(e.snoId).isEmpty()) return e.snoId;
        }
    }

    // ── Stage 3: measured — the appearance whose clips are MOSTLY about this species ─────────────
    // The owner's own name is deliberately not consulted: nameForSno returns an EMPTY string for a
    // record whose name is encrypted, so a name test silently skips exactly the owners that are
    // hardest to reach any other way. The clips themselves say who they belong to.
    //
    // Two gates, because "owns the most clips mentioning the token" on its own is far too weak.
    // The bucket set holds the whole mount corpus (see rowNamesToken), so without them a pet whose
    // species genuinely owns nothing gets handed some NPC or merc rig, and a populated-but-wrong
    // list is worse than an empty one: it decodes into the .glb as silently wrong animation, where
    // an empty one shows the list's honest "(no clips)" row. Fail closed, as everywhere else here.
    //
    //   · the token must appear as a whole SEGMENT of the clip name, not as a substring
    //   · those clips must be the MAJORITY of what that owner owns — a rig with 400 clips of which
    //     12 mention your species is not your carrier; one with 20 of which 20 do, is
    int best = 0, bestN = 0, bestTotal = 0;
    for (auto b = buckets.constBegin(); b != buckets.constEnd(); ++b) {
        const int total = int(b.value().size());
        int n = 0;
        for (const QString& row : b.value())
            if (rowNamesToken(row, tok)) ++n;
        if (n == 0 || n * 2 < total) continue;          // not predominantly this species
        // Deterministic tiebreak. QHash iteration order is randomised per process, so first-seen
        // wins would make the carrier — and therefore the exported clip set — differ between
        // launches of the same build on the same data.
        if (n > bestN || (n == bestN && (total > bestTotal
                                         || (total == bestTotal && b.key() < best)))) {
            best = b.key(); bestN = n; bestTotal = total;
        }
    }
    return best > 0 ? best : apprSno;
}

int StableTab2::animCarrierSno()
{
    const int sno = m_slotSel[SlotMount];
    if (sno <= 0) return sno;
    const bool pet = petMode();
    return animCarrierFor(m_slotName[SlotMount], sno, pet, pet ? QString() : mountCategory());
}

// Clip rows for the live selection — the ONE row source. Preview list, auto-play, the export menu
// count and the export itself all ask this, so the set the user is looking at is the set that ships.
QStringList StableTab2::currentClipRows()
{
    const int sno = m_slotSel[SlotMount];
    // No rig means no clip can play, and resolving a carrier walks ~45k Anim files — so the guard
    // belongs HERE, at the one place every consumer goes through, not in each caller.
    if (sno <= 0 || m_lastGeo.skeleton.isEmpty()) return QStringList();
    const bool pet = petMode();
    const QString tok = animTokenFor(m_slotName[SlotMount], pet, pet ? QString() : mountCategory());
    // Resolve FIRST, then take the bucket reference. animCarrierSno() calls clipBuckets() itself,
    // and an insert there can rehash m_clipTok — a reference taken before it would dangle.
    const int carrier = animCarrierSno();
    if (!m_index) return QStringList();   // as in animCarrierFor: no index, no scan
    return clipBuckets(tok).value(carrier);
}

// The species hint to resolve `appr` with: the authoritative eMountType token when `appr` is the
// mount currently selected (the panel's own answer), empty otherwise so it falls back to the name.
QString StableTab2::hintForAppr(const QString& appr, bool pet) const
{
    // petMode() as well as the caller's flag: the two are derived differently (item scan vs name
    // heuristic) and can disagree, and mountCategory() answers "pet" for a companion — feeding
    // that to the mount path would send it hunting for mnt_base00_pet.
    if (pet || petMode() || appr.isEmpty()) return QString();
    return appr.compare(m_slotName[SlotMount], Qt::CaseInsensitive) == 0 ? mountCategory() : QString();
}

// The clips an export should carry for `appr`, per Settings ▸ Export scope. Mount and pet clips are
// discovered off the CARRIER, which IS the animated rig, so `original`, `sets` and `base` all name
// the same set here — any of them means "every clip this rig owns". Otherwise it is the clip
// playing in preview, falling back to the rig's nav-idle.
QStringList StableTab2::exportClipNames(const QString& appr, int apprSno, bool pet)
{
    // When `appr` IS the live selection, use the same authoritative eMountType hint the ANIMATIONS
    // panel uses. mountCategory() and catOf(name) can disagree, and resolving the export against a
    // different token than the panel reintroduces the exact "panel lists N, export writes 0" split
    // this whole change exists to remove.
    const QString hint = hintForAppr(appr, pet);
    const QString tok = animTokenFor(appr, pet, hint);
    const int carrier = animCarrierFor(appr, apprSno, pet, hint);
    if (carrier <= 0 || !m_index) return QStringList();   // as in animCarrierFor: no index, no scan
    const QStringList rows = clipBuckets(tok).value(carrier);
    auto clipOf = [](const QString& r) { return r.section(QStringLiteral("  ·  "), 0, 0); };
    QStringList want;
    const AnimExportScope asc = AnimExportScope::load();
    if (asc.original || asc.sets || asc.base) {
        for (const QString& r : rows) want << clipOf(r);
        return want;
    }
    QString c = appr.compare(m_slotName[SlotMount], Qt::CaseInsensitive) == 0 ? m_playingAnim : QString();
    if (c.isEmpty())
        for (const QString& r : rows)
            if (clipOf(r).toLower().contains(QLatin1String("nav_idle"))) { c = clipOf(r); break; }
    if (!c.isEmpty()) want << c;
    return want;
}

// Decode named clips against `geo`'s rest pose. The ONE decoder every Stable export path uses, so
// every path ships the same set for the same settings. Unreadable, empty-payload and zero-frame
// clips are skipped, so the menu's count is an UPPER BOUND on what lands in the file, not a
// promise. Each decode stays inside an SEH guard because a malformed payload faults rather than
// returning an error.
void StableTab2::collectExportAnims(const ModelGeometry& geo, const QStringList& clipNames,
                                    QVector<AnimParser::DecodedAnim>& anims, QStringList& names)
{
    if (geo.skeleton.isEmpty() || clipNames.isEmpty() || !m_reader) return;
    const QString d4 = Config::d4dataDir();
    QHash<quint32, AnimParser::RestTRS> rest;
    for (const ModelJoint& j : geo.skeleton) {
        AnimParser::RestTRS t; t.q = j.restQ; t.t = j.restT; t.s = j.restS;
        rest.insert(j.nameHash, t);
    }
    for (const QString& nm : clipNames) {
        QFile jf(d4 + QStringLiteral("/json/base/meta/Anim/") + nm + QStringLiteral(".ani.json"));
        if (!jf.open(QIODevice::ReadOnly)) continue;
        const QJsonObject root = QJsonDocument::fromJson(jf.readAll()).object();
        const int animSno = root.value(QStringLiteral("__snoID__")).toInt();
        const QJsonArray perms = root.value(QStringLiteral("ptPermutations")).toArray();
        if (animSno <= 0 || perms.isEmpty()) continue;
        const QJsonObject perm = perms.first().toObject();
        const int offset = perm.value(QStringLiteral("ptPayloadData")).toObject()
                               .value(QStringLiteral("value")).toObject()
                               .value(QStringLiteral("dataOffset")).toInt();
        const int frames = perm.value(QStringLiteral("nKeyframeCount")).toInt();
        const int comp = perm.value(QStringLiteral("flCompression")).toInt();
        const float fps = float(perm.value(QStringLiteral("flFrameRate")).toDouble(30.0));
        if (frames <= 0) continue;
        const QByteArray ap = m_reader->readPayloadBySno(quint64(animSno));
        if (ap.isEmpty()) continue;
        AnimParser::DecodedAnim a;
        const bool okA = seh::runGuarded("stableExportAnim",
                                         [&]() { a = AnimParser::decode(ap, offset, frames, comp, fps, rest); });
        if (okA && a.valid) { anims << a; names << nm; }
    }
}

// Export-menu hook: the anim-library export is offered as the menu's contextual "anim export"
// action, enabled once a mount is assembled.
bool StableTab2::hasAnimExport() const { return hasExportSelection() && !m_lastGeo.skeleton.isEmpty(); }

// Says which clip set the action will write, so the menu cannot promise a "library" and deliver
// one clip. Reports the SELECTION when there is a real one, else names the configured scope —
// which is exactly what exportAnimLibrary will use. Deliberately performs no Anim/ scan; it is not
// otherwise side-effect free, since AnimExportScope::load() may run its one-shot settings
// migration, and this is called every time the Export menu opens.
QString StableTab2::animExportLabel() const
{
    const int n = m_anims ? int(m_anims->selectedItems().size()) : 0;
    if (n > 1)
        return QStringLiteral("Export animation library (%1 selected clips, .glb)…").arg(n);
    const AnimExportScope asc = AnimExportScope::load();
    return (asc.original || asc.sets || asc.base)
               ? QStringLiteral("Export animation library (all clips, .glb)…")
               : QStringLiteral("Export animation library (playing clip, .glb)…");
}
void StableTab2::exportAnimations()    { exportAnimLibrary(); }

// Export the RIG + clips only (no mesh) — a clip library to append onto an already-imported mount
// in Blender. The clip set is whatever the ANIMATIONS list has selected; with no selection it falls
// back to the Settings ▸ Export scope, so the action does something sensible either way.
void StableTab2::exportAnimLibrary()
{
    if (m_lastGeo.skeleton.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Animation library"),
            QStringLiteral("Assemble a mount first (the rig comes from the equipped mount)."));
        return;
    }
    // Only an explicit MULTI-selection overrides the Settings ▸ Export scope. fillAnimList always
    // leaves the playing row selected, so treating one selected row as a deliberate choice would
    // make this export ship a single clip and call it a library — the fallback below would never
    // run at all.
    QStringList want;
    if (m_anims && m_anims->selectedItems().size() > 1)
        for (QListWidgetItem* it : m_anims->selectedItems()) {
            // The "(no clips found)" placeholder carries no UserRole.
            const QString nm = it->data(Qt::UserRole).toString();
            if (!nm.isEmpty()) want << nm;
        }
    if (want.isEmpty())
        want = exportClipNames(m_slotName[SlotMount], m_slotSel[SlotMount], petMode());

    QVector<AnimParser::DecodedAnim> anims; QStringList names;
    collectExportAnims(m_lastGeo, want, anims, names);
    if (anims.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Animation library"),
            QStringLiteral("No animations to export. Ctrl-select TWO or more clips in the "
                           "ANIMATIONS list to choose exactly those, or play one and let the "
                           "Settings ▸ Export scope decide."));
        return;
    }

    QString base = m_slotName[SlotMount];
    if (base.isEmpty()) base = QStringLiteral("mount");
    const QString dir = QSettings().value(QStringLiteral("stable2/exportDir"), QDir::homePath()).toString();
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export animation library"),
                       dir + QStringLiteral("/") + base + QStringLiteral("_anims.glb"),
                       QStringLiteral("glTF Binary (*.glb)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".glb"), Qt::CaseInsensitive)) path += QStringLiteral(".glb");

    ModelGeometry geo;                    // rig only — no primitives
    geo.valid = true;
    geo.skeleton = m_lastGeo.skeleton;
    geo.nBaseBones = m_lastGeo.nBaseBones;
    const ModelExporter::Options opt = ModelExporter::optionsFromSettings();
    if (QSettings().value(QStringLiteral("export/hardpointEmpties"), false).toBool())
        Hardpoints::readInto(geo, apprJsonPath(Config::d4dataDir(), m_slotName[SlotMount]));
    // No retarget on a clip library: remapping or collapsing bones would drop the very bones the
    // clips drive, which is the one thing this export exists to preserve. Renaming is still fine —
    // it does not change the bone SET — and the re-index after it is what keeps the hardpoint
    // empties pointing at the right bones.
    if (opt.blenderFriendly) GLModelWidget::blenderizeSkeletonNames(geo.skeleton);
    else if (QSettings().value(QStringLiteral("export/boneNamesTranslated"), false).toBool())
        GLModelWidget::translateSkeletonNames(geo.skeleton);
    Hardpoints::resolveBoneIndices(geo);
    const bool ok = ModelExporter::exportGlb(geo, path, {}, anims, names, opt);
    const QString folder = QFileInfo(path).absolutePath();
    QSettings().setValue(QStringLiteral("stable2/exportDir"), folder);
    if (ok)
        ExportNotifier::instance().notify(
            QStringLiteral("Exported %1 animation clip(s), rig only").arg(anims.size()), folder);
    else
        QMessageBox::warning(this, QStringLiteral("Animation library"), QStringLiteral("Export failed."));
}

// D4_DUMP_MNTTROPHYANIM=1 — one-shot: do mount TROPHY appearances own animation clips at all?
//
// This exists because the question gates a real feature and cannot be answered by inspection.
// Stable seats trophies with ModelAttach::seat, which bakes their verts and CLEARS their skeleton.
// That is deliberate: seatTrophyOnMount records that the rig-preserving path (attachSubRig) had
// positioning issues and was reverted, so trophies are static on purpose, not by omission. Giving
// a trophy its own clip means reopening that placement problem — worth doing only if trophies own
// clips to play. One run with the variable set answers it from the shipped data instead of a guess.
//
// Bounded (40 lines) and one-shot per session, but NOT free: "trophy" is its own cache key, never
// one of the species tokens the panel uses, so this forces an EXTRA full walk of json/base/meta/Anim
// and leaves a permanent bucket set behind. That is fine behind an env gate and is the reason it
// stays behind one — do not ungate it.
void StableTab2::dumpMountTrophyAnims()
{
    static bool done = false;
    if (done || !m_index || !qEnvironmentVariableIsSet("D4_DUMP_MNTTROPHYANIM")) return;
    done = true;
    // "trophy" widens the filename filter to mnt/mount clips PLUS anything else carrying the word,
    // so a trophy clip that breaks the mnt_ naming still lands in a bucket.
    // Second site to bind a clipBuckets reference (see the warning on its declaration): nothing in
    // the loop below may call clipBuckets, or this dangles the moment a new token inserts.
    const QHash<int, QStringList>& buckets = clipBuckets(QStringLiteral("trophy"));
    int total = 0, withClips = 0, shown = 0;
    for (const SnoEntry& e : m_index->entries(kGroupAppearance)) {
        const QString lower = e.name.toLower();
        if (!lower.startsWith(QLatin1String("mnt_")) || !lower.contains(QLatin1String("trophy")))
            continue;
        ++total;
        const QStringList rows = buckets.value(e.snoId);
        if (rows.isEmpty()) continue;
        ++withClips;
        if (shown++ < 40)
            qInfo("[mnttrophyanim] %s (sno %d): %d clip(s) — %s",
                  qPrintable(e.name), e.snoId, int(rows.size()),
                  qPrintable(rows.first().section(QStringLiteral("  ·  "), 0, 0)));
    }
    qInfo("[mnttrophyanim] %d of %d mount trophy appearances own clips%s",
          withClips, total,
          withClips == 0 ? "  — nothing to animate; leave trophies seated static" : "");
}

// D4_DUMP_PETANIM=1 — one-shot audit: does every companion resolve to a carrier that owns clips?
//
// The pet path was the mount bug in a second costume. Its token was the first two segments of the
// appearance name (cmp_stor105), which the game's own clip naming does not use for the shared
// family (CMP_dogLarge_nav_idle), so 13 of 48 companions could not see one of their clips and 15
// saw only part of the set. Measured against CoreTOC before the fix; this reports what the FIXED
// resolution actually finds on the user's own snapshot, which is the half a name study cannot
// answer — which appearance each clip says it belongs to.
//
// EXPENSIVE by construction, and more so than its trophy sibling: one Anim/ walk per distinct
// species token the first time it runs (~27 species on a cold cache), on the GUI thread, each one
// permanently added to stable_anims_v2.json — which saveClipCache rewrites once per token as it
// grows. A cold run is a multi-minute freeze. That is why it is env-gated and one-shot, why it
// says so in the log, and why it must not be ungated.
void StableTab2::dumpPetAnims()
{
    static bool done = false;
    if (done || !m_index || !qEnvironmentVariableIsSet("D4_DUMP_PETANIM")) return;
    done = true;
    qInfo("[petanim] auditing companion clip resolution — this walks Anim/ once per species, "
          "so the first run is slow by design");

    // The TAB's pets, not a name scan, when the item index is ready: a companion is found through
    // CompanionItem -> snoCompanion -> Actor -> Appearance, and nothing makes that appearance's
    // name start with cmp_. Auditing the name scan alone could report "48 of 48 fine" while a real
    // companion is broken. Falls back to the name scan only before the scan lands.
    QVector<QPair<int, QString>> petAppr;
    if (m_petReady && !m_petItems.isEmpty()) {
        for (const StableEntry& e : m_petItems)
            if (e.apprSno > 0) petAppr.append({ e.apprSno, e.appr });
    } else {
        for (const SnoEntry& e : m_index->entries(kGroupAppearance)) {
            const QString lower = e.name.toLower();
            if (lower.startsWith(QLatin1String("cmp_base")) || lower.startsWith(QLatin1String("cmp_stor")))
                petAppr.append({ e.snoId, e.name });
        }
        qInfo("[petanim] item scan not ready — auditing the cmp_* NAME scan instead, which is not "
              "necessarily the tab's pet set");
    }

    int withClips = 0, shown = 0;
    for (const auto& pa : petAppr) {
        const QString tok = animTokenFor(pa.second, /*pet=*/true, QString());
        const int carrier = animCarrierFor(pa.second, pa.first, /*pet=*/true, QString());
        // Fresh call, and the result is COPIED — never hold a clipBuckets reference across a loop
        // that calls clipBuckets again (see the warning on its declaration).
        const QStringList rows = clipBuckets(tok).value(carrier);
        if (!rows.isEmpty()) ++withClips;
        if (shown++ < 60)
            qInfo("[petanim] %-30s tok=%-16s carrier=%-8d clips=%-3d %s",
                  qPrintable(pa.second), qPrintable(tok), carrier, int(rows.size()),
                  carrier == pa.first ? "(own)" : "(shared rig)");
    }
    qInfo("[petanim] %d of %d companion appearance(s) resolve to a carrier that owns clips",
          withClips, int(petAppr.size()));
}

void StableTab2::populateAnims()
{
    dumpMountTrophyAnims();   // D4_DUMP_MNTTROPHYANIM=1 (one-shot, no cost otherwise)
    dumpPetAnims();           // D4_DUMP_PETANIM=1 (one-shot, no cost otherwise)
    if (!m_anims) return;
    // Deliberately does NOT touch m_animPanel's visibility. That belongs to the sidebar strip
    // toggle alone — a refresh path that also sets it would silently reopen a panel the user
    // closed, which is the same failure the overlay master gate exists to prevent. The panel is
    // permanently mounted now, so a rig with no clips falls THROUGH to fillAnimList and gets its
    // "(no clips)" row: returning early here would leave a titled, empty, unexplained box.
    // currentClipRows() skips the Anim/ scan when there is no rig, so this costs nothing.
    if (m_lastGeo.skeleton.isEmpty() && m_timeline) m_timeline->setVisible(false);
    fillAnimList();
}

// Human summary of what an export of `appr` will include, per Settings ▸ Export (parsed on open,
// as chosen): always "1 model", plus the real animation count (or the clip name, if scope = playing)
// when animations are enabled, plus the real raw-source file count (.app + distinct .tex) when raw
// export is enabled.
QString StableTab2::exportMenuSuffix(const QString& appr, bool pet)
{
    QSettings s;
    QStringList parts; parts << QStringLiteral("1 model");
    const QString d4 = Config::d4dataDir();
    if (s.value(QStringLiteral("export/includeAnim"), false).toBool()) {
        // Resolve the clip-owning carrier for this item through the SAME helper the preview list
        // and the exporter use — staged resolution for both families, never an assumption.
        // This had its own inline copy with no fallback, so the count shown in the menu could
        // disagree with what the export actually wrote.
        const int selfSno = m_index ? m_index->snoForName(kGroupAppearance, appr) : 0;
        const QString hint = hintForAppr(appr, pet);
        const QString tok = animTokenFor(appr, pet, hint);
        const int carrier = animCarrierFor(appr, selfSno, pet, hint);
        const QStringList clips = carrier > 0 ? clipBuckets(tok).value(carrier) : QStringList();
        const AnimExportScope asc = AnimExportScope::load();   // as in exportOne: carrier == the rig
        if (asc.original || asc.sets || asc.base) {
            parts << QStringLiteral("%1 animation%2").arg(clips.size()).arg(clips.size() == 1 ? QString() : QStringLiteral("s"));
        } else {
            QString clip = appr.compare(m_slotName[SlotMount], Qt::CaseInsensitive) == 0 ? m_playingAnim : QString();
            if (clip.isEmpty())
                for (const QString& r : clips)
                    if (r.section(QStringLiteral("  ·  "), 0, 0).toLower().contains(QLatin1String("nav_idle")))
                        { clip = r.section(QStringLiteral("  ·  "), 0, 0); break; }
            parts << (clip.isEmpty() ? QStringLiteral("playing clip") : QStringLiteral("clip: %1").arg(clip));
        }
    }
    if (s.value(QStringLiteral("export/withDeps"), false).toBool()) {
        QSet<qint64> tex;
        for (const QString& mn : MaterialDecode::appearanceRoster(d4, appr)) {
            if (mn.isEmpty()) continue;
            QFile mf(d4 + QStringLiteral("/json/base/meta/Material/") + mn + QStringLiteral(".mat.json"));
            if (!mf.open(QIODevice::ReadOnly)) continue;
            for (const MatTexture& mt : parseMaterialJson(mf.readAll())) if (mt.texSno) tex.insert(mt.texSno);
        }
        const int raw = 1 + tex.size();   // 1 .app + distinct .tex
        parts << QStringLiteral("%1 raw file%2").arg(raw).arg(raw == 1 ? QString() : QStringLiteral("s"));
    }
    return parts.join(QStringLiteral(" + "));
}

void StableTab2::fillAnimList()
{
    if (!m_anims) return;
    const QStringList rows = currentClipRows();
    const QString search = m_animSearch ? m_animSearch->text().trimmed().toLower() : QString();
    m_anims->blockSignals(true);
    m_anims->clear();
    for (const QString& r : rows) {
        if (!search.isEmpty() && !r.toLower().contains(search)) continue;
        const QString name = r.section(QStringLiteral("  ·  "), 0, 0);
        auto* it = new QListWidgetItem(r, m_anims);
        it->setData(Qt::UserRole, name);
        if (name == m_playingAnim) m_anims->setCurrentItem(it);
    }
    m_anims->blockSignals(false);
    if (rows.isEmpty()) m_anims->addItem(QStringLiteral("  (no clips found for this rig)"));
}

void StableTab2::playAnimByName(const QString& animName)
{
    if (animName.isEmpty() || !m_view || m_lastGeo.skeleton.isEmpty()
        || !m_reader || !m_reader->isReady())
        return;
    const QString d4 = Config::d4dataDir();
    QFile jf(QStringLiteral("%1/json/base/meta/Anim/%2.ani.json").arg(d4, animName));
    if (!jf.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(jf.readAll()).object();
    const int animSno = root.value(QStringLiteral("__snoID__")).toInt();
    const QJsonArray perms = root.value(QStringLiteral("ptPermutations")).toArray();
    if (animSno <= 0 || perms.isEmpty()) return;
    const QJsonObject perm = perms.first().toObject();
    const int offset = perm.value(QStringLiteral("ptPayloadData")).toObject()
                           .value(QStringLiteral("value")).toObject()
                           .value(QStringLiteral("dataOffset")).toInt();
    const int frames = perm.value(QStringLiteral("nKeyframeCount")).toInt();
    const int comp = perm.value(QStringLiteral("flCompression")).toInt();
    const float fps = float(perm.value(QStringLiteral("flFrameRate")).toDouble(30.0));
    if (frames <= 0) return;
    const QByteArray payload = m_reader->readPayloadBySno(quint64(animSno));
    if (payload.isEmpty()) return;

    QHash<quint32, AnimParser::RestTRS> rest;
    for (const ModelJoint& j : m_lastGeo.skeleton) {
        AnimParser::RestTRS t; t.q = j.restQ; t.t = j.restT; t.s = j.restS;
        rest.insert(j.nameHash, t);
    }
    AnimParser::DecodedAnim anim;
    const bool decoded = seh::runGuarded("stableAnimDecode",
        [&]() { anim = AnimParser::decode(payload, offset, frames, comp, fps, rest); });
    if (!decoded || !anim.valid) return;

    m_playingAnim = animName;
    m_curAnim = anim;
    const bool applied = seh::runGuarded("stableAnimApply",
        [&]() { m_view->setAnimation(anim); });
    if (!applied) return;
    m_timeline->setVisible(true);
    m_animSlider->blockSignals(true);
    m_animSlider->setRange(0, frames - 1);
    m_animSlider->setValue(0);
    m_animSlider->blockSignals(false);
    m_animFps = fps > 0 ? fps : 30.0f;
    applyAnimSpeed();
    m_animTimer->start();
    m_playBtn->setText(QStringLiteral("Pause"));
}

// "Reset to default": return to the default state — the base horse mount (mnt_base00_horse) with
// NO Mount Armor and NO Trophy — at 1× speed, looping, playing its default nav-idle clip.
void StableTab2::resetAnimToDefault()
{
    pushUndo();
    // Base horse mount appearance (the canonical default; also the mount that carries the clips).
    int baseSno = 0;
    if (m_index)
        for (const SnoEntry& e : m_index->entries(kGroupAppearance))
            if (e.name.compare(QLatin1String("mnt_base00_horse"), Qt::CaseInsensitive) == 0) { baseSno = e.snoId; break; }
    if (baseSno > 0) {
        m_slotSel[SlotMount] = baseSno;
        m_slotName[SlotMount] = QStringLiteral("mnt_base00_horse");
        m_slotDisp[SlotMount].clear(); m_slotDesc[SlotMount].clear();
        m_slotLook[SlotMount] = 0; m_mountType = 0;   // 0 = Horse
    }
    // Clear Mount Armor + Trophy.
    for (int s : { SlotBarding, SlotTrophy }) {
        m_slotSel[s] = 0; m_slotName[s].clear();
        m_slotDisp[s].clear(); m_slotDesc[s].clear(); m_slotLook[s] = 0;
    }
    if (m_speedCombo) m_speedCombo->setCurrentText(QStringLiteral("1x"));
    if (m_loopCheck)  m_loopCheck->setChecked(true);
    m_playingAnim.clear();   // force rebuildMount to pick the nav-idle (not keep the current clip)
    refreshSlotCells();
    fillGrid();
    rebuildMount();          // reassembles + auto-plays the default nav-idle
    saveCurrent();
}

void StableTab2::applyAnimSpeed()
{
    float mult = 1.0f;
    if (m_speedCombo) {
        const QString s = m_speedCombo->currentText();
        bool ok = false;
        const float vv = s.left(s.size() - 1).toFloat(&ok);
        if (ok && vv > 0.0f) mult = vv;
    }
    const float eff = m_animFps * mult;
    if (m_animTimer) m_animTimer->setInterval(eff > 0.0f ? int(1000.0f / eff) : 33);
}

void StableTab2::tickAnimation()
{
    const int fc = m_view ? m_view->animFrameCount() : 0;
    if (fc <= 0 || !m_animSlider) { if (m_animTimer) m_animTimer->stop(); return; }
    int next = m_animSlider->value() + 1;
    if (next >= fc) {
        if (m_loopCheck && m_loopCheck->isChecked()) next = 0;
        else { m_animTimer->stop(); m_playBtn->setText(QStringLiteral("Play")); return; }
    }
    m_animSlider->setValue(next);
}

void StableTab2::clearAnim()
{
    m_playingAnim.clear();
    m_curAnim = {};
    if (m_animTimer) m_animTimer->stop();
    if (m_playBtn) m_playBtn->setText(QStringLiteral("Play"));
    if (m_timeline) m_timeline->setVisible(false);
    if (m_view) m_view->clearAnimation();
    if (m_anims) { m_anims->blockSignals(true); m_anims->setCurrentItem(nullptr); m_anims->blockSignals(false); }
}

// ── Lighting / Camera popups ────────────────────────────────────────────────────
void StableTab2::showPopup(QWidget* panel, QWidget* anchor)
{
    if (!panel || !anchor) return;
    panel->adjustSize();
    // N-strip buttons open LEFTward (the strip hugs the viewport's right edge); everything
    // else opens below its anchor as before.
    const bool leftward = m_vpStrip && anchor->parentWidget() == m_vpStrip;
    QPoint pos = leftward ? anchor->mapToGlobal(QPoint(-panel->width() - 8, 0))
                          : anchor->mapToGlobal(QPoint(0, anchor->height() + 2));
    // Keep it on-screen: nudge left if it would spill past the window's right edge.
    const int rightEdge = window()->frameGeometry().right();
    if (pos.x() + panel->width() > rightEdge) pos.setX(rightEdge - panel->width() - 4);
    panel->move(pos);
    panel->show();
    panel->raise();
}

// Shared popup-frame factory: the dark, rounded QFrame skin all the viewport popovers use
// (matches the Wardrobe/Models popups). Returns the frame + its ready-to-fill layout.
static QVBoxLayout* makePopupFrame(QFrame*& out, QWidget* parent, const QString& objName,
                                   const QString& title)
{
    out = new QFrame(parent, Qt::Popup);
    out->setObjectName(objName);
    out->setStyleSheet(QStringLiteral(
        "QFrame#%1{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
        "QLabel{color:#cccccc;} QCheckBox{color:#cccccc;}").arg(objName));
    auto* pl = new QVBoxLayout(out);
    pl->setContentsMargins(12, 10, 12, 10);
    pl->setSpacing(5);
    auto* hdr = new QLabel(title, out);
    hdr->setStyleSheet(QLatin1String(kHdrQss));
    pl->addWidget(hdr);
    return pl;
}

// Detail-map config: baked defaults overlaid with the saved stable2/detail/* keys.
static GLModelWidget::DetailConfig stableDetailCfg()
{
    QSettings s;
    GLModelWidget::DetailConfig c;
    auto key = [](const QString& k) { return QStringLiteral("stable2/detail/") + k; };
    c.autoMode = s.value(key(QStringLiteral("auto")), c.autoMode).toBool();
    for (int i = 0; i < 4; ++i) {
        c.zoneMap[i] = s.value(key(QStringLiteral("zone%1").arg(i)), c.zoneMap[i]).toInt();
        c.bands[i]   = float(s.value(key(QStringLiteral("band%1").arg(i)), c.bands[i]).toDouble());
    }
    c.metalThresh = float(s.value(key(QStringLiteral("metalThresh")), c.metalThresh).toDouble());
    c.metalRoute  = s.value(key(QStringLiteral("metalRoute")), c.metalRoute).toInt();
    return c;
}
static QString stableDetailCfgText()
{
    const GLModelWidget::DetailConfig c = stableDetailCfg();
    auto layerName = [](int l) { return l < 0 ? QStringLiteral("none") : QStringLiteral("map%1").arg(l); };
    auto routeName = [](int r) {
        return r == -2 ? QStringLiteral("auto (by texture name)")
             : r == -1 ? QStringLiteral("off")
                       : QStringLiteral("force map%1").arg(r);
    };
    QString t = QStringLiteral("Detail-map config (global):\n");
    t += c.autoMode ? QStringLiteral("  MODE: Auto (per-item game data — values below are the manual fallback)\n")
                    : QStringLiteral("  MODE: Manual override (the values below apply to all items)\n");
    t += QStringLiteral("  zone→map:  zone0(unmasked)=none");
    for (int i = 1; i < 4; ++i)
        t += QStringLiteral(", zone%1=%2").arg(i).arg(layerName(c.zoneMap[i]));
    t += QStringLiteral("\n  dye bands: %1, %2, %3, %4\n")
             .arg(c.bands[0], 0, 'f', 3).arg(c.bands[1], 0, 'f', 3)
             .arg(c.bands[2], 0, 'f', 3).arg(c.bands[3], 0, 'f', 3);
    t += QStringLiteral("  metalness threshold: %1\n").arg(c.metalThresh, 0, 'f', 2);
    t += QStringLiteral("  metal routing: %1\n").arg(routeName(c.metalRoute));
    return t;
}

// ── Lighting popover (faithful Wardrobe port): three-point rig from D4's character-screen
// values + surface/shadow/AO/colour-grade sliders, plus an Environment picker (the Env combo
// removed from the toolbar now lives here). Persisted under stable2/light/*; applyLightRig
// pushes everything to the viewport. ─────────────────────────────────────────────────────────
void StableTab2::buildLightingPanel()
{
    if (m_lightPanel) return;
    QSettings s;
    auto* pl = makePopupFrame(m_lightPanel, this, QStringLiteral("stableLightPanel"), QStringLiteral("Lighting"));
    auto* sub = new QLabel(QStringLiteral("Three-point rig — real D4 character-screen values"), m_lightPanel);
    sub->setStyleSheet(QStringLiteral("color:#888;"));
    pl->addWidget(sub);

    // Environment (ambient backdrop) — the toolbar Env combo re-homed here.
    auto* envRow = new QHBoxLayout();
    envRow->addWidget(new QLabel(QStringLiteral("Environment"), m_lightPanel));
    auto* env = new QComboBox(m_lightPanel);
    {   // Value = GLModelWidget::setEnvironment's own numbering (0 Studio 1 Outdoor 2 Dungeon 3 Night).
        const char* const kEnv[4] = { "Studio", "Outdoor", "Dungeon", "Night" };
        for (int e = 0; e < 4; ++e) env->addItem(QString::fromLatin1(kEnv[e]), e);
    }
    selectByValue(env, envOrDefault(s.value(QStringLiteral("stable2/env"), 1).toInt()), 1);
    connect(env, &QComboBox::currentIndexChanged, this, [this, env](int) {
        const int e = envOrDefault(env->currentData().toInt());
        QSettings().setValue(QStringLiteral("stable2/env"), e);
        if (m_view) m_view->setEnvironment(e);
    });
    envRow->addWidget(env, 1);
    pl->addLayout(envRow);

    // Preset selects the key/rim/fill COLOURS (intensities + key direction are the sliders below).
    auto* preRow = new QHBoxLayout();
    preRow->addWidget(new QLabel(QStringLiteral("Preset"), m_lightPanel));
    auto* preset = new QComboBox(m_lightPanel);
    {   // Value = the rig's own preset numbering, carried as data (see selectByValue).
        const char* const kPre[3] = { "D4 Wardrobe (campfire)", "Hero Direct (neutral)",
                                      "Studio (cool 3-point)" };
        for (int i = 0; i < 3; ++i) preset->addItem(QString::fromLatin1(kPre[i]), i);
    }
    selectByValue(preset, lightPresetOrDefault(s.value(QStringLiteral("stable2/light/preset"), 1).toInt()), 1);
    connect(preset, &QComboBox::currentIndexChanged, this, [this, preset](int) {
        QSettings().setValue(QStringLiteral("stable2/light/preset"),
                             lightPresetOrDefault(preset->currentData().toInt()));
        applyLightRig();
    });
    preRow->addWidget(preset, 1);
    pl->addLayout(preRow);

    auto* reflChk = new QCheckBox(QStringLiteral("Reflections (game probe)"), m_lightPanel);
    reflChk->setChecked(s.value(QStringLiteral("stable2/light/reflections"), true).toBool());
    connect(reflChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("stable2/light/reflections"), on);
        if (m_view) m_view->setReflectionEnabled(on);
    });
    pl->addWidget(reflChk);

    auto* lockChk = new QCheckBox(QStringLiteral("Lock lights to world"), m_lightPanel);
    lockChk->setChecked(s.value(QStringLiteral("stable2/light/lock"), false).toBool());
    lockChk->setToolTip(QStringLiteral(
        "Off: three-point rig tracks the camera. On: pin the lights at the current orbit in world space."));
    connect(lockChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("stable2/light/lock"), on);
        if (m_view) m_view->setLightLock(on);
    });
    pl->addWidget(lockChk);

    struct SRow { QSlider* sl; int def; QString key; };
    QVector<SRow> rows;
    auto slider = [&](const QString& key, const QString& label, int lo, int hi, int def, const QString& tip) {
        auto* row = new QHBoxLayout();
        auto* lbl = new QLabel(label, m_lightPanel);
        lbl->setMinimumWidth(64); lbl->setToolTip(tip);
        row->addWidget(lbl);
        auto* sl = new QSlider(Qt::Horizontal, m_lightPanel);
        sl->setRange(lo, hi);
        const int init = s.value(QStringLiteral("stable2/light/") + key, def).toInt();
        sl->setValue(init);
        auto* val = new QLabel(QString::number(init), m_lightPanel);
        val->setMinimumWidth(30); val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        connect(sl, &QSlider::valueChanged, this, [this, key, val](int vv) {
            QSettings().setValue(QStringLiteral("stable2/light/") + key, vv);
            val->setText(QString::number(vv));
            applyLightRig();
        });
        row->addWidget(sl, 1); row->addWidget(val);
        pl->addLayout(row);
        rows.append({ sl, def, key });
    };
    auto section = [&](const QString& title) {
        auto* h = new QLabel(title, m_lightPanel);
        h->setStyleSheet(QStringLiteral("color:#e0a060;font-weight:bold;margin-top:7px;"));
        pl->addWidget(h);
    };
    section(QStringLiteral("Lights"));
    slider(QStringLiteral("key"),  QStringLiteral("Key %"),     0, 200, 100, QStringLiteral("Warm campfire key intensity"));
    slider(QStringLiteral("rim"),  QStringLiteral("Rim %"),     0, 200, 100, QStringLiteral("Cool back-rim intensity (edge separation)"));
    slider(QStringLiteral("fill"), QStringLiteral("Fill %"),    0, 200, 100, QStringLiteral("Cool front-fill intensity (shadow lift)"));
    slider(QStringLiteral("amb"),  QStringLiteral("Ambient %"), 0, 200, 100, QStringLiteral("Hemisphere-ambient (IBL) scale"));
    slider(QStringLiteral("exp"),  QStringLiteral("Exposure %"), 25, 300, 100, QStringLiteral("Overall exposure before tonemapping"));
    slider(QStringLiteral("az"),   QStringLiteral("Key L-R"),  -90,  90,  15, QStringLiteral("Key azimuth (degrees, + = camera-right)"));
    slider(QStringLiteral("el"),   QStringLiteral("Key U-D"),    0,  80,  25, QStringLiteral("Key elevation (degrees above the camera horizon)"));
    section(QStringLiteral("Surface"));
    slider(QStringLiteral("refl"),     QStringLiteral("Reflection %"), 0, 300, 100, QStringLiteral("Reflection / ambient-specular intensity"));
    slider(QStringLiteral("sss"),      QStringLiteral("Subsurface %"), 0, 200,  15, QStringLiteral("Subsurface scattering strength"));
    slider(QStringLiteral("skinwarm"), QStringLiteral("Skin warmth"),  0, 200, 100, QStringLiteral("Subsurface red-bleed hue"));
    slider(QStringLiteral("wetness"),  QStringLiteral("Wetness %"),    0, 100,   0, QStringLiteral("Rain-slick look: darkens diffuse, sharpens reflections"));
    slider(QStringLiteral("snow"),     QStringLiteral("Snow %"),       0, 100,   0, QStringLiteral("Snow dusting on upward-facing surfaces"));
    slider(QStringLiteral("emis"),     QStringLiteral("Emissive %"),   0, 300,  50, QStringLiteral("Glow intensity of emissive materials"));
    section(QStringLiteral("Shadows"));
    slider(QStringLiteral("shadowStr"),  QStringLiteral("Shadow %"),    0, 100,  60, QStringLiteral("Self-shadow darkness"));
    slider(QStringLiteral("shadowSoft"), QStringLiteral("Shadow soft"), 0,  40,  15, QStringLiteral("Shadow edge softness (PCF radius, ÷10 texels)"));
    slider(QStringLiteral("shadowBias"), QStringLiteral("Shadow bias"), 0,  50,  18, QStringLiteral("Depth bias to avoid shadow acne (÷10000)"));
    slider(QStringLiteral("shadowNBias"),QStringLiteral("Shadow n-bias"),0, 50,  10, QStringLiteral("Normal-offset bias (÷1000 of model size)"));
    slider(QStringLiteral("shadowRange"),QStringLiteral("Shadow range"),100,300,130, QStringLiteral("Shadow frustum tightness (÷100)"));
    slider(QStringLiteral("shadowRes"),  QStringLiteral("Shadow res"), 1024,4096,2048,QStringLiteral("Shadow-map resolution"));
    section(QStringLiteral("Ambient occlusion"));
    slider(QStringLiteral("ssaoStr"), QStringLiteral("Amb. occlusion %"), 0, 200, 100, QStringLiteral("SSAO darkness in creases/contact areas"));
    slider(QStringLiteral("ssaoRad"), QStringLiteral("AO radius"),         5, 100,  30, QStringLiteral("SSAO sampling radius (÷100)"));
    section(QStringLiteral("Colour grade"));
    auto* gradeChk = new QCheckBox(QStringLiteral("Enable colour grade"), m_lightPanel);
    gradeChk->setChecked(s.value(QStringLiteral("stable2/light/grade"), false).toBool());
    connect(gradeChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("stable2/light/grade"), on); applyLightRig();
    });
    pl->addWidget(gradeChk);
    slider(QStringLiteral("gradeContrast"), QStringLiteral("Contrast"),  50, 200, 105, QStringLiteral("Contrast S-curve about mid-grey (÷100)"));
    slider(QStringLiteral("gradeSat"),      QStringLiteral("Saturation"), 0, 200, 110, QStringLiteral("Colour saturation (÷100)"));
    slider(QStringLiteral("gradeWarmth"),   QStringLiteral("Split-tone"), 0, 200,  30, QStringLiteral("Warm shadows / cool highlights (÷1000)"));

    const QVector<SRow> rowsCopy = rows;
    auto* btnRow = new QHBoxLayout();
    auto* dBtn = new QPushButton(QStringLiteral("Default"), m_lightPanel);
    auto* saveBtn = new QPushButton(QStringLiteral("Save preset"), m_lightPanel);
    auto* restoreBtn = new QPushButton(QStringLiteral("Restore preset"), m_lightPanel);
    connect(dBtn, &QPushButton::clicked, this, [rowsCopy] { for (const SRow& r : rowsCopy) r.sl->setValue(r.def); });
    connect(saveBtn, &QPushButton::clicked, this, [rowsCopy] {
        QSettings q; for (const SRow& r : rowsCopy)
            q.setValue(QStringLiteral("stable2/preset/light/%1").arg(r.key), r.sl->value());
    });
    connect(restoreBtn, &QPushButton::clicked, this, [rowsCopy] {
        QSettings q; for (const SRow& r : rowsCopy)
            r.sl->setValue(q.value(QStringLiteral("stable2/preset/light/%1").arg(r.key), r.sl->value()).toInt());
    });
    btnRow->addWidget(dBtn); btnRow->addWidget(saveBtn); btnRow->addWidget(restoreBtn);
    pl->addLayout(btnRow);
}

void StableTab2::applyLightRig()
{
    if (!m_view) return;
    QSettings s;
    GLModelWidget::LightRig r;
    r.preset       = lightPresetOrDefault(s.value(QStringLiteral("stable2/light/preset"), 1).toInt());
    r.keyInt       = s.value(QStringLiteral("stable2/light/key"),  100).toInt() / 100.0f;
    r.rimInt       = s.value(QStringLiteral("stable2/light/rim"),  100).toInt() / 100.0f;
    r.fillInt      = s.value(QStringLiteral("stable2/light/fill"), 100).toInt() / 100.0f;
    r.ambInt       = s.value(QStringLiteral("stable2/light/amb"),  100).toInt() / 100.0f;
    r.keyAzimuth   = float(s.value(QStringLiteral("stable2/light/az"), 15).toInt());
    r.keyElevation = float(s.value(QStringLiteral("stable2/light/el"), 25).toInt());
    m_view->setLightRig(r);
    m_view->setReflectionStrength(s.value(QStringLiteral("stable2/light/refl"),     100).toInt() / 100.0f);
    m_view->setSkinWarmth(        s.value(QStringLiteral("stable2/light/skinwarm"), 100).toInt() / 100.0f);
    m_view->setSssStrength(       s.value(QStringLiteral("stable2/light/sss"),       15).toInt() / 100.0f);
    m_view->setWetness(           s.value(QStringLiteral("stable2/light/wetness"),    0).toInt() / 100.0f);
    m_view->setSnow(              s.value(QStringLiteral("stable2/light/snow"),       0).toInt() / 100.0f);
    m_view->setEmissiveScale(     s.value(QStringLiteral("stable2/light/emis"),      50).toInt() / 100.0f);
    m_view->setShadowParams(      s.value(QStringLiteral("stable2/light/shadowStr"),  60).toInt() / 100.0f,
                                  s.value(QStringLiteral("stable2/light/shadowSoft"), 15).toInt() / 10.0f,
                                  s.value(QStringLiteral("stable2/light/shadowBias"), 18).toInt() / 10000.0f);
    m_view->setShadowExtra(       s.value(QStringLiteral("stable2/light/shadowRange"), 130).toInt() / 100.0f,
                                  s.value(QStringLiteral("stable2/light/shadowNBias"),  10).toInt() / 1000.0f,
                                  s.value(QStringLiteral("stable2/light/shadowRes"),  2048).toInt());
    m_view->setLightLock(         s.value(QStringLiteral("stable2/light/lock"), false).toBool());
    m_view->setExposure(          s.value(QStringLiteral("stable2/light/exp"),        100).toInt() / 100.0f);
    m_view->setReflectionEnabled( s.value(QStringLiteral("stable2/light/reflections"), true).toBool());
    m_view->setColorGrade(        s.value(QStringLiteral("stable2/light/grade"),     false).toBool(),
                                  s.value(QStringLiteral("stable2/light/gradeContrast"), 105).toInt() / 100.0f,
                                  s.value(QStringLiteral("stable2/light/gradeSat"),      110).toInt() / 100.0f,
                                  s.value(QStringLiteral("stable2/light/gradeWarmth"),    30).toInt() / 1000.0f);
    m_view->setSsaoParams(        s.value(QStringLiteral("stable2/light/ssaoStr"),    100).toInt() / 100.0f,
                                  s.value(QStringLiteral("stable2/light/ssaoRad"),     30).toInt() / 100.0f);
}

// ── Camera popover (Wardrobe port): FOV · view angles · frame · turntable · orthographic ·
// remember-camera · three camera presets. (The equipment-only "Camera Snap to slot" controls
// are dropped — mounts aren't slot-framed like a character.) ─────────────────────────────────
void StableTab2::buildCameraPanel()
{
    if (m_camPanel) return;
    QSettings s;
    auto* pl = makePopupFrame(m_camPanel, this, QStringLiteral("stableCamPanel"), QStringLiteral("Camera"));

    // Frame part on select — the same GLOBAL key the Models/Wardrobe Camera panels write.
    auto* frameChk = new QCheckBox(QStringLiteral("Frame part on select"), m_camPanel);
    frameChk->setToolTip(QStringLiteral("Double-clicking a part in the viewport also zooms/centres the camera on it."));
    frameChk->setChecked(s.value(QStringLiteral("viewer/framePartOnPick"), true).toBool());
    connect(frameChk, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("viewer/framePartOnPick"), on);
    });
    pl->addWidget(frameChk);

    // Camera field-of-view.
    auto* fovRow = new QHBoxLayout();
    fovRow->addWidget(new QLabel(QStringLiteral("FOV"), m_camPanel));
    auto* fovSlider = new QSlider(Qt::Horizontal, m_camPanel);
    fovSlider->setRange(10, 100);
    // Seeded from the LIVE viewport, not from a settings key of its own. stable2/cam/fov already
    // carries this state and restoreCameraState() has already applied it; a second key meant two
    // answers to one question, and because this panel is built lazily its copy was never applied
    // at startup at all — whichever ran last won.
    fovSlider->setValue(m_view ? qBound(10, int(m_view->cameraState().fov + 0.5f), 100) : 45);
    fovSlider->setToolTip(QStringLiteral("Camera field of view (degrees)"));
    connect(fovSlider, &QSlider::valueChanged, this, [this](int vv) {
        if (m_view) m_view->setFov(float(vv));   // persisted by saveCameraState(), one key
    });
    fovRow->addWidget(fovSlider, 1);
    pl->addLayout(fovRow);

    // View-angle presets: orbit to a fixed angle around the whole model (keeps current zoom).
    pl->addWidget(new QLabel(QStringLiteral("View angle"), m_camPanel));
    auto* presetRow = new QHBoxLayout();
    presetRow->setSpacing(3);
    auto mkPreset = [&](const QString& text, float yaw, float pitch) {
        auto* b = new QPushButton(text, m_camPanel);
        connect(b, &QPushButton::clicked, this, [this, yaw, pitch] {
            if (!m_view) return;
            m_view->followParts(QVector<int>{});
            m_view->frameThreeQuarter(yaw, pitch, 0.12f);
        });
        presetRow->addWidget(b);
    };
    mkPreset(QStringLiteral("¾"),     0.9708f,  0.12f);
    mkPreset(QStringLiteral("Front"), 1.5708f,  0.05f);
    mkPreset(QStringLiteral("Back"), -1.5708f,  0.05f);
    mkPreset(QStringLiteral("Left"),  0.0f,     0.05f);
    mkPreset(QStringLiteral("Right"), 3.14159f, 0.05f);
    pl->addLayout(presetRow);

    auto* fullBtn = new QPushButton(QStringLiteral("Frame full body  (F)"), m_camPanel);
    fullBtn->setToolTip(QStringLiteral("Zoom back out to the whole model, keeping your current angle."));
    connect(fullBtn, &QPushButton::clicked, this, [this] {
        if (m_view) m_view->frameAll(/*keepRotation=*/true);
    });
    pl->addWidget(fullBtn);

    // Auto-rotate turntable (spin speed persisted as a float 0.001–0.1).
    auto* spinChk = new QCheckBox(QStringLiteral("Auto-rotate (turntable)"), m_camPanel);
    spinChk->setChecked(s.value(QStringLiteral("stable2/spin"), false).toBool());
    auto* spinRow = new QHBoxLayout();
    spinRow->addWidget(new QLabel(QStringLiteral("Speed"), m_camPanel));
    auto* spinSpeed = new QSlider(Qt::Horizontal, m_camPanel);
    spinSpeed->setRange(1, 100);
    spinSpeed->setValue(qBound(1, int(s.value(QStringLiteral("stable2/spinSpeed"), 0.025f).toFloat() * 1000.0f + 0.5f), 100));
    spinRow->addWidget(spinSpeed, 1);
    connect(spinChk, &QCheckBox::toggled, this, [this, spinSpeed](bool on) {
        QSettings().setValue(QStringLiteral("stable2/spin"), on);
        if (!m_view) return;
        m_view->setSpinSpeed(float(spinSpeed->value()) / 1000.0f);
        m_view->setAutoSpin(on);
    });
    connect(spinSpeed, &QSlider::valueChanged, this, [this](int vv) {
        QSettings().setValue(QStringLiteral("stable2/spinSpeed"), float(vv) / 1000.0f);
        if (m_view) m_view->setSpinSpeed(float(vv) / 1000.0f);
    });
    pl->addWidget(spinChk);
    pl->addLayout(spinRow);

    // Numeric orbit control. Placed AFTER the turntable so it can be handed that checkbox —
    // editing an angle by hand unticks it, since the spin would otherwise overwrite yaw 30x a
    // second and the control would look dead.
    m_camOrbitSync = CameraOrbit::addRows(m_camPanel, pl, m_view, spinChk);

    // Orthographic projection.
    auto* orthoChk = new QCheckBox(QStringLiteral("Orthographic projection"), m_camPanel);
    // As with FOV: the live viewport is the source, stable2/cam/ortho the single persisted copy.
    orthoChk->setChecked(m_view && m_view->cameraState().ortho);
    connect(orthoChk, &QCheckBox::toggled, this, [this](bool on) {
        if (m_view) m_view->setOrthographic(on);   // persisted by saveCameraState(), one key
    });
    pl->addWidget(orthoChk);

    // Remember camera on relaunch.
    auto* rememberChk = new QCheckBox(QStringLiteral("Remember camera on relaunch"), m_camPanel);
    rememberChk->setChecked(s.value(QStringLiteral("stable2/rememberCam"), true).toBool());
    connect(rememberChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("stable2/rememberCam"), on);
        if (on) saveCameraState();
    });
    pl->addWidget(rememberChk);

    // Camera presets: three slots storing the current camera (angle/zoom/FOV/projection).
    auto* presetHdr = new QLabel(QStringLiteral("Camera presets"), m_camPanel);
    presetHdr->setStyleSheet(QStringLiteral("color:#aaa;"));
    pl->addWidget(presetHdr);
    for (int n = 1; n <= 3; ++n) {
        const QString key = QStringLiteral("stable2/campreset/%1/").arg(n);
        auto* prow = new QHBoxLayout(); prow->setSpacing(3);
        auto* loadBtn = new QPushButton(QStringLiteral("Preset %1").arg(n), m_camPanel);
        loadBtn->setEnabled(s.value(key + QStringLiteral("set"), false).toBool());
        auto* saveBtn = new QPushButton(QStringLiteral("Save"), m_camPanel);
        connect(saveBtn, &QPushButton::clicked, this, [this, key, loadBtn] {
            if (!m_view) return;
            const GLModelWidget::CamState c = m_view->cameraState();
            QSettings st2;
            st2.setValue(key + QStringLiteral("yaw"), c.yaw);   st2.setValue(key + QStringLiteral("pitch"), c.pitch);
            st2.setValue(key + QStringLiteral("dist"), c.dist); st2.setValue(key + QStringLiteral("fov"), c.fov);
            st2.setValue(key + QStringLiteral("cx"), c.cx);     st2.setValue(key + QStringLiteral("cy"), c.cy);
            st2.setValue(key + QStringLiteral("cz"), c.cz);     st2.setValue(key + QStringLiteral("ortho"), c.ortho);
            st2.setValue(key + QStringLiteral("set"), true);
            loadBtn->setEnabled(true);
        });
        connect(loadBtn, &QPushButton::clicked, this, [this, key, fovSlider] {
            QSettings st2;
            if (!m_view || !st2.value(key + QStringLiteral("set"), false).toBool()) return;
            GLModelWidget::CamState c;
            c.yaw   = st2.value(key + QStringLiteral("yaw"),   c.yaw).toFloat();
            c.pitch = st2.value(key + QStringLiteral("pitch"), c.pitch).toFloat();
            c.dist  = st2.value(key + QStringLiteral("dist"),  c.dist).toFloat();
            c.fov   = st2.value(key + QStringLiteral("fov"),   c.fov).toFloat();
            c.cx    = st2.value(key + QStringLiteral("cx"), 0.0).toFloat();
            c.cy    = st2.value(key + QStringLiteral("cy"), 0.0).toFloat();
            c.cz    = st2.value(key + QStringLiteral("cz"), 0.0).toFloat();
            c.ortho = st2.value(key + QStringLiteral("ortho"), false).toBool();
            c.valid = true;
            m_view->setCameraState(c);
            fovSlider->setValue(int(c.fov));
        });
        prow->addWidget(loadBtn, 1);
        prow->addWidget(saveBtn);
        pl->addLayout(prow);
    }
}

// ── Graphics popover (Wardrobe Preview/Graphics port): render-quality features grouped by
// concern + backdrop presets/gradient. Live + persisted under stable2/gfx/*. ─────────────────
void StableTab2::buildGraphicsPanel()
{
    if (m_gfxPanel) return;
    QSettings s;
    auto* pl = makePopupFrame(m_gfxPanel, this, QStringLiteral("stableGfxPanel"), QStringLiteral("Graphics"));

    auto addChkTo = [&](QVBoxLayout* into, const QString& key, const QString& label, bool def,
                        std::function<void(bool)> apply) {
        auto* cb = new QCheckBox(label, m_gfxPanel);
        cb->setChecked(s.value(QStringLiteral("stable2/gfx/") + key, def).toBool());
        connect(cb, &QCheckBox::toggled, this, [this, key, apply](bool on) {
            QSettings().setValue(QStringLiteral("stable2/gfx/") + key, on);
            apply(on);
        });
        into->addWidget(cb);
    };
    auto addGroup = [&](const QString& title) -> QVBoxLayout* {
        auto* box = new QGroupBox(title, m_gfxPanel);
        auto* gl  = new QVBoxLayout(box);
        gl->setContentsMargins(8, 4, 8, 4);
        pl->addWidget(box);
        return gl;
    };

    auto* gLight = addGroup(QStringLiteral("Scene && shadows"));
    addChkTo(gLight, QStringLiteral("ibl"), QStringLiteral("Environment lighting (IBL)"), true,
             [this](bool on) { if (m_view) m_view->setFeatureIbl(on); });
    addChkTo(gLight, QStringLiteral("shadow"), QStringLiteral("Self-shadows"), true,
             [this](bool on) { if (m_view) m_view->setShadowEnabled(on); });
    addChkTo(gLight, QStringLiteral("ssao"), QStringLiteral("Ambient occlusion (SSAO)"), true,
             [this](bool on) { if (m_view) m_view->setSsaoEnabled(on); });
    addChkTo(gLight, QStringLiteral("tonemap"), QStringLiteral("Tonemap (ACES) + sRGB"), true,
             [this](bool on) { if (m_view) m_view->setFeatureTonemap(on); });

    auto* gShade = addGroup(QStringLiteral("Shading"));
    addChkTo(gShade, QStringLiteral("detail"), QStringLiteral("Detail maps"), true,
             [this](bool on) { if (m_view) m_view->setFeatureDetail(on); });
    addChkTo(gShade, QStringLiteral("subsurface"), QStringLiteral("Subsurface / translucency"), true,
             [this](bool on) { if (m_view) m_view->setFeatureSubsurface(on); });
    addChkTo(gShade, QStringLiteral("hair"), QStringLiteral("Hair anisotropy"), true,
             [this](bool on) { if (m_view) m_view->setFeatureHair(on); });
    addChkTo(gShade, QStringLiteral("specaa"), QStringLiteral("Specular anti-aliasing"), true,
             [this](bool on) { if (m_view) m_view->setFeatureSpecAA(on); });

    auto* gGeom = addGroup(QStringLiteral("Geometry && debug"));
    addChkTo(gGeom, QStringLiteral("mask"), QStringLiteral("Primary mask"), false,
             [this](bool on) { if (m_view) m_view->setFeatureMask(on); });

    // Backdrop: one-click studio presets + optional vertical gradient + custom colour.
    {
        auto* gBg = addGroup(QStringLiteral("Backdrop"));
        auto* row = new QHBoxLayout();
        row->setSpacing(4);
        auto chip = [&](const char* name, const QColor& c) {
            auto* b = new QToolButton(m_gfxPanel);
            b->setFixedSize(24, 20);
            b->setToolTip(QString::fromLatin1(name));
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(QStringLiteral("QToolButton{background:%1;border:1px solid #555;"
                                            "border-radius:3px;}QToolButton:hover{border-color:#b0453c;}")
                                 .arg(c.name()));
            connect(b, &QToolButton::clicked, this, [this, c]() {
                if (m_view) m_view->setBackgroundColor(c);
                QSettings().setValue(QStringLiteral("stable2/gfx/bg"), c.name());
            });
            row->addWidget(b);
        };
        chip("Dark",     QColor(0x10, 0x10, 0x10));
        chip("Charcoal", QColor(0x23, 0x23, 0x23));
        chip("Grey",     QColor(0x4b, 0x4b, 0x4b));
        chip("Light",    QColor(0xa6, 0xa6, 0xa6));
        auto* custom = new QToolButton(m_gfxPanel);
        custom->setText(QStringLiteral("…"));
        custom->setToolTip(QStringLiteral("Custom background colour"));
        custom->setFixedSize(24, 20);
        custom->setCursor(Qt::PointingHandCursor);
        custom->setStyleSheet(QLatin1String(kArrowBtnQss));
        connect(custom, &QToolButton::clicked, this, [this] {
            if (!m_view) return;
            const QColor c = QColorDialog::getColor(m_view->backgroundColor(), m_gfxPanel,
                                                    QStringLiteral("Viewport background"));
            if (c.isValid()) {
                m_view->setBackgroundColor(c);
                QSettings().setValue(QStringLiteral("stable2/gfx/bg"), c.name());
            }
        });
        row->addWidget(custom);
        row->addStretch(1);
        gBg->addLayout(row);
        auto* grad = new QCheckBox(QStringLiteral("Gradient (lighter top, darker floor)"), m_gfxPanel);
        grad->setChecked(s.value(QStringLiteral("stable2/gfx/bgGradient"), false).toBool());
        connect(grad, &QCheckBox::toggled, this, [this](bool on) {
            QSettings().setValue(QStringLiteral("stable2/gfx/bgGradient"), on);
            if (m_view) m_view->setBackgroundGradient(on);
        });
        gBg->addWidget(grad);
    }
    applyGraphics();
}

void StableTab2::applyGraphics()
{
    if (!m_view) return;
    QSettings s;
    auto b = [&](const QString& k, bool def) { return s.value(QStringLiteral("stable2/gfx/") + k, def).toBool(); };
    m_view->setFeatureIbl(b(QStringLiteral("ibl"), true));
    m_view->setShadowEnabled(b(QStringLiteral("shadow"), true));
    m_view->setSsaoEnabled(b(QStringLiteral("ssao"), true));
    m_view->setFeatureTonemap(b(QStringLiteral("tonemap"), true));
    m_view->setFeatureDetail(b(QStringLiteral("detail"), true));
    m_view->setFeatureSubsurface(b(QStringLiteral("subsurface"), true));
    m_view->setFeatureHair(b(QStringLiteral("hair"), true));
    m_view->setFeatureSpecAA(b(QStringLiteral("specaa"), true));
    m_view->setFeatureMask(b(QStringLiteral("mask"), false));
    const QString bg = s.value(QStringLiteral("stable2/gfx/bg")).toString();
    if (!bg.isEmpty()) m_view->setBackgroundColor(QColor(bg));
    m_view->setBackgroundGradient(b(QStringLiteral("bgGradient"), false));
}

// ── Shaders popover (Wardrobe port): shell-fur (mane/tail/fur) + mesh-FX shading, with
// Default/Save/Restore preset buttons per section. Fur under stable2/fur/*, FX under stable2/fx/*. ─
void StableTab2::buildShaderPanel()
{
    if (m_shaderPanel) return;
    QSettings s;
    auto* pl = makePopupFrame(m_shaderPanel, this, QStringLiteral("stableShaderPanel"), QStringLiteral("Shaders"));

    auto* furChk = new QCheckBox(QStringLiteral("Fur (shell displacement)"), m_shaderPanel);
    furChk->setChecked(s.value(QStringLiteral("stable2/fur/on"), true).toBool());
    furChk->setToolTip(QStringLiteral("Render auto-detected fur/mane/tail materials as extruded shell fur."));
    connect(furChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("stable2/fur/on"), on);
        if (m_view) m_view->setFurEnabled(on);
    });
    pl->addWidget(furChk);

    auto* furHdr = new QLabel(QStringLiteral("Fur detail"), m_shaderPanel);
    furHdr->setStyleSheet(QStringLiteral("color:#9ad; margin-top:4px;"));
    pl->addWidget(furHdr);

    struct SRow { QSlider* sl; int def; QString key; QString group; };
    QVector<SRow> furRows, fxRows;
    // A slider whose int value maps to a float via `scale`; persisted under stable2/<group>/<key>.
    auto shaderSlider = [&](QVector<SRow>& rows, const QString& group, const QString& key,
                            const QString& label, int lo, int hi, int def, double scale,
                            std::function<void(double)> apply) {
        auto* row = new QHBoxLayout();
        auto* lbl = new QLabel(label, m_shaderPanel);
        lbl->setMinimumWidth(54);
        row->addWidget(lbl);
        auto* sl = new QSlider(Qt::Horizontal, m_shaderPanel);
        sl->setRange(lo, hi);
        const int init = s.value(QStringLiteral("stable2/%1/%2").arg(group, key), def).toInt();
        sl->setValue(init);
        auto* val = new QLabel(QString::number(init), m_shaderPanel);
        val->setMinimumWidth(26); val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        connect(sl, &QSlider::valueChanged, this, [this, group, key, apply, scale, val](int v) {
            QSettings().setValue(QStringLiteral("stable2/%1/%2").arg(group, key), v);
            val->setText(QString::number(v));
            apply(v * scale);
        });
        row->addWidget(sl, 1); row->addWidget(val);
        pl->addLayout(row);
        rows.append({ sl, def, key, group });
    };
    auto presetButtons = [&](const QString& section, const QVector<SRow>& rowsRef) {
        const QVector<SRow> rows = rowsRef;
        auto* row = new QHBoxLayout();
        auto* dBtn = new QPushButton(QStringLiteral("Default"), m_shaderPanel);
        auto* sBtn = new QPushButton(QStringLiteral("Save preset"), m_shaderPanel);
        auto* rBtn = new QPushButton(QStringLiteral("Restore preset"), m_shaderPanel);
        connect(dBtn, &QPushButton::clicked, this, [rows] { for (const SRow& r : rows) r.sl->setValue(r.def); });
        connect(sBtn, &QPushButton::clicked, this, [rows, section] {
            QSettings q; for (const SRow& r : rows)
                q.setValue(QStringLiteral("stable2/preset/%1/%2").arg(section, r.key), r.sl->value());
        });
        connect(rBtn, &QPushButton::clicked, this, [rows, section] {
            QSettings q; for (const SRow& r : rows)
                r.sl->setValue(q.value(QStringLiteral("stable2/preset/%1/%2").arg(section, r.key), r.sl->value()).toInt());
        });
        row->addWidget(dBtn); row->addWidget(sBtn); row->addWidget(rBtn);
        pl->addLayout(row);
    };

    shaderSlider(furRows, QStringLiteral("fur"), QStringLiteral("furLength"),  QStringLiteral("Length"),  0,  60, 44, 0.0005,
                 [this](double v) { if (m_view) m_view->setFurLength(float(v)); });
    shaderSlider(furRows, QStringLiteral("fur"), QStringLiteral("furDensity"), QStringLiteral("Density"), 16, 120, 30, 1.0,
                 [this](double v) { if (m_view) m_view->setFurDensity(float(v)); });
    shaderSlider(furRows, QStringLiteral("fur"), QStringLiteral("furShells"),  QStringLiteral("Shells"),  4,  24, 20, 1.0,
                 [this](double v) { if (m_view) m_view->setFurShells(int(v + 0.5)); });
    shaderSlider(furRows, QStringLiteral("fur"), QStringLiteral("furGravity"), QStringLiteral("Gravity"), 0,  40, 18, 0.00025,
                 [this](double v) { if (m_view) m_view->setFurGravity(float(v)); });
    shaderSlider(furRows, QStringLiteral("fur"), QStringLiteral("furCurl"),    QStringLiteral("Comb"),    0,  40, 14, 0.00025,
                 [this](double v) { if (m_view) m_view->setFurCurl(float(v)); });
    shaderSlider(furRows, QStringLiteral("fur"), QStringLiteral("furCoverage"),QStringLiteral("Coverage"),0,  60, 57, 0.01,
                 [this](double v) { if (m_view) m_view->setFurCoverage(float(0.60 - v)); });
    presetButtons(QStringLiteral("fur"), furRows);

    auto* fxHdr = new QLabel(QStringLiteral("Mesh FX  (× authored game values)"), m_shaderPanel);
    fxHdr->setStyleSheet(QStringLiteral("color:#9ad; margin-top:6px;"));
    pl->addWidget(fxHdr);
    shaderSlider(fxRows, QStringLiteral("fx"), QStringLiteral("fxIntensity"), QStringLiteral("Bright"), 0, 40, 20, 0.05,
                 [this](double v) { if (m_view) m_view->setFxIntensity(float(v)); });
    shaderSlider(fxRows, QStringLiteral("fx"), QStringLiteral("fxScroll"),    QStringLiteral("Scroll"), 0, 40, 20, 0.05,
                 [this](double v) { if (m_view) m_view->setFxScrollSpeed(float(v)); });
    shaderSlider(fxRows, QStringLiteral("fx"), QStringLiteral("fxWobble"),    QStringLiteral("Wobble"), 0, 40, 20, 0.05,
                 [this](double v) { if (m_view) m_view->setFxWobble(float(v)); });
    presetButtons(QStringLiteral("fx"), fxRows);
    applyFur();
}

void StableTab2::applyFur()
{
    if (!m_view) return;
    QSettings s;
    m_view->setFurEnabled(s.value(QStringLiteral("stable2/fur/on"), true).toBool());
    m_view->setFurLength(s.value(QStringLiteral("stable2/fur/furLength"), 44).toInt() * 0.0005f);
    m_view->setFurDensity(float(s.value(QStringLiteral("stable2/fur/furDensity"), 30).toInt()));
    m_view->setFurShells(s.value(QStringLiteral("stable2/fur/furShells"), 20).toInt());
    m_view->setFurGravity(s.value(QStringLiteral("stable2/fur/furGravity"), 18).toInt() * 0.00025f);
    m_view->setFurCurl(s.value(QStringLiteral("stable2/fur/furCurl"), 14).toInt() * 0.00025f);
    m_view->setFurCoverage(0.60f - s.value(QStringLiteral("stable2/fur/furCoverage"), 57).toInt() * 0.01f);
    m_view->setFxIntensity(s.value(QStringLiteral("stable2/fx/fxIntensity"), 20).toInt() * 0.05f);
    m_view->setFxScrollSpeed(s.value(QStringLiteral("stable2/fx/fxScroll"), 20).toInt() * 0.05f);
    m_view->setFxWobble(s.value(QStringLiteral("stable2/fx/fxWobble"), 20).toInt() * 0.05f);
}

// ── Detail-maps popover (Wardrobe port): a global discovery tool for the detail-map selection
// rule (zone→map, metalness routing, dye bands). Auto uses the per-item game data; turn it off to
// override and experiment, then Copy config. Persisted under stable2/detail/*. ────────────────
void StableTab2::buildDetailPanel()
{
    if (m_detailPanel) return;
    m_detailPanel = new QFrame(this, Qt::Popup);
    m_detailPanel->setObjectName(QStringLiteral("stableDetailPanel"));
    m_detailPanel->setStyleSheet(QStringLiteral(
        "QFrame#stableDetailPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
        "QLabel{color:#cccccc;} QComboBox{color:#dddddd;background:#2b2b2b;border:1px solid #555;"
        "border-radius:3px;padding:1px 4px;} QComboBox QAbstractItemView{background:#2b2b2b;color:#ddd;"
        "selection-background-color:#8a1414;}"));
    auto* pl = new QVBoxLayout(m_detailPanel);
    pl->setContentsMargins(12, 10, 12, 10);
    pl->setSpacing(5);
    auto* hdr = new QLabel(QStringLiteral("Detail maps  (discovery tool — global)"), m_detailPanel);
    hdr->setStyleSheet(QLatin1String(kHdrQss));
    pl->addWidget(hdr);
    auto* note = new QLabel(QStringLiteral("Auto uses the rule derived from each item's game data.\n"
                                           "Turn it off to override and experiment, then Copy config."), m_detailPanel);
    note->setStyleSheet(QStringLiteral("color:#888;font-size:11px;"));
    pl->addWidget(note);

    auto setD = [](const QString& k, const QVariant& val) {
        QSettings().setValue(QStringLiteral("stable2/detail/") + k, val); };

    auto* autoChk = new QCheckBox(QStringLiteral("Auto (derive from game data)"), m_detailPanel);
    autoChk->setChecked(stableDetailCfg().autoMode);
    autoChk->setToolTip(QStringLiteral("Bands from the dye mask, zone→map from present maps, metal by name."));
    auto* manual = new QWidget(m_detailPanel);
    manual->setEnabled(!autoChk->isChecked());
    connect(autoChk, &QCheckBox::toggled, this, [this, setD, manual](bool on) {
        setD(QStringLiteral("auto"), on); applyDetailConfig();
        manual->setEnabled(!on);
    });
    pl->addWidget(autoChk);
    auto* ml0 = new QVBoxLayout(manual); ml0->setContentsMargins(0, 0, 0, 0); ml0->setSpacing(5);
    pl->addWidget(manual);

    // Zone → map selectors (zone0 is the unmasked/bare band → always none).
    auto* zHdr = new QLabel(QStringLiteral("Dye-zone → detail map"), manual);
    zHdr->setStyleSheet(QStringLiteral("color:#9ad; margin-top:4px;"));
    ml0->addWidget(zHdr);
    const GLModelWidget::DetailConfig cur = stableDetailCfg();
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    const char* zoneLbl[4] = { "Zone 0 (bare)", "Zone 1", "Zone 2", "Zone 3" };
    for (int z = 1; z < 4; ++z) {
        auto* lbl = new QLabel(QString::fromLatin1(zoneLbl[z]), manual);
        auto* combo = new QComboBox(manual);
        combo->addItem(QStringLiteral("none"), -1);
        combo->addItem(QStringLiteral("map 0"), 0);
        combo->addItem(QStringLiteral("map 1"), 1);
        combo->addItem(QStringLiteral("map 2"), 2);
        const int idx = combo->findData(cur.zoneMap[z]);
        combo->setCurrentIndex(idx >= 0 ? idx : 0);
        connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, z, setD](int) {
            setD(QStringLiteral("zone%1").arg(z), combo->currentData().toInt());
            applyDetailConfig();
        });
        grid->addWidget(lbl, z - 1, 0);
        grid->addWidget(combo, z - 1, 1);
    }
    ml0->addLayout(grid);

    // Metal routing.
    auto* mHdr = new QLabel(QStringLiteral("Metalness routing"), manual);
    mHdr->setStyleSheet(QStringLiteral("color:#9ad; margin-top:6px;"));
    ml0->addWidget(mHdr);
    auto* mRow = new QHBoxLayout();
    mRow->addWidget(new QLabel(QStringLiteral("Metal uses"), manual));
    auto* mCombo = new QComboBox(manual);
    mCombo->addItem(QStringLiteral("auto (by name)"), -2);
    mCombo->addItem(QStringLiteral("off"), -1);
    mCombo->addItem(QStringLiteral("map 0"), 0);
    mCombo->addItem(QStringLiteral("map 1"), 1);
    mCombo->addItem(QStringLiteral("map 2"), 2);
    { const int idx = mCombo->findData(cur.metalRoute); mCombo->setCurrentIndex(idx >= 0 ? idx : 0); }
    connect(mCombo, &QComboBox::currentIndexChanged, this, [this, mCombo, setD](int) {
        setD(QStringLiteral("metalRoute"), mCombo->currentData().toInt()); applyDetailConfig();
    });
    mRow->addWidget(mCombo, 1);
    ml0->addLayout(mRow);

    // Sliders: metalness threshold + the four detail-band centres.
    auto slider = [&](const QString& key, const QString& label, int lo, int hi, int init, double scale) {
        auto* row = new QHBoxLayout();
        auto* lbl = new QLabel(label, manual); lbl->setMinimumWidth(78); row->addWidget(lbl);
        auto* sl = new QSlider(Qt::Horizontal, manual); sl->setRange(lo, hi); sl->setValue(init);
        auto* val = new QLabel(QString::number(init * scale, 'f', 3), manual);
        val->setMinimumWidth(38); val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        connect(sl, &QSlider::valueChanged, this, [this, key, scale, val, setD](int v) {
            setD(key, v * scale); val->setText(QString::number(v * scale, 'f', 3)); applyDetailConfig(); });
        row->addWidget(sl, 1); row->addWidget(val);
        ml0->addLayout(row);
    };
    auto* tHdr = new QLabel(QStringLiteral("Thresholds"), manual);
    tHdr->setStyleSheet(QStringLiteral("color:#9ad; margin-top:6px;"));
    ml0->addWidget(tHdr);
    slider(QStringLiteral("metalThresh"), QStringLiteral("Metal ≥"), 0, 100, int(cur.metalThresh * 100 + 0.5), 0.01);
    for (int i = 0; i < 4; ++i)
        slider(QStringLiteral("band%1").arg(i), QStringLiteral("Band %1").arg(i), 0, 1000,
               int(cur.bands[i] * 1000 + 0.5), 0.001);

    auto* btnRow = new QHBoxLayout();
    auto* dBtn = new QPushButton(QStringLiteral("Reset to game default"), m_detailPanel);
    auto* cBtn = new QPushButton(QStringLiteral("Copy config"), m_detailPanel);
    connect(dBtn, &QPushButton::clicked, this, [this] {
        QSettings q; const QString p = QStringLiteral("stable2/detail/");
        for (const QString& k : q.allKeys()) if (k.startsWith(p)) q.remove(k);
        applyDetailConfig();
        if (m_detailPanel) { m_detailPanel->hide(); m_detailPanel->deleteLater(); m_detailPanel = nullptr; }
    });
    connect(cBtn, &QPushButton::clicked, this, [] {
        QGuiApplication::clipboard()->setText(stableDetailCfgText());
    });
    btnRow->addWidget(dBtn); btnRow->addWidget(cBtn);
    pl->addLayout(btnRow);
}

void StableTab2::applyDetailConfig()
{
    if (m_view) m_view->setDetailConfig(stableDetailCfg());
}

// ── Physics popover (Wardrobe port): live cloth-sim tuning for mane/tail/cloth. Persisted under
// stable2/cloth/*; applyClothParams pushes it to the viewport. (Phys-bone/axis overlays live in
// the Overlays popup now, so they're not duplicated here.) ────────────────────────────────────
void StableTab2::buildPhysicsPanel()
{
    if (m_physPanel) return;
    m_physPanel = new QFrame(this, Qt::Popup);
    m_physPanel->setObjectName(QStringLiteral("stablePhysPanel"));
    m_physPanel->setStyleSheet(QStringLiteral(
        "QFrame#stablePhysPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
        "QLabel{color:#cccccc;}"));
    auto* pl = new QVBoxLayout(m_physPanel);
    pl->setContentsMargins(12, 10, 12, 10);
    pl->setSpacing(4);
    auto* hdr = new QLabel(QStringLiteral("Cloth physics (live)"), m_physPanel);
    hdr->setStyleSheet(QLatin1String(kHdrQss));
    pl->addWidget(hdr);
    GLModelWidget::ClothParams d;

    auto* enablePhys = new QCheckBox(QStringLiteral("Enable physics"), m_physPanel);
    enablePhys->setStyleSheet(QStringLiteral("QCheckBox{color:#fff;font-weight:bold;}"));
    enablePhys->setToolTip(QStringLiteral("Master switch for the mane/tail/cloth simulation."));
    enablePhys->setChecked(QSettings().value(QStringLiteral("stable2/cloth/enabled"), true).toBool());
    connect(enablePhys, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("stable2/cloth/enabled"), on);
        if (m_view) m_view->setClothEnabled(on);
    });
    pl->addWidget(enablePhys);

    // Each slider resets in place (no menu close) → collect a resetter per row.
    auto* resetters = new QVector<std::function<void()>>();
    m_physPanel->connect(m_physPanel, &QObject::destroyed, [resetters] { delete resetters; });
    struct SliderRef { QSlider* sld; QString key; double scale; };
    auto* sliderRefs = new QVector<SliderRef>();
    m_physPanel->connect(m_physPanel, &QObject::destroyed, [sliderRefs] { delete sliderRefs; });

    auto section = [&](const QString& t) {
        auto* sc = new QLabel(t, m_physPanel);
        sc->setStyleSheet(QStringLiteral("color:#8ab4f8;font-weight:bold;margin-top:6px;"));
        pl->addWidget(sc);
    };
    auto row = [&](const QString& key, const QString& label, int lo, int hi, double scale,
                   double def, const QString& tip) {
        auto* rl = new QHBoxLayout();
        auto* name = new QLabel(label, m_physPanel); name->setFixedWidth(108);
        name->setToolTip(tip);
        auto* sld = new QSlider(Qt::Horizontal, m_physPanel);
        sld->setRange(lo, hi);
        sld->setToolTip(tip);
        auto* val = new QLabel(m_physPanel); val->setFixedWidth(56);
        val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        const double curv = QSettings().value(QStringLiteral("stable2/cloth/") + key, def).toDouble();
        sld->setValue(int(qRound(curv * scale)));
        val->setText(QString::number(curv, 'g', 3));
        connect(sld, &QSlider::valueChanged, this, [this, key, val, scale](int v) {
            const double fv = v / scale;
            QSettings().setValue(QStringLiteral("stable2/cloth/") + key, fv);
            val->setText(QString::number(fv, 'g', 3));
            applyClothParams();
        });
        rl->addWidget(name); rl->addWidget(sld, 1); rl->addWidget(val);
        pl->addLayout(rl);
        resetters->append([sld, scale, def] { sld->setValue(int(qRound(def * scale))); });
        sliderRefs->append(SliderRef{ sld, key, scale });
    };

    section(QStringLiteral("Tracking & motion"));
    row(QStringLiteral("tracking"), QStringLiteral("Bone tracking"), 0, 100, 100.0, d.boneTracking,
        QStringLiteral("How strongly the cloth follows its authored bone pose each frame. Higher = tighter."));
    row(QStringLiteral("maxdist"), QStringLiteral("Max distance"), 0, 1000, 1000.0, d.maxDistance,
        QStringLiteral("Swing reach: scales the authored per-bone motion constraint."));
    row(QStringLiteral("damping"), QStringLiteral("Damping"), 800, 999, 1000.0, d.damping,
        QStringLiteral("Velocity retention per frame. Lower settles faster (stiffer)."));
    row(QStringLiteral("gravity"), QStringLiteral("Gravity"), 0, 400, 10000.0, -d.gravity,
        QStringLiteral("Downward pull. Higher droops more."));

    section(QStringLiteral("Stiffness"));
    row(QStringLiteral("bonestiff"), QStringLiteral("Bone stiffness"), 0, 200, 1000.0, d.boneStiffness,
        QStringLiteral("How strongly the cloth bones return to their authored shape."));
    row(QStringLiteral("stretch"), QStringLiteral("Stretch stiff"), 0, 100, 100.0, d.stretchStiffness,
        QStringLiteral("Structural tightness — resistance to stretching."));
    row(QStringLiteral("bend"), QStringLiteral("Bend stiff"), 0, 100, 100.0, d.bendStiffness,
        QStringLiteral("Resistance to folding/creasing."));

    section(QStringLiteral("Aerodynamics"));
    row(QStringLiteral("drag"), QStringLiteral("Drag"), 0, 100, 100.0, 0.0,
        QStringLiteral("Air resistance — settles billowing faster."));

    section(QStringLiteral("Collision"));
    auto* showCol = new QCheckBox(QStringLiteral("Show collision models"), m_physPanel);
    showCol->setStyleSheet(QStringLiteral("QCheckBox{color:#ccc;}"));
    showCol->setToolTip(QStringLiteral("Draw the authored collision capsules the cloth collides against."));
    showCol->setChecked(QSettings().value(QStringLiteral("stable2/cloth/showColliders"), false).toBool());
    connect(showCol, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("stable2/cloth/showColliders"), on);
        if (m_overlaysOn && m_view) m_view->setShowColliders(on);   // obey the overlay master gate
    });
    pl->addWidget(showCol);
    m_physChkColliders = showCol;
    linkColliderToggles();   // keep this box and the Overlays one in lockstep (same setting)

    auto* axisBtn = new QPushButton(m_physPanel);
    axisBtn->setStyleSheet(QStringLiteral("QPushButton{color:#ccc;text-align:left;padding:2px 6px;}"));
    axisBtn->setToolTip(QStringLiteral("Cycle the authored capsule's long axis (X/Y/Z) if it looks wrong."));
    auto setAxisLabel = [axisBtn](int a) {
        static const char* const kAxisName[4] = { "X", "Y", "Z", "bone" };
        axisBtn->setText(QStringLiteral("Capsule axis: %1  (click to cycle)").arg(QLatin1String(kAxisName[a & 3])));
    };
    setAxisLabel(QSettings().value(QStringLiteral("stable2/cloth/capAxis"), 3).toInt());
    connect(axisBtn, &QPushButton::clicked, this, [this, setAxisLabel] {
        int a = (QSettings().value(QStringLiteral("stable2/cloth/capAxis"), 3).toInt() + 1) & 3;
        QSettings().setValue(QStringLiteral("stable2/cloth/capAxis"), a);
        setAxisLabel(a);
        if (m_view) m_view->setCapsuleAxis(a);
    });
    pl->addWidget(axisBtn);
    row(QStringLiteral("capScale"), QStringLiteral("Capsule size"), 20, 220, 100.0, d.capsuleRadius,
        QStringLiteral("Scales ALL body-collision capsules (authored + fitted). ~0.52 (default) matches "
                       "the body mesh — the authored radii are larger than the visible body, so 1.0 "
                       "inflates it and splays garments open. Raise "
                       "to push cloth further off the body."));

    // Per-region capsule trim (see ClothParams::capRegion). The game authors a radius PER CAPSULE
    // PER BONE, so a skirt clipping the thighs is a LEGS problem; the global knob above also
    // inflates chest and arms, which is why tuning it alone never lands. 1.0 = authored size.
    row(QStringLiteral("capLegs"),  QStringLiteral("  · Legs"),  20, 300, 100.0, d.capRegion[0],
        QStringLiteral("Thigh / shin / ankle / foot capsules only. Raise to stop a skirt or hem "
                       "clipping through the legs without inflating the torso."));
    row(QStringLiteral("capWaist"), QStringLiteral("  · Waist"), 20, 300, 100.0, d.capRegion[1],
        QStringLiteral("Pelvis capsules only — where most skirts and loincloths anchor."));
    row(QStringLiteral("capTorso"), QStringLiteral("  · Torso"), 20, 300, 100.0, d.capRegion[2],
        QStringLiteral("Chest / centre capsules only — capes and tabards ride on these."));
    row(QStringLiteral("capArms"),  QStringLiteral("  · Arms"),  20, 300, 100.0, d.capRegion[3],
        QStringLiteral("Upper arm / forearm / hand capsules only."));
    row(QStringLiteral("capHead"),  QStringLiteral("  · Head"),  20, 300, 100.0, d.capRegion[4],
        QStringLiteral("Head capsules only — hoods, hair and feathers."));
    row(QStringLiteral("capOther"), QStringLiteral("  · Other"), 20, 300, 100.0, d.capRegion[5],
        QStringLiteral("Capsules on bones outside the shared player rig (mounts, monsters, props)."));
    row(QStringLiteral("margin"), QStringLiteral("Collide margin"), 0, 50, 1000.0, d.collisionMargin,
        QStringLiteral("Extra clearance kept from the body capsules."));
    row(QStringLiteral("friction"), QStringLiteral("Friction"), 0, 100, 100.0, d.friction,
        QStringLiteral("Grip at body contact."));
    row(QStringLiteral("backstop"), QStringLiteral("Backstop"), 0, 80, 1000.0, d.backstop,
        QStringLiteral("How far the cloth may sink toward the body. 0 disables it."));
    row(QStringLiteral("self"), QStringLiteral("Self-collide"), 0, 60, 1000.0, d.selfCollision,
        QStringLiteral("Cloth thickness for self-collision. 0 disables it."));

    section(QStringLiteral("Interaction"));
    {
        // Same feature as the Wardrobe / Models panels: rotating the view feeds inertia into the
        // mane/tail and any barding cloth, so spinning the mount swings it instead of leaving it
        // rigid. (Section order matches the other tabs: … Collision · Interaction · Solver.)
        auto* spinChk = new QCheckBox(QStringLiteral("React to rotation"), m_physPanel);
        spinChk->setStyleSheet(QStringLiteral("QCheckBox{color:#ccc;}"));
        spinChk->setToolTip(QStringLiteral(
            "Rotating the view swings the mane/tail/cloth: simulated parts lag behind as the turn "
            "starts and stops (momentum) and fan outward while it continues (centrifugal)."));
        spinChk->setChecked(QSettings().value(QStringLiteral("stable2/cloth/userSpin"), false).toBool());
        connect(spinChk, &QCheckBox::toggled, this, [this, enablePhys](bool on) {
            QSettings().setValue(QStringLiteral("stable2/cloth/userSpin"), on);
            if (on && !enablePhys->isChecked()) enablePhys->setChecked(true);   // can't do nothing
            applyClothParams();
        });
        pl->addWidget(spinChk);
    }
    row(QStringLiteral("spinForce"), QStringLiteral("Rotation force"), 0, 500, 100.0, d.userSpinForce,
        QStringLiteral("How strongly view rotation pushes the cloth (needs 'React to rotation'). "
                       "0 = none · 0.1 = subtle (default) · higher = exaggerated swing."));

    section(QStringLiteral("Solver"));
    row(QStringLiteral("substeps"), QStringLiteral("Sub-steps"), 1, 4, 1.0, d.subSteps,
        QStringLiteral("Physics passes per frame. More = steadier under fast motion and much less "
                       "clipping; costs CPU in proportion. 2 is a good balance."));
    row(QStringLiteral("iters"), QStringLiteral("Iterations"), 1, 20, 1.0, d.iterations,
        QStringLiteral("Constraint solver passes per frame. More = stiffer/more stable, costs CPU."));

    auto* reset = new QPushButton(QStringLiteral("Reset to defaults"), m_physPanel);
    connect(reset, &QPushButton::clicked, this, [resetters] {
        for (const auto& r : *resetters) r();
    });
    pl->addWidget(reset);

    auto* presetRow = new QHBoxLayout();
    auto* saveBtn = new QPushButton(QStringLiteral("Save preset"), m_physPanel);
    auto* restoreBtn = new QPushButton(QStringLiteral("Restore preset"), m_physPanel);
    restoreBtn->setEnabled(QSettings().value(QStringLiteral("stable2/cloth/preset/exists"), false).toBool());
    connect(saveBtn, &QPushButton::clicked, this, [sliderRefs, restoreBtn] {
        QSettings s;
        for (const SliderRef& r : *sliderRefs)
            s.setValue(QStringLiteral("stable2/cloth/preset/") + r.key,
                       s.value(QStringLiteral("stable2/cloth/") + r.key));
        s.setValue(QStringLiteral("stable2/cloth/preset/exists"), true);
        restoreBtn->setEnabled(true);
    });
    connect(restoreBtn, &QPushButton::clicked, this, [this, sliderRefs] {
        QSettings s;
        if (!s.value(QStringLiteral("stable2/cloth/preset/exists"), false).toBool()) return;
        for (const SliderRef& r : *sliderRefs) {
            const QString pk = QStringLiteral("stable2/cloth/preset/") + r.key;
            if (!s.contains(pk)) continue;
            r.sld->setValue(int(qRound(s.value(pk).toDouble() * r.scale)));
        }
    });
    presetRow->addWidget(saveBtn); presetRow->addWidget(restoreBtn);
    pl->addLayout(presetRow);
}

void StableTab2::applyClothParams()
{
    if (!m_view) return;
    QSettings s;
    GLModelWidget::ClothParams d;
    // Undo the v2 capsule-size migration (see WardrobeTab2::applyClothParams).
    if (!s.value(QStringLiteral("cloth/capsuleFix_v3"), false).toBool()) {
        s.setValue(QStringLiteral("cloth/capsuleFix_v3"), true);
        if (s.value(QStringLiteral("cloth/capsuleFix_v2"), false).toBool())
            for (const char* k : {"wardrobe2/cloth/capScale", "models/cloth/capScale", "stable2/cloth/capScale"})
                if (qFuzzyCompare(s.value(QLatin1String(k), 0.52).toDouble(), 1.0))
                    s.setValue(QLatin1String(k), double(d.capsuleRadius));
    }
    auto f = [&](const QString& k, double def) {
        return float(s.value(QStringLiteral("stable2/cloth/") + k, def).toDouble()); };
    GLModelWidget::ClothParams p;
    p.gravity          = -f(QStringLiteral("gravity"), -d.gravity);   // stored as positive magnitude
    p.damping          = f(QStringLiteral("damping"),  d.damping);
    p.maxDistance      = f(QStringLiteral("maxdist"),  d.maxDistance);
    p.bendStiffness    = f(QStringLiteral("bend"),     d.bendStiffness);
    p.stretchStiffness = f(QStringLiteral("stretch"),  d.stretchStiffness);
    p.iterations       = s.value(QStringLiteral("stable2/cloth/iters"), d.iterations).toInt();
    p.subSteps         = s.value(QStringLiteral("stable2/cloth/substeps"), d.subSteps).toInt();
    p.selfCollision    = f(QStringLiteral("self"),     d.selfCollision);
    p.collisionMargin  = f(QStringLiteral("margin"),   d.collisionMargin);
    p.friction         = f(QStringLiteral("friction"), d.friction);
    p.backstop         = f(QStringLiteral("backstop"), d.backstop);
    p.capsuleRadius    = f(QStringLiteral("capScale"), d.capsuleRadius);   // one knob for all capsules
    p.capRegion[0]     = f(QStringLiteral("capLegs"),  d.capRegion[0]);
    p.capRegion[1]     = f(QStringLiteral("capWaist"), d.capRegion[1]);
    p.capRegion[2]     = f(QStringLiteral("capTorso"), d.capRegion[2]);
    p.capRegion[3]     = f(QStringLiteral("capArms"),  d.capRegion[3]);
    p.capRegion[4]     = f(QStringLiteral("capHead"),  d.capRegion[4]);
    p.capRegion[5]     = f(QStringLiteral("capOther"), d.capRegion[5]);
    p.boneTracking     = f(QStringLiteral("tracking"), d.boneTracking);
    p.dragFactor       = f(QStringLiteral("drag"), 0.0);
    p.boneStiffness    = f(QStringLiteral("bonestiff"), d.boneStiffness);
    // "React to rotation": orbit-driven inertia (see ClothParams::userSpin).
    p.userSpin         = s.value(QStringLiteral("stable2/cloth/userSpin"), false).toBool();
    p.userSpinForce    = f(QStringLiteral("spinForce"), d.userSpinForce);
    // Master switch, converged the same way as the other tabs (see WardrobeTab2::applyClothParams).
    bool clothOn = s.value(QStringLiteral("stable2/cloth/enabled"), true).toBool();
    // NOTE: "React to rotation" no longer force-enables cloth HERE. This runs on every apply — and
    // on every model load — so with userSpin on, an explicit decision to switch physics OFF was
    // silently reverted (and persisted) each time, e.g. loading a new hair model brought physics
    // back until the master toggle was cycled. The convergence now happens once, in the userSpin
    // toggle handler, which is the moment the user actually asks for rotation-driven cloth.
    m_view->setClothEnabled(clothOn);
    m_view->setCapsuleAxis(s.value(QStringLiteral("stable2/cloth/capAxis"), 3).toInt());
    m_view->setClothParams(p);
    // MASTER GATE: this runs on every physics-panel edit, so replaying the saved flag ungated
    // switched the collider overlay back on behind the master toggle.
    m_view->setShowColliders(m_overlaysOn && s.value(QStringLiteral("stable2/cloth/showColliders"), false).toBool());
}

// Single place that pushes overlay state to the viewport: master gate AND each box's own state.
// Anything needing overlays refreshed calls THIS — never setShow*() directly, or the master gate
// gets bypassed.
// ONE part menu, shown from BOTH the 3D viewport and the PARTS PANEL.
// "Why does this part look the way it does" for one mount part, in the same read-only pane the
// Wardrobe uses. Pure: it decodes no pixels and writes nothing, so it is safe on any part however
// broken — which is the point, since the parts worth asking about are the broken ones.
void StableTab2::showMaterialReport(const QString& materialName, const QString& apprName,
                                    int apprSno)
{
    TextReport::show(this,
                     materialName.isEmpty()
                         ? QStringLiteral("Explain materials — %1")
                               .arg(apprName.isEmpty() ? QStringLiteral("piece") : apprName)
                         : QStringLiteral("Explain material — %1").arg(materialName),
                     MaterialReport::explain(m_reader, m_index, Config::d4dataDir(),
                                             apprName, apprSno, materialName));
}

void StableTab2::showPartContextMenu(int part, const QPoint& gp, int groupPart)
{
        if (!m_partTree) return;
        auto setAll = [this](Qt::CheckState st) {
            for (int r = 0; r < m_partTree->topLevelItemCount(); ++r) {
                QTreeWidgetItem* root = m_partTree->topLevelItem(r);
                for (int c = 0; c < root->childCount(); ++c) root->child(c)->setCheckState(0, st);
            }
        };

        // What this menu ACTS ON — see the Wardrobe's copy for the reasoning. Right-clicking
        // inside the selection acts on all of it; outside replaces it. The set is both what runs
        // and what turns blue.
        QList<int> sel = selectedParts();
        QList<int> acted;
        if (part >= 0) {
            if (sel.contains(part)) {
                acted = sel;
            } else {
                acted = QList<int>{part};
                m_partTree->clearSelection();
                if (QTreeWidgetItem* it = itemForPart(part)) {
                    if (it->parent()) it->parent()->setExpanded(true);
                    it->setSelected(true);
                }
            }
        }
        std::sort(acted.begin(), acted.end());
        acted.erase(std::unique(acted.begin(), acted.end()), acted.end());
        // Spanning more than one equipped piece? Then naming one of them in the title is the same
        // under-reporting the count exists to fix — see the Wardrobe's copy.
        bool oneSource = true;
        for (int p : acted)
            if (m_partSource.value(p) != m_partSource.value(acted.first())) { oneSource = false; break; }
        // Right-click SELECTS (blue outline); the camera only moves via "Frame part".
        if (m_view) m_view->setPickedParts(acted);
        ViewportPartMenu::Info in;
        ViewportPartMenu::Actions act;
        QTreeWidgetItem* item = nullptr;
        // "Model" scope = the equipped appearance the picked part belongs to (empty space → all).
        const int srcPart = (part >= 0) ? part : groupPart;   // group header → that group's item
        const QVector<int> modelParts = partsOfSource(srcPart);
        int modelTris = 0;
        if (m_view) {
            if (modelParts.isEmpty())
                for (int i = 0; i < m_lastGeo.primitives.size(); ++i) modelTris += m_view->partTriangles(i);
            else
                for (int i : modelParts) modelTris += m_view->partTriangles(i);
        }
        in.sourceModel   = (srcPart >= 0 && srcPart < m_partSource.size() && !m_partSource[srcPart].isEmpty())
                             ? m_partSource[srcPart] : QStringLiteral("Mount");
        if (acted.size() > 1 && !oneSource) in.sourceModel.clear();
        in.modelTris     = modelTris;
        in.lastExportDir = QSettings().value(QStringLiteral("stable2/exportDir")).toString();
        // Keyed on srcPart, not part — same reason as the Wardrobe: srcPart resolves a group-header
        // right-click to that group's equipped piece, so the Copy section works there too.
        if (srcPart >= 0) {
            in.sourceFileName = m_partSource.value(srcPart);   // the equipped piece this part came from
            in.sourceName     = m_partSource.value(srcPart);
            in.sno            = m_partSourceSno.value(srcPart, 0);
            in.collection     = AppearanceMeta::instance().collectionFor(in.sno);
        }
        if (part >= 0 && part < m_lastGeo.primitives.size()) {
            item = itemForPart(part);
            in.part         = part;
            // As in the Wardrobe: the merge pipeline puts the real material name on the primitive.
            in.partName     = m_lastGeo.primitives[part].materialName;
            in.partMaterial = in.partName;
            in.partOwner    = m_partSource.value(part);      // parts-panel PARENT row (see Wardrobe)
            in.partTris     = m_view ? m_view->partTriangles(part) : 0;
            in.visible      = !item || item->checkState(0) == Qt::Checked;
            in.isSim        = part < m_partSim.size() && m_partSim[part];
            in.isFx         = part < m_partFx.size()  && m_partFx[part];
            // Per PART, and per the equipped piece that part came from — the merged mount spans up
            // to three appearances, so the report has to be told which one it is being asked about.
            {
                const QString mn = in.partName;
                const QString an = m_partSource.value(part);
                const int     as = m_partSourceSno.value(part, 0);
                act.explainMaterial = [this, mn, an, as] { showMaterialReport(mn, an, as); };
            }
            // Everything below runs on `acted`, which for a single pick is exactly {part}.
            const QVector<int> actedV(acted.begin(), acted.end());
            if (acted.size() > 1) {
                in.selParts = actedV;
                int t = 0;
                QStringList mats;
                for (int p : acted) {
                    t += m_view ? m_view->partTriangles(p) : 0;
                    const QString mn = (p < m_lastGeo.primitives.size())
                                           ? m_lastGeo.primitives[p].materialName : QString();
                    if (!mn.isEmpty() && !mats.contains(mn)) mats << mn;
                }
                in.selTris = t;
                in.selMaterials = mats;
            }
            // Signals blocked, then ONE recompute — see the Wardrobe's copy.
            act.setVisible  = [this, acted](bool on) {
                const bool was = m_partTree->blockSignals(true);
                for (int p : acted)
                    if (QTreeWidgetItem* it = itemForPart(p))
                        it->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
                m_partTree->blockSignals(was);
                recomputePartVisibility();
            };
            act.isolate     = [this, setAll, acted] {
                const bool was = m_partTree->blockSignals(true);
                setAll(Qt::Unchecked);
                for (int p : acted)
                    if (QTreeWidgetItem* it = itemForPart(p)) it->setCheckState(0, Qt::Checked);
                m_partTree->blockSignals(was);
                recomputePartVisibility();
            };
            act.selectPart  = [this, acted] {
                if (!m_partTree) return;
                m_partTree->clearSelection();
                QTreeWidgetItem* first = nullptr;
                for (int p : acted)
                    if (QTreeWidgetItem* it = itemForPart(p)) {
                        if (it->parent()) it->parent()->setExpanded(true);
                        it->setSelected(true);
                        if (!first) first = it;
                    }
                if (first) { m_partTree->setCurrentItem(first); m_partTree->scrollToItem(first); }
            };
            act.frame       = [this, actedV] {
                if (!m_view) return;
                QVector3D c; float r;
                if (m_view->partsBounds(actedV, c, r))
                    m_view->frameRegionKeepRotation(c, r, /*animate=*/true);
            };
            const QString pn = acted.size() > 1
                ? (in.sourceModel.isEmpty() ? QStringLiteral("parts") : in.sourceModel)
                : (in.partName.isEmpty() ? QStringLiteral("part") : in.partName);
            act.exportPart        = [this, actedV, pn] { exportMount(actedV, pn, false); };
            act.exportPartLastDir = [this, actedV, pn] { exportMount(actedV, pn, true); };
        }
        if (modelParts.isEmpty()) {                       // empty space → the whole assembled mount
            act.exportModel        = [this] { exportMount(); };
            act.exportModelLastDir = [this] { exportMount(QVector<int>(), QString(), true); };
        } else {                                          // a part was picked → just its source item
            const QString sn = in.sourceModel;
            act.exportModel        = [this, modelParts, sn] { exportMount(modelParts, sn, false); };
            act.exportModelLastDir = [this, modelParts, sn] { exportMount(modelParts, sn, true); };
        }
        act.showAll = [setAll] { setAll(Qt::Checked); };
        act.hideAll = [setAll] { setAll(Qt::Unchecked); };
        act.invert  = [this] {
            for (int r = 0; r < m_partTree->topLevelItemCount(); ++r) {
                QTreeWidgetItem* root = m_partTree->topLevelItem(r);
                for (int c = 0; c < root->childCount(); ++c) {
                    QTreeWidgetItem* it = root->child(c);
                    it->setCheckState(0, it->checkState(0) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
                }
            }
        };
        // The right-click outline is transient: it marks what the menu acts on and must go
        // when the menu does. Persistent selection is the tree/highlight, not this.
        act.closed = [this] { if (m_view) m_view->setPickedPart(-1); };
        ViewportPartMenu::exec(this, gp, in, act);
}


void StableTab2::reapplyOverlays()
{
    if (!m_view) return;
    for (const auto& e : m_overlayChks)
        if (e.first) e.second(m_overlaysOn && e.first->isChecked());
    applyClothParams();   // cloth/collider flags share the same gate
}

// The collision-model state is reachable from two places (Overlays panel and Physics panel).
// They share one setting, so mirror their check states once both boxes exist — whichever is
// constructed second completes the link.
void StableTab2::linkColliderToggles()
{
    if (!m_ovlChkColliders || !m_physChkColliders || m_colliderTogglesLinked) return;
    m_colliderTogglesLinked = true;
    connect(m_ovlChkColliders,  &QCheckBox::toggled, m_physChkColliders, &QCheckBox::setChecked);
    connect(m_physChkColliders, &QCheckBox::toggled, m_ovlChkColliders,  &QCheckBox::setChecked);
}

// ── Sidebar collapse: hide the whole right pane, N-strip » ↔ « toggle. ───────────────────────
void StableTab2::setSideCollapsed(bool on)
{
    m_sideCollapsed = on;
    QSettings().setValue(QStringLiteral("stable2/sideCollapsed"), on);
    if (m_sideArrow) {
        m_sideArrow->setText(on ? QStringLiteral("«") : QStringLiteral("»"));
        m_sideArrow->setToolTip(on ? QStringLiteral("Show the right panels")
                                   : QStringLiteral("Hide the right panels"));
    }
    if (m_sidebarW && !m_fullscreen) m_sidebarW->setVisible(!on);
}

// ── Undo of slot/look changes (Ctrl+Z) ───────────────────────────────────────────────────────
void StableTab2::pushUndo()
{
    if (m_restoring) return;
    Snapshot s;
    for (int i = 0; i < SlotCount; ++i) {
        s.sel[i] = m_slotSel[i]; s.name[i] = m_slotName[i];
        s.disp[i] = m_slotDisp[i]; s.desc[i] = m_slotDesc[i]; s.look[i] = m_slotLook[i];
    }
    s.mountType = m_mountType;
    m_undo.append(s);
    if (m_undo.size() > 40) m_undo.remove(0);
}

void StableTab2::undo()
{
    if (m_undo.isEmpty()) { if (m_status) m_status->setText(QStringLiteral("Nothing to undo.")); return; }
    const Snapshot s = m_undo.takeLast();
    m_restoring = true;
    for (int i = 0; i < SlotCount; ++i) {
        m_slotSel[i] = s.sel[i]; m_slotName[i] = s.name[i];
        m_slotDisp[i] = s.disp[i]; m_slotDesc[i] = s.desc[i]; m_slotLook[i] = s.look[i];
    }
    m_mountType = s.mountType;
    refreshSlotCells();
    fillGrid();
    rebuildMount();
    m_restoring = false;
}

// ── Export ──────────────────────────────────────────────────────────────────────
// All merged parts belonging to the same equipped appearance as `part` — the "model" that owns it
// (mount, barding or trophy), not the whole assembled mount.
QVector<int> StableTab2::partsOfSource(int part) const
{
    QVector<int> out;
    if (part < 0 || part >= m_partSourceSlot.size()) return out;
    const int sl = m_partSourceSlot[part];
    for (int i = 0; i < m_partSourceSlot.size(); ++i)
        if (m_partSourceSlot[i] == sl) out << i;
    return out;
}

// `keep` selects which primitives to write; empty = the whole mount. The subset path backs the
// context menu's "Export Part" and its source-scoped "Export Model" — those used to isolate parts
// in the tree and call this function, but the exporter reads m_lastGeo wholesale and never
// consulted the tree, so it silently wrote the entire mount instead.
//
// SCOPE RULE, shared with WardrobeTab2: an explicit `keep` is the user pointing at something in the
// viewport, so it is written verbatim, hidden parts included. Only the no-subset path is the
// "export what you see" scope that filters on partVisible().
//
// `toLast` skips the file dialog and reuses the remembered folder.
void StableTab2::exportMount(const QVector<int>& keep, const QString& label, bool toLast)
{
    if (!m_lastGeo.valid || m_lastGeo.primitives.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Export"), QStringLiteral("No mount to export.")); return;
    }
    // Resolved BEFORE the file dialog so an all-hidden mount fails before asking for a filename.
    QVector<int> keepEff = keep;
    if (keepEff.isEmpty() && m_view) {          // whole-mount export honours the parts tree, as
        const int n = m_lastGeo.primitives.size();   // Models tab does — unchecking a part in the
        // Settings ▸ Export ▸ "Export FX / simulation meshes" only WIDENS what the viewport shows:
        // FX and sim-cage parts are hidden by their own toolbar toggles, not by the user picking
        // parts, so re-including them overrides that specific reason for hiding and nothing else.
        // Stable read neither flag before, while the Settings tooltip claimed it covered this tab.
        const bool wantFx = QSettings().value(QStringLiteral("export/exportFxSim"), false).toBool();
        for (int i = 0; i < n; ++i) {                // panel now affects the .glb, not just the view
            if (m_view->partVisible(i)) { keepEff << i; continue; }
            if (wantFx && ((i < m_partFx.size() && m_partFx[i]) || (i < m_partSim.size() && m_partSim[i])))
                keepEff << i;
        }
        if (keepEff.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Export"),
                QStringLiteral("Every part is hidden — nothing to export."));
            return;
        }
    }
    QString base = label.isEmpty()
        ? (m_slotName[SlotMount].isEmpty() ? QStringLiteral("mount") : m_slotName[SlotMount])
        : label;
    base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]+")), QStringLiteral("_"));
    if (base.isEmpty()) base = QStringLiteral("mount");
    const QString dir = QSettings().value(QStringLiteral("stable2/exportDir"), QDir::homePath()).toString();
    QString path;
    if (toLast && !QSettings().value(QStringLiteral("stable2/exportDir")).toString().isEmpty()) {
        path = QDir(dir).filePath(base + QStringLiteral(".glb"));
    } else {
        path = QFileDialog::getSaveFileName(this, QStringLiteral("Export mount"),
                   dir + QStringLiteral("/") + base + QStringLiteral(".glb"),
                   QStringLiteral("glTF Binary (*.glb)"));
    }
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".glb"), Qt::CaseInsensitive)) path += QStringLiteral(".glb");
    // Animations per Settings ▸ Export — the same scope the export menu's suffix counts. The
    // WHOLE-mount export used to ship only the playing clip regardless of the setting, so a menu
    // reading "+ 24 animations" wrote one; the menu was not wrong, this path was.
    //
    // Scoped to the whole mount deliberately. Clips are skeleton-level, so nothing downstream
    // trims them to `keepEff` — attaching the full library to a single right-clicked strap would
    // turn a one-primitive export into a multi-megabyte one with a decode stall to match.
    QVector<AnimParser::DecodedAnim> anims;
    QStringList animNames;
    // NB: `keep`, not `keepEff` — keepEff is back-filled with every visible part for a whole-mount
    // export, so it is almost never empty and gating on it would silently disable animations
    // everywhere. The caller's own empty `keep` is what means "the whole mount".
    if (keep.isEmpty() && QSettings().value(QStringLiteral("export/includeAnim"), false).toBool())
        collectExportAnims(m_lastGeo,
                           exportClipNames(m_slotName[SlotMount], m_slotSel[SlotMount], petMode()),
                           anims, animNames);
    // Fall back to the live clip so "export what you are looking at" still holds with the setting
    // off, or when the scope resolved to nothing.
    if (anims.isEmpty() && m_curAnim.valid && !m_lastGeo.skeleton.isEmpty()) {
        anims << m_curAnim;
        animNames << (m_playingAnim.isEmpty() ? QStringLiteral("clip") : m_playingAnim);
    }
    const ModelExporter::Options opt = ModelExporter::optionsFromSettings();
    ModelGeometry geoCopy = m_lastGeo;   // copy so retarget/rename never touches the live preview
    QVector<ModelExporter::ExportMaterial> mats = m_exportMats;
    if (!keepEff.isEmpty()) {               // subset: primitives and materials stay index-aligned
        QVector<MeshPrimitive> sub;
        QVector<ModelExporter::ExportMaterial> subMats;
        for (int si : keepEff) {
            if (si < 0 || si >= m_lastGeo.primitives.size()) continue;
            sub << m_lastGeo.primitives[si];
            sub.last().materialIndex = sub.size() - 1;   // ModelExporter looks materials up BY
            subMats << m_exportMats.value(si);           // materialIndex, not by position: leaving
        }                                                // the full-mount index here drops textures
        if (sub.isEmpty()) return;
        geoCopy.primitives = sub;
        mats = subMats;
    }
    // Bake the tiled/zone-routed detail grain into the exported normal + ORM (opt-in), so barding
    // exports with its leather/metal surface texture instead of a smooth base normal. Same setting
    // and same guards the Wardrobe export uses; until now `export/bakeDetail` was read by Wardrobe
    // alone and this path silently ignored it.
    //
    // Here and not in the rebuild's decode pass, deliberately: the setting is read at EXPORT time,
    // so toggling it takes effect on the next export rather than the next mount load — and a mount
    // browsed with the option off costs nothing. `mats` is a copy; m_exportMats (which the parts
    // panel reads) is never touched.
    //
    // Cached by material NAME, which is safe here because every ExportMaterial with a given name
    // took its normal/ORM from the same per-material cache above — so one decode+bake per distinct
    // material, not one per primitive.
    // Settings ▸ Export ▸ "Include textures". Stable shipped m_exportMats wholesale and ignored it,
    // so unticking the box changed nothing here. Dropping the four images (rather than the whole
    // ExportMaterial) keeps names, double-sidedness, alpha mode and the scalar metal/rough/emissive
    // factors, which is what an untextured export is FOR — a correctly-shaded grey model, not an
    // unshaded one. Runs before the detail bake so a skipped texture is never decoded and baked.
    if (!QSettings().value(QStringLiteral("export/includeTex"), true).toBool())
        for (ModelExporter::ExportMaterial& em : mats) {
            em.baseColor = QImage(); em.normal = QImage();
            em.orm = QImage();       em.emissive = QImage();
        }
    else if (QSettings().value(QStringLiteral("export/bakeDetail"), false).toBool()) {
        const QString d4 = Config::d4dataDir();
        QHash<QString, QImage> bakedN, bakedO;   // two hashes rather than QPair: <QPair> is not included here
        for (ModelExporter::ExportMaterial& em : mats) {
            if (em.normal.isNull() || em.name.isEmpty()) continue;
            auto it = bakedN.constFind(em.name);
            if (it != bakedN.constEnd()) { em.normal = it.value(); em.orm = bakedO.value(em.name); continue; }
            MaterialDecode::bakeDetailForMaterial(m_reader, d4, em.name, em.normal, em.orm);
            bakedN.insert(em.name, em.normal);
            bakedO.insert(em.name, em.orm);
        }
    }
    // Hardpoint empties + bone naming + the post-rename re-index — see exportAppearanceModel. This
    // path had none of them, so a mount exported without its saddle/trophy sockets.
    if (QSettings().value(QStringLiteral("export/hardpointEmpties"), false).toBool())
        Hardpoints::readInto(geoCopy, apprJsonPath(Config::d4dataDir(), m_slotName[SlotMount]));
    Retarget::applyFromSettings(geoCopy);
    if (opt.blenderFriendly)
        GLModelWidget::blenderizeSkeletonNames(geoCopy.skeleton);
    else if (QSettings().value(QStringLiteral("export/boneNamesTranslated"), false).toBool())
        GLModelWidget::translateSkeletonNames(geoCopy.skeleton);
    Hardpoints::resolveBoneIndices(geoCopy);
    const bool ok = ModelExporter::exportGlb(geoCopy, path, mats, anims, animNames, opt);
    const QString folder = QFileInfo(path).absolutePath();
    QSettings().setValue(QStringLiteral("stable2/exportDir"), folder);
    if (ok)
        ExportNotifier::instance().notify(
            QStringLiteral("Exported %1%2%3").arg(QFileInfo(path).fileName(),
                anims.isEmpty() ? QString() : QStringLiteral("  (with %1 animation%2)").arg(animNames.size()).arg(animNames.size() == 1 ? QString() : QStringLiteral("s")),
                ExportNotifier::glbOptionsLine(opt)),
            folder);
    else
        QMessageBox::information(this, QStringLiteral("Export"), QStringLiteral("Export failed."));
}
