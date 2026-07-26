#include "tabs/TexturesTab.h"

#include <QElapsedTimer>
#include <memory>
#include "app/AppPaths.h"
#include "app/SehGuard.h"
#include "tabs/BatchSink.h"

#include "app/Config.h"
#include "app/ExportNotifier.h"
#include "casc/CascReader.h"
#include "gl/GLTextureWidget.h"

#include <functional>

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDrag>
#include <QMimeData>
#include <QStandardPaths>
#include <QUrl>
#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QScreen>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>
#include "index/AppearanceMeta.h"
#include "index/AssetLinks.h"
#include "index/SnoIndex.h"
#include "index/SnoListModel.h"
#include "tex/BcDecode.h"
#include "tex/FrameTable.h"
#include "tex/TexFormat.h"
#include "tex/TexMeta.h"
#include "util/CsvCopy.h"
#include "util/HoverInfo.h"
#include "util/NameTemplate.h"
#include "util/PanelPersist.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDataStream>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QPainter>
#include <QPolygonF>
#include <QSize>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPixmap>
#include <QPixmapCache>
#include <QProgressDialog>
#include <QRunnable>
#include <QStyledItemDelegate>
#include <QThreadPool>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRect>
#include <QScrollBar>
#include <QTime>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTableView>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QSignalBlocker>

#include <QImageWriter>
#include <QMutex>
#include <QThread>
#include <atomic>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {
// Name-pinned group id (fallback 44): survives a hypothetical SNO-group renumbering, since
// only SnoIndex's name map would need correcting — this callsite follows automatically.
inline int kGroupTextureId() { static const int g = SnoIndex::groupIdByName(QStringLiteral("Texture"), 44); return g; }

// Grid-view cell painter (mirrors the Models tab): a square, aspect-preserved thumbnail with a
// single elided caption line beneath it, so long D4 names don't squeeze the icon to a strip.
class GridItemDelegate : public QStyledItemDelegate {
public:
    explicit GridItemDelegate(int iconPx, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), m_icon(iconPx) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(m_icon + 26, m_icon + 34);
    }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        p->save();
        const QRect r = opt.rect;
        if (opt.state & QStyle::State_Selected)        p->fillRect(r, QColor(0x8a, 0x14, 0x14));
        else if (opt.state & QStyle::State_MouseOver)  p->fillRect(r, QColor(0x3a, 0x20, 0x20));
        QPixmap pm;
        const QVariant dec = idx.data(Qt::DecorationRole);
        if (dec.canConvert<QIcon>()) pm = qvariant_cast<QIcon>(dec).pixmap(m_icon, m_icon);
        if (!pm.isNull()) {
            const QPixmap sp = pm.scaled(m_icon, m_icon, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            p->drawPixmap(r.left() + (r.width() - sp.width()) / 2,
                          r.top() + 4 + (m_icon - sp.height()) / 2, sp);
        } else {
            // Not decoded yet — a faint placeholder square so the grid reads as a grid while lazy.
            p->fillRect(QRect(r.left() + (r.width() - m_icon) / 2, r.top() + 4, m_icon, m_icon),
                        QColor(0x22, 0x22, 0x22));
        }
        const QString name = idx.data(Qt::DisplayRole).toString();
        const QRect tr(r.left() + 2, r.top() + 4 + m_icon + 2, r.width() - 4, r.height() - m_icon - 8);
        p->setPen((opt.state & QStyle::State_Selected) ? QColor(0xff, 0xff, 0xff)
                                                       : QColor(0xcc, 0xcc, 0xcc));
        p->drawText(tr, Qt::AlignHCenter | Qt::AlignTop,
                    opt.fontMetrics.elidedText(name, Qt::ElideMiddle, tr.width()));
        p->restore();
    }
private:
    int m_icon;
};

// Segment a decoded atlas image into its sprite cells by alpha gutters.
//   1. rows: contiguous bands of scanlines that contain any opaque pixel,
//      separated by fully-transparent gutter rows;
//   2. within each row band, columns: contiguous bands of opaque columns;
//   3. tighten each cell vertically to its own opaque bounds.
// Returns tight (trimmed) rectangles in row-major (top→bottom, left→right)
// order. This recovers the per-frame rectangles the game's ptFrame table would
// carry, for atlases whose descriptor is not available (seasonal / stale
// d4data). `alphaThresh` ignores near-transparent halo pixels.
QVector<QRect> segmentAtlasFrames(const QImage& srcAny, int alphaThresh = 8)
{
    QVector<QRect> cells;
    if (srcAny.isNull())
        return cells;
    const QImage img = srcAny.convertToFormat(QImage::Format_RGBA8888);
    const int W = img.width(), H = img.height();
    if (W <= 0 || H <= 0)
        return cells;

    auto rowHasOpaque = [&](int y) {
        const uchar* s = img.constScanLine(y);
        for (int x = 0; x < W; ++x)
            if (s[x * 4 + 3] > alphaThresh) return true;
        return false;
    };

    // Row bands.
    QVector<QPair<int, int>> rowBands;
    for (int y = 0; y < H; ) {
        if (!rowHasOpaque(y)) { ++y; continue; }
        int y0 = y;
        while (y < H && rowHasOpaque(y)) ++y;
        rowBands.append({ y0, y - 1 });
    }

    for (const auto& rb : rowBands) {
        const int y0 = rb.first, y1 = rb.second;
        // Column occupancy within this row band.
        QVector<bool> colOpaque(W, false);
        for (int y = y0; y <= y1; ++y) {
            const uchar* s = img.constScanLine(y);
            for (int x = 0; x < W; ++x)
                if (s[x * 4 + 3] > alphaThresh) colOpaque[x] = true;
        }
        for (int x = 0; x < W; ) {
            if (!colOpaque[x]) { ++x; continue; }
            int x0 = x;
            while (x < W && colOpaque[x]) ++x;
            const int x1 = x - 1;
            // Tighten vertical bounds for this x-slice.
            int ty0 = y1, ty1 = y0;
            for (int yy = y0; yy <= y1; ++yy) {
                const uchar* s = img.constScanLine(yy);
                bool any = false;
                for (int xx = x0; xx <= x1; ++xx)
                    if (s[xx * 4 + 3] > alphaThresh) { any = true; break; }
                if (any) { ty0 = qMin(ty0, yy); ty1 = qMax(ty1, yy); }
            }
            if (ty1 >= ty0)
                cells.append(QRect(x0, ty0, x1 - x0 + 1, ty1 - ty0 + 1));
        }
    }
    return cells;
}

QString humanSize(qint64 n)
{
    const char* u[] = {"B", "KiB", "MiB", "GiB"};
    double f = double(n);
    int i = 0;
    while (f >= 1024.0 && i < 3) { f /= 1024.0; ++i; }
    return i == 0 ? QStringLiteral("%1 B").arg(n)
                  : QStringLiteral("%1 %2 (%3 bytes)").arg(f, 0, 'f', 2).arg(u[i]).arg(n);
}

// Grayscale view of one channel (0=R,1=G,2=B,3=A) of an RGBA image.
QImage channelGrey(const QImage& srcAny, int ch)
{
    if (srcAny.isNull()) return {};
    const QImage src = srcAny.convertToFormat(QImage::Format_RGBA8888);
    QImage out(src.width(), src.height(), QImage::Format_RGBA8888);
    for (int y = 0; y < src.height(); ++y) {
        const uchar* s = src.constScanLine(y);
        uchar* d = out.scanLine(y);
        for (int x = 0; x < src.width(); ++x) {
            const uchar v = s[x * 4 + ch];
            d[x * 4 + 0] = d[x * 4 + 1] = d[x * 4 + 2] = v;
            d[x * 4 + 3] = 255;
        }
    }
    return out;
}

// ── Texture categories (derived from real d4data name conventions) ──────────────────────────────
// Textures aren't tagged like appearances, but their NAMES encode what they are (2DUI_*, 2DInventory_*,
// map/zmap/mmap, npc*, dye*, class-prefixes for gear, …). These give the whole texture set — not just
// gear — a meaningful category filter. The order here is the order shown in the funnel.
static const char* const kTexCats[] = {
    "Latest",   // special: assets new in the most recent update (matched by SNO, not name)
    "2D", "2D UI", "Inventory", "Items", "Store", "Bundles", "Loading screens", "Emblems / Icons",
    "Map", "NPC", "Gear / Armor", "Weapons", "Environment / World", "FX / VFX", "Cinematic",
    "Dye", "Body markings", "Trophy",
};

// ── User-editable category rules (future-proofing) ──────────────────────────────────────────────
// data/category_rules.json can EXTEND the built-in name heuristics without a recompile — when a
// patch shifts naming conventions, drop in corrections. Shape:
//   { "categories": [ { "name": "Store", "prefixes": ["shop"], "contains": ["marketplace"] } ] }
// A known name widens that category's match; an unknown name adds a NEW category to the funnel.
struct TexCatRule { QString name; QStringList prefixes; QStringList contains; };
static const QVector<TexCatRule>& customTexCatRules()
{
    static const QVector<TexCatRule> rules = [] {
        QVector<TexCatRule> out;
        QFile f(AppPaths::dataDir() + QStringLiteral("/category_rules.json"));
        if (!f.open(QIODevice::ReadOnly)) return out;
        const QJsonArray cats = QJsonDocument::fromJson(f.readAll())
                                    .object().value(QStringLiteral("categories")).toArray();
        for (const QJsonValue& v : cats) {
            const QJsonObject o = v.toObject();
            TexCatRule r;
            r.name = o.value(QStringLiteral("name")).toString();
            for (const QJsonValue& p : o.value(QStringLiteral("prefixes")).toArray())
                r.prefixes << p.toString().toLower();
            for (const QJsonValue& c : o.value(QStringLiteral("contains")).toArray())
                r.contains << c.toString().toLower();
            if (!r.name.isEmpty()) out << r;
        }
        if (!out.isEmpty())
            qInfo("textures: %d custom category rule(s) loaded from category_rules.json", int(out.size()));
        return out;
    }();
    return rules;
}
static bool customTexCatMatch(const QString& nl, const QString& cat)
{
    for (const TexCatRule& r : customTexCatRules()) {
        if (r.name.compare(cat, Qt::CaseInsensitive) != 0) continue;
        for (const QString& p : r.prefixes) if (nl.startsWith(p)) return true;
        for (const QString& c : r.contains) if (nl.contains(c)) return true;
    }
    return false;
}
// The full ordered category list: built-ins + any NEW names from the JSON + "Uncategorized" last
// (graceful-unknown: assets matching nothing stay findable instead of invisible).
static QStringList texCatList()
{
    QStringList cats;
    for (const char* c : kTexCats) cats << QString::fromLatin1(c);
    for (const TexCatRule& r : customTexCatRules())
        if (!cats.contains(r.name)) cats << r.name;
    cats << QStringLiteral("Uncategorized");
    return cats;
}
// True when texture name `nl` (already lower-cased) belongs to category `cat`. Patterns are derived
// from real d4data name conventions (see the counts logged while building this). Custom JSON rules
// widen any category; "Uncategorized" = matches nothing else (so new naming schemes stay browsable).
static bool texInCategory(const QString& nl, const QString& cat)
{
    if (customTexCatMatch(nl, cat))              return true;
    if (cat == QLatin1String("Uncategorized")) {
        for (const char* c : kTexCats) {
            const QLatin1String other(c);
            if (other == QLatin1String("Latest")) continue;   // SNO-based, not a name category
            if (texInCategory(nl, other)) return false;
        }
        for (const TexCatRule& r : customTexCatRules())
            if (customTexCatMatch(nl, r.name)) return false;
        return true;
    }
    if (cat == QLatin1String("2D"))              return nl.startsWith(QLatin1String("2d"));
    if (cat == QLatin1String("2D UI"))           return nl.startsWith(QLatin1String("2dui"));
    if (cat == QLatin1String("Inventory"))       return nl.startsWith(QLatin1String("2dinventory"));
    if (cat == QLatin1String("Items"))           return nl.contains(QLatin1String("2dinventory_items"))
                                                      || nl.contains(QLatin1String("_catalog"));
    if (cat == QLatin1String("Store"))           return nl.contains(QLatin1String("store"));
    if (cat == QLatin1String("Bundles"))         return nl.contains(QLatin1String("bundle"));
    if (cat == QLatin1String("Loading screens")) return nl.contains(QLatin1String("loadingscreen"))
                                                      || nl.contains(QLatin1String("loadscreen"));
    if (cat == QLatin1String("Emblems / Icons")) return nl.contains(QLatin1String("emblem"))
                                                      || nl.contains(QLatin1String("icon"));
    if (cat == QLatin1String("Map"))             return nl.startsWith(QLatin1String("map"))
                                                      || nl.startsWith(QLatin1String("zmap"))
                                                      || nl.startsWith(QLatin1String("mmap"))
                                                      || nl.contains(QLatin1String("minimap"));
    if (cat == QLatin1String("NPC"))             return nl.startsWith(QLatin1String("npc"));
    if (cat == QLatin1String("Dye"))             return nl.contains(QLatin1String("dye"));
    if (cat == QLatin1String("FX / VFX"))        return nl.startsWith(QLatin1String("fx"))
                                                      || nl.startsWith(QLatin1String("vfx"))
                                                      || nl.contains(QLatin1String("fxkit"));
    if (cat == QLatin1String("Cinematic"))       return nl.startsWith(QLatin1String("igc"))
                                                      || nl.contains(QLatin1String("cine"));
    if (cat == QLatin1String("Body markings"))   return nl.contains(QLatin1String("bodymarking"))
                                                      || nl.contains(QLatin1String("tattoo"));
    if (cat == QLatin1String("Trophy"))          return nl.contains(QLatin1String("trophy"));
    if (cat == QLatin1String("Gear / Armor")) {
        // Class alternation built from the central class table — a new class matches automatically.
        static const QRegularExpression rx(
            QStringLiteral("^%1[mf]").arg(AppearanceMeta::classPrefixPattern()),
            QRegularExpression::CaseInsensitiveOption);
        return rx.match(nl).hasMatch() || nl.contains(QLatin1String("_base"));
    }
    if (cat == QLatin1String("Weapons")) {
        static const QRegularExpression rx(
            QStringLiteral("^(twohand|offhand)?(sword|dagger|mace|axe|shield|wand|staff|polearm|"
                           "scythe|focus|crossbow|bow|totem)"),
            QRegularExpression::CaseInsensitiveOption);
        return rx.match(nl).hasMatch();
    }
    if (cat == QLatin1String("Environment / World")) {
        static const char* const kZones[] = {
            "sanctuary", "mnt", "cave", "protodun", "fow", "kehj", "skov", "scos", "frac", "naha",
            "step", "askari", "tega", "kurast", "hawe", "hell", "tora", "zak", "vat", "firstborn",
            "ancients", "lacuni", "amazon", "raid", "generic", "global", "drysteppe", "fracturedpeak"};
        for (const char* z : kZones) if (nl.startsWith(QLatin1String(z))) return true;
        return nl.contains(QLatin1String("terrain")) || nl.contains(QLatin1String("foliage"))
            || nl.contains(QLatin1String("_water"))  || nl.contains(QLatin1String("_sky"))
            || nl.contains(QLatin1String("tileset"));
    }
    return false;
}

// Bytes per 4×4 BC block for the formats we decode (TexFormat codes).
QString fmtFamily(int eTexFormat)
{
    switch (eTexFormat) {
        case 9: case 41:           return QStringLiteral("BC4");
        case 10: case 46: case 47: return QStringLiteral("BC1");
        case 42:                   return QStringLiteral("BC5");
        case 49:                   return QStringLiteral("BC3");
        case 50:                   return QStringLiteral("BC7");
        default:                   return QString();
    }
}

int bytesPerBlock(int fmt)
{
    switch (fmt) {
        case 9: case 41:                 return 8;   // BC4
        case 10: case 46: case 47:       return 8;   // BC1
        case 42:                         return 16;  // BC5
        case 49:                         return 16;  // BC3
        case 50:                         return 16;  // BC7
        default:                         return 16;
    }
}

QTreeView* makeTree(QStandardItemModel* model)
{
    auto* t = new QTreeView;
    t->setModel(model);
    t->setRootIsDecorated(false);
    t->setAlternatingRowColors(true);
    t->setUniformRowHeights(true);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    return t;
}
}  // namespace

// Public wrappers so the Bulk Extract funnel can reuse the exact same texture categories/matcher.
// Defined at file scope (not in the anonymous namespace above) since they're class members; they
// still see the internal-linkage kTexCats / texInCategory declared earlier in this translation unit.
QStringList TexturesTab::bulkTexCategories()
{
    QStringList out = texCatList();
    out.removeAll(QStringLiteral("Latest"));   // Latest = SNO-based, caller handles
    return out;
}
bool TexturesTab::bulkTexInCategory(const QString& lowerName, const QString& cat)
{
    return texInCategory(lowerName, cat);
}

TexturesTab::TexturesTab(QWidget* parent) : BrowserTab(parent)
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto* split = new QSplitter(Qt::Horizontal, this);
    root->addWidget(split);
    split->addWidget(buildLeft());
    split->addWidget(buildMiddle());
    split->addWidget(buildRight());
    split->setSizes({360, 760, 460});
    PanelPersist::bind(split, QStringLiteral("tex/mainSplit"));   // remember column widths

    // Hover preview: dwell 0.5s over a channel tile → floating popup; scroll resizes it.
    m_iconPreview = new QLabel(this, Qt::ToolTip);
    m_iconPreview->setAlignment(Qt::AlignCenter);
    m_iconPreview->hide();
    m_hoverTimer = new QTimer(this);
    m_hoverTimer->setSingleShot(true);
    m_hoverTimer->setInterval(HoverInfo::delayMs());
    m_previewPx = HoverInfo::previewPx();   // initial hover-popup size (Settings ▸ General ▸ On-hover)
    connect(m_hoverTimer, &QTimer::timeout, this, [this]() {
        if (m_hoverTile >= 0) showTilePreview(m_hoverTile);
        else if (m_hoverGridSno > 0) showGridPreview(m_hoverGridSno);
    });
}

QWidget* TexturesTab::buildLeft()
{
    auto* w = new QWidget;
    auto* lay = new QVBoxLayout(w);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(4);

    // ── Filter bar: the funnel button on the FAR LEFT, then ONE search box + count — exactly the
    // Models tab's layout. The search box does name/#tag; a pure-digit query filters by SNO. ──
    m_onlyDecrypted = new QCheckBox(QStringLiteral("Only decrypted"));
    m_onlyDecrypted->setToolTip(QStringLiteral("Hide textures with no CASC payload."));
    connect(m_onlyDecrypted, &QCheckBox::toggled, this, [this] { loadList(); applyNameFilter(); });

    m_search = new QLineEdit;
    m_search->setPlaceholderText(QStringLiteral("Search…   #tag · digits = SNO"));
    m_search->setClearButtonEnabled(true);
    m_search->setToolTip(QStringLiteral(
        "Filter by name, #tag, title or collection. All terms must match; prefix a term with \"-\" to EXCLUDE.\n"
        "A term that is all digits matches the SNO.   e.g.  armor -helm -#cape   ·   1341926"));

    m_orphanCheck = new QCheckBox(QStringLiteral("Orphans"));
    m_orphanCheck->setToolTip(QStringLiteral("Only textures used by no material/model."));
    m_sortCombo = new QComboBox;
    m_sortCombo->addItem(QStringLiteral("Sort: Name"),       QStringLiteral("name"));
    m_sortCombo->addItem(QStringLiteral("Sort: SNO"),        QStringLiteral("sno"));
    m_sortCombo->addItem(QStringLiteral("Sort: Size"),       QStringLiteral("size"));
    m_sortCombo->addItem(QStringLiteral("Sort: Dimensions"), QStringLiteral("dim"));
    m_multiSelect = new QCheckBox(QStringLiteral("Multi select"));
    m_selLabel = new QLabel(QStringLiteral("Selected: 0/0"));

    // ── Funnel popup — cleaned & organized top-to-bottom: data toggles · CATEGORY (the primary,
    // texture-native filter) · FORMAT · VIEW · GEAR TAGS (appearance class/type/gender, only for
    // armor/weapon textures — with their own tag-search + AND/OR). Section headers separate each. ──
    m_filterPanel = new QFrame(w, Qt::Popup);
    m_filterPanel->setObjectName(QStringLiteral("texFilterPanel"));
    m_filterPanel->setStyleSheet(QStringLiteral(
        "QFrame#texFilterPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
        "QLabel{color:#cccccc;} QCheckBox{color:#cccccc;}"));
    auto* fp = new QVBoxLayout(m_filterPanel);
    fp->setContentsMargins(10, 8, 10, 8);
    fp->setSpacing(6);
    auto secHdr = [&](const QString& t) {
        auto* h = new QLabel(t, m_filterPanel);
        h->setStyleSheet(QString::fromLatin1(kHdrQss));
        fp->addWidget(h);
    };

    // Data toggles — one compact row.
    m_rememberFilters = new QCheckBox(QStringLiteral("Remember filters"));
    m_rememberFilters->setToolTip(QStringLiteral(
        "Restore the search text, categories, formats, gear tags and toggles on next launch."));
    m_rememberFilters->setChecked(QSettings().value(QStringLiteral("tex/rememberFilters"), false).toBool());
    connect(m_rememberFilters, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("tex/rememberFilters"), on);
        if (on) saveTexFilterState();
    });
    {
        auto* r = new QHBoxLayout();
        r->addWidget(m_onlyDecrypted);   // both reparented into the popup
        r->addWidget(m_orphanCheck);
        r->addWidget(m_rememberFilters);
        r->addStretch(1);
        fp->addLayout(r);
        connect(m_orphanCheck, &QCheckBox::toggled, this, [this](bool) { applyNameFilter(); });
    }

    // ── CATEGORY (real d4data name conventions) — the primary filter; covers the whole texture set
    // (UI / inventory / map / gear / weapons / world / fx…), not just gear. Two-column grid. ──
    secHdr(QStringLiteral("Category"));
    {
        auto* grid = new QGridLayout();
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(14);
        grid->setVerticalSpacing(2);
        int i = 0;
        for (const QString& cat : texCatList()) {
            auto* cb = new QCheckBox(cat, m_filterPanel);
            m_catChecks.insert(cat, cb);
            connect(cb, &QCheckBox::toggled, this, [this, cat](bool on) {
                if (on) m_catFilter.insert(cat); else m_catFilter.remove(cat);
                applyNameFilter();
            });
            grid->addWidget(cb, i / 2, i % 2);
            ++i;
        }
        fp->addLayout(grid);
    }

    secHdr(QStringLiteral("Format"));
    {
        auto* fr = new QHBoxLayout();
        for (const char* f : {"BC1", "BC3", "BC4", "BC5", "BC7"}) {
            auto* cb = new QCheckBox(QString::fromLatin1(f), m_filterPanel);
            const QString fam = QString::fromLatin1(f);
            m_fmtChecks.insert(fam, cb);
            connect(cb, &QCheckBox::toggled, this, [this, fam](bool on) {
                if (on) m_fmtFilter.insert(fam); else m_fmtFilter.remove(fam);
                ensureFmtIndex();
                applyNameFilter();
            });
            fr->addWidget(cb);
        }
        fr->addStretch(1);
        fp->addLayout(fr);
    }

    secHdr(QStringLiteral("View"));
    {
        auto* r = new QHBoxLayout();
        r->addWidget(new QLabel(QStringLiteral("Sort"), m_filterPanel));
        r->addWidget(m_sortCombo, 1);
        r->addWidget(m_multiSelect);
        fp->addLayout(r);
    }

    // ── GEAR TAGS — appearance class/type/gender reachable from a texture via its material links.
    // Only meaningful for armor/weapon textures; its own tag-search box + AND/OR live here. ──
    secHdr(QStringLiteral("Gear tags"));
    auto* topRow = new QHBoxLayout();
    m_tagSearch = new QLineEdit(m_filterPanel);
    m_tagSearch->setPlaceholderText(QStringLiteral("Search gear tags…"));
    m_tagSearch->setClearButtonEnabled(true);
    topRow->addWidget(m_tagSearch, 1);
    m_tagOrChk = new QCheckBox(QStringLiteral("Any (OR)"), m_filterPanel);
    m_tagOrChk->setToolTip(QStringLiteral("Off: a texture must carry ALL ticked gear tags (narrowing).\n"
                                          "On: AT LEAST ONE (widening)."));
    connect(m_tagOrChk, &QCheckBox::toggled, this, [this](bool on) { m_tagOrMode = on; applyNameFilter(); });
    topRow->addWidget(m_tagOrChk);
    fp->addLayout(topRow);

    auto* scroll = new QScrollArea(m_filterPanel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setFixedHeight(200);
    scroll->setMinimumWidth(240);
    m_tagPanelBody = new QWidget(scroll);
    auto* bodyLay = new QVBoxLayout(m_tagPanelBody);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    bodyLay->setSpacing(3);
    bodyLay->addWidget(new QLabel(QStringLiteral("Gear tags load with the index…"), m_tagPanelBody));
    bodyLay->addStretch(1);
    scroll->setWidget(m_tagPanelBody);
    fp->addWidget(scroll, 1);

    auto* clearBtn = new QPushButton(QStringLiteral("Clear all filters"), m_filterPanel);
    fp->addWidget(clearBtn);
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        m_tagFilter.clear();
        m_catFilter.clear();
        m_fmtFilter.clear();
        for (QCheckBox* c : std::as_const(m_tagChecks)) { QSignalBlocker b(c); c->setChecked(false); }
        for (QCheckBox* c : std::as_const(m_catChecks)) { QSignalBlocker b(c); c->setChecked(false); }
        for (QCheckBox* c : std::as_const(m_fmtChecks)) { QSignalBlocker b(c); c->setChecked(false); }
        if (m_orphanCheck)    { QSignalBlocker b(m_orphanCheck);    m_orphanCheck->setChecked(false); }
        if (m_onlyDecrypted)  { QSignalBlocker b(m_onlyDecrypted);  m_onlyDecrypted->setChecked(false); }
        if (m_tagSearch)      m_tagSearch->clear();
        loadList();          // re-include encrypted rows dropped by "Only decrypted"
        applyNameFilter();
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

    // Funnel icon button (identical to the Models / Bulk tabs).
    m_filtersToggle = new QToolButton;
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
    m_filtersToggle->setToolTip(QStringLiteral("Filter by tags, format, orphans…"));
    connect(m_filtersToggle, &QToolButton::clicked, this, [this] {
        if (!m_filterPanel) return;
        refillTagPanel();
        m_filterPanel->adjustSize();
        m_filterPanel->move(m_filtersToggle->mapToGlobal(QPoint(0, m_filtersToggle->height() + 2)));
        m_filterPanel->show();
        m_filterPanel->raise();
    });

    // ── Active-filter chips. ──
    m_filterChips = new QWidget(w);
    auto* chipLay = new QHBoxLayout(m_filterChips);
    chipLay->setContentsMargins(0, 0, 0, 0); chipLay->setSpacing(4);

    // ── Always-visible main row: [funnel] search … count — the Models tab's layout. ──
    m_gridBtn = new QToolButton;
    m_gridBtn->setText(QStringLiteral("▦"));
    m_gridBtn->setCheckable(true);
    m_gridBtn->setFixedSize(28, kBarH);
    m_gridBtn->setStyleSheet(QString::fromLatin1(kIconBtnQss));
    m_gridBtn->setCursor(Qt::PointingHandCursor);
    m_gridBtn->setToolTip(QStringLiteral("Toggle thumbnail (grid) view"));
    connect(m_gridBtn, &QToolButton::toggled, this, [this](bool on) { setGridView(on); });

    auto* mainRow = new QHBoxLayout();
    mainRow->setSpacing(4);
    mainRow->addWidget(m_filtersToggle);
    mainRow->addWidget(m_search, 1);
    mainRow->addWidget(m_gridBtn);
    mainRow->addWidget(m_filterChips);
    mainRow->addStretch(1);
    mainRow->addWidget(m_selLabel);
    lay->addLayout(mainRow);

    m_model = new SnoListModel(this);
    m_model->setModelsColumns(true);   // SNO | Icon | FILENAME | NAME | COLLECTION
    m_view = new QTableView;
    m_view->setModel(m_model);
    m_view->setSortingEnabled(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->verticalHeader()->setVisible(false);
    m_view->hideColumn(1);   // Icon
    m_view->hideColumn(3);   // NAME
    m_view->hideColumn(4);   // COLLECTION
    m_view->setColumnWidth(0, 70);   // SNO
    m_view->horizontalHeader()->setStretchLastSection(true);
    CsvCopy::install(m_view);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& p) { showBrowserMenu(m_view, p); });
    m_view->viewport()->setMouseTracking(true);      // row hover → info popup
    m_view->viewport()->installEventFilter(this);

    // ── Grid view: IconMode QListView over the SAME model + selection as the table. ──
    m_model->setIconProvider([this](int sno) { return gridThumb(sno); });
    m_grid = new QListView;
    m_grid->setModel(m_model);
    m_grid->setModelColumn(2);                          // FILENAME (icon + caption in grid mode)
    m_grid->setSelectionModel(m_view->selectionModel()); // share selection with the table
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setUniformItemSizes(true);
    m_grid->setWordWrap(false);
    m_grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_grid->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_grid->setSpacing(6);
    m_grid->setLayoutMode(QListView::SinglePass);
    m_grid->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_grid->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_grid->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    m_gridPx = qBound(48, QSettings().value(QStringLiteral("tex/gridPx"), 88).toInt(), 192);
    {
        m_grid->setIconSize(QSize(m_gridPx, m_gridPx));
        m_grid->setGridSize(QSize(m_gridPx + 26, m_gridPx + 34));
        m_grid->setItemDelegate(new GridItemDelegate(m_gridPx, m_grid));
    }
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_grid, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& p) { showBrowserMenu(m_grid, p); });
    m_grid->viewport()->setMouseTracking(true);
    m_grid->viewport()->installEventFilter(this);   // hover preview + Ctrl+wheel tile resize
    // Decode thumbnails for visible cells only, once scrolling settles (debounced). This is what
    // keeps a fast scroll from queuing every row you passed.
    m_gridScrollTimer = new QTimer(this);
    m_gridScrollTimer->setSingleShot(true);
    m_gridScrollTimer->setInterval(120);
    connect(m_gridScrollTimer, &QTimer::timeout, this, [this] { queueVisibleGridThumbs(); });
    connect(m_grid->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { if (m_grid->isVisible()) m_gridScrollTimer->start(); });
    connect(m_model, &QAbstractItemModel::modelReset, this,
            [this] { if (m_grid && m_grid->isVisible()) m_gridScrollTimer->start(); });
    connect(m_model, &QAbstractItemModel::layoutChanged, this,
            [this] { if (m_grid && m_grid->isVisible()) m_gridScrollTimer->start(); });

    m_browserStack = new QStackedWidget;
    m_browserStack->addWidget(m_view);   // 0 = table
    m_browserStack->addWidget(m_grid);   // 1 = grid
    lay->addWidget(m_browserStack, 1);

    connect(m_search, &QLineEdit::textChanged, this, [this](const QString&) { applyNameFilter(); });
    connect(m_sortCombo, &QComboBox::currentIndexChanged, this, [this](int) { applySort(); });
    // Populate the funnel's tag groups when the appearance/link indexes come online.
    connect(&AppearanceMeta::instance(), &AppearanceMeta::readyChanged, this,
            [this] { refillTagPanel(); applyNameFilter(); });
    connect(m_multiSelect, &QCheckBox::toggled, this, [this](bool on) {
        const auto mode = on ? QAbstractItemView::ExtendedSelection
                             : QAbstractItemView::SingleSelection;
        m_view->setSelectionMode(mode);
        if (m_grid) m_grid->setSelectionMode(mode);
    });
    connect(m_view->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { onSelectionChanged(); });
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) { updateSelLabel(); });

    // Restore the remembered layout (table / grid) across launches.
    setGridView(QSettings().value(QStringLiteral("tex/gridView"), false).toBool());
    return w;
}

QWidget* TexturesTab::buildMiddle()
{
    auto* w = new QWidget;
    auto* lay = new QVBoxLayout(w);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(6);

    // FILE INFO as a normal section above the preview (selectable text for copying).
    lay->addWidget(new QLabel(QStringLiteral("FILE INFO")));
    auto* grid = new QVBoxLayout(); grid->setSpacing(2);
    for (const char* key : {"Filename", "Filesize", "Format", "Size (meta)",
                            "Size (displayed)", "Face count", "Used by", "Tags"}) {
        auto* r = new QHBoxLayout();
        auto* k = new QLabel(QString::fromLatin1(key)); k->setFixedWidth(110);
        k->setStyleSheet(QStringLiteral("color:#888;"));
        auto* v = new QLabel(QStringLiteral("—"));
        v->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        v->setCursor(Qt::IBeamCursor);
        r->addWidget(k); r->addWidget(v, 1);
        grid->addLayout(r);
        m_infoVals.insert(QString::fromLatin1(key), v);
    }
    lay->addLayout(grid);

    // Preview tabs. TEXTURE PREVIEW (GL + channel isolation) / TEXFRAMES.
    m_previewTabs = new QTabWidget;
    m_previewTabs->setTabPosition(QTabWidget::North);
    m_texStack = new QStackedWidget;
    m_preview = new GLTextureWidget;
    m_preview->setMinimumSize(256, 256);
    m_chanLabel = new QLabel;
    m_chanLabel->setAlignment(Qt::AlignCenter);
    m_chanLabel->setStyleSheet(QStringLiteral("background:#111;"));
    m_texStack->addWidget(m_preview);    // index 0: RGB GPU view
    m_texStack->addWidget(m_chanLabel);  // index 1: isolated channel grayscale

    auto* texPage = new QWidget;
    auto* texLay = new QVBoxLayout(texPage);
    texLay->setContentsMargins(0, 0, 0, 0); texLay->setSpacing(2);
    texLay->addWidget(m_texStack, 1);
    auto* chRow = new QHBoxLayout(); chRow->setSpacing(2);
    auto* chLbl = new QLabel(QStringLiteral("Channels")); chLbl->setStyleSheet(QStringLiteral("color:#888;"));
    chRow->addWidget(chLbl);
    static const char* const kCh[5] = {"RGB", "R", "G", "B", "A"};
    for (int i = 0; i < 5; ++i) {
        auto* b = new QPushButton(QString::fromLatin1(kCh[i]));
        b->setCheckable(true); b->setMaximumWidth(40);
        b->setChecked(i == 0);
        connect(b, &QPushButton::clicked, this, [this, i] { setChannel(i); });
        m_chanBtns[i] = b;
        chRow->addWidget(b);
    }
    m_checkerBtn = new QPushButton(QStringLiteral("Alpha BG"));
    m_checkerBtn->setCheckable(true);
    m_checkerBtn->setMaximumWidth(64);
    m_checkerBtn->setToolTip(QStringLiteral("Show a checkerboard behind transparent pixels"));
    connect(m_checkerBtn, &QPushButton::toggled, this,
            [this](bool on) { if (m_preview) m_preview->setCheckerboard(on); });
    chRow->addWidget(m_checkerBtn);
    chRow->addSpacing(8);
    m_faceCombo = new QComboBox;
    m_faceCombo->setToolTip(QStringLiteral("Cubemap / array face"));
    m_faceCombo->hide();   // shown only when faceCount > 1
    connect(m_faceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int i) { if (i >= 0) { m_curFace = i; uploadFaceMip(); } });
    chRow->addWidget(m_faceCombo);
    chRow->addStretch();
    texLay->addLayout(chRow);

    // Pixel inspector readout (filled from the GL widget's hover signal).
    m_pixelLabel = new QLabel(QStringLiteral(" "));
    m_pixelLabel->setStyleSheet(QStringLiteral("color:#9aa;font-family:monospace;font-size:10px;"));
    texLay->addWidget(m_pixelLabel);
    connect(m_preview, &GLTextureWidget::hoverUv, this,
            [this](QPointF uv) { onPreviewHover(uv); });

    // Scroll-to-zoom / drag-to-pan hint + right-click copy/save on the GL widget.
    m_preview->setToolTip(QStringLiteral("Scroll = zoom · drag = pan · double-click = reset"));
    m_preview->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_preview, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        const QImage img = m_preview->grabImage();
        if (img.isNull()) return;
        QMenu menu(this);
        menu.addAction(QStringLiteral("Copy image"), this,
                       [img]() { QApplication::clipboard()->setImage(img); });
        menu.addAction(QStringLiteral("Save image…"), this, [this, img]() {
            const QString stem = m_currentName.isEmpty() ? QStringLiteral("texture") : m_currentName;
            const QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Save texture"),
                stem + QStringLiteral(".png"), QStringLiteral("PNG (*.png);;JPEG (*.jpg)"));
            if (!f.isEmpty()) img.save(f);
        });
        menu.exec(m_preview->mapToGlobal(p));
    });

    m_previewTabs->addTab(texPage, QStringLiteral("TEXTURE PREVIEW"));
    m_galleryScroll = new QScrollArea;
    m_galleryScroll->setWidgetResizable(true);
    // Reserve the scrollbar so the viewport width is stable (prevents a rebuild loop).
    m_galleryScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_galleryScroll->setWidget(new QWidget);   // content rebuilt per texture in buildGallery()
    m_galleryScroll->viewport()->installEventFilter(this);   // refit tiles on resize
    m_previewTabs->addTab(m_galleryScroll, QStringLiteral("TEXFRAMES PREVIEW"));
    connect(m_previewTabs, &QTabWidget::currentChanged, this, [this](int i) {
        if (i == 1) buildGallery();   // build lazily when the gallery is shown
    });
    lay->addWidget(m_previewTabs, 1);

    // ── CHANNELS strip below the preview: RGBA split, or PBR via a material ──
    auto* tpHead = new QHBoxLayout();
    auto* tpLbl = new QLabel(QStringLiteral("CHANNELS")); tpLbl->setStyleSheet(QStringLiteral("color:#888;"));
    tpHead->addWidget(tpLbl);
    tpHead->addStretch();
    m_chanViewCombo = new QComboBox;
    m_chanViewCombo->hide();   // shown only when a texture has PBR materials to switch to
    m_chanViewCombo->setToolTip(QStringLiteral(
        "RGBA channel split of this texture, or the PBR roles of an associated material."));
    connect(m_chanViewCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int i) {
        if (i >= 0) QSettings().setValue(QStringLiteral("tex/chanView"), i);
        populateChannelView();
    });
    tpHead->addWidget(m_chanViewCombo);
    lay->addLayout(tpHead);
    buildChannelStrip(lay);
    return w;
}

QWidget* TexturesTab::buildRight()
{
    auto* w = new QWidget;
    auto* lay = new QVBoxLayout(w);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(6);

    // ── TEXFRAMES ──
    auto* tfHead = new QHBoxLayout();
    m_tfTitle = new QLabel(QStringLiteral("TEXFRAMES (0)"));
    tfHead->addWidget(m_tfTitle); tfHead->addStretch();
    m_trimCheck = new QCheckBox(QStringLiteral("Trim TexFrame"));
    m_trimCheck->setChecked(QSettings().value(QStringLiteral("tex/trim"), false).toBool());
    m_trimCheck->setToolTip(QStringLiteral("Crop exported frames to their trimmed (tight) bounds."));
    connect(m_trimCheck, &QCheckBox::toggled, this,
            [](bool on) { QSettings().setValue(QStringLiteral("tex/trim"), on); });
    tfHead->addWidget(m_trimCheck);
    lay->addLayout(tfHead);

    m_framesModel = new QStandardItemModel(0, 5, this);
    m_framesModel->setHorizontalHeaderLabels({QStringLiteral("Idx"), QStringLiteral("Hash Hex"),
        QStringLiteral("Hash Dec"), QStringLiteral("Name"), QStringLiteral("Size")});
    m_frames = makeTree(m_framesModel);
    m_frames->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_frames->header()->resizeSection(0, 36);
    m_frames->header()->resizeSection(1, 76);
    m_frames->header()->resizeSection(2, 84);
    m_frames->header()->resizeSection(3, 120);
    CsvCopy::install(m_frames);
    connect(m_frames->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) { onFrameSelected(); });
    lay->addWidget(m_frames, 3);

    auto* tfExp = new QHBoxLayout();
    m_exportFrame = new QPushButton(QStringLiteral("Export Frames…"));
    m_exportFrame->setEnabled(false);
    connect(m_exportFrame, &QPushButton::clicked, this, [this] { exportSelectedFrames(); });
    tfExp->addStretch(); tfExp->addWidget(m_exportFrame);
    lay->addLayout(tfExp);

    // ── ASSOCIATED MODELS ──
    m_assocTitle = new QLabel(QStringLiteral("ASSOCIATED MODELS (0 MODELS / 0 MATERIALS)"));
    lay->addWidget(m_assocTitle);
    m_assocModel = new QStandardItemModel(0, 2, this);
    m_assocModel->setHorizontalHeaderLabels({QStringLiteral("ASSET / ROLE"), QStringLiteral("SNO")});
    m_assocView = new QTreeView;
    m_assocView->setModel(m_assocModel);
    m_assocView->setAlternatingRowColors(true);
    m_assocView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_assocView->header()->resizeSection(0, 240);
    // Single-click selects, double-click expands/collapses (Qt default). Reveal is
    // right-click only — so onAssocDoubleClick is NOT wired to the doubleClicked signal.
    m_assocView->setExpandsOnDoubleClick(true);
    m_assocView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_assocView, &QWidget::customContextMenuRequested, this, &TexturesTab::showAssocMenu);
    CsvCopy::install(m_assocView);
    lay->addWidget(m_assocView, 3);

    // ── DEBUG CONSOLE ── load / decode diagnostics (payload, format, codec, decode result).
    auto* dbgRow = new QHBoxLayout();
    auto* dbgTitle = new QLabel(QStringLiteral("DEBUG CONSOLE"));
    dbgRow->addWidget(dbgTitle);
    dbgRow->addStretch(1);
    auto* dbgClear = new QPushButton(QStringLiteral("Clear"));
    dbgClear->setFixedHeight(20);
    dbgRow->addWidget(dbgClear);
    auto* dbgCopy = new QPushButton(QStringLiteral("Copy"));
    dbgCopy->setFixedHeight(20);
    dbgRow->addWidget(dbgCopy);
    lay->addLayout(dbgRow);
    m_texLog = new QPlainTextEdit;
    m_texLog->setReadOnly(true);
    m_texLog->setMaximumBlockCount(2000);            // ring-buffer the log
    m_texLog->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_texLog->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{background:#161616;color:#cfcfcf;font-family:Consolas,'Courier New',monospace;"
        "font-size:11px;border:1px solid #3a3a3a;}"));
    connect(dbgClear, &QPushButton::clicked, m_texLog, &QPlainTextEdit::clear);
    connect(dbgCopy, &QPushButton::clicked, this, [this] {
        if (m_texLog) QGuiApplication::clipboard()->setText(m_texLog->toPlainText());
    });
    lay->addWidget(m_texLog, 2);
    return w;
}

// Append a timestamped line to the Textures debug console (errors in red).
void TexturesTab::logTex(const QString& msg, bool err)
{
    if (!m_texLog) return;
    const QString ts = QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    m_texLog->appendHtml(QStringLiteral("<span style='color:#666'>%1</span> %2")
        .arg(ts, err ? QStringLiteral("<span style='color:#ff6b6b'>%1</span>").arg(msg.toHtmlEscaped())
                     : msg.toHtmlEscaped()));
    m_texLog->verticalScrollBar()->setValue(m_texLog->verticalScrollBar()->maximum());
}

// Build the 6-tile channel strip (visual parity with the Models tab TEXTURE PREVIEW).
void TexturesTab::buildChannelStrip(QVBoxLayout* lay)
{
    auto* row = new QHBoxLayout();
    row->setSpacing(1);
    row->setContentsMargins(0, 0, 0, 0);
    constexpr int kTile = 92;
    for (int i = 0; i < 6; ++i) {
        m_chanImg[i] = new QLabel;
        m_chanImg[i]->setFixedSize(kTile, kTile);
        m_chanImg[i]->setAlignment(Qt::AlignCenter);
        m_chanImg[i]->setScaledContents(false);
        m_chanImg[i]->setStyleSheet(QStringLiteral(
            "QLabel{border:1px solid #444;background:#1b1b1b;}"));
        m_chanImg[i]->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_chanImg[i], &QWidget::customContextMenuRequested, this,
                [this, i](const QPoint& p) {
            if (m_chanFull[i].isNull()) return;
            QMenu menu(this);
            menu.addAction(QStringLiteral("Copy image"), this,
                           [this, i]() { QApplication::clipboard()->setImage(m_chanFull[i]); });
            menu.addAction(QStringLiteral("Save image…"), this, [this, i]() {
                const QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Save channel"),
                    QStringLiteral("channel.png"), QStringLiteral("PNG (*.png)"));
                if (!f.isEmpty()) m_chanFull[i].save(f);
            });
            menu.exec(m_chanImg[i]->mapToGlobal(p));
        });
        m_chanCap[i] = new QLabel(m_chanImg[i]);
        m_chanCap[i]->setStyleSheet(QStringLiteral(
            "QLabel{color:#fff;background:rgba(0,0,0,150);border:0;padding:0px 2px;font-size:8px;}"));
        m_chanCap[i]->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_chanCap[i]->move(2, kTile - 14);
        m_chanImg[i]->setMouseTracking(true);
        m_chanImg[i]->installEventFilter(this);   // hover → zoom popup + wheel resize + drag
        row->addWidget(m_chanImg[i]);
    }
    row->addStretch(1);
    lay->addLayout(row);
}

void TexturesTab::setTileCaptions(const char* const labels[6])
{
    for (int i = 0; i < 6; ++i) {
        if (!m_chanCap[i]) continue;
        const QString s = QString::fromLatin1(labels[i]);
        m_chanCap[i]->setText(s);
        m_chanCap[i]->setVisible(!s.isEmpty());
        m_chanCap[i]->adjustSize();
        if (m_chanImg[i]) m_chanCap[i]->move(2, m_chanImg[i]->height() - m_chanCap[i]->height() - 2);
        m_chanCap[i]->raise();
    }
}

void TexturesTab::setChannelTile(int idx, const QImage& img)
{
    if (idx < 0 || idx >= 6 || !m_chanImg[idx]) return;
    m_chanFull[idx] = img;
    if (img.isNull()) { m_chanImg[idx]->setPixmap(QPixmap()); m_chanImg[idx]->setText(QStringLiteral("—")); return; }
    m_chanImg[idx]->setText(QString());
    const int side = qMax(8, m_chanImg[idx]->width() - 6);
    m_chanImg[idx]->setPixmap(QPixmap::fromImage(
        img.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

void TexturesTab::clearChannelStrip()
{
    for (int i = 0; i < 6; ++i) setChannelTile(i, QImage());
}

// Floating zoom popup at the cursor, clamped inside the app window (parity with Models).
void TexturesTab::popupPreview(const QPixmap& scaled)
{
    if (scaled.isNull() || !m_iconPreview) return;
    m_iconPreview->setPixmap(scaled);
    m_iconPreview->resize(scaled.size());
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
    m_iconPreview->move(pos);
    m_iconPreview->show();
    m_iconPreview->raise();
}

void TexturesTab::showTilePreview(int idx)
{
    if (idx < 0 || idx >= 6 || !m_iconPreview || m_chanFull[idx].isNull()) return;
    if (!HoverInfo::imagePreview()) return;
    popupPreview(QPixmap::fromImage(m_chanFull[idx])
        .scaled(m_previewPx, m_previewPx, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void TexturesTab::hideTilePreview()
{
    if (m_iconPreview) m_iconPreview->hide();
}

// CPU-decode any texture by SNO (does NOT disturb the GPU preview).
QImage TexturesTab::decodeTexCpu(int sno)
{
    if (sno <= 0) return {};
    const QString name = m_snoName.value(sno);
    if (name.isEmpty()) return {};
    const QString d4 = Config::d4dataDir();
    TexMeta meta;
    if (!d4.isEmpty()) {
        QFile f(QStringLiteral("%1/json/base/meta/Texture/%2.tex.json").arg(d4, name));
        if (f.open(QIODevice::ReadOnly)) meta = parseTexMetaJson(f.readAll());
    }
    if (!meta.valid) return {};
    QByteArray payload;
    if (m_reader && m_reader->isReady()) payload = m_reader->readPayloadBySno(quint64(sno));
    if (payload.isEmpty()) return {};
    return BcDecode::decode(payload, meta.width, meta.height, meta.eTexFormat);
}

// ── Grid view ───────────────────────────────────────────────────────────────
// Switch the browser between the table and the thumbnail-grid layout (same model + selection).
void TexturesTab::setGridView(bool on)
{
    if (!m_browserStack || !m_model || !m_grid) return;
    m_model->setGridMode(on);   // decoration on FILENAME col only while the grid is up (keeps list fast)
    if (on) {
        m_model->setIconPx(m_gridPx);
        // Make sure every SNO can resolve its file name (grid thumbnails decode by name).
        if (m_snoName.isEmpty() && m_index)
            for (int g : m_index->groups())
                for (const SnoEntry& e : m_index->entries(g))
                    m_snoName.insert(e.snoId, e.name);
    }
    m_browserStack->setCurrentWidget(on ? static_cast<QWidget*>(m_grid)
                                        : static_cast<QWidget*>(m_view));
    if (m_gridBtn && m_gridBtn->isChecked() != on) {
        const bool b = m_gridBtn->blockSignals(true);
        m_gridBtn->setChecked(on);
        m_gridBtn->blockSignals(b);
    }
    QSettings().setValue(QStringLiteral("tex/gridView"), on);
    if (on && m_gridScrollTimer) m_gridScrollTimer->start();   // load the first visible page
}

// Ctrl+wheel over the grid resizes the thumbnail tiles (persisted). Thumbnails are decoded at a
// fixed 160px, so this only rescales cached pixmaps — no re-decode.
void TexturesTab::setGridIconPx(int px)
{
    if (!m_grid) return;
    m_gridPx = qBound(48, px, 192);
    m_grid->setIconSize(QSize(m_gridPx, m_gridPx));
    m_grid->setGridSize(QSize(m_gridPx + 26, m_gridPx + 34));
    m_grid->setItemDelegate(new GridItemDelegate(m_gridPx, m_grid));
    if (m_model) m_model->setIconPx(m_gridPx);
    QSettings().setValue(QStringLiteral("tex/gridPx"), m_gridPx);
    if (m_gridScrollTimer) m_gridScrollTimer->start();   // tile count changed → refresh visible set
}

// Dwell-hover popup for a grid cell: a scaled, bounds-aware floating preview (like the channel
// tiles) plus the configured info lines (Settings ▸ General ▸ On-hover). Decodes the full texture
// once per SNO and caches it so wheel-resizing is instant.
void TexturesTab::showGridPreview(int sno)
{
    if (sno <= 0 || !m_iconPreview) return;
    // The decoded image is included ONLY when hovering an icon (a grid cell) with previews on;
    // over the list rows the popup is the info lines alone. Dimensions still need the decode, so
    // it happens either way — it's cached per SNO.
    const bool wantImage = m_hoverIconArea && HoverInfo::imagePreview();
    if (sno != m_gridPreviewSno) {
        m_gridPreviewImg = decodeTexCpu(sno);
        m_gridPreviewSno = sno;
    }
    QPixmap pm;
    if (wantImage && !m_gridPreviewImg.isNull())
        pm = QPixmap::fromImage(m_gridPreviewImg)
                 .scaled(m_previewPx, m_previewPx, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // Info lines under the image, per the toggles. `cols` carries each line's colour when
    // colour-coding is on (name white, file grey, metadata light grey, ★ new ember).
    QStringList lines;
    QVector<const char*> cols;
    const bool colour = HoverInfo::colourCode();
    const QString name = m_snoName.value(sno);
    if (HoverInfo::on("tex/sno")) { lines << QStringLiteral("%1 · %2").arg(sno).arg(name);
                                    cols << HoverInfo::Col::kFile; }
    if (HoverInfo::on("tex/dims") && !m_gridPreviewImg.isNull()) {
        QString d = QStringLiteral("%1×%2").arg(m_gridPreviewImg.width()).arg(m_gridPreviewImg.height());
        const QString fam = fmtFamily(m_texFmt.value(sno, -1));
        if (!fam.isEmpty()) d += QStringLiteral("  ·  ") + fam;
        lines << d; cols << HoverInfo::Col::kMeta;
    }
    if (HoverInfo::on("tex/size") && m_reader && m_reader->isReady()) {
        const quint64 b = m_reader->payloadSize(quint64(sno));
        if (b) { lines << (b >= 1048576 ? QStringLiteral("%1 MB").arg(double(b) / 1048576.0, 0, 'f', 1)
                                        : QStringLiteral("%1 KB").arg(double(b) / 1024.0, 0, 'f', 0));
                 cols << HoverInfo::Col::kMeta; }
    }
    if (HoverInfo::on("tex/frames") && !name.isEmpty()) {
        QFile f(QStringLiteral("%1/json/base/meta/Texture/%2.tex.json").arg(Config::d4dataDir(), name));
        if (f.open(QIODevice::ReadOnly)) {
            const TexMeta meta = parseTexMetaJson(f.readAll());
            if (!meta.frames.isEmpty()) { lines << QStringLiteral("%1 frames").arg(meta.frames.size());
                                          cols << HoverInfo::Col::kMeta; }
        }
    }
    if (HoverInfo::on("tex/usedby")) {
        AssetLinks& l = AssetLinks::instance();
        if (l.ready()) {
            const int n = l.linksForTexture(sno).size();
            if (n) { lines << QStringLiteral("used by %1 material(s)").arg(n);
                     cols << HoverInfo::Col::kSeries; }
        }
    }
    if (HoverInfo::on("tex/latest") && m_index && m_index->isNew(sno)) {
        lines << QStringLiteral("★ new this update"); cols << HoverInfo::Col::kNew;
    }

    if (lines.isEmpty() && pm.isNull()) { hideTilePreview(); return; }   // nothing to show
    if (!lines.isEmpty()) {
        // Compose image + caption strip into one pixmap (the popup label shows a single pixmap).
        const QFont capFont = font();
        const QFontMetrics fm(capFont);
        const int lh = fm.height() + 1;
        const int capH = lh * lines.size() + 6;
        int w = pm.isNull() ? 0 : pm.width();
        for (const QString& s : lines) w = qMax(w, fm.horizontalAdvance(s) + 12);
        QPixmap out(qBound(200, w, 900), pm.height() + capH);
        out.fill(QColor(26, 26, 28));
        QPainter p(&out);
        if (!pm.isNull()) p.drawPixmap((out.width() - pm.width()) / 2, 0, pm);
        p.fillRect(0, pm.height(), out.width(), capH, QColor(26, 26, 28, 235));
        p.setFont(capFont);
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

// Icon provider for the grid — CACHE-ONLY. It must never spawn a decode, because the model calls it
// for every cell painted while scrolling; enqueuing here is what queued up everything you scrolled
// past. Decodes are requested separately, for visible cells only (queueVisibleGridThumbs).
QPixmap TexturesTab::gridThumb(int sno)
{
    if (sno <= 0) return {};
    QPixmap pm;
    QPixmapCache::find(QStringLiteral("texgrid_%1").arg(sno), &pm);
    return pm;   // null → the delegate draws a placeholder until the thumbnail is decoded
}

// Enqueue a background CPU decode for one texture (dedup via cache + in-flight set). No disk cache —
// QPixmapCache is a bounded, auto-evicting, in-memory store only.
void TexturesTab::requestGridThumb(int sno)
{
    if (sno <= 0) return;
    { QPixmap cached; if (QPixmapCache::find(QStringLiteral("texgrid_%1").arg(sno), &cached)) return; }
    if (m_gridPending.contains(sno)) return;   // decode already in flight

    const QString name = m_snoName.value(sno);
    if (name.isEmpty()) return;
    CascReader* reader = m_reader;
    if (!reader) return;
    m_gridPending.insert(sno);

    const QString d4 = Config::d4dataDir();
    QThreadPool::globalInstance()->start(QRunnable::create([this, sno, name, d4, reader]() {
        QImage img;
        // SEH-guarded: a malformed payload after a game patch must cost one blank thumbnail,
        // not take the whole app down from a worker thread.
        seh::runGuarded("gridThumb", [&]() {
            TexMeta meta;
            if (!d4.isEmpty()) {
                QFile f(QStringLiteral("%1/json/base/meta/Texture/%2.tex.json").arg(d4, name));
                if (f.open(QIODevice::ReadOnly)) meta = parseTexMetaJson(f.readAll());
            }
            if (!meta.valid) return;
            const QByteArray payload = reader->readPayloadBySno(quint64(sno));   // mutex-guarded
            if (payload.isEmpty()) return;
            QImage full = BcDecode::decode(payload, meta.width, meta.height, meta.eTexFormat);
            if (full.isNull()) return;
            // Decode to a fixed 160px (independent of tile size) so Ctrl+wheel enlarging stays crisp
            // without re-decoding; the model + delegate scale it down to the current tile size.
            img = full.scaled(160, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        });
        // Hop back to the GUI thread to build the QPixmap + repaint (QPixmap is GUI-thread only).
        QMetaObject::invokeMethod(this, [this, sno, img]() { onGridThumbReady(sno, img); },
                                  Qt::QueuedConnection);
    }));
}

// Request decodes for ONLY the grid cells currently on screen. Called after scrolling settles (and on
// show / resize / model change) so fast scrolling never queues the rows you flew past.
void TexturesTab::queueVisibleGridThumbs()
{
    if (!m_grid || !m_grid->isVisible() || !m_model) return;
    const QRect vp = m_grid->viewport()->rect();
    if (vp.isEmpty()) return;
    // Find the first item on the top visible row (probe across the top edge — the very corner may
    // land in a gutter between tiles).
    QModelIndex first;
    for (int x = 2; x < vp.width() && !first.isValid(); x += 8)
        first = m_grid->indexAt(QPoint(x, 2));
    if (!first.isValid()) first = m_grid->indexAt(vp.center());
    if (!first.isValid()) return;
    const int rows = m_model->rowCount();
    int guard = 0;
    for (int r = first.row(); r < rows && guard < 600; ++r, ++guard) {
        const QRect vr = m_grid->visualRect(m_model->index(r, 2));
        if (vr.top() > vp.bottom()) break;           // walked past the bottom → done
        if (!vr.intersects(vp)) continue;            // a stray off-screen row (rare) — skip
        if (const SnoEntry* e = m_model->entryAt(r)) requestGridThumb(e->snoId);
    }
}

// GUI thread: cache the decoded thumbnail (bounded, in-memory) and repaint just its cell.
void TexturesTab::onGridThumbReady(int sno, const QImage& img)
{
    m_gridPending.remove(sno);
    if (img.isNull()) return;   // undecodable texture — leave the placeholder
    QPixmapCache::insert(QStringLiteral("texgrid_%1").arg(sno), QPixmap::fromImage(img));
    if (m_model) m_model->refreshIconForSno(sno);
}

// Shared right-click menu for both the table and the grid (they share one selection model).
void TexturesTab::showBrowserMenu(QAbstractItemView* view, const QPoint& viewportPos)
{
    if (!view || !m_model) return;
    QStringList names, snoStrs;
    auto add = [&](const SnoEntry* e) { if (e) { names << e->name; snoStrs << QString::number(e->snoId); } };
    for (const QModelIndex& idx : view->selectionModel()->selectedRows()) add(m_model->entryAt(idx.row()));
    if (names.isEmpty()) add(m_model->entryAt(view->indexAt(viewportPos).row()));
    if (names.isEmpty()) return;
    QMenu menu(this);
    const int n = names.size();
    auto copy = [](const QStringList& l) { QApplication::clipboard()->setText(l.join(QLatin1Char('\n'))); };
    auto prev = [](const QString& s) { return s.size() > 30 ? s.left(29) + QChar(0x2026) : s; };
    const QString exCount = QStringLiteral("%1 texture%2").arg(n).arg(n == 1 ? QString() : QStringLiteral("s"));
    menu.addAction(QStringLiteral("Export (last dir)  —  %1").arg(exCount), this, [this] { exportSelected(false, true); });
    menu.addAction(QStringLiteral("Export to…  —  %1").arg(exCount), this, [this] { exportSelected(false, false); });
    {
        // Copy the (first) selected texture's decoded image to the clipboard.
        const int firstSno  = snoStrs.first().toInt();
        const QString first = names.first();
        menu.addAction(QStringLiteral("Copy image  (%1)").arg(prev(first)), this, [this, firstSno, first] {
            if (!m_snoName.contains(firstSno)) m_snoName.insert(firstSno, first);
            const QImage img = decodeTexCpu(firstSno);
            if (!img.isNull()) QApplication::clipboard()->setImage(img);
        });
    }
    menu.addSeparator();
    if (n == 1) {
        menu.addAction(QStringLiteral("Copy SNO id  (%1)").arg(snoStrs.first()), this, [snoStrs, copy] { copy(snoStrs); });
        menu.addAction(QStringLiteral("Copy file name  (%1)").arg(prev(names.first())), this, [names, copy] { copy(names); });
        menu.addAction(QStringLiteral("Copy name  (%1)").arg(prev(names.first())), this, [names, copy] { copy(names); });
        QAction* aC = menu.addAction(QStringLiteral("Copy collection name  (—)"));   // textures have no collection
        aC->setEnabled(false);
    } else {
        menu.addAction(QStringLiteral("Copy %1 SNO ids").arg(n), this, [snoStrs, copy] { copy(snoStrs); });
        menu.addAction(QStringLiteral("Copy %1 file names").arg(n), this, [names, copy] { copy(names); });
        menu.addAction(QStringLiteral("Copy %1 names").arg(n), this, [names, copy] { copy(names); });
    }
    menu.exec(view->viewport()->mapToGlobal(viewportPos));
}

// Bulk extractor: decode + save each matched texture into dir (PNG/JPG per Settings ▸ Export tex
// format). Self-contained (doesn't rely on the tab's list being loaded); incremental via _bulk_manifest.json.
void TexturesTab::bulkExportTextures(const QVector<QPair<int, QString>>& items, const QString& dir,
                                     bool onlyNew, const BatchSink* sink)
{
    if (items.isEmpty() || dir.isEmpty() || !m_reader || !m_reader->isReady()) return;
    const QString d4 = Config::d4dataDir();
    const bool jpg = QSettings().value(QStringLiteral("tex/format"), QStringLiteral("png"))
                         .toString().contains(QLatin1String("jp"), Qt::CaseInsensitive);
    const QString ext = jpg ? QStringLiteral(".jpg") : QStringLiteral(".png");

    const QString manPath = QDir(dir).filePath(QStringLiteral("_bulk_manifest.json"));
    QJsonArray man;
    { QFile f(manPath); if (f.open(QIODevice::ReadOnly)) man = QJsonDocument::fromJson(f.readAll()).array(); }
    QSet<int> already;
    for (const QJsonValue& v : man) already.insert(v.toObject().value(QStringLiteral("sno")).toInt());

    // "Only new" skip-check snapshot: ONE directory listing up front instead of a filesystem stat
    // per item (thousands of QFileInfo::exists calls on big runs).
    QSet<QString> existingFiles;
    if (onlyNew)
        for (const QString& fn : QDir(dir).entryList(QDir::Files))
            existingFiles.insert(fn);

    // Per-stage timing (cheap atomics) — logged with the summary so the actual bottleneck of a
    // run (read vs decode vs encode) is visible instead of guessed at.
    std::atomic<qint64> nsRead{0}, nsDecode{0}, nsEncode{0};

    int ok = 0, skip = 0, step = 0;
    QStringList failed;
    QMutex shared;   // guards man / already / ok / skip / failed across the parallel workers
    // Sink (Bulk Extract's live console) or the classic modal dialog.
    std::unique_ptr<QProgressDialog> prog;
    if (!sink) {
        prog = std::make_unique<QProgressDialog>(QStringLiteral("Extracting textures…"),
                                                 QStringLiteral("Cancel"), 0, items.size(), this);
        prog->setWindowModality(Qt::WindowModal);
    }
    auto texFail = [&](const QString& name, const QString& why) {
        // Caller holds `shared` (or is the serial path).
        failed << (name + QStringLiteral(" — ") + why);
        if (sink && sink->log) sink->log(QStringLiteral("  ✗ %1 — %2").arg(name, why));
    };
    // One item, start to finish: skip-check, meta, payload, decode, save, ledger. The heavy work
    // (BC decode + PNG encode) runs OUTSIDE the lock; only the shared containers lock. SEH-guarded
    // per item — a corrupt payload logs and skips, never takes the run (or app) down.
    auto processOne = [&](const QPair<int, QString>& it, bool locked) {
        const QString outName = NameTemplate::texture(it.second, it.first) + ext;
        const QString outPath = QDir(dir).filePath(outName);
        {
            std::optional<QMutexLocker<QMutex>> l;
            if (locked) l.emplace(&shared);
            if (onlyNew && (already.contains(it.first) || existingFiles.contains(outName))) { ++skip; return; }
        }
        seh::HardwareFault fault;
        const bool survived = seh::runGuarded("bulk-tex", [&]() {
            QElapsedTimer stage; stage.start();
            TexMeta meta;
            { QFile f(QStringLiteral("%1/json/base/meta/Texture/%2.tex.json").arg(d4, it.second));
              if (f.open(QIODevice::ReadOnly)) meta = parseTexMetaJson(f.readAll()); }
            std::optional<QMutexLocker<QMutex>> l;
            auto guard = [&] { if (locked) l.emplace(&shared); };
            if (!meta.valid) { guard(); texFail(it.second, QStringLiteral("no/invalid .tex.json (update d4data?)")); return; }
            const QByteArray payload = m_reader->readPayloadBySno(quint64(it.first));
            nsRead += stage.nsecsElapsed(); stage.restart();
            if (payload.isEmpty()) { guard(); texFail(it.second, QStringLiteral("no payload (encrypted or missing)")); return; }
            const QImage img = BcDecode::decode(payload, meta.width, meta.height, meta.eTexFormat);
            nsDecode += stage.nsecsElapsed(); stage.restart();
            if (img.isNull()) { guard(); texFail(it.second, QStringLiteral("BC decode failed (format %1)").arg(meta.eTexFormat)); return; }
            bool saved = false;
            if (ext == QLatin1String(".png")) {
                // Fast PNG: low zlib effort — PNG is LOSSLESS at every compression level, so this
                // only trades a slightly larger file for a 3-5× faster encode. Pixels identical.
                QImageWriter w(outPath);
                w.setQuality(90);
                saved = w.write(img);
            } else {
                saved = img.save(outPath);   // JPEG: untouched — its quality setting is lossy
            }
            nsEncode += stage.nsecsElapsed();
            if (!saved) { guard(); texFail(it.second, QStringLiteral("could not write file")); return; }
            guard();
            ++ok;
            if (sink && sink->log)
                sink->log(QStringLiteral("  ✓ %1  (%2×%3)").arg(it.second).arg(img.width()).arg(img.height()));
            if (!already.contains(it.first)) {
                man.append(QJsonObject{{QStringLiteral("sno"),  it.first},
                                       {QStringLiteral("name"), it.second},
                                       {QStringLiteral("file"), outName},
                                       {QStringLiteral("date"), QDateTime::currentDateTime().toString(Qt::ISODate)}});
                already.insert(it.first);
            }
        }, &fault);
        if (!survived) {
            std::optional<QMutexLocker<QMutex>> l;
            if (locked) l.emplace(&shared);
            texFail(it.second, QStringLiteral("CRASHED (%1) — skipped, run continues").arg(fault.what));
        }
    };

    int par = 1;
    if (sink) {
        const int cfg = QSettings().value(QStringLiteral("bulk/parallel"), -1).toInt();
        par = qBound(1, cfg <= 0 ? QThread::idealThreadCount() : cfg, 16);   // -1 = Auto (core count)
    }
    if (par > 1 && items.size() > 1) {
        // ── Parallel path (Bulk Extract worker): N decode workers over an atomic work index.
        // CASC reads stay mutex-serialized inside the reader; the BC decode + PNG encode — the
        // actual time sink — run concurrently. sink callbacks marshal to the GUI, so they're
        // safe from any thread; sink->canceled also holds the workers while paused.
        std::atomic<int> next{0}, done{0};
        auto workerFn = [&]() {
            for (;;) {
                if (sink->canceled && sink->canceled()) break;
                const int i = next.fetch_add(1);
                if (i >= items.size()) break;
                processOne(items[i], /*locked=*/true);
                const int d = done.fetch_add(1) + 1;
                if (sink->progress) sink->progress(d, items.size());
            }
        };
        std::vector<std::thread> pool;
        pool.reserve(par);
        for (int t = 0; t < par; ++t) pool.emplace_back(workerFn);
        for (auto& th : pool) th.join();
        step = done.load();
    } else {
        for (const auto& it : items) {
            if (prog && prog->wasCanceled()) break;
            if (sink && sink->canceled && sink->canceled()) break;
            if (prog) {
                prog->setValue(step);
                prog->setLabelText(QStringLiteral("Extracting %1…").arg(it.second));
            }
            if (sink && sink->progress) sink->progress(step, items.size());
            ++step;
            if (!sink) QCoreApplication::processEvents();   // legacy modal path only
            processOne(it, /*locked=*/false);
        }
    }
    if (prog) prog->setValue(items.size());
    if (sink && sink->progress) sink->progress(step, items.size());   // final tick
    QFile mf(manPath);
    if (mf.open(QIODevice::WriteOnly | QIODevice::Truncate))
        mf.write(QJsonDocument(man).toJson(QJsonDocument::Indented));
    if (!failed.isEmpty()) {
        QFile ff(QDir(dir).filePath(QStringLiteral("_bulk_failed.txt")));
        if (ff.open(QIODevice::WriteOnly | QIODevice::Truncate)) ff.write(failed.join('\n').toUtf8());
    }
    const QString summary = QStringLiteral("Textures: %1 exported, %2 already present, %3 failed%4.")
            .arg(ok).arg(skip).arg(failed.size())
            .arg(failed.isEmpty() ? QString() : QStringLiteral(" (see _bulk_failed.txt)"));
    if (sink && sink->log) {
        sink->log(summary);   // console runs must not stack modal boxes per subfolder
        // Stage breakdown (summed across workers) — shows where the time actually went.
        if (ok > 0) {
            auto s = [](qint64 ns) { return QString::number(double(ns) / 1e9, 'f', 1); };
            sink->log(QStringLiteral("   timing (worker-seconds): read %1 · decode %2 · encode %3 · %4 worker(s)")
                          .arg(s(nsRead.load()), s(nsDecode.load()), s(nsEncode.load())).arg(par));
        }
    }
    else
        QMessageBox::information(this, QStringLiteral("Bulk extract"), summary);
}

// Rebuild the view dropdown: "RGBA channels" + one entry per associated material.
void TexturesTab::refreshChannelCombo(int texSno)
{
    if (!m_chanViewCombo) return;
    QSignalBlocker block(m_chanViewCombo);
    m_chanViewCombo->clear();
    m_chanMatSnos.clear();
    m_chanViewCombo->addItem(QStringLiteral("RGBA channels"));
    AssetLinks& links = AssetLinks::instance();
    if (links.ready()) {
        const QVector<AssetLinks::MatLink> rows = links.linksForTexture(texSno);
        for (int i = 0; i < rows.size() && i < 40; ++i) {
            const int mat = rows[i].matSno;
            const QString nm = m_snoName.value(mat, QStringLiteral("#%1").arg(mat));
            m_chanViewCombo->addItem(QStringLiteral("PBR: %1").arg(nm));
            m_chanMatSnos.append(mat);
        }
    }
    // Restore the remembered view choice (clamped to what's available now).
    const int saved = QSettings().value(QStringLiteral("tex/chanView"), 0).toInt();
    m_chanViewCombo->setCurrentIndex(qBound(0, saved, m_chanViewCombo->count() - 1));
    // Only surface the view dropdown when it actually offers a choice — i.e. when
    // there are associated PBR materials to switch to. With just "RGBA channels"
    // it's a redundant single-item control (the labelled tile strip already shows
    // the RGBA split), so hide it.
    m_chanViewCombo->setVisible(m_chanViewCombo->count() > 1);
}

// Fill the six tiles per the current dropdown selection.
void TexturesTab::populateChannelView()
{
    if (!m_chanViewCombo || !m_chanImg[0]) return;
    const int sel = m_chanViewCombo->currentIndex();

    if (sel <= 0) {   // ── RGBA channel split of the selected texture ──
        static const char* const caps[6] = {"RGBA", "RED", "GREEN", "BLUE", "ALPHA", "LUMA"};
        setTileCaptions(caps);
        clearChannelStrip();
        QImage img = m_fullImage.isNull() ? m_preview->grabImage() : m_fullImage;
        if (img.isNull()) return;
        img = img.convertToFormat(QImage::Format_RGBA8888);
        setChannelTile(0, img);
        setChannelTile(1, channelGrey(img, 0));
        setChannelTile(2, channelGrey(img, 1));
        setChannelTile(3, channelGrey(img, 2));
        setChannelTile(4, channelGrey(img, 3));
        setChannelTile(5, img.convertToFormat(QImage::Format_Grayscale8)
                              .convertToFormat(QImage::Format_RGBA8888));
        return;
    }

    // ── PBR roles, pulled from the chosen associated material's textures ──
    static const char* const caps[6] = {"COLOR", "ROUGHNESS", "METAL", "NORMAL", "ALPHA", "EMISSIVE"};
    setTileCaptions(caps);
    clearChannelStrip();
    const int matSno = m_chanMatSnos.value(sel - 1, 0);
    if (matSno <= 0) return;
    AssetLinks& links = AssetLinks::instance();
    QVector<QPair<int, int>> pairs;
    for (const AssetLinks::MatLink& l : links.linksForTexture(m_currentSno))
        if (l.matSno == matSno) { pairs = l.texPairs; break; }
    QImage baseColor;
    bool filled[6] = {false, false, false, false, false, false};
    for (const auto& tp : pairs) {
        const QString role = AssetLinks::slotRole(tp.second);
        int tile = -1;
        if (role == QLatin1String("BASE_COLOR"))     tile = 0;
        else if (role == QLatin1String("ROUGHNESS")) tile = 1;
        else if (role == QLatin1String("METALLIC"))  tile = 2;
        else if (role == QLatin1String("NORMAL"))    tile = 3;
        else if (role == QLatin1String("EMISSIVE"))  tile = 5;
        if (tile < 0 || filled[tile]) continue;
        const QImage img = decodeTexCpu(tp.first);
        if (img.isNull()) continue;
        filled[tile] = true;
        setChannelTile(tile, img);
        if (tile == 0) baseColor = img;
    }
    if (!baseColor.isNull() && baseColor.hasAlphaChannel())
        setChannelTile(4, channelGrey(baseColor, 3));
}

// (Re)build the funnel's grouped tag checkboxes from the appearance tag groups, preserving ticks.
// A texture "carries" a tag when the tag appears in its appearance-derived blob (texBlob).
void TexturesTab::refillTagPanel()
{
    if (!m_tagPanelBody) return;
    auto* bl = static_cast<QVBoxLayout*>(m_tagPanelBody->layout());
    while (QLayoutItem* it = bl->takeAt(0)) { if (it->widget()) it->widget()->deleteLater(); delete it; }
    m_tagChecks.clear();
    const QMap<QString, QStringList> groups = AppearanceMeta::instance().tagGroups();
    if (groups.isEmpty()) {
        bl->addWidget(new QLabel(QStringLiteral("Gear tags load with the index…"), m_tagPanelBody));
        bl->addStretch(1);
        updateFunnelTint();
        return;
    }
    // Only the gear-relevant groups belong here. The "Category" group (Player/Monster/NPC/…) duplicates
    // the texture-native Category above, so it's dropped to keep the menu clean and unambiguous.
    static const QSet<QString> kKeep = {QStringLiteral("Class"), QStringLiteral("Type"),
                                        QStringLiteral("Gender")};
    for (auto g = groups.constBegin(); g != groups.constEnd(); ++g) {
        if (g.value().isEmpty() || !kKeep.contains(g.key())) continue;
        auto* gw = new QWidget(m_tagPanelBody);
        gw->setObjectName(QStringLiteral("tagGroup"));   // the search box finds these
        auto* gl = new QVBoxLayout(gw);
        gl->setContentsMargins(0, 2, 0, 2);
        gl->setSpacing(2);
        auto* head = new QLabel(QStringLiteral("%1 (%2)").arg(g.key()).arg(g.value().size()), gw);
        head->setStyleSheet(QString::fromLatin1(kHdrQss));
        gl->addWidget(head);
        QStringList vals = g.value();
        vals.removeDuplicates();
        vals.sort(Qt::CaseInsensitive);
        for (const QString& tv : vals) {
            auto* cb = new QCheckBox(tv, gw);
            cb->setChecked(m_tagFilter.contains(tv));
            m_tagChecks.insert(tv, cb);
            connect(cb, &QCheckBox::toggled, this, [this, tv](bool on) {
                if (on) m_tagFilter.insert(tv); else m_tagFilter.remove(tv);
                applyNameFilter();
            });
            gl->addWidget(cb);
        }
        bl->addWidget(gw);
    }
    bl->addStretch(1);
    if (m_tagOrChk) { QSignalBlocker b(m_tagOrChk); m_tagOrChk->setChecked(m_tagOrMode); }
    updateFunnelTint();
}

// Gold-tint the funnel while any filter is active (parity with the Models / Wardrobe tabs).
void TexturesTab::updateFunnelTint()
{
    if (!m_filtersToggle) return;
    const bool active = !m_tagFilter.isEmpty() || !m_catFilter.isEmpty() || !m_fmtFilter.isEmpty()
                        || (m_orphanCheck && m_orphanCheck->isChecked())
                        || (m_onlyDecrypted && m_onlyDecrypted->isChecked());
    m_filtersToggle->setStyleSheet(active
        ? QStringLiteral("QToolButton{padding:1px;border:1px solid #a07a1a;border-radius:3px;"
                         "background:#3a2f12;} QToolButton:hover{border-color:#b0453c;}")
        : QString::fromLatin1(kIconBtnQss));
}

// Rebuild the inline active-filter chips (one removable pill per set filter).
void TexturesTab::rebuildFilterChips()
{
    if (!m_filterChips || !m_filterChips->layout()) return;
    QLayout* lay = m_filterChips->layout();
    while (QLayoutItem* it = lay->takeAt(0)) {
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    auto addChip = [this, lay](const QString& label, const std::function<void()>& clear) {
        auto* chip = new QToolButton(m_filterChips);
        chip->setText(label + QStringLiteral("  ✕"));
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(QStringLiteral("Remove this filter"));
        chip->setStyleSheet(QStringLiteral(
            "QToolButton{background:#3a3a3a; border:1px solid #565656; border-radius:9px;"
            " padding:1px 8px; color:#d8a23a;} QToolButton:hover{background:#4a4a4a;}"));
        connect(chip, &QToolButton::clicked, this, [clear]() { clear(); });
        lay->addWidget(chip);
    };
    for (const QString& c : m_catFilter)
        addChip(c, [this, c] { if (QCheckBox* cb = m_catChecks.value(c)) cb->setChecked(false);
                               else { m_catFilter.remove(c); applyNameFilter(); } });
    for (const QString& t : m_tagFilter)
        addChip(t, [this, t] { if (QCheckBox* c = m_tagChecks.value(t)) c->setChecked(false);
                               else { m_tagFilter.remove(t); applyNameFilter(); } });
    for (const QString& f : m_fmtFilter)
        addChip(f, [this, f] { m_fmtFilter.remove(f); applyNameFilter(); });
    if (m_orphanCheck && m_orphanCheck->isChecked())
        addChip(QStringLiteral("Orphans"), [this] { m_orphanCheck->setChecked(false); });
    if (m_onlyDecrypted && m_onlyDecrypted->isChecked())
        addChip(QStringLiteral("Only decrypted"), [this] { m_onlyDecrypted->setChecked(false); });
    updateFunnelTint();
}

QString TexturesTab::texBlob(int sno)
{
    const auto it = m_texBlob.constFind(sno);
    if (it != m_texBlob.constEnd()) return it.value();
    QString blob;
    AssetLinks& links = AssetLinks::instance();
    if (links.ready()) {
        AppearanceMeta& am = AppearanceMeta::instance();
        QSet<int> apps;
        for (const AssetLinks::MatLink& l : links.linksForTexture(sno))
            for (int a : l.apps) apps.insert(a);
        for (int a : apps) {
            blob += QLatin1Char(' ') + am.titleFor(a) + QLatin1Char(' ') + am.collectionFor(a);
            for (const QString& tg : am.tagsFor(a)) blob += QLatin1Char(' ') + tg;
        }
        blob = blob.toLower();
        m_texBlob.insert(sno, blob);   // cache once the index is ready
    }
    return blob;
}

void TexturesTab::applyNameFilter()
{
    // Unified predicate: search terms (name / #tag / title / collection, "-" excludes; a pure-digit
    // term matches the SNO) + funnel tag checkboxes (AND/OR, via the appearance-derived blob) +
    // selected formats (match ANY) + orphan toggle.
    QStringList terms, excl, snoTerms;
    for (const QString& part : m_search->text().split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        QString t = part.trimmed().toLower();
        bool neg = false;
        if (t.startsWith(QLatin1Char('-'))) { neg = true; t = t.mid(1); }
        const bool tag = t.startsWith(QLatin1Char('#'));
        if (tag) t = t.mid(1);
        if (t.isEmpty()) continue;
        // A bare all-digits term (not a #tag) filters by SNO.
        bool allDigits = !tag && !t.isEmpty();
        for (const QChar c : t) if (!c.isDigit()) { allDigits = false; break; }
        if (allDigits && !neg) snoTerms << t;
        else (neg ? excl : terms) << t;
    }
    const bool orphan = m_orphanCheck && m_orphanCheck->isChecked();
    const QSet<QString> tagSel = m_tagFilter;
    const QSet<QString> catSel = m_catFilter;
    const bool tagOr = m_tagOrMode;
    const QSet<QString> fmtSel = m_fmtFilter;

    if (terms.isEmpty() && excl.isEmpty() && snoTerms.isEmpty() && !orphan
        && tagSel.isEmpty() && catSel.isEmpty() && fmtSel.isEmpty()) {
        m_model->setPredicate(nullptr);
        updateSelLabel();
        rebuildFilterChips();
        saveTexFilterState();
        return;
    }
    m_model->setPredicate([this, terms, excl, snoTerms, orphan, tagSel, catSel, tagOr, fmtSel](const SnoEntry& e) {
        if (!snoTerms.isEmpty()) {
            const QString s = QString::number(e.snoId);
            for (const QString& d : snoTerms) if (!s.contains(d)) return false;
        }
        if (!catSel.isEmpty()) {   // category is a UNION (match ANY selected category)
            bool any = catSel.contains(QStringLiteral("Latest")) && m_index && m_index->isNew(e.snoId);
            if (!any) {
                const QString nl = e.name.toLower();
                for (const QString& c : catSel) {
                    if (c == QLatin1String("Latest")) continue;   // SNO-based, handled above
                    if (texInCategory(nl, c)) { any = true; break; }
                }
            }
            if (!any) return false;
        }
        QString blob;   // built lazily (name + appearance tags/title/collection), lowercased
        auto haveBlob = [&]() -> const QString& {
            if (blob.isEmpty()) blob = e.name.toLower() + QLatin1Char(' ') + texBlob(e.snoId);
            return blob;
        };
        if (!terms.isEmpty() || !excl.isEmpty()) {
            const QString& b = haveBlob();
            for (const QString& t : terms) if (!b.contains(t)) return false;
            for (const QString& x : excl)  if (b.contains(x))  return false;
        }
        if (!tagSel.isEmpty()) {
            const QString& b = haveBlob();
            if (tagOr) {
                bool any = false;
                for (const QString& t : tagSel) if (b.contains(t, Qt::CaseInsensitive)) { any = true; break; }
                if (!any) return false;
            } else {
                for (const QString& t : tagSel) if (!b.contains(t, Qt::CaseInsensitive)) return false;
            }
        }
        if (orphan) {
            AssetLinks& l = AssetLinks::instance();
            if (l.ready() && !l.linksForTexture(e.snoId).isEmpty()) return false;  // has a model/material
        }
        if (!fmtSel.isEmpty() && m_fmtReady) {
            const auto it = m_texFmt.constFind(e.snoId);
            if (it == m_texFmt.constEnd() || !fmtSel.contains(fmtFamily(it.value()))) return false;
        }
        return true;
    });
    updateSelLabel();
    rebuildFilterChips();
    saveTexFilterState();
}

// ── "Remember filters" — persist the whole filter STATE (search text, category / format / gear-tag
// selections, the data toggles, AND/OR mode and sort) and restore it on next launch. No-op unless
// the toggle is on. Saved on every filter change (a few cheap QSettings writes).
void TexturesTab::saveTexFilterState()
{
    if (!m_rememberFilters || !m_rememberFilters->isChecked()) return;
    QSettings s;
    s.setValue(QStringLiteral("tex/lastSearch"), m_search ? m_search->text() : QString());
    auto setOf = [&](const char* key, const QSet<QString>& set) {
        QStringList l(set.begin(), set.end()); l.sort();
        s.setValue(QLatin1String(key), l);
    };
    setOf("tex/lastCats", m_catFilter);
    setOf("tex/lastFmts", m_fmtFilter);
    setOf("tex/lastTags", m_tagFilter);
    s.setValue(QStringLiteral("tex/lastOrphan"),   m_orphanCheck && m_orphanCheck->isChecked());
    s.setValue(QStringLiteral("tex/lastOnlyDec"),  m_onlyDecrypted && m_onlyDecrypted->isChecked());
    s.setValue(QStringLiteral("tex/lastTagOr"),    m_tagOrMode);
    s.setValue(QStringLiteral("tex/lastSort"),     m_sortCombo ? m_sortCombo->currentData().toString() : QString());
}

void TexturesTab::restoreTexFilterState()
{
    QSettings s;
    if (!s.value(QStringLiteral("tex/rememberFilters"), false).toBool()) return;
    const auto toSet = [](const QStringList& l) { return QSet<QString>(l.begin(), l.end()); };
    m_catFilter = toSet(s.value(QStringLiteral("tex/lastCats")).toStringList());
    m_fmtFilter = toSet(s.value(QStringLiteral("tex/lastFmts")).toStringList());
    m_tagFilter = toSet(s.value(QStringLiteral("tex/lastTags")).toStringList());
    // Tick the checkboxes to match the restored sets (block signals so we apply once at the end).
    for (auto it = m_catChecks.constBegin(); it != m_catChecks.constEnd(); ++it) {
        QSignalBlocker b(it.value()); it.value()->setChecked(m_catFilter.contains(it.key()));
    }
    for (auto it = m_fmtChecks.constBegin(); it != m_fmtChecks.constEnd(); ++it) {
        QSignalBlocker b(it.value()); it.value()->setChecked(m_fmtFilter.contains(it.key()));
    }
    for (auto it = m_tagChecks.constBegin(); it != m_tagChecks.constEnd(); ++it) {
        QSignalBlocker b(it.value()); it.value()->setChecked(m_tagFilter.contains(it.key()));
    }
    if (m_orphanCheck)   { QSignalBlocker b(m_orphanCheck);   m_orphanCheck->setChecked(s.value(QStringLiteral("tex/lastOrphan"), false).toBool()); }
    if (m_onlyDecrypted) { QSignalBlocker b(m_onlyDecrypted); m_onlyDecrypted->setChecked(s.value(QStringLiteral("tex/lastOnlyDec"), false).toBool()); }
    m_tagOrMode = s.value(QStringLiteral("tex/lastTagOr"), false).toBool();
    if (m_tagOrChk) { QSignalBlocker b(m_tagOrChk); m_tagOrChk->setChecked(m_tagOrMode); }
    if (m_sortCombo) {
        const int i = m_sortCombo->findData(s.value(QStringLiteral("tex/lastSort")).toString());
        if (i >= 0) { QSignalBlocker b(m_sortCombo); m_sortCombo->setCurrentIndex(i); }
    }
    if (m_search) { QSignalBlocker b(m_search); m_search->setText(s.value(QStringLiteral("tex/lastSearch")).toString()); }
    if (!m_fmtFilter.isEmpty()) ensureFmtIndex();
    if (m_onlyDecrypted && m_onlyDecrypted->isChecked()) loadList();   // re-filter the source rows
    applySort();
    applyNameFilter();
    updateFunnelTint();
}

void TexturesTab::applySort()
{
    const QString mode = m_sortCombo ? m_sortCombo->currentData().toString() : QStringLiteral("name");
    if (mode == QLatin1String("size")) {
        m_model->setSortValue([this](const SnoEntry& e) -> double {
            return m_reader && m_reader->isReady() ? double(m_reader->payloadSize(quint64(e.snoId))) : 0.0;
        });
        m_model->sort(0, Qt::DescendingOrder);
    } else if (mode == QLatin1String("dim")) {
        m_model->setSortValue([this](const SnoEntry& e) -> double {
            const quint32 d = m_texDim.value(e.snoId, 0);
            return double((d >> 16) * (d & 0xFFFF));
        });
        m_model->sort(0, Qt::DescendingOrder);
    } else {
        m_model->setSortValue(nullptr);
        // SNO column is 0; FILENAME (name) is column 2 in the models layout.
        m_model->sort(mode == QLatin1String("sno") ? 0 : 2, Qt::AscendingOrder);
    }
    updateSelLabel();
}

// Background-scan every group-44 texture's tex.json for format + dimensions, cached
// to disk. Until ready, the format filter / dimension sort simply pass through.
void TexturesTab::ensureFmtIndex()
{
    if (m_fmtReady || m_fmtBuilding || !m_index) return;
    m_fmtBuilding = true;
    const QString cacheBase = AppPaths::dataDir();
    const QString cachePath = cacheBase + QStringLiteral("/tex_info_v1.bin");
    constexpr quint32 kMagic = 0x7E410001u;

    // Try the disk cache first.
    if (QFile::exists(cachePath)) {
        QFile f(cachePath);
        if (f.open(QIODevice::ReadOnly)) {
            QDataStream ds(&f);
            quint32 magic = 0; ds >> magic;
            if (magic == kMagic) {
                QHash<int, int> fmt; QHash<int, quint32> dim;
                ds >> fmt >> dim;
                if (ds.status() == QDataStream::Ok && !fmt.isEmpty()) {
                    m_texFmt = std::move(fmt); m_texDim = std::move(dim);
                    m_fmtReady = true; m_fmtBuilding = false;
                    applyNameFilter(); applySort();
                    return;
                }
            }
        }
    }

    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty()) { m_fmtBuilding = false; return; }
    QVector<QPair<int, QString>> items;
    for (const SnoEntry& e : m_index->entries(kGroupTextureId())) items.append({e.snoId, e.name});

    std::thread([this, items, d4, cacheBase, cachePath, kMagic]() {
        static const QRegularExpression rxF(QStringLiteral("\"eTexFormat\":\\s*(\\d+)"));
        static const QRegularExpression rxW(QStringLiteral("\"dwWidth\":\\s*(\\d+)"));
        static const QRegularExpression rxH(QStringLiteral("\"dwHeight\":\\s*(\\d+)"));
        QHash<int, int> fmt; QHash<int, quint32> dim;
        fmt.reserve(items.size()); dim.reserve(items.size());
        for (const auto& it : items) {
            QFile f(QStringLiteral("%1/json/base/meta/Texture/%2.tex.json").arg(d4, it.second));
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QString raw = QString::fromUtf8(f.readAll());
            const auto mf = rxF.match(raw);
            if (mf.hasMatch()) fmt.insert(it.first, mf.captured(1).toInt());
            const auto mw = rxW.match(raw); const auto mh = rxH.match(raw);
            if (mw.hasMatch() && mh.hasMatch())
                dim.insert(it.first, (quint32(mw.captured(1).toInt()) << 16)
                                     | quint32(mh.captured(1).toInt() & 0xFFFF));
        }
        QDir().mkpath(cacheBase);
        QFile out(cachePath);
        if (out.open(QIODevice::WriteOnly)) {
            QDataStream ds(&out); ds << kMagic << fmt << dim; out.flush();
        }
        QMetaObject::invokeMethod(this, [this, fmt, dim]() mutable {
            m_texFmt = std::move(fmt); m_texDim = std::move(dim);
            m_fmtReady = true; m_fmtBuilding = false;
            applyNameFilter(); applySort();
        }, Qt::QueuedConnection);
    }).detach();
}

void TexturesTab::updateSelLabel()
{
    if (!m_selLabel || !m_view) return;
    const int sel = m_view->selectionModel() ? m_view->selectionModel()->selectedRows().size() : 0;
    m_selLabel->setText(QStringLiteral("Selected: %1/%2").arg(sel).arg(m_model->rowCount()));
}

void TexturesTab::refresh()
{
    if (m_loaded || m_index == nullptr || !m_index->isLoaded())
        return;
    QElapsedTimer rt; rt.start();
    loadList();
    const qint64 tLoad = rt.elapsed();
    applySort();
    qInfo("startup: textures refresh — loadList %lld ms · sort %lld ms", tLoad, rt.elapsed() - tLoad);
    m_loaded = true;
    restoreTexFilterState();   // "Remember filters": re-apply the saved filter state, if enabled
    // Reverse-link index (texture ← material ← appearance) for ASSOCIATED MODELS.
    connect(&AssetLinks::instance(), &AssetLinks::readyChanged, this, [this] {
        m_texBlob.clear();
        if (m_assocTitle) m_assocTitle->setStyleSheet(QString());
        if (m_currentSno >= 0) populateAssociated(m_currentSno);
        applyNameFilter();   // tag terms (#barbarian …) resolve once the index is built
    });
    connect(&AssetLinks::instance(), &AssetLinks::progress, this, [this](int p) {
        if (m_assocTitle && !AssetLinks::instance().ready()) {
            m_assocTitle->setStyleSheet(QStringLiteral("color:#d8a23a;"));   // amber, like Models
            m_assocTitle->setText(QStringLiteral("ASSOCIATED MODELS  ⟳ building index %1%").arg(p));
        }
    });
    AssetLinks::instance().ensureBuilt(Config::d4dataDir());
    AppearanceMeta::instance().ensureBuilt(Config::d4dataDir(), m_index, m_reader);

    // Session memory: reselect the last-viewed texture, if it's still in the list.
    const int lastSno = QSettings().value(QStringLiteral("tex/lastSno"), -1).toInt();
    if (lastSno >= 0) {
        for (int r = 0; r < m_model->rowCount(); ++r) {
            const SnoEntry* e = m_model->entryAt(r);
            if (e && e->snoId == lastSno) {
                const QModelIndex idx = m_model->index(r, 0);
                m_view->setCurrentIndex(idx);
                m_view->scrollTo(idx, QAbstractItemView::PositionAtCenter);
                break;
            }
        }
    }
}

// Cross-tab jump target (Ctrl+K / nav history): select the row for `sno`, which loads the preview
// via the selection-changed wiring. No-op when the sno isn't in the (possibly filtered) list.
void TexturesTab::selectBySno(int sno)
{
    if (!m_model || !m_view || sno <= 0) return;
    for (int r = 0; r < m_model->rowCount(); ++r) {
        const SnoEntry* e = m_model->entryAt(r);
        if (e && e->snoId == sno) {
            const QModelIndex idx = m_model->index(r, 0);
            m_view->setCurrentIndex(idx);
            m_view->scrollTo(idx, QAbstractItemView::PositionAtCenter);
            return;
        }
    }
}

void TexturesTab::reset()
{
    m_loaded = false;
    m_model->setEntries({});
    m_currentName.clear();
    m_currentSno = -1;
    m_frameDefs.clear();
    m_fullImage = QImage();
    if (m_framesModel) m_framesModel->setRowCount(0);
    if (m_assocModel) m_assocModel->setRowCount(0);
    if (m_preview) m_preview->clearTexture();
    for (auto it = m_infoVals.begin(); it != m_infoVals.end(); ++it) it.value()->setText(QStringLiteral("—"));
    updateSelLabel();
}

void TexturesTab::loadList()
{
    QVector<SnoEntry> entries = m_index->entries(kGroupTextureId());
    if (m_onlyDecrypted && m_onlyDecrypted->isChecked() && m_reader && m_reader->isReady()) {
        QVector<SnoEntry> kept;
        kept.reserve(entries.size());
        for (const SnoEntry& e : entries)
            if (m_reader->payloadSize(quint64(e.snoId)) > 0) kept.append(e);
        entries = kept;
    }
    m_model->setEntries(entries);
    updateSelLabel();
}

// The Textures "Options…" button now opens the shared Export settings dialog (via the
// exportSettingsRequested signal → MainWindow). Re-sync the TexFrames trim toggle when those
// settings change so the tab UI reflects the shared tex/trim value.
void TexturesTab::onSettingsChanged()
{
    if (m_trimCheck) {
        m_trimCheck->blockSignals(true);
        m_trimCheck->setChecked(QSettings().value(QStringLiteral("tex/trim"), false).toBool());
        m_trimCheck->blockSignals(false);
    }
}

void TexturesTab::setInfo(const QString& key, const QString& value)
{
    if (QLabel* l = m_infoVals.value(key, nullptr))
        l->setText(value.isEmpty() ? QStringLiteral("—") : value);
}

void TexturesTab::onSelectionChanged()
{
    const QModelIndex cur = m_view->currentIndex();
    if (!cur.isValid()) return;
    if (const SnoEntry* e = m_model->entryAt(cur.row()))
        showTexture(e->snoId, e->name);
}

void TexturesTab::onPreviewHover(QPointF uv)
{
    if (!m_pixelLabel) return;
    if (uv.x() < 0 || m_fullImage.isNull()) { m_pixelLabel->setText(QStringLiteral(" ")); return; }
    const int x = qBound(0, int(uv.x() * m_fullImage.width()),  m_fullImage.width()  - 1);
    const int y = qBound(0, int(uv.y() * m_fullImage.height()), m_fullImage.height() - 1);
    const QColor c = m_fullImage.pixelColor(x, y);
    m_pixelLabel->setText(QStringLiteral("(%1, %2)   RGBA  %3  %4  %5  %6   %7")
        .arg(x, 4).arg(y, 4).arg(c.red(), 3).arg(c.green(), 3).arg(c.blue(), 3).arg(c.alpha(), 3)
        .arg(c.name(QColor::HexArgb)));
}

// Start a drag carrying an image: a temp PNG file URL (for Explorer) + raw image data.
void TexturesTab::startImageDrag(const QImage& img, const QString& baseName)
{
    if (img.isNull()) return;
    QString safe = baseName.isEmpty() ? QStringLiteral("texture") : baseName;
    safe.replace(QRegularExpression(QStringLiteral("[^\\w.-]")), QStringLiteral("_"));
    const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                             .filePath(safe + QStringLiteral(".png"));
    if (!img.save(path)) return;
    auto* mime = new QMimeData;
    mime->setImageData(img);
    mime->setUrls({QUrl::fromLocalFile(path)});
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->setPixmap(QPixmap::fromImage(img).scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    drag->exec(Qt::CopyAction);
}

void TexturesTab::setChannel(int mode)
{
    m_channel = mode;
    for (int i = 0; i < 5; ++i) if (m_chanBtns[i]) m_chanBtns[i]->setChecked(i == mode);
    if (mode == 0) { m_texStack->setCurrentIndex(0); return; }
    if (m_fullImage.isNull()) m_fullImage = m_preview->grabImage();
    const QImage g = channelGrey(m_fullImage, mode - 1);
    if (!g.isNull())
        m_chanLabel->setPixmap(QPixmap::fromImage(g).scaled(
            m_chanLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_texStack->setCurrentIndex(1);
}

void TexturesTab::showTexture(int sno, const QString& name)
{
    m_currentName = name;
    m_currentSno = sno;
    QSettings().setValue(QStringLiteral("tex/lastSno"), sno);   // session memory
    m_selFrame = -1;
    m_frameDefs.clear();
    m_fullImage = QImage();
    if (m_framesModel) m_framesModel->setRowCount(0);
    if (m_exportFrame) m_exportFrame->setEnabled(false);

    const QString d4 = Config::d4dataDir();
    TexMeta meta;
    if (!d4.isEmpty()) {
        QFile f(QStringLiteral("%1/json/base/meta/Texture/%2.tex.json").arg(d4, name));
        if (f.open(QIODevice::ReadOnly)) meta = parseTexMetaJson(f.readAll());
    }
    // Dimension override: the CASC has only headerless pixels, so when the d4data JSON is stale
    // (wrong dims → scrambled) or missing (no descriptor), substitute the real pixel dimensions.
    {
        int ow = 0, oh = 0;
        if (textureDimOverride(name, ow, oh) && (ow != meta.width || oh != meta.height)) {
            const bool wasValid = meta.valid;
            meta.width = ow; meta.height = oh;
            if (!wasValid) { meta.eTexFormat = 49; meta.faceCount = 1; meta.mipMax = 0; }
            meta.valid = true;
            logTex(QStringLiteral("  dims: override → %1×%2 (JSON was %3)")
                   .arg(ow).arg(oh).arg(wasValid ? QStringLiteral("stale") : QStringLiteral("missing")));
        }
    }
    m_curMeta = meta;

    logTex(QStringLiteral("── load %1 [sno %2] ──").arg(name).arg(sno));
    if (!meta.valid)
        logTex(QStringLiteral("  meta: INVALID (no %1/json/base/meta/Texture/%2.tex.json)").arg(d4, name), true);
    else
        logTex(QStringLiteral("  meta: fmt=%1 %2 · %3×%4 · faces=%5 · mipMax=%6")
               .arg(meta.eTexFormat).arg(TexFormat::name(meta.eTexFormat))
               .arg(meta.width).arg(meta.height).arg(meta.faceCount).arg(meta.mipMax));

    QByteArray payload;
    if (!m_reader)
        logTex(QStringLiteral("  CASC: no reader (game folder not set)"), true);
    else if (!m_reader->isReady())
        logTex(QStringLiteral("  CASC: reader NOT ready (%1)").arg(m_reader->lastError()), true);
    else {
        payload = m_reader->readPayloadBySno(quint64(sno));
        if (payload.isEmpty())
            logTex(QStringLiteral("  CASC: readPayloadBySno(%1) returned EMPTY — sno not in this build's storage").arg(sno), true);
        else
            logTex(QStringLiteral("  CASC: payload = %1 bytes").arg(payload.size()));
    }
    // Schema probe: find where the real width/height/format live. This was the reverse-engineering
    // scaffold — it calls CascReader::enumerate() TWICE, each a full linear scan of the ENTIRE TVFS
    // (every asset in the game, under a mutex), plus extra CASC reads and ~15 log lines. Running it
    // on every selection made scrolling the texture list crawl. It's now OFF by default and only
    // runs when D4_TEX_PROBE is set, so browsing decodes-and-shows instantly (like d4analyzer).
    static const bool kTexProbe = qEnvironmentVariableIsSet("D4_TEX_PROBE");
    if (kTexProbe && m_reader && m_reader->isReady()) {
        const QByteArray mb = m_reader->readMetaBySno(quint64(sno));
        auto rdU32 = [](const QByteArray& b, int i) -> quint32 {
            return quint32(quint8(b[i])) | (quint32(quint8(b[i+1])) << 8)
                 | (quint32(quint8(b[i+2])) << 16) | (quint32(quint8(b[i+3])) << 24);
        };
        auto hex = [](const QByteArray& b, int n) {
            return QString::fromLatin1(b.left(n).toHex(' '));
        };
        auto findWH = [&](const QByteArray& b) -> int {   // offset of dwWidth (width then height u32)
            if (!(meta.valid && meta.width > 0 && meta.height > 0)) return -1;
            for (int i = 0; i + 8 <= b.size(); ++i)
                if (rdU32(b, i) == quint32(meta.width) && rdU32(b, i+4) == quint32(meta.height)) return i;
            return -1;
        };
        // One-time survey of the CASC path scheme — where do definitions live vs payloads?
        static bool surveyed = false;
        if (!surveyed) {
            surveyed = true;
            int total = 0, metaN = 0, payN = 0, paylowN = 0, otherN = 0;
            QStringList metaSamp, otherSamp;
            m_reader->enumerate(QString(), [&](const CascReader::Entry& e) {
                ++total;
                if (e.name.startsWith(QLatin1String("base/meta/")))          { if (++metaN <= 8) metaSamp << e.name; }
                else if (e.name.startsWith(QLatin1String("base/payload/")))   ++payN;
                else if (e.name.startsWith(QLatin1String("base/paylow/")))    ++paylowN;
                else { if (++otherN <= 8) otherSamp << e.name; }
                return true;
            });
            logTex(QStringLiteral("CASC SCHEME: total=%1 · base/meta=%2 · base/payload=%3 · base/paylow=%4 · other=%5")
                   .arg(total).arg(metaN).arg(payN).arg(paylowN).arg(otherN));
            logTex(QStringLiteral("  base/meta samples: %1").arg(metaSamp.join(QLatin1Char(' '))));
            logTex(QStringLiteral("  other samples: %1").arg(otherSamp.join(QLatin1Char(' '))));
        }
        logTex(QStringLiteral("  metaBin=%1B  payloadHead=[%2]")
               .arg(mb.size()).arg(hex(payload, 24)));
        // Every CASC path that mentions this sno — reveals where (if anywhere) the descriptor lives.
        QStringList paths;
        m_reader->enumerate(QString::number(sno), [&](const CascReader::Entry& e) {
            if (e.name.contains(QString::number(sno)))
                paths << QStringLiteral("%1(%2B)").arg(e.name).arg(e.size);
            return paths.size() < 16;
        });
        logTex(QStringLiteral("  CASC paths(%1): %2").arg(paths.size()).arg(paths.join(QLatin1Char(' '))));
        // The real descriptor lives at the NAME-based path base/meta/Texture/<name>.tex (rustydemon
        // mirror), not numeric base/meta/<sno>. Read it and, for a texture whose JSON still matches,
        // locate dwWidth/dwHeight so we can map the flat-struct field offsets and parse it directly.
        const QByteArray texMeta = m_reader->readFile(QStringLiteral("base/meta/Texture/%1.tex").arg(name));
        logTex(QStringLiteral("  texMeta[base/meta/Texture/%1.tex]=%2B").arg(name).arg(texMeta.size()));
        logTex(QStringLiteral("    head128=[%1]").arg(hex(texMeta, 128)));
        // Does ANY name-based path exist in the parsed TVFS? Enumerate by a distinctive name token.
        {
            const QString frag = name.section(QLatin1Char('_'), -2).toLower();   // e.g. "torso_generic"
            QStringList np;
            m_reader->enumerate(frag, [&](const CascReader::Entry& e) {
                np << QStringLiteral("%1(%2B)").arg(e.name).arg(e.size);
                return np.size() < 12;
            });
            logTex(QStringLiteral("  name-paths for '%1'(%2): %3").arg(frag).arg(np.size()).arg(np.join(QLatin1Char(' '))));
        }
        // Dump every u32 in the first 96 bytes as offset:value so the flat-struct fields
        // (eTexFormat, dwWidth, dwHeight, dwFaceCount, mip levels) can be located directly.
        {
            QStringList u;
            for (int i = 0; i + 4 <= texMeta.size() && i < 96; i += 4)
                u << QStringLiteral("%1:%2").arg(i).arg(rdU32(texMeta, i));
            logTex(QStringLiteral("    u32s: %1").arg(u.join(QLatin1Char(' '))));
        }
        for (const auto& pr : { qMakePair(QStringLiteral("meta"), mb), qMakePair(QStringLiteral("payload"), payload) }) {
            const int w = findWH(pr.second);
            if (w >= 0) {
                QStringList win;
                for (int i = qMax(0, w - 20); i + 4 <= pr.second.size() && i <= w + 28; i += 4)
                    win << QStringLiteral("%1:%2").arg(i - w).arg(rdU32(pr.second, i));
                logTex(QStringLiteral("  %1: dwWidth@%2 · window(off:val): %3").arg(pr.first).arg(w).arg(win.join(QLatin1Char(' '))));
            }
        }
    }
    m_curPayload = payload;
    m_curFace = 0;

    setInfo(QStringLiteral("Filename"), QStringLiteral("%1 [%2]").arg(name).arg(sno));
    if (!meta.valid) {
        setInfo(QStringLiteral("Format"), QStringLiteral("(descriptor unavailable — set d4data)"));
        m_preview->clearTexture();
        return;
    }

    // Cubemap / array face selector: shown only when there's more than one face.
    if (m_faceCombo) {
        QSignalBlocker fb(m_faceCombo);
        m_faceCombo->clear();
        const int faces = qMax(1, meta.faceCount);
        for (int i = 0; i < faces; ++i) m_faceCombo->addItem(QStringLiteral("Face %1").arg(i));
        m_faceCombo->setVisible(faces > 1);
    }
    const int alignW = (meta.width + 3) & ~3;
    setInfo(QStringLiteral("Filesize"),
            payload.isEmpty() ? QStringLiteral("(not in CASC)") : humanSize(payload.size()));
    setInfo(QStringLiteral("Format"), QStringLiteral("%1 (%2)  (align: %3)")
            .arg(TexFormat::name(meta.eTexFormat)).arg(meta.eTexFormat).arg(alignW));
    setInfo(QStringLiteral("Size (meta)"), QStringLiteral("%1×%2  (aligned width: %3)")
            .arg(meta.width).arg(meta.height).arg(alignW));
    // Some maps (e.g. *_Metal, *_FurMask) ship only a 4×4 mip-tail: the game authored them as a
    // single flat value (constant metalness / fur coverage), so they decode to a uniform colour and
    // just *look* empty. Flag that so it doesn't read as a load failure.
    QString dispSize = QStringLiteral("%1×%2").arg(meta.width).arg(meta.height);
    if (meta.width <= 8 && meta.height <= 8) {
        dispSize += QStringLiteral("  — tiny authored mask (a flat colour by design, not a load error)");
        logTex(QStringLiteral("  note: %1×%2 mip-tail — this map is a constant flat value")
               .arg(meta.width).arg(meta.height));
    }
    setInfo(QStringLiteral("Size (displayed)"), dispSize);
    setInfo(QStringLiteral("Face count"), QString::number(meta.faceCount));

    if (payload.isEmpty()) {
        m_preview->clearTexture();
    } else {
        m_channel = 0;
        uploadFaceMip();   // uploads face 0 / mip 0 + refreshes m_fullImage + channel view
    }

    // TEXFRAMES table. Preferred source is d4data's ptFrame (meta.frames). When that's
    // missing/stale (seasonal e001/e002 atlases, or after a patch the snapshot lags), fall
    // back — in order — to (a) user-exported d4analyzer frame PNGs, then (b) the game's own
    // global frame table (base/Misc/2D_table.dat) for the authoritative frame count/handles
    // combined with alpha-gutter segmentation of the decoded atlas for the rectangles. (b)
    // needs no d4data and covers every atlas the game ships.
    if (meta.frames.isEmpty()) {
        const int nOv = frameOverrideCount(sno);
        if (nOv > 0) {
            for (int i = 0; i < nOv; ++i) meta.frames.append(TexFrame{});
            logTex(QStringLiteral("  frames: %1 from exported d4analyzer icons").arg(nOv));
        } else if (!m_fullImage.isNull()) {
            FrameTable::instance().ensureLoaded(m_reader);
            const QVector<quint32> handles = FrameTable::instance().handles(quint32(sno));
            if (!handles.isEmpty()) {
                const QVector<QRect> cells = segmentAtlasFrames(m_fullImage);
                const int W = m_fullImage.width(), H = m_fullImage.height();
                const bool pairable = (cells.size() == handles.size());
                for (int i = 0; i < cells.size(); ++i) {
                    const QRect r = cells[i];
                    TexFrame f;
                    f.u0 = float(r.x()) / W;          f.v0 = float(r.y()) / H;
                    f.u1 = float(r.x() + r.width()) / W;
                    f.v1 = float(r.y() + r.height()) / H;
                    // Handle pairing is best-effort: 2D_table's authored order isn't the atlas's
                    // spatial order, so we only attach handles when the counts line up, and even
                    // then the pairing is approximate. The rectangles/images are exact.
                    f.handle = (pairable && i < handles.size()) ? quint64(handles[i]) : 0;
                    meta.frames.append(f);
                }
                logTex(QStringLiteral("  frames: %1 via 2D_table(%2)+segmentation [%3, handles %4]")
                       .arg(cells.size()).arg(handles.size())
                       .arg(FrameTable::instance().source(),
                            pairable ? QStringLiteral("approx-paired") : QStringLiteral("unpaired")));
            } else {
                logTex(QStringLiteral("  frames: none (sno %1 not in 2D_table; no d4data ptFrame)").arg(sno));
            }
        }
    }
    m_frameDefs = meta.frames;
    for (int i = 0; i < meta.frames.size(); ++i) {
        const TexFrame& fr = meta.frames[i];
        const int fw = qRound((fr.u1 - fr.u0) * meta.width);
        const int fh = qRound((fr.v1 - fr.v0) * meta.height);
        QList<QStandardItem*> row{
            new QStandardItem(QString::number(i)),
            new QStandardItem(QStringLiteral("%1").arg(quint32(fr.handle), 0, 16)),
            new QStandardItem(QString::number(fr.handle)),
            new QStandardItem(AppearanceMeta::instance().nameForIconHandle(quint32(fr.handle))),
            new QStandardItem(QStringLiteral("%1×%2").arg(fw).arg(fh))};
        m_framesModel->appendRow(row);
    }
    if (m_tfTitle) m_tfTitle->setText(QStringLiteral("TEXFRAMES (%1)").arg(meta.frames.size()));

    populateAssociated(sno);   // fills m_snoName (used for the PBR material names)
    refreshChannelCombo(sno);
    populateChannelView();
    if (m_previewTabs && m_previewTabs->currentIndex() == 1) buildGallery();
}

void TexturesTab::buildGallery()
{
    if (!m_galleryScroll) return;
    m_galleryWidth = m_galleryScroll->viewport()->width();   // remember (loop guard)
    const int avail = qMax(220, m_galleryWidth - 12);

    // Fresh content widget each rebuild (setWidget deletes the previous one + its cells).
    auto* content = new QWidget;
    auto* vbox = new QVBoxLayout(content);
    vbox->setContentsMargins(6, 6, 6, 6);
    vbox->setSpacing(8);
    vbox->setAlignment(Qt::AlignTop);

    if (m_frameDefs.isEmpty()) { m_galleryScroll->setWidget(content); return; }
    if (m_fullImage.isNull()) m_fullImage = m_preview->grabImage();

    QSet<int> selRows;   // highlight whatever the TexFrames table has selected
    if (m_frames->selectionModel())
        for (const QModelIndex& idx : m_frames->selectionModel()->selectedRows())
            selRows.insert(idx.row());

    // d4analyzer style: each frame is sized to its REAL resolution (so a 1600×600
    // banner shows large and 150×228 icons stay small), capped to fit, flowed into
    // rows that wrap at the viewport width.
    const int maxDim = qMin(avail, 600);
    QHBoxLayout* row = nullptr;
    int rowW = 0;
    auto newRow = [&] {
        row = new QHBoxLayout(); row->setSpacing(8); row->setAlignment(Qt::AlignLeft);
        vbox->addLayout(row); rowW = 0;
    };
    newRow();
    for (int i = 0; i < m_frameDefs.size(); ++i) {
        const QImage f = croppedFrame(i, false);
        int dw = f.isNull() ? 120 : f.width();
        int dh = f.isNull() ? 120 : f.height();
        double s = 1.0;
        if (dw > maxDim) s = double(maxDim) / dw;
        if (dh * s > maxDim) s = double(maxDim) / dh;
        dw = qMax(48, int(dw * s));
        dh = qMax(48, int(dh * s));
        if (rowW > 0 && rowW + dw + 8 > avail) newRow();

        auto* cell = new QWidget;
        cell->setProperty("frameIdx", i);
        cell->setCursor(Qt::PointingHandCursor);
        cell->setMouseTracking(true);   // move events for drag-to-export
        cell->installEventFilter(this);
        auto* cl = new QVBoxLayout(cell);
        cl->setContentsMargins(2, 2, 2, 2); cl->setSpacing(1);
        auto* img = new QLabel;
        img->setFixedSize(dw, dh);
        img->setAlignment(Qt::AlignCenter);
        img->setStyleSheet(selRows.contains(i)
            ? QStringLiteral("background:#1b1b1b;border:2px solid #5a93c9;")
            : QStringLiteral("background:transparent;border:0;"));
        if (!f.isNull())
            img->setPixmap(QPixmap::fromImage(f).scaled(dw, dh, Qt::KeepAspectRatio,
                                                        Qt::SmoothTransformation));
        cl->addWidget(img);
        const quint32 h = quint32(m_frameDefs[i].handle);
        const QString nm = AppearanceMeta::instance().nameForIconHandle(h);
        const QString label = nm.isEmpty() ? QStringLiteral("%1").arg(h, 0, 16) : nm;
        auto* cap = new QLabel((f.isNull() ? QStringLiteral("✗  ") : QStringLiteral("✓  ")) + label);
        cap->setAlignment(Qt::AlignCenter);
        cap->setStyleSheet(f.isNull() ? QStringLiteral("color:#c0392b;font-size:9px;")
                                      : QStringLiteral("color:#5fae5f;font-size:9px;"));
        cl->addWidget(cap, 0, Qt::AlignHCenter);
        cell->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        row->addWidget(cell, 0, Qt::AlignTop);
        rowW += dw + 8;
    }
    if (row) row->addStretch(1);
    m_galleryScroll->setWidget(content);   // replaces (deletes) the old content
}

void TexturesTab::populateAssociated(int sno)
{
    if (!m_assocModel) return;
    m_assocModel->setRowCount(0);
    AssetLinks& links = AssetLinks::instance();
    if (!links.ready()) {
        m_assocTitle->setStyleSheet(QStringLiteral("color:#d8a23a;"));
        m_assocTitle->setText(QStringLiteral("ASSOCIATED MODELS  ⟳ building index…"));
        return;
    }
    m_assocTitle->setStyleSheet(QString());
    if (m_snoName.isEmpty() && m_index)   // lazy sno → name across all groups
        for (int g : m_index->groups())
            for (const SnoEntry& e : m_index->entries(g))
                m_snoName.insert(e.snoId, e.name);
    auto nm = [this](int s) { return m_snoName.value(s, QStringLiteral("#%1").arg(s)); };
    AppearanceMeta& am = AppearanceMeta::instance();

    auto texChildren = [&](QStandardItem* parent, const QVector<QPair<int, int>>& pairs) {
        for (const auto& tp : pairs) {
            auto* c1 = new QStandardItem(QStringLiteral("%1:  %2")
                           .arg(AssetLinks::slotRole(tp.second), nm(tp.first)));
            auto* c2 = new QStandardItem(QString::number(tp.first));
            c1->setData(tp.first, Qt::UserRole);
            c1->setData(QStringLiteral("tex"), Qt::UserRole + 1);
            if (tp.first == sno) { QFont f = c1->font(); f.setBold(true); c1->setFont(f); }
            parent->appendRow({c1, c2});
        }
    };

    const QVector<AssetLinks::MatLink> rows = links.linksForTexture(sno);
    constexpr int MAT_CAP = 60, APP_CAP = 40, TOTAL_APP_CAP = 300;
    int napps = 0, shownApps = 0;
    QStringList usedBy; QSet<QString> seen, tagSet;
    for (int mi = 0; mi < rows.size() && mi < MAT_CAP; ++mi) {
        const AssetLinks::MatLink& l = rows[mi];
        napps += l.apps.size();
        if (!l.apps.isEmpty() && shownApps < TOTAL_APP_CAP) {
            int shownHere = 0;
            for (int ai = 0; ai < l.apps.size() && ai < APP_CAP && shownApps < TOTAL_APP_CAP; ++ai) {
                const int app = l.apps[ai];
                QString albl = nm(app) + QStringLiteral(".app");
                const QString title = am.titleFor(app), coll = am.collectionFor(app);
                QStringList extra;
                if (!title.isEmpty()) extra << title;
                if (!coll.isEmpty())  extra << coll;
                if (!extra.isEmpty()) albl += QStringLiteral("   —   ") + extra.join(QStringLiteral("  ·  "));
                if (!title.isEmpty() && !seen.contains(title)) { seen.insert(title); usedBy << title; }
                for (const QString& tg : am.tagsFor(app)) tagSet.insert(tg);
                auto* a1 = new QStandardItem(albl);
                auto* a2 = new QStandardItem(QString::number(app));
                a1->setData(app, Qt::UserRole);
                a1->setData(QStringLiteral("app"), Qt::UserRole + 1);
                auto* m1 = new QStandardItem(QStringLiteral("Material:  %1").arg(nm(l.matSno)));
                auto* m2 = new QStandardItem(QString::number(l.matSno));
                m1->setData(l.matSno, Qt::UserRole);
                m1->setData(QStringLiteral("mat"), Qt::UserRole + 1);
                texChildren(m1, l.texPairs);
                a1->appendRow({m1, m2});
                m_assocModel->appendRow({a1, a2});
                ++shownApps; ++shownHere;
            }
            if (l.apps.size() > shownHere)
                m_assocModel->appendRow({new QStandardItem(QStringLiteral("… %1 more models use %2")
                    .arg(l.apps.size() - shownHere).arg(nm(l.matSno))), new QStandardItem(QString())});
        } else if (l.apps.isEmpty()) {
            auto* m1 = new QStandardItem(QStringLiteral("Material:  %1  (not referenced by a model)").arg(nm(l.matSno)));
            auto* m2 = new QStandardItem(QString::number(l.matSno));
            texChildren(m1, l.texPairs);
            m_assocModel->appendRow({m1, m2});
        }
    }
    if (rows.size() > MAT_CAP)
        m_assocModel->appendRow({new QStandardItem(QStringLiteral("… and %1 more materials")
            .arg(rows.size() - MAT_CAP)), new QStandardItem(QString())});

    m_assocTitle->setText(QStringLiteral("ASSOCIATED MODELS (%1 MODELS / %2 MATERIALS)")
                              .arg(napps).arg(rows.size()));
    if (shownApps <= 8) m_assocView->expandAll();

    setInfo(QStringLiteral("Used by"), usedBy.isEmpty() ? QString()
            : QStringList(usedBy.mid(0, 3)).join(QStringLiteral("; "))
              + (usedBy.size() > 3 ? QStringLiteral(" …") : QString()));
    QStringList tags = tagSet.values(); tags.sort();
    setInfo(QStringLiteral("Tags"), tags.join(QStringLiteral(", ")));
}

void TexturesTab::onAssocDoubleClick(const QModelIndex& index)
{
    QStandardItem* it = m_assocModel->itemFromIndex(index.siblingAtColumn(0));
    if (!it) return;
    const int sno = it->data(Qt::UserRole).toInt();
    if (sno <= 0) return;
    const QString kind = it->data(Qt::UserRole + 1).toString();
    if (kind == QLatin1String("app")) {
        emit revealModelRequested(sno);   // → Models tab loads it (MainWindow wires this)
        return;
    }
    // Texture (or material) → jump to that texture in this tab's list.
    for (int r = 0; r < m_model->rowCount(); ++r) {
        const SnoEntry* e = m_model->entryAt(r);
        if (e && e->snoId == sno) {
            const QModelIndex idx = m_model->index(r, 0);
            m_view->setCurrentIndex(idx);
            m_view->scrollTo(idx, QAbstractItemView::PositionAtCenter);
            return;
        }
    }
}

void TexturesTab::showAssocMenu(const QPoint& pos)
{
    const QModelIndex idx = m_assocView->indexAt(pos);
    if (!idx.isValid()) return;
    QStandardItem* it = m_assocModel->itemFromIndex(idx.siblingAtColumn(0));
    if (!it) return;
    const int sno = it->data(Qt::UserRole).toInt();
    const QString kind = it->data(Qt::UserRole + 1).toString();
    const QString label = it->text();

    QMenu menu(this);
    if (kind == QLatin1String("app") && sno > 0) {
        menu.addAction(QStringLiteral("Reveal model in Models tab"), this,
                       [this, sno]() { emit revealModelRequested(sno); });
    } else if (kind == QLatin1String("tex") && sno > 0) {
        menu.addAction(QStringLiteral("Reveal texture in list"), this,
                       [this, idx]() { onAssocDoubleClick(idx); });
    }
    if (sno > 0) {
        menu.addAction(QStringLiteral("Copy name"), this,
                       [label]() { QApplication::clipboard()->setText(label); });
        menu.addAction(QStringLiteral("Copy SNO"), this,
                       [sno]() { QApplication::clipboard()->setText(QString::number(sno)); });
    }
    if (!menu.isEmpty()) menu.exec(m_assocView->viewport()->mapToGlobal(pos));
}

// Slice the cached payload to the current face (top mip) and upload it for preview.
void TexturesTab::uploadFaceMip()
{
    if (!m_preview) return;
    if (m_curPayload.isEmpty() || !m_curMeta.valid) { m_preview->clearTexture(); return; }
    const int bpb = bytesPerBlock(m_curMeta.eTexFormat);
    const int levels = qMax(1, m_curMeta.mipMax);
    auto mipSz = [&](int i) {
        const int mw = qMax(1, m_curMeta.width >> i), mh = qMax(1, m_curMeta.height >> i);
        return TexFormat::mip0Size(TexFormat::alignedWidth(mw, bpb), mh, bpb);
    };
    // Per-face stride = the whole mip chain; we display the top mip of the face.
    qint64 faceStride = 0;
    for (int i = 0; i < levels; ++i) faceStride += mipSz(i);
    m_curFace = qBound(0, m_curFace, qMax(1, m_curMeta.faceCount) - 1);
    const qint64 off = qint64(m_curFace) * faceStride;
    const int mw = m_curMeta.width, mh = m_curMeta.height;
    const qint64 sz = mipSz(0);
    if (off < 0 || off >= m_curPayload.size()) {
        logTex(QStringLiteral("  decode: face offset %1 past payload %2 — abort").arg(off).arg(m_curPayload.size()), true);
        m_preview->clearTexture(); return;
    }
    const QByteArray slice = m_curPayload.mid(int(off), int(qMin(sz, qint64(m_curPayload.size()) - off)));
    // Diagnostics: what the slice size implies about the BC1/BC3 codec choice for this format.
    {
        const TexFormat::Codec c = TexFormat::codec(m_curMeta.eTexFormat, slice.size(), mw, mh);
        const qint64 bc1 = TexFormat::mip0Size(TexFormat::alignedWidth(mw, 8), mh, 8);
        const qint64 bc3 = TexFormat::mip0Size(TexFormat::alignedWidth(mw, 16), mh, 16);
        logTex(QStringLiteral("  decode: slice=%1 · sliceBpb=%2 · codec=%3 (%4bpb) · BC1mip0=%5 BC3mip0=%6")
               .arg(slice.size()).arg(bpb).arg(c.name).arg(c.bytesPerBlock).arg(bc1).arg(bc3));
    }
    m_preview->setTexture(slice, mw, mh, m_curMeta.eTexFormat);
    // Keep the CPU atlas (channel strip + frame gallery) in sync with what's shown.
    m_fullImage = BcDecode::decode(slice, mw, mh, m_curMeta.eTexFormat);
    if (m_fullImage.isNull())
        logTex(QStringLiteral("  decode: BcDecode returned NULL (payload too small for codec, or unsupported format)"), true);
    else
        logTex(QStringLiteral("  decode: OK → %1×%2").arg(m_fullImage.width()).arg(m_fullImage.height()));
    setChannel(m_channel);
    if (m_chanViewCombo) populateChannelView();
}

QImage TexturesTab::croppedFrame(int row, bool trim)
{
    if (row < 0 || row >= m_frameDefs.size()) return {};
    const TexFrame& fr = m_frameDefs[row];
    // Prefer an exported frame icon (d4analyzer TexFrames export, keyed by atlas sno + frame index) —
    // correct even when the atlas's ptFrame UVs in d4data are stale after a re-layout.
    {
        QImage ov = frameIconOverride(m_currentSno, row);
        if (!ov.isNull()) return ov;
    }
    if (m_fullImage.isNull()) m_fullImage = m_preview->grabImage();
    if (m_fullImage.isNull()) return {};
    const int w = m_fullImage.width(), h = m_fullImage.height();
    QRect r(qRound(fr.u0 * w), qRound(fr.v0 * h),
            qRound((fr.u1 - fr.u0) * w), qRound((fr.v1 - fr.v0) * h));
    r = r.intersected(QRect(0, 0, w, h));
    if (r.width() <= 0 || r.height() <= 0) return {};
    QImage out = m_fullImage.copy(r);
    if (trim && out.hasAlphaChannel()) {
        // Tight non-transparent bounds (alpha > 0).
        const QImage a = out.convertToFormat(QImage::Format_RGBA8888);
        int x0 = a.width(), y0 = a.height(), x1 = -1, y1 = -1;
        for (int y = 0; y < a.height(); ++y) {
            const uchar* s = a.constScanLine(y);
            for (int x = 0; x < a.width(); ++x)
                if (s[x * 4 + 3] != 0) { x0 = qMin(x0, x); x1 = qMax(x1, x);
                                         y0 = qMin(y0, y); y1 = qMax(y1, y); }
        }
        if (x1 >= x0 && y1 >= y0)
            out = out.copy(QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1));
    }
    return out;
}

void TexturesTab::onFrameSelected()
{
    const auto* sm = m_frames->selectionModel();
    const bool any = sm && !sm->selectedRows().isEmpty();
    if (m_exportFrame) m_exportFrame->setEnabled(any);
    // Auto-switch to the gallery on first selection; otherwise refresh the highlights.
    if (m_previewTabs && any && m_previewTabs->currentIndex() != 1)
        m_previewTabs->setCurrentIndex(1);   // currentChanged → buildGallery()
    else if (m_previewTabs && m_previewTabs->currentIndex() == 1)
        buildGallery();
}

bool TexturesTab::eventFilter(QObject* obj, QEvent* ev)
{
    // Gallery cell: click → select that frame; hover → highlight tint.
    if (auto* wgt = qobject_cast<QWidget*>(obj); wgt && wgt->property("frameIdx").isValid()) {
        if (ev->type() == QEvent::MouseButtonPress) {
            const int row = wgt->property("frameIdx").toInt();
            auto* sel = m_frames->selectionModel();
            if (sel && row >= 0 && row < m_framesModel->rowCount()) {
                const auto* me = static_cast<QMouseEvent*>(ev);
                const QModelIndex first = m_framesModel->index(row, 0);
                const QModelIndex last  = m_framesModel->index(row, m_framesModel->columnCount() - 1);
                QItemSelectionModel::SelectionFlags fl = QItemSelectionModel::Rows;
                if (me->modifiers() & Qt::ControlModifier)    fl |= QItemSelectionModel::Toggle;
                else if (me->modifiers() & Qt::ShiftModifier) fl |= QItemSelectionModel::Select;
                else                                          fl |= QItemSelectionModel::ClearAndSelect;
                sel->select(QItemSelection(first, last), fl);
                sel->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
            }
            m_dragStartPos = static_cast<QMouseEvent*>(ev)->globalPosition().toPoint();
            return true;
        }
        if (ev->type() == QEvent::MouseMove) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            if ((me->buttons() & Qt::LeftButton)
                && (me->globalPosition().toPoint() - m_dragStartPos).manhattanLength()
                       > QApplication::startDragDistance()) {
                const int row = wgt->property("frameIdx").toInt();
                const QImage f = croppedFrame(row, m_trimCheck && m_trimCheck->isChecked());
                const quint64 h = (row < m_frameDefs.size()) ? m_frameDefs[row].handle : quint64(row);
                startImageDrag(f, QStringLiteral("%1_%2").arg(m_currentName).arg(h));
                return true;
            }
        }
        if (ev->type() == QEvent::MouseButtonDblClick) {
            const int row = wgt->property("frameIdx").toInt();
            const QImage f = croppedFrame(row, false);
            if (!f.isNull()) {
                // Click-to-zoom: full-size popup (capped so it stays on screen).
                const int cap = qMax(256, m_previewPx * 2);
                popupPreview(QPixmap::fromImage(f).scaled(qMin(f.width(), 1400), qMin(f.height(), cap),
                                                          Qt::KeepAspectRatio, Qt::SmoothTransformation));
            }
            return true;
        }
        if (ev->type() == QEvent::ContextMenu) {
            const int row = wgt->property("frameIdx").toInt();
            const QImage f = croppedFrame(row, m_trimCheck && m_trimCheck->isChecked());
            if (!f.isNull()) {
                QMenu menu(this);
                menu.addAction(QStringLiteral("Copy image"), this,
                               [f]() { QApplication::clipboard()->setImage(f); });
                menu.addAction(QStringLiteral("Save image…"), this, [this, f]() {
                    const QString p = QFileDialog::getSaveFileName(this, QStringLiteral("Save frame"),
                        QStringLiteral("frame.png"), QStringLiteral("PNG (*.png);;JPEG (*.jpg)"));
                    if (!p.isEmpty()) f.save(p);
                });
                menu.exec(static_cast<QContextMenuEvent*>(ev)->globalPos());
            }
            return true;
        }
        if (ev->type() == QEvent::Enter)      wgt->setStyleSheet(QStringLiteral("background:#26303c;"));
        else if (ev->type() == QEvent::Leave) wgt->setStyleSheet(QString());
    }
    // Channel tiles: hide caption + 0.5s zoom popup on hover; wheel resizes the popup.
    for (int i = 0; i < 6; ++i) {
        if (obj != m_chanImg[i]) continue;
        const QEvent::Type t = ev->type();
        if (t == QEvent::Enter) {
            if (m_chanCap[i]) m_chanCap[i]->hide();
            m_hoverTile = i;
            if (!m_chanFull[i].isNull()) m_hoverTimer->start(HoverInfo::delayMs());
        } else if (t == QEvent::Leave) {
            if (m_chanCap[i] && !m_chanCap[i]->text().isEmpty()) m_chanCap[i]->show();
            m_hoverTile = -1; m_hoverTimer->stop(); hideTilePreview();
        } else if (t == QEvent::Wheel && m_iconPreview && m_iconPreview->isVisible()
                   && HoverInfo::scrollZoom()) {
            const auto* we = static_cast<QWheelEvent*>(ev);
            m_previewPx = qBound(64, m_previewPx + (we->angleDelta().y() > 0 ? 24 : -24), 1024);
            showTilePreview(i);
            return true;   // consume: scroll resizes the preview
        } else if (t == QEvent::MouseButtonPress) {
            m_dragStartPos = static_cast<QMouseEvent*>(ev)->globalPosition().toPoint();
        } else if (t == QEvent::MouseMove) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            if ((me->buttons() & Qt::LeftButton) && !m_chanFull[i].isNull()
                && (me->globalPosition().toPoint() - m_dragStartPos).manhattanLength()
                       > QApplication::startDragDistance()) {
                startImageDrag(m_chanFull[i], QStringLiteral("%1_%2")
                    .arg(m_currentName, m_chanCap[i] ? m_chanCap[i]->text() : QString::number(i)));
                return true;
            }
        }
        return BrowserTab::eventFilter(obj, ev);
    }
    if (m_galleryScroll && obj == m_galleryScroll->viewport()
               && ev->type() == QEvent::Resize) {
        // Only relay out when the width actually changed (avoids a rebuild loop).
        if (m_previewTabs && m_previewTabs->currentIndex() == 1
            && m_galleryScroll->viewport()->width() != m_galleryWidth)
            buildGallery();
    }
    // List (table) view: hover the row → the SAME info popup, minus the image (no icon column is
    // shown here, and the image belongs to icons only).
    if (m_view && obj == m_view->viewport()) {
        const QEvent::Type t = ev->type();
        if (t == QEvent::MouseMove) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            const QModelIndex idx = m_view->indexAt(me->position().toPoint());
            const SnoEntry* e = (idx.isValid() && m_model) ? m_model->entryAt(idx.row()) : nullptr;
            const int sno = e ? e->snoId : 0;
            if (sno != m_hoverGridSno || m_hoverIconArea) {
                m_hoverGridSno = sno;
                m_hoverTile = -1;
                m_hoverIconArea = false;   // info-only popup over list rows
                if (m_iconPreview) m_iconPreview->hide();
                if (sno > 0) m_hoverTimer->start(HoverInfo::delayMs()); else m_hoverTimer->stop();
            }
        } else if (t == QEvent::Leave) {
            m_hoverGridSno = 0; m_hoverTimer->stop();
            if (m_iconPreview) m_iconPreview->hide();
        }
    }

    // Grid view: dwell-hover → scaled popup · Ctrl+wheel resizes tiles · plain wheel over a visible
    // popup resizes the popup (mirrors the channel tiles). Plain wheel otherwise scrolls the list.
    if (m_grid && obj == m_grid->viewport()) {
        const QEvent::Type t = ev->type();
        if (t == QEvent::MouseMove) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            const QModelIndex idx = m_grid->indexAt(me->position().toPoint());
            const SnoEntry* e = idx.isValid() ? m_model->entryAt(idx.row()) : nullptr;
            const int sno = e ? e->snoId : 0;
            if (sno != m_hoverGridSno || !m_hoverIconArea) {
                m_hoverGridSno = sno;
                m_hoverTile = -1;
                m_hoverIconArea = true;   // grid cells ARE icons — include the image
                if (m_iconPreview) m_iconPreview->hide();
                if (sno > 0) m_hoverTimer->start(HoverInfo::delayMs()); else m_hoverTimer->stop();
            } else if (sno > 0 && m_iconPreview && m_iconPreview->isVisible()) {
                showGridPreview(sno);   // keep the popup on the hovered cell
            }
        } else if (t == QEvent::Leave) {
            m_hoverGridSno = 0; m_hoverTimer->stop();
            if (m_iconPreview) m_iconPreview->hide();
        } else if (t == QEvent::Resize) {
            if (m_gridScrollTimer) m_gridScrollTimer->start();   // more/fewer columns now visible
        } else if (t == QEvent::Wheel) {
            const auto* we = static_cast<QWheelEvent*>(ev);
            if (we->modifiers() & Qt::ControlModifier) {
                setGridIconPx(m_gridPx + (we->angleDelta().y() > 0 ? 16 : -16));
                return true;   // consume: Ctrl+scroll resizes tiles
            }
            if (m_iconPreview && m_iconPreview->isVisible() && m_hoverGridSno > 0
                && HoverInfo::scrollZoom()) {
                m_previewPx = qBound(64, m_previewPx + (we->angleDelta().y() > 0 ? 24 : -24), 1024);
                showGridPreview(m_hoverGridSno);
                return true;   // consume: scroll resizes the popup
            }
        }
    }
    return BrowserTab::eventFilter(obj, ev);
}

QImage TexturesTab::decodeTexture(int sno, const QString& name)
{
    const QString d4 = Config::d4dataDir();
    TexMeta meta;
    if (!d4.isEmpty()) {
        QFile f(QStringLiteral("%1/json/base/meta/Texture/%2.tex.json").arg(d4, name));
        if (f.open(QIODevice::ReadOnly)) meta = parseTexMetaJson(f.readAll());
    }
    if (!meta.valid) return {};
    QByteArray payload;
    if (m_reader && m_reader->isReady()) payload = m_reader->readPayloadBySno(quint64(sno));
    if (payload.isEmpty()) return {};
    m_preview->setTexture(payload, meta.width, meta.height, meta.eTexFormat);
    return m_preview->grabImage();
}

// ── Export-menu hooks (BrowserTab) ───────────────────────────────────────────
bool TexturesTab::hasExportSelection() const
{
    if (!m_view || !m_view->selectionModel()) return false;
    return !m_view->selectionModel()->selectedRows().isEmpty() || m_view->currentIndex().isValid();
}

void TexturesTab::exportSelection() { exportSelected(/*frames=*/false); }   // batch textures (prompts/last dir)

QString TexturesTab::exportNoun() const
{
    int n = 0;
    if (m_view && m_view->selectionModel()) n = m_view->selectionModel()->selectedRows().size();
    if (n == 0 && m_view && m_view->currentIndex().isValid()) n = 1;
    return n > 1 ? QStringLiteral("selected textures") : QStringLiteral("selected texture");
}

void TexturesTab::exportSelectionToLast() { exportTexture(/*toLast=*/true); }

void TexturesTab::exportTexture(bool toLast)
{
    const QImage img = m_preview->grabImage();
    if (img.isNull()) {
        QMessageBox::warning(this, QStringLiteral("Export"), QStringLiteral("No decoded texture."));
        return;
    }
    const QString stem = m_currentName.isEmpty() ? QStringLiteral("texture")
                                                 : NameTemplate::texture(m_currentName, m_currentSno);
    QSettings s;
    const QString ext = s.value(QStringLiteral("tex/format"), QStringLiteral("png"))
                            .toString().contains(QLatin1String("jp")) ? QStringLiteral("jpg")
                                                                       : QStringLiteral("png");
    QString path;
    if (toLast) {
        const QString dir = s.value(QStringLiteral("tex/lastDir")).toString();
        if (dir.isEmpty()) { exportTexture(false); return; }
        path = QDir(dir).filePath(stem + QStringLiteral(".") + ext);
    } else {
        path = QFileDialog::getSaveFileName(this, QStringLiteral("Export texture"),
            stem + QStringLiteral(".") + ext, QStringLiteral("PNG (*.png);;JPEG (*.jpg)"));
    }
    if (path.isEmpty()) return;
    if (img.save(path)) {
        const QString folder = QFileInfo(path).absolutePath();
        s.setValue(QStringLiteral("tex/lastDir"), folder);
        ExportNotifier::instance().notify(QStringLiteral("Exported %1").arg(QFileInfo(path).fileName()), folder);
    } else {
        QMessageBox::warning(this, QStringLiteral("Export"), QStringLiteral("Failed to save %1").arg(path));
    }
}

void TexturesTab::exportSelected(bool frames, bool toLast)
{
    QModelIndexList rows = m_view->selectionModel()->selectedRows();
    if (rows.isEmpty() && m_view->currentIndex().isValid()) rows << m_view->currentIndex();
    if (rows.isEmpty()) return;
    if (frames && rows.size() <= 1) { exportSelectedFrames(false, toLast); return; }

    QString dir;
    if (toLast) {
        dir = QSettings().value(QStringLiteral("tex/lastDir")).toString();
        if (dir.isEmpty()) { exportSelected(frames, false); return; }   // nothing remembered → prompt
    } else {
        dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Export %1 texture(s) to…").arg(rows.size()));
    }
    if (dir.isEmpty()) return;
    QProgressDialog prog(QStringLiteral("Exporting…"), QStringLiteral("Cancel"), 0, rows.size(), this);
    prog.setWindowModality(Qt::WindowModal);
    int ok = 0, i = 0;
    for (const QModelIndex& idx : rows) {
        prog.setValue(i++);
        if (prog.wasCanceled()) break;
        const SnoEntry* e = m_model->entryAt(idx.row());
        if (!e) continue;
        const QImage img = decodeTexture(e->snoId, e->name);
        if (!img.isNull()
            && img.save(QDir(dir).filePath(NameTemplate::texture(e->name, e->snoId) + QStringLiteral(".png")))) ++ok;
    }
    prog.setValue(rows.size());
    QSettings().setValue(QStringLiteral("tex/lastDir"), dir);
    onSelectionChanged();   // restore the current preview
    ExportNotifier::instance().notify(
        QStringLiteral("Exported %1 of %2 textures").arg(ok).arg(rows.size()), dir);
}

// hasFrameExport() gates the two menu items; "selected" exports the frame-list selection (falling
// back to all when nothing is selected), "all" always exports every frame of the current texture.
bool TexturesTab::hasFrameExport() const       { return !m_frameDefs.isEmpty(); }
void TexturesTab::exportFramesSelected()       { exportSelectedFrames(false, false); }
void TexturesTab::exportFramesSelectedToLast() { exportSelectedFrames(false, true); }
void TexturesTab::exportFramesAll()            { exportSelectedFrames(true,  false); }
void TexturesTab::exportFramesAllToLast()      { exportSelectedFrames(true,  true); }

void TexturesTab::exportSelectedFrames(bool all, bool toLast)
{
    if (!m_frames->selectionModel()) return;
    QList<int> rows;
    if (!all)
        for (const QModelIndex& idx : m_frames->selectionModel()->selectedRows())
            rows << idx.row();
    if (rows.isEmpty()) for (int i = 0; i < m_frameDefs.size(); ++i) rows << i;   // all frames
    if (rows.isEmpty()) return;
    // "→ last dir": reuse the remembered frames folder without prompting (fall back to prompt if
    // none remembered yet). Otherwise ask, and remember the chosen folder for next time.
    QSettings s;
    QString dir;
    if (toLast) {
        dir = s.value(QStringLiteral("tex/framesLastDir")).toString();
        if (dir.isEmpty()) { exportSelectedFrames(all, false); return; }
    } else {
        dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Export frames to…"));
    }
    if (dir.isEmpty()) return;
    s.setValue(QStringLiteral("tex/framesLastDir"), dir);   // remember for "→ last dir"
    const bool trim = m_trimCheck && m_trimCheck->isChecked();
    int ok = 0;
    for (int r : rows) {
        const QImage f = croppedFrame(r, trim);
        if (f.isNull()) continue;
        const quint32 h = (r < m_frameDefs.size()) ? quint32(m_frameDefs[r].handle) : 0;
        const QString frameName = h ? AppearanceMeta::instance().nameForIconHandle(h) : QString();
        // Template-driven (Settings ▸ Export ▸ File names). The DEFAULT is d4analyzer's TexFrames
        // format "<atlas> [<sno>] - <idx> <frameName>", which also re-imports via icon_overrides
        // (frameIconOverride parses "[sno] - idx") — keep it if you use that workflow.
        const QString base = NameTemplate::frame(m_currentName, m_currentSno, r, frameName);
        if (f.save(QDir(dir).filePath(base + QStringLiteral(".png")))) ++ok;
    }
    ExportNotifier::instance().notify(
        QStringLiteral("Exported %1 of %2 frames").arg(ok).arg(rows.size()), dir);
}
