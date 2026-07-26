#include "tabs/ModelsTab.h"

#include "app/AppPaths.h"
#include "app/Config.h"
#include "app/SehGuard.h"
#include "casc/CascReader.h"
#include "index/AppearanceMeta.h"
#include "index/IconIndex.h"
#include "index/ItemDef.h"
#include "model/AnimParser.h"
#include "gl/GLModelWidget.h"
#include "index/SnoIndex.h"
#include "index/SnoListModel.h"
#include "model/Appearance.h"
#include "model/Attachments.h"
#include "model/Material.h"
#include "model/MaterialDecode.h"
#include "model/ModelExporter.h"
#include "model/Retarget.h"
#include "model/Hardpoints.h"
#include "model/ModelGeometry.h"
#include "model/ModelParser.h"
#include "tex/BcDecode.h"
#include "tex/FrameTable.h"
#include "tex/TexFormat.h"
#include "tex/TexMeta.h"
#include "index/ItemHoverIndex.h"
#include "util/CsvCopy.h"
#include "util/DyeColorWheel.h"
#include "util/HoverInfo.h"
#include "util/PanelPersist.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QCheckBox>
#include <QFrame>
#include <QClipboard>
#include <QSpinBox>
#include <QGroupBox>
#include <QContextMenuEvent>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QComboBox>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QToolButton>
#include <QWidgetAction>
#include <QCoreApplication>
#include <QDataStream>
#include <QElapsedTimer>
#include <QDir>
#include <QSaveFile>
#include <QDirIterator>
#include <QEvent>
#include <QShowEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QSlider>

#include <cmath>
#include <functional>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QActionGroup>
#include <QMenu>
#include <QShortcut>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QMouseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QIcon>
#include <QInputDialog>
#include <QPainter>
#include <QScrollArea>
#include <QPair>
#include <QWheelEvent>
#include <algorithm>
#include <utility>   // std::as_const
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>
#include <QThread>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QRect>
#include <QSettings>
#include <QSizePolicy>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTableView>
#include <QTabWidget>
#include <QTimer>
#include <QScrollBar>
#include <QTreeView>
#include <QTreeWidget>
#include "tabs/ModelOutliner.h"
#include "tabs/HintBar.h"
#include <QVBoxLayout>

// (appendFitReferenceBody() + both export paths moved to ModelsTab_Export.cpp)

namespace {
constexpr int kGroupAppearance = 9;

// Grid-view cell painter: a square, aspect-preserved thumbnail (never cropped/stretched) with a
// single elided caption line beneath it. The default IconMode delegate wraps long D4 names onto
// several lines, which squeezes the icon into a thin strip — this fixes that.
class GridItemDelegate : public QStyledItemDelegate {
public:
    explicit GridItemDelegate(int iconPx, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), m_icon(iconPx) {}
    void setIconPx(int px) { m_icon = px; }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(m_icon + 26, m_icon + 34);   // icon square + one caption line + padding
    }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        p->save();
        const QRect r = opt.rect;
        if (opt.state & QStyle::State_Selected)        p->fillRect(r, QColor(0x8a, 0x14, 0x14));
        else if (opt.state & QStyle::State_MouseOver)  p->fillRect(r, QColor(0x3a, 0x20, 0x20));
        // Thumbnail — scaled to fit the icon square, keeping aspect, centred (letterboxed if wide).
        QPixmap pm;
        const QVariant dec = idx.data(Qt::DecorationRole);
        if (dec.canConvert<QIcon>()) pm = qvariant_cast<QIcon>(dec).pixmap(m_icon, m_icon);
        if (!pm.isNull()) {
            const QPixmap sp = pm.scaled(m_icon, m_icon, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            p->drawPixmap(r.left() + (r.width() - sp.width()) / 2,
                          r.top() + 4 + (m_icon - sp.height()) / 2, sp);
        }
        // Caption — one line, elided in the middle so the meaningful suffix stays visible.
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

// Extract one channel of an RGBA decode as grayscale (ch 0..3 = R,G,B,A). Used by the outliner's
// per-texture tile strips, their hover previews, and right-click extraction.
// (Defined here in full: this region is inside the file's anonymous namespace, so a forward
// declaration here + definition at file scope would create TWO functions — ambiguous overload.)
static QImage channelImage(const QImage& src, int ch)
{
    if (src.isNull() || ch < 0 || ch > 3) return src;
    const QImage in = src.convertToFormat(QImage::Format_RGBA8888);
    QImage out(in.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < in.height(); ++y) {
        const uchar* s = in.constScanLine(y);
        uchar* d = out.scanLine(y);
        for (int x = 0; x < in.width(); ++x) {
            const uchar v = s[x * 4 + ch];   // RGBA8888 byte order is R,G,B,A
            d[x * 4 + 0] = v; d[x * 4 + 1] = v; d[x * 4 + 2] = v; d[x * 4 + 3] = 255;
        }
    }
    return out;
}

// Display-mode glyphs for the outliner header dropdown (List / Outliner / Grid).
static QPixmap displayModeGlyph(int mode)
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c(200, 190, 150);
    if (mode == 0) {          // List: three full-width lines
        p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap));
        for (int y : {3, 7, 11}) p.drawLine(QPointF(2, y), QPointF(12, y));
    } else if (mode == 1) {   // Outliner: trunk with indented branches
        p.setPen(QPen(c, 1.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(3, 2.5), QPointF(3, 10.5));
        p.drawLine(QPointF(3, 6), QPointF(7, 6));
        p.drawLine(QPointF(3, 10.5), QPointF(7, 10.5));
        p.setPen(Qt::NoPen); p.setBrush(c);
        p.drawEllipse(QPointF(3, 2.5), 1.6, 1.6);
        p.drawEllipse(QPointF(8.7, 6), 1.6, 1.6);
        p.drawEllipse(QPointF(8.7, 10.5), 1.6, 1.6);
    } else {                  // Grid: four squares
        p.setPen(Qt::NoPen); p.setBrush(c);
        for (int x : {2, 8}) for (int y : {2, 8}) p.drawRect(x, y, 4, 4);
    }
    return pm;
}

// ── One visual language for every control this tab builds ───────────────────────────────────
// Two tiers, matching what the tab already used most: BUTTONS sit on the dark chrome (#2b2b2b,
// #555 border, 3px radius, red hover, red-filled when checked); PANELS/popups are one shade
// darker (#232323, #5a5a5a, 4px). Everything below is a named constant so a new control can't
// drift again — that drift is exactly how the header bar ended up unstyled while the viewport
// toolbar beside it was themed.
// (kToolBtnQss / kIconBtnQss / kArrowBtnQss / kPanelQss / kBarH moved to BrowserTab.h — the
// Wardrobe toolbar wears the same skin now, so the constants live where both tabs can see them.
// Defining them here again would make every unqualified use ambiguous.)

}   // ── leave the anonymous namespace: PanelBox is named in ModelsTab.h, so it must be a
    //    GLOBAL-scope type (an internal-linkage one would be a different class to the header's).

// PanelBox (the right-column stacking panel), its sizing contract, kWantH and the arrival-height
// helper all live in PanelBox.h — SHARED with the Wardrobe tab, which runs the same panel system.
#include "PanelBox.h"
#include "ViewGlyphs.h"   // shadeBallGlyph + overlayGlyph — shared with the Wardrobe toolbar
#include <QColorDialog>   // Backdrop ▸ custom colour

namespace {   // ── back to file-local helpers ──

// Gold checkmark (or nothing) — replaces QMenu's stock check indicator, which renders as an
// ugly undersized box under the app stylesheet. Menus using this must zero the indicator via
// "QMenu::indicator{width:0;height:0;}" and call bindCheckIcon on their checkable actions.
static QPixmap checkGlyph(bool on)
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    if (on) {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(0xd8, 0xa2, 0x3a), 1.9, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(2.5, 7.5), QPointF(5.5, 10.5));
        p.drawLine(QPointF(5.5, 10.5), QPointF(11.5, 3.5));
    }
    return pm;
}

static void bindCheckIcon(QAction* a)
{
    a->setIcon(QIcon(checkGlyph(a->isChecked())));
    QObject::connect(a, &QAction::toggled, a,
                     [a](bool on) { a->setIcon(QIcon(checkGlyph(on))); });
}

// Painter-drawn playback transport icons — 0 play · 1 pause · 2 step-back · 3 step-forward.
// (The unicode media glyphs rendered as mismatched emoji; these match the rest of the drawn UI.)
static QIcon transportGlyph(int kind)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c(210, 205, 190);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    switch (kind) {
    case 0:   // play
        p.drawPolygon(QPolygonF({{4.5, 2.5}, {4.5, 13.5}, {13.0, 8.0}}));
        break;
    case 1:   // pause
        p.drawRect(4, 3, 3, 10);
        p.drawRect(9, 3, 3, 10);
        break;
    case 2:   // step back: bar + left triangle
        p.drawRect(3, 3, 2, 10);
        p.drawPolygon(QPolygonF({{13.0, 3.0}, {13.0, 13.0}, {6.0, 8.0}}));
        break;
    case 3:   // step forward: right triangle + bar
        p.drawPolygon(QPolygonF({{3.0, 3.0}, {3.0, 13.0}, {10.0, 8.0}}));
        p.drawRect(11, 3, 2, 10);
        break;
    }
    return QIcon(pm);
}

// Distinctive glyph for the ATTACHMENTS panel strip button: a socket ring with a short link out
// to a small filled diamond (the held object). Drawn (no assets) like the outliner kind glyphs,
// but unique so it doesn't collide with the PARTS icon.
static QPixmap attachStripIcon()
{
    const qreal dpr = 2.0;
    QPixmap pm(int(16 * dpr), int(16 * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor c(210, 205, 190);
    QPen pen(c); pen.setWidthF(1.4);
    p.setPen(pen); p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(5.4, 10.6), 3.0, 3.0);                 // socket ring (lower-left)
    p.setBrush(c); p.drawEllipse(QPointF(5.4, 10.6), 0.9, 0.9);  // socket centre dot
    p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(7.5, 8.5), QPointF(10.3, 5.7));           // link out to the held object
    p.setBrush(c);                                               // held object: filled diamond
    p.drawPolygon(QPolygonF({{12.0, 2.8}, {14.2, 5.0}, {12.0, 7.2}, {9.8, 5.0}}));
    p.end();
    return pm;
}

// FX material by SHADER MAP: D4 effect materials use vfx/particle/distort/glow/… shader maps
// (verified against base/meta/ShaderMap names), so a weapon's glow/trail parts are caught even
// when the material NAME gives nothing away. Cheap single-file read; callers dedupe.
static bool matIsFxByShader(const QString& d4, const QString& matName)
{
    if (matName.isEmpty() || d4.isEmpty()) return false;
    QFile mf(QStringLiteral("%1/json/base/meta/Material/%2.mat.json").arg(d4, matName));
    if (!mf.open(QIODevice::ReadOnly)) return false;
    const QString sm = QJsonDocument::fromJson(mf.readAll()).object()
                           .value(QStringLiteral("tUberMaterial")).toObject()
                           .value(QStringLiteral("snoShaderMap")).toObject()
                           .value(QStringLiteral("name")).toString().toLower();
    if (sm.isEmpty()) return false;
    static const char* const kTok[] = { "vfx", "particle", "distort", "refract", "glow",
                                        "flipbook", "dissolve", "emissiveflow",
                                        "blend_uber_unlit", "trail", "_fx", "fxmesh" };
    for (const char* t : kTok) if (sm.contains(QLatin1String(t))) return true;
    return false;
}

// QTreeView whose expand/collapse arrows survive the app stylesheet. The global QSS styles
// QTreeView::indicator, which flips Qt into stylesheet-rendering for tree views — and with no
// ::branch image rules the branch arrows are drawn as NOTHING (this is also why the old PARTS
// pane never had arrows). Painting the glyphs ourselves needs no image assets and can't be
// silenced by QSS. Hit-testing/expansion is unaffected — this only draws.
class OutlinerView : public QTreeView {
public:
    using QTreeView::QTreeView;
protected:
    // A left-drag in a QTreeView sweeps the current row across every item the cursor passes over,
    // which here re-highlighted (and, with auto-load, tried to re-open) the wrong model mid-drag.
    // Ignore drag-moves entirely: a press selects/loads the clicked row, Ctrl/Shift-click still
    // multi-selects, and double-click loads — no accidental sweep. (Drag-autoscroll isn't needed
    // for the browse list; the wheel/scrollbar cover it.)
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (e->buttons() & Qt::LeftButton) return;
        QTreeView::mouseMoveEvent(e);
    }
    void drawBranches(QPainter* p, const QRect& rect, const QModelIndex& index) const override
    {
        if (!model() || !model()->hasChildren(index)) return;
        p->save();
        p->setPen(isExpanded(index) ? QColor(200, 190, 150) : QColor(150, 143, 120));
        // The last indentation() slice of the branch rect is this item's own arrow cell.
        const QRect cell(rect.right() - indentation() + 1, rect.top(), indentation(), rect.height());
        p->drawText(cell, Qt::AlignCenter,
                    isExpanded(index) ? QStringLiteral("▾") : QStringLiteral("▸"));
        p->restore();
    }
};

// A real D4 dye/pigment: name + its 4 colours (rgbaWardrobeColorSwatch), which map to
// the 4 DyeMask value-zones.
struct DyeDef {
    QString name;
    QColor  colors[4];
    int     sno = 0;
};

// Load the player-facing dye definitions from d4data (DyeDefinition.arColorSamples).
QVector<DyeDef> loadPlayerDyes(const QString& d4)
{
    QVector<DyeDef> out;
    if (d4.isEmpty()) return out;
    QDir dir(d4 + QStringLiteral("/json/base/meta/Dye"));
    const QStringList files = dir.entryList(QStringList{QStringLiteral("*.dye.json")}, QDir::Files);
    for (const QString& fn : files) {
        const QString stem = fn.left(fn.size() - 9);   // strip ".dye.json"
        if (stem.startsWith(QLatin1String("NPC_")) || stem == QLatin1String("Debug")
            || stem.contains(QLatin1String("Bad Data")))
            continue;
        QFile f(dir.filePath(fn));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        if (o.value(QStringLiteral("fHiddenFromUI")).toBool()) continue;
        // The 4 wardrobe swatches ARE the pigment's 4 colours (→ DyeMask zones).
        const QJsonArray sw = o.value(QStringLiteral("rgbaWardrobeColorSwatch")).toArray();
        if (sw.isEmpty()) continue;
        DyeDef d;
        d.name = stem;
        d.sno = o.value(QStringLiteral("__snoID__")).toInt();
        for (int i = 0; i < 4; ++i) {
            const QJsonObject c = sw[qMin(i, sw.size() - 1)].toObject();
            d.colors[i] = QColor(c.value(QStringLiteral("r")).toInt(), c.value(QStringLiteral("g")).toInt(),
                                 c.value(QStringLiteral("b")).toInt());
        }
        out.append(d);
    }
    std::sort(out.begin(), out.end(), [](const DyeDef& a, const DyeDef& b) { return a.name < b.name; });
    return out;
}

// Bake a pigment into a base-colour image (same maths as the shader) so exports
// match the preview: DyeMask is value-banded → classify the 4 neighbouring mask
// texels to their zone colours and bilinearly blend, for anti-aliased zone edges.
QImage applyDyeBake(const QImage& base0, const QImage& mask0, const QImage& ramp0,
                    const QColor pigment[4])
{
    if (base0.isNull() || mask0.isNull() || mask0.width() <= 8) return base0;
    QImage base = base0.convertToFormat(QImage::Format_RGBA8888);
    const int W = base.width(), H = base.height();
    const QImage mask = mask0.convertToFormat(QImage::Format_RGBA8888);   // native resolution
    const int mW = mask.width(), mH = mask.height();
    const QImage ramp = ramp0.isNull() ? QImage()
        : ramp0.convertToFormat(QImage::Format_RGBA8888).scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    static const float bands[4] = {0.063f, 0.345f, 0.596f, 0.831f};
    auto maskR = [&](int mx, int my) {
        mx = qBound(0, mx, mW - 1); my = qBound(0, my, mH - 1);
        return mask.constScanLine(my)[mx * 4] / 255.0f;
    };
    for (int y = 0; y < H; ++y) {
        uchar* b = base.scanLine(y);
        const uchar* r = ramp.isNull() ? nullptr : ramp.constScanLine(y);
        for (int x = 0; x < W; ++x) {
            const float sh = 0.35f + 1.05f * (r ? r[x * 4] / 255.0f : 0.5f);
            const float orig[3] = {float(b[x*4]), float(b[x*4+1]), float(b[x*4+2])};
            const float fx = (x + 0.5f) / W * mW - 0.5f, fy = (y + 0.5f) / H * mH - 0.5f;
            const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
            const float frx = fx - x0, fry = fy - y0;
            float out[3] = {0, 0, 0};
            const float wx[2] = {1 - frx, frx}, wy[2] = {1 - fry, fry};
            for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
                const float w = wx[i] * wy[j];
                const float mv = maskR(x0 + i, y0 + j);
                float cr, cg, cb;
                if (mv <= 0.02f) { cr = orig[0]; cg = orig[1]; cb = orig[2]; }
                else {
                    int zone = 0; float best = 2.0f;
                    for (int k = 0; k < 4; ++k) { const float d = qAbs(mv - bands[k]); if (d < best) { best = d; zone = k; } }
                    cr = qBound(0.0f, pigment[zone].red()   * sh, 255.0f);
                    cg = qBound(0.0f, pigment[zone].green() * sh, 255.0f);
                    cb = qBound(0.0f, pigment[zone].blue()  * sh, 255.0f);
                }
                out[0] += cr * w; out[1] += cg * w; out[2] += cb * w;
            }
            b[x * 4 + 0] = uchar(qBound(0, int(out[0] + 0.5f), 255));
            b[x * 4 + 1] = uchar(qBound(0, int(out[1] + 0.5f), 255));
            b[x * 4 + 2] = uchar(qBound(0, int(out[2] + 0.5f), 255));
        }
    }
    return base;
}

// Disk path for a model's cached 3D-render thumbnail, so list icons persist across
// sessions instead of being re-rendered each launch.
QString thumbCachePath(int sno)
{
    static const QString dir = AppPaths::subDir(QStringLiteral("model_thumbs"));
    return dir + QStringLiteral("/%1.png").arg(sno);
}

// Sentinel written just before a risky thumbnail render and deleted right after. If it
// survives to the next launch, the tool crashed rendering that model → we blocklist it.
QString renderGuardPath()
{
    return AppPaths::file(QStringLiteral("model_render.guard"));
}
// Human-readable append-only log of models that crashed the renderer.
QString renderCrashLogPath()
{
    return AppPaths::file(QStringLiteral("model_render_crashes.log"));
}

// ── Parallel file-scan helper (first-run indexing speed) ──────────────────────────────────────────
namespace {
// Parse a big list of metadata files across all CPU cores. `parse(path)` runs on worker threads and
// returns one record R per file (no shared state → no locking); the caller then aggregates the returned
// records serially (keeping the existing, correct merge logic). Results preserve input order.
// `report(done,total)` is polled from the coordinating thread for live progress. Blocks until done —
// call it from a background thread (the scans already run on one). Cuts first-run parsing ~N-fold.
// `threadMul` oversubscribes the pool: for I/O-bound loose-file scans (opening tens of thousands of
// tiny JSON files, where threads spend most of their time blocked on disk), running ~2× cores hides
// that latency and raises throughput. Leave it 1 for CPU-bound work (e.g. the texture BC-decode).
template <typename R, typename ParseFn, typename ReportFn>
std::vector<R> parallelMap(const QStringList& files, ParseFn parse, ReportFn report,
                           bool installSeh = false, int threadMul = 1)
{
    const int total = files.size();
    const size_t nSlots = size_t(total);   // NB: a plain variable (NOT 'slots' — that's a Qt macro!)
    std::vector<R> out(nSlots);   // one slot per file; workers write distinct indices (no COW, no lock)
                                  // NB: 'nSlots' as a plain variable also avoids a most-vexing-parse.
    if (total == 0) return out;
    unsigned hw = std::thread::hardware_concurrency();
    if (hw < 2) hw = 2;
    if (threadMul < 1) threadMul = 1;
    const int nThreads = std::min<int>(int(hw) * threadMul, total);
    std::atomic<int> done{0};
    std::vector<std::thread> pool;
    pool.reserve(size_t(nThreads));
    for (int t = 0; t < nThreads; ++t) {
        pool.emplace_back([&, t]() {
            if (installSeh) seh::installSehTranslator();
            for (int i = t; i < total; i += nThreads) {   // strided → balanced load
                // A parse that throws (or an SEH fault translated to a C++ exception when
                // installSeh is set) must not std::terminate the whole app from a worker —
                // leave that slot default-constructed so one bad file is skipped, not fatal.
                try { out[i] = parse(files.at(i)); }
                catch (...) { }
                done.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    while (done.load(std::memory_order_relaxed) < total) {
        report(done.load(std::memory_order_relaxed), total);
        QThread::msleep(60);
    }
    for (std::thread& th : pool) th.join();
    report(total, total);
    return out;
}
} // namespace

// ── Disk cache for the heavy background indexes (anim + entity) ───────────────────────────────────
namespace {
// Cache key = game build + buildVersion.txt mtime. A game patch (new build) OR a d4data re-extraction
// (which rewrites buildVersion.txt, changing its mtime) invalidates the cache → forces a re-scan.
QString d4dataSignature()
{
    const QString bv = Config::d4dataDir() + QStringLiteral("/buildVersion.txt");
    QString ver;
    QFile f(bv);
    if (f.open(QIODevice::ReadOnly)) ver = QString::fromUtf8(f.readAll()).trimmed();
    const QDateTime m = QFileInfo(bv).lastModified();
    return ver + QLatin1Char('|') + (m.isValid() ? m.toString(Qt::ISODate) : QString());
}
QString indexCachePath(const QString& name)
{
    static const QString dir = AppPaths::subDir(QStringLiteral("index_cache"));
    return dir + QLatin1Char('/') + name;
}

// The full result of the animation scan (Anim + AnimSet + rig-bone parse + family index).
struct AnimBlob {
    QSet<int>                     animatedSnos;
    QHash<int, QStringList>       rowsBySno;
    QSet<QString>                 famPrefixes;
    QHash<QString, QStringList>   famRows;
    QHash<QString, QString>       famOwner;
    QHash<QString, QSet<quint32>> famBones;
    QHash<QString, QString>       clipSet;
    QHash<QString, QStringList>   setClips;
    QSet<QString>                 femaleClips;
    QHash<QString, QString>       femalePair;
    QHash<QString, QString>       clipPower;
};
QDataStream& operator<<(QDataStream& ds, const AnimBlob& b) {
    return ds << b.animatedSnos << b.rowsBySno << b.famPrefixes << b.famRows << b.famOwner
              << b.famBones << b.clipSet << b.setClips << b.femaleClips << b.femalePair << b.clipPower;
}
QDataStream& operator>>(QDataStream& ds, AnimBlob& b) {
    return ds >> b.animatedSnos >> b.rowsBySno >> b.famPrefixes >> b.famRows >> b.famOwner
              >> b.famBones >> b.clipSet >> b.setClips >> b.femaleClips >> b.femalePair >> b.clipPower;
}

// The full result of the entity scan (Actor + Item metadata).
struct EntityBlob {
    QHash<int, QStringList> apprActors;
    QHash<int, int>         apprActorN;
    QHash<int, QString>     apprFamily;
    QHash<int, QStringList> apprItems;
    QHash<int, int>         apprItemN;
    QHash<QString, int>     itemAppr;
    QHash<int, QStringList> apprSets;
    QHash<int, QStringList> apprVariants;
    QHash<int, QList<int>>  apprVariantSnos;
    QHash<int, QString>     apprName;
};
QDataStream& operator<<(QDataStream& ds, const EntityBlob& b) {
    return ds << b.apprActors << b.apprActorN << b.apprFamily << b.apprItems << b.apprItemN
              << b.itemAppr << b.apprSets << b.apprVariants << b.apprVariantSnos << b.apprName;
}
QDataStream& operator>>(QDataStream& ds, EntityBlob& b) {
    return ds >> b.apprActors >> b.apprActorN >> b.apprFamily >> b.apprItems >> b.apprItemN
              >> b.itemAppr >> b.apprSets >> b.apprVariants >> b.apprVariantSnos >> b.apprName;
}

template <typename T>
bool readIndexCache(const QString& path, const QString& magic, const QString& sig, T& out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QDataStream ds(&f);
    ds.setVersion(QDataStream::Qt_6_0);
    QString gotMagic, gotSig;
    ds >> gotMagic >> gotSig;
    if (gotMagic != magic || gotSig != sig) return false;   // wrong version → re-scan
    ds >> out;
    return ds.status() == QDataStream::Ok;
}
template <typename T>
void writeIndexCache(const QString& path, const QString& magic, const QString& sig, const T& blob)
{
    QSaveFile f(path);   // atomic write (temp + rename) so a crash mid-write can't corrupt the cache
    if (!f.open(QIODevice::WriteOnly)) return;
    QDataStream ds(&f);
    ds.setVersion(QDataStream::Qt_6_0);
    ds << magic << sig << blob;
    f.commit();
}
}   // namespace

// Equipment slot tags (DJB2 seed=0 of the 3-letter slot code → readable name).
// Matches d4extract's hash dictionary; used to label parts whose material slot
// has no .mat name (armour pieces resolve to "Torso"/"Helmet"/… not blank).
QString slotLabelForHash(quint32 h)
{
    switch (h) {
        case 110143u: return QStringLiteral("Body");
        case 110665u: return QStringLiteral("Boots");
        case 115849u: return QStringLiteral("Gloves");
        case 116929u: return QStringLiteral("Helmet");
        case 121048u: return QStringLiteral("Legs");
        case 130201u: return QStringLiteral("Torso");
        default:      return QString();
    }
}

// Strip artist-convention boilerplate so "Lilith_LowerBody_mat" reads as
// "Lilith_LowerBody" in the parts list (matches d4extract's _sanitise).
QString prettyMatName(const QString& n)
{
    static const char* const suf[] = {"_mat", "_Mat", "_MAT", "_material",
                                       "_Material", "_diffuse", "_Diffuse"};
    for (const char* s : suf) {
        const QString q = QString::fromLatin1(s);
        if (n.endsWith(q) && n.size() > q.size()) return n.left(n.size() - q.size());
    }
    return n;
}

QString humanSize(qint64 n)
{
    const char* u[] = {"B", "KiB", "MiB", "GiB"};
    double f = double(n);
    int i = 0;
    while (f >= 1024.0 && i < 3) { f /= 1024.0; ++i; }
    return i == 0 ? QStringLiteral("%1 B").arg(n)
                  : QStringLiteral("%1 %2").arg(f, 0, 'f', 1).arg(u[i]);
}

// Decode a material texture (by sno+name) from CASC → RGBA QImage, or null.
QImage decodeMatTex(CascReader* reader, const QString& d4, const QString& name, int sno)
{
    if (!reader || !reader->isReady() || d4.isEmpty() || name.isEmpty())
        return {};
    QFile tf(QStringLiteral("%1/json/base/meta/Texture/%2.tex.json").arg(d4, name));
    if (!tf.open(QIODevice::ReadOnly)) return {};
    const TexMeta tmeta = parseTexMetaJson(tf.readAll());
    if (!tmeta.valid) return {};
    const QByteArray pl = reader->readPayloadBySno(quint64(sno));
    if (pl.isEmpty()) return {};
    return BcDecode::decode(pl, tmeta.width, tmeta.height, tmeta.eTexFormat);
}

// Reconstruct a glTF tangent-space normal map from a BC5 (RG) decode: B = +Z.
QImage reconstructNormal(const QImage& rg)
{
    if (rg.isNull()) return {};
    QImage out(rg.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < rg.height(); ++y) {
        const uchar* in = rg.constScanLine(y);
        uchar* o = out.scanLine(y);
        for (int x = 0; x < rg.width(); ++x) {
            const float nx = in[x * 4 + 0] / 255.0f * 2.0f - 1.0f;
            const float ny = in[x * 4 + 1] / 255.0f * 2.0f - 1.0f;
            float nz = 1.0f - nx * nx - ny * ny;
            nz = nz > 0.0f ? std::sqrt(nz) : 0.0f;
            o[x * 4 + 0] = in[x * 4 + 0];
            o[x * 4 + 1] = in[x * 4 + 1];
            o[x * 4 + 2] = uchar(qBound(0, int((nz * 0.5f + 0.5f) * 255.0f + 0.5f), 255));
            o[x * 4 + 3] = 255;
        }
    }
    return out;
}

// Pack AO/Roughness/Metallic single-channel maps into one ORM image
// (R=occlusion, G=roughness, B=metallic), sized to the largest input.
QImage packORM(const QImage& ao, const QImage& rough, const QImage& metal)
{
    QSize sz;
    for (const QImage* m : {&ao, &rough, &metal})
        if (!m->isNull() && m->width() * m->height() > sz.width() * sz.height()) sz = m->size();
    if (!sz.isValid() || sz.isEmpty()) return {};
    auto scaled = [&](const QImage& m) {
        return m.isNull() ? QImage() : m.scaled(sz, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    };
    const QImage a = scaled(ao), r = scaled(rough), me = scaled(metal);
    QImage out(sz, QImage::Format_RGBA8888);
    for (int y = 0; y < sz.height(); ++y) {
        uchar* o = out.scanLine(y);
        const uchar* ap = a.isNull() ? nullptr : a.constScanLine(y);
        const uchar* rp = r.isNull() ? nullptr : r.constScanLine(y);
        const uchar* mp = me.isNull() ? nullptr : me.constScanLine(y);
        for (int x = 0; x < sz.width(); ++x) {
            o[x * 4 + 0] = ap ? ap[x * 4] : 255;   // occlusion (default 1)
            o[x * 4 + 1] = rp ? rp[x * 4] : 255;   // roughness (default 1)
            o[x * 4 + 2] = mp ? mp[x * 4] : 0;     // metallic  (default 0)
            o[x * 4 + 3] = 255;
        }
    }
    return out;
}
}   // ── end anonymous namespace ── the two helpers below are given EXTERNAL linkage so the
    //    Wardrobe tab can reuse the same palette/material-export pipeline (it forward-declares them).

// Default-look material roster (index == materialIndex): first non-empty
// snoMaterial across the SOAs, else the cloth name. Used for batch export of
// models that aren't the currently-loaded one (no live look state for them).
QStringList appearancePalette(const QString& d4, const QString& name)
{
    QStringList palette;
    QFile af(QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4, name));
    if (!af.open(QIODevice::ReadOnly)) return palette;
    const QJsonArray rawMats = QJsonDocument::fromJson(af.readAll()).object()
                                   .value(QStringLiteral("ptAppearanceMaterials")).toArray();
    for (const QJsonValue& mv : rawMats) {
        const QJsonArray soas = mv.toObject().value(QStringLiteral("ptSOAs")).toArray();
        QString nm, cloth;
        for (const QJsonValue& sv : soas) {
            const QJsonObject s = sv.toObject();
            if (nm.isEmpty())
                nm = s.value(QStringLiteral("snoMaterial")).toObject()
                      .value(QStringLiteral("name")).toString();
            if (cloth.isEmpty())
                cloth = s.value(QStringLiteral("snoCloth")).toObject()
                         .value(QStringLiteral("name")).toString();
        }
        palette.append(nm.isEmpty() ? cloth : nm);
    }
    return palette;
}

// Build ExportMaterials for a palette (index == materialIndex), resolving empty
// slots to the dominant token-matched body material — shared by single + batch export.
QVector<ModelExporter::ExportMaterial> buildExportMats(
    const QStringList& palette, const ModelGeometry& geo, const QString& modelName,
    const QString& d4, CascReader* reader, bool wantTex)
{
    QVector<ModelExporter::ExportMaterial> mats;
    if (d4.isEmpty()) return mats;
    // Dye-bake settings (so exports match the preview when enabled). The active
    // pigment is just the 4 dye-colour slots (set by the dropdown or custom).
    const bool bakeDye = QSettings().value(QStringLiteral("export/bakeDye"), false).toBool()
                         && QSettings().value(QStringLiteral("models/viewport/dye"), false).toBool();
    QColor pigment[4];
    for (int k = 0; k < 4; ++k)
        pigment[k] = QColor(QSettings().value(QStringLiteral("models/viewport/dyeColor%1").arg(k),
                                              QStringLiteral("#ffffff")).toString());
    QHash<int, int> triByMat;
    for (const MeshPrimitive& p : geo.primitives)
        triByMat[p.materialIndex] += p.indices.size() / 3;
    QString token;
    { const int sp = modelName.indexOf(QStringLiteral("stor"), 0, Qt::CaseInsensitive);
      if (sp >= 0) token = modelName.mid(sp); }
    QString fallbackMat; int bestTris = -1; bool matchedToken = false;
    for (auto it = triByMat.constBegin(); it != triByMat.constEnd(); ++it) {
        const QString n = palette.value(it.key());
        if (n.isEmpty()) continue;
        const bool tok = !token.isEmpty() && n.contains(token, Qt::CaseInsensitive);
        const bool better = (tok && !matchedToken) || (tok == matchedToken && it.value() > bestTris);
        if (better) { matchedToken = tok; bestTris = it.value(); fallbackMat = n; }
    }
    auto buildMat = [&](const QString& matName) -> ModelExporter::ExportMaterial {
        ModelExporter::ExportMaterial em;
        em.name = matName;
        QFile mf(QStringLiteral("%1/json/base/meta/Material/%2.mat.json").arg(d4, matName));
        if (mf.open(QIODevice::ReadOnly)) {
            const QByteArray matData = mf.readAll();
            const MaterialValues v = parseMaterialValues(matData);
            if (v.valid) {
                em.hasMetal = v.hasMetal; em.metal = v.metal;
                em.hasRough = v.hasRough; em.rough = v.rough;
                if (v.hasEmisColor || v.hasEmisMult) {
                    em.hasEmissive = true;
                    em.emisR = v.emisR; em.emisG = v.emisG; em.emisB = v.emisB;
                    em.emisMult = v.hasEmisMult ? v.emisMult : 1.0f;
                }
            }
            if (wantTex && reader) {
                QImage normalRG, aoImg, roughImg, metalImg, dyeMaskImg, dyeRampImg;
                for (const MatTexture& mt : parseMaterialJson(matData)) {
                    if (mt.texName.isEmpty()) continue;
                    const QString& role = mt.role;
                    if (role == QLatin1String("BASE_COLOR")) {
                        if (em.baseColor.isNull()) em.baseColor = decodeMatTex(reader, d4, mt.texName, mt.texSno);
                    } else if (role == QLatin1String("NORMAL")) {
                        if (normalRG.isNull()) normalRG = decodeMatTex(reader, d4, mt.texName, mt.texSno);
                    } else if (role == QLatin1String("ROUGHNESS")) {
                        if (roughImg.isNull()) roughImg = decodeMatTex(reader, d4, mt.texName, mt.texSno);
                    } else if (role == QLatin1String("METALLIC")) {
                        if (metalImg.isNull()) metalImg = decodeMatTex(reader, d4, mt.texName, mt.texSno);
                    } else if (role == QLatin1String("AO")) {
                        if (aoImg.isNull()) aoImg = decodeMatTex(reader, d4, mt.texName, mt.texSno);
                    } else if (role == QLatin1String("EMISSIVE")) {
                        if (em.emissive.isNull()) em.emissive = decodeMatTex(reader, d4, mt.texName, mt.texSno);
                    } else if (bakeDye && role == QLatin1String("DYE_MASK")) {
                        if (dyeMaskImg.isNull()) dyeMaskImg = decodeMatTex(reader, d4, mt.texName, mt.texSno);
                    } else if (bakeDye && role == QLatin1String("DYE_RAMP")) {
                        if (dyeRampImg.isNull()) dyeRampImg = decodeMatTex(reader, d4, mt.texName, mt.texSno);
                    }
                }
                em.normal = reconstructNormal(normalRG);
                em.orm = packORM(aoImg, roughImg, metalImg);
                // Bake the active pigment into the exported base colour (matches preview).
                if (bakeDye && !dyeMaskImg.isNull() && !em.baseColor.isNull())
                    em.baseColor = applyDyeBake(em.baseColor, dyeMaskImg, dyeRampImg, pigment);
            }
        }
        return em;
    };
    mats.reserve(palette.size());
    for (int mi = 0; mi < palette.size(); ++mi) {
        QString usedMat = palette[mi];
        if (usedMat.isEmpty()) usedMat = fallbackMat;
        mats.push_back(usedMat.isEmpty() ? ModelExporter::ExportMaterial{} : buildMat(usedMat));
    }
    return mats;
}
namespace {   // ── re-enter the anonymous namespace for the remaining file-local helpers ──

// (exportModelDeps() moved to ModelsTab_Export.cpp)

// (fmtBytes() moved to ModelsTab_Export.cpp with showDependencies)

QTableView* makeTable(QWidget* parent, QStandardItemModel* model)
{
    auto* t = new QTableView(parent);
    t->setModel(model);
    t->setSortingEnabled(true);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setAlternatingRowColors(true);
    t->verticalHeader()->setVisible(false);
    t->horizontalHeader()->setStretchLastSection(true);
    // Right-column tables live in a drag-resizable splitter: they must FILL the height their
    // panel is given and scroll past it — never dictate it. The floor is header + ~1 row, so a
    // panel can always be squeezed down to a sliver without the splitter refusing the drag.
    t->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    t->setMinimumHeight(46);
    return t;
}
}   // ── end anonymous namespace ──

// Record a stacked-section table's natural height (its rows, up to maxRows). Declared here
// (external linkage, OUTSIDE the anonymous namespace above) because the section fills call it
// from all over this file, and its definition sits further down next to the other page fillers.
void autoSizeTable(QTableView* t, int maxRows = 10);

ModelsTab::ModelsTab(QWidget* parent) : BrowserTab(parent)
{
    // If a previous session left a render-guard sentinel, the tool crashed while rendering a
    // model thumbnail. Recover: blocklist the culprit, revert to Original icons, log it. Must
    // run before the icon combo is built so it reads the reverted mode.
    recoverFromRenderCrash();
    for (const QString& s : QSettings().value(QStringLiteral("models/renderBlocklist")).toStringList())
        m_renderBlocklist.insert(s.toInt());

    // Perf-cache budgets (cost is tracked in KB). These bound memory while making re-selects
    // and look changes near-instant. Roughly ~192 MB of decoded textures + ~64 MB of geometry.
    m_texCache.setMaxCost(192 * 1024);
    m_geoCache.setMaxCost(64 * 1024);

    auto* root = new QVBoxLayout(this);   // vertical so the first-run hint can sit above the columns
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    if (QWidget* hint = makeHintBar(this,
            QStringLiteral("Tip: double-click a part in the viewport to select it · F fullscreen · "
                           "H / Shift+H / Alt+H hide · middle-click re-frames · F1 lists everything"),
            "hints/models"))
        root->addWidget(hint);
    auto* split = new QSplitter(Qt::Horizontal, this);
    m_mainSplit = split;   // "Maximize viewport" hides its side panes in place
    root->addWidget(split, 1);

    // ════════ LEFT column: filters + list ════════
    auto* left = new QWidget(split);
    auto* ll = new QVBoxLayout(left);
    ll->setContentsMargins(4, 4, 4, 4);

    // ── Compact filter bar ── everything below is CREATED here, then placed into one always-visible
    // row (search + Filters/View toggles + count) plus two collapsible sections — see the assembly
    // near the end. Tex / Anim mirror the shared export settings and now live in the View section.
    m_exportTex = new QCheckBox(QStringLiteral("Tex"), left);
    m_exportTex->setChecked(QSettings().value(QStringLiteral("export/includeTex"), true).toBool());
    m_exportTex->setToolTip(QStringLiteral("Include textures (synced with Export settings)"));
    connect(m_exportTex, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("export/includeTex"), on);
    });
    m_exportAnim = new QCheckBox(QStringLiteral("Anim"), left);
    m_exportAnim->setChecked(QSettings().value(QStringLiteral("export/includeAnim"), false).toBool());
    m_exportAnim->setToolTip(QStringLiteral("Embed the animation currently playing in the preview into the "
                                            "exported .glb (synced with Export settings)"));
    connect(m_exportAnim, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("export/includeAnim"), on);
    });
    m_onlyDecrypted = new QCheckBox(QStringLiteral("Only decrypted"), left);
    m_onlyDecrypted->setToolTip(QStringLiteral("Hide models encrypted with a TACT key you don't have (they can't be opened)"));
    connect(m_onlyDecrypted, &QCheckBox::toggled, this, [this](bool) { loadList(); });   // re-filter the list

    m_snoSearch = new QLineEdit(left);
    m_snoSearch->setPlaceholderText(QStringLiteral("SNO"));
    m_snoSearch->setClearButtonEnabled(true);
    m_search = new QLineEdit(left);
    m_search->setPlaceholderText(QStringLiteral("NAME"));
    m_search->setClearButtonEnabled(true);
    m_search->setToolTip(QStringLiteral(
        "Filter by name. Space-separate terms (all must match); prefix a term with \"-\" to EXCLUDE it.\n"
        "e.g.  pandem -destroyed -pillar"));
    m_collSearch = new QLineEdit(left);
    m_collSearch->setPlaceholderText(QStringLiteral("COLLECTION"));
    m_collSearch->setClearButtonEnabled(true);
    m_catCombo = new QComboBox(left);
    m_catCombo->addItem(QStringLiteral("All Tags"), QString());   // populated on meta-ready
    m_catCombo->addItem(QStringLiteral("Latest (new this update)"), QStringLiteral("__latest__"));  // added since the previous build
    m_catCombo->addItem(QStringLiteral("Animated"), QStringLiteral("__animated__"));  // owns clips or inherits a base rig's clips
    m_catCombo->addItem(QStringLiteral("Rigged"), QStringLiteral("__rigged__"));      // belongs to a base rig (⊇ Animated)
    m_catCombo->addItem(QStringLiteral("Orphaned (no actor)"), QStringLiteral("__orphaned__"));  // not used by any actor
    m_catCombo->setToolTip(QStringLiteral("Filter by category tag, or by usage: Latest / Animated / Rigged / Orphaned"));
    m_classCombo = new QComboBox(left);
    m_classCombo->setToolTip(QStringLiteral("Filter by class"));
    m_classCombo->addItem(QStringLiteral("(any)"), QString());
    // Class list derived from THE central class table (AppearanceMeta) — a new class added there
    // (a Spiritborn-style expansion) appears here automatically, sorted by display name.
    {
        QStringList prefs = AppearanceMeta::heroClassPrefixes();
        std::sort(prefs.begin(), prefs.end(), [](const QString& a, const QString& b) {
            return AppearanceMeta::classDisplayName(a) < AppearanceMeta::classDisplayName(b); });
        for (const QString& p : prefs) m_classCombo->addItem(AppearanceMeta::classDisplayName(p), p);
    }
    m_genderCombo = new QComboBox(left);
    m_genderCombo->setToolTip(QStringLiteral("Filter by gender"));
    m_genderCombo->addItem(QStringLiteral("(any)"), QString());
    m_genderCombo->addItem(QStringLiteral("Female"), QStringLiteral("f"));
    m_genderCombo->addItem(QStringLiteral("Male"), QStringLiteral("m"));
    m_typeCombo = new QComboBox(left);
    m_typeCombo->setToolTip(QStringLiteral("Filter by item type (helm, torso, weapon…)"));
    m_typeCombo->addItem(QStringLiteral("(any)"), QString());
    const QVector<QPair<QString, QString>> kTypes = {
        {QStringLiteral("Helm"), QStringLiteral("hlm")}, {QStringLiteral("Head"), QStringLiteral("hed")},
        {QStringLiteral("Torso"), QStringLiteral("trs")}, {QStringLiteral("Shoulders"), QStringLiteral("sho")},
        {QStringLiteral("Gloves"), QStringLiteral("glv")}, {QStringLiteral("Legs"), QStringLiteral("leg")},
        {QStringLiteral("Boots"), QStringLiteral("bts")}, {QStringLiteral("Cape"), QStringLiteral("cap")},
        {QStringLiteral("Belt"), QStringLiteral("blt")},
        {QStringLiteral("Sword"), QStringLiteral("sword")}, {QStringLiteral("Axe"), QStringLiteral("axe")},
        {QStringLiteral("Mace"), QStringLiteral("mace")}, {QStringLiteral("Dagger"), QStringLiteral("dagger")},
        {QStringLiteral("Bow"), QStringLiteral("bow")}, {QStringLiteral("Crossbow"), QStringLiteral("crossbow")},
        {QStringLiteral("Staff"), QStringLiteral("staff")}, {QStringLiteral("Polearm"), QStringLiteral("polearm")},
        {QStringLiteral("Scythe"), QStringLiteral("scythe")}, {QStringLiteral("Shield"), QStringLiteral("shield")},
        {QStringLiteral("Wand"), QStringLiteral("wand")}, {QStringLiteral("Focus"), QStringLiteral("focus")},
        {QStringLiteral("Totem"), QStringLiteral("totem")}};
    for (const auto& t : kTypes) m_typeCombo->addItem(t.first, t.second);

    // Group-by dropdown (placed in the View section below).
    m_groupCombo = new QComboBox(left);
    m_groupCombo->setToolTip(QStringLiteral("Cluster the list under headers by collection, set, class, type, or category"));
    m_groupCombo->addItem(QStringLiteral("None"),       QStringLiteral(""));
    m_groupCombo->addItem(QStringLiteral("Collection"), QStringLiteral("collection"));
    m_groupCombo->addItem(QStringLiteral("Appearance set"), QStringLiteral("appset"));
    m_groupCombo->addItem(QStringLiteral("Set (M+F)"),  QStringLiteral("set"));
    m_groupCombo->addItem(QStringLiteral("Class"),      QStringLiteral("class"));
    m_groupCombo->addItem(QStringLiteral("Type"),       QStringLiteral("type"));
    m_groupCombo->addItem(QStringLiteral("Category"),   QStringLiteral("category"));

    m_countLabel = new QLabel(QStringLiteral("0 models"), left);
    // (Indexing progress is shown in the app's floating toast, not inline — see setScan/scanStatus.)
    m_iconModeCombo = new QComboBox(left);
    m_iconModeCombo->addItem(QStringLiteral("Original icons"), QStringLiteral("orig"));
    m_iconModeCombo->addItem(QStringLiteral("3D"), QStringLiteral("3d"));
    m_iconModeCombo->addItem(QStringLiteral("Original + 3D"), QStringLiteral("both"));
    // Default = Original; the chosen mode is remembered across sessions. 3D/both auto-render
    // thumbnails for rows scrolled into view (see scheduleVisibleIconRender).
    {
        const QString saved = QSettings().value(QStringLiteral("models/iconMode"),
                                                 QStringLiteral("orig")).toString();
        int mi = m_iconModeCombo->findData(saved);
        m_iconModeCombo->setCurrentIndex(mi >= 0 ? mi : 0);
    }
    m_iconModeCombo->setToolTip(QStringLiteral(
        "List icon style: 2D inventory icons, rendered 3D thumbnails, or both."));
    // ── Stable filter widths ─────────────────────────────────────────────────────────────────
    // These four combos are the DATA-DRIVEN ones — filled from real game tags. A QComboBox defaults
    // to AdjustToContentsOnFirstShow, i.e. it measures its WIDEST item and demands that width, so a
    // single long class/type name set the minimum width of the whole filter row, and the row in turn
    // shoved the left column wider the moment you toggled Filters open. Asking only for a small
    // character budget makes the minimum depend on the layout, never on the data; each of these has
    // stretch 1 in the filter row, so they still expand to fill it and only elide when genuinely
    // narrow — and the drop-down always opens at full content width, so no name is unreadable.
    // (m_groupCombo / m_iconModeCombo are deliberately excluded: their items are hardcoded, so their
    // content-based width is already stable and looks better sized to fit.)
    // (Qt6 removed the plain AdjustToMinimumContentsLength; the WithIcon variant is all that's left,
    // and it reserves room for an icon. These combos are text-only, so zero the icon size to reclaim
    // that padding — otherwise each one silently carries ~20px it will never draw into.)
    for (QComboBox* c : {m_catCombo, m_classCombo, m_genderCombo, m_typeCombo}) {
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setIconSize(QSize(0, 0));
        c->setMinimumContentsLength(6);
    }

    m_clearBtn = new QPushButton(QStringLiteral("Clear"), left);
    m_clearBtn->setToolTip(QStringLiteral("Clear all filters and the search boxes"));
    m_multiSelect = new QCheckBox(QStringLiteral("Multi select"), left);
    m_multiSelect->setToolTip(QStringLiteral("Select several models at once (Ctrl/Shift-click) to batch-export"));
    m_hideBrokenChk = new QCheckBox(QStringLiteral("Hide un-renderable"), left);
    m_hideBrokenChk->setToolTip(QStringLiteral("Hide models that can't be displayed (blocklisted or with no "
                                             "geometry — props, attachments, effect meshes)"));
    m_hideBrokenChk->setChecked(QSettings().value(QStringLiteral("models/hideUnrenderable"), false).toBool());
    m_hideUnrenderable = m_hideBrokenChk->isChecked();
    connect(m_hideBrokenChk, &QCheckBox::toggled, this, [this](bool on) {
        m_hideUnrenderable = on;
        QSettings().setValue(QStringLiteral("models/hideUnrenderable"), on);
        applyCategoryFilter();
        updateCount();
    });
    // List/Grid toggle + a discoverable Columns menu button.
    m_gridBtn = new QToolButton(left);
    m_gridBtn->setText(QStringLiteral("▦"));
    m_gridBtn->setCheckable(true);
    m_gridBtn->setToolTip(QStringLiteral("Toggle thumbnail (grid) view"));
    connect(m_gridBtn, &QToolButton::toggled, this, [this](bool on) {
        setGridView(on);
        // Keep the header's display-mode dropdown honest when the grid is toggled directly.
        const int want = on ? 2 : (m_displayMode == 2 ? 1 : m_displayMode);
        if (want != m_displayMode) applyDisplayMode(want);
    });
    m_colBtn = new QToolButton(left);
    m_colBtn->setText(QStringLiteral("Columns ▾"));
    m_colBtn->setToolTip(QStringLiteral("Show / hide list columns (also: right-click a column header)"));
    connect(m_colBtn, &QToolButton::clicked, this, [this]() {
        showColumnMenu(m_colBtn->mapToGlobal(QPoint(0, m_colBtn->height())));
    });
    // ("By item…" removed with the View row — openItemBrowser() remains for a future home.)
    m_variantsBtn = new QToolButton(left);
    m_variantsBtn->setText(QStringLiteral("Variants ▾"));
    m_variantsBtn->setToolTip(QStringLiteral("Jump to this model's skin variants (same actor)"));
    m_variantsBtn->setEnabled(false);
    connect(m_variantsBtn, &QToolButton::clicked, this, [this]() { showVariantsMenu(); });
    // ── Collapsible "Filters" section: category / class / gender / type + decrypt + hide-broken. ──
    auto* filterSection = new QWidget(left);
    auto* fsl = new QHBoxLayout(filterSection);
    fsl->setContentsMargins(0, 2, 0, 2);
    fsl->setSpacing(4);
    fsl->addWidget(m_catCombo, 1);
    fsl->addWidget(m_classCombo, 1);
    fsl->addWidget(m_genderCombo, 1);
    fsl->addWidget(m_typeCombo, 1);
    fsl->addWidget(m_onlyDecrypted);
    fsl->addWidget(m_hideBrokenChk);
    fsl->addWidget(m_clearBtn);
    filterSection->setVisible(false);

    // ── Collapsible "View" section: grouping / icons / grid / columns / multi-select / browse +
    //    the export-include toggles (Tex/Anim). ──
    // ── The old "View" row is gone (Blender-style condensation) ─────────────────────────────
    // Group by + Icons moved into the display-mode dropdown; Columns lives in the header
    // right-click; multi-select is now ALWAYS on (Ctrl/Shift-click, no mode); the export
    // Include toggles live in Export settings (these checkboxes mirror them and stay as hidden
    // state-holders); "By item…" was removed; Variants moved to the model context menu.
    // Everything kept alive below is a child of `left` with NO layout slot — without hide()
    // each would paint at the widget's top-left corner.
    for (QWidget* orphan : std::initializer_list<QWidget*>{
             m_gridBtn, m_colBtn, m_variantsBtn, m_multiSelect, m_exportTex, m_exportAnim})
        if (orphan) orphan->hide();

    // ── Always-visible main row: search fields + Filters/View toggles + count. ──
    m_filtersToggle = new QToolButton(left);
    m_filtersToggle->setText(QStringLiteral("Filters ▾"));
    m_filtersToggle->setCheckable(true);
    m_filtersToggle->setToolTip(QStringLiteral("Show the category / class / gender / type filters"));
    connect(m_filtersToggle, &QToolButton::toggled, this, [this, filterSection](bool on) {
        filterSection->setVisible(on);
        m_filtersToggle->setText(on ? QStringLiteral("Filters ▴") : QStringLiteral("Filters ▾"));  // arrow now
        updateCount();   // …then updateCount() refines it to "Filters (N) ▾/▴" once the list is ready
    });
    // (The "View ▾" toggle is gone with its row; the Filters toggle survives hidden because
    // updateCount still writes its badge text and the funnel panel links its checkboxes.)
    m_filtersToggle->hide();

    // Active-filter chips: one removable pill per set filter, shown inline so you can see and drop
    // filters at a glance without opening the section. Rebuilt by rebuildFilterChips() (from updateCount).
    m_filterChips = new QWidget(left);
    auto* chipLay = new QHBoxLayout(m_filterChips);
    chipLay->setContentsMargins(0, 0, 0, 0);
    chipLay->setSpacing(4);

    // ONE-BAR TOP: the old main row is gone — chips + count join the header bar below.
    // The SNO / NAME / COLLECTION fields are hidden state-holders the smart search parses into,
    // like the filter combos.
    m_snoSearch->hide();
    m_search->hide();
    m_collSearch->hide();
    ll->addWidget(filterSection);   // stays collapsed — the funnel panel is its replacement UI
    // (No open-state restore: both toggles are retired; filterSection's combos live on hidden
    // as the single source of truth the funnel panel and chips drive.)

    m_listModel = new SnoListModel(this);
    m_listModel->setModelsColumns(true);   // SNO | Icon | FILENAME | NAME | COLLECTION
    // Blender-style outliner: the browse list IS the scene tree. The wrapper mirrors the flat
    // SnoListModel 1:1 at top level (same rows/columns — entryAt(row) stays valid for any index
    // whose parent() is invalid) and hangs the loaded model's subtree (animations, armature,
    // parts → material → texture groups) off its row. See ModelOutliner.h.
    m_treeModel = new ModelOutlinerModel(m_listModel, this);
    m_list = new OutlinerView(left);   // QTreeView + stylesheet-proof branch arrows
    m_list->setModel(m_treeModel);
    m_list->setItemDelegate(new OutlinerDelegate(m_list));   // part rows: Blender-style eye toggle
    m_list->setSortingEnabled(true);
    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setUniformRowHeights(true);   // required: 67k rows — per-row measuring would crawl
    m_list->setIndentation(14);
    m_list->setRootIsDecorated(true);
    m_listBaseFont = m_list->font();      // remembered so Outliner/Grid keep the normal font
    // Blender-style zebra striping — alternating row backgrounds make long flat stretches
    // scannable. The group-header rows' own BackgroundRole still wins over the stripe.
    m_list->setAlternatingRowColors(true);
    // Compact/dense rows: zero item padding so the row height is driven purely by the icon/font,
    // plus a slim overlay-style scrollbar that reclaims width from the fat default one.
    m_list->setStyleSheet(QStringLiteral(
        "QTreeView{alternate-background-color:#282828;}"
        "QTreeView::item{padding:0px;margin:0px;}"
        "QScrollBar:vertical{width:8px;background:transparent;margin:0px;}"
        "QScrollBar::handle:vertical{background:#4a4a4a;border-radius:4px;min-height:24px;}"
        "QScrollBar::handle:vertical:hover{background:#606060;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}"
        "QScrollBar:horizontal{height:8px;background:transparent;margin:0px;}"
        "QScrollBar::handle:horizontal{background:#4a4a4a;border-radius:4px;min-width:24px;}"
        "QScrollBar::handle:horizontal:hover{background:#606060;}"
        "QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{width:0px;}"
        "QScrollBar::add-page:horizontal,QScrollBar::sub-page:horizontal{background:transparent;}"));
    // (The compact rows / small condensed font / icon-less columns are applied per display mode in
    //  applyListDensity() — List only; Outliner and Grid keep their normal look.)
    // The expander/hierarchy lives in FILENAME — SNO (col 0) is far too narrow to indent into.
    m_list->setTreePosition(ModelOutlinerModel::kTreeCol);
    m_list->setColumnWidth(0, 64);    // SNO
    m_list->setColumnWidth(1, 52);    // Icon
    m_list->setColumnWidth(2, 190);   // FILENAME (also the stretch column — see below)
    m_list->setColumnWidth(3, 150);   // NAME
    m_list->setColumnWidth(4, 130);   // COLLECTION
    m_list->setIconSize(QSize(m_iconPx, m_iconPx));
    m_list->setWordWrap(false);
    m_list->setTextElideMode(Qt::ElideRight);   // long names get an ellipsis instead of overflowing
    // Right-click the header (or use the Columns button) to show/hide columns.
    m_list->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list->header(), &QWidget::customContextMenuRequested, this,
            [this](const QPoint& p) { showColumnMenu(m_list->header()->mapToGlobal(p)); });
    // Persist + restore column sizes, order, visibility, and sort order.
    QHeaderView* hh = m_list->header();
    hh->restoreState(QSettings().value(QStringLiteral("models/listHeader")).toByteArray());
    // FILENAME is the primary column — let it fill the remaining width so long names read fully and
    // COLLECTION is never cut off the right edge. (Applied after restore so it always wins.)
    hh->setSectionResizeMode(2, QHeaderView::Stretch);
    hh->setStretchLastSection(false);
    // SNO reads better beside FILENAME than stranded left of the icon. Visual move only — the
    // LOGICAL column order is load-bearing (entryAt, grid column 2, tree position), so it never
    // changes. One-time migration: applied once, then the user's own column drags always win.
    if (!QSettings().value(QStringLiteral("models/snoAfterFilename"), false).toBool()) {
        hh->moveSection(hh->visualIndex(0), hh->visualIndex(2));   // SNO → right after FILENAME
        QSettings().setValue(QStringLiteral("models/snoAfterFilename"), true);
        // Persist now — the save-on-change connects don't exist yet, and the flag must never
        // outlive an unsaved header state (that combination would silently undo the move).
        QSettings().setValue(QStringLiteral("models/listHeader"), hh->saveState());
    }
    auto saveHdr = [hh]() {
        QSettings().setValue(QStringLiteral("models/listHeader"), hh->saveState());
    };
    connect(hh, &QHeaderView::sectionResized, this, [saveHdr](int, int, int) { saveHdr(); });
    connect(hh, &QHeaderView::sectionMoved, this, [saveHdr](int, int, int) { saveHdr(); });
    connect(hh, &QHeaderView::sortIndicatorChanged, this,
            [saveHdr](int, Qt::SortOrder) { saveHdr(); });
    CsvCopy::install(m_list);
    // Right-click: icon column → image actions (copy/save/render); other columns →
    // name + export actions. Both honor the current multi-selection.
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        const QModelIndex hit = m_list->indexAt(p);
        // ── Subtree nodes get their own menus ──
        if (hit.isValid() && hit.parent().isValid() && m_treeModel) {
            // Texture leaf / channel tile: Copy + Save the decoded image.
            const auto* n = m_treeModel->node(hit);
            QString xName; int xSno = -1; int xChan = -1; QString xLabel;
            if (n && n->kind == ModelOutlinerModel::Texture && !n->aux.isEmpty()) {
                xName = n->aux; xSno = int(n->hash);
            } else if (n && (n->kind == ModelOutlinerModel::MatTiles
                             || n->kind == ModelOutlinerModel::TexTiles) && !n->tiles.isEmpty()) {
                constexpr int step = ModelOutlinerModel::kTilePx + ModelOutlinerModel::kTileGap;
                const QRect vr = m_list->visualRect(hit.siblingAtColumn(ModelOutlinerModel::kTreeCol));
                const int t = (p.x() - vr.left()) / step;
                if (p.x() >= vr.left() && t >= 0 && t < n->tiles.size()) {
                    xName = n->tiles[t].first; xSno = n->tiles[t].second;
                    xLabel = n->tileLabels.value(t);
                    if (n->kind == ModelOutlinerModel::TexTiles && t > 0) xChan = t - 1;
                }
            }
            if (xSno > 0 && !xName.isEmpty()) {
                QMenu menu(this);
                auto image = [this, xName, xSno, xChan]() -> QImage {
                    const QImage img = decodeTexImage(xName, xSno);
                    return xChan >= 0 ? channelImage(img, xChan) : img;
                };
                const QString sfx2 = xChan >= 0 ? QStringLiteral("_%1").arg(xLabel) : xLabel.isEmpty()
                                     ? QString() : QStringLiteral("_%1").arg(xLabel);
                menu.addAction(QStringLiteral("Copy image"), this, [image]() {
                    const QImage img = image();
                    if (!img.isNull()) QApplication::clipboard()->setImage(img);
                });
                // Mirrors the model export pair: bare = straight to the last folder, "to…" = pick
                // one (and that pick becomes the new last folder).
                menu.addAction(QStringLiteral("Save image"), this, [this, image, xName, sfx2]() {
                    const QImage img = image();
                    if (img.isNull()) return;
                    QString dir = QSettings().value(QStringLiteral("models/lastImageSaveDir")).toString();
                    if (dir.isEmpty() || !QDir(dir).exists()) {   // first use: fall back to a prompt
                        dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Save image to…"));
                        if (dir.isEmpty()) return;
                        QSettings().setValue(QStringLiteral("models/lastImageSaveDir"), dir);
                    }
                    img.save(QDir(dir).filePath(QStringLiteral("%1%2.png").arg(xName, sfx2)), "PNG");
                });
                menu.addAction(QStringLiteral("Save image to…"), this, [this, image, xName, sfx2]() {
                    const QImage img = image();
                    if (img.isNull()) return;
                    const QString fn = QFileDialog::getSaveFileName(
                        this, QStringLiteral("Save texture"),
                        QStringLiteral("%1%2.png").arg(xName, sfx2),
                        QStringLiteral("PNG image (*.png)"));
                    if (fn.isEmpty()) return;
                    img.save(fn, "PNG");
                    QSettings().setValue(QStringLiteral("models/lastImageSaveDir"),
                                         QFileInfo(fn).absolutePath());
                });
                menu.exec(m_list->viewport()->mapToGlobal(p));
                return;
            }
            // Part nodes: Blender-style isolate tools.
            const QList<int> parts = m_treeModel->partsUnder(hit);
            if (parts.isEmpty()) return;   // bones/looks/groups: no menu (yet)
            QMenu menu(this);
            auto applyAll = [this](const std::function<bool(int, bool)>& want) {
                QHash<int, bool> all;   // prim → currently checked
                m_treeModel->partChecks(all);
                for (auto it = all.constBegin(); it != all.constEnd(); ++it)
                    m_treeModel->setPartCheck(it.key(), want(it.key(), it.value()));
                recomputePartVisibility();   // silent setters → exactly one recompute
            };
            menu.addAction(QStringLiteral("Solo"), this, [applyAll, parts]() {
                applyAll([&parts](int prim, bool) { return parts.contains(prim); });
            });
            menu.addAction(QStringLiteral("Show all"), this, [applyAll]() {
                applyAll([](int, bool) { return true; });
            });
            menu.addAction(QStringLiteral("Invert"), this, [applyAll]() {
                applyAll([](int, bool on) { return !on; });
            });
            menu.exec(m_list->viewport()->mapToGlobal(p));
            return;
        }
        const QList<int> snos = contextSnos(p);
        if (snos.isEmpty()) return;
        const QPoint gp = m_list->viewport()->mapToGlobal(p);
        const int n = snos.size();
        const QString sfx = n > 1 ? QStringLiteral("s") : QString();
        QMenu menu(this);

        if (hit.column() == 1) {   // ── Icon column: image actions ──
            addRowImageActions(menu, snos);
            menu.exec(gp);
            return;
        }

        addRowExportCopyActions(menu, snos);
        menu.exec(gp);
    });

    // ── Thumbnail-grid view — an alternate icon-mode layout over the SAME model + selection ──
    m_gridView = new QListView(left);
    m_gridView->setModel(m_treeModel);   // same wrapper as the tree → the selection model is shareable
                                         // (QListView only shows root-level rows, so no subtree leaks in)
    m_gridView->setModelColumn(2);                    // FILENAME (icon + caption in grid mode)
    m_gridView->setSelectionModel(m_list->selectionModel());   // share selection with the table
    m_gridView->setViewMode(QListView::IconMode);
    m_gridView->setResizeMode(QListView::Adjust);     // reflow icons on resize
    m_gridView->setMovement(QListView::Static);
    m_gridView->setUniformItemSizes(true);
    m_gridView->setWordWrap(false);
    m_gridView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_gridView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_gridView->setSpacing(6);
    m_gridView->setLayoutMode(QListView::SinglePass);   // lay out once; Batched reflows + resets scroll
    m_gridView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);   // items wrap; never widen
    // Keep the vertical scrollbar always visible so the usable width (and therefore the column
    // count) stays constant — otherwise the bar appearing/disappearing reflows the items and looks
    // like the panel is resizing.
    m_gridView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    // Don't advertise a preferred width, so a relayout can't nudge the splitter and make the left
    // panel briefly resize.
    m_gridView->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    m_gridPx = qBound(48, QSettings().value(QStringLiteral("models/gridPx"), 88).toInt(), 256);
    {
        const int g = m_gridPx;
        m_gridView->setIconSize(QSize(g, g));
        m_gridView->setGridSize(QSize(g + 26, g + 34));   // match the delegate's sizeHint
        m_gridView->setItemDelegate(new GridItemDelegate(g, m_gridView));   // square icon + elided caption
    }
    m_gridView->viewport()->setMouseTracking(true);     // hover previews need move events
    m_gridView->viewport()->installEventFilter(this);   // hover preview + Ctrl+scroll resize
    // Right-click the grid reuses the table's row context menu (name / export actions).
    m_gridView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_gridView, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        const QModelIndex hit = m_gridView->indexAt(p);
        if (!hit.isValid()) return;
        const SnoEntry* e = m_listModel->entryAt(hit.row());
        if (!e) return;
        // PARITY: the grid is the same model + selection as the list, so it gets the same actions.
        // Use the multi-selection when the clicked tile is part of it, exactly like the list does;
        // a grid tile is entirely icon, so BOTH the image actions and the export/copy set apply.
        QList<int> snos = contextSnos(p);
        if (snos.isEmpty()) snos = { e->snoId };
        const int sno = e->snoId;
        QMenu menu(this);
        menu.addAction(QStringLiteral("Load / preview"), this, [this, sno]() { selectModelBySno(sno); });
        menu.addSeparator();
        addRowImageActions(menu, snos);
        menu.addSeparator();
        addRowExportCopyActions(menu, snos);
        menu.exec(m_gridView->viewport()->mapToGlobal(p));
    });

    m_viewStack = new QStackedWidget(left);
    m_viewStack->addWidget(m_list);       // 0 = table
    m_viewStack->addWidget(m_gridView);   // 1 = grid
    // ── Blender-outliner header: [filter] [search] [display mode] directly above the tree ──
    // These are LINKED VIEWS of existing controls, not new state: the funnel mirrors the
    // "Filters ▾" toggle, the search box mirrors the Name/#tag box (two-way, no-op guarded),
    // and the display dropdown drives the same grid toggle the View section uses.
    {
        auto* hdrRow = new QHBoxLayout();
        hdrRow->setSpacing(3);
        auto* funnel = new QToolButton(left);
        {
            QPixmap pm(14, 14); pm.fill(Qt::transparent);
            QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen); p.setBrush(QColor(190, 180, 150));
            p.drawPolygon(QPolygonF({{1.5, 2.0}, {12.5, 2.0}, {8.5, 7.0}, {8.5, 12.0},
                                     {5.5, 10.0}, {5.5, 7.0}}));
            funnel->setIcon(QIcon(pm));
        }
        funnel->setIconSize(QSize(14, 14));
        funnel->setFixedSize(28, kBarH);
        funnel->setStyleSheet(QLatin1String(kIconBtnQss));   // icon-only: no 8px side padding
        funnel->setCursor(Qt::PointingHandCursor);
        // Blender-style filter PANEL (not a QMenu — menus close on every click, which makes
        // multi-tag selection miserable; this popup stays open until you click elsewhere).
        // Search + Clear on top, the match/data toggles, then scrollable tag groups filled on
        // meta-ready.
        funnel->setToolTip(QStringLiteral("Filter by tags — select any number; results must match all"));
        m_tagPanel = new QFrame(this, Qt::Popup);
        m_tagPanel->setObjectName(QStringLiteral("tagPanel"));
        m_tagPanel->setStyleSheet(QStringLiteral(   // = kPanelQss, scoped to this frame
            "QFrame#tagPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
            "QLabel{color:#cccccc;} QCheckBox{color:#cccccc;}"));
        auto* tpl = new QVBoxLayout(m_tagPanel);
        tpl->setContentsMargins(10, 8, 10, 8);
        tpl->setSpacing(5);
        auto* topRow = new QHBoxLayout();
        auto* tagSearch = new QLineEdit(m_tagPanel);
        tagSearch->setPlaceholderText(QStringLiteral("Search tags…"));
        tagSearch->setClearButtonEnabled(true);
        topRow->addWidget(tagSearch, 1);
        auto* clearBtn = new QPushButton(QStringLiteral("Clear"), m_tagPanel);
        clearBtn->setToolTip(QStringLiteral("Uncheck every tag"));
        topRow->addWidget(clearBtn);
        tpl->addLayout(topRow);
        auto* orChk = new QCheckBox(QStringLiteral("Match any tag (OR)"), m_tagPanel);
        orChk->setToolTip(QStringLiteral("Off: results carry ALL selected tags (narrowing).\n"
                                         "On: results carry AT LEAST ONE (widening)."));
        orChk->setChecked(QSettings().value(QStringLiteral("models/tagOrMode"), false).toBool());
        connect(orChk, &QCheckBox::toggled, this, [this](bool on) {
            m_tagOrMode = on;
            QSettings().setValue(QStringLiteral("models/tagOrMode"), on);
            applyCategoryFilter();
            updateCount();
            updateTagButtonTint();
        });
        // Data toggles — LINKED VIEWS of the filter-row checkboxes (setChecked no-ops break cycles).
        // Returned (not auto-added) so they can be laid out in compact horizontal rows — matching
        // the Textures funnel, which packs its toggles side-by-side instead of one-per-line.
        auto linkChk = [&](const QString& label, QCheckBox* master) -> QCheckBox* {
            auto* c = new QCheckBox(label, m_tagPanel);
            c->setChecked(master->isChecked());
            connect(c, &QCheckBox::toggled, master, &QCheckBox::setChecked);
            connect(master, &QCheckBox::toggled, c, &QCheckBox::setChecked);
            return c;
        };
        auto* remChk = new QCheckBox(QStringLiteral("Remember filters"), m_tagPanel);
        remChk->setToolTip(QStringLiteral("Restore the search box, selected tags, match mode and "
                                          "every filter exactly as you left them when the tool re-opens"));
        remChk->setChecked(QSettings().value(QStringLiteral("models/rememberSearch"), false).toBool());
        connect(remChk, &QCheckBox::toggled, this, [this](bool on) {
            QSettings().setValue(QStringLiteral("models/rememberSearch"), on);
            if (on) saveFilterState();   // capture the CURRENT state immediately, not on next change
        });
        {
            auto* r = new QHBoxLayout();
            r->addWidget(linkChk(QStringLiteral("Only decrypted"), m_onlyDecrypted));
            r->addWidget(linkChk(QStringLiteral("Hide un-renderable"), m_hideBrokenChk));
            r->addWidget(remChk);
            r->addStretch(1);
            tpl->addLayout(r);
        }
        // Special pseudo-filters (from the retired category dropdown). They drive the HIDDEN
        // combo — the single source of truth the predicate, chips and badge already read — so
        // they're mutually exclusive by construction, and the combo→checkbox sync below keeps
        // them honest when a chip clears the filter. Two-column grid, like the Textures Category.
        {
            auto* spLbl = new QLabel(QStringLiteral("Special"), m_tagPanel);
            spLbl->setStyleSheet(QLatin1String(kHdrQss));
            tpl->addWidget(spLbl);
            // Creature / Gear were dropped — they duplicate the Category tags "Monster" / "Item".
            // What's left is unique (animation-, usage- or update-based, not a tag).
            static const struct { const char* label; const char* data; } kSpecial[] = {
                {"Latest (new this update)",         "__latest__"},
                {"Animated (owns / inherits clips)", "__animated__"},
                {"Rigged (on a base rig)",           "__rigged__"},
                {"Orphaned (no actor uses it)",      "__orphaned__"}};
            auto pairs = std::make_shared<QVector<QPair<QCheckBox*, QString>>>();
            auto* grid = new QGridLayout();
            grid->setContentsMargins(0, 0, 0, 0);
            grid->setHorizontalSpacing(14);
            grid->setVerticalSpacing(2);
            int gi = 0;
            for (const auto& sp : kSpecial) {
                auto* c = new QCheckBox(QString::fromLatin1(sp.label), m_tagPanel);
                const QString data = QString::fromLatin1(sp.data);
                c->setChecked(m_catCombo->currentData().toString() == data);
                connect(c, &QCheckBox::toggled, this, [this, data](bool on) {
                    const int i = m_catCombo->findData(data);
                    if (on && i >= 0)
                        m_catCombo->setCurrentIndex(i);           // → applyCategoryFilter + chips
                    else if (!on && m_catCombo->currentData().toString() == data)
                        m_catCombo->setCurrentIndex(0);
                });
                pairs->append(qMakePair(c, data));
                grid->addWidget(c, gi / 2, gi % 2);
                ++gi;
            }
            tpl->addLayout(grid);
            // Combo → checkboxes: covers chip removal, sibling exclusivity, and programmatic
            // resets — each box simply re-derives from the one source of truth.
            connect(m_catCombo, &QComboBox::currentIndexChanged, this, [this, pairs](int) {
                const QString cur = m_catCombo->currentData().toString();
                for (const auto& pr : *pairs) {
                    QSignalBlocker b(pr.first);
                    pr.first->setChecked(pr.second == cur);
                }
            });
        }
        // View — sort + multi-select on one row (mirrors the Textures funnel's View section).
        {
            auto* vLbl = new QLabel(QStringLiteral("View"), m_tagPanel);
            vLbl->setStyleSheet(QLatin1String(kHdrQss));
            tpl->addWidget(vLbl);
            auto* r = new QHBoxLayout();
            r->addWidget(new QLabel(QStringLiteral("Sort"), m_tagPanel));
            auto* sortCombo = new QComboBox(m_tagPanel);
            sortCombo->addItem(QStringLiteral("Name"),       2);   // FILENAME column
            sortCombo->addItem(QStringLiteral("SNO"),        0);
            sortCombo->addItem(QStringLiteral("Collection"), 4);
            const int savedCol = QSettings().value(QStringLiteral("models/sortCol"), 2).toInt();
            { const int i = sortCombo->findData(savedCol); if (i >= 0) { QSignalBlocker b(sortCombo); sortCombo->setCurrentIndex(i); } }
            connect(sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                    [this, sortCombo](int) {
                        if (!m_list) return;
                        const int col = sortCombo->currentData().toInt();
                        m_list->sortByColumn(col, Qt::AscendingOrder);
                        QSettings().setValue(QStringLiteral("models/sortCol"), col);
                    });
            r->addWidget(sortCombo, 1);
            r->addWidget(linkChk(QStringLiteral("Multi select"), m_multiSelect));
            tpl->addLayout(r);
        }
        // Tags — the AND/OR toggle sits on the header row, then the scrollable tag groups.
        {
            auto* r = new QHBoxLayout();
            auto* tLbl = new QLabel(QStringLiteral("Tags"), m_tagPanel);
            tLbl->setStyleSheet(QLatin1String(kHdrQss));
            r->addWidget(tLbl);
            r->addStretch(1);
            r->addWidget(orChk);
            tpl->addLayout(r);
        }
        auto* scroll = new QScrollArea(m_tagPanel);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setFixedHeight(320);
        scroll->setMinimumWidth(240);
        m_tagPanelBody = new QWidget(scroll);
        auto* bodyLay = new QVBoxLayout(m_tagPanelBody);
        bodyLay->setContentsMargins(0, 0, 0, 0);
        bodyLay->setSpacing(3);
        bodyLay->addWidget(new QLabel(QStringLiteral("Tags load with the index…"), m_tagPanelBody));
        bodyLay->addStretch(1);
        scroll->setWidget(m_tagPanelBody);
        tpl->addWidget(scroll, 1);
        connect(clearBtn, &QPushButton::clicked, this, [this]() {
            m_tagFilter.clear();
            for (QCheckBox* c : std::as_const(m_tagChecks)) {   // quietly, then filter once
                QSignalBlocker b(c);
                c->setChecked(false);
            }
            applyCategoryFilter();
            updateCount();
            updateTagButtonTint();
        });
        connect(tagSearch, &QLineEdit::textChanged, this, [this](const QString& t) {
            const QString needle = t.trimmed();
            const QList<QWidget*> groups =
                m_tagPanelBody->findChildren<QWidget*>(QStringLiteral("tagGroup"),
                                                       Qt::FindDirectChildrenOnly);
            for (QWidget* g : groups) {
                int vis = 0;
                for (QCheckBox* c : g->findChildren<QCheckBox*>()) {
                    const bool hit = needle.isEmpty()
                                     || c->text().contains(needle, Qt::CaseInsensitive);
                    c->setVisible(hit);
                    if (hit) ++vis;
                }
                g->setVisible(vis > 0);
            }
        });
        connect(funnel, &QToolButton::clicked, this, [this, funnel]() {
            if (!m_tagPanel) return;
            m_tagPanel->adjustSize();
            m_tagPanel->move(funnel->mapToGlobal(QPoint(0, funnel->height() + 2)));
            m_tagPanel->show();
        });
        m_tagBtn = funnel;   // tinted red while a tag filter is active
        hdrRow->addWidget(funnel);
        // ONE smart search box (the SNO / NAME / COLLECTION fields live on hidden — this parses
        // into them, so every existing filter connect is the unchanged code path). TOKENIZED:
        // "c:" reads to the end of the line (collection names contain spaces — put it last),
        // any all-digit token is the SNO, everything else joins into the name search. The text
        // colors itself when the whole query is a single kind; mixed queries stay default (the
        // chips show the decomposition).
        m_hdrSearch = new QLineEdit(left);
        m_hdrSearch->setPlaceholderText(QStringLiteral("Search…   (c: collection · #tag · digits = SNO)"));
        m_hdrSearch->setClearButtonEnabled(true);
        m_hdrSearch->setFixedHeight(kBarH);   // aligns with the funnel/display buttons beside it
        m_hdrSearch->setToolTip(QStringLiteral(
            "Smart search — combine freely, space-separated:\n"
            "  text — filename / name / tags        (default color)\n"
            "  #tag — exact tag match               (green)\n"
            "  2642029 or #2642029 — SNO            (gold)\n"
            "  c:text — collection contains, reads to END of line — put it last   (blue)\n"
            "e.g.   barb 260 c:the soulstained\n"
            "Ctrl+F focuses · Esc clears · ↓ recalls recent searches"));
        connect(m_hdrSearch, &QLineEdit::textChanged, this, [this](const QString& raw) {
            QString t = raw.trimmed();
            QString sno, coll;
            QStringList nameParts;
            static const QRegularExpression kColl(QStringLiteral("(?:^|\\s)c:(.*)$"),
                                                  QRegularExpression::CaseInsensitiveOption);
            static const QRegularExpression kSno(QStringLiteral("^#?\\d+$"));
            const QRegularExpressionMatch cm = kColl.match(t);
            if (cm.hasMatch()) {
                coll = cm.captured(1).trimmed();
                t = t.left(cm.capturedStart(0)).trimmed();
            }
            for (const QString& tok : t.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                if (kSno.match(tok).hasMatch() && sno.isEmpty())
                    sno = tok.startsWith(QLatin1Char('#')) ? tok.mid(1) : tok;
                else
                    nameParts << tok;
            }
            const QString name = nameParts.join(QLatin1Char(' '));
            const int kinds = int(!sno.isEmpty()) + int(!coll.isEmpty()) + int(!name.isEmpty());
            QString color = QStringLiteral("#dddddd");
            if (kinds == 1) {
                if (!sno.isEmpty())       color = QStringLiteral("#d8a23a");   // SNO — gold
                else if (!coll.isEmpty()) color = QStringLiteral("#8ab4f8");   // collection — blue
                else if (name.startsWith(QLatin1Char('#')))
                                          color = QStringLiteral("#6ee7a0");   // tag — green
            }
            m_hdrSearch->setStyleSheet(QStringLiteral("QLineEdit{color:%1;}").arg(color));
            // Route through the hidden fields — their textChanged connects ARE the filters.
            if (m_snoSearch && m_snoSearch->text() != sno)   m_snoSearch->setText(sno);
            if (m_collSearch && m_collSearch->text() != coll) m_collSearch->setText(coll);
            if (m_search && m_search->text() != name)         m_search->setText(name);
            saveFilterState();    // (no-op unless "Remember last search" is on)
            refreshHistPopup();   // typing filters the dropdown; emptying shows the full history
        });
        // Recent searches live in a CHILD-widget dropdown, not a QCompleter: Qt::Popup windows
        // (which QCompleter uses) grab the mouse and EAT the first click outside themselves —
        // with the popup auto-opening on focus, that swallowed clicks aimed at the right panel.
        // A NoFocus child list never grabs: clicks elsewhere land normally, and focus-out hides it.
        m_histList = new QListWidget(left);
        m_histList->setFocusPolicy(Qt::NoFocus);   // clicking an entry must NOT steal the edit's focus
        m_histList->setStyleSheet(QStringLiteral(   // popup tier: matches the tag/overlay panels
            "QListWidget{background:#232323;border:1px solid #5a5a5a;border-radius:4px;color:#cccccc;}"
            "QListWidget::item{padding:2px 6px;}"
            "QListWidget::item:hover{background:#2b2b2b;}"
            "QListWidget::item:selected{background:#8a1414;color:#ffffff;}"));
        m_histList->hide();
        connect(m_histList, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
            if (!it) return;
            m_hdrSearch->setText(it->text());
            m_histList->hide();
        });
        connect(m_hdrSearch, &QLineEdit::returnPressed, this, [this]() {
            // If a history row is highlighted (↓/↑), Enter applies it; otherwise commit the query.
            if (m_histList && m_histList->isVisible() && m_histList->currentItem()) {
                m_hdrSearch->setText(m_histList->currentItem()->text());
                m_histList->hide();
            }
            const QString q = m_hdrSearch->text().trimmed();
            if (q.isEmpty()) return;
            QStringList hist = QSettings().value(QStringLiteral("models/searchHistory")).toStringList();
            hist.removeAll(q);
            hist.prepend(q);
            while (hist.size() > 10) hist.removeLast();
            QSettings().setValue(QStringLiteral("models/searchHistory"), hist);
        });
        hdrRow->addWidget(m_hdrSearch, 1);
        // Ctrl+F focuses the search from anywhere in this tab; Esc inside it clears.
        auto* findSc = new QShortcut(QKeySequence::Find, this);
        findSc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(findSc, &QShortcut::activated, this, [this]() {
            m_hdrSearch->setFocus(Qt::ShortcutFocusReason);
            m_hdrSearch->selectAll();
        });
        m_hdrSearch->installEventFilter(this);   // Esc → clear (handled in eventFilter)
        m_displayBtn = new QToolButton(left);
        m_displayBtn->setPopupMode(QToolButton::InstantPopup);
        m_displayBtn->setFixedSize(28, kBarH);
        m_displayBtn->setIconSize(QSize(14, 14));
        m_displayBtn->setCursor(Qt::PointingHandCursor);
        m_displayBtn->setStyleSheet(QLatin1String(kIconBtnQss));   // icon-only
        m_displayBtn->setToolTip(QStringLiteral(
            "Display mode:\nList — flat rows only\nOutliner — expandable scene tree\nGrid — thumbnails"));
        auto* dm = new QMenu(m_displayBtn);
        dm->setStyleSheet(QStringLiteral("QMenu::indicator{width:0px;height:0px;}"));   // no stock box
        auto* dmGroup = new QActionGroup(dm);
        static const char* kModes[3] = {"List", "Outliner", "Grid"};
        for (int i = 0; i < 3; ++i) {
            QAction* a = dm->addAction(QIcon(displayModeGlyph(i)), QString::fromLatin1(kModes[i]));
            a->setCheckable(true);
            a->setActionGroup(dmGroup);
            connect(a, &QAction::triggered, this, [this, i]() { applyDisplayMode(i); });
        }
        // "Outliner shows" — subtree composition toggles. They belong to the Outliner display
        // mode, so applyDisplayMode() shows them only while Outliner is selected.
        m_kindActs.clear();
        m_kindActs << dm->addSection(QStringLiteral("Outliner shows"));
        static const struct { const char* key; const char* label; } kKinds[] = {
            {"looks",    "Looks"},          {"anims",   "Animations"},
            {"armature", "Armature"},       {"bones",   "Bones"},
            {"tiles",    "Channel tiles"},  {"textures","Textures"},
            {"values",   "Values"},         {"shaders", "Shaders"}};
        for (const auto& k : kKinds) {
            QAction* a = dm->addAction(QString::fromLatin1(k.label));
            a->setCheckable(true);
            const QString key = QStringLiteral("models/outliner/show_") + QLatin1String(k.key);
            a->setChecked(QSettings().value(key, true).toBool());
            bindCheckIcon(a);   // drawn gold checkmark — the stock indicator box is suppressed
            connect(a, &QAction::toggled, this, [this, key](bool on) {
                QSettings().setValue(key, on);
                if (m_curGeo.valid) buildOutlinerSubtree();   // recompose with the new gates
            });
            m_kindActs << a;
        }
        // Behavior toggle for the same mode: whether loading a model auto-opens its subtree.
        {
            QAction* a = dm->addAction(QStringLiteral("Auto-expand loaded model"));
            a->setCheckable(true);
            a->setChecked(QSettings().value(QStringLiteral("models/outliner/autoExpand"), true).toBool());
            bindCheckIcon(a);
            connect(a, &QAction::toggled, this, [](bool on) {
                QSettings().setValue(QStringLiteral("models/outliner/autoExpand"), on);
            });
            m_kindActs << a;   // Outliner-mode only, like the kind gates
        }
        // List options (all modes): the Group-by and Icon-style combos, re-homed from the old
        // View row. QWidgetAction reparents them into the menu; their existing connects live on.
        dm->addSection(QStringLiteral("List options"));
        auto addComboRow = [&](const QString& caption, QWidget* w) {
            auto* row = new QWidget(dm);
            auto* rowLay = new QHBoxLayout(row);
            rowLay->setContentsMargins(10, 2, 10, 2);
            rowLay->setSpacing(6);
            rowLay->addWidget(new QLabel(caption, row));
            rowLay->addWidget(w, 1);
            auto* wa = new QWidgetAction(dm);
            wa->setDefaultWidget(row);
            dm->addAction(wa);
        };
        addComboRow(QStringLiteral("Group by"), m_groupCombo);
        addComboRow(QStringLiteral("Icons"), m_iconModeCombo);
        m_displayBtn->setMenu(dm);
        hdrRow->addWidget(m_displayBtn);
        hdrRow->addWidget(m_filterChips);   // active-filter pills, inline
        hdrRow->addWidget(m_countLabel);    // "X of Y models"
        ll->addLayout(hdrRow);
    }
    ll->addWidget(m_viewStack, 1);
    setGridView(QSettings().value(QStringLiteral("models/gridView"), false).toBool());   // restore layout
    split->addWidget(left);

    // ════════ CENTER column: preview + info + export + timeline + animations ════════
    auto* center = new QWidget(split);
    auto* cl = new QVBoxLayout(center);
    cl->setContentsMargins(4, 4, 4, 4);

    auto* pvHead = new QHBoxLayout();
    // (No "MODEL PREVIEW" title — the viewport is self-evident and the row is better spent on
    //  controls. pvHead now only carries the hidden state-holder buttons.)
    pvHead->addStretch(1);
    m_autoLoad = QSettings().value(QStringLiteral("models/autoLoad"), true).toBool();
    m_autoLoadBtn = new QToolButton(center);
    m_autoLoadBtn->setText(QStringLiteral("Auto-Load"));
    m_autoLoadBtn->setCheckable(true);
    m_autoLoadBtn->setChecked(m_autoLoad);
    m_autoLoadBtn->setToolTip(QStringLiteral(   // hidden state-holder; the UI lives in Settings
        "When on, selecting a model loads its 3D preview. Turn off to browse/"
        "multi-select quickly; double-click a row to load on demand."));
    m_autoLoadBtn->setStyleSheet(QStringLiteral(
        "QToolButton{padding:2px 8px;border:1px solid #555;border-radius:3px;"
        "background:#2b2b2b;color:#bbb;}"
        "QToolButton:checked{background:#8a1414;color:#fff;border-color:#a01818;}"));
    connect(m_autoLoadBtn, &QToolButton::toggled, this, [this](bool on) {
        m_autoLoad = on;
        QSettings().setValue(QStringLiteral("models/autoLoad"), on);
    });
    // Graphics + Pigment are QPushButtons but must look identical to the QToolButton view
    // cluster they sit among — same neutral style, plus the red [panelOpen] open state.
    const QString viewBtnStyle = QStringLiteral(
        "QPushButton{padding:2px 8px;border:1px solid #555;border-radius:3px;background:#2b2b2b;color:#bbb;}"
        "QPushButton:hover{border-color:#b0453c;}"
        "QPushButton:pressed{background:#333333;}"
        "QPushButton[panelOpen=\"true\"]{background:#8a1414;color:#fff;border-color:#a01818;}");
    m_vpBtn = new QPushButton(QStringLiteral("Graphics"), center);
    m_vpBtn->setToolTip(QStringLiteral("Rendering quality: scene/shadows, shading, tonemap, background"));
    m_vpBtn->setCursor(Qt::PointingHandCursor);
    m_vpBtn->setStyleSheet(viewBtnStyle);
    connect(m_vpBtn, &QPushButton::clicked, this, [this]() { togglePreviewPanel(); });
    m_dyeBtn = new QPushButton(QStringLiteral("Pigment"), center);
    m_dyeBtn->setToolTip(QStringLiteral("Dye zones: recolour the model's dyeable materials (D4 pigments)"));
    m_dyeBtn->setCursor(Qt::PointingHandCursor);
    m_dyeBtn->setStyleSheet(viewBtnStyle);
    connect(m_dyeBtn, &QPushButton::clicked, this, [this]() { toggleDyePanel(); });
    // (The Rig popup and the old Reset/Reload buttons are gone entirely: rig toggles live in
    // Overlays ▾, reloading is re-selecting, and reset is selecting another model.)
    // Auto-Load left the header for Settings ▸ Models; the button stays as a HIDDEN state-holder
    // because the settings dialog and restore paths still drive it by objectName.
    m_autoLoadBtn->hide();
    // Wrapped in a container so "Maximize viewport" can hide the row as ONE unit — hiding the
    // children individually would clobber each button's own visibility (dev gating, etc.).
    // Everything in this row is now a HIDDEN state-holder (Auto-Load/Reset/Reload moved out, and
    // the MODEL PREVIEW title is gone), so the container itself is hidden: an empty row would
    // just eat vertical space above the viewport.
    m_pvHeadW = new QWidget(center);
    m_pvHeadW->setLayout(pvHead);
    m_pvHeadW->hide();
    cl->addWidget(m_pvHeadW);

    // View toggles: an always-visible toolbar of grouped checkable buttons
    // (replaces the old "View ▾" dropdown). Sections separated by thin rules:
    // Shading · Overlays · Submeshes · Camera.
    auto* viewBar = new QHBoxLayout();
    viewBar->setSpacing(3);
    viewBar->setContentsMargins(0, 2, 0, 2);
    auto mkToggle = [&](const QString& key, const QString& text, const QString& tip,
                        bool checked, std::function<void(bool)> slot) {
        auto* b = new QToolButton(center);
        b->setObjectName(key);   // settings key for "remember preview settings"
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        // Remember every viewport toggle (Flat / Wireframe / Grid / Skeleton / FX / SIM / Spin) across
        // sessions, restoring the saved state — matching the Graphics-panel checkboxes.
        const bool initial = QSettings().value(QStringLiteral("models/view/") + key, checked).toBool();
        b->setChecked(initial);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QLatin1String(kToolBtnQss));   // text buttons: FX / SIM / GIB
        b->setFixedHeight(kBarH);                       // = the icon buttons beside them
        connect(b, &QToolButton::toggled, this, [this, slot, key](bool on) {
            slot(on);
            QSettings().setValue(QStringLiteral("models/view/") + key, on);   // always remember
        });
        viewBar->addWidget(b);
        m_viewToggleBtns.append(b);
        slot(initial);   // apply the restored state (setChecked ran before connect, so fire it now)
    };
    auto sep = [&]() {
        auto* f = new QFrame(center);
        f->setFrameShape(QFrame::VLine);
        f->setStyleSheet(QStringLiteral("color:#444;"));
        viewBar->addWidget(f);
    };
    // Channel viewer (parity with Wardrobe): lit result, or one raw material channel.
    m_channelCombo = new QComboBox(center);
    m_channelCombo->addItems({QStringLiteral("Shaded"), QStringLiteral("Base Color"),
                              QStringLiteral("Normal"), QStringLiteral("Roughness"),
                              QStringLiteral("Metallic"), QStringLiteral("AO"),
                              QStringLiteral("Emissive")});
    m_channelCombo->setToolTip(QStringLiteral("View the lit result or one raw material channel (↑/↓ to scroll)"));
    m_channelCombo->setCursor(Qt::PointingHandCursor);
    m_channelCombo->setStyleSheet(QStringLiteral(
        "QComboBox{padding:2px 8px;border:1px solid #555;border-radius:3px;background:#2b2b2b;color:#bbb;}"
        "QComboBox:hover{border-color:#b0453c;}"
        "QComboBox QAbstractItemView{background:#2b2b2b;color:#dddddd;"
        "selection-background-color:#8a1414;selection-color:#ffffff;}"));
    m_channelCombo->setCurrentIndex(QSettings().value(QStringLiteral("models/view/channel"), 0).toInt());
    connect(m_channelCombo, &QComboBox::currentIndexChanged, this, [this](int i) {
        QSettings().setValue(QStringLiteral("models/view/channel"), i);
        if (m_modelView) m_modelView->setViewChannel(i);
    });
    // (m_channelCombo joins the shading ⌄ popover below as "Channel" — not added here.)
    // ── Skeleton toggle — HIDDEN state-holder. Three systems drive it by objectName (outliner
    // auto-skeleton, Rig-panel sync ×2); the visible control moved into Overlays ▾ below.
    mkToggle(QStringLiteral("skeleton"), QStringLiteral("Skeleton"),
             QStringLiteral("Show bones"), false,
             [this](bool on) { if (m_modelView) m_modelView->setShowSkeleton(on); });
    if (!m_viewToggleBtns.isEmpty()) m_viewToggleBtns.last()->hide();

    // ── Shading mode: Blender's four spheres — wire · flat · shaded · rendered. "Rendered"
    // additionally turns the post pipeline ON (IBL/shadows/SSAO/tonemap) and "Shaded" turns it
    // OFF — written through the Graphics panel's own settings keys, so its checkboxes (built
    // lazily from those keys) and the mode can never disagree.
    {
        auto* shadeGroup = new QButtonGroup(this);
        shadeGroup->setExclusive(true);
        auto mkShade = [&](int mode, const QString& tip) {
            auto* b = new QToolButton(center);
            b->setIcon(QIcon(shadeBallGlyph(mode)));
            b->setIconSize(QSize(20, 20));   // = the pixmap's own size: no up/down-scaling
            b->setToolTip(tip);
            b->setCheckable(true);
            b->setCursor(Qt::PointingHandCursor);
            b->setFixedSize(28, kBarH);
            b->setStyleSheet(QLatin1String(kIconBtnQss));
            shadeGroup->addButton(b, mode);
            viewBar->addWidget(b);
        };
        mkShade(0, QStringLiteral("Wireframe"));
        mkShade(1, QStringLiteral("Flat: base colour only"));
        mkShade(2, QStringLiteral("Shaded: PBR, post effects off"));
        mkShade(3, QStringLiteral("Rendered: PBR + IBL, shadows, SSAO, tonemap"));
        const int savedShade = qBound(0,
            QSettings().value(QStringLiteral("models/view/shadeMode"), 3).toInt(), 3);
        if (QAbstractButton* b = shadeGroup->button(savedShade)) b->setChecked(true);
        connect(shadeGroup, &QButtonGroup::idClicked, this, [this](int id) {
            QSettings s2;
            s2.setValue(QStringLiteral("models/view/shadeMode"), id);
            if (!m_modelView) return;
            m_modelView->setWireframe(id == 0);
            m_modelView->setPbr(id >= 2);
            if (id >= 2) {   // Shaded/Rendered own the post pipeline (keys = Graphics checkboxes')
                const bool post = (id == 3);
                s2.setValue(QStringLiteral("models/viewport/ibl"), post);
                s2.setValue(QStringLiteral("models/viewport/shadows"), post);
                s2.setValue(QStringLiteral("models/viewport/ssao"), post);
                s2.setValue(QStringLiteral("models/viewport/tonemap"), post);
                m_modelView->setFeatureIbl(post);
                m_modelView->setShadowEnabled(post);
                m_modelView->setSsaoEnabled(post);
                m_modelView->setFeatureTonemap(post);
            }
        });
        // ⌄ — shading popover holding the Channel combo. Crucially it's ALSO scrollable in place:
        // wheel over this button cycles channels live without opening anything (see eventFilter),
        // which is the fast way to flip Base Color → Normal → Roughness while inspecting.
        auto* shadeMore = new QToolButton(center);
        m_shadeMoreBtn = shadeMore;
        shadeMore->setText(QStringLiteral("⌄"));
        shadeMore->setPopupMode(QToolButton::InstantPopup);
        shadeMore->setFixedSize(18, kBarH);   // same slim arrow as the Overlays one
        shadeMore->setCursor(Qt::PointingHandCursor);
        shadeMore->setStyleSheet(QLatin1String(kArrowBtnQss));   // padding:0 → the ⌄ has room
        shadeMore->installEventFilter(this);   // wheel → cycle channel
        auto* sm2 = new QMenu(shadeMore);
        {
            auto* row = new QWidget(sm2);
            auto* rl2 = new QHBoxLayout(row);
            rl2->setContentsMargins(10, 4, 10, 4);
            rl2->setSpacing(6);
            rl2->addWidget(new QLabel(QStringLiteral("Channel"), row));
            rl2->addWidget(m_channelCombo, 1);   // reparents; its connect lives on
            auto* wa = new QWidgetAction(sm2);
            wa->setDefaultWidget(row);
            sm2->addAction(wa);
        }
        shadeMore->setMenu(sm2);
        viewBar->addWidget(shadeMore);
        // The button reports the live channel, so a non-default view is never a mystery.
        auto syncChannelBtn = [this]() {
            if (!m_shadeMoreBtn || !m_channelCombo) return;
            const int i = m_channelCombo->currentIndex();
            const QString ch = m_channelCombo->currentText();
            m_shadeMoreBtn->setText(i == 0 ? QStringLiteral("⌄") : QStringLiteral("◆"));
            m_shadeMoreBtn->setToolTip(
                QStringLiteral("Channel: %1\nScroll here to flip channels · click for the list").arg(ch));
        };
        syncChannelBtn();
        connect(m_channelCombo, &QComboBox::currentIndexChanged, this,
                [syncChannelBtn](int) { syncChannelBtn(); });
    }
    sep();
    // ── Overlays — Blender's split control: a SPHERE TOGGLE (master on/off for every guide) plus
    // an ARROW opening a persistent settings panel. One home for grid/axes and the whole rig/bone
    // overlay set (moved out of the dev-only Rig panel). The panel is a Qt::Popup QFrame, not a
    // QMenu, so ticking boxes doesn't close it — it stays until the arrow is clicked again or you
    // click outside. Each checkbox writes the same settings key + GL setter the panel used before.
    {
        auto* ovBtn = new QToolButton(center);
        m_overlayBtn = ovBtn;
        ovBtn->setIcon(QIcon(overlayGlyph()));   // shared Blender two-circle glyph (ViewGlyphs.h)
        ovBtn->setIconSize(QSize(20, 20));
        ovBtn->setToolTip(QStringLiteral("Show overlays (grid, axes, skeleton…) — master toggle"));
        ovBtn->setCursor(Qt::PointingHandCursor);
        ovBtn->setCheckable(true);
        ovBtn->setChecked(QSettings().value(QStringLiteral("models/view/overlays"), true).toBool());
        ovBtn->setFixedSize(28, kBarH);   // matches the shading balls it sits beside
        ovBtn->setStyleSheet(QLatin1String(kIconBtnQss));
        viewBar->addWidget(ovBtn);

        // The persistent settings panel.
        m_overlayPanel = new QFrame(this, Qt::Popup);
        m_overlayPanel->setObjectName(QStringLiteral("ovPanel"));
        m_overlayPanel->setStyleSheet(QStringLiteral(   // = kPanelQss, scoped to this frame
            "QFrame#ovPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
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
        // Each box: persist the key, push to GL. m_overlayChks drives the master toggle's gating.
        auto addOverlay = [&](const QString& label, const QString& key, bool def, bool indent,
                              const QString& tip, std::function<void(bool)> apply) {
            auto* cb = new QCheckBox(label, m_overlayPanel);
            if (indent) cb->setStyleSheet(QStringLiteral("QCheckBox{color:#cccccc;margin-left:16px;}"));
            if (!tip.isEmpty()) cb->setToolTip(tip);
            cb->setChecked(QSettings().value(key, def).toBool());
            connect(cb, &QCheckBox::toggled, this, [this, key, apply](bool on) {
                QSettings().setValue(key, on);
                if (m_overlaysOn) apply(on);   // master off → GL stays dark; re-applied on master on
            });
            opl->addWidget(cb);
            m_overlayChks.append({cb, apply});
            return cb;
        };

        ovSection(QStringLiteral("Guides"));
        addOverlay(QStringLiteral("Statistics"), QStringLiteral("models/view/stats"), false, false,
                   QStringLiteral("Live vert/tri/part/bone counts for the VISIBLE geometry."),
                   [this](bool on) {
                       if (m_statsOv) m_statsOv->setVisible(on);
                       if (on) updateStatsOverlay();
                   });
        addOverlay(QStringLiteral("Ground grid"), QStringLiteral("models/view/grid"), false, false,
                   QStringLiteral("Ground plane grid."),
                   [this](bool on) { if (m_modelView) m_modelView->setShowGrid(on); });
        addOverlay(QStringLiteral("Axis gizmo"), QStringLiteral("viewer/axisGizmo"), true, false,
                   QStringLiteral("Clickable X/Y/Z orientation ball in the viewport corner."),
                   [this](bool on) { if (m_modelView) m_modelView->setShowAxisGizmo(on); });
        addOverlay(QStringLiteral("Colored grid axes"), QStringLiteral("viewer/gridAxisColors"), true, true,
                   QStringLiteral("Tint the grid's world axes: X red, Z blue."),
                   [this](bool on) { if (m_modelView) m_modelView->setGridAxisColors(on); });

        ovSection(QStringLiteral("Skeleton"));
        // Skeleton — two-way linked to the HIDDEN legacy toggle (single source of truth that the
        // outliner's auto-skeleton also drives).
        {
            QToolButton* skelBtn = nullptr;
            for (QToolButton* b : m_viewToggleBtns)
                if (b->objectName() == QLatin1String("skeleton")) { skelBtn = b; break; }
            auto* cb = new QCheckBox(QStringLiteral("Skeleton"), m_overlayPanel);
            cb->setToolTip(QStringLiteral("Draw the bone hierarchy."));
            cb->setChecked(skelBtn && skelBtn->isChecked());
            if (skelBtn) {
                connect(cb, &QCheckBox::toggled, skelBtn, &QToolButton::setChecked);
                connect(skelBtn, &QToolButton::toggled, cb, &QCheckBox::setChecked);
            }
            opl->addWidget(cb);
            m_overlayChks.append({cb, [this](bool on) { if (m_modelView) m_modelView->setShowSkeleton(on); }});
        }
        addOverlay(QStringLiteral("Hardpoints"), QStringLiteral("models/rig/hardpoints"), false, false,
                   QStringLiteral("Draw the rig's attach sockets (weapon grips, sheaths, trail emitters, "
                                  "saddle…) as labeled XYZ gizmos — where held/attached models snap on."),
                   [this](bool on) { if (m_modelView) m_modelView->setShowHardpoints(on); });
        addOverlay(QStringLiteral("Collision model"), QStringLiteral("models/rig/colliders"), false, false,
                   QStringLiteral("Draw the cloth collision model — the authored capsules and plane "
                                  "colliders the cloth is solved against. Use it to see whether a "
                                  "garment is clipping because the capsules don't match the body."),
                   [this](bool on) { if (m_modelView) m_modelView->setShowColliders(on); });
        addOverlay(QStringLiteral("Physics bones"), QStringLiteral("models/rig/physBones"), false, false,
                   QStringLiteral("Overlay the cloth/physics bones (anchored grey, simulated orange)."),
                   [this](bool on) { if (m_modelView) m_modelView->setShowPhysBones(on); });
        addOverlay(QStringLiteral("Axis gizmos (per-bone)"), QStringLiteral("models/rig/axis"), true, true,
                   QStringLiteral("Per-bone XYZ rotation gizmo (R/G/B)."),
                   [this](bool on) { if (m_modelView) m_modelView->setShowPhysAxes(on); });
        addOverlay(QStringLiteral("Bone names"), QStringLiteral("models/rig/boneNames"), false, false,
                   QStringLiteral("Label each bone at its position in the viewport."),
                   [this](bool on) { if (m_modelView) m_modelView->setShowBoneNames(on); });
        addOverlay(QStringLiteral("Translated names"), QStringLiteral("models/rig/boneNamesTranslated"), false, true,
                   QStringLiteral("Readable labels from verified D4 hardpoint/IK data; others keep bone_<hash>."),
                   [this](bool on) { if (m_modelView) m_modelView->setBoneNamesTranslated(on); });
        addOverlay(QStringLiteral("Hide unnamed bones"), QStringLiteral("models/rig/boneNamesHideUnknown"), false, true,
                   QStringLiteral("Only label bones with a known/translated name."),
                   [this](bool on) { if (m_modelView) m_modelView->setBoneNamesHideUnknown(on); });
        auto* ovNote = new QLabel(QStringLiteral(
            "<span style='color:#888'>Identified bones draw green; others show bone_&lt;hash&gt;.</span>"),
            m_overlayPanel);
        ovNote->setWordWrap(true);
        ovNote->setMinimumWidth(230);
        opl->addWidget(ovNote);

        // Master toggle: all guides off at once, remembering each box's own state.
        m_overlaysOn = ovBtn->isChecked();
        connect(ovBtn, &QToolButton::toggled, this, [this](bool on) {
            m_overlaysOn = on;
            QSettings().setValue(QStringLiteral("models/view/overlays"), on);
            reapplyOverlays();   // off = force-off; on = restore each box (and the rig flags)
            if (m_overlayPanel) m_overlayPanel->setEnabled(on);
        });
        m_overlayPanel->setEnabled(m_overlaysOn);

        // ▾ — opens/closes the panel (Blender's arrow beside the overlay toggle).
        auto* ovArrow = new QToolButton(center);
        ovArrow->setText(QStringLiteral("⌄"));
        ovArrow->setToolTip(QStringLiteral("Overlay settings"));
        ovArrow->setCursor(Qt::PointingHandCursor);
        ovArrow->setFixedSize(18, kBarH);
        ovArrow->setStyleSheet(QLatin1String(kArrowBtnQss));   // padding:0 → the ⌄ has room
        connect(ovArrow, &QToolButton::clicked, this, [this, ovArrow]() {
            if (!m_overlayPanel) return;
            if (m_overlayPanel->isVisible()) { m_overlayPanel->hide(); return; }
            m_overlayPanel->adjustSize();
            m_overlayPanel->move(ovArrow->mapToGlobal(QPoint(0, ovArrow->height() + 2)));
            m_overlayPanel->show();
            m_overlayPanel->raise();
        });
        viewBar->addWidget(ovArrow);
    }
    sep();
    mkToggle(QStringLiteral("fx"), QStringLiteral("FX"),
             QStringLiteral("Show FX submeshes"), true,
             [this](bool on) { m_showFx = on; setFlaggedPartsChecked(m_partIsFx, on); });
    mkToggle(QStringLiteral("sim"), QStringLiteral("SIM"),
             QStringLiteral("Show cloth-sim submeshes"), true,
             [this](bool on) { m_showSim = on; setFlaggedPartsChecked(m_partIsSim, on); });
    mkToggle(QStringLiteral("gib"), QStringLiteral("GIB"),
             QStringLiteral("Show gore / flesh (gib) submeshes — usually on NPCs / monsters"), true,
             [this](bool on) { m_showGib = on; setFlaggedPartsChecked(m_partIsGib, on); });
    // (Spin moved into the Camera panel — it's a camera behavior, not a shading toggle.)
    viewBar->addStretch(1);
    sep();   // divider between shading toggles and the view-control cluster

    // Right cluster — full parity with the Wardrobe preview toolbar:
    // Reset view · Fullscreen · Graphics · Camera · Lighting · Shaders · Detail maps · Rig · Physics.
    // (Shaders / Detail maps / Rig / Physics are hidden unless Developer mode is on.)
    // Shared skin (BrowserTab.h) + the red [panelOpen] open state — same recipe as Wardrobe.
    const QString rsStyle = QLatin1String(kToolBtnQss) + QStringLiteral(
        "QToolButton[panelOpen=\"true\"]{background:#8a1414;color:#fff;border-color:#a01818;}");
    auto mkViewBtn = [&](const QString& text, const QString& tip, std::function<void()> slot) {
        auto* b = new QToolButton(center);
        b->setText(text);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(rsStyle);   // rsStyle = kToolBtnQss + the panelOpen red state
        b->setFixedHeight(kBarH);    // aligns with the shading/overlay cluster
        connect(b, &QToolButton::clicked, this, [slot]() { slot(); });
        viewBar->addWidget(b);
        return b;
    };
    // ("Reset view" is gone — MIDDLE-CLICK the viewport to re-frame; see GLModelWidget's
    //  mousePressEvent. The Camera panel also has explicit framing buttons.)
    // (The text "Fullscreen" toolbar button was removed — the ⛶ icon on the viewport N-strip
    //  plus the F key and the floating Exit button cover it; m_fsBtn stays null and its
    //  toggleFullscreen text-swap is guarded.)
    viewBar->addWidget(m_vpBtn);                        // Graphics (created above)
    viewBar->addWidget(m_dyeBtn);                       // Pigment (dye zones)
    m_camBtn = mkViewBtn(QStringLiteral("Camera"),
                         QStringLiteral("FOV · view angles · turntable · projection · presets"),
                         [this] { toggleCameraPanel(); });
    m_lightBtn = mkViewBtn(QStringLiteral("Lighting"),
                           QStringLiteral("Three-point light rig (key / rim / fill) + surface + shadows"),
                           [this] { toggleLightingPanel(); });
    m_shaderBtn = mkViewBtn(QStringLiteral("Shaders"),
                            QStringLiteral("Shell-fur + mesh-FX shading settings"),
                            [this] { toggleShaderPanel(); });
    m_detailBtn = mkViewBtn(QStringLiteral("Detail maps"),
                            QStringLiteral("Detail-map selection rule (discovery tool — global)"),
                            [this] { toggleDetailPanel(); });
    // (Rig button retired — its toggles live in the Overlays menu now.)
    m_physBtn = mkViewBtn(QStringLiteral("Physics"),
                          QStringLiteral("Live cloth-physics tuning (debug)"),
                          [this] { togglePhysicsPanel(); });
    m_viewBarW = new QWidget(center);   // container → hidden as one unit when maximized
    m_viewBarW->setLayout(viewBar);
    cl->addWidget(m_viewBarW);
    applyViewportDevGating();   // hide the dev-only buttons unless Developer mode is on

    m_modelView = new GLModelWidget(center);
    m_modelView->setMinimumHeight(260);
    // QOpenGLWidget defaults to Qt::NoFocus, so clicking the viewport never gave it keyboard
    // focus — which is why H/Shift+H/Alt+H only worked after clicking the outliner first. With
    // ClickFocus the viewport can own the keys the moment you interact with it.
    m_modelView->setFocusPolicy(Qt::ClickFocus);
    // F = fullscreen (maximize-in-place), matching the Wardrobe tab. Scoped to the viewport so
    // it never hijacks the letter "f" typed into the search boxes.
    {
        auto* fsSc = new QShortcut(QKeySequence(Qt::Key_F), m_modelView);
        fsSc->setContext(Qt::WidgetShortcut);
        connect(fsSc, &QShortcut::activated, this, [this] { toggleFullscreen(); });
    }
    // (No viewport tooltip — a hint card following the cursor over the model is just in the way.)
    // Push the persisted view state into GL — the toolbar restored its BUTTON states before the
    // viewport existed, so their slots ran against a null view.
    {
        QSettings vs;
        // 0 wire · 1 flat · 2 shaded · 3 rendered. Restore only wire/pbr here — the post-feature
        // keys keep whatever the mode/Graphics checkboxes last wrote (no clobbering at startup).
        const int sm = qBound(0, vs.value(QStringLiteral("models/view/shadeMode"), 3).toInt(), 3);
        m_modelView->setWireframe(sm == 0);
        m_modelView->setPbr(sm >= 2);
        m_modelView->setShowGrid(vs.value(QStringLiteral("models/view/grid"), false).toBool());
        m_modelView->setShowSkeleton(vs.value(QStringLiteral("models/view/skeleton"), false).toBool());
        // Turntable state lives on the CAMERA PANEL's keys (it always had the richer control —
        // the removed toolbar Spin was a parallel duplicate on models/view/spin).
        m_modelView->setSpinSpeed(
            float(qBound(1, vs.value(QStringLiteral("models/turntableSpeed"), 25).toInt(), 100)) / 1000.0f);
        m_modelView->setAutoSpin(vs.value(QStringLiteral("models/turntable"), false).toBool());
    }
    // ── Blender N-strip: the eight settings popovers live as icon buttons on the viewport's
    // right edge (below the axis gizmo) instead of eating half the toolbar. The BUTTONS move;
    // the panels, their toggle functions, anchoring and dev-gating are completely untouched —
    // popups now simply open beside the strip.
    {
        m_vpStrip = new QWidget(m_modelView);
        auto* sv = new QVBoxLayout(m_vpStrip);
        sv->setContentsMargins(2, 2, 2, 2);
        sv->setSpacing(3);
        // »/« — hide/show the whole right panel column, Blender's sidebar arrow: the column
        // vanishes COMPLETELY (viewport takes the width) and this floating arrow brings it back.
        m_sideArrow = new QToolButton(m_vpStrip);
        m_sideArrow->setText(QStringLiteral("»"));
        m_sideArrow->setToolTip(QStringLiteral("Hide the side panels (this arrow brings them back)"));
        m_sideArrow->setCheckable(true);
        m_sideArrow->setCursor(Qt::PointingHandCursor);
        m_sideArrow->setFixedSize(26, 18);
        m_sideArrow->setStyleSheet(QLatin1String(kArrowBtnQss));
        connect(m_sideArrow, &QToolButton::toggled, this, [this](bool on) { setSideCollapsed(on); });
        sv->addWidget(m_sideArrow);
        // stripGlyph lives in ViewGlyphs.h — the Wardrobe tab's N-strip wears the same icons.
        // (Rig is gone — its whole content moved into the Overlays menu.)
        const struct { QAbstractButton* b; int g; } stripBtns[] = {
            {m_vpBtn, 0},     {m_dyeBtn, 1},    {m_camBtn, 2}, {m_lightBtn, 3},
            {m_shaderBtn, 4}, {m_detailBtn, 5}, {m_physBtn, 7}};
        for (const auto& e : stripBtns) {
            if (!e.b) continue;
            e.b->setToolTip(e.b->text() + QStringLiteral(" — ") + e.b->toolTip());
            e.b->setText(QString());
            e.b->setIcon(QIcon(stripGlyph(e.g)));
            e.b->setIconSize(QSize(16, 16));
            e.b->setFixedSize(26, 26);   // 24 left almost nothing around a 16px glyph
            sv->addWidget(e.b);   // reparents the button out of the toolbar
        }
        // Fullscreen ⛶ on the strip too (parity with the Stable tab) — same toggle as the toolbar button.
        auto* fsStrip = new QToolButton(m_vpStrip);
        fsStrip->setText(QStringLiteral("⛶"));
        fsStrip->setToolTip(QStringLiteral("Fullscreen — viewport fills the tab (F / Esc restores)"));
        fsStrip->setCursor(Qt::PointingHandCursor);
        fsStrip->setFixedSize(26, 26);
        fsStrip->setStyleSheet(QLatin1String(kArrowBtnQss));
        connect(fsStrip, &QToolButton::clicked, this, [this] { toggleFullscreen(); });
        sv->addWidget(fsStrip);
        m_vpStrip->adjustSize();
    }
    // The Models tab is for inspecting/exporting single models; cloth SIMULATION isn't needed
    // here (exports use the bind pose, not the sim), and its per-model paint-time build is the
    // heaviest/most GPU-crash-prone code path. Default it OFF for stability — a model that would
    // otherwise crash the driver in the cloth solver now loads statically. Opt back in via the
    // setting for users who want the cloth to swing in the preview.
    m_modelView->setClothEnabled(
        QSettings().value(QStringLiteral("models/clothSim"), false).toBool());
    // (Viewport toggle states are now restored unconditionally in mkToggle — see setChecked(initial).)
    {   // Restore the saved viewport backdrop (colour + gradient).
        const QString bg = QSettings().value(QStringLiteral("models/bgColor")).toString();
        if (!bg.isEmpty()) m_modelView->setBackgroundColor(QColor(bg));
        m_modelView->setBackgroundGradient(
            QSettings().value(QStringLiteral("models/bgGradient"), false).toBool());
    }
    // Empty-state hint until a model is loaded (cleared in applyLoadedGeometry). Beats a black void.
    m_modelView->setOverlayText(QStringLiteral(
        "Select a model from the list to preview it\n"
        "(auto-load is off — double-click a row to load it)"));
    applyModelRig();   // push saved Rig-popup flags (phys bones / axis / bone names / translated) to the view
    {   // Apply saved "Model preview viewport" shading features.
        QSettings s;
        auto vp = [&](const QString& k, bool def) {
            return s.value(QStringLiteral("models/viewport/") + k, def).toBool();
        };
        m_modelView->setFeatureDetail(vp(QStringLiteral("detail"), true));
        m_modelView->setFeatureSubsurface(vp(QStringLiteral("subsurface"), true));
        m_modelView->setFeatureHair(vp(QStringLiteral("hair"), true));
        m_modelView->setFeatureIbl(vp(QStringLiteral("ibl"), true));
        m_modelView->setShadowEnabled(vp(QStringLiteral("shadows"), true));
        m_modelView->setSsaoEnabled(vp(QStringLiteral("ssao"), true));
        m_modelView->setFeatureSpecAA(vp(QStringLiteral("specaa"), true));
        m_modelView->setBackfaceCull(!vp(QStringLiteral("backfaces"), true));   // default: show back faces

        m_modelView->setFeatureMask(vp(QStringLiteral("mask"), false));
        m_modelView->setFeatureTonemap(vp(QStringLiteral("tonemap"), true));
        m_modelView->setFeatureDye(vp(QStringLiteral("dye"), false));
        for (int r = 0; r < 4; ++r)   // the 4 pigment colours (mask zones)
            m_modelView->setDyeColor(r, QColor(s.value(
                QStringLiteral("models/viewport/dyeColor%1").arg(r), QStringLiteral("#ffffff")).toString()));
        m_modelView->setExposure(s.value(QStringLiteral("models/viewport/exposure"), 1.0).toFloat());
        m_modelView->setEnvironment(s.value(QStringLiteral("models/viewport/env"), 1).toInt());
    }
    {   // Apply saved viewport-panel settings (Lighting rig, Detail-map config, cloth params,
        // channel view, FOV, projection, shader fur/FX) so the popups' persisted values take
        // effect immediately on startup — parity with the Wardrobe preview.
        QSettings s;
        applyLightRig();
        applyDetailConfig();
        applyClothParams();
        m_modelView->setViewChannel(s.value(QStringLiteral("models/view/channel"), 0).toInt());
        // Restore the viewport toggle buttons that depend on m_modelView (FX/SIM are member flags
        // already restored by mkToggle; these need the now-created view). Defaults match mkToggle.
        auto vt = [&](const QString& k, bool def) { return s.value(QStringLiteral("models/view/") + k, def).toBool(); };
        m_modelView->setPbr(!vt(QStringLiteral("flat"), false));   // "Flat" = PBR off
        m_modelView->setWireframe(vt(QStringLiteral("wireframe"), false));
        m_modelView->setShowGrid(vt(QStringLiteral("grid"), false));
        m_modelView->setShowSkeleton(vt(QStringLiteral("skeleton"), false));
        m_modelView->setAutoSpin(vt(QStringLiteral("spin"), false));
        m_modelView->setFov(float(s.value(QStringLiteral("models/fov"), 45).toInt()));
        m_modelView->setOrthographic(s.value(QStringLiteral("models/ortho"), false).toBool());
        m_modelView->setFurEnabled(s.value(QStringLiteral("models/viewport/fur"), true).toBool());
        m_modelView->setFurLength(s.value(QStringLiteral("models/viewport/furLength"), 44).toInt() * 0.0005f);
        m_modelView->setFurDensity(float(s.value(QStringLiteral("models/viewport/furDensity"), 30).toInt()));
        m_modelView->setFurShells(s.value(QStringLiteral("models/viewport/furShells"), 20).toInt());
        m_modelView->setFurGravity(s.value(QStringLiteral("models/viewport/furGravity"), 18).toInt() * 0.00025f);
        m_modelView->setFurCurl(s.value(QStringLiteral("models/viewport/furCurl"), 14).toInt() * 0.00025f);
        m_modelView->setFurCoverage(0.60f - s.value(QStringLiteral("models/viewport/furCoverage"), 57).toInt() * 0.01f);
        m_modelView->setFxIntensity(s.value(QStringLiteral("models/viewport/fxIntensity"), 20).toInt() * 0.05f);
        m_modelView->setFxScrollSpeed(s.value(QStringLiteral("models/viewport/fxScroll"), 20).toInt() * 0.05f);
        m_modelView->setFxWobble(s.value(QStringLiteral("models/viewport/fxWobble"), 20).toInt() * 0.05f);
    }

    // FILE INFO now lives entirely on the right-column INFO page — the viewport overlay was a
    // duplicate and was covering the model. Keep a HIDDEN sink widget so every setInfo() call
    // still has an m_infoVals target (setInfo also feeds m_dataVals, the visible INFO page), but
    // it's never shown. (Cheap: ~11 offscreen labels.)
    m_infoOverlay = new QWidget(m_modelView);
    m_infoOverlay->hide();
    auto* infoForm = new QFormLayout(m_infoOverlay);
    for (const QString& key : {QStringLiteral("Filename"), QStringLiteral("Title"),
                               QStringLiteral("Filesize"), QStringLiteral("Format"),
                               QStringLiteral("Materials"), QStringLiteral("Textures"),
                               QStringLiteral("Animations"), QStringLiteral("Family"),
                               QStringLiteral("Used by"), QStringLiteral("Items"),
                               QStringLiteral("Variants")}) {
        auto* val = new QLabel(QStringLiteral("—"), m_infoOverlay);
        m_infoVals.insert(key, val);
        infoForm->addRow(new QLabel(key + QStringLiteral(":"), m_infoOverlay), val);
    }
    // Statistics overlay (Blender's overlay panel has one): live verts/tris/parts/bones for what's
    // actually VISIBLE — the numeric feedback for the eye toggles, Solo and the H hotkeys.
    m_statsOv = new QLabel(m_modelView);
    m_statsOv->setAttribute(Qt::WA_TransparentForMouseEvents, true);   // never blocks orbiting
    m_statsOv->setStyleSheet(QStringLiteral("color:#c8c2ae;background:transparent;"));
    m_statsOv->move(8, 8);
    m_statsOv->setVisible(QSettings().value(QStringLiteral("models/view/stats"), false).toBool());

    m_modelView->installEventFilter(this);   // Resize → keep the N-strip pinned (overlay stays hidden)

    // Vertical splitter: drag to resize the viewport against the panels below it.
    auto* vsplit = new QSplitter(Qt::Vertical, center);
    m_viewSplit = vsplit;   // viewport/transport splitter (kept for layout queries)
    vsplit->addWidget(m_modelView);
    auto* bottom = new QWidget(vsplit);
    m_bottomW = bottom;   // hidden while the viewport is maximized
    auto* bl = new QVBoxLayout(bottom);
    bl->setContentsMargins(0, 0, 0, 0);

    // (Export controls live at the top-left of the list — see the LEFT column.)

    // Timeline.
    // ── Blender-style transport: step / play-pause / step (drawn icons) + scrub + frame field ──
    m_timeline = new QWidget(center);
    auto* tlay = new QHBoxLayout(m_timeline);
    tlay->setContentsMargins(0, 0, 0, 0);
    tlay->setSpacing(3);
    auto mkTransport = [&](int glyph, const QString& tip) {
        auto* b = new QToolButton(m_timeline);
        b->setIcon(transportGlyph(glyph));
        b->setAutoRaise(true);
        b->setToolTip(tip);
        tlay->addWidget(b);
        return b;
    };
    auto* stepB = mkTransport(2, QStringLiteral("Step back one frame"));
    m_playBtn = new QPushButton(m_timeline);
    m_playBtn->setIcon(transportGlyph(0));
    m_playBtn->setMaximumWidth(34);
    m_playBtn->setToolTip(QStringLiteral("Play / pause (Play at the end restarts)"));
    tlay->addWidget(m_playBtn);
    auto* stepF = mkTransport(3, QStringLiteral("Step forward one frame"));
    connect(stepB, &QToolButton::clicked, this,
            [this]() { m_animSlider->setValue(m_animSlider->value() - 1); });
    connect(stepF, &QToolButton::clicked, this,
            [this]() { m_animSlider->setValue(m_animSlider->value() + 1); });
    m_animSlider = new QSlider(Qt::Horizontal, m_timeline);
    m_animSlider->setTickPosition(QSlider::TicksBelow);   // frame ticks (interval set per clip)
    m_animSlider->setSingleStep(1);
    m_animSlider->setToolTip(QStringLiteral(
        "Scrub the clip — wheel steps one frame, Shift+wheel changes playback speed"));
    m_animSlider->installEventFilter(this);   // wheel = precise 1-frame scrub · Shift+wheel = speed
    m_frameSpin = new QSpinBox(m_timeline);
    m_frameSpin->setToolTip(QStringLiteral("Current frame — type to jump"));
    m_frameSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_frameSpin->setFixedWidth(52);
    m_frameSpin->setAlignment(Qt::AlignRight);
    connect(m_frameSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
        if (m_animSlider->value() != v) m_animSlider->setValue(v);
    });
    m_frameMax = new QLabel(QStringLiteral("/ 0"), m_timeline);
    m_timeLabel = new QLabel(QStringLiteral("0.00 / 0.00 s"), m_timeline);
    m_speedCombo = new QComboBox(m_timeline);
    m_speedCombo->addItems({QStringLiteral("0.25x"), QStringLiteral("0.5x"), QStringLiteral("1x"),
                            QStringLiteral("1.5x"), QStringLiteral("2x")});
    m_speedCombo->setCurrentIndex(2);   // 1x
    m_loopCheck = new QCheckBox(QStringLiteral("Loop"), m_timeline);
    m_loopCheck->setChecked(true);
    tlay->addWidget(m_animSlider, 1);
    tlay->addWidget(m_frameSpin);
    tlay->addWidget(m_frameMax);
    tlay->addWidget(m_timeLabel);
    tlay->addWidget(m_speedCombo);
    tlay->addWidget(m_loopCheck);
    m_timeline->setVisible(false);
    bl->addWidget(m_timeline);

    // ── ANIMATIONS page: lives in the right-side icon stack (Blender: actions are data, and data
    // browsing belongs in the properties column). The transport bar above stays with the viewport.
    // Parented to `center` because that's the column being built here — the right column doesn't
    // exist yet. addRightPage's PanelBox re-parents it at registration (end of the ctor).
    auto* animPage = new QWidget(center);
    animPage->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);   // greedy: the clip
                                                                              // list fills its panel
    auto* apl = new QVBoxLayout(animPage);
    apl->setContentsMargins(0, 0, 0, 0);
    apl->setSpacing(2);
    {
        auto* ah = new QHBoxLayout();
        ah->setContentsMargins(0, 0, 0, 0);
        m_animsHdr = new QLabel(QStringLiteral("ANIMATIONS"), animPage);
        m_animsHdr->setStyleSheet(QLatin1String(kHdrQss));   // = the
        // other page titles (this page is hand-built, so addRightPage's styling doesn't reach it)
        ah->addWidget(m_animsHdr);
        auto* animHelp = new QToolButton(center);
        animHelp->setText(QStringLiteral("?"));
        animHelp->setAutoRaise(true);
        animHelp->setToolTip(QStringLiteral(
            "<b>Animation list key</b><br>"
            "• <b>— Name —</b> bold rows group clips by their in-game <b>AnimSet</b>.<br>"
            "• The leading text on a clip is its <b>action</b> (Idle / Walk / Get Hit…), from the clip's Power.<br>"
            "• <span style='color:#d8a23a'>Gold</span> = <b>pulled</b> from a model you picked (Pull from…).<br>"
            "• <span style='color:#6fb7d4'>Cyan</span> = skeleton-<b>compatible</b> only — not a confirmed in-game clip.<br>"
            "• Normal colour = this model's own / its actor's authoritative clips.<br>"
            "Right-click a clip for Play / Play whole set / Export set / female variant."));
        ah->addWidget(animHelp);
        ah->addStretch(1);
        auto* pullBtn = new QToolButton(center);
        pullBtn->setText(QStringLiteral("Pull from…"));
        pullBtn->setToolTip(QStringLiteral("Also list animations from another model you pick "
                                           "(retargeted to this model's skeleton)"));
        connect(pullBtn, &QToolButton::clicked, this, [this]() { pullAnimsFromModel(); });
        auto* pullAutoBtn = new QToolButton(center);
        pullAutoBtn->setText(QStringLiteral("Pull suggested"));
        pullAutoBtn->setToolTip(QStringLiteral(
            "Pull animations from this model's own base rig, worked out automatically — "
            "barF_stor157_HLM matches barF_base00.\n"
            "Uses the name's rig family first, then falls back to bone-hash skeleton matching."));
        connect(pullAutoBtn, &QToolButton::clicked, this, [this]() { pullSuggestedAnims(); });
        m_pullClearBtn = new QToolButton(center);
        m_pullClearBtn->setText(QStringLiteral("Clear pulls"));
        m_pullClearBtn->setToolTip(QStringLiteral(
            "Show only this model's own animations — drops the gold pulled clips and the muted-cyan "
            "\"skeleton-compatible, unconfirmed\" guesses."));
        m_pullClearBtn->setEnabled(false);
        connect(m_pullClearBtn, &QToolButton::clicked, this, [this]() {
            m_pullSources.clear();
            m_suppressSkelFallback = true;   // the cyan guesses are not this model's own either
            m_pullClearBtn->setEnabled(false);
            if (m_curSno >= 0) populateAnimList(m_curSno, m_curName.toLower());
        });
        ah->addWidget(pullBtn);
        ah->addWidget(pullAutoBtn);
        ah->addWidget(m_pullClearBtn);
        apl->addLayout(ah);
    }
    m_animSearch = new QLineEdit(animPage);
    m_animSearch->setPlaceholderText(QStringLiteral("Search animations…"));
    m_animSearch->setClearButtonEnabled(true);
    apl->addWidget(m_animSearch);
    m_anims = new QListWidget(animPage);
    // Multi-select: ctrl/shift-click to pick clips to embed or export as an animation library.
    m_anims->setSelectionMode(QAbstractItemView::ExtendedSelection);
    apl->addWidget(m_anims, 1);
    // REGISTERED AT THE END OF THE CONSTRUCTOR, not here: the center column builds BEFORE the
    // right column, so m_rstripLay doesn't exist yet — registering now silently no-ops (that was
    // the "where did the animations panel go" bug).
    m_animPage = animPage;
    // With the list gone, the viewport's bottom pane is just the transport bar — cap it so the
    // splitter can't waste height on emptiness.
    bottom->setMaximumHeight(64);
    // Right-click a clip → operate on its whole AnimSet (the game's real grouping). Lets the user
    // select/play/export an entire set at once instead of ctrl-clicking each clip.
    m_anims->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_anims, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        QListWidgetItem* hit = m_anims->itemAt(p);
        if (!hit || hit->flags() == Qt::NoItemFlags) return;   // ignore the group-header rows
        const QString clip = hit->data(Qt::UserRole).toString();
        const QString set = m_clipSet.value(clip.toLower());
        // Gather a set's clips in the list's (grouped) display order.
        auto clipsInSet = [this](const QString& s) {
            QStringList out;
            for (int i = 0; i < m_anims->count(); ++i) {
                QListWidgetItem* it = m_anims->item(i);
                if (it->flags() == Qt::NoItemFlags) continue;
                const QString c = it->data(Qt::UserRole).toString();
                if (m_clipSet.value(c.toLower()) == s) out << c;
            }
            return out;
        };
        auto selectSet = [this](const QString& s) {
            m_anims->clearSelection();
            for (int i = 0; i < m_anims->count(); ++i) {
                QListWidgetItem* it = m_anims->item(i);
                if (it->flags() == Qt::NoItemFlags) continue;
                if (m_clipSet.value(it->data(Qt::UserRole).toString().toLower()) == s)
                    it->setSelected(true);
            }
        };
        QMenu menu(this);
        menu.addAction(QStringLiteral("Play"), this, [this, clip]() { playAnimationByName(clip); });
        menu.addSeparator();
        // Export the selected clip(s) as a rig-only .glb (a Blender clip library). If the right-clicked
        // clip isn't part of the current selection, export just it. exportAnimationsOnly() reads the
        // selection via collectExportAnims().
        auto pickHit = [this, hit]() { if (hit && !hit->isSelected()) { m_anims->clearSelection(); hit->setSelected(true); } };
        const int nSel = hit->isSelected() ? qMax(1, int(m_anims->selectedItems().size())) : 1;
        menu.addAction(nSel > 1 ? QStringLiteral("Export %1 animations (.glb)…").arg(nSel)
                                : QStringLiteral("Export animation (.glb)…"),
                       this, [this, pickHit]() { pickHit(); exportAnimationsOnly(false); });
        menu.addAction(QStringLiteral("Export animation(s) to last dir"),
                       this, [this, pickHit]() { pickHit(); exportAnimationsOnly(true); });
        if (!set.isEmpty()) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("Play whole set \"%1\"").arg(set), this,
                           [this, set, clipsInSet]() { playAnimationSet(clipsInSet(set)); });
            menu.addSeparator();
            menu.addAction(QStringLiteral("Select all in set \"%1\"").arg(set), this,
                           [set, selectSet]() { selectSet(set); });
            menu.addAction(QStringLiteral("Export set \"%1\" only (.glb)…").arg(set), this,
                           [this, set, selectSet]() { selectSet(set); exportAnimationsOnly(); });
        }
        // Gender preview: swap in the female-override clips (only meaningful where pairs exist).
        if (!m_femalePair.isEmpty()) {
            menu.addSeparator();
            QAction* fem = menu.addAction(QStringLiteral("Preview female variant (applies on next Play)"));
            fem->setCheckable(true);
            fem->setChecked(m_previewFemale);
            connect(fem, &QAction::toggled, this, [this](bool on) { m_previewFemale = on; });
        }
        menu.exec(m_anims->viewport()->mapToGlobal(p));
    });
    // (Rig-only animation export lives on the Export menu — "Export animation(s) only (.glb)".)
    vsplit->addWidget(bottom);
    vsplit->setStretchFactor(0, 3);   // viewport gets the lion's share by default
    vsplit->setStretchFactor(1, 2);
    vsplit->setChildrenCollapsible(false);   // neither the viewport nor the anim panel can vanish
    cl->addWidget(vsplit, 1);
    // Remember the viewport / animation split (gated by Settings ▸ View ▸ Remember panel sizes).
    PanelPersist::bind(vsplit, QStringLiteral("models/centerSplit"));
    split->addWidget(center);

    // ════════ RIGHT column: looks + materials + material textures ════════
    // Every section lives in a vertical splitter so the user can drag-resize
    // each (parts / looks / materials / textures / preview) freely.
    auto* right = new QWidget(split);
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(4, 4, 4, 4);
    // Blender-style breadcrumb: whose properties am I looking at? Tracks the outliner selection
    // (model › part › material › …). Ignored width policy — a long path can never widen the
    // column (today's layout-stability rule).
    m_breadcrumb = new QLabel(right);
    m_breadcrumb->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_breadcrumb->setMinimumWidth(1);
    m_breadcrumb->setStyleSheet(QStringLiteral("color:#9a8f78;"));
    // Segments are LINKS (Blender's properties breadcrumb): clicking one selects that node in
    // the outliner. href = position in m_breadcrumbIx, rebuilt by updateBreadcrumb.
    m_breadcrumb->setTextFormat(Qt::RichText);
    m_breadcrumb->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    connect(m_breadcrumb, &QLabel::linkActivated, this, [this](const QString& href) {
        const int i = href.toInt();
        if (i < 0 || i >= m_breadcrumbIx.size() || !m_breadcrumbIx[i].isValid() || !m_list) return;
        const QModelIndex ix(m_breadcrumbIx[i]);
        m_list->selectionModel()->setCurrentIndex(
            ix, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_list->scrollTo(ix);
    });
    rl->addWidget(m_breadcrumb);
    // Right column: the icon strip TOGGLES panels in and out of a vertical splitter — you pick
    // which are up, they stack, you drag the handles to size them and ▲▼ to reorder. Hidden
    // panels aren't in the splitter at all, so they cost nothing.
    auto* body = new QHBoxLayout();
    body->setSpacing(2);
    auto* stripW = new QWidget(right);
    auto* strip = new QVBoxLayout(stripW);
    strip->setContentsMargins(0, 2, 0, 0);
    strip->setSpacing(2);
    body->addWidget(stripW);            // strip on the LEFT of the panels
    m_rstack = new QSplitter(Qt::Vertical, right);
    m_rstack->setChildrenCollapsible(false);   // a drag can't erase a panel — ✕ / the strip does
    m_rstack->setHandleWidth(4);
    body->addWidget(m_rstack, 1);
    connect(m_rstack, &QSplitter::splitterMoved, this, [this](int, int) { savePanelLayout(); });
    // Gentle empty state: veils the column until a model is selected (hidden in showAppearance,
    // shown again by reset). Parented to `right`, NOT to the splitter — QSplitter adopts every
    // child widget as a pane, so a veil parented there would become a draggable panel of its own.
    // Tracked onto the splitter's rect by the Resize/Move hook in eventFilter.
    m_rstackHint = new QLabel(QStringLiteral("No model loaded\n\nSelect a model in the list"), right);
    m_rstackHint->setAlignment(Qt::AlignCenter);
    m_rstackHint->setStyleSheet(QStringLiteral("color:#777;background:#232323;"));
    m_rstack->installEventFilter(this);
    rl->addLayout(body, 1);

    // Small painter-drawn glyphs for the two pages that have no outliner kind to borrow from.
    auto listGlyph = []() {
        QPixmap pm(14, 14); pm.fill(Qt::transparent);
        QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(200, 190, 150), 1.5, Qt::SolidLine, Qt::RoundCap));
        for (int y : {3, 7, 11}) { p.drawLine(QPointF(5, y), QPointF(12, y));
                                   p.drawEllipse(QPointF(2.5, y), 0.9, 0.9); }
        return pm;
    };
    // (The old PREVIEW page's picture glyph retired with the page — preview lives under the
    // MATERIAL TEXTURES tabs now.)

    m_rstripLay = strip;   // late pages (ANIMATIONS, built further down) register via addRightPage
    // A titled page = header label + content in the stack, plus its icon button on the strip.
    auto section = [&](const QString& title, QWidget* content, const QPixmap& icon,
                       const QString& tip) -> QLabel* {
        return addRightPage(title, content, icon, tip);
    };

    // PARTS
    // (The old PARTS pane lived here — it's been absorbed into the outliner: parts hang off the
    // loaded model's row in the left list, with the same eye-checkboxes. See buildOutlinerSubtree.)

    // LOOKS
    m_looksModel = new QStandardItemModel(0, 4, this);
    m_looksModel->setHorizontalHeaderLabels({QStringLiteral("INDEX"), QStringLiteral("HASH (HEX)"),
                                             QStringLiteral("HASH (DEC)"), QStringLiteral("NAME")});
    m_looksView = makeTable(right, m_looksModel);
    CsvCopy::install(m_looksView);
    m_looksHdr = section(QStringLiteral("LOOKS (0)"), m_looksView, listGlyph(),
                         QStringLiteral("Looks — appearance variants (SOA looks)"));

    // MATERIALS — App Materials / Materials / Vertex Buffers (counts on the tabs)
    m_matModel = new QStandardItemModel(0, 7, this);
    m_matModel->setHorizontalHeaderLabels({QStringLiteral("MAT. HASH"), QStringLiteral("MAT. NAME"),
                                           QStringLiteral("SO/"), QStringLiteral("FLAGS"),
                                           QStringLiteral("MAT. SNO"), QStringLiteral("CLOTH SN"),
                                           QStringLiteral("O. MAT. SNO")});
    m_mats = makeTable(right, m_matModel);
    m_mats->setColumnWidth(1, 200);
    CsvCopy::install(m_mats);
    m_matListModel = new QStandardItemModel(0, 2, this);
    m_matListModel->setHorizontalHeaderLabels({QStringLiteral("MAT. SNO"), QStringLiteral("MAT. NAME")});
    m_matListView = makeTable(right, m_matListModel);
    m_matListView->setColumnWidth(0, 90);
    CsvCopy::install(m_matListView);
    m_vbModel = new QStandardItemModel(0, 4, this);
    m_vbModel->setHorizontalHeaderLabels({QStringLiteral("BUFFER"), QStringLiteral("STRIDE"),
                                          QStringLiteral("VERTS"), QStringLiteral("ATTRIBUTES")});
    m_vbView = makeTable(right, m_vbModel);
    CsvCopy::install(m_vbView);
    // "SubObject Apps": the LOD0 sub-object (draw-call) table — material index/name,
    // hash, vertex/index buffer, max LOD, shader-map override, cloth flag, flags.
    m_subObjModel = new QStandardItemModel(0, 9, this);
    m_subObjModel->setHorizontalHeaderLabels({QStringLiteral("#"), QStringLiteral("MAT"),
        QStringLiteral("MATERIAL"), QStringLiteral("HASH"), QStringLiteral("VB"),
        QStringLiteral("IB"), QStringLiteral("MAXLOD"), QStringLiteral("SHADER OVR"),
        QStringLiteral("CLOTH")});
    m_subObjView = makeTable(right, m_subObjModel);
    m_subObjView->setColumnWidth(2, 180);
    CsvCopy::install(m_subObjView);
    m_matTabs = new QTabWidget(right);
    m_matTabs->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);   // greedy
    m_matTabs->addTab(m_mats, QStringLiteral("App Materials"));
    m_matTabs->addTab(m_subObjView, QStringLiteral("SubObject Apps"));
    m_matTabs->addTab(m_matListView, QStringLiteral("Materials"));
    m_matTabs->addTab(m_vbView, QStringLiteral("Vertex Buffers"));
    m_matsHdr = section(QStringLiteral("MATERIALS"), m_matTabs,
                        ModelOutlinerModel::kindIcon(ModelOutlinerModel::Material),
                        QStringLiteral("Materials — app materials, sub-object apps, vertex buffers"));

    // MATERIAL TEXTURES — Textures / Values / Shaders (counts on the tabs)
    m_matTexModel = new QStandardItemModel(0, 3, this);
    m_matTexModel->setHorizontalHeaderLabels({QStringLiteral("SHADERTEX"), QStringLiteral("SNO"),
                                              QStringLiteral("NAME")});
    m_matTex = makeTable(right, m_matTexModel);
    CsvCopy::install(m_matTex);
    m_matValModel = new QStandardItemModel(0, 3, this);
    m_matValModel->setHorizontalHeaderLabels({QStringLiteral("SNO"), QStringLiteral("NAME"),
                                              QStringLiteral("VALUE")});
    m_matValView = makeTable(right, m_matValModel);
    m_matValView->setColumnWidth(1, 200);
    CsvCopy::install(m_matValView);
    m_shaderModel = new QStandardItemModel(0, 2, this);
    m_shaderModel->setHorizontalHeaderLabels({QStringLiteral("SNO"), QStringLiteral("SHADER MAP")});
    m_shaderView = makeTable(right, m_shaderModel);
    m_shaderView->setColumnWidth(0, 90);
    CsvCopy::install(m_shaderView);
    m_detailTabs = new QTabWidget(right);
    m_detailTabs->addTab(m_matTex, QStringLiteral("Textures"));
    m_detailTabs->addTab(m_matValView, QStringLiteral("Values"));
    m_detailTabs->addTab(m_shaderView, QStringLiteral("Shaders"));
    // (MATERIAL TEXTURES registers as a page AFTER the preview strip below is built — the two
    // describe the same selection, so they merged onto one page: tabs on top, preview pinned under.)

    // PREVIEW pane: material values text + the 6-tile texture channel strip.
    auto* prev = new QWidget(right);
    auto* pv = new QVBoxLayout(prev);
    pv->setContentsMargins(0, 0, 0, 0);
    pv->setSpacing(2);
    m_matValues = new QLabel(prev);
    m_matValues->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_matValues->setWordWrap(true);
    // A word-wrapped QLabel reports its longest UNBREAKABLE token as its minimum width. Material
    // values are full asset names and hashes, so selecting a model with a long one pushed this
    // column wider and reflowed the tab. `Ignored` + an explicit floor decouples the column width
    // from whatever text happens to be showing: the label wraps into the space it's given.
    m_matValues->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    m_matValues->setMinimumWidth(1);
    pv->addWidget(m_matValues);
    pv->addWidget(new QLabel(QStringLiteral("TEXTURE PREVIEW"), prev));
    m_texFacts = new QLabel(prev);   // "2048×2048 · 11 mips · format 43" for the previewed texture
    m_texFacts->setStyleSheet(QStringLiteral("color:#9a8f78;"));
    m_texFacts->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_texFacts->setMinimumWidth(1);
    pv->addWidget(m_texFacts);
    auto* tpRow = new QHBoxLayout();
    tpRow->setSpacing(1);
    tpRow->setContentsMargins(0, 0, 0, 0);
    static const char* kChan[6] = {"COLOR", "ROUGHNESS", "METAL",
                                   "NORMAL", "ALPHA", "EMISSIVE"};
    // 6 tiles across a ~460px column: 92px each (552px) overflowed and forced a scrollbar. 64
    // fits with room to breathe — hover still pops the full-size preview.
    constexpr int kTile = 64;
    for (int i = 0; i < 6; ++i) {
        m_chanImg[i] = new QLabel(prev);
        m_chanImg[i]->setFixedSize(kTile, kTile);     // square: never stretches the image
        m_chanImg[i]->setAlignment(Qt::AlignCenter);
        m_chanImg[i]->setScaledContents(false);       // aspect-preserving (set in setChannelTile)
        m_chanImg[i]->setStyleSheet(QStringLiteral(
            "QLabel{border:1px solid #444;border-radius:0px;background:#1b1b1b;}"));
        m_chanImg[i]->installEventFilter(this);   // hover → caption + zoom preview
        // Right-click a tile → copy / save the channel image.
        m_chanImg[i]->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_chanImg[i], &QWidget::customContextMenuRequested, this,
                [this, i](const QPoint& p) {
            if (m_chanFull[i].isNull()) return;
            QMenu menu(this);
            menu.addAction(QStringLiteral("Copy image"), this, [this, i]() {
                QApplication::clipboard()->setImage(m_chanFull[i]);
            });
            menu.addAction(QStringLiteral("Save image"), this, [this, i]() { saveTileImage(i, false); });
            menu.addAction(QStringLiteral("Save image as…"), this, [this, i]() { saveTileImage(i, true); });
            menu.exec(m_chanImg[i]->mapToGlobal(p));
        });
        // Caption overlaid on the image, bottom-left, child of the tile.
        m_chanCap[i] = new QLabel(QString::fromLatin1(kChan[i]), m_chanImg[i]);
        m_chanCap[i]->setStyleSheet(QStringLiteral(
            "QLabel{color:#fff;background:rgba(0,0,0,150);border:0;padding:0px 2px;font-size:8px;}"));
        m_chanCap[i]->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_chanCap[i]->adjustSize();
        m_chanCap[i]->move(2, kTile - m_chanCap[i]->height() - 2);
        tpRow->addWidget(m_chanImg[i]);
    }
    tpRow->addStretch(1);
    pv->addLayout(tpRow);
    auto* texPage = new QWidget(right);
    texPage->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);   // greedy: fills its panel
    auto* tpv = new QVBoxLayout(texPage);
    tpv->setContentsMargins(0, 0, 0, 0);
    tpv->setSpacing(2);
    tpv->addWidget(m_detailTabs, 1);
    tpv->addWidget(prev);   // values line + channel tiles, always in view under the tabs
    m_texHdr = section(QStringLiteral("SHADING"), texPage,
                       ModelOutlinerModel::kindIcon(ModelOutlinerModel::TexGroup),
                       QStringLiteral("Shading — texture bindings, values, shaders + channel preview"));

    // ── DATA page: everything the FILE INFO overlay shows, plus game-data the overlay lacks
    // (SNO, collection, tags) — compact label/value rows, selectable, clip-not-widen. ──
    {
        auto* dataBody = new QWidget(right);
        auto* form = new QFormLayout(dataBody);
        form->setContentsMargins(2, 2, 2, 2);
        form->setHorizontalSpacing(10);
        form->setVerticalSpacing(3);
        for (const QString& key : {QStringLiteral("Filename"), QStringLiteral("Title"),
                                   QStringLiteral("SNO"), QStringLiteral("Collection"),
                                   QStringLiteral("Tags"), QStringLiteral("Sets"),
                                   QStringLiteral("Filesize"),
                                   QStringLiteral("Format"), QStringLiteral("Bounds"),
                                   QStringLiteral("LODs"), QStringLiteral("Bones"),
                                   QStringLiteral("Materials"),
                                   QStringLiteral("Textures"), QStringLiteral("Animations"),
                                   QStringLiteral("Family"), QStringLiteral("Actor"),
                                   QStringLiteral("Physics"), QStringLiteral("Used by"),
                                   QStringLiteral("Items"), QStringLiteral("Item facts"),
                                   QStringLiteral("Variants")}) {
            auto* v = new QLabel(QStringLiteral("—"), dataBody);
            v->setTextInteractionFlags(Qt::TextSelectableByMouse);
            v->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // never widens the column
            v->setMinimumWidth(1);
            m_dataVals.insert(key, v);
            auto* k = new QLabel(key + QStringLiteral(":"), dataBody);
            k->setStyleSheet(QStringLiteral("color:#9a8f78;"));
            form->addRow(k, v);
        }
        // NO inner QScrollArea: the section column already scrolls. Nesting one here gave INFO a
        // fixed slab with its own scrollbar (the cramped, clipped block in the messy layout).
        // The form sizes to its rows and the outer scroll handles overflow.
        QPixmap dg(14, 14);
        dg.fill(Qt::transparent);
        {
            QPainter p(&dg);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(QColor(200, 190, 150), 1.3));
            p.drawEllipse(QPointF(7, 7), 5.5, 5.5);
            p.setPen(QPen(QColor(200, 190, 150), 1.8, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(7, 6.2), QPointF(7, 10.2));
            p.drawPoint(QPointF(7, 3.8));
        }
        section(QStringLiteral("INFO"), dataBody, dg,
                QStringLiteral("Info — file, usage and game-data facts about the loaded model"));

        // ── PARTS page: every mesh piece with its own visibility box, triangle count, material
        // and slot. The outliner has the same eyes, but a flat sortable table is the better
        // surface for "which part is the 12k-tri one" / bulk visibility work. Both drive the
        // SAME m_treeModel part checks, so they can't disagree.
        m_partsModel = new QStandardItemModel(0, 4, this);
        m_partsModel->setHorizontalHeaderLabels({QStringLiteral("PART"), QStringLiteral("TRIS"),
                                                 QStringLiteral("SLOT"), QStringLiteral("MATERIAL")});
        m_partsView = makeTable(right, m_partsModel);
        m_partsView->setSelectionMode(QAbstractItemView::ExtendedSelection);
        CsvCopy::install(m_partsView);
        m_partsHdr = section(QStringLiteral("PARTS"), m_partsView,
                             ModelOutlinerModel::kindIcon(ModelOutlinerModel::Part),
                             QStringLiteral("Parts — per-piece visibility, triangle counts, materials"));
        // Checkbox → the one visibility path (same as the outliner eye / H hotkeys).
        connect(m_partsModel, &QStandardItemModel::itemChanged, this, [this](QStandardItem* it) {
            if (m_partsPageSync || !it || it->column() != 0 || !m_treeModel) return;
            const int prim = it->data(Qt::UserRole).toInt();
            if (prim < 0) return;
            m_treeModel->setPartCheck(prim, it->checkState() == Qt::Checked);
            recomputePartVisibility();
        });
        // Selecting rows highlights those parts in the viewport, like the outliner does.
        connect(m_partsView->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                [this](const QItemSelection&, const QItemSelection&) {
            if (!m_modelView) return;
            QList<int> sel;
            for (const QModelIndex& ix : m_partsView->selectionModel()->selectedRows(0))
                sel << m_partsModel->item(ix.row(), 0)->data(Qt::UserRole).toInt();
            m_modelView->setHighlightParts(sel);
            highlightMaterialsForParts(sel);
        });

        // ── CLOTH page: the authored NvCloth tuning D4 ships per simulated piece. The tool
        // already detects cloth parts (m_clothMats) but never surfaced the numbers — these are
        // the exact values the game simulates with (Cloth/<name>.clt.json ▸ tClothTuning).
        m_clothModel = new QStandardItemModel(0, 4, this);
        m_clothModel->setHorizontalHeaderLabels({QStringLiteral("PIECE"), QStringLiteral("PARAM"),
                                                 QStringLiteral("GAME"), QStringLiteral("LIVE")});
        m_clothView = makeTable(right, m_clothModel);
        CsvCopy::install(m_clothView);
        m_clothHdr = section(QStringLiteral("CLOTH"),
                             m_clothView,
                             ModelOutlinerModel::kindIcon(ModelOutlinerModel::MatTiles),
                             QStringLiteral("Cloth — authored physics tuning for each simulated piece"));
        // Variants get jump links (they have SNOs); rebuilt with each entity-scan fill.
        if (QLabel* v = m_dataVals.value(QStringLiteral("Variants"))) {
            v->setTextFormat(Qt::RichText);
            v->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByMouse);
            connect(v, &QLabel::linkActivated, this,
                    [this](const QString& href) { selectModelBySno(href.toInt()); });
        }
    }
    strip->addStretch(1);

    // ── Compact tables: the name column absorbs the width, everything else sizes to content,
    // rows tighten to the font — kills the horizontal scrollbars and wasted row height. ──
    {
        auto compactTable = [](QTableView* t, int stretchCol) {
            if (!t) return;
            QHeaderView* h = t->horizontalHeader();
            h->setSectionResizeMode(QHeaderView::ResizeToContents);
            h->setStretchLastSection(false);
            h->setSectionResizeMode(stretchCol, QHeaderView::Stretch);
            t->verticalHeader()->setDefaultSectionSize(t->fontMetrics().height() + 6);
        };
        compactTable(m_looksView, 3);    // NAME
        compactTable(m_mats, 1);         // MAT. NAME
        compactTable(m_subObjView, 1);
        compactTable(m_matListView, 1);
        compactTable(m_vbView, 3);       // attribute list
        compactTable(m_matTex, 2);       // NAME
        compactTable(m_matValView, 1);   // NAME
        compactTable(m_shaderView, 1);
        compactTable(m_clothView, 1);    // PARAM absorbs the width
        compactTable(m_partsView, 0);    // PART name absorbs the width
        // The 240px caps that used to live here are GONE. They were right when a page was the
        // only thing in a scroll column; in a splitter a cap means the panel can be dragged
        // taller than its content will ever fill, and QBoxLayout answers that by springing the
        // spare height out as gaps between the widgets. Panels are sized by the handle now, and
        // the tabs scroll inside whatever they're given.
    }
    // Pages: 0 LOOKS · 1 MATERIALS · 2 MATERIAL TEXTURES · 3 PREVIEW · 4 ANIMATIONS (added
    // later in construction) — the saved page is restored at the END of the constructor, once
    // every page exists. onOutlinerNodeSelected auto-switches these.
    updateTabCounts();

    split->addWidget(right);
    split->setSizes({640, 760, 560});
    // ── Layout stability ─────────────────────────────────────────────────────────────────────
    // A QSplitter with no stretch factors re-apportions EVERY child on every resize — and on any
    // child's size-hint change. That's why toggling the Filters/View sections or loading a long
    // filename shifted the whole three-column layout, and why shrinking the window squeezed panels
    // until their content disappeared. Pin the side columns (stretch 0) so only the centre viewport
    // absorbs slack, forbid collapsing so a column can never vanish, and give the sides a floor so
    // the window can't be shrunk to the point of hiding them — the viewport takes the hit instead.
    split->setStretchFactor(0, 0);   // left  (filters + list): keeps its width
    split->setStretchFactor(1, 1);   // centre (viewport): absorbs all resizing
    split->setStretchFactor(2, 0);   // right (properties): keeps its width
    split->setChildrenCollapsible(false);
    left->setMinimumWidth(280);
    right->setMinimumWidth(280);
    // Remember the three-column widths (gated by Settings ▸ View ▸ Remember panel sizes).
    PanelPersist::bind(split, QStringLiteral("models/mainSplit"));

    // ── Connections ──
    // Debounce the NAME box: each keystroke otherwise triggers a full beginResetModel rebuild over
    // the whole list (matching name + tags + collection), which stutters while typing. Wait ~200ms
    // after the last keystroke, then filter once.
    auto* searchTimer = new QTimer(this);
    searchTimer->setSingleShot(true);
    searchTimer->setInterval(200);
    connect(searchTimer, &QTimer::timeout, this, [this]() {
        if (m_search) { m_listModel->setFilter(m_search->text()); updateCount(); }
    });
    connect(m_search, &QLineEdit::textChanged, this,
            [searchTimer](const QString&) { searchTimer->start(); });
    connect(m_snoSearch, &QLineEdit::textChanged, this,
            [this](const QString& t) { m_listModel->setSnoFilter(t); updateCount(); });
    connect(m_collSearch, &QLineEdit::textChanged, this,
            [this](const QString&) { applyCategoryFilter(); updateCount(); });
    connect(m_catCombo, &QComboBox::currentIndexChanged, this, [this](int) { applyCategoryFilter(); updateCount(); });
    connect(m_classCombo, &QComboBox::currentIndexChanged, this, [this](int) { applyCategoryFilter(); updateCount(); });
    connect(m_genderCombo, &QComboBox::currentIndexChanged, this, [this](int) { applyCategoryFilter(); updateCount(); });
    connect(m_typeCombo, &QComboBox::currentIndexChanged, this, [this](int) { applyCategoryFilter(); updateCount(); });
    connect(m_groupCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        applyGrouping();
        updateCount();
    });
    connect(m_clearBtn, &QPushButton::clicked, this, [this]() {
        m_search->clear(); m_snoSearch->clear(); m_collSearch->clear();
        m_catCombo->setCurrentIndex(0);
        m_classCombo->setCurrentIndex(0); m_genderCombo->setCurrentIndex(0); m_typeCombo->setCurrentIndex(0);
    });
    connect(m_speedCombo, &QComboBox::currentIndexChanged, this, [this](int) { applyAnimSpeed(); });
    // Multi-select is no longer a mode — Blender-style, Ctrl/Shift-click always work.
    // Single-click browsing behaves identically under ExtendedSelection.
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    if (m_gridView) m_gridView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(m_iconModeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        QSettings().setValue(QStringLiteral("models/iconMode"), m_iconModeCombo->currentData());
        updateIconMode();
        scheduleVisibleIconRender();   // 3D/both → fill thumbnails for the rows in view
    });
    connect(&IconIndex::instance(), &IconIndex::readyChanged, this, [this]() {
        m_origIconCache.clear();          // drop pinned misses now that atlases exist
        if (m_listModel) m_listModel->refreshIcons();
        m_iconPct = -1;
        updateIndexStatus();
    });
    connect(&IconIndex::instance(), &IconIndex::progress, this, [this](int p) {
        m_iconPct = p;   // global meta/icon progress is shown in the app toast (MainWindow)
    });
    connect(&AppearanceMeta::instance(), &AppearanceMeta::progress, this, [this](int p) {
        m_metaPct = p;
    });
    connect(&AppearanceMeta::instance(), &AppearanceMeta::readyChanged, this, &ModelsTab::onMetaReady);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { onAppearanceSelected(); });
    // Auto-render 3D thumbnails for rows scrolled into view (3D / Original+3D modes).
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { scheduleVisibleIconRender(); });
    if (m_gridView)
        connect(m_gridView->verticalScrollBar(), &QScrollBar::valueChanged, this,
                [this](int) { scheduleVisibleIconRender(); });
    // Re-scan the visible set whenever the list is refiltered/regrouped/repopulated.
    connect(m_listModel, &QAbstractItemModel::modelReset, this,
            [this]() { scheduleVisibleIconRender(); });
    connect(m_listModel, &QAbstractItemModel::layoutChanged, this,
            [this]() { scheduleVisibleIconRender(); });
    connect(m_mats->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { onMaterialSelected(); });
    connect(m_matTex->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { onMatTexSelected(); });
    // LOOKS hover/select → highlight the related material rows (SOA index = look index).
    m_looksView->setMouseTracking(true);
    m_looksView->viewport()->setMouseTracking(true);
    connect(m_looksView, &QAbstractItemView::entered, this, [this](const QModelIndex& idx) {
        highlightMaterialsForLook(idx.isValid() ? idx.row() : -1);
    });
    connect(m_looksView->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& cur, const QModelIndex&) {
        highlightMaterialsForLook(cur.isValid() ? cur.row() : -1);
        // Selecting a look applies that variant's materials/textures to the preview;
        // clearing the selection restores the default look (SOA 0).
        applyLook(cur.isValid() ? cur.row() : 0);
        // Parity: the outliner's Looks eyes mirror the table (exactly one on).
        if (m_treeModel) m_treeModel->setExclusiveLookCheck(cur.isValid() ? cur.row() : 0);
    });
    // Outliner Look eye clicked. Checking one applies it via the table (the ONE code path);
    // unchecking the active look falls back to the default look 0 — there is always exactly
    // one active look, like the game itself.
    connect(m_treeModel, &ModelOutlinerModel::lookToggled, this, [this](int ref, bool on) {
        const int want = on ? ref : 0;
        if (m_looksView && want >= 0 && want < m_looksModel->rowCount())
            m_looksView->selectRow(want);   // → applyLook + highlight + eye sync above
        // Unconditional eye sync: selectRow is a no-op when `want` is already the current row
        // (e.g. unchecking look 0 itself), and the eye must still snap back to exactly-one-on.
        m_treeModel->setExclusiveLookCheck(want);
    });
    m_looksView->viewport()->installEventFilter(this);   // hover-leave / empty / reclick
    m_looksView->installEventFilter(this);               // Esc → deselect

    // Outliner subtree: part checkboxes toggle primitives; selecting/hovering nodes highlights.
    connect(m_treeModel, &ModelOutlinerModel::partCheckChanged, this,
            [this]() { recomputePartVisibility(); });   // eye-toggle → GL visibility (with FX/SIM/look state)
    connect(m_list->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) {
        const QList<int> sel = selectedParts();
        if (m_modelView) m_modelView->setHighlightParts(sel);
        highlightMaterialsForParts(sel);   // also highlight the parts' material rows
        // Armature/bone selected → skeleton overlay on, AS A TEMPORARY courtesy: it turns back
        // off when the selection moves away — unless the user had the Skeleton toggle on
        // themselves (toolbar or rig settings), in which case we never touch it.
        bool armSel = false;
        const QModelIndexList rows = m_list->selectionModel()->selectedRows(0);
        for (const QModelIndex& ix : rows) {
            const auto* n = m_treeModel->node(ix);
            if (n && (n->kind == ModelOutlinerModel::Armature
                      || n->kind == ModelOutlinerModel::Bone)) { armSel = true; break; }
        }
        QToolButton* skel = nullptr;
        for (QToolButton* b : m_viewToggleBtns)
            if (b->objectName() == QLatin1String("skeleton")) { skel = b; break; }
        if (skel) {   // parity: always drive the toolbar toggle, never the GL flag directly
            if (armSel && !skel->isChecked()) {
                m_skelViaOutliner = true;    // WE turned it on — so we may turn it off
                skel->setChecked(true);
            } else if (!armSel && m_skelViaOutliner) {
                m_skelViaOutliner = false;
                if (skel->isChecked()) skel->setChecked(false);
            }
        }
        updateBreadcrumb();
    });
    connect(m_list, &QAbstractItemView::entered, this, [this](const QModelIndex& ix) {
        if (!m_modelView || !ix.parent().isValid()) return;   // hover on subtree nodes only
        QList<int> hot = selectedParts();
        hot += m_treeModel->partsUnder(ix);
        m_modelView->setHighlightParts(hot);
        highlightMaterialsForParts(hot);
    });
    // Keep the loaded model expanded through filter/sort resets — and SELECTED. A reset clears
    // the view's selection wholesale, which left the loaded model unhighlighted after any
    // refilter/regroup/meta-refresh until you clicked it again. Restoring the host row's
    // selection is safe: onAppearanceSelected early-returns when the SNO is already loaded.
    connect(m_treeModel, &QAbstractItemModel::modelReset, this, [this]() {
        const int r = m_treeModel->subtreeRow();
        if (r < 0) return;
        if (QSettings().value(QStringLiteral("models/outliner/autoExpand"), true).toBool())
            m_list->expand(m_treeModel->index(r, 0));
        if (!m_list->currentIndex().isValid())
            m_list->selectionModel()->setCurrentIndex(
                m_treeModel->index(r, 0),
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    });
    // Viewport → tree: double-clicking a mesh in the 3D view selects its part node (Blender's
    // click-in-viewport-highlights-in-outliner). Double-clicking the SAME part again, or empty
    // space (part = -1), deselects — the selection returns to the model's own row so the browse
    // list never ends up with nothing current.
    connect(m_modelView, &GLModelWidget::partFocused, this, [this](int part) {
        if (!m_treeModel) return;
        const int host = m_treeModel->subtreeRow();
        if (host < 0) return;
        const QModelIndex hostIx = m_treeModel->index(host, 0);
        const QModelIndex ix = part >= 0 ? m_treeModel->indexOfPart(part) : QModelIndex();
        const QModelIndex cur = m_list->currentIndex();
        const bool samePart = ix.isValid() && cur.isValid()
                              && cur.parent() == ix.parent() && cur.row() == ix.row();
        if (!ix.isValid() || samePart) {   // miss or re-click → unselect the part
            m_list->selectionModel()->setCurrentIndex(
                hostIx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            return;
        }
        m_list->expand(hostIx);
        m_list->selectionModel()->setCurrentIndex(
            ix, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_list->scrollTo(ix);
    });
    // Right-click a part in the viewport → hide/show/isolate it + copy its material name.
    connect(m_modelView, &GLModelWidget::partRightClicked, this, [this](int part, const QPoint& gp) {
        if (!m_treeModel) return;
        auto showAll = [this] {
            QHash<int, bool> all; m_treeModel->partChecks(all);
            for (auto it = all.constBegin(); it != all.constEnd(); ++it) m_treeModel->setPartCheck(it.key(), true);
            recomputePartVisibility();
        };
        QMenu menu(this);
        if (part >= 0 && part < m_curGeo.primitives.size()) {
            QHash<int, bool> checks; m_treeModel->partChecks(checks);
            const bool visible = checks.value(part, true);
            const QString matName = m_curGeo.primitives[part].materialName;
            menu.addAction(visible ? QStringLiteral("Hide this part") : QStringLiteral("Show this part"),
                           this, [this, part, visible] { m_treeModel->setPartCheck(part, !visible); recomputePartVisibility(); });
            menu.addAction(QStringLiteral("Isolate this part"), this, [this, part] {
                QHash<int, bool> all; m_treeModel->partChecks(all);
                for (auto it = all.constBegin(); it != all.constEnd(); ++it) m_treeModel->setPartCheck(it.key(), it.key() == part);
                recomputePartVisibility();
            });
            menu.addSeparator();
            menu.addAction(QStringLiteral("Copy material name"), this,
                           [matName] { QApplication::clipboard()->setText(matName); });
            menu.addSeparator();
        }
        menu.addAction(QStringLiteral("Show all parts"), this, [showAll] { showAll(); });
        menu.exec(gp);
    });
    // Double-click = expand/collapse, everywhere, deterministically. The stock
    // expandsOnDoubleClick is style-hint-dependent (a QSS-styled app can silently disable it),
    // so it's turned OFF and handled manually: model rows force-load first when needed, and any
    // node with children toggles.
    m_list->setExpandsOnDoubleClick(false);
    connect(m_list, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& ix) {
        if (!ix.parent().isValid()) {
            if (const SnoEntry* e = m_listModel->entryAt(ix.row()))
                if (e->snoId != m_curSno || !m_curGeo.valid) {
                    forceLoadSno(e->snoId);   // loads even when the row is already current
                    return;
                }
        } else if (const auto* n = m_treeModel->node(ix)) {
            if (n->kind == ModelOutlinerModel::Look) {   // double-click a look = apply it
                m_treeModel->setData(ix.siblingAtColumn(ModelOutlinerModel::kTreeCol),
                                     Qt::Checked, Qt::CheckStateRole);   // → lookToggled → applies
                return;
            }
            if (n->kind == ModelOutlinerModel::Anim && !n->aux.isEmpty()) {
                // Double-click a clip = play; double-click the playing clip = stop (same toggle
                // the ANIMATIONS list click uses — one behavior everywhere).
                if (n->aux == m_playingAnim) clearAnimationSelection();
                else                         playAnimationByName(n->aux);
                return;
            }
        }
        const QModelIndex c0 = ix.siblingAtColumn(0);   // children hang off column 0
        if (m_treeModel->rowCount(c0) > 0)
            m_list->setExpanded(c0, !m_list->isExpanded(c0));
    });
    // GRID view needs the same double-click-to-load. Without this, a grid card could never be
    // loaded once the startup restore had already selected it: the row was current, so clicking
    // it emitted no selection change, and no double-click handler existed here at all — the
    // viewport just sat on "double-click this row to load it" forever.
    if (m_gridView)
        connect(m_gridView, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& ix) {
            if (!ix.isValid() || !m_listModel) return;
            if (const SnoEntry* e = m_listModel->entryAt(ix.row())) forceLoadSno(e->snoId);
        });

    // Timeline / animation.
    m_animTimer = new QTimer(this);
    connect(m_animTimer, &QTimer::timeout, this, &ModelsTab::tickAnimation);
    connect(m_animSlider, &QSlider::valueChanged, this, [this](int v) {
        if (m_modelView) m_modelView->setFrame(v);
        const int fc = m_modelView ? m_modelView->animFrameCount() : 0;
        const float fps = m_animFps > 0 ? m_animFps : 30.0f;
        const float cur = v / fps;
        const float tot = fc > 0 ? (fc - 1) / fps : 0.0f;
        m_timeLabel->setText(QStringLiteral("%1 / %2 s")
            .arg(cur, 0, 'f', 2).arg(tot, 0, 'f', 2));
        if (m_frameSpin) {   // frame counter is the editable spinbox now
            QSignalBlocker b(m_frameSpin);
            m_frameSpin->setValue(v);
        }
        if (m_frameMax) m_frameMax->setText(QStringLiteral("/ %1").arg(fc > 0 ? fc - 1 : 0));
    });
    connect(m_playBtn, &QPushButton::clicked, this, [this]() {
        if (m_animTimer->isActive()) {
            m_animTimer->stop(); m_playBtn->setIcon(transportGlyph(0));
        } else if (m_modelView->animFrameCount() > 0) {
            // Pressing Play at the end restarts from the first frame.
            if (m_animSlider->value() >= m_modelView->animFrameCount() - 1)
                m_animSlider->setValue(0);
            m_animTimer->start(); m_playBtn->setIcon(transportGlyph(1));
        }
    });
    // Click an animation to play it; click the playing one again to stop + clear.
    connect(m_anims, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        if (!it) return;
        const QString name = it->data(Qt::UserRole).toString();
        if (name == m_playingAnim) clearAnimationSelection();
        else                       playAnimationByName(name);
    });
    m_anims->installEventFilter(this);   // Esc → clear the animation selection
    connect(m_animSearch, &QLineEdit::textChanged, this, [this](const QString& t) {
        for (int i = 0; i < m_anims->count(); ++i)
            m_anims->item(i)->setHidden(!t.isEmpty() && !m_anims->item(i)->text().contains(t, Qt::CaseInsensitive));
    });

    // Debounce timer: build the 3D geometry only after the selection settles.
    m_geoTimer = new QTimer(this);
    m_geoTimer->setSingleShot(true);
    m_geoTimer->setInterval(160);
    connect(m_geoTimer, &QTimer::timeout, this, [this]() { loadGeometry(); });

    updateIconMode();          // install the initial list icon provider
    setListIconSize(QSettings().value(QStringLiteral("models/iconPx"), 48).toInt());   // Outliner icon size

    // Hover preview: dwell 0.5s over an icon → floating popup; scroll resizes it.
    m_iconPreview = new QLabel(this, Qt::ToolTip);
    m_iconPreview->setAlignment(Qt::AlignCenter);
    m_iconPreview->hide();
    m_hoverTimer = new QTimer(this);
    m_hoverTimer->setSingleShot(true);
    m_hoverTimer->setInterval(HoverInfo::delayMs());
    m_previewPx = HoverInfo::previewPx();   // initial hover-popup size (Settings ▸ General ▸ On-hover)
    connect(m_hoverTimer, &QTimer::timeout, this, [this]() {
        if (!QSettings().value(QStringLiteral("models/hoverPreview"), true).toBool())
            return;   // File ▸ Models settings ▸ On-hover preview
        if (m_hoverTile >= 0)        showTilePreview(m_hoverTile);
        else if (m_hoverTexSno > 0)  showOutlinerTexPreview();   // outliner texture leaf
        else                         showIconPreview(m_hoverSno);
    });
    m_list->setMouseTracking(true);
    m_list->viewport()->setMouseTracking(true);
    m_list->viewport()->installEventFilter(this);
    m_list->installEventFilter(this);   // H / Shift+H / Alt+H part-visibility hotkeys

    // Restore the display mode last — everything it touches (tree model, grid button, header
    // dropdown) exists by now, and it routes through the same one-path appliers.
    applyDisplayMode(QSettings().value(QStringLiteral("models/displayMode"), 1).toInt());
    m_tagOrMode = QSettings().value(QStringLiteral("models/tagOrMode"), false).toBool();

    // The ANIMATIONS page was BUILT with the center column but registers only now — the strip
    // (right column) constructs after it. Registration order = stack index 5.
    if (m_animPage) {
        // The section header IS the label populateAnimList writes counts into — the page's own
        // in-body title would otherwise be a second, duplicate header.
        QLabel* h = addRightPage(QStringLiteral("ANIMATIONS"), m_animPage,
                                 ModelOutlinerModel::kindIcon(ModelOutlinerModel::AnimRoot),
                                 QStringLiteral("Animations — the model's clips (play, pull, export)"));
        if (h) {
            if (m_animsHdr) m_animsHdr->hide();   // retire the in-body title
            m_animsHdr = h;
        }
    }

    // ATTACHMENTS page (registration index 7): the models this actor holds/spawns (weapons,
    // shields, props). Built here so it registers after ANIMATIONS; populated per model load.
    buildAttachPage();
    if (m_attachTree) {
        m_attachPage = m_rsections.size();   // page id addRightPage is about to assign
        QLabel* h = addRightPage(QStringLiteral("ATTACHMENTS"), m_attachTree,
                                 attachStripIcon(),
                                 QStringLiteral("Attachments — other models this actor holds/spawns "
                                                "(tick one to attach it in the viewport)"));
        if (h) m_attachHdr = h;
    }

    // ── Scope-sorted, like Blender's property tabs: model-level first (INFO · ANIMATIONS ·
    // LOOKS · PARTS · ATTACHMENTS), a divider, then material/sim-level (MATERIALS · SHADING ·
    // CLOTH). Applied to BOTH the strip buttons and the section column, so the icons match the
    // reading order. Registration order: 0 LOOKS · 1 MATERIALS · 2 SHADING · 3 INFO · 4 PARTS ·
    // 5 CLOTH · 6 ANIMS · 7 ATTACHMENTS.
    static const QVector<int> kOrder{3, 6, 0, 4, 7, /*divider*/ -1, 1, 2, 5};
    if (m_rstripLay && m_rpageBtns.size() == 8) {
        while (QLayoutItem* it2 = m_rstripLay->takeAt(0)) delete it2;   // items only — widgets survive
        // (The hide/show arrow for this column lives on the VIEWPORT's N-strip — the column
        // hides completely, Blender-style, and the floating arrow brings it back.)
        for (int i : kOrder) {
            if (i < 0) {
                auto* sepLn = new QFrame(m_rstripLay->parentWidget());
                sepLn->setFrameShape(QFrame::HLine);
                sepLn->setStyleSheet(QStringLiteral("color:#555555;"));
                m_rstripLay->addWidget(sepLn);
            } else {
                m_rstripLay->addWidget(m_rpageBtns[i]);
            }
        }
        m_rstripLay->addStretch(1);
        // Same reading order for the panels. NOTE: m_rsections stays in REGISTRATION order — its
        // index is the page id every strip button and header connect is bound to; only the
        // splitter position changes.
        for (int i : kOrder)
            if (i >= 0 && i < m_rsections.size()) m_rstack->addWidget(m_rsections[i]);
    }

    // ── Restore the panel layout: which panels are up, their order, their heights.
    // Opt-in (Settings ▸ Models tab ▸ "Remember the right-hand panel layout"); default INFO+PARTS.
    {
        QSettings s;
        const bool remember = s.value(QStringLiteral("models/rememberPanels"), true).toBool();
        QStringList shown{QStringLiteral("INFO"), QStringLiteral("PARTS")};
        if (remember && s.contains(QStringLiteral("models/panels/shown")))
            shown = s.value(QStringLiteral("models/panels/shown")).toStringList();
        const QStringList heights = remember
            ? s.value(QStringLiteral("models/panels/sizes")).toStringList() : QStringList();
        m_panelRestore = true;   // don't let these toggles write a half-applied layout back out
        for (const QString& name : shown) {
            const int page = m_sectKeys.indexOf(QStringLiteral("models/panel/") + name);
            if (page < 0 || page >= m_rpageBtns.size()) continue;
            m_rstack->addWidget(m_rsections[page]);   // re-append → the up panels end up in the
                                                      // saved order, at the tail, ahead of the
                                                      // hidden ones (which take no space anyway)
            m_rpageBtns[page]->setChecked(true);      // → showPanel
        }
        m_panelRestore = false;
        // Heights, by name: walk the splitter and hand each up panel the height saved against it.
        if (heights.size() == shown.size() && !heights.isEmpty()) {
            QList<int> sizes = m_rstack->sizes();
            int k = 0;
            for (int i = 0; i < m_rstack->count() && k < heights.size(); ++i) {
                if (m_rstack->widget(i)->isHidden()) continue;
                sizes[i] = heights[k++].toInt();
            }
            m_rstack->setSizes(sizes);
        }
        // The hidden-column state (» arrow on the N-strip) restores regardless of the remember
        // setting — it's column chrome, like a splitter position. Done HERE, after the whole
        // splitter is assembled, so hiding the pane acts on the finished layout.
        if (m_sideArrow && s.value(QStringLiteral("models/panels/collapsed"), false).toBool())
            m_sideArrow->setChecked(true);   // → setSideCollapsed
    }
    // Opt-in: restore the last search AND the whole filter state (tags, combos, toggles).
    restoreFilterState();
}

void ModelsTab::updateCount()
{
    if (!m_countLabel || !m_listModel) return;
    const int shown = m_listModel->entryCount();
    const int total = m_listModel->totalCount();
    const bool filtered = shown != total;
    auto grp = [](int n) { return QLocale().toString(n); };   // thousands separators
    const QString curCat = m_catCombo ? m_catCombo->currentData().toString() : QString();
    if (m_animatedScanning
        && (curCat == QLatin1String("__animated__") || curCat == QLatin1String("__rigged__"))) {
        // The filter is honest but the index is still building — say so where the user is
        // looking, instead of a scary "0 of 67,144". The completion hook refilters + recounts.
        m_countLabel->setText(QStringLiteral("indexing animations…"));
        m_countLabel->setStyleSheet(QStringLiteral("color:#d8a23a;"));
    } else if (filtered) {
        // Accent colour + "X of Y" makes it obvious you're viewing a filtered subset.
        m_countLabel->setText(QStringLiteral("%1 of %2 models").arg(grp(shown), grp(total)));
        m_countLabel->setStyleSheet(QStringLiteral("color:#d8a23a;"));   // Diablo gold
    } else {
        m_countLabel->setText(QStringLiteral("%1 models").arg(grp(total)));
        m_countLabel->setStyleSheet(QString());
    }
    // "Filters ▾" badge: show how many filters are active even while the section is collapsed.
    if (m_filtersToggle) {
        auto comboSet = [](QComboBox* c) { return c && !c->currentData().toString().isEmpty(); };
        int nf = 0;
        if (comboSet(m_catCombo))    ++nf;
        if (comboSet(m_classCombo))  ++nf;
        if (comboSet(m_genderCombo)) ++nf;
        if (comboSet(m_typeCombo))   ++nf;
        if (m_onlyDecrypted && m_onlyDecrypted->isChecked()) ++nf;
        if (m_hideUnrenderable) ++nf;
        nf += m_tagFilter.size();   // funnel tags count as active filters too
        const QString arrow = m_filtersToggle->isChecked() ? QStringLiteral(" ▴") : QStringLiteral(" ▾");
        m_filtersToggle->setText((nf ? QStringLiteral("Filters (%1)").arg(nf) : QStringLiteral("Filters")) + arrow);
    }
    rebuildFilterChips();
    // Every filter change funnels through updateCount() → one honest place to persist the state
    // for "Remember last search" (no-ops unless the option is on).
    saveFilterState();
}

// Rebuild the inline active-filter chips (one removable pill per set filter).
void ModelsTab::rebuildFilterChips()
{
    if (!m_filterChips) return;
    QLayout* lay = m_filterChips->layout();
    if (!lay) return;
    while (QLayoutItem* it = lay->takeAt(0)) {   // clear the previous chips
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    auto addChip = [this, lay](const QString& label, const std::function<void()>& clear) {
        auto* chip = new QToolButton(m_filterChips);
        chip->setText(label + QStringLiteral("  ✕"));
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(QStringLiteral("Remove this filter"));
        // Same shape/palette as every other button (kToolBtnQss) — only the gold text marks it
        // as a filter pill. Was #333/#565656/#454545: three off-palette shades for no reason.
        chip->setStyleSheet(QStringLiteral(
            "QToolButton{background:#2b2b2b; border:1px solid #555; border-radius:3px;"
            " padding:1px 6px; color:#d8a23a;}"
            "QToolButton:hover{border-color:#b0453c;}"));
        connect(chip, &QToolButton::clicked, this, [clear]() { clear(); });
        lay->addWidget(chip);
    };
    if (m_catCombo && m_catCombo->currentIndex() > 0)
        addChip(m_catCombo->currentText(), [this] { m_catCombo->setCurrentIndex(0); });
    if (m_classCombo && m_classCombo->currentIndex() > 0)
        addChip(m_classCombo->currentText(), [this] { m_classCombo->setCurrentIndex(0); });
    if (m_genderCombo && m_genderCombo->currentIndex() > 0)
        addChip(m_genderCombo->currentText(), [this] { m_genderCombo->setCurrentIndex(0); });
    if (m_typeCombo && m_typeCombo->currentIndex() > 0)
        addChip(m_typeCombo->currentText(), [this] { m_typeCombo->setCurrentIndex(0); });
    if (m_onlyDecrypted && m_onlyDecrypted->isChecked())
        addChip(QStringLiteral("Only decrypted"), [this] { m_onlyDecrypted->setChecked(false); });
    if (m_hideBrokenChk && m_hideBrokenChk->isChecked())
        addChip(QStringLiteral("Hide un-renderable"), [this] { m_hideBrokenChk->setChecked(false); });
    // Funnel tags — removing a chip unchecks its menu action, whose toggled handler does the
    // rest (set update, re-filter, count, tint): one code path, chip or menu.
    QStringList sortedTags(m_tagFilter.begin(), m_tagFilter.end());
    sortedTags.sort(Qt::CaseInsensitive);
    for (const QString& tag : sortedTags)
        addChip(tag, [this, tag] {
            if (QCheckBox* c = m_tagChecks.value(tag)) c->setChecked(false);
        });
    // Smart-search components — one pill per parsed part; removing one rebuilds the search box
    // from whatever remains (which re-parses, so the hidden fields stay authoritative).
    auto rebuildSearch = [this](bool keepSno, bool keepName, bool keepColl) {
        if (!m_hdrSearch) return;
        QStringList parts;
        if (keepSno && !m_snoSearch->text().isEmpty())   parts << m_snoSearch->text();
        if (keepName && !m_search->text().isEmpty())     parts << m_search->text();
        if (keepColl && !m_collSearch->text().isEmpty())
            parts << QStringLiteral("c:") + m_collSearch->text();   // last — c: reads to end
        m_hdrSearch->setText(parts.join(QLatin1Char(' ')));
    };
    if (m_hdrSearch) {
        if (m_snoSearch && !m_snoSearch->text().isEmpty())
            addChip(QStringLiteral("SNO %1").arg(m_snoSearch->text()),
                    [rebuildSearch] { rebuildSearch(false, true, true); });
        if (m_search && !m_search->text().isEmpty())
            addChip(m_search->text(), [rebuildSearch] { rebuildSearch(true, false, true); });
        if (m_collSearch && !m_collSearch->text().isEmpty())
            addChip(QStringLiteral("c:%1").arg(m_collSearch->text()),
                    [rebuildSearch] { rebuildSearch(true, true, false); });
    }
}

// Background-index progress (AppearanceMeta crawl / icon-atlas build) now lives in ONE persistent
// indicator on the main-window status bar (visible on every tab), so this per-tab label stays hidden
// while idle. It's still used directly for tab-specific actions (Loading model…, Rendering icons N%).
// Set (or clear, with an empty msg) one keyed scan message and push the merged status to the app's
// floating toast. Multiple concurrent scans (anims + actors…) show together, e.g. "anims 70% · actors 40%".
void ModelsTab::setScan(const QString& key, const QString& msg)
{
    if (msg.isEmpty()) m_scan.remove(key); else m_scan.insert(key, msg);
    QStringList keys = m_scan.keys();
    keys.sort();   // stable display order regardless of QHash ordering
    QStringList parts;
    for (const QString& k : keys) parts << m_scan.value(k);
    emit scanStatus(parts.join(QStringLiteral("     ·     ")));
}

// Clear the transient per-action messages (model load / icon render). Background scans clear their
// own key when they finish, so this only drops the short-lived ones.
void ModelsTab::updateIndexStatus()
{
    setScan(QStringLiteral("load"), QString());
    setScan(QStringLiteral("render"), QString());
}

// Decode (and cache) the original 2D inventory icon for an appearance. The cache
// pins misses (null pixmap) so a row that has no original icon isn't retried.
QPixmap ModelsTab::originalIcon(int sno)
{
    const auto cit = m_origIconCache.constFind(sno);
    if (cit != m_origIconCache.constEnd())
        return cit.value();
    QPixmap pm;
    IconIndex& ii = IconIndex::instance();
    if (ii.ready() && AppearanceMeta::instance().ready()) {
        const quint32 handle = AppearanceMeta::instance().iconFor(sno);
        if (handle) {
            const QImage img = ii.iconImage(handle, m_reader);
            if (!img.isNull())
                pm = QPixmap::fromImage(img);
        }
        m_origIconCache.insert(sno, pm);   // pin (even a miss) once the index is ready
    }
    return pm;
}

// Mode-aware list icon (native resolution). 'orig' = original 2D icon only,
// '3d' = cached render only, 'both' = original with render fallback.
QPixmap ModelsTab::listIconPixmap(int sno)
{
    const QString mode = m_iconModeCombo ? m_iconModeCombo->currentData().toString()
                                         : QStringLiteral("both");
    if (mode == QLatin1String("orig") || mode == QLatin1String("both")) {
        const QPixmap pm = originalIcon(sno);
        if (!pm.isNull()) return pm;
    }
    if (mode == QLatin1String("3d") || mode == QLatin1String("both")) {
        if (!m_renderCache.contains(sno)) {   // lazily load a persisted thumbnail
            QPixmap pm;
            if (pm.load(thumbCachePath(sno))) m_renderCache.insert(sno, pm);
        }
        return m_renderCache.value(sno);
    }
    return {};
}

void ModelsTab::updateIconMode()
{
    if (!m_listModel) return;
    m_listModel->setIconProvider([this](int sno) { return listIconPixmap(sno); });
    // Dim + ⚠-flag models we already know can't be shown (crashed → blocklisted, or yield no
    // geometry) so the user sees the state in the list/grid before clicking.
    m_listModel->setFailedPredicate([this](int sno) {
        return m_renderBlocklist.contains(sno) || m_noRenderSnos.contains(sno);
    });
    // Model-presence badge: ✓ when the appearance has a mesh payload, ✗ when the icon exists but the
    // model is missing (empty CASC payload, or already known to yield no geometry / blocklisted).
    m_listModel->setPresence([this](int sno) -> int {
        if (!m_reader || !m_reader->isReady()) return 0;
        if (m_noRenderSnos.contains(sno) || m_renderBlocklist.contains(sno)) return -1;
        return m_reader->payloadSize(quint64(sno)) > 0 ? 1 : -1;
    }, QStringLiteral("models"));
}

void ModelsTab::setListIconSize(int px)
{
    px = qBound(24, px, 96);
    m_iconPx = px;
    m_list->setIconSize(QSize(px, px));
    if (m_treeModel && m_displayMode != 0) m_treeModel->setRowHeight(px + 6);   // Outliner: track icon
    m_list->setColumnWidth(1, px + 8);
    // Only drive the model's icon size for the TABLE; the grid manages its own larger size.
    if (m_listModel && (!m_gridBtn || !m_gridBtn->isChecked()))
        m_listModel->setIconPx(px);   // let the model upscale icons past their source size
    QSettings().setValue(QStringLiteral("models/iconPx"), px);   // remember across launches
}

// Column show/hide menu — shared by the header right-click and the Columns toolbar button.
void ModelsTab::showColumnMenu(const QPoint& globalPos)
{
    if (!m_list) return;
    QMenu menu(this);
    static const char* const cols[5] = {"SNO", "Icon", "FILENAME", "NAME", "COLLECTION"};
    for (int c = 0; c < 5; ++c) {
        const bool shown = !m_list->isColumnHidden(c);
        QAction* a = menu.addAction(QString::fromLatin1(cols[c]));
        a->setCheckable(true);
        a->setChecked(shown);
        connect(a, &QAction::triggered, this, [this, c, shown]() { m_list->setColumnHidden(c, shown); });
    }
    menu.exec(globalPos);
    QSettings().setValue(QStringLiteral("models/listHeader"),
                         m_list->header()->saveState());
}

// Switch the list between the table and the thumbnail-grid layout (over the same model + selection).
void ModelsTab::setGridView(bool on)
{
    if (!m_viewStack || !m_listModel || !m_gridView) return;
    m_listModel->setGridMode(on);
    if (on) {
        m_listModel->setIconPx(m_gridPx);   // bigger, upscaled thumbnails for the grid
        m_gridView->setIconSize(QSize(m_gridPx, m_gridPx));
    } else {
        m_listModel->setIconPx(m_iconPx);  // back to the table's icon size
    }
    m_viewStack->setCurrentWidget(on ? static_cast<QWidget*>(m_gridView)
                                     : static_cast<QWidget*>(m_list));
    if (m_gridBtn && m_gridBtn->isChecked() != on) {
        const bool b = m_gridBtn->blockSignals(true);
        m_gridBtn->setChecked(on);
        m_gridBtn->blockSignals(b);
    }
    if (m_colBtn) m_colBtn->setEnabled(!on);   // columns only apply to the table
    QSettings().setValue(QStringLiteral("models/gridView"), on);
    scheduleVisibleIconRender();   // fill thumbnails for whichever view just became visible
}

// Grid thumbnail size — Ctrl+scroll over the grid resizes the tiles (persisted).
void ModelsTab::setGridThumbPx(int px)
{
    if (!m_gridView) return;
    m_gridPx = qBound(48, px, 256);
    m_gridView->setIconSize(QSize(m_gridPx, m_gridPx));
    m_gridView->setGridSize(QSize(m_gridPx + 26, m_gridPx + 34));
    m_gridView->setItemDelegate(new GridItemDelegate(m_gridPx, m_gridView));
    if (m_listModel && m_gridBtn && m_gridBtn->isChecked())
        m_listModel->setIconPx(m_gridPx);   // upscale the thumbnails to the new tile size
    QSettings().setValue(QStringLiteral("models/gridPx"), m_gridPx);
    scheduleVisibleIconRender();            // re-render for the changed visible set
}

// Brief, non-blocking notification pinned to the bottom-centre of the tab (auto-hides).
void ModelsTab::showToast(const QString& msg)
{
    if (!m_toast) {
        m_toast = new QLabel(this);
        m_toast->setAlignment(Qt::AlignCenter);
        m_toast->setWordWrap(true);
        m_toast->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_toast->setStyleSheet(QStringLiteral(
            "QLabel{background:#262626;color:#eee;border:1px solid #a01818;border-radius:6px;padding:8px 14px;}"));
        m_toast->hide();
        m_toastTimer = new QTimer(this);
        m_toastTimer->setSingleShot(true);
        connect(m_toastTimer, &QTimer::timeout, this, [this] { if (m_toast) m_toast->hide(); });
    }
    m_toast->setText(msg);
    m_toast->setMaximumWidth(qMax(240, width() - 60));
    m_toast->adjustSize();
    m_toast->move(qMax(8, (width() - m_toast->width()) / 2),
                  qMax(8, height() - m_toast->height() - 26));
    m_toast->raise();
    m_toast->show();
    m_toastTimer->start(3200);
}

// Hover popup for an outliner texture leaf: decode once (cached across wheel resizes), then
// reuse the shared popupPreview placement/boundary logic the icon previews use.
void ModelsTab::showOutlinerTexPreview()
{
    if (m_hoverTexSno <= 0 || !m_iconPreview) return;
    if (m_hoverTexImgSno != m_hoverTexSno) {
        m_hoverTexImg = decodeTexImage(m_hoverTexName, m_hoverTexSno);
        m_hoverTexImgSno = m_hoverTexSno;
    }
    if (m_hoverTexImg.isNull()) { hideIconPreview(); return; }
    const QImage shown = m_hoverChan >= 0 ? channelImage(m_hoverTexImg, m_hoverChan)
                                          : m_hoverTexImg;
    popupPreview(QPixmap::fromImage(shown.scaled(
        m_previewPx, m_previewPx, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

void ModelsTab::showIconPreview(int sno)
{
    if (sno < 0 || !m_iconPreview) return;
    // The image is included ONLY when the cursor is over an icon (Icon column / grid cell) and
    // image previews are enabled. Everywhere else — and when the model simply has no 2D icon —
    // the popup is the info lines alone, so hovering a name still tells you something.
    QPixmap pm;
    if (m_hoverIconArea && HoverInfo::imagePreview()) {
        pm = listIconPixmap(sno);
        if (!pm.isNull())
            pm = pm.scaled(m_previewPx, m_previewPx, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    // Info lines under the icon, per Settings ▸ General ▸ On-hover ▸ Models tab. `cols` carries
    // each line's colour when colour-coding is on (name white, file grey, series gold, …).
    QStringList lines;
    QVector<const char*> cols;
    const bool colour = HoverInfo::colourCode();
    QString name;
    if (m_listModel)
        for (int r = 0; r < m_listModel->rowCount(); ++r)
            if (const SnoEntry* e = m_listModel->entryAt(r))
                if (e->snoId == sno) { name = e->name; break; }
    AppearanceMeta& am = AppearanceMeta::instance();
    if (HoverInfo::on("mdl/name")) { const QString t = am.titleFor(sno);
                                     if (!t.isEmpty()) { lines << t; cols << HoverInfo::Col::kName; } }
    if (HoverInfo::on("mdl/sno"))  { lines << QStringLiteral("%1 · %2").arg(sno).arg(name);
                                     cols << HoverInfo::Col::kFile; }
    if (HoverInfo::on("mdl/coll")) { const QString c = am.collectionFor(sno);
                                     if (!c.isEmpty()) { lines << c; cols << HoverInfo::Col::kSeries; } }
    if (HoverInfo::on("mdl/tags")) {
        QStringList tags(am.tagsFor(sno).begin(), am.tagsFor(sno).end());
        tags.sort(Qt::CaseInsensitive);
        if (!tags.isEmpty()) { lines << tags.join(QStringLiteral(" · ")); cols << HoverInfo::Col::kMeta; }
    }
    if (HoverInfo::on("mdl/counts") && sno == m_curSno && m_curGeo.valid) {
        qint64 v = 0, t = 0;
        for (const MeshPrimitive& p : m_curGeo.primitives) { v += p.vertices.size(); t += p.indices.size() / 3; }
        lines << QStringLiteral("%1 parts · %2 verts · %3 tris").arg(m_curGeo.primitives.size()).arg(v).arg(t);
        cols << HoverInfo::Col::kMeta;
    }
    if (HoverInfo::on("mdl/variants")) {
        const int n = m_apprVariantSnos.value(sno).size();
        if (n > 0) { lines << QStringLiteral("%1 variant(s)").arg(n); cols << HoverInfo::Col::kMeta; }
    }
    // Item-level lines (rarity / introduced-in), name-joined via the ItemHoverIndex.
    {
        ItemHoverIndex::instance().ensureBuilt(Config::d4dataDir());   // no-op once built/in-flight
        const ItemHoverIndex::Info inf = ItemHoverIndex::instance().infoFor(name);
        if (HoverInfo::on("mdl/rarity") && inf.rarity > 0) {
            lines << QStringLiteral("Rarity: %1").arg(ItemHoverIndex::rarityLabel(inf.rarity));
            cols << ItemHoverIndex::rarityColor(inf.rarity);
        }
        if (HoverInfo::on("mdl/introduced") && !inf.introducedIn.isEmpty()) {
            lines << QStringLiteral("Introduced: %1").arg(inf.introducedIn);
            cols << HoverInfo::Col::kInfo;
        }
    }
    if (HoverInfo::on("mdl/latest") && m_index && m_index->isNew(sno)) {
        lines << QStringLiteral("★ new this update"); cols << HoverInfo::Col::kNew;
    }
    if (HoverInfo::on("mdl/anim") && m_animatedScanned && !name.isEmpty()
        && !animClipsFor(sno, name.toLower()).isEmpty()) {
        lines << QStringLiteral("animated (owns / inherits clips)"); cols << HoverInfo::Col::kGood;
    }

    if (lines.isEmpty() && pm.isNull()) { hideIconPreview(); return; }   // nothing to show
    if (!lines.isEmpty()) {
        const QFontMetrics fm(font());
        const int lh = fm.height() + 1, capH = lh * lines.size() + 6;
        // Width fits the longest line when there's no image to size against.
        int w = pm.isNull() ? 0 : pm.width();
        for (const QString& s : lines) w = qMax(w, fm.horizontalAdvance(s) + 12);
        QPixmap out(qBound(200, w, 900), pm.height() + capH);
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

// Show the preview popup at the cursor, clamped to stay inside the app window (and
// thus on-screen / off the taskbar). Flips to the other side of the cursor near an edge.
void ModelsTab::popupPreview(const QPixmap& scaled)
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

void ModelsTab::showTilePreview(int idx)
{
    if (idx < 0 || idx >= 6 || !m_iconPreview || m_chanFull[idx].isNull()) return;
    popupPreview(QPixmap::fromImage(m_chanFull[idx])
        .scaled(m_previewPx, m_previewPx, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void ModelsTab::hideIconPreview()
{
    if (m_iconPreview) m_iconPreview->hide();
}

// Debounced trigger for auto-rendering thumbnails of the rows currently scrolled into view.
// Only active in the 3D / Original+3D icon modes; coalesces bursts of scroll events.
void ModelsTab::scheduleVisibleIconRender()
{
    if (!m_iconModeCombo) return;
    // Auto-rendering thumbnails on scroll drives a burst of offscreen GPU renders that can
    // crash unstable drivers. OFF by default; 3D mode still shows any cached/persisted or
    // manually-rendered thumbnails (right-click ▸ Render icon). Opt in via models/autoRender3D.
    if (!QSettings().value(QStringLiteral("models/autoRender3D"), false).toBool()) return;
    const QString mode = m_iconModeCombo->currentData().toString();
    if (mode != QLatin1String("3d") && mode != QLatin1String("both")) return;
    if (!m_visIconTimer) {
        m_visIconTimer = new QTimer(this);
        m_visIconTimer->setSingleShot(true);
        m_visIconTimer->setInterval(180);   // wait for scrolling to settle
        connect(m_visIconTimer, &QTimer::timeout, this, &ModelsTab::renderVisibleIcons);
    }
    m_visIconTimer->start();
}

// Render (and cache) 3D thumbnails for the model rows currently visible in the list — only
// the ones that don't already have a cached render. Runs when 3D/both mode is on and the
// user scrolls or switches mode. Cheap: renderIcons() skips SNOs already in m_renderCache.
void ModelsTab::renderVisibleIcons()
{
    if (!m_list || !m_listModel || !m_iconModeCombo) return;
    const QString mode = m_iconModeCombo->currentData().toString();
    if (mode != QLatin1String("3d") && mode != QLatin1String("both")) return;
    // Use whichever view is actually on screen (grid or table). The hidden view's viewport is
    // degenerate — using it made the bottom row resolve to -1 and the fallback grab the ENTIRE
    // list, so auto-render churned through all 67k rows forever (a freeze every tick).
    QAbstractItemView* view = (m_gridBtn && m_gridBtn->isChecked() && m_gridView)
                                  ? static_cast<QAbstractItemView*>(m_gridView)
                                  : static_cast<QAbstractItemView*>(m_list);
    if (!view || !view->isVisible()) return;
    const QRect vp = view->viewport()->rect();
    // Visible-edge hits can land on subtree nodes when the loaded model is expanded; walk up to
    // the top-level ancestor so the row number is a real browse row (entryAt-safe).
    auto topLevelRow = [](QModelIndex ix) {
        while (ix.parent().isValid()) ix = ix.parent();
        return ix.row();   // -1 for an invalid hit, unchanged for top-level hits
    };
    const int top = topLevelRow(view->indexAt(vp.topLeft()));
    int bot = topLevelRow(view->indexAt(vp.bottomRight()));   // bottomRight covers multi-column grids
    if (top < 0) return;
    if (bot < 0) bot = qMin(top + 200, m_listModel->rowCount() - 1);   // bounded fallback (never the whole list)
    QList<int> snos;
    bool loadedFromDisk = false;
    for (int r = top; r <= bot; ++r) {
        const SnoEntry* e = m_listModel->entryAt(r);
        if (!e || e->snoId < 0 || m_renderCache.contains(e->snoId)) continue;
        if (m_renderBlocklist.contains(e->snoId)) continue;   // known-bad model — never auto-render
        if (m_noRenderSnos.contains(e->snoId)) continue;      // tried, yields no thumbnail — don't loop on it
        QPixmap pm;                              // prefer a persisted thumbnail over re-rendering
        if (pm.load(thumbCachePath(e->snoId))) { m_renderCache.insert(e->snoId, pm); loadedFromDisk = true; }
        else                                      snos.append(e->snoId);
    }
    if (loadedFromDisk && m_listModel) m_listModel->refreshIconRange(top, bot);   // only the visible span
    if (snos.isEmpty())
        return;
    // Throttle: render only a few per tick, then hand control back to the event loop and
    // reschedule the rest. A synchronous burst of many FBO renders on heavy cloth/monster
    // meshes can hang the GPU long enough to trip the driver watchdog (TDR) and crash the
    // app; spreading the work with breathing room between ticks avoids that spike.
    constexpr int kMaxPerTick = 3;
    const QList<int> batch = snos.mid(0, kMaxPerTick);
    renderIcons(batch, false, /*quiet*/true);   // no flicker, no wait cursor during scroll
    if (snos.size() > kMaxPerTick && m_visIconTimer)
        m_visIconTimer->start();                 // more remain → continue next tick
}

void ModelsTab::refresh()
{
    if (m_loaded || m_index == nullptr || !m_index->isLoaded())
        return;
    QElapsedTimer rt; rt.start();
    loadList();
    qInfo("startup: models refresh — loadList %lld ms", rt.elapsed());
    m_loaded = true;
    // Kick off the background AppearanceMeta crawl (item type/title). Cached to
    // disk after the first build; powers the authoritative Type filter once ready.
    AppearanceMeta::instance().ensureBuilt(Config::d4dataDir(), m_index, m_reader);
    // Original 2D inventory icons need the icon-atlas index (handle → atlas + UV).
    IconIndex::instance().ensureBuilt(Config::d4dataDir());
    updateIndexStatus();   // reflect any in-progress background build
}

void ModelsTab::reset()
{
    if (m_rstackHint && !m_sideCollapsed) { m_rstackHint->show(); m_rstackHint->raise(); }   // no-model veil returns
    m_loaded = false;
    if (m_geoTimer) m_geoTimer->stop();
    m_listModel->setEntries({});
    m_matModel->setRowCount(0);
    if (m_matListModel) m_matListModel->setRowCount(0);
    if (m_vbModel) m_vbModel->setRowCount(0);
    m_matTexModel->setRowCount(0);
    if (m_matValues) m_matValues->clear();
    for (auto it = m_infoVals.constBegin(); it != m_infoVals.constEnd(); ++it)
        it.value()->setText(QStringLiteral("—"));
    if (m_looksModel) m_looksModel->setRowCount(0);
    if (m_clothModel) m_clothModel->setRowCount(0);
    if (m_partsModel) { m_partsPageSync = true; m_partsModel->setRowCount(0); m_partsPageSync = false; }
    if (m_partsHdr) m_partsHdr->setText(QStringLiteral("PARTS"));
    if (m_clothHdr) m_clothHdr->setText(QStringLiteral("CLOTH"));
    if (m_anims) m_anims->clear();
    m_animCount = 0;
    if (m_treeModel) m_treeModel->clearSubtree();
    clearTexturePreview();
    if (m_modelView) m_modelView->clearGeometry();
    if (m_exportBtn) m_exportBtn->setEnabled(false);
    m_curSno = -1;
    m_curName.clear();
    m_curGeo = ModelGeometry();
    resetAttachments();     // clear the ATTACHMENTS panel + base snapshot too
    m_attachCache.clear();  // data reload → the per-appearance attachment cache is stale
    m_renderCache.clear();  // and the accumulated 3D-thumbnail cache (bounded only by SNO count)
}

void ModelsTab::loadList()
{
    QVector<SnoEntry> entries = m_index->entries(kGroupAppearance);
    // "Only decrypted": drop appearances whose payload can't be read (encrypted with a TACT key we
    // don't have) — same criterion the Textures tab uses (payloadSize > 0).
    if (m_onlyDecrypted && m_onlyDecrypted->isChecked() && m_reader && m_reader->isReady()) {
        QVector<SnoEntry> kept; kept.reserve(entries.size());
        for (const SnoEntry& e : entries)
            if (m_reader->payloadSize(quint64(e.snoId)) > 0) kept.append(e);
        entries = kept;
    }
    m_listModel->setEntries(entries);
    updateCount();
    // If a model was mid-load when the app last died, it crashed the load (parse or, far
    // more often, the GPU upload/cloth/draw stage). Blocklist it so it never auto-loads
    // again, and forget it as the "last selected" so we don't get stuck.
    {
        QSettings s;
        const int guard = s.value(QStringLiteral("models/loadGuard"), -1).toInt();
        if (guard >= 0) {
            if (s.value(QStringLiteral("models/lastSno"), -1).toInt() == guard)
                s.setValue(QStringLiteral("models/lastSno"), -1);
            s.setValue(QStringLiteral("models/loadGuard"), -1);
            QStringList bl = s.value(QStringLiteral("models/renderBlocklist")).toStringList();
            if (!bl.contains(QString::number(guard))) {
                bl.append(QString::number(guard));
                s.setValue(QStringLiteral("models/renderBlocklist"), bl);
            }
            s.sync();
            m_renderBlocklist.insert(guard);
            QFile log(renderCrashLogPath());
            if (log.open(QIODevice::Append | QIODevice::Text)) {
                log.write(QStringLiteral("%1  crashed LOADING model sno=%2 — blocklisted (won't auto-load)\n")
                              .arg(QDateTime::currentDateTime().toString(Qt::ISODate)).arg(guard).toUtf8());
                log.close();
            }
            qWarning("loadList: blocklisted model %d that crashed loading last session", guard);
        }
    }
    // Reselect the last-viewed model if the user enabled it (File ▸ Models settings).
    if (QSettings().value(QStringLiteral("models/rememberLast"), false).toBool()) {
        const int sno = QSettings().value(QStringLiteral("models/lastSno"), -1).toInt();
        if (sno >= 0) {
            for (int r = 0; r < m_listModel->rowCount(); ++r) {
                const SnoEntry* e = m_listModel->entryAt(r);
                if (e && e->snoId == sno) {
                    // Restore the selection but DON'T auto-parse on launch — a model
                    // that crashes the parser must not be able to brick startup. The
                    // user can press Reload to load it (guarded).
                    m_skipNextAutoLoad = true;
                    const QModelIndex idx = m_treeModel->index(r, 0);   // wrapper index, not source
                    m_list->setCurrentIndex(idx);
                    m_list->scrollTo(idx, QAbstractItemView::PositionAtCenter);
                    break;
                }
            }
        }
    }
}

void ModelsTab::setInfo(const QString& key, const QString& value)
{
    if (QLabel* l = m_infoVals.value(key, nullptr))
        l->setText(value.isEmpty() ? QStringLiteral("—") : value);   // hidden sink (overlay retired)
    // The INFO page is the visible surface (and carries extra keys the old overlay didn't have).
    if (QLabel* d = m_dataVals.value(key, nullptr)) {
        d->setText(value.isEmpty() ? QStringLiteral("—") : value);
        d->setToolTip(value);   // values clip rather than widen — hover reads in full
    }
}

static QString classCodeToName(const QString& code)
{
    // Central class table (AppearanceMeta) — but keep this caller's "empty on unknown"
    // contract (it renders "—" for no-class rather than shouting an unknown code).
    return AppearanceMeta::heroClassPrefixes().contains(code.toLower())
               ? AppearanceMeta::classDisplayName(code) : QString();
}

// Shared-rig animation helpers (defined below, before ensureAnimatedIndex) — forward-declared
// here so the Animatable filter predicate can use them.
static QString animFamilyPrefix(const QString& nameLower);
static QString animLongestFamily(const QString& nameLower, const QSet<QString>& families);

// Extra searchable text for a model (its tags + collection + title), lowercased and cached, so the
// NAME search box's include/exclude terms (e.g. "-player", "-armor") match metadata, not just names.
QString ModelsTab::modelSearchBlob(int sno)
{
    const auto it = m_searchBlobCache.constFind(sno);
    if (it != m_searchBlobCache.constEnd()) return it.value();
    QString b;
    AppearanceMeta& m = AppearanceMeta::instance();
    if (m.ready()) {
        for (const QString& tg : m.tagsFor(sno)) { b += tg; b += QLatin1Char(' '); }
        b += m.collectionFor(sno);
        b += QLatin1Char(' ');
        b += m.titleFor(sno);
    }
    b = b.toLower();
    m_searchBlobCache.insert(sno, b);
    return b;
}

void ModelsTab::applyCategoryFilter()
{
    QString category        = m_catCombo->currentData().toString();
    const QString classCode = m_classCombo->currentData().toString();
    const QString gender    = m_genderCombo->currentData().toString();
    const QString typeVal   = m_typeCombo->currentData().toString();
    const QString coll      = m_collSearch ? m_collSearch->text().trimmed() : QString();

    // "Animated"/"Rigged" are pseudo-tags, independent of the appearance-meta tag set. Animated =
    // owns clips or shares a clip-owning base rig; Rigged = belongs to any base rig (⊇ Animated,
    // since a model can't be animated without a rig). Kick off the (cached) indexes on first use.
    const bool wantAnimated = (category == QLatin1String("__animated__"));
    const bool wantRigged   = (category == QLatin1String("__rigged__"));
    // Orphaned = no Actor uses it (from the authoritative entity index). (Creature / Gear were
    // retired — they duplicated the Category tags "Monster" / "Item".)
    const bool wantOrphaned = (category == QLatin1String("__orphaned__"));
    // Latest = assets added in the most recent game/d4data update (SnoIndex snapshot diff).
    const bool wantLatest   = (category == QLatin1String("__latest__"));
    if (wantAnimated) { category.clear(); ensureAnimatedIndex(); }   // don't treat it as a real tag below
    if (wantRigged) {
        category.clear();
        ensureRigIndex();
        ensureAnimatedIndex();   // rigged = animated ∪ rig-family — the union NEEDS m_animatedSnos,
                                 // which only the (async) animation index provides
    }
    if (wantOrphaned) { category.clear(); ensureEntityIndex(); }
    if (wantLatest)   { category.clear(); }

    if (AppearanceMeta::instance().ready()) {
        // Authoritative tag-based filtering. Clear the name-heuristic filters.
        m_listModel->setClassFilter(QString());
        m_listModel->setGenderFilter(QString());
        m_listModel->setTypeFilter(QString());
        const QString cls = classCode.isEmpty() ? QString() : classCodeToName(classCode);
        const QString gen = gender == "f" ? QStringLiteral("Female")
                          : gender == "m" ? QStringLiteral("Male") : QString();
        const QString typeTag = typeVal;   // combo holds authoritative tags once ready
        const QString cat = category;      // entity category tag (Actor-derived)
        const QSet<QString> tagSel = m_tagFilter;   // funnel multi-tag filter
        const bool tagOr = m_tagOrMode;             // false = ALL selected tags · true = ANY
        if (cls.isEmpty() && gen.isEmpty() && typeTag.isEmpty() && cat.isEmpty() && coll.isEmpty()
            && tagSel.isEmpty()
            && !wantAnimated && !wantRigged && !wantOrphaned && !wantLatest
            && !m_hideUnrenderable) {
            m_listModel->setPredicate(nullptr);
        } else {
            m_listModel->setPredicate([this, cls, gen, typeTag, cat, coll, tagSel, tagOr, wantAnimated,
                                       wantRigged, wantOrphaned, wantLatest](const SnoEntry& e) {
                if (m_hideUnrenderable
                    && (m_renderBlocklist.contains(e.snoId) || m_noRenderSnos.contains(e.snoId)))
                    return false;   // can't be displayed → hidden
                if (wantLatest && !(m_index && m_index->isNew(e.snoId))) return false;   // not new this update
                if (wantAnimated && !m_animatedSnos.contains(e.snoId)
                    && animLongestFamily(e.name.toLower(), m_animFamilyPrefixes).isEmpty())
                    return false;   // neither owns clips nor shares a clip-owning base rig
                if (wantRigged && !m_animatedSnos.contains(e.snoId)
                    && animLongestFamily(e.name.toLower(), m_rigFamilyPrefixes).isEmpty())
                    return false;   // not on any known base rig
                if (wantOrphaned && m_apprActors.contains(e.snoId)) return false;    // some actor uses it
                const QSet<QString> tags = AppearanceMeta::instance().tagsFor(e.snoId);
                if (!cat.isEmpty() && !tags.contains(cat)) return false;
                if (!cls.isEmpty() && !tags.contains(cls)) return false;
                if (!gen.isEmpty() && !tags.contains(gen)) return false;
                if (!typeTag.isEmpty() && !tags.contains(typeTag)) return false;
                if (!tagSel.isEmpty()) {
                    if (tagOr) {   // widening: at least one selected tag
                        bool any = false;
                        for (const QString& t : tagSel)
                            if (tags.contains(t)) { any = true; break; }
                        if (!any) return false;
                    } else {       // narrowing: every selected tag
                        for (const QString& t : tagSel)
                            if (!tags.contains(t)) return false;
                    }
                }
                if (!coll.isEmpty()
                    && !AppearanceMeta::instance().collectionFor(e.snoId).contains(coll, Qt::CaseInsensitive))
                    return false;
                return true;
            });
        }
    } else {
        // Pre-build: name-convention heuristics (+ optional Animated predicate, which is
        // independent of the appearance meta).
        m_listModel->setClassFilter(classCode);
        m_listModel->setGenderFilter(gender);
        m_listModel->setTypeFilter(typeVal);
        if (wantAnimated || wantRigged || wantOrphaned || wantLatest || m_hideUnrenderable)
            m_listModel->setPredicate([this, wantAnimated, wantRigged, wantOrphaned, wantLatest](const SnoEntry& e) {
                if (m_hideUnrenderable
                    && (m_renderBlocklist.contains(e.snoId) || m_noRenderSnos.contains(e.snoId)))
                    return false;
                if (wantLatest) return m_index && m_index->isNew(e.snoId);
                if (wantAnimated) return m_animatedSnos.contains(e.snoId)
                    || !animLongestFamily(e.name.toLower(), m_animFamilyPrefixes).isEmpty();
                if (wantRigged) return m_animatedSnos.contains(e.snoId)
                    || !animLongestFamily(e.name.toLower(), m_rigFamilyPrefixes).isEmpty();
                if (wantOrphaned) return !m_apprActors.contains(e.snoId);
                return true;
            });
        else
            m_listModel->setPredicate(nullptr);
    }
}

// ══════════════════════════════════════════════════════════════════════════════════════════════
// Filter service — the Bulk Extract tab drives the SAME filter engine through these so its results
// are byte-for-byte identical to this tab's list. The combo getters mirror the live dropdowns
// (which the meta-ready rebuild in onMetaReady() swaps to authoritative tag groups); queryEntries()
// replicates BOTH SnoListModel::rebuild (name terms + pre-ready name-convention class/gender/type)
// AND applyCategoryFilter's predicate (tag-based when ready + usage facets), reading a FilterSpec
// instead of the widgets. Keep the two in lockstep if either matcher changes.
static QVector<QPair<QString, QString>> comboItems(const QComboBox* c)
{
    QVector<QPair<QString, QString>> v;
    if (c) for (int i = 0; i < c->count(); ++i) v.append({c->itemText(i), c->itemData(i).toString()});
    return v;
}
QVector<QPair<QString, QString>> ModelsTab::filterCategoryItems() const { return comboItems(m_catCombo); }
QVector<QPair<QString, QString>> ModelsTab::filterClassItems()    const { return comboItems(m_classCombo); }
QVector<QPair<QString, QString>> ModelsTab::filterGenderItems()   const { return comboItems(m_genderCombo); }
QVector<QPair<QString, QString>> ModelsTab::filterTypeItems()     const { return comboItems(m_typeCombo); }

QVector<QPair<QString, QStringList>> ModelsTab::filterTagGroups() const
{
    QVector<QPair<QString, QStringList>> v;
    const auto g = AppearanceMeta::instance().tagGroups();
    for (auto it = g.constBegin(); it != g.constEnd(); ++it) v.append({it.key(), it.value()});
    return v;
}

void ModelsTab::ensureFilterIndexes(const QString& category)
{
    if (category == QLatin1String("__animated__")) { ensureAnimatedIndex(); }
    else if (category == QLatin1String("__rigged__")) { ensureRigIndex(); ensureAnimatedIndex(); }
    else if (category == QLatin1String("__orphaned__")) { ensureEntityIndex(); }
}

QVector<QPair<int, QString>> ModelsTab::queryEntries(int group, const FilterSpec& f)
{
    QVector<QPair<int, QString>> out;
    if (!m_index) return out;
    ensureFilterIndexes(f.category);

    // Parse the NAME box exactly like SnoListModel::setFilter (space = AND include, leading '-' = exclude).
    QStringList inc, exc;
    for (const QString& tok : f.nameSearch.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (tok.startsWith(QLatin1Char('-')) && tok.size() > 1) exc << tok.mid(1);
        else if (!tok.startsWith(QLatin1Char('-')))             inc << tok;
    }
    const bool hasNameTerms = !inc.isEmpty() || !exc.isEmpty();
    const bool ready = AppearanceMeta::instance().ready();

    // Usage facets (mirror applyCategoryFilter): a facet selection is NOT a real category tag.
    QString category = f.category;
    const bool wantAnimated = category == QLatin1String("__animated__");
    const bool wantRigged   = category == QLatin1String("__rigged__");
    const bool wantOrphaned = category == QLatin1String("__orphaned__");
    const bool wantLatest   = category == QLatin1String("__latest__");
    if (wantAnimated || wantRigged || wantOrphaned || wantLatest) category.clear();

    const QString cls = f.classCode.isEmpty() ? QString() : classCodeToName(f.classCode);
    const QString gen = f.gender == QLatin1String("f") ? QStringLiteral("Female")
                      : f.gender == QLatin1String("m") ? QStringLiteral("Male") : QString();

    for (const SnoEntry& e : m_index->entries(group)) {
        if (hasNameTerms) {   // include/exclude against the name + (meta-ready) tag/collection/title blob
            const QString hay = ready ? (e.name + QLatin1Char(' ') + modelSearchBlob(e.snoId)) : e.name;
            bool ok = true;
            for (const QString& t : inc) if (!hay.contains(t, Qt::CaseInsensitive)) { ok = false; break; }
            if (ok) for (const QString& t : exc) if (hay.contains(t, Qt::CaseInsensitive)) { ok = false; break; }
            if (!ok) continue;
        }
        if (f.hideUnrenderable
            && (m_renderBlocklist.contains(e.snoId) || m_noRenderSnos.contains(e.snoId))) continue;
        if (wantLatest && !m_index->isNew(e.snoId)) continue;   // "Latest": only assets new this update
        if (wantAnimated && !m_animatedSnos.contains(e.snoId)
            && animLongestFamily(e.name.toLower(), m_animFamilyPrefixes).isEmpty()) continue;
        if (wantRigged && !m_animatedSnos.contains(e.snoId)
            && animLongestFamily(e.name.toLower(), m_rigFamilyPrefixes).isEmpty()) continue;
        if (wantOrphaned && m_apprActors.contains(e.snoId))  continue;

        if (ready) {
            const QSet<QString> tags = AppearanceMeta::instance().tagsFor(e.snoId);
            if (!category.isEmpty()   && !tags.contains(category))  continue;
            if (!cls.isEmpty()        && !tags.contains(cls))       continue;
            if (!gen.isEmpty()        && !tags.contains(gen))       continue;
            if (!f.typeVal.isEmpty()  && !tags.contains(f.typeVal)) continue;
            if (!f.tagSel.isEmpty()) {
                if (f.tagOr) {
                    bool any = false;
                    for (const QString& t : f.tagSel) if (tags.contains(t)) { any = true; break; }
                    if (!any) continue;
                } else {
                    bool all = true;
                    for (const QString& t : f.tagSel) if (!tags.contains(t)) { all = false; break; }
                    if (!all) continue;
                }
            }
            if (!f.collection.isEmpty()
                && !AppearanceMeta::instance().collectionFor(e.snoId).contains(f.collection, Qt::CaseInsensitive))
                continue;
        } else {
            // Pre-ready: name-convention class/gender + type substring (mirrors SnoListModel::rebuild).
            if (!f.typeVal.isEmpty() && !e.name.contains(f.typeVal, Qt::CaseInsensitive)) continue;
            if (!f.classCode.isEmpty() || !f.gender.isEmpty()) {
                const QString n = e.name.toLower();
                const bool okc = n.size() >= 5 && n[4] == QLatin1Char('_')
                    && n[0].isLetter() && n[1].isLetter() && n[2].isLetter()
                    && (n[3] == QLatin1Char('f') || n[3] == QLatin1Char('m'));
                if (!okc) continue;
                if (!f.classCode.isEmpty() && n.left(3) != f.classCode) continue;
                if (!f.gender.isEmpty()    && n.mid(3, 1) != f.gender)  continue;
            }
        }
        out.append({e.snoId, e.name});
    }
    return out;
}

// ── Shared-rig ("base") animation resolution helpers ─────────────────────────
// Family prefix of a clip-owning appearance name: strip a trailing _base<NN> (the body-rig
// carrier) or a slot token, so barF_base00 → "barf" and npcF_S14_Dannica_base00 → "npcf_s14_dannica".
static QString animFamilyPrefix(const QString& nameLower)
{
    static const QRegularExpression rxBase(QStringLiteral("_base\\d*$"));
    const auto m = rxBase.match(nameLower);
    if (m.hasMatch()) return nameLower.left(m.capturedStart());
    static const QSet<QString> kSlots = {
        QStringLiteral("trs"), QStringLiteral("hlm"), QStringLiteral("leg"), QStringLiteral("glv"),
        QStringLiteral("bts"), QStringLiteral("sho"), QStringLiteral("cap"), QStringLiteral("blt"),
        QStringLiteral("hed"), QStringLiteral("bdy")};
    const int us = nameLower.lastIndexOf(QLatin1Char('_'));
    if (us > 0 && kSlots.contains(nameLower.mid(us + 1))) return nameLower.left(us);
    return nameLower;
}
// The longest `_`-boundary prefix of a model name that names a clip-owning base family
// (empty if none): npcF_S14_Dannica_TRS matches base family "npcf_s14_dannica".
static QString animLongestFamily(const QString& nameLower, const QSet<QString>& families)
{
    QString best;
    for (int i = nameLower.indexOf(QLatin1Char('_')); i > 0;
         i = nameLower.indexOf(QLatin1Char('_'), i + 1)) {
        const QString p = nameLower.left(i);
        if (families.contains(p) && p.size() > best.size()) best = p;
    }
    if (families.contains(nameLower) && nameLower.size() > best.size()) best = nameLower;
    return best;
}

// Background-scan the Anim JSON once, collecting every appearance SNO that an animation
// references (snoAppearance). Cached for the session. When it finishes, re-apply the filter
// if "Animated" is the active selection so the list fills in.
void ModelsTab::ensureAnimatedIndex()
{
    if (m_animatedScanned || m_animatedScanning) return;
    m_animatedScanning = true;
    setScan(QStringLiteral("anim"), QStringLiteral("Loading animation index…"));
    const QString animDir = QStringLiteral("%1/json/base/meta/Anim").arg(Config::d4dataDir());
    CascReader* reader = m_reader;   // thread-safe (mutex-locked reads); used for the Phase-2 rig parse
    const QString sig = d4dataSignature();
    const QString cachePath = indexCachePath(QStringLiteral("anim_index.bin"));
    std::thread([this, animDir, reader, sig, cachePath]() {
        // Load the cached index if it matches the current game/d4data build → skip the whole scan.
        AnimBlob cached;
        if (readIndexCache(cachePath, QStringLiteral("ANIMIDX2"), sig, cached)) {
            QMetaObject::invokeMethod(this, [this, cached]() {
                m_animatedSnos = cached.animatedSnos;   m_animRowsBySno = cached.rowsBySno;
                m_animFamilyPrefixes = cached.famPrefixes; m_animFamilyRows = cached.famRows;
                m_animFamilyOwner = cached.famOwner;    m_familyBones = cached.famBones;
                m_clipSet = cached.clipSet;             m_setClips = cached.setClips;
                m_femaleClips = cached.femaleClips;     m_femalePair = cached.femalePair;
                m_clipPower = cached.clipPower;
                m_animatedScanned = true;
                m_animatedScanning = false;
                m_animCache.clear();
                setScan(QStringLiteral("anim"), QString());
                if (m_curSno >= 0 && m_anims) populateAnimList(m_curSno, m_curName.toLower());
                const QString cd = m_catCombo ? m_catCombo->currentData().toString() : QString();
                if (cd == QLatin1String("__animated__") || cd == QLatin1String("__rigged__")) {
                    applyCategoryFilter(); updateCount();   // rigged widens with m_animatedSnos too
                }
            }, Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(this, [this]() {   // cache miss → announce the scan
            if (m_animatedScanning) setScan(QStringLiteral("anim"), QStringLiteral("Scanning animations… 0%"));
        }, Qt::QueuedConnection);
        QElapsedTimer idxClk; idxClk.start();   // first-run indexing timing (logged via qInfo below)
        QSet<int> found;
        QHash<int, QStringList> rowsBySno;   // authoritative anim rows per appearance SNO
        QHash<QString, QString> clipRowByName;   // clip name (lower) → its display row ("name  ·  N frames")
        // Each .ani.json may reference one or more snoAppearance blocks — capture every one, plus
        // the clip's keyframe count, matching the per-model ANIMATIONS panel's row format exactly.
        QStringList files;
        {
            QDirIterator it(animDir, QStringList{QStringLiteral("*.ani.json")}, QDir::Files);
            while (it.hasNext()) files << it.next();
        }
        clipRowByName.reserve(files.size());   // ~one clip row per .ani.json → size up-front, avoid rehashing
        found.reserve(files.size() / 4);
        // Parse every .ani.json in parallel (its snoAppearance owners + frame count), then aggregate.
        struct AnimRec { QString lower; QString row; QList<int> snos; };
        const std::vector<AnimRec> arecs = parallelMap<AnimRec>(files,
            [](const QString& path) -> AnimRec {
                static const QRegularExpression rxApp(
                    QStringLiteral("\"snoAppearance\":\\s*\\{[^{}]*?\"__raw__\":\\s*(\\d+)"));
                static const QRegularExpression rxFrames(QStringLiteral("\"nKeyframeCount\":\\s*(\\d+)"));
                AnimRec r;
                QFile jf(path);
                if (!jf.open(QIODevice::ReadOnly)) return r;
                // fromLatin1 (not fromUtf8): we only match ASCII patterns (snoAppearance ids,
                // nKeyframeCount, base/meta paths) and capture ASCII — skipping UTF-8 decoding of
                // every file is faster and cannot affect the (ASCII-only) captured values.
                const QString raw = QString::fromLatin1(jf.readAll());
                QString animName = QFileInfo(path).fileName();
                if (animName.endsWith(QLatin1String(".ani.json"))) animName.chop(9);
                r.lower = animName.toLower();
                const auto fm = rxFrames.match(raw);
                r.row = fm.hasMatch()
                    ? QStringLiteral("%1  ·  %2 frames").arg(animName, fm.captured(1)) : animName;
                auto mi = rxApp.globalMatch(raw);
                while (mi.hasNext()) { const int sno = mi.next().captured(1).toInt(); if (sno > 0) r.snos << sno; }
                return r;
            },
            [this](int d, int t) {
                QMetaObject::invokeMethod(this, [this, d, t]() {
                    if (m_animatedScanning) setScan(QStringLiteral("anim"),
                        QStringLiteral("Scanning animations… %1%").arg(t > 0 ? int(qint64(d) * 100 / t) : 100));
                }, Qt::QueuedConnection);
            }, /*installSeh=*/false, /*threadMul=*/2);   // I/O-bound loose-file scan → oversubscribe
        for (const AnimRec& r : arecs) {
            if (r.lower.isEmpty()) continue;
            clipRowByName.insert(r.lower, r.row);   // for AnimSet → clip-row resolution
            for (int sno : r.snos) {
                found.insert(sno);
                QStringList& list = rowsBySno[sno];
                if (!list.contains(r.row)) list << r.row;
            }
        }
        for (auto it = rowsBySno.begin(); it != rowsBySno.end(); ++it) it.value().sort();
        qInfo("[index] anim files: %lld parsed in %lld ms", (qint64)files.size(), idxClk.elapsed());

        // ── AnimSet index: the game's authoritative clip grouping (base/meta/AnimSet/*.ans.json).
        // Every set's ptPowerEntryList lists snoAnim (+ optional snoFemaleOverrideAnim) clips; we map
        // each clip → its set name (for provenance + display grouping) and flag female-override
        // variants. Pure game data — no name/skeleton inference.
        QHash<QString, QString> clipSet;   // clip name (lower) → AnimSet display name
        QHash<QString, QStringList> setClips;  // AnimSet name → its clip rows (for authoritative borrow)
        QSet<QString> femaleClips;         // clip names (lower) that appear as a female override
        QHash<QString, QString> femalePair;// base clip (lower) → its female-override clip (for gender swap)
        QHash<QString, QString> clipPower; // clip name (lower) → snoPower name (the action it plays)
        {
            const QString setDir = QStringLiteral("%1/json/base/meta/AnimSet").arg(Config::d4dataDir());
            QStringList setFiles;
            { QDirIterator it(setDir, QStringList{QStringLiteral("*.ans.json")}, QDir::Files);
              while (it.hasNext()) setFiles << it.next(); }
            // Parse every .ans.json in parallel into a per-file record, then aggregate serially in
            // file order so the "first set/pair/power wins" semantics stay identical to the old loop.
            struct ClipEntry { QString clip; bool female; QString orig; };
            struct SetRec { QString setName;
                            QList<ClipEntry> clips;
                            QList<QPair<QString, QString>> pairs;    // base(lower) → female clip name
                            QList<QPair<QString, QString>> powers; };// clip(lower) → power name
            const std::vector<SetRec> srecs = parallelMap<SetRec>(setFiles,
                [](const QString& path) -> SetRec {
                    // Match a snoAnim / snoFemaleOverrideAnim block and pull the referenced Anim file name.
                    static const QRegularExpression rxSetAnim(
                        QStringLiteral("\"sno(FemaleOverride)?Anim\":\\s*\\{[^{}]*?base/meta/Anim/([^\"]+?)\\.ani"));
                    // Pair a base clip with its female override IN THE SAME entry: the "(?!\"snoAnim\")"
                    // guard stops the gap crossing into the next entry, so a null female never mis-pairs.
                    static const QRegularExpression rxPair(QStringLiteral(
                        "\"snoAnim\":\\s*\\{[^{}]*?base/meta/Anim/([^\"]+?)\\.ani"
                        "(?:(?!\"snoAnim\")[\\s\\S])*?"
                        "\"snoFemaleOverrideAnim\":\\s*\\{[^{}]*?base/meta/Anim/([^\"]+?)\\.ani"));
                    // Pair the entry's Power (the action) with its clip. Group1 = power, group2 = clip.
                    static const QRegularExpression rxPower(QStringLiteral(
                        "\"snoPower\":\\s*\\{[^{}]*?base/meta/Power/([^\"]+?)\\.pow"
                        "(?:(?!\"snoPower\")[\\s\\S])*?"
                        "\"snoAnim\":\\s*\\{[^{}]*?base/meta/Anim/([^\"]+?)\\.ani"));
                    // The female-override clip shares the entry's power → label it too.
                    static const QRegularExpression rxPowerFemale(QStringLiteral(
                        "\"snoPower\":\\s*\\{[^{}]*?base/meta/Power/([^\"]+?)\\.pow"
                        "(?:(?!\"snoPower\")[\\s\\S])*?"
                        "\"snoFemaleOverrideAnim\":\\s*\\{[^{}]*?base/meta/Anim/([^\"]+?)\\.ani"));
                    SetRec r;
                    QFile sf(path);
                    if (!sf.open(QIODevice::ReadOnly)) return r;
                    // fromLatin1 (not fromUtf8): only ASCII patterns/captures here → faster, identical result.
                    const QString raw = QString::fromLatin1(sf.readAll());
                    r.setName = QFileInfo(path).fileName();
                    if (r.setName.endsWith(QLatin1String(".ans.json"))) r.setName.chop(9);
                    auto mi = rxSetAnim.globalMatch(raw);
                    while (mi.hasNext()) {
                        const auto m = mi.next();
                        const QString clip = m.captured(2).toLower();
                        if (clip.isEmpty()) continue;
                        r.clips.append({clip, !m.captured(1).isEmpty(), m.captured(2)});
                    }
                    auto pi = rxPair.globalMatch(raw);
                    while (pi.hasNext()) { const auto m = pi.next();
                        r.pairs.append({m.captured(1).toLower(), m.captured(2)}); }
                    auto qi = rxPower.globalMatch(raw);        // base clips first…
                    while (qi.hasNext()) { const auto m = qi.next();
                        r.powers.append({m.captured(2).toLower(), m.captured(1)}); }
                    auto qf = rxPowerFemale.globalMatch(raw);  // …then female overrides (same first-wins order)
                    while (qf.hasNext()) { const auto m = qf.next();
                        r.powers.append({m.captured(2).toLower(), m.captured(1)}); }
                    return r;
                },
                [this](int d, int t) {
                    QMetaObject::invokeMethod(this, [this, d, t]() {
                        if (m_animatedScanning) setScan(QStringLiteral("anim"),
                            QStringLiteral("Indexing anim sets… %1%").arg(t > 0 ? int(qint64(d) * 100 / t) : 100));
                    }, Qt::QueuedConnection);
                }, /*installSeh=*/false, /*threadMul=*/2);   // I/O-bound loose-file scan → oversubscribe
            for (const SetRec& rec : srecs) {
                if (rec.setName.isEmpty()) continue;
                QStringList& sc = setClips[rec.setName];       // creates the (possibly empty) set entry
                for (const ClipEntry& ce : rec.clips) {
                    if (!clipSet.contains(ce.clip)) clipSet.insert(ce.clip, rec.setName);  // first set wins
                    if (ce.female) femaleClips.insert(ce.clip);                            // female-override slot
                    const QString row = clipRowByName.value(ce.clip, ce.orig);             // "name · N frames"
                    if (!sc.contains(row)) sc << row;                                      // set → its clip rows
                }
                for (const auto& pr : rec.pairs)
                    if (!pr.first.isEmpty() && !pr.second.isEmpty() && !femalePair.contains(pr.first))
                        femalePair.insert(pr.first, pr.second);   // base → female clip name
                for (const auto& pw : rec.powers)
                    if (!pw.first.isEmpty() && !pw.second.isEmpty() && !clipPower.contains(pw.first))
                        clipPower.insert(pw.first, pw.second);     // clip → the action/power it plays
            }
            qInfo("[index] animset files: %lld parsed, cumulative %lld ms", (qint64)setFiles.size(), idxClk.elapsed());
        }

        // Phase 2: parse each clip-owning base rig's skeleton → its bone-name-hash set, so a model
        // with no name-family match can still be matched to its rig by bone-hash overlap. parseApp is
        // the crash-prone path, so each parse is SEH-guarded — a bad base can't kill the scan thread.
        QHash<int, QSet<quint32>> bonesBySno;
        if (reader) {
            seh::installSehTranslator();
            const QList<int> owners = rowsBySno.keys();
            const int otot = owners.size();
            int lastRp = -1;
            for (int i = 0; i < otot; ++i) {
                const int osno = owners.at(i);
                QSet<quint32> bones;
                seh::runGuarded("rigparse", [&]() {
                    const QByteArray meta = reader->readMetaBySno(quint64(osno));
                    const QByteArray payload = reader->readPayloadBySno(quint64(osno));
                    if (!meta.isEmpty() && !payload.isEmpty()) {
                        const ModelGeometry g = ModelParser::parseApp(meta, payload);
                        for (const ModelJoint& j : g.skeleton) if (j.nameHash) bones.insert(j.nameHash);
                    }
                });
                if (!bones.isEmpty()) bonesBySno.insert(osno, bones);
                const int pct = otot > 0 ? int((qint64(i + 1) * 100) / otot) : 100;
                if (pct != lastRp) {
                    lastRp = pct;
                    QMetaObject::invokeMethod(this, [this, pct]() {
                        if (m_animatedScanning)
                            setScan(QStringLiteral("anim"), QStringLiteral("Indexing rigs… %1%").arg(pct));
                    }, Qt::QueuedConnection);
                }
            }
        }

        qInfo("[index] anim TOTAL (incl. rig phase): %lld ms", idxClk.elapsed());
        QMetaObject::invokeMethod(this, [this, found, rowsBySno, bonesBySno, clipSet, setClips, femaleClips, femalePair, clipPower, sig, cachePath]() {
            m_animatedSnos = found;
            m_animRowsBySno = rowsBySno;
            m_clipSet = clipSet;             // authoritative clip → AnimSet grouping
            m_setClips = setClips;           // AnimSet → its clip rows
            m_femaleClips = femaleClips;     // female-override clip variants
            m_femalePair = femalePair;       // base → female clip (gender swap)
            m_clipPower = clipPower;         // clip → action/power name
            // Build the base-family index: each clip owner's name → family prefix → its clips, so a
            // rigged piece can inherit its base body's animations (bone-hash retargeting handles the rest).
            m_animFamilyPrefixes.clear();
            m_animFamilyRows.clear();
            m_animFamilyOwner.clear();
            m_familyBones.clear();
            if (m_index) {
                QHash<int, QString> ownerName;
                for (const SnoEntry& e : m_index->entries(kGroupAppearance))
                    if (m_animRowsBySno.contains(e.snoId)) ownerName.insert(e.snoId, e.name);
                for (auto it = m_animRowsBySno.begin(); it != m_animRowsBySno.end(); ++it) {
                    const QString nm = ownerName.value(it.key());
                    if (nm.isEmpty()) continue;
                    const QString fam = animFamilyPrefix(nm.toLower());
                    if (fam.isEmpty()) continue;
                    m_animFamilyPrefixes.insert(fam);
                    if (!m_animFamilyOwner.contains(fam)) m_animFamilyOwner.insert(fam, nm);   // base name for the tooltip
                    QStringList& fr = m_animFamilyRows[fam];
                    for (const QString& r : it.value()) if (!fr.contains(r)) fr << r;
                }
                for (auto it = m_animFamilyRows.begin(); it != m_animFamilyRows.end(); ++it) it.value().sort();
                // Phase 2: union each family's base-rig bone-hash sets (parsed on the scan thread),
                // keyed by the same family prefix, for skeleton-overlap fallback matching.
                for (auto it = bonesBySno.constBegin(); it != bonesBySno.constEnd(); ++it) {
                    const QString fam = animFamilyPrefix(ownerName.value(it.key()).toLower());
                    if (!fam.isEmpty() && !ownerName.value(it.key()).isEmpty())
                        m_familyBones[fam].unite(it.value());
                }
            }
            m_animatedScanned = true;
            m_animatedScanning = false;
            m_animCache.clear();   // drop the prefix-based cache; the map is authoritative now
            setScan(QStringLiteral("anim"), QString());   // clear the animation-scan status
            // If the current model is loaded, refresh its ANIMATIONS list (own + inherited base clips,
            // inherited ones coloured/tooltipped).
            if (m_curSno >= 0 && m_anims)
                populateAnimList(m_curSno, m_curName.toLower());
            // If the Animated/Rigged filter is active, re-apply now that the index is ready
            // (Rigged is animated ∪ rig-family, so it widens with m_animatedSnos as well).
            const QString cd2 = m_catCombo ? m_catCombo->currentData().toString() : QString();
            if (cd2 == QLatin1String("__animated__") || cd2 == QLatin1String("__rigged__")) {
                applyCategoryFilter();
                updateCount();
            }
            // Persist the whole index so the next launch skips this scan (until the game/d4data updates).
            AnimBlob blob{ m_animatedSnos, m_animRowsBySno, m_animFamilyPrefixes, m_animFamilyRows,
                           m_animFamilyOwner, m_familyBones, m_clipSet, m_setClips, m_femaleClips,
                           m_femalePair, m_clipPower };
            std::thread([blob, sig, cachePath]() {
                writeIndexCache(cachePath, QStringLiteral("ANIMIDX2"), sig, blob);
            }).detach();
        }, Qt::QueuedConnection);
    }).detach();
}

// (animRowsForModel removed — populateAnimList resolves owned + inherited base-rig clips itself,
//  with per-source colouring the plain merge here couldn't express. Nothing called it.)

// Background-scan the Actor + Item metadata once → who actually uses each appearance in-game:
// the NPCs/monsters that wear it (Actor.snoAppearance), its monster family (Actor.snoMonsterFamily),
// and the gear items that render it (Item.snoActor → that actor's appearance). Turns a filename into
// real context. Exact game data — no inference. Cached for the session; runs on the first model load.
void ModelsTab::ensureEntityIndex()
{
    if (m_entityScanned || m_entityScanning || !m_reader) return;
    m_entityScanning = true;
    setScan(QStringLiteral("entity"), QStringLiteral("Loading model-usage index…"));
    const QString d4 = Config::d4dataDir();
    const QString sig = d4dataSignature();
    const QString cachePath = indexCachePath(QStringLiteral("entity_index.bin"));
    std::thread([this, d4, sig, cachePath]() {
        // Load the cached index if it matches the current game/d4data build → skip the whole scan.
        EntityBlob cached;
        if (readIndexCache(cachePath, QStringLiteral("ENTIDX3"), sig, cached)) {
            QMetaObject::invokeMethod(this, [this, cached]() {
                m_apprActors = cached.apprActors;   m_apprActorN = cached.apprActorN;
                m_apprFamily = cached.apprFamily;
                m_apprItems  = cached.apprItems;    m_apprItemN  = cached.apprItemN;
                m_itemAppr   = cached.itemAppr;     m_apprSets   = cached.apprSets;
                m_apprVariants = cached.apprVariants; m_apprVariantSnos = cached.apprVariantSnos;
                m_apprName   = cached.apprName;
                m_entityScanned = true;
                m_entityScanning = false;
                setScan(QStringLiteral("entity"), QString());
                if (m_curSno >= 0) {
                    updateEntityInfo(m_curSno);
                    if (m_animatedScanned) populateAnimList(m_curSno, m_curName.toLower());
                    scanAttachments();   // appearance→actors now known → list held/spawned models
                }
                if (m_catCombo) {
                    const QString c = m_catCombo->currentData().toString();
                    if (c == QLatin1String("__orphaned__")) { applyCategoryFilter(); updateCount(); }
                }
            }, Qt::QueuedConnection);
            return;
        }
        constexpr int kCap = 50;   // keep per-appearance name lists bounded (shared base rigs are huge)
        QElapsedTimer idxClk; idxClk.start();   // first-run indexing timing (logged via qInfo below)

        QHash<int, QStringList> apprActors;   // appearance → capped actor names
        QHash<int, int>         apprActorN;   // appearance → true count
        QHash<int, QString>     apprFamily;   // appearance → family name
        QHash<int, int>         actorAppr;    // actor sno → its (base) appearance sno (to resolve items)
        QHash<int, QStringList> apprSets;     // appearance → AnimSet names its actors play (AUTHORITATIVE)
        QHash<int, QStringList> apprVariants; // appearance → sibling skin-variant names (same actor)
        QHash<int, QList<int>>  apprVariantSnos; // appearance → sibling variant SNOs (for jump)
        QHash<int, QString>     apprName;     // appearance sno → its short name (for variant display/menu)

        // ── Actors (parsed as JSON so appearance/animset scoping is exact, not guessed) ──────────
        {
            const QString dir = d4 + QStringLiteral("/json/base/meta/Actor");
            QStringList files;
            { QDirIterator it(dir, QStringList{QStringLiteral("*.acr.json")}, QDir::Files);
              while (it.hasNext()) files << it.next(); }
            actorAppr.reserve(files.size());   // ~one entry per actor → size up-front, avoid rehashing
            apprName.reserve(files.size());
            // Parse every actor JSON in parallel (the slow part), then aggregate the records serially
            // (identical logic to the old loop). A record is one actor's self-appearances + animsets.
            struct ActorRec {
                bool ok = false;
                int  selfSno = 0;
                int  base = 0;
                QList<int> apprs;              // self appearances (base + add-on skins), base first
                QHash<int, QString> apprName;  // appearance sno → short name
                QStringList sets;              // AnimSet names this actor plays
                QString fam;                   // monster family name
                QString name;                  // actor filename (no ext)
            };
            const std::vector<ActorRec> recs = parallelMap<ActorRec>(files,
                [](const QString& path) -> ActorRec {
                    ActorRec r;
                    QFile f(path);
                    if (!f.open(QIODevice::ReadOnly)) return r;
                    QJsonParseError pe;
                    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
                    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return r;
                    const QJsonObject o = doc.object();
                    auto nameOfAppr = [](const QJsonObject& ob) {
                        QString nm = ob.value(QStringLiteral("__targetFileName__")).toString()
                                         .section(QLatin1Char('/'), -1);
                        if (nm.endsWith(QLatin1String(".app"))) nm.chop(4);
                        return nm;
                    };
                    QSet<int> seen;
                    auto addAppr = [&](int sno, const QString& anm) {
                        if (sno > 0 && !seen.contains(sno)) {
                            seen.insert(sno); r.apprs << sno;
                            if (!anm.isEmpty()) r.apprName.insert(sno, anm);
                        }
                    };
                    const QJsonObject baseObj = o.value(QStringLiteral("snoAppearance")).toObject();
                    r.base = baseObj.value(QStringLiteral("__raw__")).toInt();
                    addAppr(r.base, nameOfAppr(baseObj));
                    std::function<void(const QJsonValue&)> gatherApp = [&](const QJsonValue& v) {
                        if (v.isObject()) {
                            const QJsonObject ob = v.toObject();
                            if (ob.value(QStringLiteral("__targetFileName__")).toString()
                                    .contains(QLatin1String("/Appearance/")))
                                addAppr(ob.value(QStringLiteral("__raw__")).toInt(), nameOfAppr(ob));
                            for (const QString& k : ob.keys()) gatherApp(ob.value(k));
                        } else if (v.isArray()) {
                            for (const QJsonValue& e : v.toArray()) gatherApp(e);
                        }
                    };
                    gatherApp(o.value(QStringLiteral("arActorAppearanceAddOnEntries")));
                    gatherApp(o.value(QStringLiteral("arCustomizationAppearances")));
                    auto gatherSets = [&](const QJsonValue& v) {
                        if (!v.isArray()) return;
                        for (const QJsonValue& e : v.toArray()) {
                            QString tf = e.toObject().value(QStringLiteral("__targetFileName__")).toString();
                            if (!tf.contains(QLatin1String("/AnimSet/"))) continue;
                            QString nm = tf.section(QLatin1Char('/'), -1);
                            if (nm.endsWith(QLatin1String(".ans"))) nm.chop(4);
                            if (!nm.isEmpty() && !r.sets.contains(nm)) r.sets << nm;
                        }
                    };
                    gatherSets(o.value(QStringLiteral("arAnimSets")));
                    gatherSets(o.value(QStringLiteral("arStoreAnimSets")));
                    // Monster family — lives under ptMonsterData[], NOT at the actor's top level.
                    // (Verified against d4data: 0/400 actors have a top-level snoMonsterFamily;
                    // ~7% have ptMonsterData[0].snoMonsterFamily. The old top-level read always
                    // came back empty, which left "Family" blank and made the Creature filter
                    // match nothing.) `name` is the family directly — no path parsing needed.
                    { const QJsonArray md = o.value(QStringLiteral("ptMonsterData")).toArray();
                      if (!md.isEmpty()) {
                          const QJsonObject fam = md.first().toObject()
                                                      .value(QStringLiteral("snoMonsterFamily")).toObject();
                          r.fam = fam.value(QStringLiteral("name")).toString();
                          if (r.fam.endsWith(QLatin1String(".mfm"))) r.fam.chop(4);
                      } }
                    r.selfSno = o.value(QStringLiteral("__snoID__")).toInt();
                    r.name = QFileInfo(path).fileName();
                    if (r.name.endsWith(QLatin1String(".acr.json"))) r.name.chop(9);
                    r.ok = true;
                    return r;
                },
                [this](int d, int t) {
                    QMetaObject::invokeMethod(this, [this, d, t]() {
                        if (m_entityScanning) setScan(QStringLiteral("entity"),
                            QStringLiteral("Indexing actors… %1%").arg(t > 0 ? int(qint64(d) * 100 / t) : 100));
                    }, Qt::QueuedConnection);
                }, /*installSeh=*/false, /*threadMul=*/2);   // I/O-bound loose-file scan → oversubscribe
            qInfo("[index] actor files: %lld parsed in %lld ms", (qint64)files.size(), idxClk.elapsed());
            for (const ActorRec& r : recs) {
                if (!r.ok) continue;
                for (auto it = r.apprName.constBegin(); it != r.apprName.constEnd(); ++it)
                    if (!apprName.contains(it.key())) apprName.insert(it.key(), it.value());
                if (r.base > 0 && r.selfSno > 0) actorAppr.insert(r.selfSno, r.base);   // items resolve via base
                for (int appr : r.apprs) {
                    int& n = apprActorN[appr]; ++n;
                    QStringList& l = apprActors[appr];
                    if (l.size() < kCap && !l.contains(r.name)) l << r.name;
                    if (!r.fam.isEmpty() && !apprFamily.contains(appr)) apprFamily.insert(appr, r.fam);
                    if (!r.sets.isEmpty()) {
                        QStringList& as = apprSets[appr];
                        for (const QString& s : r.sets) if (!as.contains(s)) as << s;
                    }
                    if (r.apprs.size() > 1) {   // sibling skin variants
                        QStringList& vs = apprVariants[appr];
                        QList<int>& vsno = apprVariantSnos[appr];
                        for (int b : r.apprs) {
                            if (b == appr) continue;
                            const QString bn = r.apprName.value(b);
                            if (!bn.isEmpty() && !vs.contains(bn)) vs << bn;
                            if (b > 0 && !vsno.contains(b)) vsno << b;
                        }
                    }
                }
            }
        }

        // ── Items → (via their actor) the appearances they render ────────────────
        QHash<int, QStringList> apprItems;
        QHash<int, int>         apprItemN;
        QHash<QString, int>     itemAppr;   // item name (original case) → appearance sno (forward jump)
        {
            const QString dir = d4 + QStringLiteral("/json/base/meta/Item");
            QStringList files;
            { QDirIterator it(dir, QStringList{QStringLiteral("*.itm.json")}, QDir::Files);
              while (it.hasNext()) files << it.next(); }
            struct ItemRec { int actor = 0; QString name; };
            const std::vector<ItemRec> recs = parallelMap<ItemRec>(files,
                [](const QString& path) -> ItemRec {
                    static const QRegularExpression rxA(
                        QStringLiteral("\"snoActor\":\\s*\\{[^{}]*?\"__raw__\":\\s*(\\d+)"));
                    ItemRec r;
                    QFile f(path);
                    if (!f.open(QIODevice::ReadOnly)) return r;
                    const auto mo = rxA.match(QString::fromUtf8(f.readAll()));
                    if (mo.hasMatch()) {
                        r.actor = mo.captured(1).toInt();
                        r.name = QFileInfo(path).fileName();
                        if (r.name.endsWith(QLatin1String(".itm.json"))) r.name.chop(9);
                    }
                    return r;
                },
                [this](int d, int t) {
                    QMetaObject::invokeMethod(this, [this, d, t]() {
                        if (m_entityScanning) setScan(QStringLiteral("entity"),
                            QStringLiteral("Indexing items… %1%").arg(t > 0 ? int(qint64(d) * 100 / t) : 100));
                    }, Qt::QueuedConnection);
                }, /*installSeh=*/false, /*threadMul=*/2);   // I/O-bound loose-file scan → oversubscribe
            qInfo("[index] item files: %lld parsed in %lld ms", (qint64)files.size(), idxClk.elapsed());
            for (const ItemRec& r : recs) {
                if (r.actor <= 0) continue;
                const int appr = actorAppr.value(r.actor, 0);
                if (appr <= 0) continue;
                int& n = apprItemN[appr]; ++n;
                QStringList& l = apprItems[appr];
                if (l.size() < kCap) l << r.name;
                itemAppr.insert(r.name, appr);   // forward: item → its model (original-case key)
            }
        }
        for (auto it = apprActors.begin(); it != apprActors.end(); ++it) it.value().sort();
        for (auto it = apprItems.begin();  it != apprItems.end();  ++it) it.value().sort();
        for (auto it = apprVariants.begin(); it != apprVariants.end(); ++it) it.value().sort();
        qInfo("[index] entity TOTAL (actors+items): %lld ms", idxClk.elapsed());

        QMetaObject::invokeMethod(this,
            [this, apprActors, apprActorN, apprFamily, apprItems, apprItemN, itemAppr, apprSets,
             apprVariants, apprVariantSnos, apprName, sig, cachePath]() {
                m_apprActors = apprActors;   m_apprActorN = apprActorN;
                m_apprFamily = apprFamily;
                m_apprItems  = apprItems;    m_apprItemN  = apprItemN;
                m_itemAppr   = itemAppr;
                m_apprSets   = apprSets;     // authoritative appearance → AnimSets
                m_apprVariants = apprVariants;   // sibling skin variants
                m_apprVariantSnos = apprVariantSnos;
                m_apprName   = apprName;
                m_entityScanned = true;
                m_entityScanning = false;
                setScan(QStringLiteral("entity"), QString());   // clear the actor/item-scan status
                if (m_curSno >= 0) {
                    updateEntityInfo(m_curSno);
                    if (m_animatedScanned) populateAnimList(m_curSno, m_curName.toLower());   // now authoritative
                    scanAttachments();   // appearance→actors now known → list held/spawned models
                }
                // If the Orphaned usage filter is active, it can now resolve → re-apply.
                if (m_catCombo) {
                    const QString c = m_catCombo->currentData().toString();
                    if (c == QLatin1String("__orphaned__")) { applyCategoryFilter(); updateCount(); }
                }
                // Persist the index so the next launch skips the 60k-actor scan (until game/d4data updates).
                EntityBlob blob{ m_apprActors, m_apprActorN, m_apprFamily, m_apprItems, m_apprItemN,
                                 m_itemAppr, m_apprSets, m_apprVariants, m_apprVariantSnos, m_apprName };
                std::thread([blob, sig, cachePath]() {
                    writeIndexCache(cachePath, QStringLiteral("ENTIDX3"), sig, blob);
                }).detach();
            }, Qt::QueuedConnection);
    }).detach();
}

// Fill the "Family / Used by / Items" info rows for the current model from the entity index.
void ModelsTab::updateEntityInfo(int sno)
{
    if (!m_entityScanned) {
        const QString hint = m_entityScanning ? QStringLiteral("indexing…") : QString();
        setInfo(QStringLiteral("Family"), hint);
        setInfo(QStringLiteral("Used by"), hint);
        setInfo(QStringLiteral("Items"), hint);
        setInfo(QStringLiteral("Variants"), hint);
        if (m_variantsBtn) m_variantsBtn->setEnabled(false);
        return;
    }
    setInfo(QStringLiteral("Family"), m_apprFamily.value(sno));
    auto summarize = [](const QStringList& names, int total) {
        if (names.isEmpty()) return QString();
        const int show = qMin(names.size(), 6);
        QString s = QStringList(names.mid(0, show)).join(QStringLiteral(", "));
        if (total > show) s += QStringLiteral("  … (+%1 more)").arg(total - show);
        return s;
    };
    setInfo(QStringLiteral("Used by"), summarize(m_apprActors.value(sno), m_apprActorN.value(sno)));
    setInfo(QStringLiteral("Items"),   summarize(m_apprItems.value(sno),  m_apprItemN.value(sno)));
    // The rows show 6 names; the index stores up to 50. Put the FULL stored list in the tooltip
    // (one per line) so the "+N more" isn't a dead end. NOT links: every actor/item here resolves
    // to the appearance you're already viewing — that's how the index is keyed — so a jump would
    // be a no-op. The tooltip is the honest affordance.
    auto fullTip = [this](const QString& key, const QStringList& names, int total) {
        QLabel* l = m_dataVals.value(key, nullptr);
        if (!l) return;
        if (names.isEmpty()) { l->setToolTip(QString()); return; }
        QString tip = names.join(QLatin1Char('\n'));
        if (total > names.size())
            tip += QStringLiteral("\n… %1 more (list capped)").arg(total - names.size());
        l->setToolTip(tip);
    };
    fullTip(QStringLiteral("Used by"), m_apprActors.value(sno), m_apprActorN.value(sno));
    fullTip(QStringLiteral("Items"),   m_apprItems.value(sno),  m_apprItemN.value(sno));

    // ── Deeper game-data facts, read from the FIRST using actor / wearing item's own record.
    // Everything is guarded: names come from the entity scan, so a missing/renamed file just
    // leaves the row at "—". (d4data schema: Actor .acr.json / Item .itm.json.)
    {
        const QString d4f = Config::d4dataDir();
        auto readMeta = [&d4f](const QString& group, const QString& ext, const QString& nm) {
            QJsonObject out;
            if (d4f.isEmpty() || nm.isEmpty()) return out;
            QFile jf(QStringLiteral("%1/json/base/meta/%2/%3.%4.json").arg(d4f, group, nm, ext));
            if (jf.open(QIODevice::ReadOnly))
                out = QJsonDocument::fromJson(jf.readAll()).object();
            return out;
        };
        auto refName = [](const QJsonObject& o, const char* key) {
            return o.value(QLatin1String(key)).toObject().value(QStringLiteral("name")).toString();
        };
        const QJsonObject acr = readMeta(QStringLiteral("Actor"), QStringLiteral("acr"),
                                         m_apprActors.value(sno).value(0));
        if (!acr.isEmpty()) {
            QStringList parts;
            const QJsonArray mon = acr.value(QStringLiteral("ptMonsterData")).toArray();
            if (!mon.isEmpty()) {
                const QString fam = refName(mon.first().toObject(), "snoMonsterFamily");
                if (!fam.isEmpty()) parts << QStringLiteral("family %1").arg(fam);
            }
            const QString tree = refName(acr, "snoAnimTree");
            if (!tree.isEmpty()) parts << tree;
            const QJsonObject sc = acr.value(QStringLiteral("tScaleRange")).toObject();
            const double s1 = sc.value(QStringLiteral("rangeValue1")).toDouble();
            const double s2 = sc.value(QStringLiteral("rangeValue2")).toDouble();
            if (s1 > 0.0)
                parts << (s2 > 0.0 && !qFuzzyCompare(s1, s2)
                              ? QStringLiteral("scale %1–%2").arg(s1, 0, 'f', 2).arg(s2, 0, 'f', 2)
                              : QStringLiteral("scale %1").arg(s1, 0, 'f', 2));
            // Death VFX groups (ptDeathData[].arDeathEffectGroups[]) — "actor_death_fire" → "fire".
            QStringList fx;
            for (const QJsonValue& dv : acr.value(QStringLiteral("ptDeathData")).toArray())
                for (const QJsonValue& gv : dv.toObject()
                                                .value(QStringLiteral("arDeathEffectGroups")).toArray()) {
                    QString g = gv.toObject().value(QStringLiteral("name")).toString();
                    g.remove(QStringLiteral("actor_death_"));
                    if (!g.isEmpty() && !fx.contains(g)) fx << g;
                }
            if (!fx.isEmpty())
                parts << QStringLiteral("death FX %1").arg(fx.mid(0, 4).join(QStringLiteral("/")));
            if (!parts.isEmpty()) setInfo(QStringLiteral("Actor"), parts.join(QStringLiteral(" · ")));
            const QJsonArray phys = acr.value(QStringLiteral("ptPhysData")).toArray();
            if (!phys.isEmpty()) {
                const QJsonObject p0 = phys.first().toObject();
                QStringList pp;
                const QString ph = refName(p0, "snoPhysics");
                if (!ph.isEmpty()) pp << ph;
                const double wind = p0.value(QStringLiteral("flWindFactor")).toDouble();
                if (wind > 0.0) pp << QStringLiteral("wind %1").arg(wind, 0, 'f', 2);
                const double resp =
                    p0.value(QStringLiteral("flPartialRagdollResponsiveness")).toDouble();
                if (resp > 0.0) pp << QStringLiteral("ragdoll resp %1").arg(resp, 0, 'f', 2);
                if (!pp.isEmpty()) setInfo(QStringLiteral("Physics"), pp.join(QStringLiteral(" · ")));
            }
        }
        const QJsonObject itm = readMeta(QStringLiteral("Item"), QStringLiteral("itm"),
                                         m_apprItems.value(sno).value(0));
        if (!itm.isEmpty()) {
            QStringList ip;
            const QString ty = refName(itm, "snoItemType");
            if (!ty.isEmpty()) ip << ty;
            const int req = itm.value(QStringLiteral("nExplicitRequiredLevel")).toInt();
            if (req > 0) ip << QStringLiteral("req lvl %1").arg(req);
            if (itm.value(QStringLiteral("bIsTransmog")).toBool()) ip << QStringLiteral("transmog");
            if (itm.value(QStringLiteral("bSeasonItem")).toBool()) ip << QStringLiteral("seasonal");
            int classes = 0;
            for (const QJsonValue& cv : itm.value(QStringLiteral("fUsableByClass")).toArray())
                if (cv.toInt() != 0 || cv.toBool()) ++classes;
            if (classes > 0) ip << QStringLiteral("%1 classes").arg(classes);
            const int dyes = itm.value(QStringLiteral("ptInitialDyes")).toArray().size();
            if (dyes > 0) ip << QStringLiteral("%1 default dyes").arg(dyes);
            if (!ip.isEmpty()) setInfo(QStringLiteral("Item facts"), ip.join(QStringLiteral(" · ")));
        }
    }
    const QStringList vars = m_apprVariants.value(sno);
    setInfo(QStringLiteral("Variants"), summarize(vars, vars.size()));
    // The INFO page's Variants row upgrades to jump LINKS (setInfo above still feeds the plain
    // overlay). Click a name → select + load that sibling.
    if (QLabel* v = m_dataVals.value(QStringLiteral("Variants"))) {
        const QList<int> vsnos = m_apprVariantSnos.value(sno);
        if (!vsnos.isEmpty()) {
            QStringList links;
            for (int s : vsnos)
                links << QStringLiteral("<a href=\"%1\" style=\"color:#8ab4f8;text-decoration:none;\">%2</a>")
                             .arg(s)
                             .arg(m_apprName.value(s, QString::number(s)).toHtmlEscaped());
            v->setText(links.join(QStringLiteral(", ")));
        }
    }
    if (m_variantsBtn) m_variantsBtn->setEnabled(!m_apprVariantSnos.value(sno).isEmpty());
}

// Popup listing the current model's skin variants (siblings on the same actor); picking one loads it.
void ModelsTab::showVariantsMenu()
{
    const QList<int> vs = m_apprVariantSnos.value(m_curSno);
    if (vs.isEmpty()) return;
    QMenu menu(this);
    for (int s : vs) {
        const QString nm = m_apprName.value(s, QStringLiteral("appearance %1").arg(s));
        menu.addAction(nm, this, [this, s]() { selectModelBySno(s); });
    }
    if (m_variantsBtn)
        menu.exec(m_variantsBtn->mapToGlobal(QPoint(0, m_variantsBtn->height())));
}

// Browse-by-item: a filterable list of every gear item, resolved (Item→Actor→Appearance) to the
// model it renders. Double-click / Open jumps the model list to that appearance and loads it.
void ModelsTab::openItemBrowser()
{
    ensureEntityIndex();   // build (or keep building) the item→model map
    if (m_itemAppr.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Browse by item"),
            m_entityScanning
                ? QStringLiteral("Still indexing items — try again in a moment.")
                : QStringLiteral("The item index isn't ready yet. Load any model once to build it, then retry."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Browse by item — %1 items").arg(m_itemAppr.size()));
    dlg.resize(440, 520);
    auto* v = new QVBoxLayout(&dlg);
    auto* filter = new QLineEdit(&dlg);
    filter->setPlaceholderText(QStringLiteral("Filter items…"));
    filter->setClearButtonEnabled(true);
    v->addWidget(filter);
    auto* list = new QListWidget(&dlg);
    v->addWidget(list, 1);
    QStringList names = m_itemAppr.keys();
    names.sort(Qt::CaseInsensitive);
    for (const QString& n : names) list->addItem(n);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Close, &dlg);
    v->addWidget(bb);
    connect(filter, &QLineEdit::textChanged, &dlg, [list](const QString& t) {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setHidden(!t.isEmpty() && !list->item(i)->text().contains(t, Qt::CaseInsensitive));
    });
    auto jump = [this, &dlg, list]() {
        QListWidgetItem* it = list->currentItem();
        if (!it) return;
        const int appr = m_itemAppr.value(it->text(), -1);
        if (appr > 0) { selectModelBySno(appr); dlg.accept(); }
    };
    connect(list, &QListWidget::itemDoubleClicked, &dlg, [jump](QListWidgetItem*) { jump(); });
    connect(bb, &QDialogButtonBox::accepted, &dlg, [jump]() { jump(); });
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    dlg.exec();
}

// Manual override: pick another model (appearance) whose animations should ALSO be listed on the
// current model, retargeted to its skeleton at play time. For cases the metadata doesn't link (e.g.
// playing a companion's clips on a wild variant). Added clips are flagged gold; "Clear pulls" resets.
// Appearance name for a SNO (empty if unknown). Cached on first use — the pull paths ask for this
// per source and the index scan is linear.
// Which animation families may a model legitimately play? Used to keep shared AnimSets from
// dumping unrelated rigs' clips into the list. Order: exact name family, then the *_base rig
// family, then skeleton (bone-hash) matches for assets whose names say nothing.
// An EMPTY result means "we could not establish a family" — callers must then expand nothing,
// never everything. Getting that backwards is what listed 20k clips on a leg armour piece.
QStringList ModelsTab::clipFamiliesFor(int sno, const QString& nameLower) const
{
    QStringList fams;
    auto add = [&fams](const QString& f) { if (!f.isEmpty() && !fams.contains(f)) fams << f; };
    add(animLongestFamily(nameLower, m_animFamilyPrefixes));
    add(animLongestFamily(nameLower, m_rigFamilyPrefixes));
    // animFamilyPrefix strips a slot/base suffix (barF_stor189_LEG → barf_stor189); the family that
    // actually owns clips is usually the shorter body prefix, so offer the stripped form too.
    const QString stripped = animFamilyPrefix(nameLower);
    if (m_animFamilyPrefixes.contains(stripped) || m_rigFamilyPrefixes.contains(stripped)) add(stripped);
    // Leading token (barf_stor189_leg → barf) when it is a known clip-owning family.
    const int us = nameLower.indexOf(QLatin1Char('_'));
    if (us > 0) {
        const QString head = nameLower.left(us);
        if (m_animFamilyPrefixes.contains(head) || m_rigFamilyPrefixes.contains(head)) add(head);
    }
    if (fams.isEmpty() && sno == m_curSno && m_curGeo.valid)
        for (const QString& f : animFamiliesBySkeleton(m_curGeo.skeleton, 0.5)) add(f);
    return fams;
}

QString ModelsTab::apprNameForSno(int sno) const
{
    if (sno <= 0 || !m_index) return QString();
    if (m_apprNameCache.isEmpty())
        for (const SnoEntry& e : m_index->entries(kGroupAppearance))
            m_apprNameCache.insert(e.snoId, e.name);
    return m_apprNameCache.value(sno);
}

// "Pull suggested": work out which base rig THIS model animates on and pull from it, with no
// dialog. Two independent routes, best first:
//   1. NAME — barF_stor157_HLM → longest family prefix that owns clips ("barf") → its _base rig.
//      This is how the game names things and is exact when it applies.
//   2. SKELETON — no name match (custom/renamed asset), so fall back to bone-hash overlap, the
//      same test the inherited-clip colouring already uses.
// Returns the chosen source SNO, or -1 with `why` explaining the miss.
int ModelsTab::suggestedAnimSource(QString* why) const
{
    if (!m_index || m_curSno < 0 || !m_curGeo.valid) {
        if (why) *why = QStringLiteral("Load a rigged model first.");
        return -1;
    }
    const QString nl = m_curName.toLower();
    QStringList fams;
    const QString byName = animLongestFamily(nl, m_animFamilyPrefixes);
    if (!byName.isEmpty()) fams << byName;
    const QString byRig = animLongestFamily(nl, m_rigFamilyPrefixes);
    if (!byRig.isEmpty() && !fams.contains(byRig)) fams << byRig;
    for (const QString& f : animFamiliesBySkeleton(m_curGeo.skeleton, 0.5))
        if (!fams.contains(f)) fams << f;

    // Pick the family's base appearance: prefer the lowest-numbered _base<NN>, which is the
    // canonical body rig (barF_base00), over later variants.
    static const QRegularExpression rxBaseName(QStringLiteral("_base(\\d*)$"));
    for (const QString& fam : fams) {
        int bestSno = -1, bestNum = 1 << 30;   // (no <climits> in this TU)
        for (const SnoEntry& e : m_index->entries(kGroupAppearance)) {
            const QString en = e.name.toLower();
            if (!en.startsWith(fam)) continue;
            const auto m = rxBaseName.match(en);
            if (!m.hasMatch()) continue;
            if (animFamilyPrefix(en) != fam) continue;      // exact family, not a longer cousin
            if (e.snoId == m_curSno) continue;              // already this model
            const QString d = m.captured(1);
            const int num = d.isEmpty() ? 0 : d.toInt();
            if (num < bestNum) { bestNum = num; bestSno = e.snoId; }
        }
        if (bestSno > 0) return bestSno;
    }
    if (why) *why = fams.isEmpty()
        ? QStringLiteral("Couldn't identify this model's rig family from its name or skeleton.")
        : QStringLiteral("Found rig family \"%1\" but no base appearance for it.").arg(fams.first());
    return -1;
}

void ModelsTab::pullSuggestedAnims()
{
    ensureAnimatedIndex();
    ensureEntityIndex();
    ensureRigIndex();
    QString why;
    const int src = suggestedAnimSource(&why);
    if (src <= 0) {
        emit scanStatus(why.isEmpty() ? QStringLiteral("No matching rig found to pull from.") : why);
        return;
    }
    const QString srcName = apprNameForSno(src);
    if (m_pullSources.contains(src)) {
        emit scanStatus(QStringLiteral("Already pulling from %1.").arg(srcName));
        return;
    }
    m_pullSources.append(src);
    if (m_pullClearBtn) m_pullClearBtn->setEnabled(true);
    if (m_curSno >= 0) populateAnimList(m_curSno, m_curName.toLower());
    emit scanStatus(QStringLiteral("Pulled animations from %1 (matched to %2).")
                        .arg(srcName, m_curName));
}

void ModelsTab::pullAnimsFromModel()
{
    if (!m_index) return;
    if (m_curSno < 0 || !m_curGeo.valid) {
        QMessageBox::information(this, QStringLiteral("Pull animations"),
            QStringLiteral("Load a rigged model first — pulled clips are retargeted to its skeleton."));
        return;
    }
    ensureAnimatedIndex();   // need the AnimSet/own-clip index to resolve a source model's clips
    ensureEntityIndex();
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Pull animations from a model"));
    dlg.resize(460, 560);
    auto* v = new QVBoxLayout(&dlg);
    v->addWidget(new QLabel(QStringLiteral("Pick a model whose animations should also be listed here.\n"
        "They are retargeted to the current model's skeleton (bone-hash), so pick a rig-compatible model."),
        &dlg));
    auto* filter = new QLineEdit(&dlg);
    filter->setPlaceholderText(QStringLiteral("Filter models…"));
    filter->setClearButtonEnabled(true);
    v->addWidget(filter);
    auto* list = new QListWidget(&dlg);
    v->addWidget(list, 1);
    for (const SnoEntry& e : m_index->entries(kGroupAppearance)) {
        auto* it = new QListWidgetItem(e.name, list);
        it->setData(Qt::UserRole, e.snoId);
    }
    list->sortItems();
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Close, &dlg);
    v->addWidget(bb);
    connect(filter, &QLineEdit::textChanged, &dlg, [list](const QString& t) {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setHidden(!t.isEmpty() && !list->item(i)->text().contains(t, Qt::CaseInsensitive));
    });
    auto add = [this, &dlg, list]() {
        QListWidgetItem* it = list->currentItem();
        if (!it) return;
        const int src = it->data(Qt::UserRole).toInt();
        if (src > 0 && src != m_curSno && !m_pullSources.contains(src)) {
            m_pullSources.append(src);
            if (m_pullClearBtn) m_pullClearBtn->setEnabled(true);
            if (m_curSno >= 0) populateAnimList(m_curSno, m_curName.toLower());
        }
        dlg.accept();
    };
    connect(list, &QListWidget::itemDoubleClicked, &dlg, [add](QListWidgetItem*) { add(); });
    connect(bb, &QDialogButtonBox::accepted, &dlg, [add]() { add(); });
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    dlg.exec();
}

// Cheap, synchronous: the family prefix of every base body appearance (name ending _base<NN>),
// whether or not it owns clips. A model whose name matches one of these belongs to a real rig —
// the "Rigged" filter proxy (a model can't be animated without a rig, so animated ⊆ rigged).
void ModelsTab::ensureRigIndex()
{
    if (m_rigIndexBuilt || !m_index) return;
    m_rigIndexBuilt = true;
    // `_base` with the digits OPTIONAL — must match animFamilyPrefix's `_base\d*$`, which is what
    // strips the token when the filter tests a name. With `\d+` here, the 84 rigs named plain
    // "<family>_base" (council_hydra_base, boss_inarius_*_base, cave_*_base…) never entered the
    // family set, so nothing on those rigs could ever match "Rigged".
    static const QRegularExpression rxBaseName(QStringLiteral("_base\\d*$"));
    for (const SnoEntry& e : m_index->entries(kGroupAppearance)) {
        const QString nl = e.name.toLower();
        if (rxBaseName.match(nl).hasMatch())
            m_rigFamilyPrefixes.insert(animFamilyPrefix(nl));
    }
}

// Fill the ANIMATIONS list for a model. Clips it owns are drawn normally; clips inherited from its
// shared base rig are drawn in a distinct colour with a tooltip naming the base they come from, so
// it's obvious which animations aren't the model's own (they play via bone-hash retargeting).
// Phase-2 fallback: pick the base rig whose bone-name-hash set best overlaps the given skeleton
// (the model's own skeleton). Returns the family prefix if the overlap is convincing, else "".
QStringList ModelsTab::animFamiliesBySkeleton(const QVector<ModelJoint>& skel, double minScore) const
{
    QStringList out;
    if (skel.isEmpty() || m_familyBones.isEmpty()) return out;
    QSet<quint32> mine;
    for (const ModelJoint& j : skel) if (j.nameHash) mine.insert(j.nameHash);
    if (mine.isEmpty()) return out;
    // SCORING. This used to be inter / min(|mine|, |fb|) — the fraction of the SMALLER set covered.
    // That makes any small skeleton match everything: a leg-armour piece is skinned to ~20 bones,
    // every humanoid rig in the game contains those same leg bones, so it scored ~1.0 against
    // hundreds of families and inherited all of their clips (measured: 20,251 rows on
    // barF_stor189_LEG). Use a symmetric Jaccard score instead — a genuine same-rig match stays
    // near 1.0, while a 20-bone subset of a 300-bone rig scores ~0.07 and is correctly rejected.
    QVector<QPair<double, QString>> ranked;
    for (auto it = m_familyBones.constBegin(); it != m_familyBones.constEnd(); ++it) {
        const QSet<quint32>& fb = it.value();
        if (fb.isEmpty()) continue;
        int inter = 0;
        for (quint32 h : mine) if (fb.contains(h)) ++inter;
        const double uni = double(mine.size() + fb.size() - inter);
        const double score = uni > 0 ? inter / uni : 0.0;
        if (score >= minScore) ranked.append({ score, it.key() });
    }
    // Best first, and only a handful. This is a GUESS used when the game data gave us nothing;
    // it should offer the closest rigs, never a union of every rig that happens to overlap.
    std::sort(ranked.begin(), ranked.end(),
              [](const QPair<double, QString>& a, const QPair<double, QString>& b) { return a.first > b.first; });
    constexpr int kMaxFamilies = 3;
    for (int i = 0; i < ranked.size() && i < kMaxFamilies; ++i) out << ranked[i].second;
    return out;
}

// Turn a raw snoPower name into a readable action label: drop the "AnimKey_" prefix, underscores→
// spaces, and split camelCase ("GetHit" → "Get Hit"). Best-effort cosmetic — never affects clip IDs.
static QString prettyPower(const QString& p)
{
    if (p.isEmpty()) return {};
    QString s = p;
    if (s.startsWith(QLatin1String("AnimKey_"), Qt::CaseInsensitive)) s = s.mid(8);
    s.replace(QLatin1Char('_'), QLatin1Char(' '));
    QString out; out.reserve(s.size() + 6);
    for (int i = 0; i < s.size(); ++i) {
        if (i > 0 && s[i].isUpper() && !s[i - 1].isUpper() && s[i - 1] != QLatin1Char(' '))
            out += QLatin1Char(' ');
        out += s[i];
    }
    return out.trimmed();
}

// The animation rows a model owns/plays, independent of any UI state. ONE definition, used both
// to fill the panel for the current model and to satisfy "Pull from…" — so "pull from X" is
// guaranteed to produce exactly what X shows, which is the whole point of the feature.
//
// Sources, in order:
//   1. the model's own clips (m_animRowsBySno) — always;
//   2. its actors' AnimSets, but ONLY clips from a family this model may play. m_apprSets maps an
//      appearance → the sets of EVERY actor referencing it, so a shared armour piece accumulates
//      unrelated rigs; expanding it raw listed ~20k clips on a leg piece;
//   3. if no set data exists at all, skeleton-compatible families (flagged as a fallback).
// An unresolvable family expands NOTHING. "Unknown" must never mean "everything".
QStringList ModelsTab::modelAnimRows(int sno, const QString& nameLower, bool* fallbackOut) const
{
    QStringList rows = m_animRowsBySno.value(sno);
    if (fallbackOut) *fallbackOut = false;
    const QStringList fams = clipFamiliesFor(sno, nameLower);
    auto inFamily = [&fams](const QString& row) {
        if (fams.isEmpty()) return false;
        const QString clip = row.section(QStringLiteral("  ·  "), 0, 0).toLower();
        for (const QString& f : fams)
            if (clip == f || clip.startsWith(f + QLatin1Char('_'))) return true;
        return false;
    };
    const QStringList authSets = m_apprSets.value(sno);
    if (!authSets.isEmpty() && !fams.isEmpty()) {
        for (const QString& s : authSets)
            for (const QString& r : m_setClips.value(s))
                if (inFamily(r) && !rows.contains(r)) rows << r;
    } else if (authSets.isEmpty() && m_entityScanned && sno == m_curSno && m_curGeo.valid
               && !m_suppressSkelFallback) {
        // Last resort: no set data at all, so guess by skeleton. Bounded — a symmetric score and at
        // most a few closest rigs (see animFamiliesBySkeleton). Flagged unconfirmed in the list, and
        // suppressible: "Clear pulls" turns these off too, because they are not this model's own
        // clips and they only appear once the entity scan finishes (which opening the Pull dialog
        // triggers), so they otherwise look like pulls that refused to clear.
        if (fallbackOut) *fallbackOut = true;
        const QStringList skelFams = animFamiliesBySkeleton(m_curGeo.skeleton, 0.5);
        for (const QString& f : skelFams)
            for (const QString& r : m_animFamilyRows.value(f))
                if (!rows.contains(r)) rows << r;
        if (qEnvironmentVariableIsSet("D4_DUMP_ANIMS"))
            qInfo("[anims]   skeleton fallback: bones=%d families=[%s]",
                  (int)m_curGeo.skeleton.size(), qPrintable(skelFams.join(QLatin1Char(','))));
    }
    return rows;
}

void ModelsTab::populateAnimList(int sno, const QString& nameLower)
{
    if (!m_anims) return;
    m_anims->clear();
    const QStringList own = m_animRowsBySno.value(sno);
    const QSet<QString> ownSet(own.begin(), own.end());
    // AUTHORITATIVE animation set: the AnimSets the game assigns to this appearance's actors
    // (Actor.arAnimSets, resolved to clip rows). This is exactly what plays on the model — no name
    // or skeleton guessing. Only when NO actor uses this appearance (a runtime-applied costume that
    // leaves no explicit assignment) do we fall back to skeleton-compatible rigs, and those clips are
    // clearly flagged as unconfirmed rather than mixed in as if they were real.
    bool fallback = false;
    QStringList rows = modelAnimRows(sno, nameLower, &fallback);
    // Manual pulls. DEFINITION: "Pull from X" lists exactly the animations X itself lists — nothing
    // more. It used to add X's own clips AND re-expand X's AnimSets, which is where the extra
    // ~19,500 rows came from. Reusing modelAnimRows() makes the two views agree by construction:
    // whatever the source model shows is what gets pulled.
    QSet<QString> pulledRows;
    for (int src : m_pullSources) {
        const QStringList srcRows = modelAnimRows(src, apprNameForSno(src).toLower(), nullptr);
        for (const QString& r : srcRows)
            if (!rows.contains(r)) { rows << r; pulledRows.insert(r); }
        if (qEnvironmentVariableIsSet("D4_DUMP_ANIMS"))
            qInfo("[anims] pull src=%d '%s' → %d rows from source; list now %d",
                  src, qPrintable(apprNameForSno(src)), (int)srcRows.size(), (int)rows.size());
    }
    if (qEnvironmentVariableIsSet("D4_DUMP_ANIMS"))
        qInfo("[anims] model=%d '%s' own=%d fallback=%d pulls=%d → %d rows",
              sno, qPrintable(nameLower), (int)own.size(), (int)fallback,
              (int)m_pullSources.size(), (int)rows.size());
    // Order by the game's authoritative AnimSet (base/meta/AnimSet), then clip name, so the panel
    // reads as the real grouped sets (nav / combat / death / events…) instead of one flat A–Z list.
    // Clips with no set fall to the end under "(ungrouped)".
    auto clipOf = [](const QString& row) { return row.section(QStringLiteral("  ·  "), 0, 0).toLower(); };
    auto setOf  = [this, &clipOf](const QString& row) {
        const QString s = m_clipSet.value(clipOf(row));
        return s.isEmpty() ? QStringLiteral("￿(ungrouped)") : s;   // ￿ sorts last
    };
    std::sort(rows.begin(), rows.end(), [&](const QString& a, const QString& b) {
        const QString sa = setOf(a), sb = setOf(b);
        if (sa != sb) return sa.compare(sb, Qt::CaseInsensitive) < 0;
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    QString curSet;   // header row inserted whenever the set changes
    for (const QString& r : rows) {
        const QString setName = m_clipSet.value(clipOf(r));
        if (setName != curSet) {   // group header (non-selectable, no export)
            curSet = setName;
            auto* hdr = new QListWidgetItem(setName.isEmpty() ? QStringLiteral("— (ungrouped) —")
                                                              : QStringLiteral("— %1 —").arg(setName), m_anims);
            hdr->setFlags(Qt::NoItemFlags);   // not selectable/checkable → never exported or played
            QFont hf = hdr->font(); hf.setBold(true); hdr->setFont(hf);
            hdr->setForeground(QColor(0x9a, 0x9a, 0x9a));   // AnimSet name = a section title, not
            // state — neutral like every other header (gold stays for pulled/compatible clips).
        }
        // Lead the row with the readable action (from the clip's snoPower) so the list scans as
        // "Walk / Idle / Attack / Get Hit…" instead of raw filenames. UserRole keeps the real clip id.
        const QString action = prettyPower(m_clipPower.value(clipOf(r)));
        const QString label = action.isEmpty() ? r : QStringLiteral("%1   —   %2").arg(action, r);
        auto* item = new QListWidgetItem(label, m_anims);
        item->setData(Qt::UserRole, r.section(QStringLiteral("  ·  "), 0, 0));
        QStringList tip;
        if (!action.isEmpty()) tip << QStringLiteral("Action: %1").arg(action);
        if (!setName.isEmpty()) tip << QStringLiteral("AnimSet: %1").arg(setName);
        if (m_femaleClips.contains(clipOf(r))) tip << QStringLiteral("Female-override variant");
        if (pulledRows.contains(r)) {            // user explicitly pulled this from another model
            item->setForeground(QColor(0xd8, 0xa2, 0x3a));   // gold = manually pulled
            item->setData(Qt::UserRole + 1, true);           // marker: NOT one of this model's own clips
            tip << QStringLiteral("Pulled from a model you selected — retargeted to this skeleton.");
        } else if (fallback && !ownSet.contains(r)) {   // skeleton-compatible only — not confirmed
            item->setForeground(QColor(0x6f, 0xb7, 0xd4));   // muted cyan = "compatible, unconfirmed"
            tip << QStringLiteral("Compatible with this model's skeleton — no explicit in-game "
                                  "assignment found (best-effort, may not actually play).");
        }
        if (!tip.isEmpty()) item->setToolTip(tip.join(QLatin1Char('\n')));
    }
    // The button clears BOTH pulled and unconfirmed rows, so it must be live whenever either is on
    // screen — previously it only tracked pulls and sat dead next to a list full of cyan guesses.
    if (m_pullClearBtn)
        m_pullClearBtn->setEnabled(!m_pullSources.isEmpty() || (fallback && rows.size() > own.size()));
    setInfo(QStringLiteral("Animations"), QString::number(rows.size()));
    m_animCount = rows.size();   // outliner badge ("Animations · N") reads this at subtree build
    if (m_animsHdr)
        m_animsHdr->setText(rows.isEmpty() ? QStringLiteral("ANIMATIONS")
                                           : QStringLiteral("ANIMATIONS · %1").arg(rows.size()));
}

// Reverse-map appearance name → the AppearanceSet registries containing it (BarbF_Armor, …).
// d4data ships only ~20 .aps.json files (one per class/gender armor registry plus a few misc),
// so this loads synchronously on first use — no cache or thread ceremony required.
void ModelsTab::ensureAppearanceSets()
{
    if (m_appSetsLoaded) return;
    m_appSetsLoaded = true;
    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty()) return;
    QDir dir(QStringLiteral("%1/json/base/meta/AppearanceSet").arg(d4));
    for (const QString& fn : dir.entryList({QStringLiteral("*.aps.json")}, QDir::Files)) {
        QFile f(dir.filePath(fn));
        if (!f.open(QIODevice::ReadOnly)) continue;
        QString setName = fn;
        setName.chop(9);   // ".aps.json"
        const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).object()
                                   .value(QStringLiteral("arAppearances")).toArray();
        for (const QJsonValue& av : arr) {
            const QString nm = av.toObject().value(QStringLiteral("name")).toString().toLower();
            if (!nm.isEmpty()) m_appSetsByName[nm] << setName;
        }
    }
}

void ModelsTab::applyGrouping()
{
    if (!m_listModel || !m_groupCombo) return;
    const QString key = m_groupCombo->currentData().toString();
    if (key.isEmpty()) { m_listModel->setGroupKey(nullptr); return; }

    if (key == QLatin1String("collection")) {
        m_listModel->setGroupKey([](const SnoEntry& e) {
            return AppearanceMeta::instance().collectionFor(e.snoId);
        });
    } else if (key == QLatin1String("appset")) {
        // Game-authored registries (BarbF_Armor, …) from AppearanceSet records.
        ensureAppearanceSets();
        m_listModel->setGroupKey([this](const SnoEntry& e) {
            const QStringList s = m_appSetsByName.value(e.name.toLower());
            return s.isEmpty() ? QStringLiteral("(no appearance set)") : s.first();
        });
    } else if (key == QLatin1String("set")) {
        // Drop the gender char so a piece's male + female variants group together:
        // "barF_stor248_TRS" / "barM_stor248_TRS" → "bar_stor248_trs".
        m_listModel->setGroupKey([](const SnoEntry& e) {
            const QString n = e.name.toLower();
            if (n.size() >= 5 && n[4] == QLatin1Char('_')
                && (n[3] == QLatin1Char('f') || n[3] == QLatin1Char('m')))
                return n.left(3) + n.mid(4);
            return n;
        });
    } else if (key == QLatin1String("class")) {
        m_listModel->setGroupKey([this](const SnoEntry& e) {
            const QString n = e.name.toLower();
            if (n.size() >= 4 && n[0].isLetter() && n[1].isLetter() && n[2].isLetter())
                return classCodeToName(n.left(3));
            return QStringLiteral("(other)");
        });
    } else if (key == QLatin1String("type")) {
        m_listModel->setGroupKey([](const SnoEntry& e) {
            const QString suf = e.name.section(QLatin1Char('_'), -1).toUpper();
            return suf.isEmpty() ? e.name : suf;   // slot tag (TRS/HLM/…)
        });
    } else if (key == QLatin1String("category")) {
        // Match each appearance's tags against the known entity-category set.
        const QStringList cats = AppearanceMeta::instance()
                                     .tagGroups().value(QStringLiteral("Category"));
        const QSet<QString> catSet(cats.begin(), cats.end());
        m_listModel->setGroupKey([catSet](const SnoEntry& e) {
            for (const QString& t : AppearanceMeta::instance().tagsFor(e.snoId))
                if (catSet.contains(t)) return t;
            return QString();
        });
    }
}

void ModelsTab::onMetaReady()
{
    m_metaPct = -1;
    updateIndexStatus();
    // Swap the heuristic Type list for the authoritative item-type tags.
    const auto groups = AppearanceMeta::instance().tagGroups();

    // Fill the funnel panel's scroll area: one gold group header + checkbox per tag. AND
    // semantics by default — an entry must carry every selected tag (narrows 67k rows fastest).
    if (m_tagPanelBody) {
        QLayout* bl = m_tagPanelBody->layout();
        while (QLayoutItem* it2 = bl->takeAt(0)) {   // clear the placeholder / previous fill
            if (it2->widget()) it2->widget()->deleteLater();
            delete it2;
        }
        m_tagChecks.clear();
        auto* bodyLay = static_cast<QVBoxLayout*>(bl);
        for (auto it2 = groups.constBegin(); it2 != groups.constEnd(); ++it2) {
            if (it2.value().isEmpty()) continue;
            auto* gw = new QWidget(m_tagPanelBody);
            gw->setObjectName(QStringLiteral("tagGroup"));   // the search handler finds these
            auto* gl2 = new QVBoxLayout(gw);
            gl2->setContentsMargins(0, 2, 0, 2);
            gl2->setSpacing(2);
            auto* hdr2 = new QLabel(QStringLiteral("%1 (%2)").arg(it2.key()).arg(it2.value().size()), gw);
            hdr2->setStyleSheet(QLatin1String(kHdrQss));
            gl2->addWidget(hdr2);
            for (const QString& tag : it2.value()) {
                auto* c = new QCheckBox(tag, gw);
                c->setChecked(m_tagFilter.contains(tag));
                m_tagChecks.insert(tag, c);
                connect(c, &QCheckBox::toggled, this, [this, tag](bool on) {
                    if (on) m_tagFilter.insert(tag); else m_tagFilter.remove(tag);
                    applyCategoryFilter();
                    updateCount();
                    updateTagButtonTint();
                });
                gl2->addWidget(c);
            }
            bodyLay->addWidget(gw);
        }
        bodyLay->addStretch(1);
        updateTagButtonTint();
    }
    const QStringList types = groups.value(QStringLiteral("Type"));
    m_typeCombo->blockSignals(true);
    m_typeCombo->clear();
    m_typeCombo->addItem(QStringLiteral("All types"), QString());
    for (const QString& t : types) m_typeCombo->addItem(t, t);
    m_typeCombo->blockSignals(false);
    // Entity Category dropdown (Actor-derived): Player / Monster / Boss / NPC / …
    const QStringList cats = groups.value(QStringLiteral("Category"));
    const QString prevCat = m_catCombo->currentData().toString();   // keep the user's selection across the rebuild
    m_catCombo->blockSignals(true);
    m_catCombo->clear();
    m_catCombo->addItem(QStringLiteral("All Tags"), QString());
    m_catCombo->addItem(QStringLiteral("Latest (new this update)"), QStringLiteral("__latest__"));
    m_catCombo->addItem(QStringLiteral("Animated"), QStringLiteral("__animated__"));   // owns/inherits clips
    m_catCombo->addItem(QStringLiteral("Rigged"), QStringLiteral("__rigged__"));        // belongs to a base rig
    m_catCombo->addItem(QStringLiteral("Orphaned (no actor)"), QStringLiteral("__orphaned__"));
    for (const QString& c : cats) m_catCombo->addItem(c, c);
    { const int i = m_catCombo->findData(prevCat); if (i >= 0) m_catCombo->setCurrentIndex(i); }
    m_catCombo->blockSignals(false);
    applyCategoryFilter();
    // Now that tags/collection/title exist, let the NAME search match them too (so include/exclude
    // terms like "-player" / "-armor" work on metadata). Cached per SNO.
    m_searchBlobCache.clear();
    m_listModel->setSearchBlob([this](int sno) { return modelSearchBlob(sno); });
    applyGrouping();                     // Collection/Category grouping needs the meta
    m_listModel->metaColumnsUpdated();   // fill NAME/COLLECTION columns now that data exists
    m_origIconCache.clear();             // iconFor() is now available
    m_listModel->refreshIcons();
    // Refresh the Title field for the current selection.
    if (m_curSno >= 0)
        setInfo(QStringLiteral("Title"), AppearanceMeta::instance().titleFor(m_curSno));

    emit filtersChanged();   // combos now hold authoritative tag groups → the Bulk tab re-syncs
}

void ModelsTab::onAppearanceSelected()
{
    const QModelIndex cur = m_list->currentIndex();
    if (!cur.isValid())
        return;
    // Subtree nodes (parts, armature, materials…) are NOT browse rows — entryAt(cur.row())
    // would alias some unrelated model. Route them to the node handler instead.
    if (cur.parent().isValid()) {
        onOutlinerNodeSelected(cur);
        return;
    }
    if (const SnoEntry* e = m_listModel->entryAt(cur.row())) {
        if (e->snoId == m_curSno) return;   // clicking back onto the loaded model's row: keep the
                                            // subtree + panels instead of a pointless reload cycle
        showAppearance(e->snoId, e->name);
    }
}

// A subtree node was selected — drive the detail panels the way Blender's outliner drives its
// properties editor. Reuses the existing tables' selection plumbing (selectRow triggers the same
// slots a manual click would), so there is exactly one code path per panel.
void ModelsTab::onOutlinerNodeSelected(const QModelIndex& ix)
{
    if (!m_treeModel) return;
    const ModelOutlinerModel::Node* n = m_treeModel->node(ix);
    if (!n) return;
    // For material-scoped nodes: find the owning Material ancestor and select its App-Materials
    // row — that fills the detail tabs via the one existing code path (showMaterialTextures).
    // The row is resolved BY NAME at click time (the build-time ref can go stale after a look
    // change), and the MATERIALS pane switches to the App Materials tab so the selection — with
    // its color/rough/metal values line — is actually visible, not made on a hidden tab.
    auto selectMaterialOf = [this](const ModelOutlinerModel::Node* node) {
        const ModelOutlinerModel::Node* mat = node;
        while (mat && mat->kind != ModelOutlinerModel::Material) mat = mat->parent;
        if (!mat || !m_mats || !m_matModel) return;
        int row = -1;
        for (int r = 0; r < m_matModel->rowCount(); ++r)
            if (QStandardItem* it = m_matModel->item(r, 1))
                if (it->text() == mat->text) { row = r; break; }
        if (row < 0) row = mat->ref;   // fallback: the row located at build time
        if (row < 0 || row >= m_matModel->rowCount()) return;
        if (m_matTabs) m_matTabs->setCurrentIndex(0);   // App Materials
        m_mats->selectRow(row);                         // → onMaterialSelected → values/textures/preview
        m_mats->scrollTo(m_matModel->index(row, 0));
    };
    // NOTE: none of these force a panel open any more. Which panels are up is the user's choice
    // (strip toggles); auto-showing on every click is what made the column creep to fully-open.
    // We still drive the SELECTION inside whatever panels they've chosen to keep visible.
    switch (n->kind) {
    case ModelOutlinerModel::Material:
        selectMaterialOf(n);
        showMaterialChannels();   // restore the COLOR/ROUGH/METAL… strip — a texture leaf may have
                                  // swapped in its RGBA split, and selectRow no-ops on the same row
        break;
    case ModelOutlinerModel::TexGroup:
    case ModelOutlinerModel::ValueGroup:
    case ModelOutlinerModel::ShaderGroup:
        selectMaterialOf(n);
        if (m_detailTabs && n->ref >= 0 && n->ref < m_detailTabs->count())
            m_detailTabs->setCurrentIndex(n->ref);
        if (n->kind == ModelOutlinerModel::TexGroup)
            showMaterialChannels();   // "Textures" node = the material's channel tileset
        break;
    // Leaves: fill the tabs, open the right one, and select the leaf's row — for a texture that
    // also fires the GPU preview (onMatTexSelected), exactly like clicking the table row.
    case ModelOutlinerModel::Texture:
        selectMaterialOf(n);
        if (m_detailTabs) m_detailTabs->setCurrentIndex(0);
        if (m_matTex && n->ref >= 0 && n->ref < m_matTexModel->rowCount())
            m_matTex->selectRow(n->ref);
        break;
    case ModelOutlinerModel::Value:
        selectMaterialOf(n);
        if (m_detailTabs) m_detailTabs->setCurrentIndex(1);
        if (m_matValView && n->ref >= 0 && n->ref < m_matValModel->rowCount())
            m_matValView->selectRow(n->ref);
        break;
    case ModelOutlinerModel::Shader:
        selectMaterialOf(n);
        if (m_detailTabs) m_detailTabs->setCurrentIndex(2);
        if (m_shaderView && n->ref >= 0 && n->ref < m_shaderModel->rowCount())
            m_shaderView->selectRow(n->ref);
        break;
    case ModelOutlinerModel::AnimRoot:
        break;
    case ModelOutlinerModel::Anim:
        if (m_anims && n->ref >= 0 && n->ref < m_anims->count())
            m_anims->setCurrentRow(n->ref);   // sync the list row (no playback — dblclick plays)
        break;
    case ModelOutlinerModel::Part:
        if (m_partsView && m_partsModel) {
            QSignalBlocker b(m_partsView->selectionModel());   // don't bounce the highlight back
            for (int r = 0; r < m_partsModel->rowCount(); ++r)
                if (m_partsModel->item(r, 0)->data(Qt::UserRole).toInt() == n->ref) {
                    m_partsView->selectRow(r);
                    m_partsView->scrollTo(m_partsModel->index(r, 0));
                    break;
                }
        }
        // Also select this part's material so the MATERIALS + textures panels populate for the
        // picked part (parity with clicking a material node) — a viewport part-pick then fills in
        // exactly what that part is made of.
        for (const ModelOutlinerModel::Node* ch : n->kids)
            if (ch && ch->kind == ModelOutlinerModel::Material) { selectMaterialOf(ch); break; }
        break;
    default:
        break;   // Bone: highlight + the temporary skeleton overlay live in the
                 // selection-changed hook (which also handles DE-selection — this slot can't)
    }
}

// Load `sno` NOW, regardless of Auto-Load and regardless of whether the row is already current.
// (Selecting an already-current row emits no selection change, so showAppearance never fires —
// that's why double-clicking the model restored at startup appeared to do nothing.)
void ModelsTab::forceLoadSno(int sno)
{
    if (!m_list || !m_listModel) return;
    m_skipNextAutoLoad = false;   // an explicit double-click is never the startup-safety case
    for (int r = 0; r < m_listModel->rowCount(); ++r) {
        const SnoEntry* e = m_listModel->entryAt(r);
        if (!e || e->snoId != sno) continue;
        const QModelIndex idx = m_treeModel->index(r, 0);
        if (m_list->currentIndex() != idx) {
            m_list->setCurrentIndex(idx);   // → showAppearance (fills metadata)
            m_list->scrollTo(idx, QAbstractItemView::PositionAtCenter);
        } else if (m_modelView) {
            m_modelView->setOverlayText(QStringLiteral("Loading  %1 …").arg(e->name));
        }
        if (m_geoTimer) m_geoTimer->start();   // force the 3D load either way
        return;
    }
}

void ModelsTab::selectModelBySno(int sno)
{
    if (!m_list || !m_listModel) return;
    for (int r = 0; r < m_listModel->rowCount(); ++r) {
        const SnoEntry* e = m_listModel->entryAt(r);
        if (e && e->snoId == sno) {
            // NB: index on the WRAPPER — a source-model index set on the tree view is silently invalid.
            const QModelIndex idx = m_treeModel->index(r, 0);
            m_list->setCurrentIndex(idx);   // selection → showAppearance()
            m_list->scrollTo(idx, QAbstractItemView::PositionAtCenter);
            if (m_geoTimer) m_geoTimer->start();   // force-load even if Auto-Load is off
            return;
        }
    }
}

void ModelsTab::showAppearance(int sno, const QString& name)
{
    if (QSettings().value(QStringLiteral("models/rememberLast"), false).toBool())
        QSettings().setValue(QStringLiteral("models/lastSno"), sno);
    m_matModel->setRowCount(0);
    m_matTexModel->setRowCount(0);
    m_matValues->clear();
    m_looksModel->setRowCount(0);
    m_anims->clear();
    m_playingAnim.clear();
    m_curAnim = {};   // drop the retained clip so exports don't embed a stale animation
    m_animCount = 0;
    if (m_treeModel) m_treeModel->clearSubtree();
    if (m_animTimer) m_animTimer->stop();
    if (m_timeline) m_timeline->setVisible(false);
    if (m_playBtn) m_playBtn->setIcon(transportGlyph(0));
    clearTexturePreview();
    for (const QString& k : {QStringLiteral("Title"), QStringLiteral("Filesize"),
                             QStringLiteral("Format"), QStringLiteral("Materials"),
                             QStringLiteral("Textures"), QStringLiteral("Animations"),
                             QStringLiteral("Bounds"), QStringLiteral("LODs"),
                             QStringLiteral("Bones"), QStringLiteral("Actor"),
                             QStringLiteral("Physics"), QStringLiteral("Item facts"),
                             QStringLiteral("Sets")})
        setInfo(k, QString());

    m_curSno = sno;
    m_curName = name;
    m_curGeo = ModelGeometry();
    if (m_modelView) { m_modelView->clearGeometry(); m_modelView->setOverlayText(QString()); }
    if (m_exportBtn) m_exportBtn->setEnabled(false);
    setInfo(QStringLiteral("Filename"), QStringLiteral("%1 [%2]").arg(name).arg(sno));
    // DATA-page extras (game-data facts the overlay doesn't carry) + drop the no-model veil.
    setInfo(QStringLiteral("SNO"), QString::number(sno));
    setInfo(QStringLiteral("Collection"), AppearanceMeta::instance().collectionFor(sno));
    {
        QStringList tl(AppearanceMeta::instance().tagsFor(sno).values());
        tl.sort(Qt::CaseInsensitive);
        setInfo(QStringLiteral("Tags"), tl.join(QStringLiteral(", ")));
    }
    ensureAppearanceSets();   // ~20 files, loaded once
    setInfo(QStringLiteral("Sets"),
            m_appSetsByName.value(name.toLower()).join(QStringLiteral(", ")));
    if (m_rstackHint) m_rstackHint->hide();
    // Who uses this appearance in-game (NPCs / monster family / gear items). Independent of the 3D
    // load, so fill it straight away: kick off the one-time Actor/Item scan and show what we have.
    ensureEntityIndex();
    updateEntityInfo(sno);
    m_pullSources.clear();                                   // manual anim pulls are per-model
    m_suppressSkelFallback = false;                          // ...as is the "own clips only" state
    if (m_pullClearBtn) m_pullClearBtn->setEnabled(false);   // ...so the Clear button follows them
    if (m_pullClearBtn) m_pullClearBtn->setEnabled(false);
    // Blocklisted models crashed the loader/renderer before — do NOT auto-load them, so
    // selecting or right-clicking the row is safe. The user can force it via Reload, or
    // remove it from the blocklist (right-click ▸ icon, or Settings ▸ Maintenance).
    if (m_renderBlocklist.contains(sno)) {
        if (m_modelView)
            m_modelView->setOverlayText(QStringLiteral("This model previously crashed the 3D view "
                "and is blocklisted.\nUse Reload to try loading it anyway, or clear the render "
                "blocklist in Settings ▸ Maintenance."));
        m_skipNextAutoLoad = false;
        return;
    }
    // Defer the heavy CASC read + parse + GPU upload until the selection settles.
    // Doing it inline on every row crashes/locks when scrolling fast; this mirrors
    // the Python tool, which only does light metadata work per selection.
    // When Auto-Load is off, skip the 3D load entirely (Reload loads on demand).
    // m_skipNextAutoLoad suppresses just the startup-restore load (crash safety).
    const bool willLoad = m_geoTimer && m_autoLoad && !m_skipNextAutoLoad;
    if (willLoad) m_geoTimer->start();
    // Always tell the user what the (empty) viewport is doing: loading soon, or waiting — and say
    // WHY it's waiting. The old text always blamed Auto-Load, which was wrong (and confusing) for
    // the startup restore, where Auto-Load is on and the load is skipped once for crash safety.
    if (m_modelView) {
        if (willLoad)
            m_modelView->setOverlayText(QStringLiteral("Loading  %1 …").arg(name));
        else if (!m_autoLoad)
            m_modelView->setOverlayText(QStringLiteral(
                "Auto-load is off — double-click to load this model."));
        else
            m_modelView->setOverlayText(QStringLiteral(
                "Not loaded on launch (crash safety) — double-click to load %1.").arg(name));
    }
    m_skipNextAutoLoad = false;

    // Filesize from the (decoded) CASC payload, like D4Analyzer.
    if (m_reader && m_reader->isReady()) {
        const quint64 bytes = m_reader->payloadSize(quint64(sno));
        if (bytes > 0) setInfo(QStringLiteral("Filesize"), humanSize(qint64(bytes)));
    }

    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty())
        return;
    QFile f(QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4, name));
    if (!f.open(QIODevice::ReadOnly))
        return;

    const QByteArray appBytes = f.readAll();
    const AppearanceInfo app = parseAppearanceJson(appBytes);
    setInfo(QStringLiteral("Format"), QStringLiteral("glTF (.glb)"));
    setInfo(QStringLiteral("Materials"), QString::number(app.materials.size()));
    // Structure facts (INFO page) — authored bounds, LOD ladder, base bone count, straight from
    // the .app.json we already hold: tStructure.{aabbBounds,wsBounds,ptChunks,ptBoneData}.
    {
        const QJsonObject appRoot = QJsonDocument::fromJson(appBytes).object();
        const QJsonObject st = appRoot.value(QStringLiteral("tStructure")).toObject();
        const QJsonObject ext = st.value(QStringLiteral("aabbBounds")).toObject()
                                    .value(QStringLiteral("wvExt")).toObject();
        const double bx = ext.value(QStringLiteral("x")).toDouble() * 2.0;
        const double by = ext.value(QStringLiteral("y")).toDouble() * 2.0;
        const double bz = ext.value(QStringLiteral("z")).toDouble() * 2.0;
        const double rad = st.value(QStringLiteral("wsBounds")).toObject()
                               .value(QStringLiteral("wdRadius")).toDouble();
        if (bx > 0.0 || rad > 0.0)
            setInfo(QStringLiteral("Bounds"),
                    QStringLiteral("%1 × %2 × %3 m · r %4")
                        .arg(bx, 0, 'f', 2).arg(by, 0, 'f', 2).arg(bz, 0, 'f', 2)
                        .arg(rad, 0, 'f', 2));
        QStringList lods;
        const QJsonArray chunks = st.value(QStringLiteral("ptChunks")).toArray();
        if (!chunks.isEmpty())
            for (const QJsonValue& lv : chunks.first().toObject()
                                            .value(QStringLiteral("ptLODs")).toArray()) {
                const double dist = lv.toObject().value(QStringLiteral("flLODDistance")).toDouble();
                if (dist > 0.0) lods << QString::number(dist, 'f', 1);
            }
        if (!lods.isEmpty()) {
            const double mul = appRoot.value(QStringLiteral("flLODDistanceMultiplier")).toDouble();
            QString s = QStringLiteral("%1 · @ %2 m").arg(lods.size()).arg(lods.join(QStringLiteral(" / ")));
            if (mul > 0.0 && !qFuzzyCompare(mul, 1.0))
                s += QStringLiteral(" · ×%1").arg(mul, 0, 'f', 2);
            setInfo(QStringLiteral("LODs"), s);
        }
        const QJsonArray bd = st.value(QStringLiteral("ptBoneData")).toArray();
        if (!bd.isEmpty()) {
            const int nb = bd.first().toObject().value(QStringLiteral("nBaseBoneCount")).toInt();
            if (nb > 0) setInfo(QStringLiteral("Bones"), QString::number(nb));
        }
    }
    // Material roster (real .mat names), indexed by the RAW ptAppearanceMaterials
    // position so it lines up with each primitive's materialIndex. (Rebuilt in the
    // raw parse loop below — parseAppearanceJson drops empties and flattens per-SOA,
    // which misaligns the index for multi-material models like Lilith.)
    m_appMatNames.clear();
    m_soaNames.clear();
    m_clothMats.clear();
    m_fxMats.clear();
    m_gibMats.clear();

    // Looks table (INDEX | HASH HEX | HASH DEC | NAME). Hashes unavailable from the
    // .app.json look list, so those cells are left blank.
    m_looksModel->setRowCount(app.looks.size());
    for (int r = 0; r < app.looks.size(); ++r) {
        m_looksModel->setItem(r, 0, new QStandardItem(QString::number(r)));
        m_looksModel->setItem(r, 3, new QStandardItem(app.looks[r]));
    }
    if (m_looksHdr) m_looksHdr->setText(QStringLiteral("LOOKS (%1)").arg(app.looks.size()));
    autoSizeTable(m_looksView, 8);   // stacked section: hug the rows, don't demand a slab

    // Materials table: one row per SubObjectAppearance (ptSOAs), like the Python tab.
    // Re-parse the raw JSON since AppearanceInfo flattens away the sub-object detail.
    m_matModel->setRowCount(0);
    m_matListModel->setRowCount(0);
    QMap<int, QString> matBySno;   // distinct materials across all looks/SOAs (by SNO)
    {
        QFile rf(QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4, name));
        int row = 0, mi = 0;   // mi = raw ptAppearanceMaterials index (== materialIndex)
        if (rf.open(QIODevice::ReadOnly)) {
            const QJsonObject ro = QJsonDocument::fromJson(rf.readAll()).object();
            const QJsonArray mats = ro.value(QStringLiteral("ptAppearanceMaterials")).toArray();
            auto rawInt = [](const QJsonValue& v) -> int {
                return v.toObject().value(QStringLiteral("__raw__")).toInt();
            };
            auto rawOf = [&](const QJsonValue& v) -> QString {
                return v.toObject().isEmpty() ? QString() : QString::number(rawInt(v));
            };
            for (const QJsonValue& mv : mats) {
                const QJsonObject m = mv.toObject();
                const QString hash = QString::number(quint32(m.value(QStringLiteral("dwMaterialHash")).toDouble()), 16);
                const QJsonArray soas = m.value(QStringLiteral("ptSOAs")).toArray();
                bool isCloth = false;
                QString firstName, firstCloth;
                QVector<QString> perSoa;   // name selected by each look (SOA index)
                for (int si = 0; si < soas.size(); ++si) {
                    const QJsonObject s = soas[si].toObject();
                    const QJsonObject sm = s.value(QStringLiteral("snoMaterial")).toObject();
                    const QJsonObject cl = s.value(QStringLiteral("snoCloth")).toObject();
                    const QJsonObject ovm = s.value(QStringLiteral("snoOverrideMaterial")).toObject();
                    const int flags = s.value(QStringLiteral("dwFlags")).toInt();
                    if (firstName.isEmpty())  firstName  = sm.value(QStringLiteral("name")).toString();
                    if (firstCloth.isEmpty()) firstCloth = cl.value(QStringLiteral("name")).toString();
                    if (cl.value(QStringLiteral("__raw__")).toInt() != 0) isCloth = true;
                    // Per-look material name: override beats base; cloth is the fallback.
                    QString soaName = ovm.value(QStringLiteral("name")).toString();
                    if (soaName.isEmpty()) soaName = sm.value(QStringLiteral("name")).toString();
                    if (soaName.isEmpty()) soaName = cl.value(QStringLiteral("name")).toString();
                    perSoa.append(soaName);
                    // Collect distinct materials (incl. override) for the "Materials" tab.
                    const int msno = sm.value(QStringLiteral("__raw__")).toInt();
                    if (msno != 0) matBySno.insert(msno, sm.value(QStringLiteral("name")).toString());
                    const QJsonObject ov = s.value(QStringLiteral("snoOverrideMaterial")).toObject();
                    const int osno = ov.value(QStringLiteral("__raw__")).toInt();
                    if (osno != 0) matBySno.insert(osno, ov.value(QStringLiteral("name")).toString());
                    QList<QStandardItem*> cells{
                        new QStandardItem(hash),
                        new QStandardItem(sm.value(QStringLiteral("name")).toString()),
                        new QStandardItem(QString::number(si)),
                        new QStandardItem(QStringLiteral("0x%1").arg(flags, 0, 16)),
                        new QStandardItem(rawOf(s.value(QStringLiteral("snoMaterial")))),
                        new QStandardItem(rawOf(s.value(QStringLiteral("snoCloth")))),
                        new QStandardItem(rawOf(s.value(QStringLiteral("snoOverrideMaterial"))))};
                    m_matModel->appendRow(cells);
                    ++row;
                }
                // Display name: real material, else the cloth name (Cloth_lilith_*),
                // so cloth parts read with their true name instead of "Material_N".
                const QString matName = !firstName.isEmpty() ? firstName : firstCloth;
                m_appMatNames.append(matName);   // index mi == materialIndex (default look)
                m_soaNames.append(perSoa);       // every look's choice for this material
                if (isCloth) m_clothMats.insert(mi);
                if (matName.contains(QLatin1String("fx"), Qt::CaseInsensitive)
                    || matName.contains(QLatin1String("effect"), Qt::CaseInsensitive)
                    || matName.contains(QLatin1String("glow"), Qt::CaseInsensitive))
                    m_fxMats.insert(mi);
                if (matName.contains(QLatin1String("gib"), Qt::CaseInsensitive)
                    || matName.contains(QLatin1String("gore"), Qt::CaseInsensitive)
                    || matName.contains(QLatin1String("flesh"), Qt::CaseInsensitive)
                    || matName.contains(QLatin1String("viscera"), Qt::CaseInsensitive))
                    m_gibMats.insert(mi);   // gore/flesh submeshes (dismemberment, mostly NPCs)
                ++mi;
            }

            // SubObject Apps: the LOD0 draw-call list.
            m_subObjModel->setRowCount(0);
            const QJsonArray subs = ro.value(QStringLiteral("tStructure")).toObject()
                .value(QStringLiteral("ptChunks")).toArray().at(0).toObject()
                .value(QStringLiteral("ptLODs")).toArray().at(0).toObject()
                .value(QStringLiteral("ptSubObjects")).toArray();
            for (int si = 0; si < subs.size(); ++si) {
                const QJsonObject so = subs[si].toObject();
                const int midx = so.value(QStringLiteral("nMaterialIndex")).toInt();
                const quint32 sh = quint32(so.value(QStringLiteral("dwSubObjectHash")).toDouble());
                const quint32 ov = quint32(so.value(QStringLiteral("dwShaderMapOverride")).toDouble());
                const QString ovStr = (ov == 0u || ov == 0xFFFFFFFFu)
                    ? QStringLiteral("-") : QString::number(ov);
                m_subObjModel->appendRow(QList<QStandardItem*>{
                    new QStandardItem(QString::number(si)),
                    new QStandardItem(QString::number(midx)),
                    new QStandardItem(m_appMatNames.value(midx)),
                    new QStandardItem(QStringLiteral("%1").arg(sh, 0, 16)),
                    new QStandardItem(QString::number(so.value(QStringLiteral("nVertBufferIndex")).toInt())),
                    new QStandardItem(QString::number(so.value(QStringLiteral("nIndexBufferIndex")).toInt())),
                    new QStandardItem(QString::number(so.value(QStringLiteral("nSubObjectMaxLOD")).toInt())),
                    new QStandardItem(ovStr),
                    new QStandardItem(m_clothMats.contains(midx) ? QStringLiteral("yes")
                                                                 : QString())});
            }
        }
        Q_UNUSED(row);
    }
    // "Materials" sub-tab: distinct materials by SNO.
    for (auto it = matBySno.constBegin(); it != matBySno.constEnd(); ++it)
        m_matListModel->appendRow(QList<QStandardItem*>{
            new QStandardItem(QString::number(it.key())),
            new QStandardItem(it.value())});
    updateTabCounts();
}

// (Re)fill, place and show the search-history dropdown — or hide it when there's nothing
// useful to offer. Called on focus-in and every text change; focus-out hides it.
void ModelsTab::refreshHistPopup()
{
    if (!m_histList || !m_hdrSearch) return;
    if (!m_hdrSearch->hasFocus()) { m_histList->hide(); return; }
    const QString needle = m_hdrSearch->text().trimmed();
    m_histList->clear();
    for (const QString& q :
         QSettings().value(QStringLiteral("models/searchHistory")).toStringList())
        if (needle.isEmpty() || q.contains(needle, Qt::CaseInsensitive))
            m_histList->addItem(q);
    // Nothing to offer — or the only entry is exactly what's typed already.
    if (m_histList->count() == 0
        || (m_histList->count() == 1 && m_histList->item(0)->text() == needle)) {
        m_histList->hide();
        return;
    }
    const QPoint tl = m_hdrSearch->mapTo(m_histList->parentWidget(),
                                         QPoint(0, m_hdrSearch->height() + 1));
    const int rowH = m_histList->fontMetrics().height() + 8;
    m_histList->setGeometry(tl.x(), tl.y(), m_hdrSearch->width(),
                            qMin(m_histList->count(), 8) * rowH + 4);
    m_histList->show();
    m_histList->raise();
}

// ── "Remember last search" — the whole filter STATE, not just the text ──────────────────────
// Saved on every change (cheap: a few QSettings writes), restored once at construction. Tags,
// the special/category pseudo-filter, class/gender/type, the data toggles and the AND/OR mode
// all come back exactly as you left them.
void ModelsTab::saveFilterState()
{
    if (!QSettings().value(QStringLiteral("models/rememberSearch"), false).toBool()) return;
    QSettings s;
    s.setValue(QStringLiteral("models/lastSearch"), m_hdrSearch ? m_hdrSearch->text() : QString());
    QStringList tags(m_tagFilter.begin(), m_tagFilter.end());
    tags.sort();
    s.setValue(QStringLiteral("models/lastTags"), tags);
    // (AND/OR mode isn't saved here — models/tagOrMode already persists on its own toggle.)
    auto cd = [](QComboBox* c) { return c ? c->currentData().toString() : QString(); };
    s.setValue(QStringLiteral("models/lastCat"), cd(m_catCombo));
    s.setValue(QStringLiteral("models/lastClass"), cd(m_classCombo));
    s.setValue(QStringLiteral("models/lastGender"), cd(m_genderCombo));
    s.setValue(QStringLiteral("models/lastType"), cd(m_typeCombo));
    s.setValue(QStringLiteral("models/lastOnlyDec"), m_onlyDecrypted && m_onlyDecrypted->isChecked());
    s.setValue(QStringLiteral("models/lastHideBroken"), m_hideBrokenChk && m_hideBrokenChk->isChecked());
}

void ModelsTab::restoreFilterState()
{
    QSettings s;
    if (!s.value(QStringLiteral("models/rememberSearch"), false).toBool()) return;
    // Tags first (the panel's checkboxes may not exist yet — the set IS the source of truth, and
    // onMetaReady ticks the boxes from it when it builds them).
    const QStringList tags = s.value(QStringLiteral("models/lastTags")).toStringList();
    m_tagFilter = QSet<QString>(tags.begin(), tags.end());
    // (m_tagOrMode is restored from models/tagOrMode just above this call — one source of truth.)
    auto pick = [](QComboBox* c, const QString& data) {
        if (!c || data.isEmpty()) return;
        const int i = c->findData(data);
        if (i >= 0) c->setCurrentIndex(i);   // fires the filter connects
    };
    pick(m_catCombo, s.value(QStringLiteral("models/lastCat")).toString());
    pick(m_classCombo, s.value(QStringLiteral("models/lastClass")).toString());
    pick(m_genderCombo, s.value(QStringLiteral("models/lastGender")).toString());
    pick(m_typeCombo, s.value(QStringLiteral("models/lastType")).toString());
    if (m_onlyDecrypted && s.value(QStringLiteral("models/lastOnlyDec"), false).toBool())
        m_onlyDecrypted->setChecked(true);
    if (m_hideBrokenChk && s.value(QStringLiteral("models/lastHideBroken"), false).toBool())
        m_hideBrokenChk->setChecked(true);
    // Search text last: its textChanged does the parse + filter, so everything lands together.
    if (m_hdrSearch) m_hdrSearch->setText(s.value(QStringLiteral("models/lastSearch")).toString());
    applyCategoryFilter();
    updateCount();
    updateTagButtonTint();
}

// The funnel button tints red while a tag filter is live — the button itself is the
// "you are looking at a filtered list" signal, like every checked toolbar toggle.
void ModelsTab::updateTagButtonTint()
{
    if (!m_tagBtn) return;
    if (m_tagFilter.isEmpty()) {
        m_tagBtn->setStyleSheet(QString());
        m_tagBtn->setToolTip(QStringLiteral("Filter by tags — select any number"));
    } else {
        m_tagBtn->setStyleSheet(QStringLiteral(
            "QToolButton{background:#8a1414;border:1px solid #a01818;border-radius:3px;}"));
        QStringList tags(m_tagFilter.begin(), m_tagFilter.end());
        tags.sort(Qt::CaseInsensitive);
        m_tagBtn->setToolTip(QStringLiteral("Tag filter (%1): %2")
                                 .arg(m_tagOrMode ? QStringLiteral("any") : QStringLiteral("all"),
                                      tags.join(QStringLiteral(", "))));
    }
}

// Header display-mode dropdown. List = flat browse rows (subtree suppressed on the wrapper),
// Outliner = the scene tree, Grid = thumbnails. Grid parity goes THROUGH the existing grid
// toggle so both controls agree; setChecked on an already-matching state is a no-op, which
// breaks the recursion with the grid button's own handler.
void ModelsTab::applyDisplayMode(int mode)
{
    mode = qBound(0, mode, 2);
    m_displayMode = mode;
    QSettings().setValue(QStringLiteral("models/displayMode"), mode);
    if (m_treeModel) m_treeModel->setFlatMode(mode == 0);
    if (m_list) m_list->setRootIsDecorated(mode != 0);
    const bool grid = (mode == 2);
    if (m_gridBtn && m_gridBtn->isChecked() != grid)
        m_gridBtn->setChecked(grid);   // → setGridView (the one existing path)
    if (m_displayBtn) {
        static const char* kModes[3] = {"List", "Outliner", "Grid"};
        m_displayBtn->setIcon(QIcon(displayModeGlyph(mode)));   // icon-only, Blender-style
        m_displayBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
        m_displayBtn->setToolTip(QStringLiteral("Display mode: %1\nList — flat rows · Outliner — "
                                                "scene tree · Grid — thumbnails")
                                     .arg(QLatin1String(kModes[mode])));
        if (QMenu* dm = m_displayBtn->menu()) {
            const QList<QAction*> acts = dm->actions();
            for (int i = 0; i < 3 && i < acts.size(); ++i) {
                acts[i]->setChecked(i == mode);
                QFont f = acts[i]->font();   // the stock indicator is suppressed — bold marks
                f.setBold(i == mode);        // the active mode instead
                acts[i]->setFont(f);
            }
        }
        // The "Outliner shows" toggles only make sense in Outliner mode.
        for (QAction* a : m_kindActs) a->setVisible(mode == 1);
    }
    applyListDensity();   // List = tight, icon-less rows; Outliner/Grid = normal
}

// Compact styling scoped to the LIST display mode only: hide the Icon column, tighten the rows and
// use a small condensed font. Outliner and Grid keep their normal font, icons and (for Outliner)
// icon-tracking row height.
void ModelsTab::applyListDensity()
{
    if (!m_list || !m_treeModel) return;
    const bool listMode = (m_displayMode == 0);
    m_list->setColumnHidden(1, listMode);   // Icon column: off in List, on otherwise
    if (listMode) {
        QFont lf = m_listBaseFont;
        lf.setPointSizeF(qMax(7.5, m_listBaseFont.pointSizeF() - 1.0));
        lf.setStretch(QFont::SemiCondensed);
        m_list->setFont(lf);
        m_treeModel->setRowHeight(18);      // tight, text-only rows
    } else {
        m_list->setFont(m_listBaseFont);
        m_treeModel->setRowHeight(m_iconPx + 6);   // Outliner rows track the icon size
    }
}

// Register a right-column page: content into the stack, an icon button onto the strip (inserted
// before the trailing stretch when one exists — late registrations like ANIMATIONS land in order).
// Returns the created header label, or nullptr when title is empty (page brings its own header).
QLabel* ModelsTab::addRightPage(const QString& title, QWidget* content, const QPixmap& icon,
                                const QString& tip)
{
    if (!m_rstack || !m_rstripLay) return nullptr;
    const int page = m_rsections.size();
    auto* box = new PanelBox(title, content, m_rstack);
    box->hide();               // up only when its strip toggle says so
    m_rstack->addWidget(box);
    m_rsections.append(box);
    // Settings key from the REGISTRATION title, captured now: the label text later grows live
    // counts ("PARTS · 4 of 6 shown"), so deriving the key at write time would scribble to a
    // different key on every refresh.
    m_sectKeys.append(QStringLiteral("models/panel/") + title.section(QLatin1Char(' '), 0, 0));

    auto* b = new QToolButton(m_rstripLay->parentWidget());
    b->setIcon(QIcon(icon));
    b->setIconSize(QSize(16, 16));
    b->setCheckable(true);     // checked = panel is up
    b->setToolTip(tip);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral(
        "QToolButton{border:1px solid transparent;border-radius:3px;padding:3px;background:transparent;}"
        "QToolButton:hover{border-color:#b0453c;}"
        "QToolButton:checked{background:#8a1414;border-color:#a01818;}"));
    connect(b, &QToolButton::toggled, this, [this, page](bool on) { showPanel(page, on); });
    // Header buttons: ▲▼ reorder; ✕ hides by unchecking the strip toggle (one path in/out).
    connect(box->up,   &QToolButton::clicked, this, [this, page]() { movePanel(page, -1); });
    connect(box->down, &QToolButton::clicked, this, [this, page]() { movePanel(page, +1); });
    connect(box->close, &QToolButton::clicked, this, [this, page]() {
        if (page < m_rpageBtns.size()) m_rpageBtns[page]->setChecked(false);
    });
    // Insert before a trailing stretch so late pages don't end up below it.
    int at = m_rstripLay->count();
    if (at > 0 && m_rstripLay->itemAt(at - 1)->spacerItem()) --at;
    m_rstripLay->insertWidget(at, b);
    m_rpageBtns.append(b);
    return box->label;
}

// ── Attachments panel: the other models an actor holds/spawns (weapons, shields, props) ─────
// In game data an Actor attaches child models via ptMsgTriggeredEvents → TriggerEventAddObject
// (see model/Attachments.h). The Models tab browses by APPEARANCE, so we resolve the actors that
// use the loaded appearance (the entity index), scan each for attach events, and list them here.
// Ticking a row seats that child mesh onto the parent hardpoint bone and re-textures the view.
void ModelsTab::buildAttachPage()
{
    m_attachTree = new QTreeWidget;
    m_attachTree->setColumnCount(1);
    m_attachTree->setHeaderHidden(true);
    m_attachTree->setRootIsDecorated(true);
    m_attachTree->setUniformRowHeights(true);
    m_attachTree->setSelectionMode(QAbstractItemView::NoSelection);
    m_attachTree->setFocusPolicy(Qt::NoFocus);
    m_attachTree->setStyleSheet(QStringLiteral("QTreeWidget{background:transparent;border:none;}"));
    connect(m_attachTree, &QTreeWidget::itemChanged, this, &ModelsTab::onAttachItemChanged);
    // Double-click anywhere on an attachment row flips its checkbox (→ attach/detach).
    connect(m_attachTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int) {
        if (!it || it->data(0, Qt::UserRole).toString().isEmpty()) return;   // group node
        it->setCheckState(0, it->checkState(0) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    });
}

// Drop all attachment state → used when a new model has no geometry, a load fails, or the tab is
// reset, so the panel never shows (or re-assembles against) a previous model's data.
void ModelsTab::resetAttachments()
{
    ++m_attachScanToken;              // invalidate any in-flight background scan
    m_attachAll.clear();
    m_attachActive.clear();
    m_haveBase = false;
    m_baseGeo = ModelGeometry{};
    m_baseMatNames.clear();
    m_baseSoaNames.clear();
    m_baseClothMats.clear(); m_baseFxMats.clear(); m_baseGibMats.clear();
    m_baseHpMap.clear();
    if (m_attachTree) {
        m_attachTree->blockSignals(true);   // clearing must not fire itemChanged
        m_attachTree->clear();
        m_attachTree->blockSignals(false);
    }
    if (m_attachHdr) m_attachHdr->setText(QStringLiteral("ATTACHMENTS"));
}

void ModelsTab::scanAttachments()
{
    if (!m_attachTree) return;
    if (m_curSno < 0) { m_attachAll.clear(); populateAttachTree(); return; }
    // Cache hit: the appearance→attachments mapping is immutable per game build, so re-selecting a
    // model repopulates the panel instantly with no disk work.
    if (m_attachCache.contains(m_curSno)) {
        m_attachAll = m_attachCache.value(m_curSno);
        populateAttachTree();
        return;
    }
    if (!m_entityScanned) {                       // need the appearance→actors map first
        if (m_attachHdr) m_attachHdr->setText(QStringLiteral("ATTACHMENTS · …"));
        ensureEntityIndex();                      // its completion re-invokes scanAttachments()
        return;
    }
    // Defer the (up-to-80-file) actor scan until the panel is actually open — no point paying it
    // on every load if the user never looks at attachments. Opening the panel re-invokes this.
    const bool panelOpen = m_attachPage >= 0 && m_attachPage < m_rpageBtns.size()
                        && m_rpageBtns[m_attachPage] && m_rpageBtns[m_attachPage]->isChecked();
    if (!panelOpen) { m_attachAll.clear(); populateAttachTree(); return; }

    const QStringList actors = m_apprActors.value(m_curSno);
    if (actors.isEmpty()) {
        m_attachCache.insert(m_curSno, {});   // nothing to attach — cache the empty result too
        m_attachAll.clear(); populateAttachTree(); return;
    }
    const int token = ++m_attachScanToken;
    const int sno   = m_curSno;
    const QString d4 = Config::d4dataDir();
    const QStringList actorsCap = actors.mid(0, 80);   // shared rigs list many actors — cap the scan
    if (m_attachHdr) m_attachHdr->setText(QStringLiteral("ATTACHMENTS · …"));
    std::thread([this, d4, actorsCap, token, sno]() {
        QVector<ModelAttach::Attachment> all;
        for (const QString& an : actorsCap) all += ModelAttach::scanActor(d4, an);
        QMetaObject::invokeMethod(this, [this, all, token, sno]() {
            m_attachCache.insert(sno, all);            // cache regardless (result is model-specific)
            if (token != m_attachScanToken) return;    // a newer load/scan superseded this one
            m_attachAll = all;
            populateAttachTree();
        }, Qt::QueuedConnection);
    }).detach();
}

void ModelsTab::populateAttachTree()
{
    if (!m_attachTree) return;
    m_attachTree->blockSignals(true);   // building rows must not fire itemChanged (→ attach)
    m_attachTree->clear();
    // Group by parent actor → trigger label (permanent "held" vs combat/animation spawns), stable
    // in first-seen order.
    QHash<QString, QTreeWidgetItem*> actorNodes;
    QHash<QString, QTreeWidgetItem*> trigNodes;   // key "actor|triggerLabel"
    int leaves = 0;
    for (const ModelAttach::Attachment& a : m_attachAll) {
        QTreeWidgetItem* an = actorNodes.value(a.parentActor);
        if (!an) {
            an = new QTreeWidgetItem(m_attachTree);
            an->setText(0, a.parentActor);
            an->setExpanded(true);
            an->setFlags(Qt::ItemIsEnabled);
            actorNodes.insert(a.parentActor, an);
        }
        const QString tkey = a.parentActor + QLatin1Char('|') + a.triggerLabel();
        QTreeWidgetItem* tn = trigNodes.value(tkey);
        if (!tn) {
            tn = new QTreeWidgetItem(an);
            tn->setText(0, a.triggerLabel());
            tn->setExpanded(true);
            tn->setFlags(Qt::ItemIsEnabled);
            tn->setForeground(0, QColor(0x9a, 0x9a, 0x9a));
            trigNodes.insert(tkey, tn);
        }
        auto* leaf = new QTreeWidgetItem(tn);
        leaf->setText(0, QStringLiteral("%1  ·  %2")
                             .arg(a.childApprName.isEmpty() ? a.childActor : a.childApprName, a.hpName));
        leaf->setToolTip(0, QStringLiteral("Child actor: %1\nAppearance: %2\nHardpoint: %3\nTrigger: %4")
                                .arg(a.childActor, a.childApprName, a.hpName, a.triggerLabel()));
        leaf->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        leaf->setCheckState(0, m_attachActive.contains(a.key()) ? Qt::Checked : Qt::Unchecked);
        leaf->setData(0, Qt::UserRole, a.key());
        ++leaves;
    }
    if (leaves == 0) {
        auto* none = new QTreeWidgetItem(m_attachTree);
        none->setText(0, QStringLiteral("No held or spawned models"));
        none->setFlags(Qt::ItemIsEnabled);
        none->setForeground(0, QColor(0x88, 0x88, 0x88));
    }
    m_attachTree->blockSignals(false);
    if (m_attachHdr) m_attachHdr->setText(QStringLiteral("ATTACHMENTS · %1").arg(leaves));
}

void ModelsTab::onAttachItemChanged(QTreeWidgetItem* item, int col)
{
    if (!item || col != 0) return;
    const QString key = item->data(0, Qt::UserRole).toString();
    if (key.isEmpty()) return;   // a group node, not an attachment row
    const bool on  = item->checkState(0) == Qt::Checked;
    const bool had = m_attachActive.contains(key);
    if (on == had) return;       // no state change
    if (on) m_attachActive.insert(key); else m_attachActive.remove(key);
    rebuildAssembledGeometry();
}

// Re-assemble the displayed model = pristine base + every ticked attachment, seated onto the
// parent's hardpoint bone and textured by each child's own material roster. Runs under the same
// hardware-fault guard the base loader uses; a bad child reverts cleanly to the base model.
void ModelsTab::rebuildAssembledGeometry()
{
    if (!m_haveBase || !m_modelView) return;
    // Always start from the clean base so detaching is exact.
    m_curGeo      = m_baseGeo;
    m_appMatNames = m_baseMatNames;
    m_soaNames    = m_baseSoaNames;
    m_clothMats   = m_baseClothMats;   // reset FX/SIM/GIB sets to base; child mats classified below
    m_fxMats      = m_baseFxMats;
    m_gibMats     = m_baseGibMats;

    // Remember the playing clip + frame so toggling an attachment doesn't reset the animation
    // (setGeometry clears the widget's pose — we re-apply it after the rebuild, like icon-render
    // restore does). The timer keeps running, so playback simply continues from here.
    const int savedAnimFrame = m_animSlider ? m_animSlider->value() : 0;

    seh::HardwareFault fault;
    const bool ok = seh::runGuarded("attach", [&]() {
        if (!m_attachActive.isEmpty() && m_reader && m_reader->isReady()) {
            const QString d4 = Config::d4dataDir();
            const QVector<ModelJoint> baseSkel = m_baseGeo.skeleton;
            // Seed the roster map with base material names so a child sharing a name reuses its slot.
            QHash<QString, int> rosterIdx;
            for (int i = 0; i < m_appMatNames.size(); ++i)
                if (!rosterIdx.contains(m_appMatNames[i])) rosterIdx.insert(m_appMatNames[i], i);
            int nLooks = 1;
            for (const QVector<QString>& v : m_baseSoaNames) nLooks = qMax(nLooks, int(v.size()));

            for (const ModelAttach::Attachment& a : m_attachAll) {
                if (!m_attachActive.contains(a.key()) || a.childApprSno <= 0) continue;
                const QByteArray meta = m_reader->readMetaBySno(quint64(a.childApprSno));
                const QByteArray pay  = m_reader->readPayloadBySno(quint64(a.childApprSno));
                if (meta.isEmpty() || pay.isEmpty()) continue;
                ModelGeometry childGeo = ModelParser::parseApp(meta, pay);
                if (!childGeo.valid) continue;
                const QStringList childRoster = MaterialDecode::appearanceRoster(d4, a.childApprName);
                if (a.isMount) {
                    // Mount: seat via the MOUNT's own saddle hardpoint (its rig, not the rider's),
                    // so the mount ends up under the rider (who stays at the origin).
                    const QString mountAppPath =
                        QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4, a.childApprName);
                    const auto mountHp = ModelAttach::loadHardpointMap(mountAppPath);
                    if (!ModelAttach::seatMount(childGeo, mountHp, childGeo.skeleton))
                        continue;   // no saddle hardpoint → can't place it reliably; skip
                } else {
                    ModelAttach::seat(childGeo, baseSkel, m_baseHpMap, a);   // bake onto the parent bone
                }
                for (MeshPrimitive& p : childGeo.primitives) {   // childGeo is discarded → move-append
                    QString rn = childRoster.value(p.materialIndex);
                    if (rn.isEmpty()) rn = a.childApprName;
                    int idx = rosterIdx.value(rn, -1);
                    if (idx < 0) {
                        idx = m_appMatNames.size();
                        m_appMatNames.append(rn);
                        m_soaNames.append(QVector<QString>(nLooks, rn));
                        rosterIdx.insert(rn, idx);
                        // Classify the child material so the FX/SIM/GIB toggles cover it (weapon
                        // glow/trail planes are usually named "…_fx"/"…_glow"). Name tokens the base
                        // uses (showAppearance) + a couple weapon-FX ones, AND a shader-map check so
                        // FX is caught even when the material name gives nothing away.
                        const QString l = rn.toLower();
                        if (l.contains(QLatin1String("fx"))     || l.contains(QLatin1String("effect"))
                            || l.contains(QLatin1String("glow")) || l.contains(QLatin1String("vfx"))
                            || l.contains(QLatin1String("trail"))|| l.contains(QLatin1String("emiss"))
                            || matIsFxByShader(d4, rn))
                            m_fxMats.insert(idx);
                        if (l.contains(QLatin1String("gib"))    || l.contains(QLatin1String("gore"))
                            || l.contains(QLatin1String("flesh")) || l.contains(QLatin1String("viscera")))
                            m_gibMats.insert(idx);
                        if (l.contains(QLatin1String("cloth"))  || l.contains(QLatin1String("cape"))
                            || l.contains(QLatin1String("banner")) || l.contains(QLatin1String("flag")))
                            m_clothMats.insert(idx);
                    }
                    p.materialIndex = idx;
                    m_curGeo.primitives.append(std::move(p));
                }
            }
        }
        m_modelView->setGeometry(m_curGeo, /*keepView=*/true);
    }, &fault);

    if (!ok) {   // a child mesh faulted — fall back to the clean base and warn once
        m_curGeo      = m_baseGeo;
        m_appMatNames = m_baseMatNames;
        m_soaNames    = m_baseSoaNames;
        m_clothMats   = m_baseClothMats;   // restore FX/SIM/GIB sets too — the child indices
        m_fxMats      = m_baseFxMats;      // appended before the fault no longer match the roster
        m_gibMats     = m_baseGibMats;
        m_attachActive.clear();
        m_modelView->setGeometry(m_curGeo, /*keepView=*/true);
        populateAttachTree();   // clear the (now reverted) tick marks
        showToast(QStringLiteral("Couldn't attach that model — reverted."));
    }
    // Re-apply the playing clip + frame so the pose survives the setGeometry (both paths land here
    // with the geometry re-uploaded); the timer, if running, keeps advancing from the same frame.
    if (m_curAnim.valid) {
        m_modelView->setAnimation(m_curAnim);
        m_modelView->setFrame(savedAnimFrame);
    }
    m_modelView->update();
    // Rebuild the same detail panels the load path builds, so attachment parts get full parity:
    // the outliner lists each attachment part (checkable → hide) with its material → textures /
    // values / shaders, the PARTS page gets its visibility row, and cloth/stats recount. The
    // outliner resolves child materials by name (leavesFor reads Material/<name>.mat.json), so no
    // extra material wiring is needed. Look/selection are preserved (not reset like a fresh load).
    buildOutlinerSubtree();
    applyPartMaterials();   // sets m_partIsFx/Sim/Gib for every part (base + attachment) and
                            // applies the current FX/SIM/GIB toggle state to them.
    // Default-hide the ATTACHMENTS' FX parts (weapon glow/trail planes get in the way), while the
    // base model keeps whatever FX state it had. Only the attachment parts (index >= base count)
    // are unticked here; the base's FX still follow the global toggle. The user can re-tick an
    // attachment's FX part in the outliner / PARTS page to show it.
    if (!m_attachActive.isEmpty()) {
        const int baseCount = m_baseGeo.primitives.size();
        const int nParts    = m_curGeo.primitives.size();
        QVector<bool> childFx(nParts, false);
        bool any = false;
        for (int i = baseCount; i < nParts && i < m_partIsFx.size(); ++i)
            if (m_partIsFx[i]) { childFx[i] = true; any = true; }
        if (any) setFlaggedPartsChecked(childFx, /*checked=*/false);   // untick + hide them
    }
    queueTextureIcons();
    fillClothPage();
    fillPartsPage();
    updateStatsOverlay();
}

// » — hide the whole right column, Blender-style: no reserved sliver, the viewport takes the
// width, and the floating « on the N-strip brings it back. The panels keep their shown/hidden
// states — nothing inside the column is touched, the column itself just leaves.
void ModelsTab::setSideCollapsed(bool on)
{
    m_sideCollapsed = on;
    QSettings().setValue(QStringLiteral("models/panels/collapsed"), on);
    if (m_sideArrow) {
        m_sideArrow->setText(on ? QStringLiteral("«") : QStringLiteral("»"));
        m_sideArrow->setToolTip(on ? QStringLiteral("Show the side panels")
                                   : QStringLiteral("Hide the side panels (this arrow brings them back)"));
    }
    if (m_mainSplit && m_mainSplit->count() > 2)
        if (QWidget* pane = m_mainSplit->widget(2))
            pane->setVisible(!on && !m_viewMaxed);
}

// Strip toggle → panel in/out of the splitter. QSplitter honours child visibility: a hidden
// panel takes no space and its handle goes with it.
void ModelsTab::showPanel(int page, bool on)
{
    if (page < 0 || page >= m_rsections.size()) return;
    PanelBox* box = m_rsections[page];
    const bool was = !box->isHidden();
    box->setVisible(on);
    if (on && !was && !m_panelRestore) sizeNewPanel(box);
    // Opening the ATTACHMENTS panel: run the (deferred) attachment scan for the current model now.
    if (on && page == m_attachPage && m_curSno >= 0 && !m_attachCache.contains(m_curSno))
        scanAttachments();
    savePanelLayout();
}

// Arrival height for a freshly-toggled panel — the shared algorithm in PanelBox.h.
void ModelsTab::sizeNewPanel(PanelBox* box)
{
    panelBoxArrive(m_rstack, box);
}

// ▲▼ — move a panel one slot among the VISIBLE panels, so the arrows do what they look like
// they do even while others are hidden.
void ModelsTab::movePanel(int page, int delta)
{
    if (!m_rstack || page < 0 || page >= m_rsections.size()) return;
    PanelBox* box = m_rsections[page];
    QVector<int> vis;   // splitter indices of the up panels, in order (isHidden, not isVisible:
                        // the latter is false for every child while the tab itself is unshown)
    for (int i = 0; i < m_rstack->count(); ++i)
        if (!m_rstack->widget(i)->isHidden()) vis << i;
    const int cur = vis.indexOf(m_rstack->indexOf(box));
    const int tgt = cur + delta;
    if (cur < 0 || tgt < 0 || tgt >= vis.size()) return;   // already at an end
    const QList<int> sizes = m_rstack->sizes();
    m_rstack->insertWidget(vis[tgt], box);                 // moves the existing child
    m_rstack->setSizes(sizes);                             // insertWidget resets sizes — restore
    savePanelLayout();
}

// Which panels are up, in what order, at what heights (Settings ▸ Models tab opts in/out).
// NOTE: deliberately NOT QSplitter::saveState() — that blob is positional, and the hidden panels
// still occupy splitter slots, so index N means a different panel between runs. Names + heights
// in visible order survive reordering and re-registration.
void ModelsTab::savePanelLayout()
{
    if (!m_rstack || m_panelRestore) return;   // don't write while the ctor is replaying a layout
    QSettings s;
    const QList<int> sizes = m_rstack->sizes();
    QStringList shown, heights;
    for (int i = 0; i < m_rstack->count(); ++i) {
        QWidget* w = m_rstack->widget(i);
        if (w->isHidden()) continue;
        const int page = m_rsections.indexOf(static_cast<PanelBox*>(w));
        if (page < 0) continue;
        shown   << m_sectKeys.value(page).section(QLatin1Char('/'), -1);
        heights << QString::number(sizes.value(i));
    }
    s.setValue(QStringLiteral("models/panels/shown"), shown);
    s.setValue(QStringLiteral("models/panels/sizes"), heights);
}

void ModelsTab::updateTabCounts()
{
    auto set = [](QTabWidget* t, int i, const char* base, int n) {
        if (t && i < t->count())
            t->setTabText(i, QStringLiteral("%1 (%2)").arg(QLatin1String(base)).arg(n));
    };
    // Short labels — the long ones forced the tab bar into scroll arrows at panel width.
    set(m_matTabs, 0, "App",    m_matModel    ? m_matModel->rowCount()    : 0);
    set(m_matTabs, 1, "SubObj", m_subObjModel ? m_subObjModel->rowCount() : 0);
    set(m_matTabs, 2, "Mats",   m_matListModel? m_matListModel->rowCount(): 0);
    set(m_matTabs, 3, "VBufs",  m_vbModel     ? m_vbModel->rowCount()     : 0);
    set(m_detailTabs, 0, "Textures", m_matTexModel ? m_matTexModel->rowCount() : 0);
    set(m_detailTabs, 1, "Values",   m_matValModel ? m_matValModel->rowCount() : 0);
    set(m_detailTabs, 2, "Shaders",  m_shaderModel ? m_shaderModel->rowCount() : 0);
}

void ModelsTab::recomputePartVisibility()
{
    if (!m_modelView || !m_treeModel) return;
    // Outliner checkbox state per primitive index (the tree is the source of truth).
    QHash<int, bool> checked;
    m_treeModel->partChecks(checked);
    for (int i = 0; i < m_modelView->partCount(); ++i) {
        // FX/SIM visibility is now carried by the tree checks (the toggles flip them),
        // so visibility = tree check AND not hidden by the active look.
        bool vis = checked.value(i, true);
        if (vis && i < m_curGeo.primitives.size()
            && m_lookHiddenMats.contains(m_curGeo.primitives[i].materialIndex))
            vis = false;
        m_modelView->setPartVisible(i, vis);
    }
    updateStatsOverlay();   // the counts track what's actually drawn
    fillPartsPage();        // …and so do the PARTS page's boxes + "N of M shown" header
}

// FX/SIM toggle → check/uncheck the matching parts in the tree (a bulk action the
// user can still override per-part). Updates visibility once.
void ModelsTab::setFlaggedPartsChecked(const QVector<bool>& partFlags, bool checked)
{
    if (m_treeModel)
        m_treeModel->setPartChecks(partFlags, checked);   // silent bulk — no per-node recompute storm
    recomputePartVisibility();
}

void ModelsTab::clearAnimationSelection()
{
    m_playingAnim.clear();
    m_curAnim = {};   // drop the retained clip so exports don't embed a stale animation
    if (m_treeModel && m_animCount > 0)   // outliner badge back to the plain count
        m_treeModel->setNodeText(ModelOutlinerModel::AnimRoot,
                                 QStringLiteral("Animations · %1").arg(m_animCount));
    if (m_animTimer) m_animTimer->stop();
    if (m_modelView) m_modelView->clearAnimation();
    if (m_playBtn) m_playBtn->setIcon(transportGlyph(0));
    if (m_timeline) m_timeline->setVisible(false);
    if (m_anims) { m_anims->blockSignals(true); m_anims->clearSelection();
                   m_anims->setCurrentItem(nullptr); m_anims->blockSignals(false); }
}

QList<int> ModelsTab::selectedParts() const
{
    QList<int> out;
    if (!m_list || !m_treeModel) return out;
    const QModelIndexList sel = m_list->selectionModel()->selectedRows(0);
    for (const QModelIndex& ix : sel)
        if (ix.parent().isValid())              // subtree nodes only — browse rows aren't parts
            out += m_treeModel->partsUnder(ix);
    return out;
}

void ModelsTab::highlightMaterialsForParts(const QList<int>& parts)
{
    QSet<QString> names;
    for (int p : parts)
        if (p >= 0 && p < m_curGeo.primitives.size()) {
            const QString n = m_appMatNames.value(m_curGeo.primitives[p].materialIndex);
            if (!n.isEmpty()) names.insert(n);
        }
    static const QBrush hot(QColor(122, 26, 26)), none(Qt::NoBrush);   // Diablo-red row tint
    const int cols = m_matModel->columnCount();
    for (int r = 0; r < m_matModel->rowCount(); ++r) {
        QStandardItem* nameItem = m_matModel->item(r, 1);   // MAT. NAME
        const bool on = !names.isEmpty() && nameItem && names.contains(nameItem->text());
        for (int c = 0; c < cols; ++c)
            if (QStandardItem* it = m_matModel->item(r, c))
                it->setBackground(on ? hot : none);
    }
}

void ModelsTab::highlightMaterialsForLook(int look)
{
    static const QBrush hot(QColor(122, 26, 26));   // subtle Diablo-red row tint
    static const QBrush none(Qt::NoBrush);
    const int cols = m_matModel->columnCount();
    for (int r = 0; r < m_matModel->rowCount(); ++r) {
        QStandardItem* so = m_matModel->item(r, 2);   // SO/ column = SOA (look) index
        const bool on = look >= 0 && so && so->text().toInt() == look;
        for (int c = 0; c < cols; ++c)
            if (QStandardItem* it = m_matModel->item(r, c))
                it->setBackground(on ? hot : none);
    }
}

void ModelsTab::onMaterialSelected()
{
    const QModelIndex cur = m_mats->currentIndex();
    if (!cur.isValid())
        return;
    QStandardItem* nameItem = m_matModel->item(cur.row(), 1);   // MAT. NAME column
    if (nameItem)
        showMaterialTextures(nameItem->text());
}

void ModelsTab::showMaterialTextures(const QString& materialName)
{
    m_matTexModel->setRowCount(0);
    m_matValModel->setRowCount(0);
    m_shaderModel->setRowCount(0);
    m_matValues->clear();
    clearTexturePreview();
    if (materialName.isEmpty())
        return;

    const QString d4 = Config::d4dataDir();
    QFile f(QStringLiteral("%1/json/base/meta/Material/%2.mat.json").arg(d4, materialName));
    if (d4.isEmpty() || !f.open(QIODevice::ReadOnly))
        return;
    const QByteArray data = f.readAll();

    // Full material-value table (tUberMaterial.ptRunTimeMaterialValues[0]).
    {
        const QJsonObject rt = QJsonDocument::fromJson(data).object()
            .value(QStringLiteral("tUberMaterial")).toObject()
            .value(QStringLiteral("ptRunTimeMaterialValues")).toArray().at(0).toObject();
        auto addVals = [&](const QString& arrKey, bool vec) {
            for (const QJsonValue& ev : rt.value(arrKey).toArray()) {
                const QJsonObject tv = ev.toObject().value(QStringLiteral("tValue")).toObject();
                const QJsonObject mv = tv.value(QStringLiteral("snoMaterialValue")).toObject();
                QString valStr;
                if (vec) {
                    const QJsonObject vv = tv.value(QStringLiteral("value")).toObject();
                    valStr = QStringLiteral("%1, %2, %3, %4")
                        .arg(vv.value("x").toDouble(), 0, 'g', 4).arg(vv.value("y").toDouble(), 0, 'g', 4)
                        .arg(vv.value("z").toDouble(), 0, 'g', 4).arg(vv.value("w").toDouble(), 0, 'g', 4);
                } else {
                    valStr = QString::number(tv.value(QStringLiteral("value")).toDouble(), 'g', 4);
                }
                m_matValModel->appendRow(QList<QStandardItem*>{
                    new QStandardItem(QString::number(mv.value(QStringLiteral("__raw__")).toInt())),
                    new QStandardItem(mv.value(QStringLiteral("name")).toString()),
                    new QStandardItem(valStr)});
            }
        };
        addVals(QStringLiteral("arMaterialScalarValues"), false);
        addVals(QStringLiteral("arMaterialVectorValues"), true);
    }

    // Shaders tab: distinct ShaderMap references anywhere in the material.
    {
        QMap<int, QString> shaders;
        std::function<void(const QJsonValue&)> walk = [&](const QJsonValue& jv) {
            if (jv.isObject()) {
                const QJsonObject o = jv.toObject();
                if (o.value(QStringLiteral("__type__")).toString() == QLatin1String("DT_SNO")
                    && o.value(QStringLiteral("groupName")).toString() == QLatin1String("ShaderMap"))
                    shaders.insert(o.value(QStringLiteral("__raw__")).toInt(),
                                   o.value(QStringLiteral("name")).toString());
                for (const QString& k : o.keys()) walk(o.value(k));
            } else if (jv.isArray()) {
                for (const QJsonValue& e : jv.toArray()) walk(e);
            }
        };
        walk(QJsonDocument::fromJson(data).object());
        for (auto it = shaders.constBegin(); it != shaders.constEnd(); ++it)
            m_shaderModel->appendRow(QList<QStandardItem*>{
                new QStandardItem(QString::number(it.key())),
                new QStandardItem(it.value())});
    }

    const QVector<MatTexture> texs = parseMaterialJson(data);
    m_matTexModel->setRowCount(texs.size());
    bool hasBase = false;
    for (int r = 0; r < texs.size(); ++r) {
        m_matTexModel->setItem(r, 0, new QStandardItem(texs[r].role));   // SHADERTEX
        auto* sno = new QStandardItem;
        sno->setData(double(texs[r].texSno), Qt::DisplayRole);            // SNO
        m_matTexModel->setItem(r, 1, sno);
        m_matTexModel->setItem(r, 2, new QStandardItem(texs[r].texName)); // NAME
        if (texs[r].role == QLatin1String("BASE_COLOR") && !texs[r].texName.isEmpty())
            hasBase = true;
    }
    // No base-colour texture → the model renders this material BLACK (no borrowed
    // fallback). Flag it here and in the values line so it's obvious in the UI.
    if (!hasBase) {
        const int r = m_matTexModel->rowCount();
        m_matTexModel->insertRow(r);
        auto* note = new QStandardItem(QStringLiteral("⚠ FALLBACK"));
        note->setForeground(QBrush(QColor(0xd8, 0xa2, 0x3a)));
        m_matTexModel->setItem(r, 0, note);
        m_matTexModel->setItem(r, 2,
            new QStandardItem(QStringLiteral("no base texture — renders black")));
    }
    updateTabCounts();

    // MaterialValues (metalness / roughness / AO / emissive).
    const MaterialValues v = parseMaterialValues(data);
    if (v.valid) {
        QStringList parts;
        if (v.hasMetal) parts << QStringLiteral("Metal %1").arg(v.metal, 0, 'f', 2);
        if (v.hasRough) parts << QStringLiteral("Rough %1").arg(v.rough, 0, 'f', 2);
        if (v.hasAO)    parts << QStringLiteral("AO %1").arg(v.ao, 0, 'f', 2);
        if (v.hasEmisColor || v.hasEmisMult)
            parts << QStringLiteral("Emissive (%1, %2, %3)×%4")
                         .arg(v.emisR, 0, 'f', 2).arg(v.emisG, 0, 'f', 2)
                         .arg(v.emisB, 0, 'f', 2).arg(v.emisMult, 0, 'f', 2);
        m_matValues->setText(QStringLiteral("Values:  ") + parts.join(QStringLiteral("   ·   ")));
    }
    if (!hasBase)
        m_matValues->setText(QStringLiteral("⚠ No base texture — this material renders "
            "black (fallback). ") + m_matValues->text());

    // Fill the TEXTURE PREVIEW strip from this material's texture roster.
    showMaterialChannels();
}

void ModelsTab::loadDeferredMeta()
{
    // d4data-driven counts/lists, run after the selection settles (not on every
    // scrolled row). No CASC needed. Cached so re-visiting a model is instant.
    if (m_curSno < 0)
        return;
    if (AppearanceMeta::instance().ready())
        setInfo(QStringLiteral("Title"), AppearanceMeta::instance().titleFor(m_curSno));
    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty())
        return;

    // ── Textures count: distinct snoTex across all of the appearance's materials ──
    QFile af(QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4, m_curName));
    if (af.open(QIODevice::ReadOnly)) {
        const AppearanceInfo app = parseAppearanceJson(af.readAll());
        QSet<int> texSnos;
        for (const AppMaterial& am : app.materials) {
            QFile mf(QStringLiteral("%1/json/base/meta/Material/%2.mat.json").arg(d4, am.name));
            if (!mf.open(QIODevice::ReadOnly)) continue;
            for (const MatTexture& mt : parseMaterialJson(mf.readAll()))
                if (mt.texSno) texSnos.insert(mt.texSno);
        }
        setInfo(QStringLiteral("Textures"), QString::number(texSnos.size()));
    }

    // ── Animations: own clips + clips inherited from the shared base rig ──
    // Most NPCs own no clips directly — their animations are authored against a shared base rig
    // (e.g. npcF_base_invisible) and only resolve through the base-rig inheritance/skeleton-overlap
    // index. That index used to be built only when the "Animated" filter was picked, so a plain
    // model click showed nothing. Kick it off now (async, cached, guarded) so animations populate
    // for every rigged model; until it finishes we show the own-only fallback below.
    ensureAnimatedIndex();
    if (m_animatedScanned) {
        // Authoritative: own clips PLUS inherited base-rig clips (bone-hash retargeting), with the
        // inherited ones coloured + tooltipped. Matches the Animated/Rigged filters exactly.
        populateAnimList(m_curSno, m_curName.toLower());
    } else {
        // Pre-scan fallback: prefix-glob over Anim/*.ani.json whose snoAppearance == sno (own only).
        m_anims->clear();
        QStringList rows = m_animCache.value(m_curSno);
        if (rows.isEmpty() && !m_animCache.contains(m_curSno)) {
            const QString prefix = (m_curName.contains('_')
                                        ? m_curName.section('_', 0, 0) + QLatin1Char('_')
                                        : m_curName).toLower();
            const QString animDir = QStringLiteral("%1/json/base/meta/Anim").arg(d4);
            static const QRegularExpression rxApp(
                QStringLiteral("\"snoAppearance\":\\s*\\{[^{}]*?\"__raw__\":\\s*(\\d+)"));
            static const QRegularExpression rxFrames(QStringLiteral("\"nKeyframeCount\":\\s*(\\d+)"));
            QDirIterator it(animDir, QStringList{"*.ani.json"}, QDir::Files);
            while (it.hasNext()) {
                const QString fp = it.next();
                const QString base = it.fileName();
                if (!base.toLower().startsWith(prefix)) continue;
                QFile jf(fp);
                if (!jf.open(QIODevice::ReadOnly)) continue;
                const QString raw = QString::fromUtf8(jf.readAll());
                const auto m = rxApp.match(raw);
                if (!m.hasMatch() || m.captured(1).toInt() != m_curSno) continue;
                const QString nm = base.left(base.size() - 9);   // strip ".ani.json"
                const auto fm = rxFrames.match(raw);
                rows << (fm.hasMatch() ? QStringLiteral("%1  ·  %2 frames").arg(nm, fm.captured(1)) : nm);
            }
            rows.sort();
            m_animCache.insert(m_curSno, rows);
        }
        for (const QString& r : rows) {
            auto* it = new QListWidgetItem(r, m_anims);
            it->setData(Qt::UserRole, r.section(QStringLiteral("  ·  "), 0, 0));  // anim name
        }
        setInfo(QStringLiteral("Animations"), QString::number(rows.size()));
    }
}

// Decode a single clip for the current model's skeleton (no UI/state side effects). Returns
// an invalid DecodedAnim on any failure. Shared by playback and the "all animations" export.
AnimParser::DecodedAnim ModelsTab::decodeAnimByName(const QString& animName) const
{
    return decodeAnimForSkeleton(animName, m_curGeo);
}

// A model's clip NAMES (own + inherited base-rig, deduped) from the animation index — the batch
// export's clip lister, so "include all animations" works for models that were never LOADED in
// the viewport. Requires the anim index (ensureAnimatedIndex) to have completed.
QStringList ModelsTab::animClipsFor(int sno, const QString& nameLower) const
{
    QStringList rows = m_animRowsBySno.value(sno);                 // directly-owned clips
    const QString fam = animLongestFamily(nameLower, m_animFamilyPrefixes);
    if (!fam.isEmpty())
        for (const QString& r : m_animFamilyRows.value(fam))       // + inherited base-rig clips
            if (!rows.contains(r)) rows << r;
    QStringList names;
    for (const QString& r : rows) {
        const QString nm = r.section(QStringLiteral("  ·  "), 0, 0);
        if (!nm.isEmpty() && !names.contains(nm)) names << nm;
    }
    names.sort();
    return names;
}

// Geometry-parametrised decode — the batch pipeline hands each model's OWN skeleton in, where
// decodeAnimByName above always used the loaded model's.
AnimParser::DecodedAnim ModelsTab::decodeAnimForSkeleton(const QString& animName,
                                                         const ModelGeometry& geo) const
{
    AnimParser::DecodedAnim bad;
    if (animName.isEmpty() || !geo.valid || geo.skeleton.isEmpty() ||
        !m_reader || !m_reader->isReady())
        return bad;
    const QString d4 = Config::d4dataDir();
    QFile jf(QStringLiteral("%1/json/base/meta/Anim/%2.ani.json").arg(d4, animName));
    if (!jf.open(QIODevice::ReadOnly))
        return bad;
    const QJsonObject root = QJsonDocument::fromJson(jf.readAll()).object();
    const int animSno = root.value(QStringLiteral("__snoID__")).toInt();
    const QJsonArray perms = root.value(QStringLiteral("ptPermutations")).toArray();
    if (animSno <= 0 || perms.isEmpty())
        return bad;
    const QJsonObject perm = perms.first().toObject();
    const QJsonObject pv = perm.value(QStringLiteral("ptPayloadData")).toObject()
                               .value(QStringLiteral("value")).toObject();
    const int offset = pv.value(QStringLiteral("dataOffset")).toInt();
    const int frames = perm.value(QStringLiteral("nKeyframeCount")).toInt();
    const int comp = perm.value(QStringLiteral("flCompression")).toInt();
    const float fps = float(perm.value(QStringLiteral("flFrameRate")).toDouble(30.0));
    if (frames <= 0)
        return bad;
    const QByteArray payload = m_reader->readPayloadBySno(quint64(animSno));
    if (payload.isEmpty())
        return bad;
    QHash<quint32, AnimParser::RestTRS> rest;
    for (const ModelJoint& j : geo.skeleton) {
        AnimParser::RestTRS t; t.q = j.restQ; t.t = j.restT; t.s = j.restS;
        rest.insert(j.nameHash, t);
    }
    // Guard the decode (arbitrary .ani payload) — a malformed clip would otherwise access-violate
    // and kill the process. Covers both the interactive "play clip" path and the export path.
    AnimParser::DecodedAnim out;
    if (!seh::runGuarded("anim", [&]() { out = AnimParser::decode(payload, offset, frames, comp, fps, rest); }))
        return bad;
    return out;
}

// Gather the animations to embed on export. Priority: an explicit multi-selection in the
// ANIMATIONS list wins; else the export/animScope setting (0 = only the playing clip,
// 1 = every clip for the current model). Current-model only.
void ModelsTab::collectExportAnims(QVector<AnimParser::DecodedAnim>& anims, QStringList& names) const
{
    if (m_anims && m_anims->selectedItems().size() > 1) {   // exactly the ctrl/shift-selected clips
        for (const QListWidgetItem* it : m_anims->selectedItems()) {
            const QString nm = it->data(Qt::UserRole).toString();
            const AnimParser::DecodedAnim a = decodeAnimByName(nm);
            if (a.valid && !a.bones.isEmpty()) { anims << a; names << nm; }
        }
        if (!anims.isEmpty()) return;
    }
    if (QSettings().value(QStringLiteral("export/animScope"), 0).toInt() == 1) {
        // Every TRUE clip the model owns/inherits, taken from the ANIMATIONS list (m_anims is
        // populated on load in BOTH the authoritative and pre-scan states). The old path read
        // m_animCache, but that cache is cleared once the authoritative anim index finishes its
        // scan — so "all clips" silently exported nothing after the scan completed.
        if (m_anims) {
            for (int i = 0; i < m_anims->count(); ++i) {
                const QListWidgetItem* it = m_anims->item(i);
                if (it->flags() == Qt::NoItemFlags) continue;         // group-header row
                if (it->data(Qt::UserRole + 1).toBool()) continue;    // pulled row (added below if enabled)
                const QString nm = it->data(Qt::UserRole).toString();
                if (nm.isEmpty() || names.contains(nm)) continue;
                const AnimParser::DecodedAnim a = decodeAnimByName(nm);
                if (a.valid && !a.bones.isEmpty()) { anims << a; names << nm; }
            }
        }
    } else if (m_curAnim.valid && !m_playingAnim.isEmpty()) {
        anims << m_curAnim; names << m_playingAnim;
    }
    // Pulled clips (manually retargeted from another model — the gold rows). Included when the export
    // setting is on, on top of whatever scope produced above, deduped by clip name.
    if (m_anims && QSettings().value(QStringLiteral("export/includePulledAnims"), false).toBool()) {
        for (int i = 0; i < m_anims->count(); ++i) {
            const QListWidgetItem* it = m_anims->item(i);
            if (!it->data(Qt::UserRole + 1).toBool()) continue;   // only the "pulled" rows
            const QString nm = it->data(Qt::UserRole).toString();
            if (nm.isEmpty() || names.contains(nm)) continue;
            const AnimParser::DecodedAnim a = decodeAnimByName(nm);
            if (a.valid && !a.bones.isEmpty()) { anims << a; names << nm; }
        }
    }
}

// (exportAnimationsOnly() moved to ModelsTab_Export.cpp)

void ModelsTab::playAnimationByName(const QString& animName)
{
    // A fresh single-clip play cancels any running "play set" queue; the queue-advance path
    // (m_advancingQueue) is the exception — it keeps the queue alive as it steps through.
    if (!m_advancingQueue) { m_playQueue.clear(); m_playQueueIdx = -1; }
    // Female-preview toggle: swap in the snoFemaleOverrideAnim variant when one exists.
    QString clip = animName;
    if (m_previewFemale) {
        const QString fem = m_femalePair.value(animName.toLower());
        if (!fem.isEmpty()) clip = fem;
    }
    const AnimParser::DecodedAnim anim = decodeAnimByName(clip);
    if (!anim.valid)
        return;
    m_playingAnim = clip;
    m_curAnim = anim;   // retained for "include animation" .glb export
    if (m_treeModel)    // live outliner badge: which clip is playing right now
        m_treeModel->setNodeText(ModelOutlinerModel::AnimRoot,
            QStringLiteral("Animations · %1 · ▶ %2").arg(m_animCount).arg(clip));
    if (!seh::runGuarded("setAnim", [&]() { m_modelView->setAnimation(anim); })) return;
    m_timeline->setVisible(true);
    m_animSlider->blockSignals(true);
    m_animSlider->setRange(0, anim.frameCount - 1);
    m_animSlider->setValue(0);
    // Frame ticks: aim for ~40 marks, snapped to a friendly step (1/2/5/10/25/50…).
    {
        int step = qMax(1, (anim.frameCount + 39) / 40);
        if (step > 2 && step <= 7)       step = 5;
        else if (step > 7 && step <= 15) step = 10;
        else if (step > 15)              step = ((step + 24) / 25) * 25;
        m_animSlider->setTickInterval(step);
        m_animSlider->setPageStep(qMax(1, anim.frameCount / 10));
    }
    m_animSlider->blockSignals(false);
    if (m_frameSpin) {   // keep the editable frame field in range for the new clip
        QSignalBlocker b(m_frameSpin);
        m_frameSpin->setRange(0, anim.frameCount - 1);
        m_frameSpin->setValue(0);
    }
    if (m_frameMax) m_frameMax->setText(QStringLiteral("/ %1").arg(anim.frameCount - 1));
    m_animFps = anim.frameRate > 0 ? anim.frameRate : 30.0f;
    applyAnimSpeed();
    m_animTimer->start();
    m_playBtn->setIcon(transportGlyph(1));
}

// Queue a list of clips and play them back-to-back (tickAnimation advances at each clip's end).
// With Loop checked the whole set repeats; otherwise it stops after the last clip.
void ModelsTab::playAnimationSet(const QStringList& clips)
{
    if (clips.isEmpty()) return;
    m_playQueue = clips;
    m_playQueueIdx = 0;
    m_advancingQueue = true;                 // don't let playAnimationByName wipe the queue we just set
    playAnimationByName(clips.first());
    m_advancingQueue = false;
}

void ModelsTab::applyAnimSpeed()
{
    float mult = 1.0f;
    if (m_speedCombo) {
        const QString s = m_speedCombo->currentText();   // "0.5x" / "1x" / "2x"
        bool ok = false;
        const float v = s.left(s.size() - 1).toFloat(&ok);   // strip trailing 'x'
        if (ok && v > 0.0f) mult = v;
    }
    const float eff = m_animFps * mult;
    m_animTimer->setInterval(eff > 0.0f ? int(1000.0f / eff) : 33);
}

void ModelsTab::tickAnimation()
{
    const int fc = m_modelView ? m_modelView->animFrameCount() : 0;
    if (fc <= 0) { m_animTimer->stop(); return; }
    int next = m_animSlider->value() + 1;
    if (next >= fc) {
        // Playing a whole set → advance to the next clip in the queue at each clip's end.
        if (!m_playQueue.isEmpty()) {
            int ni = m_playQueueIdx + 1;
            if (ni >= m_playQueue.size()) {
                if (m_loopCheck && m_loopCheck->isChecked()) ni = 0;   // loop the set
                else {
                    m_playQueue.clear(); m_playQueueIdx = -1;
                    m_animTimer->stop(); m_playBtn->setIcon(transportGlyph(0));
                    return;
                }
            }
            m_playQueueIdx = ni;
            m_advancingQueue = true;
            playAnimationByName(m_playQueue.at(ni));   // restarts the timer for the next clip
            m_advancingQueue = false;
            return;
        }
        if (m_loopCheck && m_loopCheck->isChecked()) {
            next = 0;
        } else {
            m_animTimer->stop();
            m_playBtn->setIcon(transportGlyph(0));
            return;
        }
    }
    m_animSlider->setValue(next);
}

// Kick off an async load: the heavy CASC read + parse runs on a worker thread, and
// the result is applied back on the UI thread (applyLoadedGeometry), so selecting a
// model no longer hitches the UI. Out-of-order results are dropped via a token.
void ModelsTab::loadGeometry()
{
    loadDeferredMeta();
    if (m_curSno < 0 || !m_reader || !m_reader->isReady())
        return;
    const int sno = m_curSno;
    const int token = ++m_geoToken;
    // Crash guard: record the SNO we're about to parse and flush to disk. If the
    // parser hard-crashes on a malformed model, the value survives the crash; on the
    // next launch loadList() sees it and refuses to auto-load that model again.
    { QSettings s; s.setValue(QStringLiteral("models/loadGuard"), sno); s.sync(); }
    // Cache hit: this model was parsed already this session → skip the CASC read + parse thread
    // entirely and go straight to the (still fault-guarded) GPU stage. Makes re-selects instant.
    if (std::shared_ptr<ModelGeometry>* cached = m_geoCache.object(sno)) {
        applyLoadedGeometry(*cached, token);
        return;
    }
    setScan(QStringLiteral("load"), QStringLiteral("Loading model…"));
    // Immediate feedback in the viewport while the CASC read + parse + GPU upload runs on the
    // worker thread — otherwise a heavy mesh just sits on the previous frame and looks frozen.
    if (m_modelView)
        m_modelView->setOverlayText(m_curName.isEmpty()
            ? QStringLiteral("Loading model…")
            : QStringLiteral("Loading  %1 …").arg(m_curName));

    CascReader* reader = m_reader;
    const QString name = m_curName;
    std::thread([this, reader, sno, token, name]() {
        auto geo = std::make_shared<ModelGeometry>();
        // Parse under hardware-fault protection: a malformed model that would
        // otherwise segfault the worker (killing the whole process) is caught
        // here, and we hand the fault back to the UI thread to quarantine it.
        seh::HardwareFault fault;
        const bool ok = seh::runGuarded("parse", [&]() {
            const QByteArray meta = reader->readMetaBySno(quint64(sno));
            const QByteArray payload = reader->readPayloadBySno(quint64(sno));
            if (!meta.isEmpty() && !payload.isEmpty())
                *geo = ModelParser::parseApp(meta, payload);
        }, &fault);
        const QString faultWhat = fault.what;
        QMetaObject::invokeMethod(this, [this, geo, token, ok, sno, name, faultWhat]() {
            if (!ok) {
                if (token == m_geoToken)
                    handleModelFault(sno, name, faultWhat.isEmpty() ? QStringLiteral("parse") : faultWhat);
                return;
            }
            // Cache the parsed geometry for instant re-selects (cost ≈ interleaved-vertex KB).
            if (geo->valid && !m_geoCache.contains(sno)) {
                int kb = 1;
                for (const MeshPrimitive& p : geo->primitives) kb += p.vertices.size() / 12;
                m_geoCache.insert(sno, new std::shared_ptr<ModelGeometry>(geo), kb);
            }
            applyLoadedGeometry(geo, token);
        }, Qt::QueuedConnection);
    }).detach();
}

void ModelsTab::applyLoadedGeometry(std::shared_ptr<ModelGeometry> geo, int token)
{
    if (token != m_geoToken) return;   // a newer load superseded this one — drop it
    updateIndexStatus();   // clear the "Loading model…" status
    if (!geo->valid) {
        // Parse succeeded (produced no geometry) — not a crash, so clear the guard.
        { QSettings s; s.setValue(QStringLiteral("models/loadGuard"), -1); s.sync(); }
        qWarning("loadGeometry: parse produced no geometry");
        if (m_exportBtn) m_exportBtn->setEnabled(false);
        if (m_modelView)
            m_modelView->setOverlayText(QStringLiteral(
                "This asset has no displayable geometry.\n"
                "(It may be a placeholder, an effect, or a reference-only model.)"));
        // True-geometry refinement for the ✗ presence badge: this appearance HAS a payload but
        // parses to zero geometry — record it so its icon shows ✗ (not a false ✓).
        if (m_curSno > 0) { m_noRenderSnos.insert(m_curSno); if (m_listModel) m_listModel->refreshIconForSno(m_curSno); }
        resetAttachments();   // don't leave the panel showing the previous model's attachments
        return;
    }
    // Confirmed geometry — clear any provisional ✗ for this sno so its badge reads ✓.
    if (m_curSno > 0 && m_noRenderSnos.remove(m_curSno) && m_listModel) m_listModel->refreshIconForSno(m_curSno);
    m_curGeo = *geo;   // COPY, not move: geo is shared with m_geoCache, so it must stay intact
                       // (QVector members are copy-on-write, so this stays cheap in practice)
    // Rig hardpoints (attach sockets) for the viewport overlay — read once per load from the
    // appearance .app.json (needs the parsed skeleton, which is in m_curGeo now).
    Hardpoints::readInto(m_curGeo, QStringLiteral("%1/json/base/meta/Appearance/%2.app.json")
                                       .arg(Config::d4dataDir(), m_curName));
    if (m_modelView) m_modelView->setHardpoints(m_curGeo.hardpoints);
    // Snapshot the pristine base for the Attachments panel: attach/detach always re-assembles
    // from this, and the hardpoint map resolves child sockets against this rig. m_appMatNames /
    // m_soaNames were set by showAppearance (metadata) before this geometry load.
    m_baseGeo      = m_curGeo;
    m_baseMatNames = m_appMatNames;
    m_baseSoaNames = m_soaNames;
    m_baseClothMats = m_clothMats;   // base FX/SIM/GIB classification (by roster index) — the
    m_baseFxMats    = m_fxMats;      // attach rebuild restores these then classifies child mats,
    m_baseGibMats   = m_gibMats;     // so the FX/SIM/GIB toggles hide attachment parts too.
    m_haveBase     = true;
    m_attachActive.clear();
    m_baseHpMap = ModelAttach::loadHardpointMap(
        QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(Config::d4dataDir(), m_curName));
    scanAttachments();   // populate the panel (kicks the entity index if it isn't ready yet)
    if (!m_modelView)   // no viewport → no GPU stage to crash; clear the guard
        { QSettings s; s.setValue(QStringLiteral("models/loadGuard"), -1); s.sync(); }
    if (m_modelView) {
        m_modelView->setOverlayText(QString());   // a model loaded → drop any stale hint/blocklist text
        // The crash almost always happens in the GPU stage (geometry flatten in
        // setGeometry + buffer upload + cloth build + draw), which is DEFERRED to the
        // next paint — AFTER parse. Force a synchronous paint now so that dangerous work
        // runs under fault protection while the load guard is still set. If the GPU stage
        // faults, runGuarded catches it (access violation → C++ exception via /EHa); we
        // quarantine the model in-session instead of the process dying. Belt-and-braces:
        // the on-disk guard also survives a *fatal* fault so the next launch blocklists it.
        seh::HardwareFault gpuFault;
        const bool gpuOk = seh::runGuarded("render", [&]() {
            // Keep the current orbit angle across model loads (don't snap the rotation back to the
            // default). Auto-frame re-fits the centre+zoom to the (visible) model unless the user
            // turned it off; the very first model is always framed so it can't load off-screen.
            const bool autoFrame = QSettings().value(QStringLiteral("models/autoFrame"), true).toBool();
            m_modelView->setGeometry(m_curGeo, /*keepView=*/true);
            if (autoFrame || !m_everFramed)
                m_modelView->frameAll(/*keepRotation=*/true, /*animate=*/false);   // snap so it's on-screen at once
            m_everFramed = true;
            m_modelView->repaint();
        }, &gpuFault);
        { QSettings s; s.setValue(QStringLiteral("models/loadGuard"), -1); s.sync(); }   // handled (survived or caught)
        if (!gpuOk) {
            handleModelFault(m_curSno, m_curName,
                             gpuFault.what.isEmpty() ? QStringLiteral("render") : gpuFault.what);
            return;
        }
        // Vertex Buffers tab.
        m_vbModel->setRowCount(0);
        {
            auto semName = [](int s) -> QString {
                switch (s) {
                case 0: return QStringLiteral("POSITION");   case 1: return QStringLiteral("TEXCOORD_0");
                case 2: return QStringLiteral("TEXCOORD_1");  case 7: return QStringLiteral("COLOR_0");
                case 8: return QStringLiteral("COLOR_1");     case 9: return QStringLiteral("NORMAL");
                case 10: return QStringLiteral("TANGENT");    case 11: return QStringLiteral("BLENDINDICES");
                case 12: return QStringLiteral("BLENDWEIGHTS");
                default: return QStringLiteral("SEM_%1").arg(s);
                }
            };
            for (const VertexBufferInfo& vb : m_curGeo.vertexBuffers) {
                QStringList al;
                for (const VertexAttr& a : vb.attrs)
                    al << QStringLiteral("%1@%2").arg(semName(a.semantic)).arg(a.offset);
                m_vbModel->appendRow(QList<QStandardItem*>{
                    new QStandardItem(QString::number(vb.index)),
                    new QStandardItem(QString::number(vb.stride)),
                    new QStandardItem(QString::number(vb.vertexCount)),
                    new QStandardItem(al.join(QStringLiteral(", ")))});
            }
        }
        updateTabCounts();
        buildOutlinerSubtree();   // hang animations/armature/parts off the model's row in the list
        m_modelView->setHighlightPart(-1);
        m_currentLook = 0;
        m_lookHiddenMats.clear();   // default look (SOA 0) shows every part
        applyPartMaterials();   // decode + push per-part textures for the active look
        queueTextureIcons();    // then lazily thumbnail the outliner's texture leaves
        fillClothPage();        // authored cloth tuning for this model's simulated pieces
        fillPartsPage();        // per-part visibility table
        updateStatsOverlay();   // fresh geometry → recount
        // Lazily cache a 3D render thumbnail of this model for the list icon column
        // (guarded — a fault in the GL grab is caught and just skips the thumbnail). Skip entirely
        // when we already have this model's thumbnail (re-select of an already-viewed model).
        if (!m_renderCache.contains(m_curSno)) {
            renderGuardStage(m_curSno, m_curName, "grab");
            QImage thumb;
            seh::runGuarded("thumbnail", [&]() { thumb = m_modelView->grabThumbnail(48); });
            endRenderGuard();
            if (!thumb.isNull()) {
                m_renderCache.insert(m_curSno, QPixmap::fromImage(thumb));
                thumb.save(thumbCachePath(m_curSno), "PNG");   // persist across sessions
                if (m_listModel) m_listModel->refreshIconForSno(m_curSno);   // repaint one row, don't reflow
            }
        }
    }
    if (m_exportBtn)
        m_exportBtn->setEnabled(m_curGeo.valid);
    // A new model means new geometry, a new rig and a rebuilt cloth sim, but the overlay flags are
    // per-view state that must survive the swap. They were only ever pushed once, from the
    // constructor, so anything a model load reset stayed reset until the user toggled it by hand.
    reapplyOverlays();
}

// Single place that pushes overlay state to the viewport: master gate AND each box's own state.
// Called on model load, and by the master toggle. Anything that needs overlays refreshed calls
// THIS — never setShow*() directly, or the master gate gets bypassed (that bug has happened).
void ModelsTab::reapplyOverlays()
{
    if (!m_modelView) return;
    for (const auto& e : m_overlayChks)
        if (e.first) e.second(m_overlaysOn && e.first->isChecked());
    applyModelRig();   // rig flags share the same gate (see applyModelRig)
}

// Build the Blender-style subtree under the loaded model's row in the outliner:
//   Animations · N
//   Armature · B bones · P phys · T named   →   one node per bone (translated names green)
//   <part> · <tris>t  [eye checkbox]        →   <material>  →  Textures / Values / Shaders
// Node refs: Part = primitive index, Bone = joint index, Material = its m_matModel row,
// groups = the m_detailTabs tab they jump to. Data is all already in memory — this only
// arranges it, so it's cheap enough to rebuild on every load.
void ModelsTab::buildOutlinerSubtree()
{
    if (!m_treeModel || !m_modelView) return;
    using N = ModelOutlinerModel::Node;
    auto* root = new N(ModelOutlinerModel::AnimRoot, QString());   // hidden container

    // Per-kind visibility gates — the funnel popup's "Outliner shows" checkboxes.
    QSettings st;
    auto show = [&st](const char* k) {
        return st.value(QStringLiteral("models/outliner/show_") + QLatin1String(k), true).toBool();
    };

    // Looks — appearance variants, with EXCLUSIVE eye toggles (radio-like: checking one applies
    // it and unchecks the rest; see the lookToggled wiring). Mirrors the LOOKS table 1:1.
    if (show("looks") && m_looksModel && m_looksModel->rowCount() > 0) {
        auto* lr = root->add(new N(ModelOutlinerModel::LookRoot,
            QStringLiteral("Looks · %1").arg(m_looksModel->rowCount())));
        for (int r = 0; r < m_looksModel->rowCount(); ++r) {
            const QString nm = m_looksModel->item(r, 3) ? m_looksModel->item(r, 3)->text() : QString();
            auto* lk = lr->add(new N(ModelOutlinerModel::Look,
                nm.isEmpty() ? QStringLiteral("Look %1").arg(r)
                             : QStringLiteral("%1 · %2").arg(r).arg(nm), r));
            lk->checkable = true;
            lk->check = (r == m_currentLook) ? Qt::Checked : Qt::Unchecked;
        }
    }

    if (show("anims") && m_animCount > 0) {
        auto* ar = root->add(new N(ModelOutlinerModel::AnimRoot,
                                   QStringLiteral("Animations · %1").arg(m_animCount)));
        // One leaf per clip, mirroring the ANIMATIONS list 1:1 (same text incl. keyframe count;
        // ref = list row for selection sync, aux = the real clip id for playback).
        for (int i = 0; m_anims && i < m_anims->count(); ++i) {
            QListWidgetItem* it = m_anims->item(i);
            if (it->flags() == Qt::NoItemFlags) continue;          // group-header rows
            if (it->data(Qt::UserRole + 1).toBool()) continue;     // pulled from another model — the
                                                                   // outliner shows TRUE clips only
            auto* leaf = ar->add(new N(ModelOutlinerModel::Anim, it->text(), i));
            leaf->aux = it->data(Qt::UserRole).toString();
        }
    }

    const auto& skel = m_curGeo.skeleton;
    if (show("armature") && !skel.isEmpty()) {
        const int nb = skel.size();
        // Mirror buildSpringBones' gate EXACTLY: with a zero/degenerate/over-half base split the
        // sim refuses to spring anything (bad-split guard), so the honest phys count is 0 — naive
        // "size - base" reported every bone as physics on models whose parser left nBaseBones at 0.
        const int base = m_modelView->baseBoneCount();
        const int phys = (base > 0 && base < nb && (nb - base) <= nb / 2) ? nb - base : 0;
        int named = 0;
        for (const ModelJoint& j : skel)
            if (!GLModelWidget::translateBoneName(j.nameHash).isEmpty()) ++named;
        QString armText = QStringLiteral("Armature · %1 bones · %2 named").arg(nb).arg(named);
        if (phys > 0) armText += QStringLiteral(" · %1 phys").arg(phys);
        auto* arm = root->add(new N(ModelOutlinerModel::Armature, armText));
        if (show("bones"))
            for (int b = 0; b < skel.size(); ++b) {
                const QString tr = GLModelWidget::translateBoneName(skel[b].nameHash);
                auto* bn = arm->add(new N(ModelOutlinerModel::Bone,
                                          tr.isEmpty() ? skel[b].name : tr, b));
                bn->translated = !tr.isEmpty();
            }
    }

    // Per-material leaf lists. Leaf `ref`s are ROW INDEXES into the detail tables, so ordering
    // here MUST mirror showMaterialTextures exactly: textures = parseMaterialJson order (the same
    // function), values = scalars then vectors in array order, shaders = ShaderMap refs ascending
    // by SNO (QMap iteration). Parsed once per unique material and cached for this build.
    struct MatLeaves { QStringList tex, val, shad, texRole; QVector<QPair<QString, int>> texRef; };
    QHash<QString, MatLeaves> leafCache;
    const QString d4 = Config::d4dataDir();
    auto leavesFor = [&](const QString& matName) -> const MatLeaves& {
        auto it = leafCache.find(matName);
        if (it != leafCache.end()) return it.value();
        MatLeaves L;
        QFile f(QStringLiteral("%1/json/base/meta/Material/%2.mat.json").arg(d4, matName));
        if (!d4.isEmpty() && f.open(QIODevice::ReadOnly)) {
            const QByteArray data = f.readAll();
            for (const MatTexture& t : parseMaterialJson(data)) {
                L.tex << (t.texName.isEmpty() ? t.role
                                              : QStringLiteral("%1 · %2").arg(t.role, t.texName));
                L.texRef << qMakePair(t.texName, t.texSno);   // for the lazy thumbnail decode
                L.texRole << t.role;                          // for the material channel strip
            }
            const QJsonObject rt = QJsonDocument::fromJson(data).object()
                .value(QStringLiteral("tUberMaterial")).toObject()
                .value(QStringLiteral("ptRunTimeMaterialValues")).toArray().at(0).toObject();
            auto addVals = [&](const QString& arrKey, bool vec) {
                for (const QJsonValue& ev : rt.value(arrKey).toArray()) {
                    const QJsonObject tv = ev.toObject().value(QStringLiteral("tValue")).toObject();
                    const QString name = tv.value(QStringLiteral("snoMaterialValue")).toObject()
                                             .value(QStringLiteral("name")).toString();
                    QString valStr;
                    if (vec) {
                        const QJsonObject vv = tv.value(QStringLiteral("value")).toObject();
                        valStr = QStringLiteral("%1, %2, %3, %4")
                            .arg(vv.value("x").toDouble(), 0, 'g', 4).arg(vv.value("y").toDouble(), 0, 'g', 4)
                            .arg(vv.value("z").toDouble(), 0, 'g', 4).arg(vv.value("w").toDouble(), 0, 'g', 4);
                    } else {
                        valStr = QString::number(tv.value(QStringLiteral("value")).toDouble(), 'g', 4);
                    }
                    L.val << QStringLiteral("%1 = %2").arg(name, valStr);
                }
            };
            addVals(QStringLiteral("arMaterialScalarValues"), false);
            addVals(QStringLiteral("arMaterialVectorValues"), true);
            QMap<int, QString> shaders;
            std::function<void(const QJsonValue&)> walk = [&](const QJsonValue& jv) {
                if (jv.isObject()) {
                    const QJsonObject o = jv.toObject();
                    if (o.value(QStringLiteral("__type__")).toString() == QLatin1String("DT_SNO")
                        && o.value(QStringLiteral("groupName")).toString() == QLatin1String("ShaderMap"))
                        shaders.insert(o.value(QStringLiteral("__raw__")).toInt(),
                                       o.value(QStringLiteral("name")).toString());
                    for (const QString& k : o.keys()) walk(o.value(k));
                } else if (jv.isArray()) {
                    for (const QJsonValue& e : jv.toArray()) walk(e);
                }
            };
            walk(QJsonDocument::fromJson(data).object());
            for (auto s = shaders.constBegin(); s != shaders.constEnd(); ++s) L.shad << s.value();
        }
        return *leafCache.insert(matName, L);
    };

    for (int i = 0; i < m_modelView->partCount(); ++i) {
        auto* part = root->add(new N(ModelOutlinerModel::Part, partLabel(i), i));
        part->checkable = true;
        part->check = Qt::Checked;
        // Authoritative sub-object facts for the tooltip (tNameInfo research): slot code
        // (dwSlotHash = DJB2 of "hlm"/"bts"/…), the sub-object hash, and its LOD reach.
        // Labels stay material-derived — for gear tNameInfo only restates the filename, and
        // monsters carry all-zero tNameInfo, so a tooltip is the honest surface for this data.
        {
            QStringList facts;
            const QString slot = slotLabelForHash(m_curGeo.primitives.value(i).slotHash);
            if (!slot.isEmpty()) facts << QStringLiteral("slot %1").arg(slot);
            if (m_subObjModel && i < m_subObjModel->rowCount()) {
                if (QStandardItem* h = m_subObjModel->item(i, 3))
                    if (!h->text().isEmpty())
                        facts << QStringLiteral("subobj 0x%1").arg(h->text());
                if (QStandardItem* ml = m_subObjModel->item(i, 6)) {
                    const int v = ml->text().toInt();
                    facts << (v < 0 ? QStringLiteral("all LODs")
                                    : QStringLiteral("up to LOD %1").arg(v));
                }
            }
            part->aux = facts.join(QStringLiteral(" · "));
        }
        const QString matName = m_appMatNames.value(m_curGeo.primitives.value(i).materialIndex);
        if (!matName.isEmpty()) {
            int matRow = -1;   // locate the material's row so selecting the node selects the table row
            for (int r = 0; m_matModel && r < m_matModel->rowCount(); ++r)
                if (QStandardItem* it = m_matModel->item(r, 1))
                    if (it->text() == matName) { matRow = r; break; }
            auto* mat = part->add(new N(ModelOutlinerModel::Material, matName, matRow));
            const MatLeaves& L = leavesFor(matName);
            // Channel strip under the material: one tile per canonical PBR channel (lazy decode).
            if (show("tiles")) {
                static const struct { const char* prefix; const char* label; } kChan[] = {
                    {"BASE_COLOR", "BASE"}, {"NORMAL", "NORM"}, {"ROUGHNESS", "ROUGH"},
                    {"METALLIC", "METAL"},  {"EMISSIVE", "EMIS"}, {"MASK", "MASK"}};
                auto* strip = new N(ModelOutlinerModel::MatTiles, QString());
                for (const auto& ch : kChan)
                    for (int t = 0; t < L.texRole.size(); ++t)
                        if (L.texRole[t].startsWith(QLatin1String(ch.prefix))
                            && !L.texRef[t].first.isEmpty()) {
                            strip->tiles << L.texRef[t];
                            strip->tileLabels << QString::fromLatin1(ch.label);
                            break;   // first match per channel
                        }
                if (strip->tiles.isEmpty()) delete strip;
                else mat->add(strip);
            }
            if (show("textures")) {
                auto* tg = mat->add(new N(ModelOutlinerModel::TexGroup,
                    QStringLiteral("Textures · %1").arg(L.tex.size()), 0));
                for (int t = 0; t < L.tex.size(); ++t) {
                    auto* leaf = tg->add(new N(ModelOutlinerModel::Texture, L.tex[t], t));
                    leaf->aux  = L.texRef[t].first;             // texture asset name
                    leaf->hash = quint32(L.texRef[t].second);   // texture SNO — thumbnail decode key
                    if (show("tiles") && !leaf->aux.isEmpty()) {   // RGBA strip under the texture
                        auto* ts = leaf->add(new N(ModelOutlinerModel::TexTiles, QString()));
                        static const char* kRgba[5] = {"RGB", "R", "G", "B", "A"};
                        for (int c = 0; c < 5; ++c) {
                            ts->tiles << L.texRef[t];
                            ts->tileLabels << QString::fromLatin1(kRgba[c]);
                        }
                    }
                }
            }
            if (show("values")) {
                auto* vg = mat->add(new N(ModelOutlinerModel::ValueGroup,
                    QStringLiteral("Values · %1").arg(L.val.size()), 1));
                for (int v = 0; v < L.val.size(); ++v)
                    vg->add(new N(ModelOutlinerModel::Value, L.val[v], v));
            }
            if (show("shaders")) {
                auto* sg = mat->add(new N(ModelOutlinerModel::ShaderGroup,
                    QStringLiteral("Shaders · %1").arg(L.shad.size()), 2));
                for (int s = 0; s < L.shad.size(); ++s)
                    sg->add(new N(ModelOutlinerModel::Shader, L.shad[s], s));
            }
        }
    }

    m_treeModel->setSubtree(m_curSno, root);
    const int row = m_treeModel->subtreeRow();
    if (row >= 0) {
        if (QSettings().value(QStringLiteral("models/outliner/autoExpand"), true).toBool())
            m_list->expand(m_treeModel->index(row, 0));   // open the model; kids stay collapsed
        // Belt-and-braces for the "loaded but not highlighted" case: if a refilter cleared the
        // selection between click and load-finish, re-select the loaded row now.
        if (!m_list->currentIndex().isValid())
            m_list->selectionModel()->setCurrentIndex(
                m_treeModel->index(row, 0),
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    updateBreadcrumb();
}

// Statistics overlay — counts only what's actually drawn, so hiding/soloing parts moves the
// numbers. Cheap: sums per-primitive sizes already in memory (no geometry walk).
void ModelsTab::updateStatsOverlay()
{
    if (!m_statsOv || !m_statsOv->isVisible()) return;
    if (!m_curGeo.valid || m_curGeo.primitives.isEmpty()) {
        m_statsOv->setText(QString());
        m_statsOv->adjustSize();
        return;
    }
    qint64 verts = 0, tris = 0;
    int shown = 0;
    const int total = m_curGeo.primitives.size();
    for (int i = 0; i < total; ++i) {
        if (m_modelView && !m_modelView->partVisible(i)) continue;
        ++shown;
        verts += m_curGeo.primitives[i].vertices.size();
        tris  += m_curGeo.primitives[i].indices.size() / 3;
    }
    const QLocale loc;
    QString s = QStringLiteral("Verts %1 · Tris %2 · Parts %3/%4")
                    .arg(loc.toString(verts), loc.toString(tris))
                    .arg(shown).arg(total);
    if (!m_curGeo.skeleton.isEmpty())
        s += QStringLiteral(" · Bones %1").arg(m_curGeo.skeleton.size());
    m_statsOv->setText(s);
    m_statsOv->adjustSize();
}

// Publish a table's natural height — its rows, up to maxRows — as a HINT (kWantH), read by
// PanelBox::preferredHeight when the panel first opens.
//
// This used to call setFixedHeight, which was correct in the fold-section column (a 5-row PARTS
// table should cost 5 rows, not a slab) and is now exactly wrong: a table pinned to 250px inside
// a splitter panel the user has dragged to 350px leaves 100px that no widget can absorb, and
// QBoxLayout vents that as a gap between the title and the table. Height is the handle's job.
void autoSizeTable(QTableView* t, int maxRows)
{
    if (!t || !t->model()) return;
    const int rows = qMax(1, qMin(t->model()->rowCount(), maxRows));
    const int rowH = qMax(12, t->verticalHeader()->defaultSectionSize());
    const int head = t->horizontalHeader()->isVisible() ? t->horizontalHeader()->height() : 0;
    t->setProperty(kWantH, head + rows * rowH + 2 * t->frameWidth() + 2);
}

// PARTS page — one row per mesh piece. Rebuilt wholesale (a model has tens of parts, not
// thousands) whenever geometry or visibility changes, so it can never drift from the outliner:
// both read the same checks and push through the same recomputePartVisibility().
void ModelsTab::fillPartsPage()
{
    if (!m_partsModel) return;
    m_partsPageSync = true;             // our own setCheckState must not re-enter itemChanged
    m_partsModel->setRowCount(0);
    int shown = 0;
    const int total = m_curGeo.valid ? m_curGeo.primitives.size() : 0;
    for (int i = 0; i < total; ++i) {
        const MeshPrimitive& pr = m_curGeo.primitives[i];
        const bool vis = !m_modelView || m_modelView->partVisible(i);
        if (vis) ++shown;
        auto* name = new QStandardItem(partLabel(i).section(QStringLiteral(" · "), 0, 0));
        name->setCheckable(true);
        name->setCheckState(vis ? Qt::Checked : Qt::Unchecked);
        name->setData(i, Qt::UserRole);   // primitive index — the identity everything else uses
        auto* tris = new QStandardItem;
        tris->setData(int(pr.indices.size() / 3), Qt::DisplayRole);   // numeric → sorts properly
        tris->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        QString slot = slotLabelForHash(pr.slotHash);
        if (slot.isEmpty() && m_clothMats.contains(pr.materialIndex))
            slot = QStringLiteral("cloth");
        m_partsModel->appendRow(QList<QStandardItem*>{
            name, tris, new QStandardItem(slot),
            new QStandardItem(m_appMatNames.value(pr.materialIndex))});
    }
    m_partsPageSync = false;
    autoSizeTable(m_partsView, 12);
    if (m_partsHdr)
        m_partsHdr->setText(total > 0 ? QStringLiteral("PARTS · %1 of %2 shown").arg(shown).arg(total)
                                      : QStringLiteral("PARTS"));
}

// CLOTH page — the authored NvCloth tuning for every simulated piece on this model. Read from
// Cloth/<material>.clt.json (or <material>_sim.clt.json — both conventions ship), the same files
// the Wardrobe's cloth tuning uses. Only the fields that actually mean something to a modder;
// values are the game's own, not the tool's sim defaults.
void ModelsTab::fillClothPage()
{
    if (!m_clothModel) return;
    m_clothModel->setRowCount(0);
    const QString d4 = Config::d4dataDir();
    int pieces = 0;
    if (!d4.isEmpty()) {
        // GAME = the authored .clt.json value. LIVE = what the tool's viewport sim currently uses
        // for the equivalent knob (Physics panel / hair-class overrides), so divergence is
        // visible. Blank LIVE = the tool has no equivalent (it doesn't simulate that field).
        const GLModelWidget::ClothParams lp = m_modelView ? m_modelView->clothParams()
                                                          : GLModelWidget::ClothParams{};
        auto num = [](double v) { return QString::number(v, 'g', 4); };
        static const struct { const char* key; const char* label; int live; } kFields[] = {
            {"flBoneTrackingFactor",  "Bone tracking",         1},
            {"flActorTrackingFactor", "Actor tracking",        2},
            {"flStretchingStiffness", "Stretch stiffness",     3},
            {"flHorizontalStiffness", "Horizontal stiffness",  4},
            {"flShearStiffness",      "Shear stiffness",       5},
            {"flBendingStiffness",    "Bending stiffness",     6},
            {"flDampingFactor",       "Damping",               7},
            {"flDragFactor",          "Drag",                  0},
            {"flLiftFactor",          "Lift",                  0},
            {"flWindFactor",          "Wind factor",           0},
            {"flExplosionFactor",     "Explosion factor",      0},
            {"flImpulseFactor",       "Impulse factor",        0},
            {"flImpulseCap",          "Impulse cap",           0},
            {"flDensity",             "Density",               0},
            {"flFrictionScale",       "Friction scale",        8},
            {"flAttachmentStiffness", "Attachment stiffness",  9},
            {"nIterations",           "Solver iterations",    10}};
        auto liveVal = [&](int id) -> QString {
            switch (id) {
            case 1:  return num(lp.boneTracking);
            case 2:  return num(lp.actorTracking);
            case 3:  return num(lp.stretchStiffness);
            case 4:  return num(lp.horizStiffness);
            case 5:  return num(lp.shearStiffness);
            case 6:  return num(lp.bendStiffness);
            case 7:  return num(lp.damping);
            case 8:  return num(lp.friction);
            case 9:  return num(lp.attachStiffness);
            case 10: return QString::number(lp.iterations);
            default: return QString();   // not simulated by the tool
            }
        };
        // One block per distinct cloth material on this model (dedup: looks share materials).
        QSet<QString> done;
        for (int mi : m_clothMats) {
            const QString nm = m_appMatNames.value(mi);
            if (nm.isEmpty() || done.contains(nm)) continue;
            done.insert(nm);
            const QString base = QStringLiteral("%1/json/base/meta/Cloth/%2").arg(d4, nm);
            QFile f(base + QStringLiteral(".clt.json"));
            if (!f.exists()) f.setFileName(base + QStringLiteral("_sim.clt.json"));
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
            const QJsonObject tune = root.value(QStringLiteral("tClothTuning")).toObject();
            if (tune.isEmpty()) continue;
            ++pieces;
            auto row = [this](const QString& piece, const QString& k, const QString& game,
                              const QString& live = QString()) {
                auto* gi = new QStandardItem(game);
                auto* li = new QStandardItem(live);
                // Tint the pair when the tool's sim meaningfully disagrees with the game.
                if (!live.isEmpty()) {
                    const double a = game.toDouble(), b = live.toDouble();
                    if (qAbs(a - b) > 0.01 * qMax(1.0, qAbs(a))) {
                        const QBrush warn(QColor(0xd8, 0xa2, 0x3a));
                        gi->setForeground(warn);
                        li->setForeground(warn);
                    }
                }
                m_clothModel->appendRow(QList<QStandardItem*>{
                    new QStandardItem(piece), new QStandardItem(k), gi, li});
            };
            for (const auto& fl : kFields) {
                const QJsonValue v = tune.value(QLatin1String(fl.key));
                if (v.isUndefined() || v.isNull()) continue;
                row(prettyMatName(nm), QString::fromLatin1(fl.label),
                    QString::number(v.toDouble(), 'g', 4), liveVal(fl.live));
            }
            // Gravity + self-wind are vectors; world wind sits at the file's top level.
            auto vec3 = [](const QJsonObject& o) {
                return QStringLiteral("%1, %2, %3")
                    .arg(o.value(QStringLiteral("x")).toDouble(), 0, 'g', 3)
                    .arg(o.value(QStringLiteral("y")).toDouble(), 0, 'g', 3)
                    .arg(o.value(QStringLiteral("z")).toDouble(), 0, 'g', 3);
            };
            if (tune.contains(QStringLiteral("vGravity")))
                row(prettyMatName(nm), QStringLiteral("Gravity"),
                    vec3(tune.value(QStringLiteral("vGravity")).toObject()));
            if (tune.contains(QStringLiteral("vSelfWind")))
                row(prettyMatName(nm), QStringLiteral("Self wind"),
                    vec3(tune.value(QStringLiteral("vSelfWind")).toObject()));
            if (root.contains(QStringLiteral("flWorldWindStrength")))
                row(prettyMatName(nm), QStringLiteral("World wind strength"),
                    QString::number(root.value(QStringLiteral("flWorldWindStrength")).toDouble(), 'g', 4));
            if (tune.contains(QStringLiteral("fUseShapeCollision")))
                row(prettyMatName(nm), QStringLiteral("Shape collision"),
                    tune.value(QStringLiteral("fUseShapeCollision")).toBool()
                        ? QStringLiteral("yes") : QStringLiteral("no"));
        }
    }
    if (m_clothHdr)
        m_clothHdr->setText(pieces > 0 ? QStringLiteral("CLOTH · %1 simulated piece%2")
                                             .arg(pieces).arg(pieces == 1 ? QString() : QStringLiteral("s"))
                                       : QStringLiteral("CLOTH"));
    if (m_clothModel->rowCount() == 0)
        m_clothModel->appendRow(QList<QStandardItem*>{
            new QStandardItem(QStringLiteral("—")),
            new QStandardItem(QStringLiteral("No simulated cloth on this model")),
            new QStandardItem(QString()), new QStandardItem(QString())});
    autoSizeTable(m_clothView, 12);
    if (m_clothView)
        m_clothView->setToolTip(QStringLiteral(
            "GAME = the value D4 ships for this piece (Cloth/<name>.clt.json).\n"
            "LIVE = what this tool's viewport sim uses (Physics panel). Gold = they differ;\n"
            "blank LIVE = the tool doesn't simulate that field. Exports are unaffected."));
}

// Blender-style breadcrumb over the properties column: model › part › material › …
// Reads the current outliner selection; for a plain browse selection it's just the model name.
void ModelsTab::updateBreadcrumb()
{
    if (!m_breadcrumb) return;
    // Build the index chain (model row → … → current node) and render each hop as a link.
    m_breadcrumbIx.clear();
    QStringList labels;
    const QModelIndex cur = m_list ? m_list->currentIndex() : QModelIndex();
    if (cur.isValid() && cur.parent().isValid() && m_treeModel) {
        QVector<QModelIndex> chain;
        for (QModelIndex w = cur; w.isValid(); w = w.parent())
            chain.prepend(w.siblingAtColumn(0));
        for (const QModelIndex& ix : chain) {
            m_breadcrumbIx << QPersistentModelIndex(ix);
            const auto* n = m_treeModel->node(ix);
            labels << (n ? n->text
                         : (m_curName.isEmpty() ? QStringLiteral("—") : m_curName));
        }
    } else {
        labels << (m_curName.isEmpty() ? QStringLiteral("—") : m_curName);
        const int host = m_treeModel ? m_treeModel->subtreeRow() : -1;
        if (host >= 0) m_breadcrumbIx << QPersistentModelIndex(m_treeModel->index(host, 0));
    }
    QStringList html, plain;
    for (int i = 0; i < labels.size(); ++i) {
        plain << labels[i];
        if (i < m_breadcrumbIx.size() && m_breadcrumbIx[i].isValid())
            html << QStringLiteral("<a href=\"%1\" style=\"color:#9a8f78;text-decoration:none;\">%2</a>")
                        .arg(i)
                        .arg(labels[i].toHtmlEscaped());
        else
            html << labels[i].toHtmlEscaped();
    }
    m_breadcrumb->setText(html.join(QStringLiteral("&nbsp;&nbsp;›&nbsp;&nbsp;")));
    m_breadcrumb->setToolTip(plain.join(QStringLiteral("  ›  ")));   // clips, never widens
}

// Decode thumbnails for the outliner's texture leaves — LAZILY, one per timer tick. A full
// BC decode of a 2k texture takes tens of ms; a synchronous burst for every leaf would freeze
// the UI right when the model appears. Persistent indexes make the queue self-invalidating:
// filter/sort resets or loading another model just fizzle the stale entries.
void ModelsTab::queueTextureIcons()
{
    if (!m_treeModel) return;
    m_texIconQueue.clear();
    for (const QModelIndex& ix : m_treeModel->iconlessTextureLeaves())
        m_texIconQueue << QPersistentModelIndex(ix);
    if (m_texIconQueue.isEmpty()) return;
    if (!m_texIconTimer) {
        m_texIconTimer = new QTimer(this);
        m_texIconTimer->setInterval(80);
        connect(m_texIconTimer, &QTimer::timeout, this, [this]() {
            while (!m_texIconQueue.isEmpty()) {
                const QPersistentModelIndex pix = m_texIconQueue.takeFirst();
                if (!pix.isValid()) continue;   // model reset / new subtree since queuing
                const QModelIndex ix(pix);
                const auto* n = m_treeModel ? m_treeModel->node(ix) : nullptr;
                if (!n) continue;
                if (n->kind == ModelOutlinerModel::Texture && !n->aux.isEmpty()) {
                    const QImage img = decodeTexImage(n->aux, int(n->hash));
                    if (!img.isNull())
                        m_treeModel->setNodeIcon(ix, QPixmap::fromImage(
                            img.scaled(18, 18, Qt::KeepAspectRatioByExpanding,
                                       Qt::SmoothTransformation)));
                } else if (n->kind == ModelOutlinerModel::TexTiles && !n->tiles.isEmpty()) {
                    // ONE decode fills the whole RGBA strip (channel splits are cheap).
                    const QImage img = decodeTexImage(n->tiles[0].first, n->tiles[0].second);
                    for (int t = 0; t < n->tiles.size(); ++t)
                        m_treeModel->setNodeTileImage(ix, t,
                            t == 0 ? img : channelImage(img, t - 1));
                } else if (n->kind == ModelOutlinerModel::MatTiles
                           && n->tilesDone < n->tiles.size()) {
                    const int t = n->tilesDone;   // one channel texture per tick
                    m_treeModel->setNodeTileImage(ix, t,
                        decodeTexImage(n->tiles[t].first, n->tiles[t].second));
                    if (n->tilesDone < n->tiles.size())
                        m_texIconQueue.append(pix);   // more tiles → back of the queue
                } else {
                    continue;   // nothing to do for this node — take the next one
                }
                break;   // exactly one decode per tick — keep the UI responsive
            }
            if (m_texIconQueue.isEmpty() && m_texIconTimer) m_texIconTimer->stop();
        });
    }
    m_texIconTimer->start();
}

// Compose the parts-tree label for primitive i from the active-look material name
// (suffix-stripped), an equipment slot tag, or a generic fallback, + triangle count.
QString ModelsTab::partLabel(int i) const
{
    const MeshPrimitive& pr = m_curGeo.primitives.value(i);
    QString nm = m_appMatNames.value(pr.materialIndex);
    if (!nm.isEmpty()) nm = prettyMatName(nm);
    if (nm.isEmpty()) nm = slotLabelForHash(pr.slotHash);
    if (nm.isEmpty()) nm = QStringLiteral("Submesh %1").arg(i);
    if (m_clothMats.contains(pr.materialIndex)) nm += QStringLiteral(" (cloth)");
    if (i < m_partIsFx.size()  && m_partIsFx[i])  nm += QStringLiteral("  [FX]");   // parity with Stable/Wardrobe
    if (i < m_partIsSim.size() && m_partIsSim[i]) nm += QStringLiteral("  [SIM]");
    return QStringLiteral("%1 · %2t").arg(nm).arg(m_modelView ? m_modelView->partTriangles(i) : 0);
}

// Decode each part's textures for the current m_appMatNames (set by the active
// look) and push them to the viewport. Re-runnable when the look changes.
// The class/gender BODY-SKIN material for the loaded model (e.g. barF_P00_BOD) — the material
// the Wardrobe re-skins black skin placeholders with. Resolved by globbing d4data's Material
// dir for "<prefix>_P*_BOD*" (P00 = the class's face/body piece); cached per prefix, so it's
// one directory scan per class ever. Empty for non-class models (monsters etc.) — the
// placeholder then stays black, exactly as before.
QString ModelsTab::bodySkinMaterial()
{
    const QString pref = m_curName.section(QLatin1Char('_'), 0, 0);
    if (pref.isEmpty()) return {};
    const QString key = pref.toLower();
    const auto it = m_bodySkinByPrefix.constFind(key);
    if (it != m_bodySkinByPrefix.constEnd()) return it.value();
    QString found;
    const QDir dir(Config::d4dataDir() + QStringLiteral("/json/base/meta/Material"));
    const QStringList cands = dir.entryList(
        QStringList{pref + QStringLiteral("_P*_BOD*.mat.json")}, QDir::Files, QDir::Name);
    if (!cands.isEmpty()) found = cands.first().chopped(9);   // strip ".mat.json"
    m_bodySkinByPrefix.insert(key, found);
    if (!found.isEmpty())
        qInfo().noquote() << "fillSkin:" << pref << "→" << found;
    return found;
}

void ModelsTab::applyPartMaterials()
{
    if (!m_modelView || !m_curGeo.valid) return;
    const int nParts = m_curGeo.primitives.size();
    m_partIsSim = QVector<bool>(nParts, false);
    m_partIsFx  = QVector<bool>(nParts, false);
    m_partIsGib = QVector<bool>(nParts, false);

    // Dominant named material (by triangle count) — textures cloth/unnamed slots
    // (loincloth, chains, etc.) that share the body armour's material.
    QHash<int, int> triByMat;
    for (int i = 0; i < nParts; ++i)
        triByMat[m_curGeo.primitives[i].materialIndex] += m_modelView->partTriangles(i);
    QString token;   // appearance token, e.g. "stor248_TRS" → prefer the piece's own mat
    {
        const int sp = m_curName.indexOf(QStringLiteral("stor"), 0, Qt::CaseInsensitive);
        if (sp >= 0) token = m_curName.mid(sp);
    }
    QString fallbackMat; int bestTris = -1; bool matchedToken = false;
    for (auto it = triByMat.constBegin(); it != triByMat.constEnd(); ++it) {
        if (m_clothMats.contains(it.key())) continue;
        const QString n = m_appMatNames.value(it.key());
        if (n.isEmpty()) continue;
        const bool tok = !token.isEmpty() && n.contains(token, Qt::CaseInsensitive);
        const bool better = (tok && !matchedToken)
                         || (tok == matchedToken && it.value() > bestTris);
        if (better) { matchedToken = tok; bestTris = it.value(); fallbackMat = n; }
    }

    // A material's decoded textures never change, so cache them PERSISTENTLY (across re-selects
    // and look changes) in m_texCache keyed by "ROLE|material". Copy the hit out immediately —
    // a later insert() may evict/delete the stored object. Cost is tracked in KB.
    auto cached = [this](const char* role, const QString& mn, auto decodeFn) -> QImage {
        const QString key = QString::fromLatin1(role) + QLatin1Char('|') + mn;
        if (QImage* c = m_texCache.object(key)) return *c;
        const QImage img = decodeFn();
        m_texCache.insert(key, new QImage(img), qMax(1, int(img.sizeInBytes() / 1024)));
        return img;
    };
    auto decodeBase = [&](const QString& mn) -> QImage {
        return cached("BASE", mn, [&] { return baseColorForMaterial(mn); });
    };
    auto decodeNorm = [&](const QString& mn) -> QImage {
        return cached("NORM", mn, [&] { return normalForMaterial(mn); });
    };
    auto decodeOrm = [&](const QString& mn) -> QImage {
        return cached("ORM", mn, [&] { return ormForMaterial(mn); });
    };
    auto decodeEmis = [&](const QString& mn) -> QImage {
        return cached("EMIS", mn, [&] { return emissiveForMaterial(mn); });
    };
    auto roleCached = [&](const QString& mn, const char* role) -> QImage {
        return cached(role, mn, [&] { return textureByRole(mn, role); });
    };
    // Fur: dwFlags bit 5 (0x20), a fur shader-map, or a "_fur" material name — parity with the
    // Wardrobe's isFurMaterial. The Models tab previously never marked fur parts, so fur props like
    // trophy_bar032_stor (…_mat_fur) rendered with no shells.
    QHash<QString, bool> furCache;
    auto matIsFur = [&](const QString& mn) -> bool {
        if (mn.isEmpty()) return false;
        const auto it = furCache.constFind(mn);
        if (it != furCache.constEnd()) return it.value();
        bool isF = mn.contains(QLatin1String("_fur"), Qt::CaseInsensitive);
        QFile mf(QStringLiteral("%1/json/base/meta/Material/%2.mat.json").arg(Config::d4dataDir(), mn));
        if (mf.open(QIODevice::ReadOnly)) {
            const QJsonObject root = QJsonDocument::fromJson(mf.readAll()).object();
            if ((root.value(QStringLiteral("dwFlags")).toInt() & 0x20) != 0) isF = true;
            else if (root.value(QStringLiteral("tUberMaterial")).toObject()
                         .value(QStringLiteral("snoShaderMap")).toObject()
                         .value(QStringLiteral("name")).toString()
                         .contains(QLatin1String("fur"), Qt::CaseInsensitive)) isF = true;
        }
        furCache.insert(mn, isF); return isF;
    };
    // Material scalar factors (metal/rough) are a tiny pure-per-material JSON read → local dedupe.
    QHash<QString, QPair<float, float>> facCache;
    auto decodeFactors = [&](const QString& mn) -> QPair<float, float> {
        const auto it = facCache.constFind(mn);
        if (it != facCache.constEnd()) return it.value();
        QPair<float, float> mr(0.0f, 0.6f);
        const QString d4 = Config::d4dataDir();
        QFile mf(QStringLiteral("%1/json/base/meta/Material/%2.mat.json").arg(d4, mn));
        if (mf.open(QIODevice::ReadOnly)) {
            const MaterialValues v = parseMaterialValues(mf.readAll());
            if (v.hasMetal) mr.first = float(v.metal);
            if (v.hasRough) mr.second = float(v.rough);
        }
        facCache.insert(mn, mr); return mr;
    };
    // SKIN_MASK presence, memoized per material (was re-parsed once PER PART — materials shared by
    // several parts re-read the same .mat.json each time).
    QHash<QString, bool> skinRoleCache;
    auto matHasSkinMask = [&](const QString& mn) -> bool {
        const auto it = skinRoleCache.constFind(mn);
        if (it != skinRoleCache.constEnd()) return it.value();
        const bool r = materialHasRole(mn, "SKIN_MASK");
        skinRoleCache.insert(mn, r); return r;
    };

    // ── Parallel texture prefetch (#2): decode every (role|material) this look needs across all
    // CPU cores up-front, so the sequential apply-loop below hits m_texCache instead of running the
    // heavy BC7 decode one-material-at-a-time on the UI thread. Decode is pure — CascReader
    // serialises the CASC read with its own mutex, BcDecode is CPU-only, and each worker writes a
    // distinct result slot (no shared state). Results are inserted into m_texCache serially here on
    // the UI thread, so the cache itself is never touched concurrently. Shortens the click→textured
    // stall to roughly 1/cores for every map, base/ORM included.
    {
        static const char* const kPrefetchRoles[] = { "BASE", "NORM", "ORM", "EMIS",
            "DETAIL_NORMAL", "DETAIL_ROUGHNESS", "TRANSLUCENCY", "MASK_PRIMARY", "DYE_MASK", "DYE_RAMP" };
        QStringList keys; QSet<QString> seenKeys;
        for (int i = 0; i < nParts; ++i) {
            QString um = m_appMatNames.value(m_curGeo.primitives[i].materialIndex);
            if (um.isEmpty()) um = fallbackMat;
            if (um.isEmpty()) continue;
            for (const char* r : kPrefetchRoles) {
                const QString key = QString::fromLatin1(r) + QLatin1Char('|') + um;
                if (m_texCache.contains(key) || seenKeys.contains(key)) continue;   // already have / queued
                seenKeys.insert(key);
                keys << key;
            }
        }
        if (keys.size() > 1) {   // 0–1 keys → the loop below decodes it directly; nothing to parallelise
            const std::vector<QImage> imgs = parallelMap<QImage>(keys,
                [this](const QString& key) -> QImage {
                    const int bar = key.indexOf(QLatin1Char('|'));
                    const QString role = key.left(bar);
                    const QString mat  = key.mid(bar + 1);
                    if (role == QLatin1String("BASE")) return baseColorForMaterial(mat);
                    if (role == QLatin1String("NORM")) return normalForMaterial(mat);
                    if (role == QLatin1String("ORM"))  return ormForMaterial(mat);
                    if (role == QLatin1String("EMIS")) return emissiveForMaterial(mat);
                    return textureByRole(mat, role.toLatin1().constData());
                },
                [](int, int) {},          // a handful of textures → no progress reporting needed
                /*installSeh=*/true);     // a malformed texture faults the worker, not the process
            for (int i = 0; i < keys.size(); ++i) {
                const QImage& img = imgs[size_t(i)];
                m_texCache.insert(keys.at(i), new QImage(img), qMax(1, int(img.sizeInBytes() / 1024)));
            }
        }
    }

    QVector<QImage> partTex, partNorm, partOrm, partEmis;
    QVector<QImage> partDetailN, partDetailR, partTrans, partMask, partDyeMask, partDyeRamp;
    QVector<QImage> partFurMask, partFurNoise;
    QVector<int>    partHair, partSkin, partDyeRegion, partFur;
    QHash<QString, int> dyeRegionOf;   // dyeable material → dye-colour slot (0..3)
    int nextDyeRegion = 0;
    QVector<float>  partMetal, partRough;
    partTex.reserve(nParts); partNorm.reserve(nParts); partOrm.reserve(nParts);
    partEmis.reserve(nParts);
    // "Fill skin" (Settings ▸ Models): armor pieces carry a BLACK skin-placeholder material
    // (…skin_mat) that the game fills with the character's body skin at runtime — the Wardrobe
    // does the same substitution. On, the placeholder decodes the class/gender body-skin
    // material's textures instead, so armor previews show skin rather than black cutouts.
    const bool fillSkin = QSettings().value(QStringLiteral("models/fillSkin"), true).toBool();
    for (int i = 0; i < nParts; ++i) {
        const MeshPrimitive& prim = m_curGeo.primitives[i];
        m_partIsSim[i] = m_clothMats.contains(prim.materialIndex);
        m_partIsFx[i]  = m_fxMats.contains(prim.materialIndex);
        m_partIsGib[i] = m_gibMats.contains(prim.materialIndex);
        QString usedMat = m_appMatNames.value(prim.materialIndex);
        if (usedMat.isEmpty()) usedMat = fallbackMat;   // unnamed cloth → body material
        bool skinFilled = false;
        if (fillSkin && usedMat.contains(QLatin1String("skin_mat"), Qt::CaseInsensitive)) {
            const QString bod = bodySkinMaterial();
            if (!bod.isEmpty()) { usedMat = bod; skinFilled = true; }
        }
        QImage base, norm, orm, emis, dN, dR, tr, mk, dm, rp;
        int hair = 0, skin = 0;
        if (!usedMat.isEmpty()) {
            base = decodeBase(usedMat);
            norm = decodeNorm(usedMat);
            orm  = decodeOrm(usedMat);
            emis = decodeEmis(usedMat);
            dN = roleCached(usedMat, "DETAIL_NORMAL");
            dR = roleCached(usedMat, "DETAIL_ROUGHNESS");
            tr = roleCached(usedMat, "TRANSLUCENCY");
            mk = roleCached(usedMat, "MASK_PRIMARY");
            dm = roleCached(usedMat, "DYE_MASK");
            rp = roleCached(usedMat, "DYE_RAMP");
            if (!dm.isNull() && dm.width() <= 8) { dm = QImage(); rp = QImage(); }  // 4×4 placeholder = no dye
            const QString lm = usedMat.toLower();
            hair = lm.contains(QLatin1String("hair")) ? 1 : 0;
            skin = (skinFilled || lm.contains(QLatin1String("skin"))
                    || matHasSkinMask(usedMat)) ? 1 : 0;
        }
        // A material with no usable base texture (e.g. armor_skin_mat's 4×4 dye-base)
        // renders BLACK rather than borrowing another material's texture — and the
        // Material Textures tab flags it (see showMaterialTextures).
        if (!usedMat.isEmpty() && (base.isNull() || base.width() <= 8)) {
            static const QImage kBlack = [] {
                QImage im(2, 2, QImage::Format_RGBA8888); im.fill(Qt::black); return im;
            }();
            base = kBlack; norm = QImage(); orm = QImage(); emis = QImage();
            dN = QImage(); dR = QImage(); tr = QImage(); mk = QImage(); hair = 0; skin = 0;
            dm = QImage(); rp = QImage();
        }
        const QPair<float, float> mr = usedMat.isEmpty() ? qMakePair(0.0f, 0.6f)
                                                         : decodeFactors(usedMat);
        partTex.append(base);
        partNorm.append(norm);
        partOrm.append(orm);
        partEmis.append(emis);
        partDetailN.append(dN);
        partDetailR.append(dR);
        partTrans.append(tr);
        partMask.append(mk);
        {   // fur shells (parity with Wardrobe): mark the part + feed its density mask + strand noise
            const bool isFur = matIsFur(usedMat);
            partFur.append(isFur ? 1 : 0);
            partFurMask.append(isFur ? mk : QImage());
            partFurNoise.append(isFur ? roleCached(usedMat, "NOISE_PROCEDURAL") : QImage());
        }
        partDyeMask.append(dm);
        partDyeRamp.append(rp);
        int dyeReg = 0;   // each distinct dyeable material → its own dye-colour slot
        if (!dm.isNull() && !usedMat.isEmpty()) {
            const int found = dyeRegionOf.value(usedMat, -1);
            if (found >= 0) dyeReg = found;
            else { dyeReg = qMin(nextDyeRegion, 3); dyeRegionOf.insert(usedMat, dyeReg);
                   if (nextDyeRegion < 3) ++nextDyeRegion; }
        }
        partDyeRegion.append(dyeReg);
        partHair.append(hair);
        partSkin.append(skin);
        partMetal.append(mr.first);
        partRough.append(mr.second);
    }
    m_modelView->setPartTextures(partTex);
    m_modelView->setPartNormals(partNorm);
    m_modelView->setPartOrm(partOrm);
    m_modelView->setPartEmissive(partEmis);
    m_modelView->setPartDetailNormals(partDetailN, {}, {});   // Models tab: single map → layer 0
    m_modelView->setPartDetailRoughs(partDetailR, {}, {});
    m_modelView->setPartTranslucency(partTrans);
    m_modelView->setPartMask(partMask);
    m_modelView->setPartFur(partFur);
    m_modelView->setPartFurMask(partFurMask);
    m_modelView->setPartFurNoise(partFurNoise);
    m_modelView->setPartDyeMask(partDyeMask);
    m_modelView->setPartDyeRamp(partDyeRamp);
    m_modelView->setPartDyeRegion(partDyeRegion);
    QVector<int> partCloth(nParts, 0);
    for (int i = 0; i < nParts && i < m_partIsSim.size(); ++i) partCloth[i] = m_partIsSim[i] ? 1 : 0;
    m_modelView->setPartFlags(partHair, partSkin, partCloth);

    // A pigment is 4 colours (DyeMask zones); show all 4 when the model is dyeable.
    m_dyeRegionsUsed = dyeRegionOf.isEmpty() ? 0 : 4;
    for (int r = 0; r < 4; ++r) m_dyeRegionName[r].clear();
    updateDyeSlotUsage();
    m_modelView->setPartFactors(partMetal, partRough);

    // Relabel the outliner's part nodes in place (look may have changed the material names),
    // preserving each node's visibility check.
    if (m_treeModel)
        m_treeModel->relabelParts([this](int prim) { return partLabel(prim); });
    // Apply the current FX/SIM toggle state to the freshly-built tree (a new model
    // starts all-checked, so without this an OFF toggle wouldn't hide its parts
    // until manually flipped). setFlaggedPartsChecked recomputes visibility.
    setFlaggedPartsChecked(m_partIsFx,  m_showFx);
    setFlaggedPartsChecked(m_partIsSim, m_showSim);
    setFlaggedPartsChecked(m_partIsGib, m_showGib);
}

// Switch the active look (SOA index) → rebuild m_appMatNames from m_soaNames and
// re-texture every part so the 3D preview reflects the chosen look.
void ModelsTab::applyLook(int look)
{
    if (look < 0 || m_soaNames.isEmpty() || !m_curGeo.valid) return;
    m_currentLook = look;
    m_lookHiddenMats.clear();
    for (int mi = 0; mi < m_appMatNames.size() && mi < m_soaNames.size(); ++mi) {
        const QVector<QString>& soas = m_soaNames[mi];
        if (soas.isEmpty()) continue;
        // An out-of-range look clamps to the last SOA; an in-range but empty SOA
        // means this material isn't part of the look → hide its mesh parts.
        if (look < soas.size()) {
            const QString n = soas[look];
            if (n.isEmpty()) m_lookHiddenMats.insert(mi);   // hidden this look
            else             m_appMatNames[mi] = n;
        } else {
            const QString n = soas.last();
            if (!n.isEmpty()) m_appMatNames[mi] = n;
        }
    }
    applyPartMaterials();
}

// (exportSelectedGlb(), appendFitReferenceBody(), exportModels() moved to ModelsTab_Export.cpp)

// Re-sync the Tex/Anim toggles from the shared export settings (e.g. after the Export ▸
// Settings dialog changed them). Block signals so the resync doesn't rewrite the keys.
void ModelsTab::onSettingsChanged()
{
    // "Fill skin" toggled → re-decode the loaded model's part textures with/without the body-skin
    // substitution. Cache keys are per material name, so no invalidation is needed — the two
    // states simply decode different materials.
    {
        static bool lastFill = QSettings().value(QStringLiteral("models/fillSkin"), true).toBool();
        const bool fill = QSettings().value(QStringLiteral("models/fillSkin"), true).toBool();
        if (fill != lastFill) {
            lastFill = fill;
            if (m_curGeo.valid) applyPartMaterials();
        }
    }
    // Auto-Load lives in Settings ▸ Models tab now — drive the (hidden) button so its own slot
    // runs, keeping m_autoLoad and every other consumer on one path.
    if (m_autoLoadBtn) {
        const bool on = QSettings().value(QStringLiteral("models/autoLoad"), true).toBool();
        if (m_autoLoadBtn->isChecked() != on) m_autoLoadBtn->setChecked(on);
    }
    if (m_exportTex) {
        m_exportTex->blockSignals(true);
        m_exportTex->setChecked(QSettings().value(QStringLiteral("export/includeTex"), true).toBool());
        m_exportTex->blockSignals(false);
    }
    if (m_exportAnim) {
        m_exportAnim->blockSignals(true);
        m_exportAnim->setChecked(QSettings().value(QStringLiteral("export/includeAnim"), false).toBool());
        m_exportAnim->blockSignals(false);
    }
    // Icon presence-badge toggles may have changed — repaint the list/grid icons.
    if (m_listModel) m_listModel->refreshIcons();
    // Re-sync the render blocklist from settings (e.g. after Settings ▸ Maintenance cleared it).
    m_renderBlocklist.clear();
    for (const QString& s : QSettings().value(QStringLiteral("models/renderBlocklist")).toStringList())
        m_renderBlocklist.insert(s.toInt());
    // Cloth-sim preference (default OFF for stability) may have changed.
    if (m_modelView)
        m_modelView->setClothEnabled(QSettings().value(QStringLiteral("models/clothSim"), false).toBool());
    // Developer mode may have been toggled → show/hide the dev-only viewport buttons
    // (Shaders / Detail maps / Rig / Physics), matching the Wardrobe preview.
    applyViewportDevGating();
}

// (Export-menu hooks — previewWidget / hasExportSelection / exportSelection / exportNoun /
//  hasAnimExport / animExportLabel / exportAnimations / exportSelectionToLast — and
//  showDependencies() moved to ModelsTab_Export.cpp)

// SNOs of the current selection, or — if the right-clicked row isn't selected —
// just that row. Used by the list context menus.
QList<int> ModelsTab::contextSnos(const QPoint& viewportPt) const
{
    QList<int> snos;
    const QModelIndex hit = m_list->indexAt(viewportPt);
    if (hit.isValid() && hit.parent().isValid())
        return snos;   // right-clicked a subtree node — its row number is NOT a browse row
    QModelIndexList sel = m_list->selectionModel()->selectedRows();
    // Same aliasing hazard for the selection: keep only top-level (browse) rows.
    sel.erase(std::remove_if(sel.begin(), sel.end(),
                             [](const QModelIndex& i) { return i.parent().isValid(); }),
              sel.end());
    bool hitInSel = false;
    for (const QModelIndex& idx : sel)
        if (idx.row() == hit.row()) { hitInSel = true; break; }
    if (hit.isValid() && !hitInSel) {
        if (const SnoEntry* e = m_listModel->entryAt(hit.row())) snos << e->snoId;
        return snos;
    }
    for (const QModelIndex& idx : sel)
        if (const SnoEntry* e = m_listModel->entryAt(idx.row())) snos << e->snoId;
    return snos;
}

// On startup: if a render-guard sentinel survived, the previous session crashed while
// rendering a model thumbnail. Blocklist the culprit, revert to Original icons, and log it,
// so the tab loads cleanly instead of crashing again on the same model.
void ModelsTab::recoverFromRenderCrash()
{
    QFile g(renderGuardPath());
    if (!g.exists()) return;
    QString content;
    if (g.open(QIODevice::ReadOnly)) { content = QString::fromUtf8(g.readAll()); g.close(); }
    QFile::remove(renderGuardPath());

    const int sno = content.section(QLatin1Char('\t'), 0, 0).toInt();
    const QString name = content.section(QLatin1Char('\t'), 1, 1);
    const QString stage = content.section(QLatin1Char('\t'), 2, 2);   // parse / upload / grab

    QStringList bl = QSettings().value(QStringLiteral("models/renderBlocklist")).toStringList();
    if (sno > 0 && !bl.contains(QString::number(sno))) {
        bl.append(QString::number(sno));
        QSettings().setValue(QStringLiteral("models/renderBlocklist"), bl);
    }
    QSettings().setValue(QStringLiteral("models/iconMode"), QStringLiteral("orig"));   // safe mode

    QFile log(renderCrashLogPath());
    if (log.open(QIODevice::Append | QIODevice::Text)) {
        log.write(QStringLiteral("%1  crashed at stage=%2 rendering sno=%3 name=%4 — blocklisted, icons reverted to Original\n")
                      .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                           stage.isEmpty() ? QStringLiteral("?") : stage)
                      .arg(sno).arg(name)
                      .toUtf8());
        log.close();
    }
    qWarning().noquote() << "ModelsTab: recovered from render crash — stage" << stage
                         << "sno" << sno << name << "→ blocklisted, icon mode reverted to Original";
    m_renderCrashSno = sno; m_renderCrashName = name;   // surfaced to the user after the UI is up
}

// Write the crash sentinel for the current render phase. flush()+close() forces it to disk so
// a hard crash (segfault) mid-render leaves the marker — with the STAGE — behind for next launch.
void ModelsTab::renderGuardStage(int sno, const QString& name, const char* stage)
{
    QFile f(renderGuardPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QStringLiteral("%1\t%2\t%3").arg(sno).arg(name, QString::fromLatin1(stage)).toUtf8());
        f.flush();
        f.close();
    }
}
void ModelsTab::endRenderGuard()
{
    QFile::remove(renderGuardPath());
}

// A hardware fault (access violation) was CAUGHT mid load/parse/render — the process
// is still alive thanks to the SEH→C++ guard. Quarantine the model so it isn't tried
// again, clear the on-disk guard (we've handled it, don't double-blocklist next launch),
// log it, and show a non-fatal hint in the viewport. No dialog spam — just recover.
void ModelsTab::handleModelFault(int sno, const QString& name, const QString& stage)
{
    // Persist + in-memory blocklist so this model never auto-loads/-renders again.
    if (sno > 0) {
        m_renderBlocklist.insert(sno);
        QStringList bl = QSettings().value(QStringLiteral("models/renderBlocklist")).toStringList();
        if (!bl.contains(QString::number(sno))) {
            bl.append(QString::number(sno));
            QSettings().setValue(QStringLiteral("models/renderBlocklist"), bl);
        }
    }
    // We recovered in-session → clear the guard/sentinel so next launch doesn't re-flag it.
    { QSettings s; s.setValue(QStringLiteral("models/loadGuard"), -1); s.sync(); }
    QFile::remove(renderGuardPath());

    QFile log(renderCrashLogPath());
    if (log.open(QIODevice::Append | QIODevice::Text)) {
        log.write(QStringLiteral("%1  CAUGHT fault (%2) loading sno=%3 name=%4 — quarantined in-session, tool kept running\n")
                      .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                           stage.isEmpty() ? QStringLiteral("?") : stage)
                      .arg(sno).arg(name)
                      .toUtf8());
        log.close();
    }
    qWarning().noquote() << "ModelsTab: caught model fault —" << stage
                         << "sno" << sno << name << "→ quarantined (tool survived)";

    updateIndexStatus();
    if (m_exportBtn) m_exportBtn->setEnabled(false);
    if (m_modelView) {
        m_modelView->clearGeometry();
        m_modelView->setOverlayText(
            QStringLiteral("⚠  This model couldn't be displayed and was skipped.\n"
                           "It's been added to the render blocklist so it won't crash the tool.\n"
                           "(Clear the blocklist in Settings ▸ Maintenance to retry.)"));
    }
    if (m_listModel) m_listModel->refreshRowForSno(sno);   // repaint the quarantined row (dim + ⚠)
}

void ModelsTab::renderIcons(const QList<int>& snos, bool force, bool quiet)
{
    if (!m_modelView || !m_listModel || !m_reader || !m_reader->isReady() || snos.isEmpty())
        return;
    // 'quiet' (auto in-view rendering) never runs a nested event loop, so the on-screen
    // preview never repaints mid-batch → no geometry-swap flicker while scrolling.
    if (!quiet) {
        if (m_iconModeCombo && m_iconModeCombo->currentData().toString() == QLatin1String("orig"))
            m_iconModeCombo->setCurrentIndex(2);   // show renders
        QApplication::setOverrideCursor(Qt::WaitCursor);
    }
    // Thumbnails are rendered by swapping geometry into m_modelView and grabbing an offscreen
    // FBO — but that mustn't disturb the LIVE preview. Freeze the viewport's on-screen repaint
    // for the batch and remember the exact camera, so the user neither sees icon models flash in
    // the main view nor has their orbit/zoom reset when the batch finishes. Also remember the
    // playing animation + frame and pause the timer, so swapping geometry doesn't clobber it.
    const GLModelWidget::CamState savedCam = m_modelView->cameraState();
    const bool viewWasUpdating = m_modelView->updatesEnabled();
    const bool animWasPlaying  = m_animTimer && m_animTimer->isActive();
    const int  savedAnimFrame  = m_animSlider ? m_animSlider->value() : 0;
    if (m_animTimer) m_animTimer->stop();   // don't advance frames onto swapped icon geometry
    m_modelView->setUpdatesEnabled(false);
    const int n = snos.size();
    int done = 0;
    for (int sno : snos) {
        ++done;
        if (!quiet && n > 1) {   // live percentage while rendering thumbnails
            setScan(QStringLiteral("render"), QStringLiteral("Rendering icons %1%").arg(done * 100 / n));
            QCoreApplication::processEvents();
        }
        if (!force && m_renderCache.contains(sno)) continue;
        if (m_renderBlocklist.contains(sno)) continue;   // known to crash the renderer — skip
        if (!force && m_noRenderSnos.contains(sno)) continue;   // already tried, yields nothing — don't loop on it
        const QByteArray meta = m_reader->readMetaBySno(quint64(sno));
        const QByteArray payload = m_reader->readPayloadBySno(quint64(sno));
        if (meta.isEmpty() || payload.isEmpty()) { m_noRenderSnos.insert(sno); continue; }   // nothing to render — don't retry
        // Guard the risky part in stages so a crash records WHERE it died (parse / GL upload /
        // grab). The sentinel survives a hard crash → next launch blocklists this model.
        const QString rname = (sno == m_curSno ? m_curName : QString());
        // Fault-protected: if parse/upload/draw hits an access violation the guard
        // catches it, quarantines this SNO, and rendering continues with the next row —
        // the icon batch never takes the whole tool down.
        seh::HardwareFault iconFault;
        bool produced = false;
        const bool iconOk = seh::runGuarded("icon", [&]() {
            renderGuardStage(sno, rname, "parse");
            const ModelGeometry geo = ModelParser::parseApp(meta, payload);
            if (geo.valid) {
                renderGuardStage(sno, rname, "upload");
                m_modelView->setGeometry(geo);
                renderGuardStage(sno, rname, "grab");
                const QImage thumb = m_modelView->grabThumbnail(48);
                if (!thumb.isNull()) {
                    m_renderCache.insert(sno, QPixmap::fromImage(thumb));
                    thumb.save(thumbCachePath(sno), "PNG");   // persist across sessions
                    produced = true;
                }
            }
        }, &iconFault);
        endRenderGuard();
        // Rendered but produced nothing (no geometry / null thumbnail): record it so auto-render
        // doesn't keep re-attempting the same stuck model every tick and freezing the tool.
        if (iconOk && !produced) m_noRenderSnos.insert(sno);
        if (!iconOk) {
            // Quarantine quietly — don't touch the viewport mid-batch (it's restored below).
            m_renderBlocklist.insert(sno);
            QStringList bl = QSettings().value(QStringLiteral("models/renderBlocklist")).toStringList();
            if (!bl.contains(QString::number(sno))) {
                bl.append(QString::number(sno));
                QSettings().setValue(QStringLiteral("models/renderBlocklist"), bl);
            }
            QFile log(renderCrashLogPath());
            if (log.open(QIODevice::Append | QIODevice::Text)) {
                log.write(QStringLiteral("%1  CAUGHT fault (%2) rendering icon sno=%3 — quarantined, batch continued\n")
                              .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                                   iconFault.what.isEmpty() ? QStringLiteral("icon") : iconFault.what)
                              .arg(sno).toUtf8());
                log.close();
            }
        }
    }
    // Restore the preview to EXACTLY what it was before the batch, so the last icon's geometry is
    // never left showing. If a model was loaded, put it back (keepView keeps the current camera,
    // which we then set to the saved one) and re-apply its animation clip/frame + resume playback.
    // If nothing was loaded, clear the viewport so no icon lingers.
    if (m_curGeo.valid) {
        m_modelView->setGeometry(m_curGeo, /*keepView=*/true);
        m_modelView->setCameraState(savedCam);
        if (m_curAnim.valid) {
            m_modelView->setAnimation(m_curAnim);
            m_modelView->setFrame(savedAnimFrame);
        }
    } else {
        m_modelView->clearGeometry();   // no model was displayed → don't leave the last icon showing
    }
    m_modelView->setUpdatesEnabled(viewWasUpdating);
    m_modelView->update();
    if (animWasPlaying && m_animTimer) m_animTimer->start();
    if (!quiet) QApplication::restoreOverrideCursor();
    m_listModel->refreshIcons();
    if (!quiet) updateIndexStatus();   // revert to build state (or hide)
}

void ModelsTab::copyIconImage(int sno)
{
    const QPixmap pm = listIconPixmap(sno);
    if (!pm.isNull()) QApplication::clipboard()->setImage(pm.toImage());
}

void ModelsTab::saveIconImages(const QList<int>& snos, bool chooseDir)
{
    if (snos.isEmpty()) return;
    QSettings s;
    auto entryName = [this](int sno) -> QString {
        for (int r = 0; r < m_listModel->rowCount(); ++r)
            if (const SnoEntry* e = m_listModel->entryAt(r))
                if (e->snoId == sno) return e->name;
        return QString::number(sno);
    };
    if (snos.size() == 1) {
        const QImage img = listIconPixmap(snos.first()).toImage();
        if (img.isNull()) {
            QMessageBox::warning(this, QStringLiteral("Save image"),
                                 QStringLiteral("No image for this row (render it first)."));
            return;
        }
        QString path;
        const QString fileName = entryName(snos.first()) + QStringLiteral(".png");
        if (chooseDir) {
            path = QFileDialog::getSaveFileName(this, QStringLiteral("Save image as"),
                QDir(s.value(QStringLiteral("models/lastImageDir")).toString()).filePath(fileName),
                QStringLiteral("PNG image (*.png);;JPEG image (*.jpg *.jpeg)"));
        } else {
            const QString dir = s.value(QStringLiteral("models/lastImageDir")).toString();
            if (dir.isEmpty()) { saveIconImages(snos, true); return; }   // no last dir → As…
            path = QDir(dir).filePath(fileName);
        }
        if (path.isEmpty()) return;
        if (img.save(path)) s.setValue(QStringLiteral("models/lastImageDir"), QFileInfo(path).absolutePath());
        else QMessageBox::warning(this, QStringLiteral("Save image"),
                                  QStringLiteral("Failed to save %1").arg(path));
        return;
    }
    // Multiple → pick / reuse a folder, save each as <name>.png.
    QString dir = chooseDir ? QString() : s.value(QStringLiteral("models/lastImageDir")).toString();
    if (dir.isEmpty())
        dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Save %1 images to…").arg(snos.size()));
    if (dir.isEmpty()) return;
    int ok = 0;
    for (int sno : snos) {
        const QImage img = listIconPixmap(sno).toImage();
        if (!img.isNull() && img.save(QDir(dir).filePath(entryName(sno) + QStringLiteral(".png")))) ++ok;
    }
    s.setValue(QStringLiteral("models/lastImageDir"), dir);
    QMessageBox::information(this, QStringLiteral("Save images"),
        QStringLiteral("Saved %1 of %2 image(s) to:\n%3").arg(ok).arg(snos.size()).arg(dir));
}

void ModelsTab::saveTileImage(int tile, bool chooseDir)
{
    if (tile < 0 || tile >= 6 || m_chanFull[tile].isNull()) return;
    QSettings s;
    const QString base = (m_curName.isEmpty() ? QStringLiteral("texture") : m_curName)
        + QStringLiteral("_") + m_chanCap[tile]->text() + QStringLiteral(".png");
    QString path;
    if (chooseDir) {
        path = QFileDialog::getSaveFileName(this, QStringLiteral("Save image as"),
            QDir(s.value(QStringLiteral("models/lastImageDir")).toString()).filePath(base),
            QStringLiteral("PNG image (*.png);;JPEG image (*.jpg *.jpeg)"));
    } else {
        const QString dir = s.value(QStringLiteral("models/lastImageDir")).toString();
        if (dir.isEmpty()) { saveTileImage(tile, true); return; }
        path = QDir(dir).filePath(base);
    }
    if (path.isEmpty()) return;
    if (m_chanFull[tile].save(path)) s.setValue(QStringLiteral("models/lastImageDir"), QFileInfo(path).absolutePath());
    else QMessageBox::warning(this, QStringLiteral("Save image"),
                              QStringLiteral("Failed to save %1").arg(path));
}

void ModelsTab::onMatTexSelected()
{
    const QModelIndex cur = m_matTex->currentIndex();
    if (!cur.isValid())
        return;
    const QString texName = m_matTexModel->item(cur.row(), 2)->text();          // NAME
    const int texSno = m_matTexModel->item(cur.row(), 1)->data(Qt::DisplayRole).toInt();  // SNO
    previewTexture(texName, texSno);
}

namespace {
// Greyscale view of one channel (0=R,1=G,2=B,3=A) of an RGBA image.
QImage channelGrey(const QImage& srcAny, int ch)
{
    if (srcAny.isNull())
        return {};
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
}  // namespace

void ModelsTab::previewTexture(const QString& texName, int texSno)
{
    // A single texture was picked in MATERIAL TEXTURES: show its channel split —
    // RGBA · R · G · B · A · (blank).
    static const char* const kTexCaps[6] = {"RGBA", "R", "G", "B", "A", ""};
    setTileCaptions(kTexCaps);
    clearTexturePreview();
    if (texName.isEmpty())
        return;
    // Texture facts under the tiles: dimensions, mip chain, raw format id (cheap meta re-read).
    if (m_texFacts) {
        m_texFacts->clear();
        QFile mf(QStringLiteral("%1/json/base/meta/Texture/%2.tex.json")
                     .arg(Config::d4dataDir(), texName));
        if (mf.open(QIODevice::ReadOnly)) {
            const TexMeta meta = parseTexMetaJson(mf.readAll());
            if (meta.valid) {
                // Human codec name via the tool's own table — it already formats as "BC7 (50)"
                // (or "Unknown (n)"), so no id juggling here. "format 43" meant nothing.
                const QString fmt = TexFormat::name(meta.eTexFormat);
                const QString facts = QStringLiteral("%1×%2 · mips %3–%4 · %5%6")
                    .arg(meta.width).arg(meta.height).arg(meta.mipMin).arg(meta.mipMax)
                    .arg(fmt)
                    .arg(meta.faceCount > 1 ? QStringLiteral(" · %1 faces").arg(meta.faceCount)
                                            : QString());
                m_texFacts->setText(facts);
                m_texFacts->setToolTip(facts);
            }
        }
    }
    const QImage img = decodeTexImage(texName, texSno);
    if (img.isNull())
        return;
    setChannelTile(0, img);
    setChannelTile(1, channelGrey(img, 0));
    setChannelTile(2, channelGrey(img, 1));
    setChannelTile(3, channelGrey(img, 2));
    setChannelTile(4, channelGrey(img, 3));
    // tile 5 stays blank
}

void ModelsTab::setTileCaptions(const char* const labels[6])
{
    for (int i = 0; i < 6; ++i) {
        if (!m_chanCap[i]) continue;
        const QString s = QString::fromLatin1(labels[i]);
        m_chanCap[i]->setText(s);
        m_chanCap[i]->setVisible(!s.isEmpty());
        m_chanCap[i]->adjustSize();
        if (m_chanImg[i])
            m_chanCap[i]->move(2, m_chanImg[i]->height() - m_chanCap[i]->height() - 2);
        m_chanCap[i]->raise();   // stay above the tile pixmap
    }
}

void ModelsTab::showEvent(QShowEvent* ev)
{
    BrowserTab::showEvent(ev);
    // Surface a render-crash recovery once, after the window exists.
    if (m_renderCrashSno > 0) {
        const int sno = m_renderCrashSno; const QString nm = m_renderCrashName;
        m_renderCrashSno = -1;   // one-shot
        QTimer::singleShot(0, this, [this, sno, nm] {
            QMessageBox::warning(this, QStringLiteral("Model render recovered"),
                QStringLiteral("The tool crashed last time while rendering a 3D icon for model "
                               "%1%2.\n\nThat model has been skipped (added to a render blocklist) and "
                               "icons were switched back to \"Original\". A note was written to "
                               "model_render_crashes.log in the app data folder.")
                    .arg(sno).arg(nm.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(nm)));
        });
    }
    // GL only initializes once the tab is first shown; render the in-view 3D icons then.
    scheduleVisibleIconRender();
}

bool ModelsTab::eventFilter(QObject* obj, QEvent* ev)
{
    const QEvent::Type t = ev->type();

    // A viewport popup panel opened/closed: mark its opener button active while open so it's obvious
    // which panel is showing, and (on close) clear the stuck :hover highlight — Qt::Popup panels grab
    // the mouse, so the button never gets a Leave event otherwise. The button is the "hoverBtn" property.
    if (t == QEvent::Show || t == QEvent::Hide) {
        const QVariant hb = obj->property("hoverBtn");
        if (hb.isValid()) {
            if (auto* b = qobject_cast<QWidget*>(hb.value<QObject*>())) {
                b->setProperty("panelOpen", t == QEvent::Show);
                b->style()->unpolish(b);
                b->style()->polish(b);   // re-evaluate the [panelOpen] style selector
                b->update();
                if (t == QEvent::Hide) {   // also drop the stuck hover highlight
                    b->setAttribute(Qt::WA_UnderMouse, false);
                    QEvent leave(QEvent::Leave);
                    QApplication::sendEvent(b, &leave);
                }
            }
        }
    }

    // ── Dye colour slots: drag a colour onto another slot, right-click to reset ──
    for (int r = 0; r < 4; ++r) {
        if (obj != m_dyeRegionBtn[r]) continue;
        const QString key = QStringLiteral("models/viewport/dyeColor%1").arg(r);
        if (t == QEvent::MouseButtonPress) {
            m_dyeDragStart = static_cast<QMouseEvent*>(ev)->pos();
        } else if (t == QEvent::MouseMove) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            if ((me->buttons() & Qt::LeftButton)
                && (me->pos() - m_dyeDragStart).manhattanLength() > QApplication::startDragDistance()) {
                const QColor c(QSettings().value(key, QStringLiteral("#ffffff")).toString());
                auto* mime = new QMimeData;
                mime->setColorData(c);
                auto* drag = new QDrag(m_dyeRegionBtn[r]);
                drag->setMimeData(mime);
                QPixmap pm(18, 18); pm.fill(c);
                drag->setPixmap(pm);
                drag->exec(Qt::CopyAction);
                return true;
            }
        } else if (t == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(ev);
            if (de->mimeData()->hasColor()) { de->acceptProposedAction(); return true; }
        } else if (t == QEvent::Drop) {
            auto* de = static_cast<QDropEvent*>(ev);
            const QColor c = qvariant_cast<QColor>(de->mimeData()->colorData());
            if (c.isValid()) { setDyeSlotColor(r, c); de->acceptProposedAction(); }
            return true;
        } else if (t == QEvent::ContextMenu) {
            QMenu menu(this);
            menu.addAction(QStringLiteral("Reset to white"), this,
                           [this, r]() { setDyeSlotColor(r, QColor(Qt::white)); });
            menu.exec(static_cast<QContextMenuEvent*>(ev)->globalPos());
            return true;
        }
        return BrowserTab::eventFilter(obj, ev);
    }

    // ── Memory swatches: store dropped colours; drag them out; right-click clears ──
    for (int i = 0; i < 8; ++i) {
        if (obj != m_dyeMem[i]) continue;
        const QString key = QStringLiteral("models/viewport/dyeMem%1").arg(i);
        auto setMem = [this, i, key](const QColor& c) {
            if (c.isValid()) {
                QSettings().setValue(key, c.name());
                m_dyeMem[i]->setStyleSheet(
                    QStringLiteral("QToolButton{background:%1;border:1px solid #555;}").arg(c.name()));
            } else {
                QSettings().remove(key);
                m_dyeMem[i]->setStyleSheet(
                    QStringLiteral("QToolButton{background:#2b2b2b;border:1px dashed #555;}"));
            }
        };
        if (t == QEvent::MouseButtonPress) {
            m_dyeDragStart = static_cast<QMouseEvent*>(ev)->pos();
        } else if (t == QEvent::MouseMove) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            const QString mc = QSettings().value(key).toString();
            if (!mc.isEmpty() && (me->buttons() & Qt::LeftButton)
                && (me->pos() - m_dyeDragStart).manhattanLength() > QApplication::startDragDistance()) {
                const QColor c(mc);
                auto* mime = new QMimeData; mime->setColorData(c);
                auto* drag = new QDrag(m_dyeMem[i]); drag->setMimeData(mime);
                QPixmap pm(18, 18); pm.fill(c); drag->setPixmap(pm);
                drag->exec(Qt::CopyAction);
                return true;
            }
        } else if (t == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(ev);
            if (de->mimeData()->hasColor()) { de->acceptProposedAction(); return true; }
        } else if (t == QEvent::Drop) {
            auto* de = static_cast<QDropEvent*>(ev);
            setMem(qvariant_cast<QColor>(de->mimeData()->colorData()));
            de->acceptProposedAction();
            return true;
        } else if (t == QEvent::ContextMenu) {
            QMenu menu(this);
            menu.addAction(QStringLiteral("Clear"), this, [setMem]() { setMem(QColor()); });
            menu.exec(static_cast<QContextMenuEvent*>(ev)->globalPos());
            return true;
        }
        return BrowserTab::eventFilter(obj, ev);
    }

    // ── Parts tree: hover-leave reverts to selection; empty-click / Esc deselect;
    //    plain-click an already-selected part toggles it off ──
    // (The PARTS pane's hover/deselect filters moved with it into the outliner — the list
    // viewport's Leave handler below restores the selection highlight.)

    // ── LOOKS table: hover-leave reverts to selection; reclick/empty/Esc deselect ──
    if (m_looksView && obj == m_looksView->viewport()) {
        if (t == QEvent::Leave) {
            const QModelIndex cur = m_looksView->currentIndex();
            highlightMaterialsForLook(cur.isValid() ? cur.row() : -1);
        } else if (t == QEvent::MouseButtonPress) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            const QModelIndex idx = m_looksView->indexAt(me->position().toPoint());
            const QModelIndex cur = m_looksView->currentIndex();
            const bool reclickSel = idx.isValid() && cur.isValid() && idx.row() == cur.row()
                                 && me->button() == Qt::LeftButton
                                 && me->modifiers() == Qt::NoModifier;
            if (!idx.isValid() || reclickSel) {
                m_looksView->clearSelection();
                m_looksView->setCurrentIndex(QModelIndex());   // → currentRowChanged(-1)
                highlightMaterialsForLook(-1);
                if (reclickSel) return true;   // consume so it doesn't re-select
            }
        }
        return BrowserTab::eventFilter(obj, ev);
    }
    if (m_looksView && obj == m_looksView && t == QEvent::KeyPress
        && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_Escape) {
        m_looksView->clearSelection();
        m_looksView->setCurrentIndex(QModelIndex());
        highlightMaterialsForLook(-1);
        return true;
    }

    // ── ANIMATIONS list: Esc stops + clears the playing animation. ──
    if (m_anims && obj == m_anims && t == QEvent::KeyPress
        && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_Escape) {
        clearAnimationSelection();
        return true;
    }
    // Arrow-key navigation auto-loads the highlighted clip so you can seek through animations
    // quickly. Handled on KeyRelease, after the list has moved the current row.
    if (m_anims && obj == m_anims && t == QEvent::KeyRelease) {
        const int key = static_cast<QKeyEvent*>(ev)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_PageUp
            || key == Qt::Key_PageDown || key == Qt::Key_Home || key == Qt::Key_End) {
            if (QListWidgetItem* cur = m_anims->currentItem()) {
                const QString name = cur->data(Qt::UserRole).toString();
                if (!name.isEmpty() && name != m_playingAnim) playAnimationByName(name);
            }
        }
    }

    // ── Right stack: keep the no-model veil covering the panel column ──
    // The veil is a sibling of the splitter (see ctor), so it tracks geometry(), not rect().
    if (m_rstack && obj == m_rstack && (t == QEvent::Resize || t == QEvent::Move) && m_rstackHint) {
        m_rstackHint->setGeometry(m_rstack->geometry());
        m_rstackHint->raise();
    }

    // ── Timeline slider: wheel = ONE frame per notch (precise scrubbing — the stock QSlider
    // wheel jumps by wheelScrollLines), Shift+wheel = playback speed up/down. ──
    if (m_animSlider && obj == m_animSlider && t == QEvent::Wheel) {
        const auto* we = static_cast<QWheelEvent*>(ev);
        const int dir = we->angleDelta().y() > 0 ? 1 : -1;
        if ((we->modifiers() & Qt::ShiftModifier) && m_speedCombo) {
            const int i = qBound(0, m_speedCombo->currentIndex() + dir, m_speedCombo->count() - 1);
            m_speedCombo->setCurrentIndex(i);   // → applyAnimSpeed
            showToast(QStringLiteral("Speed: %1").arg(m_speedCombo->currentText()));
        } else {
            m_animSlider->setValue(m_animSlider->value() + dir);
        }
        return true;   // consume: never let the stock multi-line jump fight the scrub
    }

    // ── Shading ⌄: wheel cycles the view channel in place (no popup, live preview) ──
    if (m_shadeMoreBtn && obj == m_shadeMoreBtn && t == QEvent::Wheel && m_channelCombo) {
        const auto* we = static_cast<QWheelEvent*>(ev);
        const int n = m_channelCombo->count();
        if (n > 0) {
            const int dir = we->angleDelta().y() > 0 ? -1 : 1;   // wheel-up = previous
            m_channelCombo->setCurrentIndex((m_channelCombo->currentIndex() + dir + n) % n);
            // Name it where the eyes are: mid-scroll you're watching the model, not the button.
            showToast(QStringLiteral("Channel: %1").arg(m_channelCombo->currentText()));
        }
        return true;   // consume: never scroll the toolbar under us
    }

    // ── Smart search: the history dropdown follows focus, Esc, and ↓/↑ navigation ──
    if (m_hdrSearch && obj == m_hdrSearch) {
        if (t == QEvent::FocusIn) {
            refreshHistPopup();   // child widget — showing it never grabs or eats clicks
        } else if (t == QEvent::FocusOut) {
            if (m_histList) m_histList->hide();
        } else if (t == QEvent::KeyPress) {
            const auto* ke = static_cast<QKeyEvent*>(ev);
            if (ke->key() == Qt::Key_Escape) {
                if (m_histList && m_histList->isVisible()) m_histList->hide();
                else                                       m_hdrSearch->clear();
                return true;
            }
            if ((ke->key() == Qt::Key_Down || ke->key() == Qt::Key_Up)
                && m_histList) {
                if (!m_histList->isVisible()) { refreshHistPopup(); return true; }
                const int n = m_histList->count();
                if (n > 0) {
                    const int cur = m_histList->currentRow();
                    m_histList->setCurrentRow(
                        ke->key() == Qt::Key_Down ? (cur + 1) % n : (cur - 1 + n) % n);
                }
                return true;
            }
        }
    }

    // Alt+H is also the menubar's &Help mnemonic, and mnemonics run through the shortcut system
    // BEFORE widget key events — so it opened the Help menu and our handler never saw the key.
    // Accepting the ShortcutOverride while the outliner has focus claims the key back; the
    // KeyPress then arrives below like any other.
    if ((obj == m_list || obj == m_modelView) && t == QEvent::ShortcutOverride
        && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_H) {
        ev->accept();
        return true;
    }

    // ── Part-visibility hotkeys — H hide · Shift+H hide others · Alt+H show all.
    // Accepted from the OUTLINER *or* the VIEWPORT: hiding parts is something you do while
    // looking at the model, so requiring list focus (the old behaviour) was backwards.
    if ((obj == m_list || obj == m_modelView) && t == QEvent::KeyPress && m_treeModel) {
        const auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_H) {
            QHash<int, bool> all;
            m_treeModel->partChecks(all);
            const QList<int> sel = selectedParts();
            if (ke->modifiers() & Qt::AltModifier) {          // Alt+H — unhide everything
                int wasHidden = 0;
                for (auto it2 = all.constBegin(); it2 != all.constEnd(); ++it2) {
                    if (!it2.value()) ++wasHidden;
                    m_treeModel->setPartCheck(it2.key(), true);
                }
                showToast(wasHidden ? QStringLiteral("Showed %1 hidden part%2").arg(wasHidden)
                                          .arg(wasHidden == 1 ? QString() : QStringLiteral("s"))
                                    : QStringLiteral("Nothing was hidden"));
            } else if (!sel.isEmpty() && (ke->modifiers() & Qt::ShiftModifier)) {
                for (auto it2 = all.constBegin(); it2 != all.constEnd(); ++it2)
                    m_treeModel->setPartCheck(it2.key(), sel.contains(it2.key()));   // solo
                showToast(QStringLiteral("Soloed %1 part%2 — Alt+H to show all")
                              .arg(sel.size()).arg(sel.size() == 1 ? QString() : QStringLiteral("s")));
            } else if (!sel.isEmpty()) {
                for (int prim : sel) m_treeModel->setPartCheck(prim, false);         // hide
                showToast(QStringLiteral("Hid %1 part%2 — Alt+H to show all")
                              .arg(sel.size()).arg(sel.size() == 1 ? QString() : QStringLiteral("s")));
            } else {
                return BrowserTab::eventFilter(obj, ev);      // H with nothing selected: ignore
            }
            recomputePartVisibility();
            return true;
        }
    }

    // ── Grid viewport: hover preview (the whole cell IS the icon, so the image is included),
    // Ctrl+scroll resizes the thumbnails, and a plain scroll over a live popup resizes it. ──
    if (m_gridView && obj == m_gridView->viewport()) {
        if (t == QEvent::MouseMove) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            const QModelIndex idx = m_gridView->indexAt(me->position().toPoint());
            int sno = -1;
            if (idx.isValid() && m_listModel)
                if (const SnoEntry* e = m_listModel->entryAt(idx.row())) sno = e->snoId;
            if (sno != m_hoverSno || !m_hoverIconArea) {
                m_hoverSno = sno;
                m_hoverTexSno = -1; m_hoverTile = -1;
                m_hoverIconArea = true;      // grid cells are icons — show the image
                hideIconPreview();
                if (sno >= 0) m_hoverTimer->start(HoverInfo::delayMs()); else m_hoverTimer->stop();
            }
        } else if (t == QEvent::Leave) {
            m_hoverTimer->stop(); m_hoverSno = -1; hideIconPreview();
        } else if (t == QEvent::Wheel) {
            const auto* we = static_cast<QWheelEvent*>(ev);
            if (we->modifiers() & Qt::ControlModifier) {
                setGridThumbPx(m_gridPx + (we->angleDelta().y() > 0 ? 12 : -12));
                return true;   // consume: Ctrl+scroll resizes grid thumbnails
            }
            if (m_iconPreview && m_iconPreview->isVisible() && m_hoverSno >= 0
                && HoverInfo::scrollZoom()) {
                m_previewPx = qBound(64, m_previewPx + (we->angleDelta().y() > 0 ? 24 : -24), 1024);
                showIconPreview(m_hoverSno);
                return true;   // consume: scroll resizes the popup
            }
        }
    }

    // ── Model list viewport: hover preview + ctrl/preview wheel resize ──
    if (m_list && obj == m_list->viewport()) {
        // ── Drag-out export: dragging an ALREADY-SELECTED model row hands Explorer/Blender a
        // real .glb (exported to temp at drag start). Pressing an unselected row still starts
        // the normal rubber-band selection — the standard file-manager split. ──
        if (t == QEvent::MouseButtonPress) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            m_dragPrimed = false;
            if (me->button() == Qt::LeftButton && me->modifiers() == Qt::NoModifier) {
                const QModelIndex ix = m_list->indexAt(me->position().toPoint());
                if (ix.isValid() && !ix.parent().isValid()
                    && m_list->selectionModel()->isSelected(ix.siblingAtColumn(0))) {
                    m_dragPrimed = true;
                    m_dragPressPos = me->position().toPoint();
                }
            }
        } else if (t == QEvent::MouseButtonRelease) {
            m_dragPrimed = false;
        } else if (t == QEvent::MouseMove && m_dragPrimed) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            if ((me->buttons() & Qt::LeftButton)
                && (me->position().toPoint() - m_dragPressPos).manhattanLength()
                       >= QApplication::startDragDistance()) {
                m_dragPrimed = false;
                startModelDrag();
                return true;   // the drag loop owns the mouse from here
            }
        }
        if (t == QEvent::MouseMove) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            const QModelIndex idx = m_list->indexAt(me->position().toPoint());
            int sno = -1;
            // ANY column on a top-level row hovers (the old code required column 1 — the Icon
            // column — so List display mode, which hides that column entirely, never hovered at
            // all). The icon image itself is only included when the cursor is actually over the
            // Icon column; elsewhere the popup is info-only.
            if (idx.isValid() && !idx.parent().isValid())
                if (const SnoEntry* e = m_listModel->entryAt(idx.row())) sno = e->snoId;
            const bool overIcon = idx.isValid() && idx.column() == 1 && !m_list->isColumnHidden(1);
            // Outliner texture nodes → same hover-preview machinery, texture flavor.
            //   Texture leaf: only when the cursor is on its THUMBNAIL (not the whole row).
            //   Tile strips: the tile under the cursor (channel previews for TexTiles).
            QString texName; int texSno = -1; int chan = -1;
            if (idx.isValid() && idx.parent().isValid() && m_treeModel) {
                const auto* n = m_treeModel->node(idx);
                const QPoint p = me->position().toPoint();
                const QRect vr = m_list->visualRect(idx.siblingAtColumn(ModelOutlinerModel::kTreeCol));
                if (n && n->kind == ModelOutlinerModel::Texture && !n->aux.isEmpty()) {
                    if (p.x() >= vr.left() && p.x() <= vr.left() + 24) {   // the thumbnail only
                        texName = n->aux;
                        texSno = int(n->hash);
                    }
                } else if (n && (n->kind == ModelOutlinerModel::MatTiles
                                 || n->kind == ModelOutlinerModel::TexTiles)
                           && !n->tiles.isEmpty()) {
                    constexpr int step = ModelOutlinerModel::kTilePx + ModelOutlinerModel::kTileGap;
                    const int t = (p.x() - vr.left()) / step;
                    if (p.x() >= vr.left() && t >= 0 && t < n->tiles.size()
                        && t < n->tilesDone) {   // only tiles that have decoded
                        texName = n->tiles[t].first;
                        texSno = n->tiles[t].second;
                        if (n->kind == ModelOutlinerModel::TexTiles && t > 0)
                            chan = t - 1;   // tile 0 = full RGB, 1..4 = R/G/B/A
                    }
                }
            }
            m_hoverTile = -1;   // hovering the list, not a texture tile
            if (sno != m_hoverSno || texSno != m_hoverTexSno || chan != m_hoverChan
                || overIcon != m_hoverIconArea) {
                m_hoverSno = sno;
                m_hoverTexSno = texSno;
                m_hoverTexName = texName;
                m_hoverChan = chan;
                m_hoverIconArea = overIcon;
                hideIconPreview();
                if (sno >= 0 || texSno > 0) m_hoverTimer->start(HoverInfo::delayMs()); else m_hoverTimer->stop();
            }
        } else if (t == QEvent::Leave) {
            m_hoverTimer->stop(); m_hoverSno = -1; m_hoverTexSno = -1; hideIconPreview();
            // Hover-highlight of subtree part nodes ends here — fall back to the selection.
            const QList<int> sel = selectedParts();
            if (m_modelView) m_modelView->setHighlightParts(sel);
            highlightMaterialsForParts(sel);
        } else if (t == QEvent::Wheel) {
            const auto* we = static_cast<QWheelEvent*>(ev);
            const int dir = we->angleDelta().y() > 0 ? 1 : -1;
            if (m_iconPreview && m_iconPreview->isVisible() && HoverInfo::scrollZoom()) {
                m_previewPx = qBound(64, m_previewPx + dir * 24, 1024);
                if (m_hoverTexSno > 0) showOutlinerTexPreview();   // resize whichever flavor is up
                else                   showIconPreview(m_hoverSno);
                return true;   // consume: scroll resizes the preview
            }
            if ((we->modifiers() & Qt::ControlModifier) && m_displayMode == 1) {
                setListIconSize(m_iconPx + dir * 6);   // Outliner only — List stays fixed-compact
                return true;   // consume: ctrl+scroll resizes Outliner icons
            }
        }
        return BrowserTab::eventFilter(obj, ev);
    }

    // ── Texture-preview tiles: caption hide + 0.5s zoom preview + wheel resize ──
    for (int i = 0; i < 6; ++i) {
        if (obj != m_chanImg[i]) continue;
        if (t == QEvent::Enter) {
            if (m_chanCap[i]) m_chanCap[i]->hide();
            m_hoverTile = i; m_hoverSno = -1;
            if (!m_chanFull[i].isNull()) m_hoverTimer->start(HoverInfo::delayMs());
        } else if (t == QEvent::Leave) {
            if (m_chanCap[i] && !m_chanCap[i]->text().isEmpty()) m_chanCap[i]->show();
            m_hoverTile = -1; m_hoverTimer->stop(); hideIconPreview();
        } else if (t == QEvent::Wheel && m_iconPreview && m_iconPreview->isVisible()) {
            const auto* we = static_cast<QWheelEvent*>(ev);
            m_previewPx = qBound(64, m_previewPx + (we->angleDelta().y() > 0 ? 24 : -24), 512);
            showTilePreview(i);
            return true;   // consume: scroll resizes the preview
        }
        return BrowserTab::eventFilter(obj, ev);
    }

    // (The FILE INFO viewport overlay was removed — the INFO right-panel page replaces it.)
    // Keep the viewport's floating children pinned to its right edge on resize.
    if (t == QEvent::Resize && obj == m_modelView) {
        if (m_vpStrip) {
            m_vpStrip->adjustSize();
            m_vpStrip->move(m_modelView->width() - m_vpStrip->width() - 6, 104);
            m_vpStrip->raise();
        }
        if (m_fsExitBtn && m_fsExitBtn->isVisible()) {   // maximized: keep Exit reachable
            m_fsExitBtn->move(m_modelView->width() - m_fsExitBtn->width() - 8, 8);
            m_fsExitBtn->raise();
        }
    }
    return BrowserTab::eventFilter(obj, ev);
}

// Decode a BC texture payload to an RGBA8888 QImage via the CPU decoder (the same
// path the .glb exporter uses to embed textures). Null QImage on any miss.
// Guard against base-colour textures whose alpha channel isn't a real cutout mask
// (some are all-zero / data). If nothing is sufficiently opaque, force alpha opaque
// so alpha-test doesn't erase the whole part; otherwise keep it for cutout cloth.
static QImage normalizeAlpha(const QImage& src)
{
    if (src.isNull()) return src;
    QImage img = src.convertToFormat(QImage::Format_RGBA8888);
    const int n = img.width() * img.height();
    const uchar* p = img.constBits();
    int maxA = 0;
    for (int i = 0; i < n && maxA < 200; ++i) maxA = qMax(maxA, int(p[i * 4 + 3]));
    if (maxA >= 200) return img;   // genuine alpha → keep for cutout
    uchar* d = img.bits();
    for (int i = 0; i < n; ++i) d[i * 4 + 3] = 255;
    return img;
}

QImage ModelsTab::baseColorForMaterial(const QString& matName)
{
    if (matName.isEmpty()) return {};
    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty()) return {};
    QFile f(QStringLiteral("%1/json/base/meta/Material/%2.mat.json").arg(d4, matName));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QVector<MatTexture> texs = parseMaterialJson(f.readAll());
    // Pick the primary (non-effect) BASE_COLOR, plus a separate opacity map if present
    // (hair/cloth keep transparency in a dedicated *_Alpha / opacity texture).
    QString baseTex; int baseSno = 0; bool baseNonEffect = false;
    QString opTex; int opSno = 0;
    for (const MatTexture& t : texs) {
        if (t.texName.isEmpty()) continue;
        if (t.role == QLatin1String("BASE_COLOR")) {
            const bool effect = t.texName.contains(QLatin1String("tileable"), Qt::CaseInsensitive)
                             || t.texName.contains(QLatin1String("energyColor"), Qt::CaseInsensitive)
                             || t.texName.contains(QLatin1String("magic"), Qt::CaseInsensitive);
            if (baseTex.isEmpty()) { baseTex = t.texName; baseSno = t.texSno; }
            if (!effect && !baseNonEffect) { baseTex = t.texName; baseSno = t.texSno; baseNonEffect = true; }
        }
        if (opTex.isEmpty()
            && (t.role == QLatin1String("ALPHA") || t.role == QLatin1String("OPACITY")
                || t.texName.contains(QLatin1String("_Alpha"), Qt::CaseInsensitive)
                || t.texName.contains(QLatin1String("opacity"), Qt::CaseInsensitive)))
            { opTex = t.texName; opSno = t.texSno; }
    }

    // The "black" placeholder texture (armor_skin_mat etc., whose skin is filled by
    // the dye system at runtime) → guaranteed flat black, not a decoded/borrowed map.
    if (baseTex.compare(QLatin1String("black"), Qt::CaseInsensitive) == 0) {
        QImage blk(4, 4, QImage::Format_RGBA8888); blk.fill(Qt::black); return blk;
    }
    QImage base = baseTex.isEmpty() ? QImage() : decodeTexImage(baseTex, baseSno);
    const QImage op = opTex.isEmpty() ? QImage() : decodeTexImage(opTex, opSno);
    if (op.isNull())
        return base.isNull() ? QImage() : normalizeAlpha(base);

    // Composite the opacity map into the (or a neutral) base colour's alpha channel.
    QImage out;
    if (base.isNull()) { out = QImage(op.size(), QImage::Format_RGBA8888); out.fill(QColor(160, 150, 140)); }
    else                 out = base.convertToFormat(QImage::Format_RGBA8888);
    const QImage a = op.convertToFormat(QImage::Format_RGBA8888)
                       .scaled(out.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for (int y = 0; y < out.height(); ++y) {
        uchar* d = out.scanLine(y);
        const uchar* s = a.constScanLine(y);
        for (int x = 0; x < out.width(); ++x)
            d[x * 4 + 3] = s[x * 4 + 0];   // opacity from the map's red/luma
    }
    return out;
}

QImage ModelsTab::ormForMaterial(const QString& matName)
{
    if (matName.isEmpty()) return {};
    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty()) return {};
    QFile f(QStringLiteral("%1/json/base/meta/Material/%2.mat.json").arg(d4, matName));
    if (!f.open(QIODevice::ReadOnly)) return {};
    QString rN, mN, aN; int rS = 0, mS = 0, aS = 0;
    for (const MatTexture& t : parseMaterialJson(f.readAll())) {
        if (t.texName.isEmpty()) continue;
        if (rN.isEmpty() && t.role == QLatin1String("ROUGHNESS")) { rN = t.texName; rS = t.texSno; }
        if (mN.isEmpty() && t.role == QLatin1String("METALLIC")) { mN = t.texName; mS = t.texSno; }
        if (aN.isEmpty() && t.role == QLatin1String("AO"))       { aN = t.texName; aS = t.texSno; }
    }
    const QImage rough = rN.isEmpty() ? QImage() : decodeTexImage(rN, rS);
    const QImage metal = mN.isEmpty() ? QImage() : decodeTexImage(mN, mS);
    const QImage ao    = aN.isEmpty() ? QImage() : decodeTexImage(aN, aS);
    if (rough.isNull() && metal.isNull() && ao.isNull()) return {};
    int w = 1, h = 1;
    for (const QImage* im : {&rough, &metal, &ao})
        if (!im->isNull()) { w = qMax(w, im->width()); h = qMax(h, im->height()); }
    auto chan = [&](const QImage& im, int def) -> QImage {
        QImage out(w, h, QImage::Format_RGBA8888);
        if (im.isNull()) { out.fill(QColor(def, def, def)); return out; }
        return im.convertToFormat(QImage::Format_RGBA8888)
                 .scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    };
    const QImage R = chan(ao, 255), G = chan(rough, 153), B = chan(metal, 0);   // ao1 / rough0.6 / metal0
    QImage orm(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) {
        uchar* d = orm.scanLine(y);
        const uchar* pr = R.constScanLine(y);
        const uchar* pg = G.constScanLine(y);
        const uchar* pb = B.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            d[x * 4 + 0] = pr[x * 4]; d[x * 4 + 1] = pg[x * 4];
            d[x * 4 + 2] = pb[x * 4]; d[x * 4 + 3] = 255;
        }
    }
    return orm;
}

QImage ModelsTab::normalForMaterial(const QString& matName)
{
    if (matName.isEmpty()) return {};
    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty()) return {};
    QFile f(QStringLiteral("%1/json/base/meta/Material/%2.mat.json").arg(d4, matName));
    if (!f.open(QIODevice::ReadOnly)) return {};
    for (const MatTexture& t : parseMaterialJson(f.readAll()))
        if (t.role == QLatin1String("NORMAL") && !t.texName.isEmpty())
            return decodeTexImage(t.texName, t.texSno);
    return {};
}

QImage ModelsTab::emissiveForMaterial(const QString& matName)
{
    return textureByRole(matName, "EMISSIVE");
}

// First decoded texture bound to a given shader role (e.g. DETAIL_NORMAL).
QImage ModelsTab::textureByRole(const QString& matName, const char* role)
{
    if (matName.isEmpty()) return {};
    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty()) return {};
    QFile f(QStringLiteral("%1/json/base/meta/Material/%2.mat.json").arg(d4, matName));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QLatin1String want(role);
    for (const MatTexture& t : parseMaterialJson(f.readAll()))
        if (t.role == want && !t.texName.isEmpty())
            return decodeTexImage(t.texName, t.texSno);
    return {};
}

bool ModelsTab::materialHasRole(const QString& matName, const char* role)
{
    if (matName.isEmpty()) return false;
    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty()) return false;
    QFile f(QStringLiteral("%1/json/base/meta/Material/%2.mat.json").arg(d4, matName));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QLatin1String want(role);
    for (const MatTexture& t : parseMaterialJson(f.readAll()))
        if (t.role == want) return true;
    return false;
}

void ModelsTab::togglePreviewPanel()
{
    if (m_vpPanel && m_vpPanel->isVisible()) { m_vpPanel->hide(); return; }
    if (!m_vpPanel) buildPreviewPanel();
    m_vpPanel->adjustSize();
    m_vpPanel->move(panelPosLeftOf(m_vpBtn, m_vpPanel->sizeHint()));
    m_vpPanel->show();
    m_vpPanel->raise();
}

// Push the saved Rig-popup flags to the view (Skeleton stays the toolbar toggle, restored separately).
// ── Shared row context-menu builders ──────────────────────────────────────────────────
// The list and the thumbnail grid are two views of the SAME model + selection, so they must
// offer the same actions. They were built independently and drifted: the grid had 4 entries
// against the list's full set. Both now compose from these two builders.
void ModelsTab::addRowImageActions(QMenu& menu, const QList<int>& snos)
{
    if (snos.isEmpty()) return;
    const int n = snos.size();
    const QString sfx = n > 1 ? QStringLiteral("s") : QString();
    menu.addAction(QStringLiteral("Copy image"), this, [this, snos]() {
        copyIconImage(snos.first());
    });
    menu.addAction(QStringLiteral("Save image%1").arg(sfx), this, [this, snos]() {
        saveIconImages(snos, false);
    });
    menu.addAction(QStringLiteral("Save image%1 as…").arg(sfx), this, [this, snos]() {
        saveIconImages(snos, true);
    });
    menu.addSeparator();
    menu.addAction(n > 1 ? QStringLiteral("Render %1 icons").arg(n)
                         : QStringLiteral("Render icon"),
                   this, [this, snos]() { renderIcons(snos, true); });
    // Force-render a blocklisted model (user override) or clear the blocklist entirely.
    bool anyBlocked = false;
    for (int s : snos) if (m_renderBlocklist.contains(s)) { anyBlocked = true; break; }
    if (anyBlocked) {
        menu.addAction(QStringLiteral("Un-block && render (may crash)"), this,
            [this, snos]() {
                QStringList bl = QSettings().value(QStringLiteral("models/renderBlocklist")).toStringList();
                for (int s : snos) { m_renderBlocklist.remove(s); bl.removeAll(QString::number(s)); }
                QSettings().setValue(QStringLiteral("models/renderBlocklist"), bl);
                for (int s : snos) if (m_listModel) m_listModel->refreshRowForSno(s);   // clear dim/⚠
                renderIcons(snos, true);
            });
    }
    if (!m_renderBlocklist.isEmpty())
        menu.addAction(QStringLiteral("Clear render blocklist (%1)").arg(m_renderBlocklist.size()),
            this, [this]() {
                m_renderBlocklist.clear();
                QSettings().remove(QStringLiteral("models/renderBlocklist"));
                if (m_list) m_list->viewport()->update();          // drop dim/⚠ on cleared rows
                if (m_gridView) m_gridView->viewport()->update();
            });
}

void ModelsTab::addRowExportCopyActions(QMenu& menu, const QList<int>& snos)
{
    if (snos.isEmpty()) return;
    const int n = snos.size();
    const QString sfx = n > 1 ? QStringLiteral("s") : QString();
    // ── Other columns: export + copy (standardized order across tabs) ──
    QStringList snoStrs, filenames, names, colls;
    QVector<QPair<int, QString>> models;
    AppearanceMeta& am = AppearanceMeta::instance();
    for (int sno : snos)
        for (int r = 0; r < m_listModel->rowCount(); ++r)
            if (const SnoEntry* e = m_listModel->entryAt(r))
                if (e->snoId == sno) {
                    snoStrs << QString::number(sno);
                    filenames << e->name;
                    const QString t = am.titleFor(sno);
                    names << (t.isEmpty() ? e->name : t);
                    colls << am.collectionFor(sno);
                    models.append({sno, e->name});
                    break;
                }
    auto copyList = [](const QStringList& l) { QApplication::clipboard()->setText(l.join(QLatin1Char('\n'))); };
    auto prev = [](const QString& s) { return s.size() > 30 ? s.left(29) + QChar(0x2026) : s; };
    // Export queue count (this menu acts on the whole selection).
    const QString exCount = QStringLiteral("%1 model%2").arg(n).arg(n == 1 ? QString() : QStringLiteral("s"));
    menu.addAction(QStringLiteral("Export (last dir)  —  %1").arg(exCount), this, [this, models]() {
        const QString dir = QSettings().value(QStringLiteral("models/lastExportDir")).toString();
        if (dir.isEmpty()) {
            const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Export to…"));
            if (!d.isEmpty()) exportModels(models, d);
        } else {
            exportModels(models, dir);
        }
    });
    menu.addAction(QStringLiteral("Export to…  —  %1").arg(exCount), this, [this, models]() {
        const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Export to…"));
        if (!d.isEmpty()) exportModels(models, d);
    });
    menu.addSeparator();
    if (n == 1) {
        menu.addAction(QStringLiteral("Copy SNO id  (%1)").arg(snoStrs.first()), this, [snoStrs, copyList]() { copyList(snoStrs); });
        menu.addAction(QStringLiteral("Copy file name  (%1)").arg(prev(filenames.first())), this, [filenames, copyList]() { copyList(filenames); });
        menu.addAction(QStringLiteral("Copy name  (%1)").arg(prev(names.first())), this, [names, copyList]() { copyList(names); });
        QAction* aC = menu.addAction(QStringLiteral("Copy collection name  (%1)").arg(prev(colls.first().isEmpty() ? QStringLiteral("—") : colls.first())), this, [colls, copyList]() { copyList(colls); });
        aC->setEnabled(!colls.first().isEmpty());
    } else {
        menu.addAction(QStringLiteral("Copy %1 SNO ids").arg(n), this, [snoStrs, copyList]() { copyList(snoStrs); });
        menu.addAction(QStringLiteral("Copy %1 file names").arg(n), this, [filenames, copyList]() { copyList(filenames); });
        menu.addAction(QStringLiteral("Copy %1 names").arg(n), this, [names, copyList]() { copyList(names); });
        menu.addAction(QStringLiteral("Copy %1 collection names").arg(n), this, [colls, copyList]() { copyList(colls); });
    }
    if (models.size() == 1) {
        menu.addSeparator();
        // Variants — skin siblings on the same actor (game-data driven); picking one jumps
        // to and loads it. Lives here now instead of the old "Variants ▾" toolbar button.
        const QList<int> vars = m_apprVariantSnos.value(models.first().first);
        if (!vars.isEmpty()) {
            QMenu* vm = menu.addMenu(QStringLiteral("Variants (%1)").arg(vars.size()));
            for (int vs : vars)
                vm->addAction(m_apprName.value(vs, QStringLiteral("appearance %1").arg(vs)),
                              this, [this, vs]() { selectModelBySno(vs); });
        }
        menu.addAction(QStringLiteral("Show dependencies…"), this, [this, models]() {
            showDependencies(models.first().first, models.first().second);
        });
    }
}

void ModelsTab::applyModelRig()
{
    if (!m_modelView) return;
    QSettings s;
    // MASTER GATE. This runs on model load and on every rig/physics edit, so replaying the saved
    // flags ungated switched overlays back on behind the master toggle. Matches StableTab2.
    const bool ovOn = m_overlaysOn;
    m_modelView->setShowColliders(ovOn && s.value(QStringLiteral("models/rig/colliders"), false).toBool());
    m_modelView->setShowPhysBones(ovOn && s.value(QStringLiteral("models/rig/physBones"), false).toBool());
    m_modelView->setShowPhysAxes(ovOn && s.value(QStringLiteral("models/rig/axis"), true).toBool());
    m_modelView->setShowBoneNames(ovOn && s.value(QStringLiteral("models/rig/boneNames"), false).toBool());
    m_modelView->setBoneNamesTranslated(s.value(QStringLiteral("models/rig/boneNamesTranslated"), false).toBool());
    m_modelView->setBoneNamesHideUnknown(s.value(QStringLiteral("models/rig/boneNamesHideUnknown"), false).toBool());
}

// (The Rig popup is gone — every rig/bone toggle lives in the Overlays ▾ panel, and its old
//  settings keys are applied by applyModelRig above.)

// Persistent popup (Qt::Popup → closes on Esc / outside-click, stays open on inner
// clicks). Holds the shading toggles, exposure, and the in-panel dye colour picker.
void ModelsTab::buildPreviewPanel()
{
    if (m_vpPanel) return;
    QSettings s;
    m_vpPanel = new QFrame(this, Qt::Popup);
    m_vpPanel->setObjectName(QStringLiteral("vpPanel"));
    m_vpPanel->setProperty("hoverBtn", QVariant::fromValue<QObject*>(m_vpBtn));
    m_vpPanel->installEventFilter(this);
    m_vpPanel->setStyleSheet(QStringLiteral(
        "QFrame#vpPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
        "QLabel{color:#cccccc;} QCheckBox{color:#cccccc;}"));
    auto* pl = new QVBoxLayout(m_vpPanel);
    pl->setContentsMargins(12, 10, 12, 10);
    pl->setSpacing(5);
    auto* hdr = new QLabel(QStringLiteral("Graphics"), m_vpPanel);
    hdr->setStyleSheet(QLatin1String(kHdrQss));
    pl->addWidget(hdr);

    // Grouped layout matching the Wardrobe Graphics panel: Scene & shadows · Shading · Geometry debug.
    auto addChkTo = [&](QVBoxLayout* into, const QString& key, const QString& label, bool def,
                        std::function<void(bool)> apply) {
        auto* cb = new QCheckBox(label, m_vpPanel);
        cb->setChecked(s.value(QStringLiteral("models/viewport/") + key, def).toBool());
        connect(cb, &QCheckBox::toggled, this, [this, key, apply](bool on) {
            QSettings().setValue(QStringLiteral("models/viewport/") + key, on);
            apply(on);
        });
        into->addWidget(cb);
    };
    auto addGroup = [&](const QString& title) -> QVBoxLayout* {
        auto* box = new QGroupBox(title, m_vpPanel);
        auto* gl  = new QVBoxLayout(box);
        gl->setContentsMargins(8, 4, 8, 4);
        pl->addWidget(box);
        return gl;
    };

    auto* gLight = addGroup(QStringLiteral("Scene & shadows"));
    addChkTo(gLight, QStringLiteral("ibl"), QStringLiteral("Environment lighting (IBL)"), true,
             [this](bool on) { if (m_modelView) m_modelView->setFeatureIbl(on); });
    addChkTo(gLight, QStringLiteral("shadows"), QStringLiteral("Self-shadows"), true,
             [this](bool on) { if (m_modelView) m_modelView->setShadowEnabled(on); });
    addChkTo(gLight, QStringLiteral("ssao"), QStringLiteral("Ambient occlusion (SSAO)"), true,
             [this](bool on) { if (m_modelView) m_modelView->setSsaoEnabled(on); });
    addChkTo(gLight, QStringLiteral("tonemap"), QStringLiteral("Tonemap (ACES) + sRGB"), true,
             [this](bool on) { if (m_modelView) m_modelView->setFeatureTonemap(on); });

    auto* gShade = addGroup(QStringLiteral("Shading"));
    addChkTo(gShade, QStringLiteral("detail"), QStringLiteral("Detail maps"), true,
             [this](bool on) { if (m_modelView) m_modelView->setFeatureDetail(on); });
    addChkTo(gShade, QStringLiteral("subsurface"), QStringLiteral("Subsurface / translucency"), true,
             [this](bool on) { if (m_modelView) m_modelView->setFeatureSubsurface(on); });
    addChkTo(gShade, QStringLiteral("hair"), QStringLiteral("Hair anisotropy"), true,
             [this](bool on) { if (m_modelView) m_modelView->setFeatureHair(on); });
    addChkTo(gShade, QStringLiteral("specaa"), QStringLiteral("Specular anti-aliasing"), true,
             [this](bool on) { if (m_modelView) m_modelView->setFeatureSpecAA(on); });

    auto* gGeom = addGroup(QStringLiteral("Geometry & debug"));
    addChkTo(gGeom, QStringLiteral("backfaces"), QStringLiteral("Show back faces (no culling)"), true,
             [this](bool on) { if (m_modelView) m_modelView->setBackfaceCull(!on); });
    addChkTo(gGeom, QStringLiteral("mask"), QStringLiteral("Primary mask"), false,
             [this](bool on) { if (m_modelView) m_modelView->setFeatureMask(on); });

    // (The old "Viewport guides" group moved to the toolbar's Overlays ▾ dropdown — one home,
    // no duplicated checkboxes to keep in sync.)

    // ── Backdrop: one-click studio presets + optional vertical gradient + custom colour. ──
    {
        auto* gBg = addGroup(QStringLiteral("Backdrop"));
        auto* row = new QHBoxLayout();
        row->setSpacing(4);
        auto chip = [&](const char* name, const QColor& c) {
            auto* b = new QToolButton(m_vpPanel);
            b->setFixedSize(24, 20);
            b->setToolTip(QString::fromLatin1(name));
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(QStringLiteral("QToolButton{background:%1;border:1px solid #555;"
                                            "border-radius:3px;}QToolButton:hover{border-color:#b0453c;}")
                                 .arg(c.name()));
            connect(b, &QToolButton::clicked, this, [this, c]() {
                if (m_modelView) m_modelView->setBackgroundColor(c);
                QSettings().setValue(QStringLiteral("models/bgColor"), c.name());
            });
            row->addWidget(b);
        };
        chip("Dark",     QColor(0x10, 0x10, 0x10));
        chip("Charcoal", QColor(0x23, 0x23, 0x23));
        chip("Grey",     QColor(0x4b, 0x4b, 0x4b));
        chip("Light",    QColor(0xa6, 0xa6, 0xa6));
        auto* custom = new QToolButton(m_vpPanel);
        custom->setText(QStringLiteral("…"));
        custom->setToolTip(QStringLiteral("Custom background colour"));
        custom->setFixedSize(24, 20);
        custom->setCursor(Qt::PointingHandCursor);
        custom->setStyleSheet(QLatin1String(kArrowBtnQss));
        connect(custom, &QToolButton::clicked, this, [this]() {
            if (!m_modelView) return;
            const QColor c = QColorDialog::getColor(m_modelView->backgroundColor(), m_vpPanel,
                                                    QStringLiteral("Viewport background"));
            if (c.isValid()) {
                m_modelView->setBackgroundColor(c);
                QSettings().setValue(QStringLiteral("models/bgColor"), c.name());
            }
        });
        row->addWidget(custom);
        row->addStretch(1);
        gBg->addLayout(row);
        auto* grad = new QCheckBox(QStringLiteral("Gradient (lighter top, darker floor)"), m_vpPanel);
        grad->setToolTip(QStringLiteral("Studio-style vertical wash derived from the backdrop colour."));
        grad->setChecked(s.value(QStringLiteral("models/bgGradient"), false).toBool());
        connect(grad, &QCheckBox::toggled, this, [this](bool on) {
            QSettings().setValue(QStringLiteral("models/bgGradient"), on);
            if (m_modelView) m_modelView->setBackgroundGradient(on);
        });
        gBg->addWidget(grad);
    }
}

// ── Pigment (dye-zone) popup — the D4 dye system, split out of Graphics into its own button ──
void ModelsTab::toggleDyePanel()
{
    if (m_dyePanel && m_dyePanel->isVisible()) { m_dyePanel->hide(); return; }
    if (!m_dyePanel) buildDyePanel();
    m_dyePanel->adjustSize();
    m_dyePanel->move(panelPosLeftOf(m_dyeBtn, m_dyePanel->sizeHint()));
    m_dyePanel->show();
    m_dyePanel->raise();
}

void ModelsTab::buildDyePanel()
{
    if (m_dyePanel) return;
    QSettings s;
    m_dyePanel = new QFrame(this, Qt::Popup);
    m_dyePanel->setObjectName(QStringLiteral("vpPanel"));   // reuse the popup styling
    m_dyePanel->setProperty("hoverBtn", QVariant::fromValue<QObject*>(m_dyeBtn));
    m_dyePanel->installEventFilter(this);
    m_dyePanel->setStyleSheet(QStringLiteral(
        "QFrame#vpPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
        "QLabel{color:#cccccc;} QCheckBox{color:#cccccc;}"));
    auto* pl = new QVBoxLayout(m_dyePanel);
    pl->setContentsMargins(12, 10, 12, 10);
    pl->setSpacing(5);
    auto* hdr = new QLabel(QStringLiteral("Pigment (dye zones)"), m_dyePanel);
    hdr->setStyleSheet(QLatin1String(kHdrQss));
    pl->addWidget(hdr);

    {   // Master dye-recolor toggle (writes the same models/viewport/dye key as before).
        auto* cb = new QCheckBox(QStringLiteral("Dye recolor"), m_dyePanel);
        cb->setChecked(s.value(QStringLiteral("models/viewport/dye"), false).toBool());
        connect(cb, &QCheckBox::toggled, this, [this](bool on) {
            QSettings().setValue(QStringLiteral("models/viewport/dye"), on);
            if (m_modelView) m_modelView->setFeatureDye(on);
        });
        pl->addWidget(cb);
    }
    // Real D4 dye picker (arColorSamples gradient) — "Custom" falls back to the manual
    // colour slots below.
    {
        auto* dyeRow = new QHBoxLayout();
        dyeRow->addWidget(new QLabel(QStringLiteral("Dye:"), m_dyePanel));
        m_dyeCombo = new QComboBox(m_dyePanel);
        connect(m_dyeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int i) {
            if (i <= 0) {
                QSettings().setValue(QStringLiteral("models/viewport/dyeSel"), QStringLiteral("custom"));
            } else {
                QSettings().setValue(QStringLiteral("models/viewport/dyeSel"), m_dyeCombo->itemText(i));
                const QStringList hex = m_dyeCombo->itemData(i).toStringList();
                for (int k = 0; k < hex.size() && k < 4; ++k) setDyeSlotColor(k, QColor(hex[k]));
            }
        });
        rebuildDyeCombo();   // Custom + real dyes + saved custom pigments
        dyeRow->addWidget(m_dyeCombo, 1);
        auto* saveBtn = new QToolButton(m_dyePanel);
        saveBtn->setText(QStringLiteral("＋"));
        saveBtn->setToolTip(QStringLiteral("Save the current 4 colours as a custom pigment"));
        connect(saveBtn, &QToolButton::clicked, this, [this]() {
            const QString name = QInputDialog::getText(this, QStringLiteral("Save pigment"),
                                                       QStringLiteral("Pigment name:")).trimmed();
            if (name.isEmpty()) return;
            QStringList parts; parts << name;
            for (int k = 0; k < 4; ++k)
                parts << QColor(QSettings().value(QStringLiteral("models/viewport/dyeColor%1").arg(k),
                                                  QStringLiteral("#ffffff")).toString()).name();
            QStringList saved = QSettings().value(QStringLiteral("models/customPigments")).toStringList();
            QStringList kept;
            for (const QString& e : saved) if (e.section(QLatin1Char('\t'), 0, 0) != name) kept << e;
            kept << parts.join(QLatin1Char('\t'));
            QSettings().setValue(QStringLiteral("models/customPigments"), kept);
            rebuildDyeCombo();
            const int idx = m_dyeCombo->findText(name);
            if (idx > 0) m_dyeCombo->setCurrentIndex(idx);
        });
        dyeRow->addWidget(saveBtn);
        auto* delBtn = new QToolButton(m_dyePanel);
        delBtn->setText(QStringLiteral("－"));
        delBtn->setToolTip(QStringLiteral("Delete the selected custom pigment"));
        connect(delBtn, &QToolButton::clicked, this, [this]() {
            const int i = m_dyeCombo->currentIndex();
            if (i <= 0) return;
            const QString name = m_dyeCombo->itemText(i);
            QStringList saved = QSettings().value(QStringLiteral("models/customPigments")).toStringList();
            QStringList kept; bool removed = false;
            for (const QString& e : saved) {
                if (e.section(QLatin1Char('\t'), 0, 0) == name) { removed = true; continue; }
                kept << e;
            }
            if (!removed) return;   // built-in dye, not a custom pigment
            QSettings().setValue(QStringLiteral("models/customPigments"), kept);
            rebuildDyeCombo();
        });
        dyeRow->addWidget(delBtn);
        auto* dbg = new QToolButton(m_dyePanel);
        dbg->setText(QStringLiteral("🔍"));
        dbg->setToolTip(QStringLiteral("Inspect this model's DyeMask / DyeRamp values"));
        connect(dbg, &QToolButton::clicked, this, [this]() { dumpDyeDebug(); });
        dyeRow->addWidget(dbg);
        pl->addLayout(dyeRow);
    }
    m_dyeUsageLbl = new QLabel(QStringLiteral("Custom colours (one per dyeable material):"), m_dyePanel);
    pl->addWidget(m_dyeUsageLbl);
    auto* regRow = new QHBoxLayout();
    auto* grp = new QButtonGroup(m_dyePanel);
    grp->setExclusive(true);
    for (int r = 0; r < 4; ++r) {
        m_dyeRegionBtn[r] = new QToolButton(m_dyePanel);
        m_dyeRegionBtn[r]->setCheckable(true);
        m_dyeRegionBtn[r]->setFixedSize(36, 22);
        m_dyeRegionBtn[r]->setText(QString::number(r + 1));
        m_dyeRegionBtn[r]->setAcceptDrops(true);
        m_dyeRegionBtn[r]->installEventFilter(this);   // drag / drop / reset
        grp->addButton(m_dyeRegionBtn[r], r);
        regRow->addWidget(m_dyeRegionBtn[r]);
    }
    regRow->addStretch(1);
    pl->addLayout(regRow);
    connect(grp, &QButtonGroup::idClicked, this, [this](int id) { loadDyeRegionToPicker(id); });

    // HSV colour wheel (replaces RGB sliders) → live recolour of the active slot.
    auto* wheel = new DyeColorWheel(m_dyePanel);
    wheel->onChanged = [this](const QColor&) { applyDyePicker(); };
    m_dyeWheel = wheel;
    pl->addWidget(wheel, 0, Qt::AlignHCenter);

    // Hex entry + live swatch.
    auto* hexRow = new QHBoxLayout();
    m_dyeSwatch = new QLabel(m_dyePanel);
    m_dyeSwatch->setFixedSize(28, 18);
    m_dyeSwatch->setStyleSheet(QStringLiteral("border:1px solid #888;"));
    hexRow->addWidget(m_dyeSwatch);
    m_dyeHex = new QLineEdit(m_dyePanel);
    m_dyeHex->setMaxLength(7);
    m_dyeHex->setPlaceholderText(QStringLiteral("#rrggbb"));
    connect(m_dyeHex, &QLineEdit::editingFinished, this, [this]() {
        const QColor c(m_dyeHex->text().trimmed());
        if (c.isValid()) setDyeSlotColor(m_dyeRegion, c);
    });
    hexRow->addWidget(m_dyeHex, 1);
    pl->addLayout(hexRow);

    // Memory swatches — drag a colour here to store it; drag one onto a slot to use
    // it, or click to apply it to the selected slot. Right-click clears one.
    pl->addWidget(new QLabel(QStringLiteral("Memory (drag colours here):"), m_dyePanel));
    auto* memRow = new QHBoxLayout();
    memRow->setSpacing(2);
    for (int i = 0; i < 8; ++i) {
        m_dyeMem[i] = new QToolButton(m_dyePanel);
        m_dyeMem[i]->setFixedSize(18, 18);
        m_dyeMem[i]->setAcceptDrops(true);
        m_dyeMem[i]->installEventFilter(this);
        m_dyeMem[i]->setToolTip(QStringLiteral(
            "Memory %1 — drag a colour here to store · drag out / click to apply · right-click to clear")
            .arg(i + 1));
        const QString mc = s.value(QStringLiteral("models/viewport/dyeMem%1").arg(i)).toString();
        const QColor c = mc.isEmpty() ? QColor() : QColor(mc);
        m_dyeMem[i]->setStyleSheet(c.isValid()
            ? QStringLiteral("QToolButton{background:%1;border:1px solid #555;}").arg(c.name())
            : QStringLiteral("QToolButton{background:#2b2b2b;border:1px dashed #555;}"));
        connect(m_dyeMem[i], &QToolButton::clicked, this, [this, i]() {
            const QString mc2 = QSettings().value(QStringLiteral("models/viewport/dyeMem%1").arg(i)).toString();
            if (!mc2.isEmpty()) setDyeSlotColor(m_dyeRegion, QColor(mc2));
        });
        memRow->addWidget(m_dyeMem[i]);
    }
    memRow->addStretch(1);
    pl->addLayout(memRow);

    m_dyeRegionBtn[0]->setChecked(true);
    loadDyeRegionToPicker(0);
    updateDyeSlotUsage();   // reflect the current model's dyeable-material count
}

void ModelsTab::loadDyeRegionToPicker(int r)
{
    if (r < 0 || r > 3 || !m_dyeWheel) return;
    m_dyeRegion = r;
    const QColor c(QSettings().value(QStringLiteral("models/viewport/dyeColor%1").arg(r),
                                     QStringLiteral("#ffffff")).toString());
    static_cast<DyeColorWheel*>(m_dyeWheel)->setColor(c);   // setColor does not echo back
    if (m_dyeSwatch)
        m_dyeSwatch->setStyleSheet(QStringLiteral("background:%1;border:1px solid #888;").arg(c.name()));
    if (m_dyeHex) { QSignalBlocker bh(m_dyeHex); m_dyeHex->setText(c.name()); }
}

void ModelsTab::applyDyePicker()
{
    if (!m_dyeWheel) return;
    const QColor c = static_cast<DyeColorWheel*>(m_dyeWheel)->color();
    if (m_dyeSwatch)
        m_dyeSwatch->setStyleSheet(QStringLiteral("background:%1;border:1px solid #888;").arg(c.name()));
    if (m_dyeHex) { QSignalBlocker bh(m_dyeHex); m_dyeHex->setText(c.name()); }
    QSettings().setValue(QStringLiteral("models/viewport/dyeColor%1").arg(m_dyeRegion), c.name());
    if (m_modelView) m_modelView->setDyeColor(m_dyeRegion, c);
    styleDyeSlot(m_dyeRegion);
}

// Set a slot's colour outside the wheel flow (drop / memory / reset / hex).
void ModelsTab::setDyeSlotColor(int r, const QColor& c)
{
    if (r < 0 || r > 3 || !c.isValid()) return;
    QSettings().setValue(QStringLiteral("models/viewport/dyeColor%1").arg(r), c.name());
    if (m_modelView) m_modelView->setDyeColor(r, c);
    styleDyeSlot(r);
    if (r == m_dyeRegion) loadDyeRegionToPicker(r);   // refresh wheel / hex / swatch
}

// Restyle a dye slot from its stored colour + whether the current model uses it.
void ModelsTab::styleDyeSlot(int r)
{
    if (r < 0 || r > 3 || !m_dyeRegionBtn[r]) return;
    const QColor c(QSettings().value(QStringLiteral("models/viewport/dyeColor%1").arg(r),
                                     QStringLiteral("#ffffff")).toString());
    const bool used = m_dyeRegionUsed[r];
    m_dyeRegionBtn[r]->setStyleSheet(QStringLiteral(
        "QToolButton{background:%1;color:%2;font-weight:bold;border:%3;}"
        "QToolButton:checked{border:2px solid #d8a23a;}")
        .arg(c.name(), used ? QStringLiteral("#000") : QStringLiteral("#777"),
             used ? QStringLiteral("1px solid #888") : QStringLiteral("1px dashed #555")));
    m_dyeRegionBtn[r]->setToolTip(used
        ? QStringLiteral("Slot %1 — %2\nclick to edit · drag onto another slot to copy · right-click to reset")
              .arg(r + 1).arg(m_dyeRegionName[r].isEmpty() ? QStringLiteral("dyeable material") : m_dyeRegionName[r])
        : QStringLiteral("Slot %1 — not used by this model").arg(r + 1));
}

// Mark which dye slots the loaded model actually uses (dimmed = unused).
void ModelsTab::updateDyeSlotUsage()
{
    for (int r = 0; r < 4; ++r) m_dyeRegionUsed[r] = (r < m_dyeRegionsUsed);
    if (m_dyeUsageLbl) {
        m_dyeUsageLbl->setText(m_dyeRegionsUsed == 0
            ? QStringLiteral("Pigment: this model has no dyeable materials")
            : QStringLiteral("Pigment colours (DyeMask zones 1–4):"));
    }
    if (!m_dyeRegionBtn[0]) return;   // panel not built yet
    // If the active slot is now unused, snap selection back to the first slot.
    if (m_dyeRegionsUsed > 0 && m_dyeRegion >= m_dyeRegionsUsed) {
        m_dyeRegionBtn[0]->setChecked(true);
        loadDyeRegionToPicker(0);
    }
    for (int r = 0; r < 4; ++r) styleDyeSlot(r);
}

// Decode this model's DyeMask / DyeRamp textures and show their previews + value
// histograms, so we can see whether the mask is banded into colour zones.
// Repopulate the dye dropdown: Custom + real D4 dyes + the user's saved pigments.
void ModelsTab::rebuildDyeCombo()
{
    if (!m_dyeCombo) return;
    QSignalBlocker block(m_dyeCombo);
    const QString cur = QSettings().value(QStringLiteral("models/viewport/dyeSel"),
                                          QStringLiteral("custom")).toString();
    m_dyeCombo->clear();
    m_dyeCombo->addItem(QStringLiteral("Custom (manual colours)"));
    auto icon4 = [](const QColor c[4]) {
        QPixmap pm(16, 16); pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.fillRect(0, 0, 8, 8, c[0]); p.fillRect(8, 0, 8, 8, c[1]);
        p.fillRect(0, 8, 8, 8, c[2]); p.fillRect(8, 8, 8, 8, c[3]);
        p.end();
        return QIcon(pm);
    };
    for (const DyeDef& dd : loadPlayerDyes(Config::d4dataDir())) {
        QStringList hex; for (int k = 0; k < 4; ++k) hex << dd.colors[k].name();
        m_dyeCombo->addItem(icon4(dd.colors), dd.name, hex);
    }
    const QStringList saved = QSettings().value(QStringLiteral("models/customPigments")).toStringList();
    if (!saved.isEmpty()) m_dyeCombo->insertSeparator(m_dyeCombo->count());
    for (const QString& enc : saved) {
        const QStringList p = enc.split(QLatin1Char('\t'));
        if (p.size() < 5) continue;
        QColor c[4]; QStringList hex;
        for (int k = 0; k < 4; ++k) { c[k] = QColor(p[k + 1]); hex << c[k].name(); }
        m_dyeCombo->addItem(icon4(c), p[0], hex);
    }
    const int idx = (cur == QLatin1String("custom")) ? 0 : m_dyeCombo->findText(cur);
    m_dyeCombo->setCurrentIndex(idx > 0 ? idx : 0);
}

void ModelsTab::dumpDyeDebug()
{
    QStringList mats; QSet<QString> seen;
    for (const QString& m : m_appMatNames) {
        if (m.isEmpty() || seen.contains(m)) continue;
        seen.insert(m);
        if (materialHasRole(m, "DYE_MASK")) mats << m;
    }
    if (mats.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Dye debug"),
                                QStringLiteral("This model has no dyeable materials."));
        return;
    }
    const QString outDir = AppPaths::subDir(QStringLiteral("dye_debug"));

    // Histogram (256 bins of the red channel) + a one-line stat on banding.
    auto histAndStats = [](const QImage& img) -> QPair<QImage, QString> {
        if (img.isNull()) return {QImage(), QStringLiteral("(missing)")};
        const QImage g = img.convertToFormat(QImage::Format_RGBA8888);
        QVector<qint64> bins(256, 0); qint64 total = 0;
        for (int y = 0; y < g.height(); ++y) {
            const uchar* s = g.constScanLine(y);
            for (int x = 0; x < g.width(); ++x) { bins[s[x * 4]]++; total++; }
        }
        qint64 mx = 1; for (qint64 b : bins) mx = qMax(mx, b);
        QImage h(256, 70, QImage::Format_RGBA8888); h.fill(QColor(24, 24, 24));
        QPainter p(&h); p.setPen(QColor(0xd8, 0xa2, 0x3a));
        for (int b = 0; b < 256; ++b) p.drawLine(b, 70, b, 70 - int(69.0 * bins[b] / mx));
        p.end();
        QStringList peaks; int levels = 0;
        for (int b = 0; b < 256; ++b) {
            const double pct = total ? bins[b] * 100.0 / total : 0.0;
            if (pct > 1.0) ++levels;
            if (pct > 4.0) peaks << QStringLiteral("%1=%2%").arg(b).arg(int(pct + 0.5));
        }
        return {h, QStringLiteral("%1×%2 · levels >1%: %3 · peaks: %4")
                       .arg(g.width()).arg(g.height()).arg(levels)
                       .arg(peaks.isEmpty() ? QStringLiteral("(smooth/continuous)") : peaks.join(QStringLiteral("  ")))};
    };

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Dye textures — DyeMask / DyeRamp"));
    dlg.resize(560, 640);
    auto* outer = new QVBoxLayout(&dlg);
    auto* scroll = new QScrollArea(&dlg); scroll->setWidgetResizable(true);
    auto* content = new QWidget; auto* lay = new QVBoxLayout(content);
    lay->addWidget(new QLabel(QStringLiteral("Decoded PNGs saved to:\n%1").arg(outDir), content));

    auto thumb = [&](const QImage& im) {
        auto* l = new QLabel(content);
        l->setFixedSize(132, 132);
        l->setStyleSheet(QStringLiteral("border:1px solid #555;background:#111;"));
        l->setAlignment(Qt::AlignCenter);
        if (!im.isNull()) l->setPixmap(QPixmap::fromImage(im).scaled(128, 128, Qt::KeepAspectRatio));
        return l;
    };
    for (const QString& mat : mats) {
        const QImage mask = textureByRole(mat, "DYE_MASK");
        const QImage ramp = textureByRole(mat, "DYE_RAMP");
        if (!mask.isNull()) mask.save(QDir(outDir).filePath(mat + QStringLiteral("_DyeMask.png")));
        if (!ramp.isNull()) ramp.save(QDir(outDir).filePath(mat + QStringLiteral("_DyeRamp.png")));
        const auto mh = histAndStats(mask);
        const auto rh = histAndStats(ramp);
        auto* hdr = new QLabel(QStringLiteral("<b>%1</b>").arg(mat), content);
        lay->addWidget(hdr);
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(QStringLiteral("DyeMask"), content));
        row->addWidget(thumb(mask));
        row->addWidget(new QLabel(QStringLiteral("DyeRamp"), content));
        row->addWidget(thumb(ramp));
        row->addStretch(1);
        lay->addLayout(row);
        auto histLbl = [&](const QImage& hh) { auto* l = new QLabel(content);
            if (!hh.isNull()) l->setPixmap(QPixmap::fromImage(hh)); return l; };
        lay->addWidget(new QLabel(QStringLiteral("Mask histogram — %1").arg(mh.second), content));
        lay->addWidget(histLbl(mh.first));
        lay->addWidget(new QLabel(QStringLiteral("Ramp histogram — %1").arg(rh.second), content));
        lay->addWidget(histLbl(rh.first));
    }
    lay->addStretch(1);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    outer->addWidget(bb);
    dlg.exec();
}

QImage ModelsTab::decodeTexImage(const QString& texName, int texSno) const
{
    if (texName.isEmpty() || texSno <= 0)
        return {};
    const QString d4 = Config::d4dataDir();
    TexMeta meta;
    if (!d4.isEmpty()) {
        QFile f(QStringLiteral("%1/json/base/meta/Texture/%2.tex.json").arg(d4, texName));
        if (f.open(QIODevice::ReadOnly))
            meta = parseTexMetaJson(f.readAll());
    }
    if (!meta.valid)
        return {};
    QByteArray payload;
    if (m_reader && m_reader->isReady())
        payload = m_reader->readPayloadBySno(quint64(texSno));
    if (payload.isEmpty())
        return {};
    return BcDecode::decode(payload, meta.width, meta.height, meta.eTexFormat);
}

void ModelsTab::setChannelTile(int idx, const QImage& img)
{
    if (idx < 0 || idx >= 6 || !m_chanImg[idx])
        return;
    m_chanFull[idx] = img;   // keep native resolution for the zoom preview
    if (img.isNull()) {
        m_chanImg[idx]->setPixmap(QPixmap());
        m_chanImg[idx]->setText(QStringLiteral("—"));
        return;
    }
    m_chanImg[idx]->setText(QString());
    const int side = qMax(8, m_chanImg[idx]->width() - 6);   // inner area inside the border
    m_chanImg[idx]->setPixmap(QPixmap::fromImage(
        img.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

void ModelsTab::clearTexturePreview()
{
    if (m_texFacts) m_texFacts->clear();
    for (int i = 0; i < 6; ++i)
        setChannelTile(i, QImage());
}

// Map the selected material's textures (role column of m_matTexModel) onto the six
// preview tiles: BASE_COLOR→COLOR(+its alpha→ALPHA), ROUGHNESS→ROUGHNESS,
// METALLIC→METAL, NORMAL→NORMAL, EMISSIVE→EMISSIVE.
void ModelsTab::showMaterialChannels()
{
    static const char* const kMatCaps[6] = {"COLOR", "ROUGHNESS", "METAL",
                                            "NORMAL", "ALPHA", "EMISSIVE"};
    setTileCaptions(kMatCaps);
    clearTexturePreview();
    QImage baseColor;
    bool filled[6] = {false, false, false, false, false, false};
    for (int r = 0; r < m_matTexModel->rowCount(); ++r) {
        const QString role    = m_matTexModel->item(r, 0)->text();                       // SHADERTEX
        const QString texName = m_matTexModel->item(r, 2)->text();                       // NAME
        const int     texSno  = m_matTexModel->item(r, 1)->data(Qt::DisplayRole).toInt(); // SNO

        int tile = -1;
        if (role == QLatin1String("BASE_COLOR"))      tile = 0;
        else if (role == QLatin1String("ROUGHNESS"))  tile = 1;
        else if (role == QLatin1String("METALLIC"))   tile = 2;
        else if (role == QLatin1String("NORMAL"))     tile = 3;
        else if (role == QLatin1String("EMISSIVE"))   tile = 5;
        if (tile < 0 || filled[tile])
            continue;   // first texture per role wins (primary before tileable effects)
        const QImage img = decodeTexImage(texName, texSno);
        if (img.isNull())
            continue;
        filled[tile] = true;
        setChannelTile(tile, img);
        if (tile == 0)
            baseColor = img;   // keep for the ALPHA tile
    }
    // ALPHA tile: the base-colour map's alpha channel rendered as greyscale.
    if (!baseColor.isNull() && baseColor.hasAlphaChannel())
        setChannelTile(4, channelGrey(baseColor, 3));
}
