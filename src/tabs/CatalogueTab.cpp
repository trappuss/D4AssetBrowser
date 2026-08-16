#include "tabs/CatalogueTab.h"

#include "app/Config.h"
#include "app/SehGuard.h"
#include "casc/CascReader.h"
#include "index/AppearanceMeta.h"
#include "index/IconIndex.h"
#include "model/MaterialDecode.h"
#include "tex/FrameTable.h"     // CASC-side atlas frames — the only frame source for shop art that
                                // d4data has no .tex.json for, which is most of it
#include "tex/TexMeta.h"        // TexMeta / TexFrame — used directly by largestFrame and the
                                // texframe export, not just pulled in via TexturesTab.h
#include "tex/TextureDefTable.h"
#include "index/SnoIndex.h"
#include "tabs/BatchSink.h"   // suppresses the pipelines' per-run modal box; see exportBundle
#include "tabs/HintBar.h"
#include "tabs/MarkingCompose.h"   // markingDef — a marking's masks + swatch, same as the Wardrobe
#include "util/CsvCopy.h"
#include "util/HoverInfo.h"
#include "tabs/ModelsTab.h"
#include "tabs/TexturesTab.h"
// MenuText + condensePath. Comment on its own line: verify-src matches the include DIRECTIVE
// exactly to end-of-line, so a trailing comment reads as "not included".
#include "util/ViewportPartMenu.h"

#include <QAction>
#include <QMenu>
#include <QComboBox>
#include <QDir>
#include <QFile>        // manifest write + the actor portrait probe
#include <QFileDialog>
#include <QFileInfo>    // "only new" probes for an existing manifest
#include <QGuiApplication>
#include <QFrame>       // the funnel popup is a Qt::Popup QFrame, as in Textures/Models
#include <QHBoxLayout>
#include <functional>   // rebuildFilterChips takes a std::function clear-callback per chip
#include <QHeaderView>
#include <QPolygonF>    // the funnel glyph is drawn, not shipped as an asset
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>   // "Export all N matching" confirmation
#include <QPushButton>
#include <QCheckBox>
#include <QSettings>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSplitter>
#include <QTextBrowser>
#include <QTimer>
#include <QCursor>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QWheelEvent>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <thread>

namespace {

// Group ids by NAME, never by literal — a renumbered game build then needs one map corrected
// rather than every call site. Same pattern as BulkExtractorTab.
inline int kAppearanceGroup() {
    static const int g = SnoIndex::groupIdByName(QStringLiteral("Appearance"), 9); return g;
}
inline int kTextureGroup() {
    static const int g = SnoIndex::groupIdByName(QStringLiteral("Texture"), 44); return g;
}

// A bundle's own shop art follows a strict naming convention in group 44. Matching is EXACT, so
// the order here carries no meaning — an earlier comment claimed it guarded against prefix
// shadowing, which cannot happen.
//
// Only the 2DUI_ stem takes suffixes: across all of d4data exactly 5 files match
// 2DInventory_Bundle_*_<suffix>, so pairing every suffix with both stems bought three guaranteed
// misses per bundle.
const char* const kUiSuffixes[] = { "", "_details", "_background", "_WebImage", "_icons" };

QString settingsLastDir()
{
    return QSettings().value(QStringLiteral("catalogue/lastDir")).toString();
}

// Grid-view cell painter — a copy of the Models tab's, which lives in ITS anonymous namespace with
// no header (TexturesTab already carries a second copy for the same reason). Aspect-preserved
// thumbnail plus one elided caption line; the stock IconMode delegate wraps long names onto
// several lines and squeezes the icon into a strip.
//
// Portrait, unlike the Models copy: a shop card is roughly 800x1300, so the tile is 4:3 taller than
// it is wide and sizeHint must stay in step with setGridSize below.
class GridItemDelegate : public QStyledItemDelegate {
public:
    explicit GridItemDelegate(int iconPx, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), m_icon(iconPx) {}
    void setIconPx(int px) { m_icon = px; }
    // +50, not +34: the caption is two lines now, and sizeHint must stay in step with the
    // setGridSize call in setThumbPx or the tiles overlap.
    static QSize tileFor(int iconPx) { return QSize(iconPx + 16, iconPx * 4 / 3 + 50); }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return tileFor(m_icon);
    }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        p->save();
        const QRect r = opt.rect;
        if (opt.state & QStyle::State_Selected)        p->fillRect(r, QColor(0x8a, 0x14, 0x14));
        else if (opt.state & QStyle::State_MouseOver)  p->fillRect(r, QColor(0x3a, 0x20, 0x20));
        const int imgH = m_icon * 4 / 3;
        QPixmap pm;
        const QVariant dec = idx.data(Qt::DecorationRole);
        if (dec.canConvert<QIcon>()) pm = qvariant_cast<QIcon>(dec).pixmap(m_icon, imgH);
        if (!pm.isNull()) {
            const QPixmap sp = pm.scaled(m_icon, imgH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            p->drawPixmap(r.left() + (r.width() - sp.width()) / 2,
                          r.top() + 4 + (imgH - sp.height()) / 2, sp);
        }
        // TWO lines, elided independently. Eliding the joined string instead removed the middle —
        // which is exactly where the separator sits — and collapsed the caption to one mangled row.
        const QString name = idx.data(Qt::DisplayRole).toString();
        const QStringList rows = name.split(QChar::LineSeparator);
        const QRect tr(r.left() + 2, r.top() + 4 + imgH + 2, r.width() - 4, r.height() - imgH - 8);
        p->setPen((opt.state & QStyle::State_Selected) ? QColor(0xff, 0xff, 0xff)
                                                       : QColor(0xcc, 0xcc, 0xcc));
        int ty = tr.top();
        for (int li = 0; li < rows.size() && li < 2; ++li) {
            if (li == 1) p->setPen(QColor(0x9a, 0x9a, 0x9a));
            p->drawText(QRect(tr.left(), ty, tr.width(), opt.fontMetrics.height()),
                        Qt::AlignHCenter | Qt::AlignTop,
                        opt.fontMetrics.elidedText(rows[li], Qt::ElideMiddle, tr.width()));
            ty += opt.fontMetrics.height();
        }
        p->restore();
    }
private:
    int m_icon;
};

}  // namespace

CatalogueTab::CatalogueTab(ModelsTab* models, TexturesTab* textures, QWidget* parent)
    : BrowserTab(parent), m_models(models), m_textures(textures)
{
    buildUi();
}

void CatalogueTab::buildUi()
{
    if (m_uiBuilt) return;
    m_uiBuilt = true;

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    if (QWidget* hint = makeHintBar(this,
            QStringLiteral("Tip: pick a bundle to see everything it shipped with · export writes "
                           "models, textures, shop art and a manifest · search matches the shop "
                           "name, the SNO name and the lore text"),
            "hints/catalogue"))
        root->addWidget(hint);

    auto* split = new QSplitter(Qt::Horizontal, this);
    root->addWidget(split, 1);

    // ── Left: the bundle list ────────────────────────────────────────────────────────────────
    auto* left = new QWidget(split);
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(8, 8, 4, 8);
    lv->setSpacing(6);

    // ── Filters ─────────────────────────────────────────────────────────────────────────────────
    // ONE row on screen — [funnel] search [grid] — with everything else behind the funnel, which is
    // how Models, Textures and Bulk Extract are laid out. This tab had three stacked rows of combos
    // and checkboxes permanently occupying the top of the pane: the widest thing in the tab was its
    // controls, the list they filter got what was left, and no other tab looked like it.
    //
    // The controls themselves are unchanged and keep their connections — only their parent and
    // position move.
    m_kindFilter = new QComboBox(left);
    m_kindFilter->setFixedHeight(kBarH);
    m_kindFilter->addItem(QStringLiteral("Any contents"), -1);
    for (int k = StoreProductIndex::Transmog; k <= StoreProductIndex::DyeArmor; ++k)
        m_kindFilter->addItem(StoreProductIndex::kindLabel(StoreProductIndex::Kind(k)), k);
    m_branchFilter = new QComboBox(left);
    m_branchFilter->setFixedHeight(kBarH);
    m_branchFilter->addItem(QStringLiteral("Any patch"), QString());
    // Season. snoAssociatedSeason is populated on roughly 2,100 products and the ref object carries
    // the display name ("Season 8") — the index parsed the sno and threw the name away, so this
    // could never be offered. "Which cosmetics came from Season 8" is the most natural question to
    // ask a shop catalogue and the tab could not answer it.
    m_seasonFilter = new QComboBox(left);
    m_seasonFilter->setFixedHeight(kBarH);
    m_seasonFilter->addItem(QStringLiteral("Any season"), 0);
    // Sort. The list was title-only, which is the one order nobody browses a shop's history in.
    // Season and patch are both indexed, so chronological costs nothing.
    m_sortCombo = new QComboBox(left);
    m_sortCombo->setFixedHeight(kBarH);
    m_sortCombo->addItem(QStringLiteral("Sort: Name"),   QStringLiteral("name"));
    m_sortCombo->addItem(QStringLiteral("Sort: Season"), QStringLiteral("season"));
    m_sortCombo->addItem(QStringLiteral("Sort: Patch"),  QStringLiteral("patch"));
    // "Latest" — bundles whose SNO first appeared in this game build. SnoIndex::isNew already backs
    // the same filter in Models and Bulk Extract; for a shop catalogue "what is new this patch" is
    // the single most natural question, and it was the one tab that could not answer it.
    m_latestChk = new QCheckBox(QStringLiteral("Latest"), left);
    m_latestChk->setToolTip(QStringLiteral(
        "Only bundles added in the current game update.\n"
        "Observational: it compares against the previous build this tool opened, so the first run "
        "on a new install establishes a baseline and marks nothing."));
    m_rememberChk = new QCheckBox(QStringLiteral("Remember filters"), left);
    m_rememberChk->setToolTip(QStringLiteral(
        "Restore the search text, filters and sort order on the next launch."));
    m_rememberChk->setChecked(
        QSettings().value(QStringLiteral("catalogue/rememberFilters"), false).toBool());
    connect(m_rememberChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("catalogue/rememberFilters"), on);
        if (on) saveFilterState();   // capture what is on screen NOW, not at next change
    });
    // Multi select — same control, same name and same place in the popup as the Textures tab's.
    // Off by default so a stray click cannot silently widen an export.
    m_multiSelect = new QCheckBox(QStringLiteral("Multi select"), left);
    m_multiSelect->setToolTip(QStringLiteral(
        "Select several bundles at once — ctrl-click, shift-range.\n"
        "The Export menu then exports every selected bundle, each into its own folder."));
    connect(m_multiSelect, &QCheckBox::toggled, this, [this](bool on) {
        m_list->setSelectionMode(on ? QAbstractItemView::ExtendedSelection
                                    : QAbstractItemView::SingleSelection);
    });

    // ── The popup ───────────────────────────────────────────────────────────────────────────────
    // Qt::Popup, so it closes on click-away and on Esc without any code of ours. Styled to match
    // the Textures tab's panel exactly — the same control should not look like a different widget
    // depending on which tab you opened it from.
    m_filterPanel = new QFrame(this, Qt::Popup);
    m_filterPanel->setObjectName(QStringLiteral("catFilterPanel"));
    m_filterPanel->setStyleSheet(QStringLiteral(
        "QFrame#catFilterPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
        "QLabel{color:#cccccc;} QCheckBox{color:#cccccc;}"));
    auto* fp = new QVBoxLayout(m_filterPanel);
    fp->setContentsMargins(10, 8, 10, 8);
    fp->setSpacing(6);
    auto secHdr = [&](const QString& t) {
        auto* h = new QLabel(t, m_filterPanel);
        h->setStyleSheet(QString::fromLatin1(kHdrQss));
        fp->addWidget(h);
    };
    // Grouped by the question each answers, not by widget type: WHAT is in the bundle, WHEN it
    // arrived, HOW the list is shown.
    secHdr(QStringLiteral("Contents"));
    fp->addWidget(m_kindFilter);
    secHdr(QStringLiteral("Released"));
    fp->addWidget(m_branchFilter);
    fp->addWidget(m_seasonFilter);
    fp->addWidget(m_latestChk);
    secHdr(QStringLiteral("View"));
    fp->addWidget(m_sortCombo);
    {
        auto* r = new QHBoxLayout;
        r->addWidget(m_multiSelect);
        r->addWidget(m_rememberChk);
        r->addStretch(1);
        fp->addLayout(r);
    }
    auto* clearBtn = new QPushButton(QStringLiteral("Clear all filters"), m_filterPanel);
    fp->addWidget(clearBtn);
    // Blocked signals, then ONE reload — four unblocked resets would rebuild the list four times,
    // and the first three results are thrown away. "Remember" and "Multi select" are deliberately
    // untouched: they are preferences about the tab, not filters on the list.
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        { QSignalBlocker b(m_kindFilter);   m_kindFilter->setCurrentIndex(0); }
        { QSignalBlocker b(m_branchFilter); m_branchFilter->setCurrentIndex(0); }
        { QSignalBlocker b(m_seasonFilter); m_seasonFilter->setCurrentIndex(0); }
        { QSignalBlocker b(m_latestChk);    m_latestChk->setChecked(false); }
        { QSignalBlocker b(m_search);       m_search->clear(); }
        reloadBundleList();
    });

    // ── The funnel button ───────────────────────────────────────────────────────────────────────
    // Glyph drawn in code, byte-identical to the Textures tab's, rather than a new icon asset.
    m_filtersToggle = new QToolButton(left);
    {
        QPixmap pm(14, 14); pm.fill(Qt::transparent);
        QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen); p.setBrush(QColor(190, 180, 150));
        p.drawPolygon(QPolygonF({{1.5, 2.0}, {12.5, 2.0}, {8.5, 7.0}, {8.5, 12.0}, {5.5, 10.0}, {5.5, 7.0}}));
        p.end();
        m_filtersToggle->setIcon(QIcon(pm));
    }
    m_filtersToggle->setIconSize(QSize(14, 14));
    m_filtersToggle->setFixedSize(28, kBarH);
    m_filtersToggle->setStyleSheet(QString::fromLatin1(kIconBtnQss));
    m_filtersToggle->setCursor(Qt::PointingHandCursor);
    m_filtersToggle->setToolTip(QStringLiteral("Filter by contents, patch, season…"));
    connect(m_filtersToggle, &QToolButton::clicked, this, [this] {
        m_filterPanel->adjustSize();
        m_filterPanel->move(m_filtersToggle->mapToGlobal(QPoint(0, m_filtersToggle->height() + 2)));
        m_filterPanel->show();
        m_filterPanel->raise();
    });

    auto* filterRow = new QHBoxLayout;
    filterRow->setSpacing(6);
    filterRow->addWidget(m_filtersToggle);
    m_search = new QLineEdit(left);
    m_search->setPlaceholderText(QStringLiteral("Search bundles…"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedHeight(kBarH);
    filterRow->addWidget(m_search, 1);
    m_gridBtn = new QToolButton(left);
    m_gridBtn->setText(QStringLiteral("▦"));
    m_gridBtn->setCheckable(true);
    m_gridBtn->setFixedHeight(kBarH);
    m_gridBtn->setStyleSheet(QString::fromLatin1(kIconBtnQss));
    m_gridBtn->setToolTip(QStringLiteral("Grid view — Ctrl+scroll over the list resizes the tiles"));
    connect(m_gridBtn, &QToolButton::toggled, this, [this](bool on) { setGridView(on); });
    filterRow->addWidget(m_gridBtn);
    lv->addLayout(filterRow);

    // Active-filter chips, one removable pill per set filter — the same affordance Models and
    // Textures have. With four filters and 1,890 bundles, "why is this list short" is easy to hit
    // and the combos are the last place anyone looks.
    m_chipRow = new QHBoxLayout;
    m_chipRow->setSpacing(4);
    m_chipRow->setContentsMargins(0, 0, 0, 0);
    lv->addLayout(m_chipRow);

    m_list = new QListWidget(left);
    m_list->setAlternatingRowColors(true);
    m_list->setUniformItemSizes(true);   // fixed row height → the visible-range maths below is exact
    m_list->setResizeMode(QListView::Adjust);          // reflow tiles on resize (grid mode)
    m_list->setMovement(QListView::Static);
    m_list->setWordWrap(false);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setMouseTracking(true);
    m_list->viewport()->setMouseTracking(true);   // hover previews need move events
    m_list->viewport()->installEventFilter(this); // hover preview + Ctrl+scroll resize
    m_listPx = qBound(48, QSettings().value(QStringLiteral("catalogue/listPx"), 96).toInt(), 256);
    m_gridPx = qBound(48, QSettings().value(QStringLiteral("catalogue/gridPx"), 128).toInt(), 256);
    lv->addWidget(m_list, 1);

    m_countLbl = new QLabel(QStringLiteral("—"), left);
    m_countLbl->setStyleSheet(QStringLiteral("QLabel{color:#9a9a9a;}"));
    lv->addWidget(m_countLbl);

    // ── Right: the bundle detail ─────────────────────────────────────────────────────────────
    auto* right = new QWidget(split);
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(4, 8, 8, 8);
    rv->setSpacing(6);

    m_title = new QLabel(QStringLiteral("Select a bundle"), right);
    m_title->setStyleSheet(QString::fromLatin1(kHdrQss));
    m_title->setWordWrap(true);
    rv->addWidget(m_title);

    m_subtitle = new QLabel(QString(), right);
    m_subtitle->setStyleSheet(QStringLiteral("QLabel{color:#9a9a9a;}"));
    m_subtitle->setWordWrap(true);
    rv->addWidget(m_subtitle);

    // Hero + card side by side, mirroring the shop page. Given real estate (stretch 3) because
    // this is the thing the tab exists to show — the first version capped it at 260px tall.
    auto* artRow = new QHBoxLayout;
    artRow->setSpacing(6);
    m_art = new QLabel(right);
    m_art->setAlignment(Qt::AlignCenter);
    m_art->setMinimumHeight(220);
    m_art->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_art->setStyleSheet(QStringLiteral("QLabel{background:#1b1b1b;border:1px solid #3a3a3a;}"));
    artRow->addWidget(m_art, 3);
    m_card = new QLabel(right);
    m_card->setAlignment(Qt::AlignCenter);
    m_card->setMinimumWidth(150);
    m_card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_card->setStyleSheet(QStringLiteral("QLabel{background:#1b1b1b;border:1px solid #3a3a3a;}"));
    artRow->addWidget(m_card, 1);
    rv->addLayout(artRow, 3);

    m_lore = new QTextBrowser(right);
    m_lore->setMaximumHeight(90);
    m_lore->setStyleSheet(QStringLiteral("QTextBrowser{background:#1b1b1b;color:#c8c0b0;"
                                         "border:1px solid #3a3a3a;}"));
    rv->addWidget(m_lore);

    m_stripHdr = new QLabel(QString(), right);
    m_stripHdr->setStyleSheet(QString::fromLatin1(kSubHdrQss));
    rv->addWidget(m_stripHdr);
    m_strip = new QListWidget(right);
    m_strip->setViewMode(QListView::IconMode);
    m_strip->setFlow(QListView::LeftToRight);
    m_strip->setWrapping(false);                 // one row that scrolls, as the shop's strip does
    m_strip->setMovement(QListView::Static);
    m_strip->setIconSize(QSize(64, 64));
    // 112/128, not 96/112: the caption is two lines for gendered items, and in IconMode the
    // item rect IS the grid size — a 96px tile leaves ~26px of text band, which drops the
    // second line entirely at 125% scaling.
    m_strip->setGridSize(QSize(78, 112));
    m_strip->setFixedHeight(128);
    m_strip->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Multi-select, like the contents tree below it. The strip is the pane you actually look at —
    // it has the icons — so being able to pick only one row there while the tree accepted several
    // meant the easier pane was the less capable one, and "export these three pieces" had to be
    // done in the list you can't see the pieces in.
    m_strip->setSelectionMode(QAbstractItemView::ExtendedSelection);
    rv->addWidget(m_strip);
    // Connected ONCE, here. It was made inside showBundle with Qt::UniqueConnection, which does
    // nothing for a lambda — the functor overload has no slot pointer, so Qt strips the flag and
    // connects unconditionally, silently. showBundle runs on every selection AND every debounced
    // keystroke, so the handler count grew without bound for the tab's lifetime.
    //
    // itemSelectionChanged, not currentItemChanged: with multi-select the CURRENT row is only the
    // last one touched, so mirroring it would have propagated one row out of five.
    connect(m_strip, &QListWidget::itemSelectionChanged, this, [this] { syncSelection(true); });


    m_contents = new QTreeWidget(right);
    m_contents->setColumnCount(4);
    // SLOT comes from the SNO reference graph (Item -> GearItem), not from parsing the name. That
    // matters: Bundle_HArmor_bar_stor235 contains twoHandPolearm_stor059 — a weapon from a
    // different set entirely — which any name-prefix rule would mislabel or miss.
    m_contents->setHeaderLabels({QStringLiteral("CONTENTS"), QStringLiteral("SLOT"),
                                 QStringLiteral("SNO"), QStringLiteral("RESOLVES TO")});
    m_contents->header()->setStretchLastSection(true);
    m_contents->setAlternatingRowColors(true);
    // Multi-select: click, ctrl-click, shift-range — highlight several rows to copy them (CsvCopy,
    // installed below) or to export exactly those. Any selection here (or in the strip, which is
    // kept in step) becomes the Export menu's subject; clear it to export the whole bundle again.
    m_contents->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // The other half of the mirror. Kept symmetric on purpose: the pane you clicked in is the
    // authority, whichever one that is.
    connect(m_contents, &QTreeWidget::itemSelectionChanged, this, [this] { syncSelection(false); });
    // Sortable, like the Models tab's views. Group rows are top-level so children sort within
    // their kind, which is what you want — sorting by SLOT across the whole tree would interleave
    // armour with mounts. showBundle turns this OFF while populating and back on afterwards.
    m_contents->setSortingEnabled(true);
    // Column show/hide, the same header menu the Models and Textures browsers have. Four columns
    // now, and no other way to get one back once hidden.
    m_contents->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_contents->header(), &QWidget::customContextMenuRequested, this,
            [this](const QPoint& p) {
        static const char* const kCols[4] = {"CONTENTS", "SLOT", "SNO", "RESOLVES TO"};
        QMenu menu(this);
        for (int c = 0; c < 4; ++c) {
            const bool shown = !m_contents->isColumnHidden(c);
            QAction* a = menu.addAction(QString::fromLatin1(kCols[c]));
            a->setCheckable(true);
            a->setChecked(shown);
            // Column 0 stays: hiding the name column leaves an unreadable tree with no way back
            // except this menu, which you then cannot aim at.
            a->setEnabled(c != 0);
            connect(a, &QAction::triggered, this,
                    [this, c, shown] { m_contents->setColumnHidden(c, shown); });
        }
        menu.exec(m_contents->header()->mapToGlobal(p));
    });
    rv->addWidget(m_contents, 1);

    // Double-click an item — in either pane — to open it in the Models tab, textured, with its
    // parts tree and animations. Cheaper and better than a second viewport in here.
    auto openInModels = [this](int childSno) {
        const int app = m_childApp.value(childSno, 0);
        if (app > 0) emit revealModelRequested(app);
    };
    // The strip prefers the row's OWN appearance (UserRole+1) so a "Male" row opens the male
    // appearance rather than whichever gender happened to be first for the product.
    connect(m_strip, &QListWidget::itemDoubleClicked, this, [this, openInModels](QListWidgetItem* it) {
        if (!it) return;
        const int app = it->data(Qt::UserRole + 1).toInt();
        if (app > 0) emit revealModelRequested(app);
        else         openInModels(it->data(Qt::UserRole).toInt());
    });
    // ── Context menus on all three panes ────────────────────────────────────────────────────
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    CsvCopy::install(m_list);     // policy first — see the note on m_contents above
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        QListWidgetItem* it = m_list->itemAt(p);
        if (!it) return;
        // Retarget only when the click lands OUTSIDE the selection — the rule the contents tree
        // already follows. setCurrentItem() clears the selection, so doing it unconditionally would
        // throw away a multi-bundle selection the moment you right-clicked it to export.
        if (!it->isSelected()) {
            m_list->clearSelection();
            it->setSelected(true);
            m_list->setCurrentItem(it);
        }
        const int bs = it->data(Qt::UserRole).toInt();
        // The bundle's own shop art is a real group-44 texture, so "View in Textures" can open it.
        int tex = 0;
        if (const auto* pb = StoreProductIndex::instance().product(bs)) {
            const auto ts = shopTextures(pb->name);
            if (!ts.isEmpty()) tex = ts.first().first;
        }
        showRowMenu(m_list, m_list->viewport()->mapToGlobal(p), bs, 0, tex, QString());
    });
    m_strip->setContextMenuPolicy(Qt::CustomContextMenu);
    CsvCopy::install(m_strip);    // policy first — see the note on m_contents above
    connect(m_strip, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        QListWidgetItem* it = m_strip->itemAt(p);
        if (!it) return;
        // texSno = 0 DELIBERATELY. A strip row is an ITEM; the bundle's card art is not that item's
        // texture. Passing it was harmless while the menu only offered "View in Textures", but the
        // menu can now EXPORT its texSno — so a strip row would have quietly written the bundle
        // background alongside the helmet. The bundle art is exported from the bundle's own menu,
        // where it is the actual subject.
        showRowMenu(m_strip, m_strip->viewport()->mapToGlobal(p), m_curBundle,
                    it->data(Qt::UserRole + 1).toInt(), 0,
                    it->text().split(QChar::LineSeparator).first());
    });
    m_contents->setContextMenuPolicy(Qt::CustomContextMenu);
    // ORDER MATTERS, and it is the trap ModelsTab documents twice: CsvCopy::install skips its own
    // Copy/Copy-all menu only when the view ALREADY has a context-menu policy set. Installed first,
    // it wins the signal and hides the real row menu behind its own.
    //
    // Installed on all three views here — the tab had none, on ten in ModelsTab alone, so
    // "select rows, copy them" simply did not work anywhere in the Catalogue.
    CsvCopy::install(m_contents);
    connect(m_contents, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        QTreeWidgetItem* it = m_contents->itemAt(p);
        if (!it) return;
        // Right-clicking OUTSIDE the current selection retargets it, the way every list in this app
        // (and every file manager) behaves. Without this, multi-select made it possible to right-
        // click row A while rows B and C are highlighted, and act on something you were not
        // pointing at.
        if (!it->isSelected()) {
            m_contents->clearSelection();
            it->setSelected(true);
            m_contents->setCurrentItem(it);
        }
        const int cs = it->data(0, Qt::UserRole).toInt();
        // Bundle-image rows carry their texture sno in UserRole+2 and no product sno, so passing 0
        // here left them without "View in Textures" — the one action they actually have.
        const int ts = it->data(0, Qt::UserRole + 2).toInt();
        showRowMenu(m_contents, m_contents->viewport()->mapToGlobal(p), m_curBundle,
                    m_childApp.value(cs, 0), ts, it->text(0));
    });

    connect(m_contents, &QTreeWidget::itemDoubleClicked, this,
            [this, openInModels](QTreeWidgetItem* it, int) {
                if (!it) return;
                // A bundle-image row has no product; double-clicking it used to call
                // openInModels(0) and silently do nothing. Send it to the Textures tab instead,
                // which is where that asset actually lives.
                if (const int ts = it->data(0, Qt::UserRole + 2).toInt()) {
                    emit revealTextureRequested(ts);
                    return;
                }
                openInModels(it->data(0, Qt::UserRole).toInt());
            });

    // No export buttons here. Export lives in the app's Export menu for every other tab, and this
    // was the only one carrying its own pair — so the same operation had two entry points that
    // could disagree, and the menu's "Export …" was competing with a button beside it.
    //
    // The tab exposes its capabilities through the BrowserTab virtuals instead
    // (hasExportSelection / exportSelection / exportSelectionToLast / exportNoun), which is exactly
    // how Models and Textures feed that menu. Texframes are a Settings ▸ Export option rather than
    // menu actions — see exportTexFrames.
    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);
    btnRow->addStretch(1);
    m_status = new QLabel(QString(), right);
    m_status->setStyleSheet(QStringLiteral("QLabel{color:#9a9a9a;}"));
    btnRow->addWidget(m_status);
    rv->addLayout(btnRow);

    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 5);

    // Debounced: reloadBundleList re-selects the kept bundle, which fires currentItemChanged and
    // rebuilds the whole detail pane. Wired straight to textChanged, every keystroke paid for that.
    m_searchDebounce = new QTimer(this);
    m_searchDebounce->setSingleShot(true);
    m_searchDebounce->setInterval(200);
    connect(m_searchDebounce, &QTimer::timeout, this, [this] { reloadBundleList(); });
    connect(m_search, &QLineEdit::textChanged, this, [this] { m_searchDebounce->start(); });
    connect(m_kindFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this] { reloadBundleList(); });
    connect(m_branchFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this] { reloadBundleList(); });
    connect(m_seasonFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this] { reloadBundleList(); });
    connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this] { reloadBundleList(); });
    connect(m_latestChk, &QCheckBox::toggled, this, [this] { reloadBundleList(); });
    connect(m_list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* cur, QListWidgetItem*) {
                showBundle(cur ? cur->data(Qt::UserRole).toInt() : -1);
            });

    // Hover preview: dwell, then a floating popup — the Models tab's arrangement exactly, a bare
    // QLabel with the Qt::ToolTip flag whose entire visual is painted into the pixmap. Parented to
    // `this` so it hides with the tab (Models has no hideEvent either, and relies on the same).
    m_hoverPopup = new QLabel(this, Qt::ToolTip);
    m_hoverPopup->setAlignment(Qt::AlignCenter);
    m_hoverPopup->hide();
    m_hoverTimer = new QTimer(this);
    m_hoverTimer->setSingleShot(true);
    m_hoverTimer->setInterval(HoverInfo::delayMs());
    m_previewPx = HoverInfo::previewPx();
    connect(m_hoverTimer, &QTimer::timeout, this, [this] {
        if (m_hoverSno > 0) showHoverPreview(m_hoverSno);
    });

    // Thumbnails: batched off a timer, kicked by scrolling and by every list rebuild.
    m_thumbTimer = new QTimer(this);
    m_thumbTimer->setSingleShot(true);
    m_thumbTimer->setInterval(60);
    connect(m_thumbTimer, &QTimer::timeout, this, [this] { renderVisibleThumbs(); });
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { m_thumbTimer->start(); });

    // Restore the saved view mode last: setGridView applies sizes and kicks a thumbnail pass, so
    // everything it touches has to exist by now.
    m_gridBtn->setChecked(QSettings().value(QStringLiteral("catalogue/gridView"), false).toBool());
    setGridView(m_gridBtn->isChecked());

    // The index build is asynchronous; repopulate the moment it lands rather than making the user
    // click away and back.
    connect(&StoreProductIndex::instance(), &StoreProductIndex::readyChanged,
            this, [this] {
                emit scanStatus(QString());   // clears this tab's slot in the shared toast
                reloadBundleList();
            });
    // Reported in BOTH places, and they are not redundant. The toast is where indexing status lives
    // for every other tab, and this build is the slowest of the lot — ~15,000 file opens plus a
    // 47 MB reference-graph parse — so it belongs there. The inline label stays because the toast
    // auto-hides and this tab looks empty until the index lands.
    connect(&StoreProductIndex::instance(), &StoreProductIndex::progress, this, [this](int pct) {
        if (StoreProductIndex::instance().ready()) return;
        emit scanStatus(QStringLiteral("Indexing the store catalogue… %1%").arg(pct));
        if (m_countLbl)
            m_countLbl->setText(QStringLiteral("Indexing the store catalogue… %1%").arg(pct));
    });
    // Shop art needs IconIndex, which builds asynchronously too. Without this, a bundle opened
    // before the icons land shows "(no shop art resolved)" and never repaints — the same
    // connection ModelsTab, StableTab2 and WardrobeTab2 all make.
    connect(&IconIndex::instance(), &IconIndex::readyChanged, this, [this] {
        if (m_curBundle > 0) showBundle(m_curBundle);
    });
}

QSize CatalogueTab::thumbBox() const
{
    // Portrait in both modes — the source card is ~800x1300, so a square box throws away most of
    // the tile. Grid tiles are m_gridPx wide; list rows are m_listPx tall.
    return m_gridMode ? QSize(m_gridPx, m_gridPx * 4 / 3)
                      : QSize(m_listPx * 3 / 4, m_listPx);
}

void CatalogueTab::setGridView(bool on)
{
    if (!m_list) return;
    m_gridMode = on;
    m_list->setViewMode(on ? QListView::IconMode : QListView::ListMode);
    m_list->setSpacing(on ? 6 : 0);
    m_list->setAlternatingRowColors(!on);
    QSettings().setValue(QStringLiteral("catalogue/gridView"), on);
    setThumbPx(on ? m_gridPx : m_listPx);   // re-applies sizes, clears the cache, re-renders
}

void CatalogueTab::setThumbPx(int px)
{
    if (!m_list) return;
    px = qBound(48, px, 256);
    (m_gridMode ? m_gridPx : m_listPx) = px;
    QSettings().setValue(m_gridMode ? QStringLiteral("catalogue/gridPx")
                                    : QStringLiteral("catalogue/listPx"), px);
    const QSize box = thumbBox();
    m_list->setIconSize(box);
    // Two delegates, built once, SWAPPED. setItemDelegate neither takes ownership of nor deletes
    // the outgoing delegate, so the obvious "new one each time" allocates a delegate per wheel
    // notch that survives until the view dies.
    if (!m_listDelegate) m_listDelegate = new QStyledItemDelegate(m_list);
    if (!m_gridDelegate) m_gridDelegate = new GridItemDelegate(px, m_list);
    if (m_gridMode) {
        m_list->setGridSize(GridItemDelegate::tileFor(px));
        static_cast<GridItemDelegate*>(m_gridDelegate)->setIconPx(px);
        if (m_list->itemDelegate() != m_gridDelegate) m_list->setItemDelegate(m_gridDelegate);
    } else {
        m_list->setGridSize(QSize());
        if (m_list->itemDelegate() != m_listDelegate) m_list->setItemDelegate(m_listDelegate);
    }
    // Cached pixmaps are scaled to the OLD size. Dropping them costs a re-decode of the ~20
    // visible rows and keeps memory bounded; caching at full resolution would not.
    m_thumbs.clear();
    const int rowH = m_gridMode ? GridItemDelegate::tileFor(px).height() : box.height() + 4;
    // Updates off for the sweep: 1,628 items × (setIcon + setSizeHint) is ~3,300 dataChanged
    // emissions and a relayout, per wheel notch.
    m_list->setUpdatesEnabled(false);
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* it = m_list->item(i);
        it->setIcon(QIcon());
        it->setSizeHint(m_gridMode ? GridItemDelegate::tileFor(px) : QSize(0, rowH));
    }
    m_list->setUpdatesEnabled(true);
    if (m_thumbTimer) m_thumbTimer->start();
}

// One row's asset, through the shared export pipelines. Exactly one of appSno/texSno is expected;
// both are tolerated so a row that is somehow both still writes both.
//
// No bundle sub-folder here: this is "give me that file", and burying a single GLB three folders
// deep is not what anyone means by it. Models and images still separate, because a .glb and a .png
// of the same piece would otherwise collide on stem alone.
// Every appearance/texture the given view currently has SELECTED. Handles both widget kinds the
// tab uses — the two QListWidgets (bundle list, includes strip) and the QTreeWidget (contents) —
// because the row menu is shared by all three and must mean the same thing in each.
void CatalogueTab::selectedAssets(QWidget* from, QVector<int>& appSnos, QVector<int>& texSnos) const
{
    QSet<int> apps, texs;
    if (auto* lw = qobject_cast<QListWidget*>(from)) {
        for (QListWidgetItem* it : lw->selectedItems()) {
            // Strip rows carry their own appearance in UserRole+1; bundle-list rows carry a bundle
            // sno in UserRole and no appearance, so they contribute nothing here — a bundle is
            // exported as a bundle, not as a pile of loose assets.
            if (const int a = it->data(Qt::UserRole + 1).toInt()) apps.insert(a);
        }
    } else if (auto* tw = qobject_cast<QTreeWidget*>(from)) {
        for (QTreeWidgetItem* it : tw->selectedItems()) {
            if (const int t = it->data(0, Qt::UserRole + 2).toInt()) { texs.insert(t); continue; }
            if (const int cs = it->data(0, Qt::UserRole).toInt())
                if (const int a = m_childApp.value(cs, 0)) apps.insert(a);
        }
    }
    appSnos = QVector<int>(apps.begin(), apps.end());
    texSnos = QVector<int>(texs.begin(), texs.end());
}

// The detail pane's selection as a whole. syncSelection keeps the two panes showing the same set,
// so this is usually one selection counted once; taking the union anyway means the answer does not
// depend on which pane happens to have focus — and it survives the one case the mirror cannot
// represent (a tree row whose product resolved to no appearance has no strip row to light up).
void CatalogueTab::currentRowSelection(QVector<int>& appSnos, QVector<int>& texSnos) const
{
    QSet<int> apps, texs;
    for (QWidget* w : {static_cast<QWidget*>(m_strip), static_cast<QWidget*>(m_contents)}) {
        if (!w) continue;
        QVector<int> a, t;
        selectedAssets(w, a, t);
        for (int x : a) apps.insert(x);
        for (int x : t) texs.insert(x);
    }
    appSnos = QVector<int>(apps.begin(), apps.end());
    texSnos = QVector<int>(texs.begin(), texs.end());
}

// Mirror the selection between the includes strip and the contents tree.
//
// The panes list the same products from two angles — icons across the top, SNOs and diagnostics
// below — so a highlight in one and nothing in the other read as two unrelated widgets, and left
// the Export menu's subject depending on invisible focus.
//
// Many-to-one by design: armour resolves to a female and a male appearance and therefore occupies
// TWO strip rows against ONE tree row, so selecting the tree row lights both genders (and exports
// both). Bundle-image rows live only in the tree; selecting one clears the strip, which is the
// honest answer — the strip has no counterpart to offer.
void CatalogueTab::syncSelection(bool fromStrip)
{
    if (m_syncingSel || !m_strip || !m_contents) return;
    m_syncingSel = true;

    if (fromStrip) {
        QSet<int> want;                       // product snos highlighted in the strip
        for (QListWidgetItem* it : m_strip->selectedItems())
            if (const int cs = it->data(Qt::UserRole).toInt()) want.insert(cs);
        QTreeWidgetItem* first = nullptr;
        for (int g = 0; g < m_contents->topLevelItemCount(); ++g) {
            QTreeWidgetItem* grp = m_contents->topLevelItem(g);
            for (int i = 0; i < grp->childCount(); ++i) {
                QTreeWidgetItem* row = grp->child(i);
                const int cs = row->data(0, Qt::UserRole).toInt();
                const bool on = cs != 0 && want.contains(cs);
                row->setSelected(on);         // image rows (cs == 0) clear, as they must
                if (on && !first) first = row;
            }
        }
        if (first) m_contents->scrollToItem(first);
    } else {
        QSet<int> want;
        for (QTreeWidgetItem* it : m_contents->selectedItems())
            if (const int cs = it->data(0, Qt::UserRole).toInt()) want.insert(cs);
        QListWidgetItem* first = nullptr;
        for (int i = 0; i < m_strip->count(); ++i) {
            QListWidgetItem* it = m_strip->item(i);
            const bool on = want.contains(it->data(Qt::UserRole).toInt());
            it->setSelected(on);
            if (on && !first) first = it;
        }
        if (first) m_strip->scrollToItem(first);
    }
    m_syncingSel = false;
}

// Rows — one or many — through the same ModelsTab/TexturesTab pipelines the bundle export uses, so
// every Settings ▸ Export option applies identically. A private writer here would be a second set
// of rules to keep in step.
//
// No bundle sub-folder: this is "give me those files", and burying them three folders deep is not
// what anyone means by it.
void CatalogueTab::exportRows(const QVector<int>& appSnos, const QVector<int>& texSnos,
                              bool promptDir)
{
    if (appSnos.isEmpty() && texSnos.isEmpty()) return;
    QString dir = settingsLastDir();
    if (promptDir || dir.isEmpty()) {
        dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Export to…"), dir);
        if (dir.isEmpty()) return;
        QSettings().setValue(QStringLiteral("catalogue/lastDir"), dir);
    }
    // The INDEX is the name authority, not the row text: a row may show a shop title ("The Lost
    // Zealot"), which is not a filename anyone can match back to an asset.
    QVector<QPair<int, QString>> models, textures;
    for (int a : appSnos) {
        // Group ids via the named helpers, never literals — see the note on kAppearanceGroup().
        QString nm = m_index ? m_index->nameForSno(kAppearanceGroup(), a) : QString();
        models.append({a, nm.isEmpty() ? QStringLiteral("appearance_%1").arg(a) : nm});
    }
    for (int t : texSnos) {
        QString nm = m_index ? m_index->nameForSno(kTextureGroup(), t) : QString();
        textures.append({t, nm.isEmpty() ? QStringLiteral("texture_%1").arg(t) : nm});
    }
    // Same sink as the bundle path — a row export of twelve items must not ask twelve times.
    BatchSink sink;
    sink.log = [this](const QString& line) { m_exportLog << line; };
    m_exportLog.clear();
    if (!models.isEmpty() && m_models)     m_models->bulkExport(models, dir, false, &sink);
    if (!textures.isEmpty() && m_textures) m_textures->bulkExportTextures(textures, dir, false, &sink);
    // Same setting, same behaviour as a bundle export — a row export is not a different kind of
    // export, just a smaller one. Frames go in a subfolder: a sheet's twelve crops dumped beside
    // the sheet is not what "give me that image" means.
    const int frameFiles = exportTexFrames(textures, QDir(dir).filePath(QStringLiteral("frames")));
    m_status->setText(QStringLiteral("Exported %1 model(s), %2 image(s)%3 → %4")
                          .arg(models.size()).arg(textures.size())
                          .arg(frameFiles ? QStringLiteral(", %1 texframe(s)").arg(frameFiles)
                                          : QString())
                          .arg(ViewportPartMenu::condensePath(dir)));
}

void CatalogueTab::showRowMenu(QWidget* from, const QPoint& globalPos, int bundleSno,
                               int appSno, int texSno, const QString& subjectName)
{
    const auto* b = StoreProductIndex::instance().product(bundleSno);
    if (!b) return;
    // Snapshot, do not capture `b`. product() returns a pointer into the index's QHash and
    // install() move-assigns that hash from the build thread — menu.exec() spins a nested event
    // loop, so a rebuild landing mid-menu would leave the copy actions dereferencing freed memory.
    const int     bSno  = b->sno;
    const QString bName = b->name, bTitle = b->title;
    QMenu menu(from);
    const QString title = bTitle.isEmpty() ? bName : bTitle;
    QAction* hdr = menu.addAction(subjectName.isEmpty() ? title : subjectName);
    hdr->setEnabled(false);
    menu.addSeparator();

    // Same wording and order as every other tab — MenuText owns the strings, so "Export to last
    // folder" here is character-for-character what Models, Textures and Bulk Extract show.
    const QString dir = ViewportPartMenu::condensePath(settingsLastDir());
    // Says "3 bundles" when three are ticked, because that is what the action will export. The menu
    // naming what it will do is the whole point of routing every export through one place.
    const int nBundles = int(selectedBundles().size());
    const QString what = nBundles > 1 ? QStringLiteral("%1 bundles").arg(nBundles)
                                      : QStringLiteral("bundle \"%1\"").arg(title);
    if (!dir.isEmpty())
        menu.addAction(MenuText::exportSetLast(what, dir), this, [this] { exportBundle(false); });
    menu.addAction(MenuText::exportSetPrompt(what), this, [this] { exportBundle(true); });

    // ── Row / selection export ──────────────────────────────────────────────────────────────────
    // Until now the only export here was the whole bundle, so pulling one helmet meant exporting
    // forty files and deleting thirty-nine.
    //
    // Acts on the SELECTION when there is more than one row, and on the clicked row otherwise —
    // the same rule the Export menu follows, so "selected" means one thing in this tab. Both
    // variants are offered, matching every other tab: prompt, and straight to the last folder.
    //
    // Routed through ModelsTab::bulkExport / TexturesTab::bulkExportTextures, so every
    // Settings ▸ Export option (format, quality, opposite-gender, FX/SIM scopes, filename
    // templates) applies exactly as it does elsewhere.
    QVector<int> selApps, selTexs;
    selectedAssets(from, selApps, selTexs);
    const int selCount = selApps.size() + selTexs.size();
    if (selCount < 2) {   // a single (or no) selection means "the row you clicked"
        selApps.clear(); selTexs.clear();
        if (appSno > 0) selApps << appSno;
        if (texSno > 0) selTexs << texSno;
    }
    if (!selApps.isEmpty() || !selTexs.isEmpty()) {
        menu.addSeparator();
        const int n = selApps.size() + selTexs.size();
        // The row's own name where it has one, its SNO otherwise. This used to build "image 12345"
        // and then wrap it below as image "image 12345" — the word twice. Only the bundle-list
        // pane hits it, because it is the only one that passes no subject name.
        const QString rowName = !subjectName.isEmpty()
            ? subjectName
            : QStringLiteral("SNO %1").arg(!selApps.isEmpty() ? selApps.first() : selTexs.first());
        const QString oneWhat = n > 1
            ? QStringLiteral("%1 selected item%2").arg(n).arg(n == 1 ? QString() : QStringLiteral("s"))
            : (!selApps.isEmpty() ? QStringLiteral("model \"%1\"").arg(rowName)
                                  : QStringLiteral("image \"%1\"").arg(rowName));
        if (!dir.isEmpty())
            menu.addAction(MenuText::exportSetLast(oneWhat, dir), this,
                           [this, selApps, selTexs] { exportRows(selApps, selTexs, false); });
        menu.addAction(MenuText::exportSetPrompt(oneWhat), this,
                       [this, selApps, selTexs] { exportRows(selApps, selTexs, true); });
    }
    if (appSno > 0 || texSno > 0) {
        menu.addSeparator();
        if (appSno > 0)
            menu.addAction(QStringLiteral("View in Models"), this,
                           [this, appSno] { emit revealModelRequested(appSno); });
        if (texSno > 0)
            menu.addAction(QStringLiteral("View in Textures"), this,
                           [this, texSno] { emit revealTextureRequested(texSno); });
    }

    menu.addSeparator();
    menu.addAction(ViewportPartMenu::withValue(MenuText::kCopySno, QString::number(bSno)),
                   this, [bSno] { ViewportPartMenu::copyText(QString::number(bSno)); });
    menu.addAction(ViewportPartMenu::withValue(MenuText::kCopyFileName, bName),
                   this, [bName] { ViewportPartMenu::copyText(bName); });
    if (!bTitle.isEmpty())
        menu.addAction(ViewportPartMenu::withValue(MenuText::kCopyName, bTitle),
                       this, [bTitle] { ViewportPartMenu::copyText(bTitle); });
    menu.exec(globalPos);
}

void CatalogueTab::hideHoverPreview()
{
    if (m_hoverPopup) m_hoverPopup->hide();
}

void CatalogueTab::popupPreview(const QPixmap& scaled)
{
    // Same clamping as ModelsTab::popupPreview — place at the cursor, flip near an edge, keep the
    // whole popup inside the app window and the screen.
    if (scaled.isNull() || !m_hoverPopup) return;
    m_hoverPopup->setPixmap(scaled);
    m_hoverPopup->resize(scaled.size());
    const QPoint cur = QCursor::pos();
    const QSize sz = scaled.size();
    QRect bound = window() ? window()->frameGeometry() : QRect();
    QScreen* scr = QGuiApplication::screenAt(cur);
    if (!scr) scr = QGuiApplication::primaryScreen();
    if (scr) bound = bound.isNull() ? scr->availableGeometry()
                                    : bound.intersected(scr->availableGeometry());
    QPoint pos = cur + QPoint(18, 18);
    if (!bound.isNull()) {
        if (pos.x() + sz.width()  > bound.right())  pos.setX(cur.x() - 18 - sz.width());
        if (pos.y() + sz.height() > bound.bottom()) pos.setY(cur.y() - 18 - sz.height());
        pos.setX(qBound(bound.left(), pos.x(), qMax(bound.left(), bound.right()  - sz.width())));
        pos.setY(qBound(bound.top(),  pos.y(), qMax(bound.top(),  bound.bottom() - sz.height())));
    }
    // A popup zoomed past the screen height clamps to the top edge and ends up UNDER the cursor.
    // The list viewport then gets a Leave, which hides it, which re-arms the dwell, which shows it
    // again — a ~2 Hz blink. Nudge it clear of the cursor rather than letting that loop start.
    const QRect rect(pos, sz);
    if (rect.contains(QCursor::pos())) {
        const int below = QCursor::pos().y() + 18;
        pos.setY(below + sz.height() <= (bound.isNull() ? below + sz.height() : bound.bottom())
                     ? below
                     : qMax(0, QCursor::pos().y() - 18 - sz.height()));
    }
    m_hoverPopup->move(pos);
    m_hoverPopup->show();
    m_hoverPopup->raise();
}

void CatalogueTab::hideEvent(QHideEvent* e)
{
    // The popup is a Qt::ToolTip, which carries the Qt::Window bit — hideChildren() skips it, so
    // parenting to `this` does NOT take it down with the tab. Without this, hovering a row and
    // switching tabs inside the dwell pops it up over whatever tab you landed on.
    if (m_hoverTimer) m_hoverTimer->stop();
    m_hoverSno = -1;
    hideHoverPreview();
    BrowserTab::hideEvent(e);
}

void CatalogueTab::showHoverPreview(int sno)
{
    const auto* b = StoreProductIndex::instance().product(sno);
    if (!b || !m_hoverPopup || !isVisible()) return;
    QPixmap pm;
    if (HoverInfo::imagePreview()) {
        // Decoded ONCE per hovered bundle. Scroll-zoom re-enters this per notch, and the source
        // can be an 11-megapixel hero — re-decoding it each notch is not affordable.
        if (m_hoverImgSno != sno) {
            m_hoverImg = cardImage(b->name);
            if (m_hoverImg.isNull()) m_hoverImg = heroImage(b->name);
            m_hoverImgSno = sno;
        }
        if (!m_hoverImg.isNull())
            pm = QPixmap::fromImage(m_hoverImg.scaled(m_previewPx, m_previewPx * 3 / 2,
                                                      Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    // Caption lines, painted into the same pixmap as the Models tab does — one popup, no layout.
    QStringList lines;
    QVector<const char*> cols;
    const bool colour = HoverInfo::colourCode();
    if (!b->title.isEmpty()) { lines << b->title;                    cols << HoverInfo::Col::kName; }
    lines << QStringLiteral("%1 · %2").arg(b->sno).arg(b->name);     cols << HoverInfo::Col::kFile;
    if (!b->branch.isEmpty()) { lines << QStringLiteral("patch %1").arg(b->branch);
                                cols << HoverInfo::Col::kMeta; }
    {
        QHash<int, int> perKind;
        for (int cs : b->children)
            if (const auto* c = StoreProductIndex::instance().product(cs)) ++perKind[int(c->kind)];
        QStringList parts;
        for (auto i = perKind.constBegin(); i != perKind.constEnd(); ++i)
            parts << QStringLiteral("%1 %2").arg(i.value())
                         .arg(StoreProductIndex::kindLabel(StoreProductIndex::Kind(i.key())).toLower());
        if (!parts.isEmpty()) { lines << parts.join(QStringLiteral(" · "));
                                cols << HoverInfo::Col::kSeries; }
    }
    if (lines.isEmpty() && pm.isNull()) { hideHoverPreview(); return; }
    if (!lines.isEmpty()) {
        const QFontMetrics fm(font());
        const int lh = fm.height() + 1, capH = lh * lines.size() + 6;
        int w = pm.isNull() ? 0 : pm.width();
        for (const QString& s : lines) w = qMax(w, fm.horizontalAdvance(s) + 12);
        QPixmap out(qBound(220, w, 900), pm.height() + capH);
        out.fill(QColor(26, 26, 28));
        QPainter p(&out);
        if (!pm.isNull()) p.drawPixmap((out.width() - pm.width()) / 2, 0, pm);
        p.setFont(font());
        int y = pm.height() + 3 + fm.ascent();
        for (int i = 0; i < lines.size(); ++i) {
            p.setPen(colour ? QColor(QLatin1String(cols.value(i, HoverInfo::Col::kMeta)))
                            : QColor(0xcc, 0xcc, 0xcc));
            p.drawText(6, y, fm.elidedText(lines[i], Qt::ElideMiddle, out.width() - 12));
            y += lh;
        }
        p.end();
        pm = out;
    }
    popupPreview(pm);
}

bool CatalogueTab::eventFilter(QObject* obj, QEvent* ev)
{
    if (m_list && obj == m_list->viewport()) {
        const QEvent::Type t = ev->type();
        if (t == QEvent::MouseMove) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            const QModelIndex idx = m_list->indexAt(me->position().toPoint());
            int sno = -1;
            if (idx.isValid())
                if (QListWidgetItem* it = m_list->item(idx.row())) sno = it->data(Qt::UserRole).toInt();
            if (sno != m_hoverSno) {
                m_hoverSno = sno;
                m_hoverImgSno = -1;   // different bundle → the cached decode no longer applies
                hideHoverPreview();
                if (sno > 0) m_hoverTimer->start(HoverInfo::delayMs()); else m_hoverTimer->stop();
            }
        } else if (t == QEvent::Leave) {
            m_hoverTimer->stop();
            m_hoverSno = -1;
            hideHoverPreview();
        } else if (t == QEvent::Wheel) {
            const auto* we = static_cast<QWheelEvent*>(ev);
            const int dy = we->angleDelta().y();
            // A horizontal/tilt wheel and some touchpads report y() == 0. Treating that as "down"
            // shrank the tiles a step per event AND swallowed the event.
            if (dy == 0) return BrowserTab::eventFilter(obj, ev);
            const int dir = dy > 0 ? 1 : -1;
            if (we->modifiers() & Qt::ControlModifier) {
                setThumbPx((m_gridMode ? m_gridPx : m_listPx) + dir * 12);
                return true;   // consume: Ctrl+scroll resizes the thumbnails
            }
            if (m_hoverPopup && m_hoverPopup->isVisible() && m_hoverSno > 0
                && HoverInfo::scrollZoom()) {
                m_previewPx = qBound(64, m_previewPx + dir * 24, 1024);
                showHoverPreview(m_hoverSno);
                return true;   // consume: scroll resizes the popup
            }
        }
    }
    return BrowserTab::eventFilter(obj, ev);
}

void CatalogueTab::refresh()
{
    // m_reader enables the CASC fallback — without it the ~1,800 products d4data never described
    // (the Doom collab bundles among them) are simply absent from this tab.
    StoreProductIndex::instance().ensureBuilt(Config::d4dataDir(), m_index, m_reader);
    IconIndex::instance().ensureBuilt(Config::d4dataDir(), m_reader);
    AppearanceMeta::instance().ensureBuilt(Config::d4dataDir(), m_index, m_reader);
    // Warm the texture-definition table OFF the GUI thread. Otherwise the first thumbnail decode
    // builds it inline — 34 MB read + 141k records, under its own mutex — and freezes the window
    // mid-scroll. IconIndex only warms it on a cache MISS, so on every run after the first it is
    // cold unless the Textures tab has been opened.
    if (m_reader && m_reader->isReady() && !TextureDefTable::instance().ready()) {
        CascReader* const rd = m_reader;
        std::thread([rd] { TextureDefTable::instance().ensureBuilt(rd); }).detach();
    }
    reloadBundleList();
}

void CatalogueTab::reset()
{
    // Deliberately does NOT reset StoreProductIndex. MainWindow::reload() calls reset() on every
    // tab for every storage reload — Settings OK, a rescan, anything — and the index costs a
    // ~15,000-file crawl to rebuild. Dropping its cache here defeated the cache entirely. The
    // singleton is invalidated where it should be: the game-build/d4data fingerprint guard.
    m_curBundle = -1;
    hideHoverPreview();
    m_hoverSno = -1;
    m_hoverImgSno = -1;
    m_hoverImg = QImage();
    m_nameMapsBuilt = false;
    m_appByName.clear();
    m_texByName.clear();
    m_portrait.clear();   // keyed by NAME, and a d4data switch can re-author the actor behind it
    m_thumbs.clear();   // a new build/snapshot can re-author the same bundle's art
    if (m_list) m_list->clear();
    if (m_branchFilter) {   // else a d4data switch keeps the previous snapshot's patch list
        const QSignalBlocker block(m_branchFilter);
        m_branchFilter->clear();
        m_branchFilter->addItem(QStringLiteral("Any patch"), QString());
    }
}

// Jump to one bundle, from a "Sold in" link in the Models tab.
//
// The filters are CLEARED first. A bundle reached this way is very often one the current filter
// excludes — you were looking at a Season 8 piece with the season filter still set to Season 3 —
// and a jump that lands on an empty list reads as a broken link rather than as a filter. Signals
// blocked so this is one rebuild, not four.
void CatalogueTab::revealBundle(int storeProductSno)
{
    buildUi();
    if (storeProductSno <= 0) return;
    if (m_search)       { QSignalBlocker b(m_search);       m_search->clear(); }
    if (m_kindFilter)   { QSignalBlocker b(m_kindFilter);   m_kindFilter->setCurrentIndex(0); }
    if (m_branchFilter) { QSignalBlocker b(m_branchFilter); m_branchFilter->setCurrentIndex(0); }
    if (m_seasonFilter) { QSignalBlocker b(m_seasonFilter); m_seasonFilter->setCurrentIndex(0); }
    if (m_latestChk)    { QSignalBlocker b(m_latestChk);    m_latestChk->setChecked(false); }
    reloadBundleList();
    if (!m_list) return;
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* it = m_list->item(i);
        if (!it || it->data(Qt::UserRole).toInt() != storeProductSno) continue;
        m_list->setCurrentItem(it);       // fires showBundle through currentItemChanged
        m_list->scrollToItem(it, QAbstractItemView::PositionAtCenter);
        return;
    }
    // Not in the list even unfiltered — it is not a bundle (a leaf product), or the index has not
    // finished. Say so rather than leaving the tab looking like it ignored the click.
    if (m_status)
        m_status->setText(QStringLiteral("Bundle %1 is not in the catalogue list "
                                         "(it may be a single item rather than a bundle)")
                              .arg(storeProductSno));
}

void CatalogueTab::reloadBundleList()
{
    if (!m_list) return;
    // The list is about to be cleared under a possibly-resting cursor, which generates no Leave.
    hideHoverPreview();
    m_hoverSno = -1;
    StoreProductIndex& idx = StoreProductIndex::instance();
    const int keep = m_curBundle;

    if (!idx.ready()) {
        m_list->clear();
        m_countLbl->setText(idx.building() ? QStringLiteral("Indexing the store catalogue…")
                                           : QStringLiteral("No d4data — set it in Settings."));
        return;
    }

    // Patch dropdown, populated once from the data rather than hard-coded.
    if (m_branchFilter->count() <= 1) {
        QStringList branches;
        for (int sno : idx.bundles())
            if (const auto* p = idx.product(sno))
                if (!p->branch.isEmpty() && !branches.contains(p->branch)) branches << p->branch;
        branches.sort();
        // Counts in the label. "Which patches actually have bundles" is otherwise invisible until
        // you pick one and see an empty list.
        QHash<QString, int> perBranch;
        for (int sno : idx.bundles())
            if (const auto* p = idx.product(sno)) ++perBranch[p->branch];
        const QSignalBlocker block(m_branchFilter);
        for (const QString& b : branches)
            m_branchFilter->addItem(QStringLiteral("%1  (%2)").arg(b).arg(perBranch.value(b)), b);
    }
    if (m_seasonFilter && m_seasonFilter->count() <= 1) {
        // Sorted by the NUMBER in the name, not the string: "Season 10" must not sort between
        // "Season 1" and "Season 2".
        QHash<int, QString> byId;
        for (int sno : idx.bundles())
            if (const auto* p = idx.product(sno))
                if (p->season && !p->seasonName.isEmpty()) byId.insert(p->season, p->seasonName);
        QVector<QPair<int, int>> ord;   // (number, sno)
        for (auto i = byId.constBegin(); i != byId.constEnd(); ++i) {
            const QString digits = QString(i.value()).remove(QRegularExpression(
                QStringLiteral("[^0-9]")));
            ord.append({digits.isEmpty() ? 9999 : digits.toInt(), i.key()});
        }
        std::sort(ord.begin(), ord.end());
        QHash<int, int> perSeason;
        for (int sno : idx.bundles())
            if (const auto* p = idx.product(sno)) ++perSeason[p->season];
        const QSignalBlocker block(m_seasonFilter);
        for (const auto& pr : ord)
            m_seasonFilter->addItem(QStringLiteral("%1  (%2)").arg(byId.value(pr.second))
                                        .arg(perSeason.value(pr.second)), pr.second);
    }

    // Restore ONCE, and only now: the patch and season combos were empty until the two blocks
    // above filled them from the index, so an earlier findData() would have matched nothing and
    // silently discarded the saved filters.
    if (!m_filtersRestored) {
        m_filtersRestored = true;
        restoreFilterState();
    }

    const QString needle = m_search->text().trimmed().toLower();
    const int wantKind   = m_kindFilter->currentData().toInt();
    const QString wantBr = m_branchFilter->currentData().toString();
    const int wantSeason = m_seasonFilter ? m_seasonFilter->currentData().toInt() : 0;
    const bool wantLatest = m_latestChk && m_latestChk->isChecked();

    // Sort BEFORE filtering, so the order is a property of the catalogue rather than of whatever
    // survived the filters. Season and patch fall back to the title, otherwise every bundle in one
    // season would come out in QHash order — different on every launch.
    QVector<int> order = idx.bundles();
    const QString sortBy = m_sortCombo ? m_sortCombo->currentData().toString()
                                       : QStringLiteral("name");
    if (sortBy != QLatin1String("name")) {
        const bool bySeason = (sortBy == QLatin1String("season"));
        std::sort(order.begin(), order.end(), [&idx, bySeason](int a, int b) {
            const auto* pa = idx.product(a);
            const auto* pb = idx.product(b);
            if (!pa || !pb) return pa != nullptr;
            // Newest first: a shop history is read from the present backwards. Unset sorts last,
            // not first — a bundle with no season is not "season zero".
            const auto keyOf = [bySeason](const StoreProductIndex::Product* p) {
                return bySeason ? p->season : 0;
            };
            if (bySeason) {
                const int ka = keyOf(pa), kb = keyOf(pb);
                if (ka != kb) return (ka == 0) ? false : (kb == 0) ? true : ka > kb;
            } else {
                if (pa->branch != pb->branch) {
                    if (pa->branch.isEmpty()) return false;
                    if (pb->branch.isEmpty()) return true;
                    return pa->branch > pb->branch;
                }
            }
            const QString ta = pa->title.isEmpty() ? pa->name : pa->title;
            const QString tb = pb->title.isEmpty() ? pb->name : pb->title;
            const int c = ta.compare(tb, Qt::CaseInsensitive);
            return c != 0 ? c < 0 : a < b;
        });
    }

    m_list->clear();
    int shown = 0;
    for (int sno : order) {
        const auto* b = idx.product(sno);
        if (!b) continue;
        if (!wantBr.isEmpty() && b->branch != wantBr) continue;
        if (wantSeason && b->season != wantSeason) continue;
        // "Latest" is per-SNO and lives on the index, exactly as Models and Bulk Extract use it.
        if (wantLatest && !(m_index && m_index->isNew(sno))) continue;
        if (wantKind >= 0) {
            bool has = false;
            for (int cs : b->children)
                if (const auto* c = idx.product(cs))
                    if (int(c->kind) == wantKind) { has = true; break; }
            if (!has) continue;
        }
        if (!needle.isEmpty()) {
            QString hay = b->name + QLatin1Char(' ') + b->title + QLatin1Char(' ') + b->description;
            if (!hay.toLower().contains(needle)) continue;
        }
        const QString label = b->title.isEmpty() ? b->name : b->title;
        // Two lines, as the shop's cards are: NAME above, then what it is. The shop's second line
        // is hBundleTypeLabel ("EQUIPMENT" / "MEGA BUNDLE"), which is a string-label hash we cannot
        // resolve yet — a summary of the actual contents is more informative anyway, and honest.
        QSet<int> kinds;
        for (int cs : b->children)
            if (const auto* c = idx.product(cs)) kinds.insert(int(c->kind));
        QStringList kindNames;
        for (int k : kinds)
            if (k != StoreProductIndex::None)
                kindNames << StoreProductIndex::kindLabel(StoreProductIndex::Kind(k)).toLower();
        std::sort(kindNames.begin(), kindNames.end());
        const QString second = QStringLiteral("%1 item%2%3")
            .arg(b->children.size()).arg(b->children.size() == 1 ? QString() : QStringLiteral("s"))
            .arg(kindNames.isEmpty() ? QString()
                                     : QStringLiteral("  ·  ") + kindNames.join(QStringLiteral(", ")));
        // QChar::LineSeparator, NOT '\n': QTextLayout breaks on U+2028, and QStyledItemDelegate
        // (unlike QItemDelegate) does not translate a raw newline into it.
        auto* it = new QListWidgetItem(
            label + QChar(QChar::LineSeparator) + second, m_list);
        it->setData(Qt::UserRole, sno);
        // Row height pinned explicitly. setUniformItemSizes samples ONE row (the last) for the
        // height it applies to all of them, and the delegate only adds decorationSize when that
        // row actually has an icon — which the last row, far below the viewport, never does. The
        // thumbnails were being squeezed into a ~22px text-height row. Per MODE: a grid tile is
        // square-ish and sized by the delegate, a list row only needs a height.
        it->setSizeHint(m_gridMode ? GridItemDelegate::tileFor(m_gridPx)
                                   : QSize(0, thumbBox().height() + 4));
        it->setToolTip(QStringLiteral("%1\nSNO %2%3")
                           .arg(b->name).arg(sno)
                           .arg(b->branch.isEmpty() ? QString()
                                                    : QStringLiteral("\nPatch %1").arg(b->branch)));
        ++shown;
        if (sno == keep) m_list->setCurrentItem(it);
    }
    m_countLbl->setText(QStringLiteral("%1 of %2 bundles").arg(shown).arg(idx.bundles().size()));
    rebuildFilterChips();
    saveFilterState();   // one honest place to persist; no-ops unless "Remember" is on
    if (m_thumbTimer) m_thumbTimer->start();   // the visible rows changed — fill their icons
}

// One removable pill per active filter, matching the Models and Textures tabs. Clicking a chip
// clears that filter through its own widget, so the change routes back through the normal
// currentIndexChanged path rather than a second, divergent reset.
void CatalogueTab::rebuildFilterChips()
{
    if (!m_chipRow) return;
    while (QLayoutItem* it = m_chipRow->takeAt(0)) {
        if (QWidget* w = it->widget()) w->deleteLater();
        delete it;
    }
    auto addChip = [this](const QString& text, std::function<void()> clear) {
        // Parent to a real widget, not m_chipRow->parentWidget(): a nested layout has no parent
        // widget until the whole hierarchy is installed, and a null parent would briefly create a
        // top-level window before addWidget reparents it.
        auto* chip = new QToolButton(m_list ? m_list->parentWidget() : nullptr);
        chip->setText(text + QStringLiteral("  ✕"));
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(QStringLiteral("Remove this filter"));
        // Byte-identical to the Models/Textures chip styling — the same control must not look
        // like a different widget depending on which tab you are in.
        chip->setStyleSheet(QStringLiteral(
            "QToolButton{background:#2b2b2b; border:1px solid #555; border-radius:3px;"
            " padding:1px 6px; color:#d8a23a;}"
            "QToolButton:hover{border-color:#b0453c;}"));
        connect(chip, &QToolButton::clicked, this, [clear]() { clear(); });
        m_chipRow->addWidget(chip);
    };
    if (m_search && !m_search->text().trimmed().isEmpty())
        addChip(QStringLiteral("\"%1\"").arg(m_search->text().trimmed()),
                [this] { m_search->clear(); });
    if (m_kindFilter && m_kindFilter->currentIndex() > 0)
        addChip(m_kindFilter->currentText(), [this] { m_kindFilter->setCurrentIndex(0); });
    if (m_branchFilter && m_branchFilter->currentIndex() > 0)
        addChip(m_branchFilter->currentText(), [this] { m_branchFilter->setCurrentIndex(0); });
    if (m_seasonFilter && m_seasonFilter->currentIndex() > 0)
        addChip(m_seasonFilter->currentText(), [this] { m_seasonFilter->setCurrentIndex(0); });
    if (m_latestChk && m_latestChk->isChecked())
        addChip(QStringLiteral("Latest"), [this] { m_latestChk->setChecked(false); });
    m_chipRow->addStretch(1);
    updateFunnelTint();
}

// Gold-tint the funnel while any filter is active — the same signal Models, Textures and Wardrobe
// give. The chips say WHICH filters are on; the tint is what you see when the chip row has scrolled
// out of your attention, and it is the difference between "this list is short" and "this list is
// filtered". The search box is excluded on purpose: its own text is already visible.
void CatalogueTab::updateFunnelTint()
{
    if (!m_filtersToggle) return;
    const bool active = (m_kindFilter   && m_kindFilter->currentIndex()   > 0)
                     || (m_branchFilter && m_branchFilter->currentIndex() > 0)
                     || (m_seasonFilter && m_seasonFilter->currentIndex() > 0)
                     || (m_latestChk    && m_latestChk->isChecked());
    m_filtersToggle->setStyleSheet(active
        ? QStringLiteral("QToolButton{padding:1px;border:1px solid #a07a1a;border-radius:3px;"
                         "background:#3a2f12;} QToolButton:hover{border-color:#b0453c;}")
        : QString::fromLatin1(kIconBtnQss));
}

void CatalogueTab::saveFilterState()
{
    QSettings s;
    if (!s.value(QStringLiteral("catalogue/rememberFilters"), false).toBool()) return;
    s.setValue(QStringLiteral("catalogue/lastSearch"), m_search ? m_search->text() : QString());
    // The SNO/data values, never the display text: the combo labels now carry live counts
    // ("Season 8  (74)") and the localized names change between builds, so findText would
    // silently resolve to nothing. This is the trap the codebase already documents for restores.
    s.setValue(QStringLiteral("catalogue/lastKind"),
               m_kindFilter ? m_kindFilter->currentData().toInt() : -1);
    s.setValue(QStringLiteral("catalogue/lastBranch"),
               m_branchFilter ? m_branchFilter->currentData().toString() : QString());
    s.setValue(QStringLiteral("catalogue/lastSeason"),
               m_seasonFilter ? m_seasonFilter->currentData().toInt() : 0);
    s.setValue(QStringLiteral("catalogue/lastSort"),
               m_sortCombo ? m_sortCombo->currentData().toString() : QString());
    s.setValue(QStringLiteral("catalogue/lastLatest"), m_latestChk && m_latestChk->isChecked());
}

// Called ONCE, after the patch/season combos have been populated from the index — restoring
// before that would findData() against empty lists and silently select nothing.
void CatalogueTab::restoreFilterState()
{
    QSettings s;
    if (!s.value(QStringLiteral("catalogue/rememberFilters"), false).toBool()) return;
    auto pick = [](QComboBox* c, const QVariant& data) {
        if (!c) return;
        const int i = c->findData(data);
        if (i >= 0) { QSignalBlocker b(c); c->setCurrentIndex(i); }
    };
    pick(m_kindFilter,   s.value(QStringLiteral("catalogue/lastKind"), -1));
    pick(m_branchFilter, s.value(QStringLiteral("catalogue/lastBranch")));
    pick(m_seasonFilter, s.value(QStringLiteral("catalogue/lastSeason"), 0));
    pick(m_sortCombo,    s.value(QStringLiteral("catalogue/lastSort")));
    if (m_latestChk) {
        QSignalBlocker b(m_latestChk);
        m_latestChk->setChecked(s.value(QStringLiteral("catalogue/lastLatest"), false).toBool());
    }
    if (m_search) {
        QSignalBlocker b(m_search);
        m_search->setText(s.value(QStringLiteral("catalogue/lastSearch")).toString());
    }
}

void CatalogueTab::showBundle(int sno)
{
    m_curBundle = sno;
    // The strip↔contents mirror must not run while the panes are being rebuilt: clear() and every
    // insert emit itemSelectionChanged, and a half-filled tree would answer on behalf of a strip
    // that has not been filled yet. Held for the whole of showBundle, released on every exit path
    // including the early return below.
    m_syncingSel = true;
    struct SyncGuard { CatalogueTab* t; ~SyncGuard() { t->m_syncingSel = false; } } sguard{this};
    // Sorting OFF while the tree is filled. With it on, QTreeWidget re-sorts on every insertion —
    // rows shuffle as they are added, group children can land under the wrong parent's position,
    // and population is needlessly O(n log n) per item. Re-enabled once the tree is complete.
    m_contents->setSortingEnabled(false);
    m_contents->clear();
    if (m_strip) m_strip->clear();
    if (m_stripHdr) m_stripHdr->clear();
    m_childApp.clear();
    m_card->clear();   // else filtering to zero results leaves the PREVIOUS bundle's card up
    m_art->setPixmap(QPixmap());
    m_art->setText(QString());
    m_lore->clear();
    m_status->clear();

    const auto* b = StoreProductIndex::instance().product(sno);
    if (!b) {
        m_title->setText(QStringLiteral("Select a bundle"));
        m_subtitle->clear();
        // Sorting was switched OFF above for the repopulate. Returning here without restoring it
        // left the contents tree permanently unsortable until the next bundle that DID resolve —
        // reachable by selecting nothing, or by an index rebuild dropping the current bundle.
        m_contents->setSortingEnabled(true);
        return;
    }

    m_title->setText(b->title.isEmpty() ? b->name : b->title);
    QStringList sub{ b->name, QStringLiteral("SNO %1").arg(b->sno) };
    if (!b->branch.isEmpty()) sub << QStringLiteral("patch %1").arg(b->branch);
    if (!b->seasonName.isEmpty()) sub << b->seasonName;
    // The shop's "Supported Classes" line. fPreviewOnClasses was sitting in the data unused.
    const QString cls = StoreProductIndex::classSummary(b->classMask);
    if (!cls.isEmpty()) sub << cls;
    if (b->hasVfx) sub << QStringLiteral("VFX included");   // the shop calls it out; so do we
    // Recovered from the game's own files because the d4data snapshot has no record of it. Said
    // out loud, because otherwise this bundle just looks half-broken: no shop title, no lore, no
    // season, no supported-classes line. Those live ONLY in the snapshot's JSON and string tables,
    // so their absence here is a property of the source, not a failure of the read — and a user
    // has no way to tell those apart without being told.
    if (b->fromCasc) sub << QStringLiteral("read from game files — no shop text in this snapshot");
    m_subtitle->setText(sub.join(QStringLiteral("  ·  ")));
    if (!b->description.isEmpty()) m_lore->setPlainText(b->description);
    else if (b->fromCasc)
        m_lore->setPlainText(QStringLiteral(
            "This bundle is not in the d4data snapshot, so its shop name, description, season and "
            "supported classes are unavailable — those exist only in the snapshot's text files. "
            "Everything below comes from the game itself and is complete: contents, artwork and "
            "models all export normally.\n\n"
            "Re-running File ▸ Dependencies… once d4data catches up with your game build will fill "
            "in the missing text."));

    // ── Provenance and relationships ────────────────────────────────────────────────────────────
    // Everything below comes from fields the .prd has always carried and this tab never read.
    // arRequiresOwning is the interesting one: mnt_stor158_trophy requires Battlepass_Season3_Premium
    // — i.e. it was never sold at all, it was a Season 3 premium pass reward. Nothing else in the
    // tool can tell you that.
    {
        const StoreProductIndex& idx = StoreProductIndex::instance();
        auto nameOf = [&idx](int ps) {
            const auto* p = idx.product(ps);
            if (!p) return QStringLiteral("SNO %1").arg(ps);
            return p->title.isEmpty() ? p->name : p->title;
        };
        auto listOf = [&nameOf](const QVector<int>& v) {
            QStringList out;
            for (int ps : v) out << nameOf(ps);
            return out.join(QStringLiteral(", "));
        };
        QStringList rel;
        if (!b->requires_.isEmpty())
            rel << QStringLiteral("Requires: %1").arg(listOf(b->requires_));
        if (!b->addOns.isEmpty())
            rel << QStringLiteral("Add-on to: %1").arg(listOf(b->addOns));
        if (!b->requiresNot.isEmpty())
            rel << QStringLiteral("Excludes: %1").arg(listOf(b->requiresNot));
        if (!rel.isEmpty()) {
            QString txt = m_lore->toPlainText();
            if (!txt.isEmpty()) txt += QStringLiteral("\n\n");
            m_lore->setPlainText(txt + rel.join(QStringLiteral("\n")));
        }
    }

    // Laid out the way the shop page is: the wide hero banner across the top, the portrait card
    // beside it. Scaled to the label's ACTUAL size rather than a hardcoded 260px — the hero is
    // 5120x2160, so a fixed small box was throwing away almost all of it.
    auto fit = [](const QImage& img, const QSize& box) {
        return QPixmap::fromImage(img).scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    };
    const QSize heroBox(qMax(320, m_art->width() - 8), qMax(160, m_art->height() - 8));
    const QImage card = cardImage(b->name);
    QImage hero = heroImage(b->name);
    // Tracked with a flag, not by comparing images: falling back to the card produces a separate
    // QImage, so a cacheKey comparison would never notice and the same picture would appear twice.
    bool heroIsCard = false;
    if (hero.isNull()) { hero = card; heroIsCard = !hero.isNull(); }
    if (hero.isNull())
        // arCardArtVariants BEFORE the generic handles: a card variant is real shop art for THIS
        // product ({hCardImage, hCardHoverImage}), while b->art's tail is mostly category icons
        // shared across dozens of products. The hover variant in particular appears nowhere else.
        for (quint32 h : b->cardArt)
            if (!(hero = IconIndex::instance().iconImage(h, m_reader)).isNull()) break;
    if (hero.isNull())
        for (quint32 h : b->art)   // last resort: a UI handle, usually a category icon
            if (!(hero = IconIndex::instance().iconImage(h, m_reader)).isNull()) break;
    if (hero.isNull()) {
        m_art->setText(QStringLiteral("(no shop art resolved)"));
    } else {
        // Title ENGRAVED over the art, the way the shop lays the panel out, rather than only as a
        // separate line above it. Drawn with a dark outline so it stays readable over bright art.
        // The shop engraves the title over the art because the art IS its header — there is no
        // separate title line. Here m_title already shows the same string directly above, so
        // painting it on again was the same words twice, forty pixels apart. Art only.
        m_art->setPixmap(fit(hero, heroBox));
    }

    if (!card.isNull() && !heroIsCard)
        m_card->setPixmap(fit(card, QSize(qMax(120, m_card->width() - 4),
                                          qMax(160, m_card->height() - 4))));
    else
        m_card->clear();

    // ONE resolve for the whole pane. showBundle used to call appearancesFor per transmog child to
    // label the tree, and then resolveBundle called it again per child for the counts — double the
    // work for the same answer. resolveBundle now reports the per-child breakdown it already had.
    const Resolved r = resolveBundle(*b);

    // ── The contents strip: "INCLUDES 8 ITEMS:" ──────────────────────────────────────────────
    StoreProductIndex& idx = StoreProductIndex::instance();
    m_strip->clear();
    m_childApp = r.firstApp;       // remembered for the "open in Models" double-click
    m_stripHdr->setText(QStringLiteral("INCLUDES %1 ITEM%2:")
                            .arg(b->children.size())
                            .arg(b->children.size() == 1 ? QString() : QStringLiteral("S")));
    for (int cs : b->children) {
        const auto* c = idx.product(cs);
        const QString label = c ? (c->title.isEmpty() ? c->name : c->title)
                                : QStringLiteral("product %1").arg(cs);
        // The item's OWN inventory icon, exactly what the Models tab shows — not the product's
        // art handles. c->art leads with hCategoryIcon, which is a generic per-class glyph shared
        // by every product of that type, so the strip was a row of identical class symbols instead
        // of the actual pieces.
        //
        // Per APPEARANCE, not per product: armour resolves to a female and a male appearance with
        // different icons, and one icon reused across both rows would be wrong for one of them.
        // Falls back to the product art only when the appearance has no icon.
        QIcon fallbackIcon;
        if (c)
            for (quint32 h : c->art) {
                const QImage ic = IconIndex::instance().iconImage(h, m_reader);
                if (!ic.isNull()) { fallbackIcon = QIcon(QPixmap::fromImage(ic)); break; }
            }
        // Last resort: the payload actor's own portrait. This is what headstones have INSTEAD of
        // the two routes above, and without it they were the one kind that reached the strip
        // blank — see portraitFor.
        if (fallbackIcon.isNull() && c)
            if (const quint32 ph = portraitFor(payloadNameOf(*c))) {
                const QImage ic = IconIndex::instance().iconImage(ph, m_reader);
                if (!ic.isNull()) fallbackIcon = QIcon(QPixmap::fromImage(ic));
            }
        auto iconForApp = [this, &fallbackIcon](int appSno) {
            if (appSno > 0 && AppearanceMeta::instance().ready()) {
                const quint32 h = AppearanceMeta::instance().iconFor(appSno);
                if (h) {
                    const QImage im = IconIndex::instance().iconImage(h, m_reader);
                    if (!im.isNull()) return QIcon(QPixmap::fromImage(im));
                }
            }
            return fallbackIcon;
        };
        const QIcon qi = iconForApp(r.firstApp.value(cs, 0));

        // ONE ROW PER APPEARANCE, not per product. Armour resolves to a female and a male
        // appearance from the same item, and listing only the product hid one of them — you could
        // neither see nor open the other gender. Weapons and mounts resolve to a single appearance
        // and so still show a single row.
        const QVector<QPair<int, QString>> apps = r.appsOf.value(cs);
        if (apps.size() > 1) {
            for (const auto& a : apps) {
                // Gender read off the appearance name's 4th character (barF_… / barM_…), which is
                // the convention cosmeticAppearanceNames builds them with.
                const QChar g = a.second.size() > 3 ? a.second.at(3).toUpper() : QChar();
                const QString suffix = g == QLatin1Char('F') ? QStringLiteral("Female")
                                     : g == QLatin1Char('M') ? QStringLiteral("Male")
                                                             : a.second;
                // QChar::LineSeparator, not '\n' — the strip uses the stock QStyledItemDelegate,
                // which (unlike QItemDelegate) does not translate a raw newline into a break.
                auto* si = new QListWidgetItem(
                    label + QChar(QChar::LineSeparator) + suffix, m_strip);
                si->setData(Qt::UserRole, cs);
                si->setData(Qt::UserRole + 1, a.first);   // the exact appearance this row opens
                si->setIcon(iconForApp(a.first));   // this gender's own icon
                si->setToolTip(QStringLiteral("%1\n%2\nappearance %3")
                                   .arg(label, a.second).arg(a.first));
            }
            continue;
        }

        auto* si = new QListWidgetItem(label, m_strip);
        si->setData(Qt::UserRole, cs);
        if (!apps.isEmpty()) si->setData(Qt::UserRole + 1, apps.first().first);
        si->setIcon(qi);
        QStringList tip{ label };
        if (c) {
            tip << c->name << StoreProductIndex::kindLabel(c->kind);
            const QString cc = StoreProductIndex::classSummary(c->classMask);
            if (!cc.isEmpty()) tip << cc;
        }
        si->setToolTip(tip.join(QLatin1Char('\n')));
    }
    // Children grouped by kind — the whole reason to look at a bundle rather than its armour.
    QHash<int, QTreeWidgetItem*> groups;
    for (int cs : b->children) {
        const auto* c = idx.product(cs);
        const QString cname = c ? (c->title.isEmpty() ? c->name : c->title)
                                : QStringLiteral("product %1").arg(cs);
        const int kind = c ? int(c->kind) : int(StoreProductIndex::None);
        QTreeWidgetItem*& g = groups[kind];
        if (!g) {
            g = new QTreeWidgetItem(m_contents,
                    QStringList{StoreProductIndex::kindLabel(StoreProductIndex::Kind(kind))});
            g->setFirstColumnSpanned(true);
            g->setExpanded(true);
        }
        // payloadNameOf, not payloadName: a CASC-recovered product carries only a sno, and the
        // RESOLVES TO column was computed through the resolver — so showing the raw field here
        // produced rows reading "  →  2 appearances" with nothing in front of the arrow.
        const QString pname = c ? payloadNameOf(*c) : QString();
        auto* row = new QTreeWidgetItem(g, QStringList{
            cname, c ? c->slot : QString(), QString::number(cs), pname});
        row->setData(0, Qt::UserRole, cs);
        // No icon here on purpose: the strip above already shows every item's art, and repeating
        // it in the tree just made the rows tall and the two panes redundant.
        if (c && c->kind == StoreProductIndex::Transmog) {
            const int n = r.appsPerChild.value(cs, 0);
            row->setText(3, n == 0
                                ? QStringLiteral("%1  (no appearance found)").arg(pname)
                                : QStringLiteral("%1  →  %2 appearance%3")
                                      .arg(pname).arg(n)
                                      .arg(n == 1 ? QString() : QStringLiteral("s")));
        }
    }
    // ── Bundle images, as their own branch ──────────────────────────────────────────────────────
    // The shop art was counted in the status line and written by the exporter, but never listed —
    // so there was no way to see which images a bundle actually has, or to select one. These are
    // the 2DUI_/2DInventory_ textures shopTextures already resolves by name:
    //     2DInventory_Bundle_HArmor_sor_stor268
    //     2DUI_Bundle_HArmor_sor_stor268            (+ _background, _details, _webImage)
    if (!r.textures.isEmpty()) {
        auto* g = new QTreeWidgetItem(m_contents, QStringList{QStringLiteral("Bundle images")});
        g->setFirstColumnSpanned(true);
        g->setExpanded(true);
        for (const auto& t : r.textures) {
            auto* row = new QTreeWidgetItem(g, QStringList{
                t.second, QStringLiteral("image"), QString::number(t.first), QString()});
            // UserRole+2 marks a texture row: the context menu and export path must treat these as
            // textures, not as store products (UserRole holds a product sno for every other row).
            row->setData(0, Qt::UserRole + 2, t.first);
            row->setToolTip(0, QStringLiteral("%1\ntexture %2").arg(t.second).arg(t.first));
        }
    }

    m_contents->setSortingEnabled(true);   // tree is complete — safe to sort now
    m_contents->resizeColumnToContents(0);
    m_contents->resizeColumnToContents(1);   // SLOT
    m_contents->resizeColumnToContents(2);   // SNO — was column 1 before SLOT was inserted

    m_status->setText(QStringLiteral("%1 model(s) · %2 texture(s) · %3 art image(s)%4")
                          .arg(r.models.size()).arg(r.textures.size()).arg(r.artHandles.size())
                          .arg(r.unresolved.isEmpty()
                                   ? QString()
                                   : QStringLiteral(" · %1 unresolved").arg(r.unresolved.size())));
}

QVector<QPair<int, QString>> CatalogueTab::shopTextures(const QString& bundleName) const
{
    QVector<QPair<int, QString>> out;
    if (!m_index) return out;
    ensureNameMaps();
    QString bare = bundleName;
    if (bare.startsWith(QLatin1String("Bundle_"), Qt::CaseInsensitive)) bare = bare.mid(7);
    // Order matters — the caller takes the first hit as the preview. The bare 2DUI_ tile is the
    // shop's own thumbnail and the smallest of the set, so it is both the right image and the
    // cheapest to decode; _background and _WebImage are full-bleed art at up to 2048².
    QStringList want;
    for (const char* sfx : kUiSuffixes)
        want << QStringLiteral("2DUI_Bundle_") + bare + QString::fromLatin1(sfx);
    want << QStringLiteral("2DInventory_Bundle_") + bare;
    for (const QString& w : want) {
        const auto it = m_texByName.constFind(w.toLower());
        if (it != m_texByName.constEnd()) out.append(it.value());
    }
    return out;
}

QImage CatalogueTab::largestFrame(int sno, const QString& name) const
{
    const QImage full = decodeTexture(sno, name);
    if (full.isNull()) return full;
    const TexMeta meta = TexturesTab::texMetaFor(m_reader, Config::d4dataDir(), name, sno);
    if (meta.frames.size() < 2) return full;   // single-frame art is the picture
    // Biggest frame by area. On the card sheet that is one of the two full portrait cards rather
    // than a badge; picking frame 0 blindly would sometimes land on a 200px sticker.
    const TexFrame* best = nullptr;
    double bestArea = 0.0;
    for (const TexFrame& f : meta.frames) {
        const double a = double(f.u1 - f.u0) * double(f.v1 - f.v0);
        if (a > bestArea) { bestArea = a; best = &f; }
    }
    if (!best || bestArea <= 0.0) return full;
    const QRect r(qRound(best->u0 * full.width()), qRound(best->v0 * full.height()),
                  qRound((best->u1 - best->u0) * full.width()),
                  qRound((best->v1 - best->v0) * full.height()));
    const QRect clipped = r.intersected(full.rect());
    return clipped.isEmpty() ? full : full.copy(clipped);
}

QImage CatalogueTab::heroImage(const QString& bundleName) const
{
    // Widest-first: the shop page leads with the background art, and _WebImage is the same picture
    // at half scale. _details is the fallback banner.
    QString bare = bundleName;
    if (bare.startsWith(QLatin1String("Bundle_"), Qt::CaseInsensitive)) bare = bare.mid(7);
    for (const char* sfx : {"_background", "_WebImage", "_details"}) {
        const QString want = (QStringLiteral("2DUI_Bundle_") + bare + QString::fromLatin1(sfx)).toLower();
        const auto it = m_texByName.constFind(want);
        if (it == m_texByName.constEnd()) continue;
        const QImage img = decodeTexture(it.value().first, it.value().second);
        if (!img.isNull()) return img;
    }
    return {};
}

QImage CatalogueTab::cardImage(const QString& bundleName) const
{
    QString bare = bundleName;
    if (bare.startsWith(QLatin1String("Bundle_"), Qt::CaseInsensitive)) bare = bare.mid(7);
    const auto it = m_texByName.constFind((QStringLiteral("2DUI_Bundle_") + bare).toLower());
    if (it != m_texByName.constEnd()) {
        const QImage img = largestFrame(it.value().first, it.value().second);
        if (!img.isNull()) return img;
    }
    // No card sheet: the inventory sheet's biggest frame is a reasonable stand-in, and for a
    // single-item product it IS the icon.
    const auto inv = m_texByName.constFind((QStringLiteral("2DInventory_Bundle_") + bare).toLower());
    if (inv != m_texByName.constEnd()) return largestFrame(inv.value().first, inv.value().second);
    return {};
}

QImage CatalogueTab::decodeTexture(int sno, const QString& name) const
{
    // Via MaterialDecode, which resolves dimensions from the .tex.json when d4data has one and
    // from the CASC texture tables when it does not — so encrypted shop art decodes here too.
    //
    // SEH-guarded, because scrolling the list now feeds EVERY bundle in the catalogue through the
    // BC decoder automatically. A malformed payload after a game patch must cost one blank
    // thumbnail, not the process — the same reasoning as the texture grid's worker.
    QImage img;
    seh::runGuarded("catalogueThumb", [&] {
        img = MaterialDecode::texture(m_reader, Config::d4dataDir(), name, sno);
    });
    return img;
}

void CatalogueTab::renderVisibleThumbs()
{
    if (!m_list || !m_index || !m_reader || !m_reader->isReady()) return;
    ensureNameMaps();
    // Do NOT run before the SNO index has entries. shopTextures would return empty for every
    // bundle, each would be cached as a null "tried and failed", and nothing short of reset()
    // would ever retry — the whole catalogue permanently art-less because of one early tick.
    if (m_texByName.isEmpty()) return;
    // The first call into MaterialDecode with no .tex.json builds TextureDefTable: a 34 MB read
    // and a 141k-record parse, HELD UNDER ITS MUTEX. On the GUI thread that is a multi-second
    // freeze on the first scroll. refresh() warms it on a worker; until then, wait rather than
    // block. Every bundle newer than the d4data snapshot takes that branch, so this is the norm.
    if (!TextureDefTable::instance().ready()) {
        // refresh() may have run before the reader was ready, in which case nothing ever started
        // the warm thread and this would re-arm at ~16 Hz forever. Kick it from here as well, and
        // back off while waiting.
        CascReader* const rd = m_reader;
        static std::atomic<bool> s_warming{false};
        bool expected = false;
        if (s_warming.compare_exchange_strong(expected, true))
            std::thread([rd] { TextureDefTable::instance().ensureBuilt(rd); s_warming = false; }).detach();
        m_thumbTimer->start(400);
        return;
    }

    const QRect vp = m_list->viewport()->rect();
    if (vp.isEmpty()) return;   // tab not shown yet — nothing is visible to fill
    // Visibility is tested per item rather than by probing for a first/last row. IconMode lays out
    // from (spacing, spacing) with the same gutter between rows, so a probe at y=2 lands in that
    // gutter whenever a gap sits at the top of the viewport; the probe then failed, `first` fell
    // back to 0, and the whole decode budget went on rows ABOVE the viewport — re-armed forever,
    // and restarted from scratch by every Ctrl+wheel because the zoom clears the cache.
    int budget = 8;   // per tick; the timer re-arms while work remains so the UI never stalls
    bool more = false;
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* it = m_list->item(i);
        if (!it) continue;
        const QRect ir = m_list->visualItemRect(it);
        if (ir.bottom() < vp.top())    continue;   // above the viewport — spend nothing on it
        if (ir.top()    > vp.bottom()) break;      // below it — everything after is too
        const int sno = it->data(Qt::UserRole).toInt();
        const auto cached = m_thumbs.constFind(sno);
        if (cached != m_thumbs.constEnd()) {
            if (!cached.value().isNull() && it->icon().isNull()) it->setIcon(QIcon(cached.value()));
            continue;
        }
        if (budget <= 0) { more = true; break; }
        const auto* b = StoreProductIndex::instance().product(sno);
        QPixmap pm;
        if (b) {
            --budget;
            // The CARD, cropped out of the sheet — not the sheet. Showing the whole 808x2888
            // atlas scaled to 64px produced a sliver with two thumbnail-sized cards in it, which
            // is what "previews are way too small" was.
            QImage img = cardImage(b->name);
            if (img.isNull()) { --budget; img = heroImage(b->name); }
            if (!img.isNull())
                pm = QPixmap::fromImage(img.scaled(m_list->iconSize(), Qt::KeepAspectRatio,
                                                   Qt::SmoothTransformation));
        }
        m_thumbs.insert(sno, pm);   // null is cached too, so a bundle with no art is tried once
        if (!pm.isNull()) it->setIcon(QIcon(pm));
    }
    if (more) m_thumbTimer->start();
}

void CatalogueTab::ensureNameMaps() const
{
    if (m_nameMapsBuilt || !m_index) return;
    m_nameMapsBuilt = true;
    const auto& apps = m_index->entries(kAppearanceGroup());
    m_appByName.reserve(apps.size());
    for (const SnoEntry& e : apps) m_appByName.insert(e.name.toLower(), {e.snoId, e.name});
    const auto& texs = m_index->entries(kTextureGroup());
    m_texByName.reserve(texs.size());
    for (const SnoEntry& e : texs) m_texByName.insert(e.name.toLower(), {e.snoId, e.name});
}

QVector<QPair<int, QString>> CatalogueTab::appearancesFor(const QString& itemName) const
{
    QVector<QPair<int, QString>> out;
    if (itemName.isEmpty() || !m_index) return out;
    const QStringList want = AppearanceMeta::cosmeticAppearanceNames(itemName);
    if (want.isEmpty()) return out;
    ensureNameMaps();
    for (const QString& w : want) {
        const auto it = m_appByName.constFind(w.toLower());
        // The INDEX's canonical name, not the lowercased candidate. bulkExport uses this string as
        // the filename stem, so returning `w` wrote barf_stor150_hlm.glb where Bulk Extract writes
        // BarF_stor150_HLM.glb — the same asset under two names, which contradicts the whole point
        // of routing through the shared pipeline.
        if (it != m_appByName.constEnd()) out.append(it.value());
    }
    return out;
}

// One product's appearances, by whichever naming rule applies to it.
//
// TWO rules, tried in order, because the game uses two:
//
//  1. The ARMOUR convention. Item "Helm_Cosmetic_Barb_150_stor" is a description, not a name —
//     the meshes are barF_stor150_HLM and barM_stor150_HLM, one per gender. cosmeticAppearanceNames
//     expands it.
//  2. The item's OWN name. Item twoHandPolearm_stor059 → appearance twoHandPolearm_stor059,
//     verbatim, single, genderless.
//
// Rule 2 is why weapons were invisible here: cosmeticAppearanceNames matches
// `helm|chest|gloves|pants|boots` and nothing else, so every weapon transmog fell straight through
// it and the product was reported unresolved — no icon in the strip, no "View in Models", and no
// .glb in the export. Verified against the snapshot: 493 appearances across the weapon families
// (axe/dagger/mace/sword/two-handers/…) are named exactly as their item is.
//
// Mounts, trophies and companions have always used rule 2; they just reached it through a separate
// copy of the lookup in resolveBundle's default branch. Both branches call this now, so a future
// kind cannot be given one rule and not the other.
// ── Headstones: the art is on the ACTOR ─────────────────────────────────────────────────────────
// A headstone product is the one shop kind that reaches the strip with nothing to draw:
//
//   · it has no Item, so AppearanceMeta — which binds icons from Item.tInvImages — has none;
//   · every art handle on the product itself (hTileImage, hIconRepresentation, hCategoryIcon,
//     hSplashImage, …) is 0, so the product-art fallback finds nothing either.
//
// Its picture lives on the Actor, as hPortraitImage — a field nothing in this tool read. Measured
// on the snapshot: 90 of 90 headstone actors carry a DISTINCT non-zero handle, so it is per-item
// art rather than a shared category glyph. Portals, emblems, pets, companions and mounts all carry
// 0 there, which is why this is a last-resort fallback and not a new primary route: for every other
// kind it costs one file probe and changes nothing.
//
// Read on demand — one small file per product, only for the bundle on screen — rather than by
// indexing the whole Actor folder for a field 90 assets use. Memoised in memory for the session
// (misses cached too, so a product without one is probed once, not on every repaint) and never
// written to disk.
quint32 CatalogueTab::portraitFor(const QString& payloadName) const
{
    if (payloadName.isEmpty()) return 0;
    const auto cached = m_portrait.constFind(payloadName);
    if (cached != m_portrait.constEnd()) return cached.value();
    quint32 h = 0;
    QFile f(QDir(Config::d4dataDir()).filePath(
        QStringLiteral("json/base/meta/Actor/%1.acr.json").arg(payloadName)));
    if (f.open(QIODevice::ReadOnly)) {
        // Text scan, not a full parse: an actor file runs to thousands of lines and the field is
        // a plain integer. Same approach AppearanceMeta's actor pass already takes.
        const QByteArray t = f.readAll();
        const int k = t.indexOf("\"hPortraitImage\":");
        if (k >= 0) {
            int i = k + 17;   // past the key and its colon
            while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
            int e = i;
            while (e < t.size() && t[e] >= '0' && t[e] <= '9') ++e;
            if (e > i) h = t.mid(i, e - i).toUInt();
        }
    }
    m_portrait.insert(payloadName, h);
    return h;
}

QVector<QPair<int, QString>> CatalogueTab::appearancesForProduct(const QString& payloadName) const
{
    QVector<QPair<int, QString>> out = appearancesFor(payloadName);
    if (!out.isEmpty() || payloadName.isEmpty()) return out;
    // Route 3, through the shared rule — the same call AppearanceMeta, ItemHoverIndex and
    // IconAudit make, so this tab cannot drift from them again.
    ensureNameMaps();
    for (const QString& nm : AppearanceMeta::withSelfName(QStringList(), payloadName)) {
        const auto it = m_appByName.constFind(nm.toLower());
        if (it != m_appByName.constEnd()) out.append(it.value());
    }
    return out;
}

// A product's payload NAME — the string every resolver downstream joins on.
//
// Products read from d4data carry it. Products recovered from the CASC binary do NOT: the binary
// stores a raw SNO and no names at all. The lookup happens HERE rather than in the index build
// because SnoIndex::nameForSno writes a mutable lazy cache and is GUI-thread-only, which is the
// same hazard the index's gearNames snapshot exists to avoid.
//
// payloadGroup was learned from d4data's own ref objects (Transmog→Item, Headstone→Actor …), so a
// renumbered group needs no change here. An encrypted payload legitimately resolves to nothing —
// its name is withheld — and the caller treats that as unresolved, which it is.
QString CatalogueTab::payloadNameOf(const StoreProductIndex::Product& p) const
{
    if (!p.payloadName.isEmpty() || p.payloadSno <= 0 || p.payloadGroup <= 0 || !m_index)
        return p.payloadName;
    return m_index->nameForSno(p.payloadGroup, p.payloadSno);
}

CatalogueTab::Resolved CatalogueTab::resolveBundle(const StoreProductIndex::Product& b) const
{
    Resolved r;
    StoreProductIndex& idx = StoreProductIndex::instance();
    // Deduped: the status line counts these and exportBundle writes them through a QSet, so an
    // undeduped list reported more art images than it actually produced.
    QSet<quint32> seenArt;
    auto addArt = [&](quint32 h) { if (h && !seenArt.contains(h)) { seenArt.insert(h); r.artHandles.append(h); } };
    for (quint32 h : b.art) addArt(h);
    // Card-art variants export too. These are real per-product shop art the tab could not reach
    // before — the hover image especially exists nowhere else — and "export EVERYTHING related to
    // this bundle" is the tab's whole premise.
    for (quint32 h : b.cardArt) addArt(h);

    // The bundle's own shop art. Full textures rather than atlas frames, which is why they are
    // exported as textures and not as icons.
    r.textures = shopTextures(b.name);
    QSet<int> seenTex;
    for (const auto& t : r.textures) seenTex.insert(t.first);
    auto addTex = [&](int sno, const QString& name) {
        if (sno > 0 && !name.isEmpty() && !seenTex.contains(sno)) {
            seenTex.insert(sno);
            r.textures.append({sno, name});
        }
    };

    QSet<int> seenModel;
    for (int cs : b.children) {
        const auto* c = idx.product(cs);
        if (!c) { r.unresolved << QStringLiteral("product %1 (not in index)").arg(cs); continue; }
        for (quint32 h : c->art) addArt(h);
        // A product with no art handles at all may still have a picture on its actor — headstones
        // always do. Exported alongside the rest so the folder matches what the tab showed you.
        if (c->art.isEmpty()) addArt(portraitFor(payloadNameOf(*c)));
        switch (c->kind) {
            case StoreProductIndex::Transmog: {
                // Armour by convention, weapons by their own name — see appearancesForProduct.
                // payloadNameOf, not payloadName: a CASC-recovered product has only the sno.
                const auto apps = appearancesForProduct(payloadNameOf(*c));
                r.appsPerChild.insert(cs, int(apps.size()));
                // Named consistently with the default branch below: the payload name where there
                // is one, else the product's own. An empty line in the manifest's "unresolved"
                // list is the one thing that list must never contain.
                if (apps.isEmpty()) {
                    const QString pn = payloadNameOf(*c);
                    r.unresolved << (pn.isEmpty() ? c->name : pn);
                    break;
                }
                r.firstApp.insert(cs, apps.first().first);
                r.appsOf.insert(cs, apps);
                for (const auto& a : apps)
                    if (!seenModel.contains(a.first)) { seenModel.insert(a.first); r.models.append(a); }
                break;
            }
            case StoreProductIndex::Marking: {
                // ── A marking is TEXTURES, not a mesh ───────────────────────────────────────────
                // It fell through to the default branch, resolved to no appearance (correctly —
                // there is no model), and was then silently dropped: a bundle's body markings
                // exported nothing at all. Its content is a MarkingShape, which carries exactly
                // three things worth exporting:
                //     snoMaskBody / snoMaskFace  the two mask textures — the marking ITSELF
                //     hIconImage                 the swatch the shop and the creator show
                // Read through the same markingDef() the Wardrobe paints with, so the export and
                // the viewport agree about what a marking is made of.
                const QString mstem = payloadNameOf(*c);
                const MarkingDef md = markingDef(Config::d4dataDir(), mstem);
                addArt(md.icon);
                int found = 0;
                for (const QString& tn : {md.bodyTex, md.faceTex}) {
                    if (tn.isEmpty()) continue;
                    const int ts = m_index ? m_index->snoForName(kTextureGroup(), tn) : 0;
                    if (ts > 0) { addTex(ts, tn); ++found; }
                }
                r.appsPerChild.insert(cs, found);
                // Reported when it produced nothing — a marking with no masks is a real miss, and
                // the whole reason this case exists is that silence hid one.
                if (found == 0 && md.icon == 0)
                    r.unresolved << (mstem.isEmpty() ? c->name : mstem);
                break;
            }
            case StoreProductIndex::None:
                r.unresolved << (c->name.isEmpty() ? QStringLiteral("product %1").arg(cs) : c->name);
                break;
            default: {
                // Mounts, mount trophies and companions name their appearance DIRECTLY: the
                // product's payload name is the group-9 entry, verified against d4data
                // (mnt_stor005_chimera and mnt_stor320_trophy are both appearances). So a plain
                // name lookup exports their geometry with no convention-expansion needed.
                //
                // Emotes legitimately miss here — an emote is an animation, not a mesh — as do
                // powers and dyes. Their art is already in artHandles, so they are not counted as
                // unresolved. (Markings USED to be excused here too, which is how a bundle's body
                // markings came to export nothing; they now have their own case above.)
                const auto apps = appearancesForProduct(payloadNameOf(*c));
                if (!apps.isEmpty()) {
                    r.appsPerChild.insert(cs, int(apps.size()));
                    r.firstApp.insert(cs, apps.first().first);
                    r.appsOf.insert(cs, apps);
                    for (const auto& a : apps)
                        if (!seenModel.contains(a.first)) {
                            seenModel.insert(a.first);
                            r.models.append(a);
                        }
                } else if (c->kind != StoreProductIndex::Emote
                        && c->kind != StoreProductIndex::Power
                        && c->kind != StoreProductIndex::DyeArmor) {
                    // Only the genuinely mesh-less kinds are silent. A MOUNT or COMPANION that did
                    // not resolve is a real miss, and blanket-suppressing the default case hid it
                    // from both the status line and the manifest's "unresolved" list — the one
                    // thing that manifest exists to make visible.
                    const QString pn = payloadNameOf(*c);
                    r.unresolved << (pn.isEmpty() ? c->name : pn);
                }
                break;
            }
        }
    }
    return r;
}

// Every bundle highlighted in the list, in list order. Empty unless "Multi select" is on, since
// SingleSelection can only ever return the current row — which exportBundle already falls back to.
QVector<int> CatalogueTab::selectedBundles() const
{
    QVector<int> out;
    if (!m_list) return out;
    for (QListWidgetItem* it : m_list->selectedItems())
        if (const int bs = it->data(Qt::UserRole).toInt()) out.append(bs);
    return out;
}

// Prompt (or reuse the last folder) once, then write every selected bundle into it.
//
// The folder is asked for ONCE for the whole batch rather than per bundle — being asked forty
// times is not a feature — and each bundle still gets its own named subfolder inside it, exactly
// as a single export does. So one bundle and forty produce the same layout.
// Every bundle the current search + filters match, in list order — the whole left-hand list, not
// just what is highlighted. This is what "Export all matching" acts on.
QVector<int> CatalogueTab::filteredBundles() const
{
    QVector<int> out;
    if (!m_list) return out;
    out.reserve(m_list->count());
    for (int i = 0; i < m_list->count(); ++i)
        if (QListWidgetItem* it = m_list->item(i))
            if (const int bs = it->data(Qt::UserRole).toInt()) out.append(bs);
    return out;
}

void CatalogueTab::exportBundle(bool promptDir)
{
    QVector<int> bundles = selectedBundles();
    if (bundles.isEmpty() && m_curBundle > 0) bundles << m_curBundle;
    exportBundleList(bundles, promptDir);
}

// Everything the filter matches. Confirmed first: this can be 1,800 bundles and tens of thousands
// of files, which is not something to start from a menu click with no warning.
void CatalogueTab::exportAllFiltered(bool promptDir)
{
    const QVector<int> all = filteredBundles();
    if (all.isEmpty()) return;
    if (all.size() > 1) {
        const auto btn = QMessageBox::question(
            this, QStringLiteral("Export all matching"),
            QStringLiteral("Export all %1 bundle(s) currently listed?\n\n"
                           "Each gets its own folder with its models, art, icons and a manifest. "
                           "Narrow the filters first if that is more than you meant.").arg(all.size()),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
        if (btn != QMessageBox::Yes) return;
    }
    exportBundleList(all, promptDir);
}

void CatalogueTab::exportBundleList(QVector<int> bundles, bool promptDir)
{
    if (m_exporting) return;   // processEvents() below re-enters otherwise
    // Drop anything the index no longer knows (it can be swapped out under us by a rebuild).
    StoreProductIndex& sidx = StoreProductIndex::instance();
    bundles.erase(std::remove_if(bundles.begin(), bundles.end(),
                                 [&sidx](int bs) { return sidx.product(bs) == nullptr; }),
                  bundles.end());
    if (bundles.isEmpty()) return;

    m_exporting = true;
    // Clears the re-entry flag on EVERY exit path, including the early return when the folder
    // dialog is cancelled. (It used to re-enable the two buttons as well; those are gone — export
    // is the Export menu's job now, and MainWindow re-queries hasExportSelection each time it
    // opens, so there is no button state left to restore.)
    struct Guard {
        CatalogueTab* t;
        ~Guard() { t->m_exporting = false; }
    } guard{this};

    QString dir = settingsLastDir();
    if (promptDir || dir.isEmpty()) {
        dir = QFileDialog::getExistingDirectory(
            this, bundles.size() > 1 ? QStringLiteral("Export %1 bundles to…").arg(bundles.size())
                                     : QStringLiteral("Export bundle to…"), dir);
        if (dir.isEmpty()) return;
        QSettings().setValue(QStringLiteral("catalogue/lastDir"), dir);
    }

    m_exportLog.clear();   // one batch, one log
    int nModels = 0, nTex = 0, nIcons = 0, nFrames = 0, done = 0, nSkipped = 0;
    for (int bs : bundles) {
        const auto* pb = StoreProductIndex::instance().product(bs);
        if (!pb) continue;   // re-checked: resolveBundle below spins the event loop
        const Written w = writeBundle(*pb, dir, ++done, int(bundles.size()));
        if (w.skipped) { ++nSkipped; continue; }
        nModels += w.models; nTex += w.textures; nIcons += w.icons; nFrames += w.frames;
    }

    // The pipelines' own per-run summaries went to the sink instead of a modal box; anything that
    // FAILED is named in them, so surface that count here rather than letting a partial batch look
    // identical to a clean one. Detail stays in the log and in _bulk_failed.txt.
    int failedRuns = 0;
    for (const QString& l : m_exportLog)
        if (l.contains(QLatin1String("failed")) && !l.contains(QLatin1String("0 failed")))
            ++failedRuns;
    m_status->setText(QStringLiteral("Exported %1%2 model(s), %3 texture(s), %4 icon(s)%5 → %6%7%8")
                          .arg(bundles.size() > 1
                                   ? QStringLiteral("%1 bundles: ").arg(bundles.size()) : QString())
                          .arg(nModels).arg(nTex).arg(nIcons)
                          .arg(nFrames ? QStringLiteral(", %1 texframe(s)").arg(nFrames) : QString())
                          .arg(ViewportPartMenu::condensePath(dir))
                          // Skips are stated, never silent: "0 models" after pointing this at 200
                          // bundles has to be distinguishable from a run that did nothing at all.
                          .arg(nSkipped ? QStringLiteral("  ·  %1 already exported, skipped")
                                              .arg(nSkipped) : QString())
                          .arg(failedRuns ? QStringLiteral("  ·  %1 run(s) had failures — see the log")
                                                .arg(failedRuns) : QString()));
    for (const QString& l : m_exportLog) qInfo().noquote() << "catalogue export:" << l;
}

// One bundle, into `parentDir`/<title>. `nth`/`total` only feed the progress line.
//
// NOTE — this is the WHOLE bundle, always. It used to quietly narrow itself to the contents rows
// when two or more were selected, which meant one command ("Export bundle…") had two outcomes
// depending on state you might not have noticed. Selections are handled a level up now: the Export
// menu routes them to exportRows and relabels itself accordingly, so a partial export is something
// you ask for by name rather than something a stale highlight causes.
CatalogueTab::Written CatalogueTab::writeBundle(const StoreProductIndex::Product& b,
                                                const QString& parentDir, int nth, int total)
{
    Written out;
    // COPIED, not referenced. product() hands back a pointer into the index's own QHash, and
    // install() move-assigns that hash from the build thread — resolveBundle and processEvents
    // below both spin the event loop, so a rebuild landing mid-export would leave every field
    // below pointing at freed memory. This is the same rule showRowMenu already follows; one copy
    // per bundle is nothing against the file writes.
    const StoreProductIndex::Product bc = b;
    // One folder per bundle, named for the shop title when there is one — a flat dump of forty
    // bundles into one folder is unusable, and the SNO name is not what anyone is looking for.
    QString stem = bc.title.isEmpty() ? bc.name : bc.title;
    stem.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    const QString outDir = QDir(parentDir).filePath(stem);

    // ── What this export is asked to write (Settings ▸ Export ▸ Catalogue export) ────────────────
    // All four default ON, so the behaviour with untouched settings is exactly what it was. They
    // matter once "Export all matching" is pointed at a few hundred bundles, where "art only" is
    // the difference between a few hundred MB and several GB.
    QSettings st;
    const bool wantModels = st.value(QStringLiteral("export/catModels"),   true).toBool();
    const bool wantArt    = st.value(QStringLiteral("export/catArt"),      true).toBool();
    const bool wantIcons  = st.value(QStringLiteral("export/catIcons"),    true).toBool();
    const bool onlyNew    = st.value(QStringLiteral("export/catOnlyNew"),  false).toBool();
    // The manifest is also the LEDGER "only new" reads, so it is forced on in that mode — skipping
    // on the strength of a marker you chose not to write would skip everything, forever.
    const bool wantMan    = st.value(QStringLiteral("export/catManifest"), true).toBool() || onlyNew;

    // ── Skip what a previous run already wrote ──────────────────────────────────────────────────
    // Compares against what that run RECORDED writing, not merely against the folder existing: an
    // art-only export followed by a full one must not be skipped, or you would silently keep the
    // partial result. Anything the earlier run did not write makes this one proceed.
    const QString manPath = QDir(outDir).filePath(QStringLiteral("manifest.json"));
    if (onlyNew && QFileInfo::exists(manPath)) {
        QFile mf(manPath);
        if (mf.open(QIODevice::ReadOnly)) {
            const QJsonObject prev = QJsonDocument::fromJson(mf.readAll()).object()
                                         .value(QStringLiteral("wrote")).toObject();
            const bool covers =
                (!wantModels || prev.value(QStringLiteral("models")).toBool())
             && (!wantArt    || prev.value(QStringLiteral("art")).toBool())
             && (!wantIcons  || prev.value(QStringLiteral("icons")).toBool());
            if (covers) {
                out.skipped = true;
                return out;
            }
        }
    }
    QDir().mkpath(outDir);

    const Resolved r = resolveBundle(bc);

    m_status->setText(total > 1
        ? QStringLiteral("Exporting %1 of %2 — %3…").arg(nth).arg(total).arg(stem)
        : QStringLiteral("Exporting…"));
    QGuiApplication::processEvents();

    // ── ONE operation, not one per bundle ───────────────────────────────────────────────────────
    // Both pipelines pop a modal "Bulk extract" box and their own progress dialog when they are
    // given no BatchSink — which is right for a lone export driven from a menu, and wrong here:
    // exporting eight bundles meant eight boxes to dismiss, each interrupting the run.
    //
    // A sink is the switch the pipelines already provide for exactly this (it is how Bulk Extract
    // runs quietly into its console). Lines are collected and folded into this tab's own status
    // line, which reports once at the end for the whole batch.
    BatchSink sink;
    sink.log = [this](const QString& line) { m_exportLog << line; };
    if (wantModels && !r.models.isEmpty() && m_models) {
        QDir().mkpath(QDir(outDir).filePath(QStringLiteral("models")));
        m_models->bulkExport(r.models, QDir(outDir).filePath(QStringLiteral("models")), false, &sink);
    }
    if (wantArt && !r.textures.isEmpty() && m_textures) {
        QDir().mkpath(QDir(outDir).filePath(QStringLiteral("art")));
        m_textures->bulkExportTextures(r.textures, QDir(outDir).filePath(QStringLiteral("art")),
                                       false, &sink);
    }

    // Frames belong to the art; with art off there is nothing for them to sit beside.
    const int frameFiles = wantArt
        ? exportTexFrames(r.textures, QDir(outDir).filePath(QStringLiteral("art/frames"))) : 0;

    // Shop icons: atlas FRAMES, so they cannot go through the texture pipeline (which writes whole
    // textures). Decoded and written here.
    int icons = 0;
    if (wantIcons && !r.artHandles.isEmpty()) {
        const QString iconDir = QDir(outDir).filePath(QStringLiteral("icons"));
        QDir().mkpath(iconDir);
        QSet<quint32> done;
        for (quint32 h : r.artHandles) {
            if (done.contains(h)) continue;
            done.insert(h);
            const QImage img = IconIndex::instance().iconImage(h, m_reader);
            if (img.isNull()) continue;
            if (img.save(QDir(iconDir).filePath(QStringLiteral("%1.png").arg(h)), "PNG")) ++icons;
        }
    }

    // Manifest — what was in the bundle, what it resolved to, and what it did not. The "did not"
    // half is the point: a silent partial export is indistinguishable from a complete one.
    QJsonObject man;
    man.insert(QStringLiteral("bundle"), bc.name);
    man.insert(QStringLiteral("title"), bc.title);
    man.insert(QStringLiteral("description"), bc.description);
    man.insert(QStringLiteral("sno"), bc.sno);
    man.insert(QStringLiteral("releaseBranch"), bc.branch);
    // WHICH PARTS this run wrote. Without it "only new" could not tell a full export from an
    // art-only one and would skip a bundle that was deliberately half-written, leaving the missing
    // half missing forever. Read back by the skip check at the top of this function.
    {
        QJsonObject wrote;
        wrote.insert(QStringLiteral("models"), wantModels);
        wrote.insert(QStringLiteral("art"),    wantArt);
        wrote.insert(QStringLiteral("icons"),  wantIcons);
        wrote.insert(QStringLiteral("frames"), frameFiles > 0);
        man.insert(QStringLiteral("wrote"), wrote);
    }
    QJsonArray kids;
    StoreProductIndex& idx = StoreProductIndex::instance();
    for (int cs : bc.children) {
        const auto* c = idx.product(cs);
        QJsonObject o;
        o.insert(QStringLiteral("sno"), cs);
        if (c) {
            o.insert(QStringLiteral("name"), c->name);
            o.insert(QStringLiteral("title"), c->title);
            o.insert(QStringLiteral("kind"), StoreProductIndex::kindLabel(c->kind));
            o.insert(QStringLiteral("payload"), payloadNameOf(*c));
        }
        kids.append(o);
    }
    man.insert(QStringLiteral("contents"), kids);
    QJsonArray mj;
    for (const auto& m : r.models) mj.append(QStringLiteral("%1 [%2]").arg(m.second).arg(m.first));
    man.insert(QStringLiteral("modelsExported"), mj);
    QJsonArray uj;
    for (const QString& u : r.unresolved) uj.append(u);
    man.insert(QStringLiteral("unresolved"), uj);
    if (wantMan) {
        QFile mf(manPath);
        if (mf.open(QIODevice::WriteOnly)) mf.write(QJsonDocument(man).toJson());
    }

    // Counts reflect what was WRITTEN, not what resolved — with models off, "41 model(s)" would be
    // a lie the status line then repeats for the whole batch.
    out.models   = wantModels ? int(r.models.size())   : 0;
    out.textures = wantArt    ? int(r.textures.size()) : 0;
    out.icons    = icons;
    out.frames   = frameFiles;
    return out;   // the status line is written once, by the caller, for the whole batch
}

// ── Texframes ───────────────────────────────────────────────────────────────────────────────────
// Every frame of each shop-art atlas, written into `dir`.
//
// Gated on ONE setting — Settings ▸ Export ▸ Catalogue export — rather than on menu actions of its
// own. Frames are not a different export, they are extra files belonging to the images you are
// already exporting, so they live where the rest of the "how should exports come out" answers live
// and apply to every catalogue export automatically. Four extra menu entries for a preference is
// how a menu becomes unreadable.
int CatalogueTab::exportTexFrames(const QVector<QPair<int, QString>>& textures, const QString& dir)
{
    if (textures.isEmpty()) return 0;
    if (!QSettings().value(QStringLiteral("export/catalogueFrames"), false).toBool()) return 0;
    QDir().mkpath(dir);
    int frameFiles = 0, noFrames = 0;
    for (const auto& t : textures) {
        // ONE route, the Textures tab's own: ptFrame when d4data has it, else 2D_table for the
        // count + alpha-gutter segmentation of the decoded atlas for the rectangles.
        //
        // This used to be two routes, the second of which asked IconIndex for each handle from
        // 2D_table. That returns nothing for an atlas IconIndex has not indexed — which is exactly
        // the seasonal/collab art this feature exists for — so 2DInventory_Bundle_HArmor_bar_stor251
        // and friends exported zero frames while the Textures tab displayed them perfectly. The
        // rectangles never came from IconIndex; 2D_table does not carry UVs at all.
        const QImage full = decodeTexture(t.first, t.second);
        const QVector<TexFrame> frames =
            TexturesTab::atlasFramesFor(m_reader, Config::d4dataDir(), t.second, t.first, full);
        int wroteThis = 0;
        if (!full.isNull()) {
            for (int fi = 0; fi < frames.size(); ++fi) {
                const TexFrame& f = frames.at(fi);
                const QRect rc(qRound(f.u0 * full.width()), qRound(f.v0 * full.height()),
                               qRound((f.u1 - f.u0) * full.width()),
                               qRound((f.v1 - f.v0) * full.height()));
                const QRect cl = rc.intersected(full.rect());
                if (cl.width() < 2 || cl.height() < 2) continue;
                if (full.copy(cl).save(QDir(dir).filePath(
                        QStringLiteral("%1_frame%2.png")
                            .arg(t.second).arg(fi, 3, 10, QLatin1Char('0')))))
                    ++wroteThis;
            }
        }
        if (wroteThis == 0) {
            ++noFrames;
            qInfo("catalogue: texframes — %s [%d] produced none (decoded %s, %d frame rect(s))",
                  qUtf8Printable(t.second), t.first, full.isNull() ? "NO" : "yes",
                  int(frames.size()));
        }
        frameFiles += wroteThis;
    }
    // Say so either way. A silent zero is what made this look broken rather than inapplicable.
    qInfo("catalogue: texframes — wrote %d image(s); %d of %d texture(s) had no frames",
          frameFiles, noFrames, int(textures.size()));
    return frameFiles;
}

// ── What the Export menu acts on ────────────────────────────────────────────────────────────────
// The rule, in one place: highlighted rows win, and the menu says which. Nothing highlighted means
// the bundle. This is why the tab needs no export UI of its own — the menu already describes the
// subject accurately before you commit to it.
bool CatalogueTab::hasExportSelection() const
{
    return !selectedBundles().isEmpty()
        || (m_curBundle > 0 && StoreProductIndex::instance().product(m_curBundle) != nullptr);
}

// ── Export all matching ─────────────────────────────────────────────────────────────────────────
// The count goes in the LABEL rather than in a tooltip or a confirmation alone, so the menu says
// what it will do before you commit — the same rule the selection-aware noun follows. Hidden
// entirely when the list is empty or holds a single bundle, where "export all" and "export this
// bundle" are the same command and two entries for it is just noise.
bool CatalogueTab::hasExportAllFiltered() const
{
    return filteredBundles().size() > 1;
}

QString CatalogueTab::exportAllFilteredLabel() const
{
    return QStringLiteral("Export all %1 matching bundles").arg(filteredBundles().size());
}

void CatalogueTab::exportAllFiltered()       { exportAllFiltered(true); }
void CatalogueTab::exportAllFilteredToLast() { exportAllFiltered(false); }

void CatalogueTab::exportSelected(bool promptDir)
{
    QVector<int> apps, texs;
    currentRowSelection(apps, texs);
    if (!apps.isEmpty() || !texs.isEmpty()) { exportRows(apps, texs, promptDir); return; }
    exportBundle(promptDir);
}

void CatalogueTab::exportSelection()       { exportSelected(true); }
void CatalogueTab::exportSelectionToLast() { exportSelected(false); }

// Counted by KIND, because that is what changes what you get: models are .glb through the Models
// pipeline, images are .png through the Textures one. "Export 2 models…" and "Export 1 image…" are
// each a promise about the files that will appear.
QString CatalogueTab::exportNoun() const
{
    QVector<int> apps, texs;
    currentRowSelection(apps, texs);
    const int na = apps.size(), nt = texs.size();
    if (na && nt)
        return QStringLiteral("%1 model%2 + %3 image%4")
            .arg(na).arg(na == 1 ? QString() : QStringLiteral("s"))
            .arg(nt).arg(nt == 1 ? QString() : QStringLiteral("s"));
    if (na)
        return QStringLiteral("%1 model%2").arg(na).arg(na == 1 ? QString() : QStringLiteral("s"));
    if (nt)
        return QStringLiteral("%1 image%2").arg(nt).arg(nt == 1 ? QString() : QStringLiteral("s"));
    // Several bundles ticked (Multi select) — name the count, not one of their titles.
    if (const int nb = int(selectedBundles().size()); nb > 1)
        return QStringLiteral("%1 bundles").arg(nb);
    const auto* b = StoreProductIndex::instance().product(m_curBundle);
    if (!b) return QStringLiteral("bundle");
    return QStringLiteral("bundle \"%1\"").arg(b->title.isEmpty() ? b->name : b->title);
}
