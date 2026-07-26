#include "tabs/StableTab2.h"
#include "util/ViewportPartMenu.h"

#include "app/ExportNotifier.h"
#include "util/HoverInfo.h"
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

    // Animation player.
    buildAnimPanel();
    if (m_animPanel) ll->addWidget(m_animPanel);

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
    connect(m_partTree, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (m_view) m_view->setHighlightParts(selectedParts());
        const QList<int> sel = selectedParts();
        updateTexTiles(sel.isEmpty() ? -1 : sel.first());   // fill the TEXTURE PREVIEW tiles
    });
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
        showPartContextMenu(idx, m_partTree->viewport()->mapToGlobal(pos));
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
        m_channelCombo->addItems({ QStringLiteral("Shaded"), QStringLiteral("Base Color"),
                                   QStringLiteral("Normal"), QStringLiteral("Roughness"),
                                   QStringLiteral("Metallic"), QStringLiteral("AO"),
                                   QStringLiteral("Emissive"), QStringLiteral("Detail maps"),
                                   QStringLiteral("Dye zones") });
        m_channelCombo->setToolTip(QStringLiteral("View the lit result or one raw material channel (↑/↓ to scroll)"));
        m_channelCombo->setCursor(Qt::PointingHandCursor);
        m_channelCombo->setStyleSheet(QStringLiteral(
            "QComboBox{padding:2px 8px;border:1px solid #555;border-radius:3px;background:#2b2b2b;color:#bbb;}"
            "QComboBox:hover{border-color:#b0453c;}"
            "QComboBox QAbstractItemView{background:#2b2b2b;color:#dddddd;"
            "selection-background-color:#8a1414;selection-color:#ffffff;}"));
        m_channelCombo->setCurrentIndex(QSettings().value(QStringLiteral("stable2/view/channel"), 0).toInt());
        connect(m_channelCombo, &QComboBox::currentIndexChanged, this, [this](int i) {
            QSettings().setValue(QStringLiteral("stable2/view/channel"), i);
            if (m_view) m_view->setViewChannel(i);
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
            const int i = m_channelCombo->currentIndex();
            m_shadeMoreBtn->setText(i == 0 ? QStringLiteral("⌄") : QStringLiteral("◆"));
            m_shadeMoreBtn->setToolTip(QStringLiteral(
                "Channel: %1\nScroll here to flip channels · click for the list").arg(m_channelCombo->currentText()));
        };
        syncChannelBtn();
        connect(m_channelCombo, &QComboBox::currentIndexChanged, this,
                [syncChannelBtn](int) { syncChannelBtn(); });
        QTimer::singleShot(0, this, [this] {   // apply the saved channel on load
            if (m_view) m_view->setViewChannel(m_channelCombo->currentIndex());
        });
    }
    sep();
    // ── Overlays: Blender's split control (wardrobe parity). A SPHERE toggle (master on/off for
    // every guide) + an ARROW opening a persistent QFrame popup (grid / axes / skeleton / physics
    // bones / per-bone axes / bone names). Keys under stable2/ovl/*. ──
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
        auto addOverlay = [&](const QString& label, const QString& key, bool def, bool indent,
                              const QString& tip, std::function<void(bool)> apply) {
            auto* cb = new QCheckBox(label, m_overlayPanel);
            if (indent) cb->setStyleSheet(QStringLiteral("QCheckBox{color:#cccccc;margin-left:16px;}"));
            if (!tip.isEmpty()) cb->setToolTip(tip);
            cb->setChecked(QSettings().value(QStringLiteral("stable2/ovl/") + key, def).toBool());
            connect(cb, &QCheckBox::toggled, this, [this, key, apply](bool on) {
                QSettings().setValue(QStringLiteral("stable2/ovl/") + key, on);
                if (m_overlaysOn) apply(on);
            });
            opl->addWidget(cb);
            m_overlayChks.append({ cb, apply });
            return cb;
        };
        ovSection(QStringLiteral("Guides"));
        addOverlay(QStringLiteral("Ground grid"), QStringLiteral("grid"), false, false,
                   QStringLiteral("Ground plane grid."),
                   [this](bool on) { if (m_view) m_view->setShowGrid(on); });
        addOverlay(QStringLiteral("Axis gizmo"), QStringLiteral("axis"), true, false,
                   QStringLiteral("Clickable X/Y/Z orientation ball in the viewport corner."),
                   [this](bool on) { if (m_view) m_view->setShowAxisGizmo(on); });
        addOverlay(QStringLiteral("Colored grid axes"), QStringLiteral("gridcolors"), true, true,
                   QStringLiteral("Tint the grid's world axes: X red, Z blue."),
                   [this](bool on) { if (m_view) m_view->setGridAxisColors(on); });
        ovSection(QStringLiteral("Skeleton"));
        addOverlay(QStringLiteral("Skeleton"), QStringLiteral("skel"), false, false,
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
        addOverlay(QStringLiteral("Physics bones"), QStringLiteral("phys"), false, false,
                   QStringLiteral("Overlay the cloth/physics bones (anchored grey, simulated orange)."),
                   [this](bool on) { if (m_view) m_view->setShowPhysBones(on); });
        addOverlay(QStringLiteral("Axis gizmos (per-bone)"), QStringLiteral("physaxes"), true, true,
                   QStringLiteral("Per-bone XYZ rotation gizmo (R/G/B)."),
                   [this](bool on) { if (m_view) m_view->setShowPhysAxes(on); });
        addOverlay(QStringLiteral("Hardpoints"), QStringLiteral("hardpoints"), false, false,
                   QStringLiteral("Draw the mount's attach sockets (saddle, HP_trophy1/2/3, reins…) as "
                                  "labeled XYZ gizmos — where the trophy and rider snap on."),
                   [this](bool on) { if (m_view) m_view->setShowHardpoints(on); });
        addOverlay(QStringLiteral("Bone names"), QStringLiteral("bnm"), false, false,
                   QStringLiteral("Label each bone at its position in the viewport."),
                   [this](bool on) { if (m_view) m_view->setShowBoneNames(on); });
        addOverlay(QStringLiteral("Translated names"), QStringLiteral("bnmtrans"), false, true,
                   QStringLiteral("Readable labels from verified D4 hardpoint/IK data; others keep bone_<hash>."),
                   [this](bool on) { if (m_view) m_view->setBoneNamesTranslated(on); });
        addOverlay(QStringLiteral("Hide unnamed bones"), QStringLiteral("bnmhide"), false, true,
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
    // Double-click a part in the viewport → select it in the PARTS tree (Blender-style), which
    // drives the highlight + TEXTURE PREVIEW. Same part again / empty space clears (part == -1).
    connect(m_view, &GLModelWidget::partFocused, this, [this](int part) {
        if (!m_partTree) return;
        QTreeWidgetItem* hit = nullptr;
        for (int r = 0; r < m_partTree->topLevelItemCount() && !hit; ++r) {
            QTreeWidgetItem* root = m_partTree->topLevelItem(r);
            for (int c = 0; c < root->childCount(); ++c)
                if (root->child(c)->data(0, Qt::UserRole).toInt() == part) { hit = root->child(c); break; }
        }
        const bool same = hit && hit->isSelected() && m_partTree->selectedItems().size() == 1;
        m_partTree->clearSelection();   // selectionChanged → highlight + tiles
        if (hit && !same) {
            if (hit->parent()) hit->parent()->setExpanded(true);
            hit->setSelected(true);
            m_partTree->scrollToItem(hit);
        }
    });
    // Right-click a part in the viewport → hide/show it + copy its material name.
    connect(m_view, &GLModelWidget::partRightClicked, this,
            [this](int part, const QPoint& gp) { showPartContextMenu(part, gp); });
    buildVpStrip();   // Reset · Camera · Lighting · Fullscreen pinned to the viewport edge

    left->setMinimumWidth(230);
    left->setMaximumWidth(500);   // a picker column, not a canvas — bounds the responsive card grid
    split->addWidget(left);
    split->addWidget(center);
    buildSidebar(split);   // wardrobe-parity right sidebar: PARTS · INFO PanelBoxes
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

// ── Right sidebar: PanelBox stack (PARTS · INFO), wardrobe-style — a vertical icon strip of
// checkable toggles beside a QSplitter of titled panels; shown/hidden state persists. ─────────
void StableTab2::buildSidebar(QSplitter* mainSplit)
{
    m_sidebarW = new QWidget;
    auto* sb = new QHBoxLayout(m_sidebarW);
    sb->setContentsMargins(2, 6, 4, 6);
    sb->setSpacing(3);
    auto* stripW = new QWidget(m_sidebarW);
    auto* stripLay = new QVBoxLayout(stripW);
    stripLay->setContentsMargins(0, 0, 0, 0);
    stripLay->setSpacing(3);
    m_rsplit = new QSplitter(Qt::Vertical, m_sidebarW);
    m_rsplit->setChildrenCollapsible(false);
    sb->addWidget(stripW);
    sb->addWidget(m_rsplit, 1);

    auto section = [&](const QString& title, QWidget* content, const QPixmap& icon,
                       const QString& tip, bool defOn) {
        const int page = m_rsections.size();
        auto* box = new PanelBox(title, content, m_rsplit);
        box->hide();
        box->up->hide(); box->down->hide();   // four panels — reorder buttons are noise
        m_rsplit->addWidget(box);
        m_rsections.append(box);
        m_rkeys.append(QStringLiteral("stable2/panel/") + title);
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
        // Restore the saved state (defaults keep both panels up like the old fixed layout).
        b->setChecked(QSettings().value(m_rkeys[page], defOn).toBool());
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
    section(QStringLiteral("PARTS"), m_partTree, K::kindIcon(K::Part),
            QStringLiteral("Parts — submesh visibility (uncheck to hide, hover to highlight)"), true);
    section(QStringLiteral("MATERIALS"), m_matTable, K::kindIcon(K::Material),
            QStringLiteral("Materials — one row per submesh (select to highlight)"), true);
    section(QStringLiteral("TEXTURES"), texW, K::kindIcon(K::TexGroup),
            QStringLiteral("Texture preview — PBR channels of the selected part"), false);
    section(QStringLiteral("INFO"), infoW, K::kindIcon(K::ValueGroup),
            QStringLiteral("Info — assembly stats for the current mount"), false);
    stripLay->addStretch(1);

    mainSplit->addWidget(m_sidebarW);
}

void StableTab2::showSidePanel(int page, bool on)
{
    if (page < 0 || page >= m_rsections.size()) return;
    PanelBox* box = m_rsections[page];
    const bool was = !box->isHidden();
    box->setVisible(on);
    if (on && !was) panelBoxArrive(m_rsplit, box);
    QSettings().setValue(m_rkeys[page], on);
    // The sidebar itself stays up even with every panel hidden — the icon strip must remain
    // reachable to bring panels back (fullscreen is what hides the whole pane).
}

void StableTab2::toggleFullscreen(bool on)
{
    m_fullscreen = on;
    if (m_mainSplit && m_mainSplit->count() > 0 && m_mainSplit->widget(0))
        m_mainSplit->widget(0)->setVisible(!on);   // left controls
    if (m_sidebarW) m_sidebarW->setVisible(!on && !m_sideCollapsed);   // honor an existing collapse
    if (m_toolbarW) m_toolbarW->setVisible(!on);   // toolbar row
    if (m_fsEsc) m_fsEsc->setEnabled(on);
    positionVpStrip();   // the viewport just changed size
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
    IconIndex::instance().ensureBuilt(d4);
    connect(&IconIndex::instance(), &IconIndex::readyChanged, this,
            [this] { refreshSlotCells(); fillGrid(); });
    connect(&AppearanceMeta::instance(), &AppearanceMeta::readyChanged, this,
            [this] { refreshSlotCells(); fillGrid(); });
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
        m_view->setEnvironment(QSettings().value(QStringLiteral("stable2/env"), 1).toInt());   // default Outdoor
        applyLightRig();
        applyGraphics();   // IBL/shadows/SSAO/tonemap/features from stable2/gfx/*
        applyFur();        // fur/mane shell + mesh-FX settings from stable2/fur/* · stable2/fx/*
        applyClothParams();// live cloth/mane sim from stable2/cloth/*
        applyDetailConfig();// detail-map selection from stable2/detail/*
        // Persisted overlay guides, gated by the master toggle (grid / axes / skeleton / bones / physics).
        QSettings ov;
        const bool om = m_overlaysOn;
        m_view->setShowGrid(om && ov.value(QStringLiteral("stable2/ovl/grid"), false).toBool());
        m_view->setShowAxisGizmo(om && ov.value(QStringLiteral("stable2/ovl/axis"), true).toBool());
        m_view->setGridAxisColors(om && ov.value(QStringLiteral("stable2/ovl/gridcolors"), true).toBool());
        m_view->setShowSkeleton(om && ov.value(QStringLiteral("stable2/ovl/skel"), false).toBool());
        m_view->setShowPhysBones(om && ov.value(QStringLiteral("stable2/ovl/phys"), false).toBool());
        m_view->setShowPhysAxes(om && ov.value(QStringLiteral("stable2/ovl/physaxes"), true).toBool());
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

    std::thread([this, d4, cacheBase, cachePath, kMagic, appByName, writeVec]() {
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
                if (!mm.hasMatch()) continue;
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
                if (apprName.isEmpty()) continue;
                e.appr = apprName;
                e.apprSno = appByName.value(apprName.toLower(), 0);
                if (e.apprSno <= 0) continue;
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
                if (e.apprSno <= 0) continue;                 // no worn mesh shipped
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
                if (e.apprSno <= 0) continue;
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
                if (actor.isEmpty()) continue;
                StableEntry e; e.item = base; e.appr = actor;
                e.apprSno = appByName.value(actor.toLower(), 0);
                if (e.apprSno <= 0) continue;
                readStrings(d4, QStringLiteral("Item_") + base, e.name, e.desc);
                if (e.name.isEmpty()) e.name = actor;
                entryIcon(raw, e.apprSno);
                pets.append(e);
            }
        }
        auto byName2 = [](const StableEntry& a, const StableEntry& b) { return a.name.toLower() < b.name.toLower(); };
        std::sort(mounts.begin(), mounts.end(), byName2);
        std::sort(armor.begin(), armor.end(), byName2);
        std::sort(trophies.begin(), trophies.end(), byName2);
        std::sort(pets.begin(), pets.end(), byName2);
        // Legacy (name, sno) pet list — several call sites key off it.
        QVector<QPair<QString, int>> petPairs;
        for (const StableEntry& e : pets) petPairs.append({ e.name, e.apprSno });

        QDir().mkpath(cacheBase);
        QFile out(cachePath);
        if (out.open(QIODevice::WriteOnly)) {
            QDataStream ds(&out);
            ds << kMagic << petPairs << icons;
            writeVec(ds, mounts); writeVec(ds, armor); writeVec(ds, trophies); writeVec(ds, pets);
            out.flush();
        }
        QMetaObject::invokeMethod(this, [this, petPairs, icons, mounts, armor, trophies, pets]() mutable {
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
                                QSettings().value(QStringLiteral("stable2/lastExportDir")).toString());
                            QStringList ex = exSuffix.split(QStringLiteral(" + "), Qt::SkipEmptyParts);
                            for (int i = ex.size() - 1; i >= 0; --i)
                                if (ex[i].trimmed() == QLatin1String("1 model")) ex.removeAt(i);
                            const QString extra = ex.isEmpty()
                                ? QString() : QStringLiteral("  —  %1").arg(ex.join(QStringLiteral(" + ")));
                            if (!exDir.isEmpty())
                                menu.addAction(ViewportPartMenu::withValue(
                                                   QStringLiteral("Export Model Last dir"), exDir) + extra, this,
                                               [this, entry] { exportAppearanceModel(entry.apprSno, entry.appr, true); });
                            menu.addAction(QStringLiteral("Export Model") + extra, this,
                                           [this, entry] { exportAppearanceModel(entry.apprSno, entry.appr, false); });
                        }
                        menu.addSeparator();
                        auto clip = [](const QString& s) { QGuiApplication::clipboard()->setText(s); };
                        auto prev = [](const QString& s) { return s.size() > 30 ? s.left(29) + QChar(0x2026) : s; };
                        const QString coll = AppearanceMeta::instance().collectionFor(entry.apprSno);
                        menu.addAction(QStringLiteral("Copy SNO id  (%1)").arg(entry.apprSno), this,
                                       [entry, clip] { clip(QString::number(entry.apprSno)); });
                        menu.addAction(QStringLiteral("Copy file name  (%1)").arg(prev(entry.appr)), this,
                                       [entry, clip] { clip(entry.appr); });
                        menu.addAction(QStringLiteral("Copy name  (%1)").arg(prev(entry.name)), this,
                                       [entry, clip] { clip(entry.name); });
                        QAction* aColl = menu.addAction(QStringLiteral("Copy collection name  (%1)").arg(prev(coll.isEmpty() ? QStringLiteral("—") : coll)), this,
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
            rebuildMount();
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
        rebuildMount();
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
    const auto hpMap = ModelAttach::loadHardpointMap(
        d4 + QStringLiteral("/json/base/meta/Appearance/") + mountAppr + QStringLiteral(".app.json"));
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
static QStringList rosterForLook(const QString& d4, const QString& appName, quint32 lookHash)
{
    if (!lookHash) return MaterialDecode::appearanceRoster(d4, appName);
    QFile f(QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4, appName));
    if (!f.open(QIODevice::ReadOnly)) return MaterialDecode::appearanceRoster(d4, appName);
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    // Find the look's SOA index by matching szLookName (a hash) against dwLookHash.
    int lookIdx = -1;
    const QJsonArray looks = root.value(QStringLiteral("ptAppearanceLooks")).toArray();
    for (int i = 0; i < looks.size(); ++i) {
        const quint32 h = quint32(looks.at(i).toObject()
                                      .value(QStringLiteral("szLookName")).toVariant().toULongLong());
        if (h == lookHash) { lookIdx = i; break; }
    }
    if (lookIdx <= 0) return MaterialDecode::appearanceRoster(d4, appName);   // default look
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
    QFile f(d4 + QStringLiteral("/json/base/meta/Appearance/") + appr + QStringLiteral(".app.json"));
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
        const QStringList roster = rosterForLook(d4, m_slotName[s], m_slotLook[s]);
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
        loaded << m_slotName[s];
    }
    if (parts.isEmpty()) {
        m_view->clearGeometry();
        m_lastGeo = ModelGeometry();
        if (m_status) m_status->setText(QStringLiteral("No mount selected."));
        return;
    }

    ModelGeometry geo;
    const bool merged = seh::runGuarded("stableMerge",
        [&]() { geo = (parts.size() == 1) ? parts[0] : ModelParser::mergeGeometries(parts); });
    if (!merged || !geo.valid || geo.primitives.isEmpty()) {
        m_view->clearGeometry(); m_lastGeo = ModelGeometry();
        if (m_status) m_status->setText(QStringLiteral("Assembly failed."));
        return;
    }
    // Reindex materials sequentially so the exporter's per-material list is unambiguous
    // after the multi-appearance merge (indices can otherwise collide across pieces).
    for (int i = 0; i < geo.primitives.size(); ++i) geo.primitives[i].materialIndex = i;

    m_lastGeo = geo;
    // Guard the GPU upload (flatten + VBO/IBO + cloth build) — the stage that most often faults.
    const bool gpuOk = seh::runGuarded("stableGpu", [&]() { m_view->setGeometry(geo, m_framed); });
    if (!gpuOk) {
        m_view->clearGeometry(); m_lastGeo = ModelGeometry();
        if (m_status) m_status->setText(QStringLiteral("Couldn't display this model — skipped."));
        return;
    }
    m_framed = true;
    m_view->setBackfaceCull(false);   // double-sided by default (parity with Wardrobe/Models)
    // Rig hardpoints (attach sockets) for the viewport overlay — read from the MOUNT appearance.
    // The mount is the first merged piece, so its bone indices are unchanged in the merged skeleton.
    m_lastGeo.hardpoints.clear();
    if (!m_slotName[SlotMount].isEmpty())
        Hardpoints::readInto(m_lastGeo, d4 + QStringLiteral("/json/base/meta/Appearance/")
                                             + m_slotName[SlotMount] + QStringLiteral(".app.json"));
    m_view->setHardpoints(m_lastGeo.hardpoints);

    const int n = geo.primitives.size();
    QVector<QImage> tex(n), norm(n), orm(n), emis(n), mask(n), trans(n);
    QVector<QImage> detN[3], detR[3];
    for (int k = 0; k < 3; ++k) { detN[k].resize(n); detR[k].resize(n); }
    QVector<float> metal(n), rough(n), emisMul(n, 1.0f), dNInt(n, 1.0f), dRInt(n, 1.0f), dROff(n, 0.0f);
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
        if (!emi.isNull()) { em.emissive = emi; em.hasEmissive = true; em.emisMult = 1.0f; }
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
    m_view->setEnvironment(QSettings().value(QStringLiteral("stable2/env"), 1).toInt());
    applyLightRig();
    // New geometry means a new rig and a rebuilt cloth sim. Re-push EVERY overlay (this also calls
    // applyClothParams for the mane/tail/cloth sim), so toggles survive a mount swap.
    reapplyOverlays();
    populateAnims();      // discover clips for the (possibly new) rig
    // Auto-play the model's nav-idle (like in-game). If a clip was already playing and the new rig
    // still has it (gear/look swap), keep that instead of resetting to idle.
    {
        const QStringList rows = m_animCache.value(animCarrierSno());
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
    if (!m_view) return;
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
    qint64 totalT = 0;
    for (const MeshPrimitive& p : m_lastGeo.primitives) totalT += p.indices.size() / 3;
    auto* root = new QTreeWidgetItem(m_partTree,
        QStringList{ QStringLiteral("mount"), QString::number(totalT) });
    root->setData(0, Qt::UserRole, -1);
    root->setFlags(root->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
    root->setCheckState(0, Qt::Checked);
    for (int i = 0; i < m_lastGeo.primitives.size(); ++i) {
        QString name = m_lastGeo.primitives[i].materialName;
        if (name.isEmpty()) name = QStringLiteral("part %1").arg(i);
        if (i < m_partFx.size() && m_partFx[i]) name += QStringLiteral("  [FX]");
        if (i < m_partSim.size() && m_partSim[i]) name += QStringLiteral("  [SIM]");
        const int tris = int(m_lastGeo.primitives[i].indices.size() / 3);
        auto* child = new QTreeWidgetItem(root, QStringList{ name, QString::number(tris) });
        child->setData(0, Qt::UserRole, i);
        child->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
        child->setCheckState(0, Qt::Checked);
    }
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
        QAction* aCopy = m.addAction(QStringLiteral("Copy name"));  aCopy->setEnabled(!cur.isEmpty());
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

bool StableTab2::hasTheme(const QString& mountAppr)
{
    buildThemeMap();
    const QString key = mountAppr.toLower();
    return m_themeArmor.contains(key) || m_themeTrophy.contains(key);
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
    const QStringList roster = MaterialDecode::appearanceRoster(d4, appr);
    for (MeshPrimitive& p : geo.primitives) {
        const QString rn = roster.value(p.materialIndex);
        if (!rn.isEmpty()) p.materialName = rn;
    }
    // Per-material PBR decode → ExportMaterial list.
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
            if (!emi.isNull()) { em.emissive = emi; em.hasEmissive = true; em.emisMult = 1.0f; }
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
    // clips, or just the one playing in preview.
    QVector<AnimParser::DecodedAnim> anims; QStringList animNames; int nAnim = 0;
    if (QSettings().value(QStringLiteral("export/includeAnim"), false).toBool() && !geo.skeleton.isEmpty()) {
        int carrier = 0; QString tok;
        const bool pet = appr.toLower().startsWith(QLatin1String("cmp_")) || appr.toLower().contains(QLatin1String("companion"));
        if (pet) { tok = appr.section(QLatin1Char('_'), 0, 1).toLower(); carrier = sno; }
        else { tok = catOf(appr.toLower());
               const QString want = QStringLiteral("mnt_base00_") + tok;
               if (m_index) for (const SnoEntry& e : m_index->entries(kGroupAppearance))
                   if (e.name.toLower() == want) { carrier = e.snoId; break; } }
        if (carrier > 0 && !m_animCache.contains(carrier)) m_animCache.insert(carrier, discoverClips(carrier, tok));
        QStringList want;
        auto clipOf = [](const QString& r) { return r.section(QStringLiteral("  ·  "), 0, 0); };
        if (QSettings().value(QStringLiteral("export/animScope"), 0).toInt() == 1) {
            for (const QString& r : m_animCache.value(carrier)) want << clipOf(r);
        } else {
            QString c = appr.compare(m_slotName[SlotMount], Qt::CaseInsensitive) == 0 ? m_playingAnim : QString();
            if (c.isEmpty())
                for (const QString& r : m_animCache.value(carrier))
                    if (clipOf(r).toLower().contains(QLatin1String("nav_idle"))) { c = clipOf(r); break; }
            if (!c.isEmpty()) want << c;
        }
        QHash<quint32, AnimParser::RestTRS> rest;
        for (const ModelJoint& j : geo.skeleton) { AnimParser::RestTRS t; t.q = j.restQ; t.t = j.restT; t.s = j.restS; rest.insert(j.nameHash, t); }
        for (const QString& nm : want) {
            QFile jf(d4 + QStringLiteral("/json/base/meta/Anim/") + nm + QStringLiteral(".ani.json"));
            if (!jf.open(QIODevice::ReadOnly)) continue;
            const QJsonObject root = QJsonDocument::fromJson(jf.readAll()).object();
            const int animSno = root.value(QStringLiteral("__snoID__")).toInt();
            const QJsonArray perms = root.value(QStringLiteral("ptPermutations")).toArray();
            if (animSno <= 0 || perms.isEmpty()) continue;
            const QJsonObject perm = perms.first().toObject();
            const int offset = perm.value(QStringLiteral("ptPayloadData")).toObject().value(QStringLiteral("value")).toObject().value(QStringLiteral("dataOffset")).toInt();
            const int frames = perm.value(QStringLiteral("nKeyframeCount")).toInt();
            const int comp = perm.value(QStringLiteral("flCompression")).toInt();
            const float fps = float(perm.value(QStringLiteral("flFrameRate")).toDouble(30.0));
            if (frames <= 0) continue;
            const QByteArray ap = m_reader->readPayloadBySno(quint64(animSno));
            if (ap.isEmpty()) continue;
            AnimParser::DecodedAnim a;
            const bool okA = seh::runGuarded("stableExportAnim", [&]() { a = AnimParser::decode(ap, offset, frames, comp, fps, rest); });
            if (okA && a.valid) { anims << a; animNames << nm; ++nAnim; }
        }
    }

    ModelExporter::Options opt = ModelExporter::optionsFromSettings();
    Retarget::applyFromSettings(geo);
    if (opt.blenderFriendly) GLModelWidget::blenderizeSkeletonNames(geo.skeleton);
    const bool wrote = ModelExporter::exportGlb(geo, path, mats, anims, animNames, opt);
    QSettings().setValue(QStringLiteral("stable2/exportDir"), QFileInfo(path).absolutePath());

    // Raw source files (.app + distinct .tex) into a "<name>_deps" folder, when enabled.
    int nRaw = 0;
    if (wrote && QSettings().value(QStringLiteral("export/withDeps"), false).toBool()) {
        const QString depDir = path.left(path.size() - 4) + QStringLiteral("_deps");
        QDir().mkpath(depDir);
        QFile af(depDir + QLatin1Char('/') + base + QStringLiteral(".app"));
        if (af.open(QIODevice::WriteOnly)) { af.write(payload); af.close(); ++nRaw; }
        QSet<qint64> doneTex;
        for (const QString& mn : roster) {
            if (mn.isEmpty()) continue;
            QFile mf(d4 + QStringLiteral("/json/base/meta/Material/") + mn + QStringLiteral(".mat.json"));
            if (!mf.open(QIODevice::ReadOnly)) continue;
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
    if (wrote)
        ExportNotifier::instance().notify(
            QStringLiteral("Exported %1%2").arg(QFileInfo(path).fileName(), extra), QFileInfo(path).absolutePath());
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
        if (n > 0) m_channelCombo->setCurrentIndex((m_channelCombo->currentIndex() + dir + n) % n);
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

#if 0   // ── Saved "Stables" loadouts removed (not needed for a browser) ────────────────────────
static QStringList stableLookKeys()
{
    QStringList keys{ QStringLiteral("species"),
                      QStringLiteral("mountSno"), QStringLiteral("bardingSno"), QStringLiteral("trophySno"),
                      QStringLiteral("mountName"), QStringLiteral("bardingName"), QStringLiteral("trophyName"),
                      QStringLiteral("mountType"), QStringLiteral("env") };
    for (int i = 0; i < 3; ++i)
        keys << QStringLiteral("look%1").arg(i) << QStringLiteral("disp%1").arg(i)
             << QStringLiteral("desc%1").arg(i);
    return keys;
}

void StableTab2::buildEnsemblePanel()
{
    m_ensemblePanel = new QWidget;
    auto* v = new QVBoxLayout(m_ensemblePanel);
    v->setContentsMargins(0, 0, 0, 0); v->setSpacing(4);

    // Collapsible header (▾ / ▸) so the panel can be tucked away.
    const bool shown = QSettings().value(QStringLiteral("stable2/showStables"), true).toBool();
    auto* hdr = new QHBoxLayout();
    auto* toggle = new QToolButton;
    toggle->setAutoRaise(true);
    toggle->setCheckable(true);
    toggle->setChecked(shown);
    toggle->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
    hdr->addWidget(toggle);
    hdr->addWidget(new QLabel(QStringLiteral("STABLES")));
    hdr->addStretch(1);
    v->addLayout(hdr);

    auto* body = new QWidget;
    auto* bl = new QVBoxLayout(body);
    bl->setContentsMargins(0, 0, 0, 0); bl->setSpacing(4);
    m_ensembleList = new QListWidget;
    m_ensembleList->setMaximumHeight(140);
    m_ensembleList->setIconSize(QSize(140, 30));
    m_ensembleList->setToolTip(QStringLiteral("Saved mount looks (mount + barding + trophy). Double-click to load."));
    bl->addWidget(m_ensembleList);
    auto* row = new QHBoxLayout();
    auto* saveB = new QPushButton(QStringLiteral("Save"));
    auto* overB = new QPushButton(QStringLiteral("Overwrite"));
    auto* delB = new QPushButton(QStringLiteral("Delete"));
    auto* renB = new QPushButton(QStringLiteral("Rename"));
    for (QPushButton* b : { saveB, overB, delB, renB }) row->addWidget(b);
    bl->addLayout(row);
    v->addWidget(body);
    body->setVisible(shown);
    connect(toggle, &QToolButton::toggled, this, [body, toggle](bool on) {
        body->setVisible(on);
        toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        QSettings().setValue(QStringLiteral("stable2/showStables"), on);
    });

    connect(m_ensembleList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        if (it) loadStable(it->data(Qt::UserRole).toString());
    });
    connect(saveB, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString nm = QInputDialog::getText(this, QStringLiteral("Save stable"),
                               QStringLiteral("Name:"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (ok && !nm.isEmpty()) saveStable(nm);
    });
    connect(overB, &QPushButton::clicked, this, [this] {
        if (auto* it = m_ensembleList->currentItem()) saveStable(it->data(Qt::UserRole).toString());
        else if (m_status) m_status->setText(QStringLiteral("Select a stable to overwrite."));
    });
    connect(delB, &QPushButton::clicked, this, [this] {
        if (auto* it = m_ensembleList->currentItem()) deleteStable(it->data(Qt::UserRole).toString());
    });
    connect(renB, &QPushButton::clicked, this, [this] {
        auto* it = m_ensembleList->currentItem();
        if (!it) return;
        const QString old = it->data(Qt::UserRole).toString();
        bool ok = false;
        const QString nm = QInputDialog::getText(this, QStringLiteral("Rename stable"),
                               QStringLiteral("New name:"), QLineEdit::Normal, old, &ok).trimmed();
        if (ok && !nm.isEmpty()) renameStable(old, nm);
    });
    refreshEnsembles();
}

QPixmap StableTab2::stableIconStrip(const QString& pfx) const
{
    QSettings s;
    QVector<QImage> icons;
    for (const QString& key : { QStringLiteral("mountSno"), QStringLiteral("bardingSno"), QStringLiteral("trophySno") }) {
        const int sno = s.value(pfx + key).toInt();
        if (sno <= 0) continue;
        const QImage ic = slotIcon(sno);
        if (!ic.isNull()) icons << ic;
    }
    if (icons.isEmpty()) return QPixmap();
    const int isz = 28, pad = 2;
    QImage strip(icons.size() * (isz + pad), isz, QImage::Format_RGBA8888);
    strip.fill(Qt::transparent);
    QPainter p(&strip);
    for (int i = 0; i < icons.size(); ++i) p.drawImage(QRect(i * (isz + pad), 0, isz, isz), icons[i]);
    p.end();
    return QPixmap::fromImage(strip);
}

void StableTab2::refreshEnsembles()
{
    if (!m_ensembleList) return;
    m_ensembleList->blockSignals(true);
    m_ensembleList->clear();
    for (const QString& name : QSettings().value(QStringLiteral("stable2/lookNames")).toStringList()) {
        const QString pfx = QStringLiteral("stable2/looks/%1/").arg(name);
        const QPixmap strip = stableIconStrip(pfx);
        auto* it = strip.isNull() ? new QListWidgetItem(name, m_ensembleList)
                                  : new QListWidgetItem(QIcon(strip), name, m_ensembleList);
        it->setData(Qt::UserRole, name);
        it->setToolTip(QStringLiteral("Double-click to load '%1'").arg(name));
    }
    m_ensembleList->blockSignals(false);
}

void StableTab2::saveStable(const QString& name)
{
    if (name.trimmed().isEmpty()) return;
    QSettings s;
    const QString pfx = QStringLiteral("stable2/looks/%1/").arg(name);
    s.setValue(pfx + QStringLiteral("species"), m_species->currentData());
    s.setValue(pfx + QStringLiteral("mountSno"), m_slotSel[SlotMount]);
    s.setValue(pfx + QStringLiteral("bardingSno"), m_slotSel[SlotBarding]);
    s.setValue(pfx + QStringLiteral("trophySno"), m_slotSel[SlotTrophy]);
    s.setValue(pfx + QStringLiteral("mountName"), m_slotName[SlotMount]);
    s.setValue(pfx + QStringLiteral("bardingName"), m_slotName[SlotBarding]);
    s.setValue(pfx + QStringLiteral("trophyName"), m_slotName[SlotTrophy]);
    for (int i = 0; i < SlotCount; ++i) {
        s.setValue(pfx + QStringLiteral("look%1").arg(i), m_slotLook[i]);
        s.setValue(pfx + QStringLiteral("disp%1").arg(i), m_slotDisp[i]);
        s.setValue(pfx + QStringLiteral("desc%1").arg(i), m_slotDesc[i]);
    }
    s.setValue(pfx + QStringLiteral("mountType"), m_mountType);
    s.setValue(pfx + QStringLiteral("env"), m_env->currentIndex());
    QStringList names = s.value(QStringLiteral("stable2/lookNames")).toStringList();
    if (!names.contains(name)) { names << name; names.sort(); s.setValue(QStringLiteral("stable2/lookNames"), names); }
    refreshEnsembles();
    if (m_status) m_status->setText(QStringLiteral("Saved stable '%1'").arg(name));
}

void StableTab2::loadStable(const QString& name)
{
    if (name.isEmpty()) return;
    QSettings s;
    const QString pfx = QStringLiteral("stable2/looks/%1/").arg(name);
    if (!s.contains(pfx + QStringLiteral("mountSno"))) return;
    const QString sp = s.value(pfx + QStringLiteral("species")).toString();
    { QSignalBlocker b(m_species); const int i = m_species->findData(sp); if (i >= 0) m_species->setCurrentIndex(i); }
    m_slotSel[SlotMount] = s.value(pfx + QStringLiteral("mountSno")).toInt();
    m_slotSel[SlotBarding] = s.value(pfx + QStringLiteral("bardingSno")).toInt();
    m_slotSel[SlotTrophy] = s.value(pfx + QStringLiteral("trophySno")).toInt();
    m_slotName[SlotMount] = s.value(pfx + QStringLiteral("mountName")).toString();
    m_slotName[SlotBarding] = s.value(pfx + QStringLiteral("bardingName")).toString();
    m_slotName[SlotTrophy] = s.value(pfx + QStringLiteral("trophyName")).toString();
    // Colour-look + type + display strings (default to 0/-1/empty for stables saved before
    // these existed — stale values from the previous selection must never leak in).
    for (int i = 0; i < SlotCount; ++i) {
        m_slotLook[i] = s.value(pfx + QStringLiteral("look%1").arg(i), 0u).toUInt();
        m_slotDisp[i] = s.value(pfx + QStringLiteral("disp%1").arg(i)).toString();
        m_slotDesc[i] = s.value(pfx + QStringLiteral("desc%1").arg(i)).toString();
    }
    m_mountType = s.value(pfx + QStringLiteral("mountType"), -1).toInt();
    { QSignalBlocker b(m_env); m_env->setCurrentIndex(s.value(pfx + QStringLiteral("env"), m_env->currentIndex()).toInt()); }
    refreshSlotCells();
    selectSlot(SlotMount);
    rebuildMount();
    if (m_status) m_status->setText(QStringLiteral("Loaded stable '%1'").arg(name));
}

void StableTab2::deleteStable(const QString& name)
{
    if (name.isEmpty()) return;
    QSettings s;
    s.remove(QStringLiteral("stable2/looks/%1").arg(name));
    QStringList names = s.value(QStringLiteral("stable2/lookNames")).toStringList();
    names.removeAll(name);
    s.setValue(QStringLiteral("stable2/lookNames"), names);
    refreshEnsembles();
    if (m_status) m_status->setText(QStringLiteral("Deleted stable '%1'").arg(name));
}

void StableTab2::renameStable(const QString& oldName, const QString& newName)
{
    const QString nn = newName.trimmed();
    if (oldName.isEmpty() || nn.isEmpty() || oldName == nn) return;
    QSettings s;
    const QString op = QStringLiteral("stable2/looks/%1/").arg(oldName);
    const QString np = QStringLiteral("stable2/looks/%1/").arg(nn);
    for (const QString& k : stableLookKeys()) s.setValue(np + k, s.value(op + k));
    s.remove(QStringLiteral("stable2/looks/%1").arg(oldName));
    QStringList names = s.value(QStringLiteral("stable2/lookNames")).toStringList();
    names.removeAll(oldName);
    if (!names.contains(nn)) names << nn;
    names.sort();
    s.setValue(QStringLiteral("stable2/lookNames"), names);
    refreshEnsembles();
}
#endif   // Saved "Stables" loadouts removed

// ── Animations ──────────────────────────────────────────────────────────────────
void StableTab2::buildAnimPanel()
{
    m_animPanel = new QWidget;
    auto* v = new QVBoxLayout(m_animPanel);
    v->setContentsMargins(0, 0, 0, 0); v->setSpacing(3);
    v->addWidget(new QLabel(QStringLiteral("ANIMATIONS")));

    m_timeline = new QWidget(m_animPanel);
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
    v->addWidget(m_timeline);
    m_timeline->setVisible(false);

    m_animSearch = new QLineEdit;
    m_animSearch->setPlaceholderText(QStringLiteral("Filter animations…"));
    m_animSearch->setClearButtonEnabled(true);
    v->addWidget(m_animSearch);
    m_anims = new QListWidget;
    m_anims->setMinimumHeight(220);
    m_anims->setMaximumHeight(420);
    v->addWidget(m_anims, 1);
    installCopyMenu(m_anims, 0);   // right-click → Copy clip name / Copy all

    auto* resetBtn = new QPushButton(QStringLiteral("Reset to default"));
    resetBtn->setToolTip(QStringLiteral("Reset to the base horse mount with no armor or trophy, "
                                        "playing the default idle (1× speed, looping)."));
    v->addWidget(resetBtn);
    connect(resetBtn, &QPushButton::clicked, this, [this] { resetAnimToDefault(); });

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

// The appearance SNO that owns the clips. For a mount that is ALWAYS the species base
// mount mnt_base00_<species> — every horse skin plays the horse base's animation set — so
// a specific skin (mnt_base00_horse26) still sources its clips from mnt_base00_horse. For a
// pet, the pet's own appearance owns its clips.
int StableTab2::animCarrierSno() const
{
    if (petMode()) return m_slotSel[SlotMount];
    if (!m_index) return m_slotSel[SlotMount];
    const QString cat = mountCategory();
    if (cat.isEmpty()) return m_slotSel[SlotMount];
    const QString want = QStringLiteral("mnt_base00_") + cat;
    int anyBase = 0;
    for (const SnoEntry& e : m_index->entries(kGroupAppearance)) {
        const QString lower = e.name.toLower();
        if (lower == want) return e.snoId;                                   // base00 (preferred)
        if (!anyBase && lower.startsWith(QLatin1String("mnt_base")) && catOf(lower) == cat)
            anyBase = e.snoId;                                               // fallback: any base of the species
    }
    return anyBase ? anyBase : m_slotSel[SlotMount];
}

void StableTab2::populateAnims()
{
    if (!m_anims) return;
    const int carrier = animCarrierSno();
    if (carrier <= 0 || m_lastGeo.skeleton.isEmpty()) {
        m_anims->clear();
        if (m_animPanel) m_animPanel->setVisible(false);
        return;
    }
    if (m_animPanel) m_animPanel->setVisible(QSettings().value(QStringLiteral("stable2/showAnims"), true).toBool());

    if (!m_animCache.contains(carrier)) {
        const QString tok = petMode() ? m_slotName[SlotMount].section(QLatin1Char('_'), 0, 1).toLower()
                                      : mountCategory();
        m_animCache.insert(carrier, discoverClips(carrier, tok));
    }
    fillAnimList();
}

// Scan Anim/*.ani.json (narrowed by the species/mount token) for the clips owned by `carrier`.
// Returns "<name>  ·  <frames> frames" rows, sorted. Shared by populateAnims + exportMenuSuffix.
QStringList StableTab2::discoverClips(int carrier, const QString& tok)
{
    QStringList rows;
    if (carrier <= 0) return rows;
    const QString d4 = Config::d4dataDir();
    static const QRegularExpression rxApp(
        QStringLiteral("\"snoAppearance\":\\s*\\{[^{}]*?\"__raw__\":\\s*(\\d+)"));
    static const QRegularExpression rxFrames(QStringLiteral("\"nKeyframeCount\":\\s*(\\d+)"));
    QDirIterator it(d4 + QStringLiteral("/json/base/meta/Anim"),
                    QStringList{ QStringLiteral("*.ani.json") }, QDir::Files);
    while (it.hasNext()) {
        const QString fp = it.next();
        const QString base = it.fileName();
        const QString low = base.toLower();
        if (!(low.contains(QLatin1String("mnt")) || low.contains(QLatin1String("mount"))
              || (!tok.isEmpty() && low.contains(tok)))) continue;
        QFile jf(fp);
        if (!jf.open(QIODevice::ReadOnly)) continue;
        const QString raw = QString::fromUtf8(jf.readAll());
        const auto m = rxApp.match(raw);
        if (!m.hasMatch() || m.captured(1).toInt() != carrier) continue;
        const QString nm = base.left(base.size() - 9);   // strip ".ani.json"
        const auto fm = rxFrames.match(raw);
        rows << (fm.hasMatch() ? QStringLiteral("%1  ·  %2 frames").arg(nm, fm.captured(1)) : nm);
    }
    rows.sort();
    return rows;
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
        // Resolve the clip-owning carrier for this item (mount → species base; pet → itself).
        int carrier = 0; QString tok;
        if (pet) {
            tok = appr.section(QLatin1Char('_'), 0, 1).toLower();
            if (m_index) for (const SnoEntry& e : m_index->entries(kGroupAppearance))
                if (e.name.compare(appr, Qt::CaseInsensitive) == 0) { carrier = e.snoId; break; }
        } else {
            tok = catOf(appr.toLower());
            const QString want = QStringLiteral("mnt_base00_") + tok;
            if (m_index) for (const SnoEntry& e : m_index->entries(kGroupAppearance))
                if (e.name.toLower() == want) { carrier = e.snoId; break; }
        }
        if (carrier > 0 && !m_animCache.contains(carrier)) m_animCache.insert(carrier, discoverClips(carrier, tok));
        const QStringList clips = carrier > 0 ? m_animCache.value(carrier) : QStringList();
        if (s.value(QStringLiteral("export/animScope"), 0).toInt() == 1) {
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
    const QStringList rows = m_animCache.value(animCarrierSno());
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
    env->addItems({ QStringLiteral("Studio"), QStringLiteral("Outdoor"),
                    QStringLiteral("Dungeon"), QStringLiteral("Night") });
    env->setCurrentIndex(s.value(QStringLiteral("stable2/env"), 1).toInt());
    connect(env, &QComboBox::currentIndexChanged, this, [this](int i) {
        QSettings().setValue(QStringLiteral("stable2/env"), i);
        if (m_view) m_view->setEnvironment(i);
    });
    envRow->addWidget(env, 1);
    pl->addLayout(envRow);

    // Preset selects the key/rim/fill COLOURS (intensities + key direction are the sliders below).
    auto* preRow = new QHBoxLayout();
    preRow->addWidget(new QLabel(QStringLiteral("Preset"), m_lightPanel));
    auto* preset = new QComboBox(m_lightPanel);
    preset->addItems({ QStringLiteral("D4 Wardrobe (campfire)"),
                       QStringLiteral("Hero Direct (neutral)"),
                       QStringLiteral("Studio (cool 3-point)") });
    preset->setCurrentIndex(s.value(QStringLiteral("stable2/light/preset"), 1).toInt());
    connect(preset, &QComboBox::currentIndexChanged, this, [this](int i) {
        QSettings().setValue(QStringLiteral("stable2/light/preset"), i); applyLightRig();
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
    r.preset       = s.value(QStringLiteral("stable2/light/preset"), 1).toInt();   // default: Hero Direct
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
    fovSlider->setValue(s.value(QStringLiteral("stable2/fov"), 45).toInt());
    fovSlider->setToolTip(QStringLiteral("Camera field of view (degrees)"));
    connect(fovSlider, &QSlider::valueChanged, this, [this](int vv) {
        QSettings().setValue(QStringLiteral("stable2/fov"), vv);
        if (m_view) m_view->setFov(float(vv));
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

    // Orthographic projection.
    auto* orthoChk = new QCheckBox(QStringLiteral("Orthographic projection"), m_camPanel);
    orthoChk->setChecked(s.value(QStringLiteral("stable2/ortho"), false).toBool());
    connect(orthoChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("stable2/ortho"), on);
        if (m_view) m_view->setOrthographic(on);
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

    auto* gLight = addGroup(QStringLiteral("Scene & shadows"));
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

    auto* gGeom = addGroup(QStringLiteral("Geometry & debug"));
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
// Isolate one part, run exportMount() (which honours the parts-tree checks), then restore.
// ONE part menu, shown from BOTH the 3D viewport and the PARTS PANEL.
void StableTab2::showPartContextMenu(int part, const QPoint& gp)
{
        if (!m_partTree) return;
        auto itemForPart = [this](int p) -> QTreeWidgetItem* {
            for (int r = 0; r < m_partTree->topLevelItemCount(); ++r) {
                QTreeWidgetItem* root = m_partTree->topLevelItem(r);
                for (int c = 0; c < root->childCount(); ++c)
                    if (root->child(c)->data(0, Qt::UserRole).toInt() == p) return root->child(c);
            }
            return nullptr;
        };
        auto setAll = [this](Qt::CheckState st) {
            for (int r = 0; r < m_partTree->topLevelItemCount(); ++r) {
                QTreeWidgetItem* root = m_partTree->topLevelItem(r);
                for (int c = 0; c < root->childCount(); ++c) root->child(c)->setCheckState(0, st);
            }
        };
        // Right-click SELECTS (blue outline); the camera only moves via "Frame Part".
        if (m_view) m_view->setPickedPart(part);
        ViewportPartMenu::Info in;
        ViewportPartMenu::Actions act;
        QTreeWidgetItem* item = nullptr;
        int modelTris = 0;
        if (m_view)
            for (int i = 0; i < m_lastGeo.primitives.size(); ++i) modelTris += m_view->partTriangles(i);
        in.sourceModel   = QStringLiteral("Mount");
        in.modelTris     = modelTris;
        in.lastExportDir = QSettings().value(QStringLiteral("stable2/lastExportDir")).toString();
        if (part >= 0 && part < m_lastGeo.primitives.size()) {
            item = itemForPart(part);
            in.part         = part;
            in.partName     = m_lastGeo.primitives[part].materialName;
            in.partFileName = in.partName;
            in.partTris     = m_view ? m_view->partTriangles(part) : 0;
            in.visible      = !item || item->checkState(0) == Qt::Checked;
            in.isSim        = part < m_partSim.size() && m_partSim[part];
            in.isFx         = part < m_partFx.size()  && m_partFx[part];
            act.setVisible  = [item](bool on) { if (item) item->setCheckState(0, on ? Qt::Checked : Qt::Unchecked); };
            act.isolate     = [setAll, item] { setAll(Qt::Unchecked); if (item) item->setCheckState(0, Qt::Checked); };
            act.selectPart  = [this, item] {
                if (!item || !m_partTree) return;
                m_partTree->setCurrentItem(item); m_partTree->scrollToItem(item);
            };
            act.frame       = [this, part] {
                if (!m_view) return;
                QVector3D c; float r;
                if (m_view->partsBounds(QVector<int>{part}, c, r))
                    m_view->frameRegionKeepRotation(c, r, /*animate=*/true);
            };
            act.exportPart        = [this, item, setAll] { exportSinglePart(item, setAll); };
            act.exportPartLastDir = [this, item, setAll] { exportSinglePart(item, setAll); };
        }
        act.exportModel        = [this] { exportMount(); };
        act.exportModelLastDir = [this] { exportMount(); };
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
        ViewportPartMenu::exec(this, gp, in, act);
}

void StableTab2::exportSinglePart(QTreeWidgetItem* item,
                                  const std::function<void(Qt::CheckState)>& setAll)
{
    if (!m_partTree || !item) return;
    QVector<Qt::CheckState> saved;
    for (int r = 0; r < m_partTree->topLevelItemCount(); ++r) {
        QTreeWidgetItem* root = m_partTree->topLevelItem(r);
        for (int c = 0; c < root->childCount(); ++c) saved << root->child(c)->checkState(0);
    }
    setAll(Qt::Unchecked);
    item->setCheckState(0, Qt::Checked);
    exportMount();
    int k = 0;
    for (int r = 0; r < m_partTree->topLevelItemCount(); ++r) {
        QTreeWidgetItem* root = m_partTree->topLevelItem(r);
        for (int c = 0; c < root->childCount(); ++c)
            if (k < saved.size()) root->child(c)->setCheckState(0, saved[k++]);
    }
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
void StableTab2::exportMount()
{
    if (!m_lastGeo.valid || m_lastGeo.primitives.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Export"), QStringLiteral("No mount to export.")); return;
    }
    const QString base = m_slotName[SlotMount].isEmpty() ? QStringLiteral("mount") : m_slotName[SlotMount];
    const QString dir = QSettings().value(QStringLiteral("stable2/exportDir"), QDir::homePath()).toString();
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export mount"),
                       dir + QStringLiteral("/") + base + QStringLiteral(".glb"),
                       QStringLiteral("glTF Binary (*.glb)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".glb"), Qt::CaseInsensitive)) path += QStringLiteral(".glb");
    // Include the currently-playing clip so the exported mount is animated in Blender.
    QVector<AnimParser::DecodedAnim> anims;
    QStringList animNames;
    if (m_curAnim.valid && !m_lastGeo.skeleton.isEmpty()) {
        anims << m_curAnim;
        animNames << (m_playingAnim.isEmpty() ? QStringLiteral("clip") : m_playingAnim);
    }
    const ModelExporter::Options opt = ModelExporter::optionsFromSettings();
    ModelGeometry geoCopy = m_lastGeo;   // copy so retarget/rename never touches the live preview
    Retarget::applyFromSettings(geoCopy);
    if (opt.blenderFriendly)
        GLModelWidget::blenderizeSkeletonNames(geoCopy.skeleton);
    const bool ok = ModelExporter::exportGlb(geoCopy, path, m_exportMats, anims, animNames, opt);
    const QString folder = QFileInfo(path).absolutePath();
    QSettings().setValue(QStringLiteral("stable2/exportDir"), folder);
    if (ok)
        ExportNotifier::instance().notify(
            QStringLiteral("Exported %1%2").arg(QFileInfo(path).fileName(),
                anims.isEmpty() ? QString() : QStringLiteral("  (with animation: %1)").arg(animNames.first())),
            folder);
    else
        QMessageBox::information(this, QStringLiteral("Export"), QStringLiteral("Export failed."));
}
