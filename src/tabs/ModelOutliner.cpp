#include "tabs/ModelOutliner.h"
#include "index/SnoListModel.h"
#include "index/CoreToc.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QToolTip>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QSize>
#include <QStyle>

#include <cmath>

ModelOutlinerModel::ModelOutlinerModel(SnoListModel* src, QObject* parent)
    : QAbstractItemModel(parent), m_src(src)
{
    // Forward the source's change signals with 1:1 row mapping. The source only ever mutates via
    // begin/endResetModel (rebuild/sort) and dataChanged (icons/thumbnails), so these four cover it;
    // layout* are forwarded defensively in case that ever changes.
    connect(m_src, &QAbstractItemModel::modelAboutToBeReset, this, [this] { beginResetModel(); });
    connect(m_src, &QAbstractItemModel::modelReset, this, [this] {
        relocateHost();   // filter/sort moved (or hid) the loaded model's row — the subtree follows it
        endResetModel();
    });
    connect(m_src, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& tl, const QModelIndex& br, const QList<int>& roles) {
                if (!tl.isValid() || !br.isValid()) return;
                emit dataChanged(createIndex(tl.row(), tl.column()),
                                 createIndex(br.row(), br.column()), roles);
            });
    connect(m_src, &QAbstractItemModel::layoutAboutToBeChanged, this,
            [this] { emit layoutAboutToBeChanged(); });
    connect(m_src, &QAbstractItemModel::layoutChanged, this, [this] {
        relocateHost();
        emit layoutChanged();
    });
    connect(m_src, &QAbstractItemModel::headerDataChanged, this,
            &QAbstractItemModel::headerDataChanged);
}

ModelOutlinerModel::~ModelOutlinerModel() { delete m_root; }

ModelOutlinerModel::Node* ModelOutlinerModel::node(const QModelIndex& ix) const
{
    return static_cast<Node*>(ix.internalPointer());   // nullptr = top-level browse row
}

void ModelOutlinerModel::relocateHost()
{
    m_hostRow = -1;
    if (m_hostSno < 0 || !m_src) return;
    for (int r = 0; r < m_src->rowCount(); ++r)
        if (const SnoEntry* e = m_src->entryAt(r))
            if (e->snoId == m_hostSno) { m_hostRow = r; return; }
}

void ModelOutlinerModel::setSubtree(int sno, Node* root)
{
    clearSubtree();
    m_hostSno = sno;
    relocateHost();
    const int n = root ? root->kids.size() : 0;
    if (m_hostRow >= 0 && n > 0) {
        // Proper row insertion (not a reset) so the view keeps its selection — the row that was
        // just clicked to load this model must stay selected.
        beginInsertRows(createIndex(m_hostRow, 0, nullptr), 0, n - 1);
        m_root = root;
        endInsertRows();
    } else {
        m_root = root;   // host filtered out right now; shows up when relocateHost finds it again
    }
}

void ModelOutlinerModel::clearSubtree()
{
    if (m_root) {
        const int n = m_root->kids.size();
        if (m_hostRow >= 0 && n > 0) {
            beginRemoveRows(createIndex(m_hostRow, 0, nullptr), 0, n - 1);
            Node* old = m_root;
            m_root = nullptr;
            delete old;
            endRemoveRows();
        } else {
            delete m_root;
            m_root = nullptr;
        }
    }
    m_hostSno = -1;
    m_hostRow = -1;
}

// ── Part helpers (the tree is the part-visibility source of truth) ──────────────────────────

void ModelOutlinerModel::collectParts(const Node* n, QList<int>& out) const
{
    if (!n) return;
    if (n->kind == Part && n->ref >= 0) out << n->ref;
    for (const Node* k : n->kids) collectParts(k, out);
}

QList<int> ModelOutlinerModel::partsUnder(const QModelIndex& ix) const
{
    QList<int> out;
    collectParts(node(ix), out);
    return out;
}

void ModelOutlinerModel::partChecks(QHash<int, bool>& out) const
{
    if (!m_root) return;
    // Parts are direct kids of the root container (flat), but walk generically to stay
    // future-proof if grouping levels are ever added.
    std::function<void(const Node*)> walk = [&](const Node* n) {
        if (n->kind == Part && n->ref >= 0) out[n->ref] = (n->check == Qt::Checked);
        for (const Node* k : n->kids) walk(k);
    };
    walk(m_root);
}

void ModelOutlinerModel::partExportFlags(QHash<int, bool>& out) const
{
    if (!m_root) return;
    std::function<void(const Node*)> walk = [&](const Node* n) {
        if (n->kind == Part && n->ref >= 0) out[n->ref] = n->exportOn;
        for (const Node* k : n->kids) walk(k);
    };
    walk(m_root);
}

void ModelOutlinerModel::togglePartExport(const QModelIndex& ix)
{
    Node* n = node(ix);
    if (!n || n->kind != Part) return;
    n->exportOn = !n->exportOn;
    const QModelIndex cell = ix.siblingAtColumn(kTreeCol);
    emit dataChanged(cell, cell, {ExportRole});   // repaint the camera glyph
}

void ModelOutlinerModel::setPartChecks(const QVector<bool>& flags, bool on)
{
    if (!m_root) return;
    std::function<void(Node*, const QModelIndex&)> walk = [&](Node* n, const QModelIndex& ixOfN) {
        for (int i = 0; i < n->kids.size(); ++i) {
            Node* k = n->kids[i];
            const QModelIndex kIx = index(i, kTreeCol, ixOfN);
            if (k->kind == Part && k->ref >= 0 && k->ref < flags.size() && flags[k->ref]) {
                const Qt::CheckState want = on ? Qt::Checked : Qt::Unchecked;
                if (k->check != want) {
                    k->check = want;
                    emit dataChanged(kIx, kIx, {Qt::CheckStateRole});
                    // deliberately NOT partCheckChanged — the bulk caller recomputes once
                }
            }
            walk(k, index(i, 0, ixOfN));
        }
    };
    if (m_hostRow < 0) {   // host row hidden by a filter: mutate silently, no indexes to signal
        std::function<void(Node*)> quiet = [&](Node* n) {
            if (n->kind == Part && n->ref >= 0 && n->ref < flags.size() && flags[n->ref])
                n->check = on ? Qt::Checked : Qt::Unchecked;
            for (Node* k : n->kids) quiet(k);
        };
        quiet(m_root);
        return;
    }
    walk(m_root, createIndex(m_hostRow, 0, nullptr));
}

void ModelOutlinerModel::setPartCheck(int prim, bool on)
{
    if (!m_root || m_hostRow < 0) return;
    const QModelIndex host = createIndex(m_hostRow, 0, nullptr);
    for (int i = 0; i < m_root->kids.size(); ++i) {
        Node* k = m_root->kids[i];
        if (k->kind != Part || k->ref != prim) continue;
        const Qt::CheckState want = on ? Qt::Checked : Qt::Unchecked;
        if (k->check != want) {
            k->check = want;
            const QModelIndex ix = index(i, kTreeCol, host);
            emit dataChanged(ix, ix, {Qt::CheckStateRole});   // silent — caller recomputes once
        }
        return;
    }
}

QModelIndex ModelOutlinerModel::indexOfPart(int prim) const
{
    if (!m_root || m_hostRow < 0) return {};
    const QModelIndex host = createIndex(m_hostRow, 0, nullptr);
    for (int i = 0; i < m_root->kids.size(); ++i)
        if (m_root->kids[i]->kind == Part && m_root->kids[i]->ref == prim)
            return index(i, 0, host);
    return {};
}

void ModelOutlinerModel::setNodeText(Kind kind, const QString& text)
{
    if (!m_root || m_hostRow < 0) return;
    const QModelIndex host = createIndex(m_hostRow, 0, nullptr);
    for (int i = 0; i < m_root->kids.size(); ++i) {
        Node* k = m_root->kids[i];
        if (k->kind != kind) continue;
        if (k->text != text) {
            k->text = text;
            const QModelIndex ix = index(i, kTreeCol, host);
            emit dataChanged(ix, ix, {Qt::DisplayRole});
        }
        return;   // first node of the kind only (Animations/Armature are singletons)
    }
}

void ModelOutlinerModel::relabelParts(const std::function<QString(int)>& labelFor)
{
    if (!m_root || m_hostRow < 0) return;
    const QModelIndex host = createIndex(m_hostRow, 0, nullptr);
    for (int i = 0; i < m_root->kids.size(); ++i) {
        Node* k = m_root->kids[i];
        if (k->kind != Part || k->ref < 0) continue;
        const QString t = labelFor(k->ref);
        if (t == k->text) continue;
        k->text = t;
        const QModelIndex ix = index(i, kTreeCol, host);
        emit dataChanged(ix, ix, {Qt::DisplayRole});
    }
}

// Compose one decoded tile into a strip node's pixmap (dark placeholders until each arrives).
void ModelOutlinerModel::setNodeTileImage(const QModelIndex& ix, int tile, const QImage& img)
{
    Node* n = node(ix);
    if (!n || tile < 0 || tile >= n->tiles.size()) return;
    const int w = n->tiles.size() * (kTilePx + kTileGap) - kTileGap;
    if (n->icon.isNull() || n->icon.width() != w) {
        n->icon = QPixmap(w, kTilePx);
        n->icon.fill(Qt::transparent);
        QPainter p(&n->icon);
        p.setPen(QColor(0x44, 0x44, 0x44));
        p.setBrush(QColor(0x1b, 0x1b, 0x1b));
        for (int i = 0; i < n->tiles.size(); ++i)
            p.drawRect(i * (kTilePx + kTileGap), 0, kTilePx - 1, kTilePx - 1);
    }
    {
        QPainter p(&n->icon);
        const QRect slot(tile * (kTilePx + kTileGap), 0, kTilePx, kTilePx);
        if (!img.isNull()) {
            const QImage s = img.scaled(kTilePx, kTilePx, Qt::KeepAspectRatioByExpanding,
                                        Qt::SmoothTransformation);
            p.drawImage(slot, s, QRect((s.width() - kTilePx) / 2, (s.height() - kTilePx) / 2,
                                       kTilePx, kTilePx));
        }
        p.setPen(QColor(0x44, 0x44, 0x44));
        p.setBrush(Qt::NoBrush);
        p.drawRect(slot.adjusted(0, 0, -1, -1));
    }
    n->tilesDone = qMax(n->tilesDone, tile + 1);
    const QModelIndex cell = ix.siblingAtColumn(kTreeCol);
    emit dataChanged(cell, cell, {Qt::DecorationRole});
}

// Looks are radio-like: exactly one eye on. Called by ModelsTab whenever the active look
// changes (tree eye OR table selection), so both stay in lockstep.
void ModelOutlinerModel::setExclusiveLookCheck(int ref)
{
    if (!m_root || m_hostRow < 0) return;
    const QModelIndex host = createIndex(m_hostRow, 0, nullptr);
    for (int i = 0; i < m_root->kids.size(); ++i) {
        Node* g = m_root->kids[i];
        if (g->kind != LookRoot) continue;
        const QModelIndex gIx = index(i, 0, host);
        for (int k = 0; k < g->kids.size(); ++k) {
            Node* lk = g->kids[k];
            const Qt::CheckState want = (lk->ref == ref) ? Qt::Checked : Qt::Unchecked;
            if (lk->check != want) {
                lk->check = want;
                const QModelIndex ixk = index(k, kTreeCol, gIx);
                emit dataChanged(ixk, ixk, {Qt::CheckStateRole});
            }
        }
        return;
    }
}

void ModelOutlinerModel::setNodeIcon(const QModelIndex& ix, const QPixmap& pm)
{
    Node* n = node(ix);
    if (!n || pm.isNull()) return;
    n->icon = pm;
    const QModelIndex cell = ix.siblingAtColumn(kTreeCol);
    emit dataChanged(cell, cell, {Qt::DecorationRole});
}

QVector<QModelIndex> ModelOutlinerModel::iconlessTextureLeaves() const
{
    QVector<QModelIndex> out;
    if (!m_root || m_hostRow < 0) return out;
    std::function<void(const Node*, const QModelIndex&)> walk =
        [&](const Node* n, const QModelIndex& ixOfN) {
        for (int i = 0; i < n->kids.size(); ++i) {
            const Node* k = n->kids[i];
            const QModelIndex kIx = index(i, 0, ixOfN);
            if (k->kind == Texture && k->icon.isNull() && !k->aux.isEmpty())
                out << kIx;
            else if ((k->kind == MatTiles || k->kind == TexTiles)
                     && k->tilesDone < k->tiles.size())
                out << kIx;   // strip with tiles still to decode
            walk(k, kIx);
        }
    };
    walk(m_root, createIndex(m_hostRow, 0, nullptr));
    return out;
}

// Blender-style type glyphs, drawn once per kind — no image assets, immune to the QSS theme.
QPixmap ModelOutlinerModel::kindIcon(Kind kind)
{
    static QHash<int, QPixmap> cache;
    auto it = cache.find(int(kind));
    if (it != cache.end()) return it.value();

    constexpr int S = 14;
    QPixmap pm(S, S);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    switch (kind) {
    case AnimRoot:
    case Anim: {       // green play triangle (leaf clips share the glyph, slightly smaller)
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(120, 220, 140));
        if (kind == AnimRoot)
            p.drawPolygon(QPolygonF({{4.0, 2.5}, {4.0, 11.5}, {11.5, 7.0}}));
        else
            p.drawPolygon(QPolygonF({{5.0, 4.0}, {5.0, 10.0}, {10.5, 7.0}}));
        break;
    }
    case Armature:
    case Bone: {       // tan "bone": two knobs + connecting shaft
        const QColor c(200, 190, 150);
        p.setPen(QPen(c, 2.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(4.5, 9.5), QPointF(9.5, 4.5));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(3.5, 10.5), 2.1, 2.1);
        p.drawEllipse(QPointF(10.5, 3.5), 2.1, 2.1);
        break;
    }
    case Part: {       // Blender's mesh orange, as a triangle outline with vertex dots
        const QColor c(255, 160, 70);
        p.setPen(QPen(c, 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(QPolygonF({{7.0, 2.5}, {12.0, 11.5}, {2.0, 11.5}}));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        for (const QPointF& v : {QPointF(7.0, 2.5), QPointF(12.0, 11.5), QPointF(2.0, 11.5)})
            p.drawEllipse(v, 1.5, 1.5);
        break;
    }
    case Material: {   // shaded sphere
        QRadialGradient g(QPointF(5.5, 5.0), 8.0);
        g.setColorAt(0.0, QColor(250, 170, 150));
        g.setColorAt(1.0, QColor(160, 70, 60));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(QPointF(7.0, 7.0), 5.2, 5.2);
        break;
    }
    case TexGroup:
    case Texture: {    // 2×2 checkerboard
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(210, 210, 210));
        p.drawRect(2, 2, 5, 5);   p.drawRect(7, 7, 5, 5);
        p.setBrush(QColor(110, 110, 110));
        p.drawRect(7, 2, 5, 5);   p.drawRect(2, 7, 5, 5);
        break;
    }
    case ValueGroup:
    case Value: {      // two slider bars with handles
        const QColor c(140, 180, 230);
        p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(2.5, 5.0), QPointF(11.5, 5.0));
        p.drawLine(QPointF(2.5, 9.5), QPointF(11.5, 9.5));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(9.0, 5.0), 1.8, 1.8);
        p.drawEllipse(QPointF(5.0, 9.5), 1.8, 1.8);
        break;
    }
    case LookRoot:
    case Look: {       // open eye — looks are appearance variants you switch between
        const QColor c(200, 190, 150);
        p.setPen(QPen(c, 1.2));
        p.setBrush(Qt::NoBrush);
        QPainterPath lid;
        lid.moveTo(1.5, 7.0);  lid.quadTo(7.0, 2.0, 12.5, 7.0);
        lid.moveTo(1.5, 7.0);  lid.quadTo(7.0, 12.0, 12.5, 7.0);
        p.drawPath(lid);
        p.setBrush(c);
        p.drawEllipse(QPointF(7.0, 7.0), 1.7, 1.7);
        break;
    }
    case MatTiles:
    case TexTiles: {   // tiny tile strip
        p.setPen(QPen(QColor(110, 110, 110), 1.0));
        p.setBrush(QColor(60, 60, 60));
        for (int i = 0; i < 3; ++i) p.drawRect(1 + i * 4, 5, 3, 4);
        break;
    }
    case ShaderGroup:
    case Shader: {     // purple hexagon (node-socket vibe)
        const QColor c(180, 140, 220);
        p.setPen(QPen(c, 1.4));
        p.setBrush(c.darker(160));
        constexpr double kPi = 3.14159265358979323846;   // M_PI needs _USE_MATH_DEFINES on MSVC
        QPolygonF hex;
        for (int i = 0; i < 6; ++i) {
            const double a = kPi / 3.0 * i - kPi / 6.0;
            hex << QPointF(7.0 + 4.8 * std::cos(a), 7.0 + 4.8 * std::sin(a));
        }
        p.drawPolygon(hex);
        break;
    }
    }
    p.end();
    cache.insert(int(kind), pm);
    return pm;
}

void ModelOutlinerModel::setRowHeight(int h)
{
    if (h == m_rowH) return;
    m_rowH = h;
    emit layoutChanged();   // uniformRowHeights caches the first row's hint; force a re-measure
}

// ── QAbstractItemModel ───────────────────────────────────────────────────────────────────────

QModelIndex ModelOutlinerModel::index(int row, int col, const QModelIndex& parent) const
{
    if (row < 0 || col < 0 || col >= columnCount()) return {};
    if (!parent.isValid()) {
        if (row >= m_src->rowCount()) return {};
        return createIndex(row, col, nullptr);
    }
    const Node* pn = node(parent);
    if (!pn) {   // parent is a top-level row — only the host row has children
        if (parent.row() != m_hostRow || !m_root) return {};
        pn = m_root;
    }
    if (row >= pn->kids.size()) return {};
    return createIndex(row, col, pn->kids[row]);
}

QModelIndex ModelOutlinerModel::parent(const QModelIndex& child) const
{
    Node* n = node(child);
    if (!n) return {};
    Node* p = n->parent;
    if (!p || p == m_root)
        return m_hostRow >= 0 ? createIndex(m_hostRow, 0, nullptr) : QModelIndex();
    Node* gp = p->parent ? p->parent : m_root;
    return createIndex(gp->kids.indexOf(p), 0, p);
}

void ModelOutlinerModel::setFlatMode(bool flat)
{
    if (m_flat == flat) return;
    beginResetModel();   // children appear/disappear wholesale — a reset is the honest signal
    m_flat = flat;
    endResetModel();
}

int ModelOutlinerModel::rowCount(const QModelIndex& parent) const
{
    if (!parent.isValid()) return m_src->rowCount();
    if (m_flat) return 0;                 // List display mode: flat browse rows only
    if (parent.column() != 0) return 0;   // Qt convention: children hang off column 0
    const Node* n = node(parent);
    if (!n) return (parent.row() == m_hostRow && m_root) ? m_root->kids.size() : 0;
    return n->kids.size();
}

int ModelOutlinerModel::columnCount(const QModelIndex&) const
{
    return m_src->columnCount();
}

QVariant ModelOutlinerModel::data(const QModelIndex& ix, int role) const
{
    if (!ix.isValid()) return {};
    const Node* n = node(ix);
    if (!n) {   // browse row → forward
        if (role == Qt::SizeHintRole && m_rowH > 0) return QSize(0, m_rowH);
        return m_src->data(m_src->index(ix.row(), ix.column()), role);
    }
    if (ix.column() != kTreeCol) return {};   // node rows only populate the tree column
    switch (role) {
    case Qt::DisplayRole:
        return n->text;
    case Qt::DecorationRole:
        // Texture leaves get their decoded thumbnail once it arrives; everything else (and
        // thumbnails still pending) shows the Blender-style type glyph.
        return n->icon.isNull() ? kindIcon(n->kind) : n->icon;
    case Qt::CheckStateRole:
        if (n->checkable) return n->check;
        break;
    case ExportRole:
        if (n->kind == Part) return n->exportOn;
        break;
    case Qt::ForegroundRole:
        // Same palette as the GL bone-label overlay: verified translations green, raw hashes tan.
        if (n->kind == Bone)
            return n->translated ? QColor(110, 235, 165) : QColor(190, 180, 150);
        if (n->kind == Armature || n->kind == AnimRoot)
            return QColor(200, 190, 150);
        if (n->kind == Material)
            return QColor(215, 170, 120);
        break;
    case Qt::ToolTipRole:
        // Node labels elide in the tree column — the tooltip always carries the full text.
        // Parts additionally carry authoritative sub-object facts in aux (slot/hash/LOD).
        if (n->kind == Part)
            return n->text
                   + (n->aux.isEmpty() ? QString() : QStringLiteral("\n") + n->aux)
                   + QStringLiteral("\nEye = show/hide · arrow = include in export");
        return n->text;
    default:
        break;
    }
    return {};
}

bool ModelOutlinerModel::setData(const QModelIndex& ix, const QVariant& v, int role)
{
    Node* n = node(ix);
    if (!n || role != Qt::CheckStateRole || !n->checkable) return false;
    n->check = static_cast<Qt::CheckState>(v.toInt());
    emit dataChanged(ix, ix, {Qt::CheckStateRole});
    if (n->kind == Look)
        emit lookToggled(n->ref, n->check == Qt::Checked);   // ModelsTab enforces exclusivity
    else
        emit partCheckChanged();
    return true;
}

Qt::ItemFlags ModelOutlinerModel::flags(const QModelIndex& ix) const
{
    if (!ix.isValid()) return Qt::NoItemFlags;
    const Node* n = node(ix);
    if (!n) {
        Qt::ItemFlags f = m_src->flags(m_src->index(ix.row(), ix.column()));   // keeps group headers unselectable
        // CRITICAL: QAbstractTableModel::flags() bakes ItemNeverHasChildren into every valid index
        // (tables can't nest). Forwarded as-is it makes QTreeView skip hasChildren() entirely —
        // no expander, no subtree, ever. Strip it; rowCount() already says who has children.
        f &= ~Qt::ItemNeverHasChildren;
        return f;
    }
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (n->checkable && ix.column() == kTreeCol) f |= Qt::ItemIsUserCheckable;
    return f;
}

QVariant ModelOutlinerModel::headerData(int s, Qt::Orientation o, int role) const
{
    return m_src->headerData(s, o, role);
}

void ModelOutlinerModel::sort(int col, Qt::SortOrder order)
{
    m_src->sort(col, order);   // source resets; our reset forwarding + relocateHost handle the rest
}

// ── OutlinerDelegate: Blender-style right-aligned visibility eye ─────────────────────────────

QRect OutlinerDelegate::eyeRect(const QRect& rowRect)
{
    constexpr int W = 22;
    return QRect(rowRect.right() - W, rowRect.top(), W, rowRect.height());
}

// Export toggle sits immediately left of the eye — an up-out-of-the-tray arrow (the camera
// glyph read as "render/photo" and confused people).
static QRect exportRect(const QRect& rowRect)
{
    constexpr int W = 22;
    return QRect(rowRect.right() - 2 * W, rowRect.top(), W, rowRect.height());
}

static void paintExport(QPainter* p, const QRect& r, bool on)
{
    const QPointF c = QRectF(r).center();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QColor col = on ? QColor(215, 210, 195) : QColor(95, 90, 82);
    p->setPen(QPen(col, 1.4, Qt::SolidLine, Qt::RoundCap));
    p->setBrush(Qt::NoBrush);
    // Tray (open box)
    p->drawPolyline(QPolygonF({{c.x() - 5.5, c.y() + 1.0}, {c.x() - 5.5, c.y() + 5.0},
                               {c.x() + 5.5, c.y() + 5.0}, {c.x() + 5.5, c.y() + 1.0}}));
    // Up arrow leaving the tray
    p->drawLine(QPointF(c.x(), c.y() + 2.0), QPointF(c.x(), c.y() - 5.5));
    p->drawLine(QPointF(c.x(), c.y() - 5.5), QPointF(c.x() - 3.0, c.y() - 2.5));
    p->drawLine(QPointF(c.x(), c.y() - 5.5), QPointF(c.x() + 3.0, c.y() - 2.5));
    if (!on) p->drawLine(QPointF(c.x() - 6.5, c.y() + 5.5), QPointF(c.x() + 6.5, c.y() - 5.5));
    p->restore();
}

void OutlinerDelegate::paint(QPainter* p, const QStyleOptionViewItem& option,
                             const QModelIndex& index) const
{
    const QVariant check = index.data(Qt::CheckStateRole);
    if (!check.isValid()) {   // ordinary row — stock painting
        QStyledItemDelegate::paint(p, option, index);
        return;
    }
    const QVariant exp = index.data(ModelOutlinerModel::ExportRole);   // Parts only
    const int reserve = exp.isValid() ? 44 : 22;
    // Suppress the stock checkbox and shorten the text area so it never runs under the icons.
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.features &= ~QStyleOptionViewItem::HasCheckIndicator;
    opt.rect.setRight(opt.rect.right() - reserve);
    const QWidget* w = option.widget;
    (w ? w->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &opt, p, w);

    if (exp.isValid())
        paintExport(p, exportRect(option.rect), exp.toBool());

    // The eye itself, right-aligned like Blender's outliner.
    const bool on = check.toInt() == Qt::Checked;
    const QRectF r = eyeRect(option.rect);
    const QPointF c = r.center();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QColor col = on ? QColor(215, 210, 195) : QColor(105, 100, 90);
    p->setPen(QPen(col, 1.3));
    p->setBrush(Qt::NoBrush);
    // Almond outline (two arcs), pupil when visible, lid slash when hidden.
    QPainterPath lid;
    lid.moveTo(c.x() - 6.0, c.y());
    lid.quadTo(c.x(), c.y() - 5.0, c.x() + 6.0, c.y());
    if (on) {
        QPainterPath bottom;
        bottom.moveTo(c.x() - 6.0, c.y());
        bottom.quadTo(c.x(), c.y() + 5.0, c.x() + 6.0, c.y());
        p->drawPath(lid);
        p->drawPath(bottom);
        p->setBrush(col);
        p->drawEllipse(c, 1.9, 1.9);
    } else {
        p->drawPath(lid);   // closed lid only
        p->drawLine(QPointF(c.x() - 2.0, c.y() + 0.5), QPointF(c.x() - 3.5, c.y() + 3.0));
        p->drawLine(QPointF(c.x() + 0.0, c.y() + 1.0), QPointF(c.x() + 0.0, c.y() + 3.5));
        p->drawLine(QPointF(c.x() + 2.0, c.y() + 0.5), QPointF(c.x() + 3.5, c.y() + 3.0));
    }
    p->restore();
}

bool OutlinerDelegate::editorEvent(QEvent* ev, QAbstractItemModel* model,
                                   const QStyleOptionViewItem& option, const QModelIndex& index)
{
    if (index.data(Qt::CheckStateRole).isValid()) {
        const QEvent::Type t = ev->type();
        if (t == QEvent::MouseButtonRelease || t == QEvent::MouseButtonPress
            || t == QEvent::MouseButtonDblClick) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            const QPoint p = me->position().toPoint();
            if (eyeRect(option.rect).contains(p)) {
                if (t == QEvent::MouseButtonRelease) {
                    const bool on = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
                    model->setData(index, on ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
                }
                return true;   // consume all eye-area clicks (incl. dbl-click → no expand toggle)
            }
            if (index.data(ModelOutlinerModel::ExportRole).isValid()
                && exportRect(option.rect).contains(p)) {
                if (t == QEvent::MouseButtonRelease)
                    if (auto* om = qobject_cast<ModelOutlinerModel*>(model))
                        om->togglePartExport(index);
                return true;
            }
            return false;      // outside the icons: plain selection, NOT the stock left-edge toggle
        }
        // Keyboard (Space) etc. keep the stock behavior.
    }
    return QStyledItemDelegate::editorEvent(ev, model, option, index);
}

// Hover tooltips per icon (the standard delay applies — roughly a second of hover).
bool OutlinerDelegate::helpEvent(QHelpEvent* ev, QAbstractItemView* view,
                                 const QStyleOptionViewItem& option, const QModelIndex& index)
{
    if (index.data(Qt::CheckStateRole).isValid()) {
        if (index.data(ModelOutlinerModel::ExportRole).isValid()
            && exportRect(option.rect).contains(ev->pos())) {
            QToolTip::showText(ev->globalPos(),
                               QStringLiteral("Include in export — a part is exported when it is "
                                              "visible AND this arrow is on"), view);
            return true;
        }
        if (eyeRect(option.rect).contains(ev->pos())) {
            QToolTip::showText(ev->globalPos(),
                               QStringLiteral("Show / hide in the viewport"), view);
            return true;
        }
    }
    return QStyledItemDelegate::helpEvent(ev, view, option, index);
}
