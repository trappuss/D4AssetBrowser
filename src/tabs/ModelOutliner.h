#pragma once
#include <QAbstractItemModel>
#include <QHash>
#include <QList>
#include <QPixmap>
#include <QString>
#include <QStyledItemDelegate>
#include <QVector>
#include <functional>

class SnoListModel;

// ── Blender-style outliner model for the Models tab ─────────────────────────────────────────
//
// Wraps the flat SnoListModel (67k browse rows) as a tree WITHOUT touching it — SnoListModel is
// shared by four tabs, so all tree behaviour lives here. Top level mirrors the source 1:1: same
// row numbers, same 5 columns, data/flags/headers forwarded verbatim, sort() delegated. That 1:1
// mapping is what keeps the rest of ModelsTab working unchanged: entryAt(row) is still valid for
// any index whose parent() is invalid, and the grid view can share the selection model because it
// sits on this same wrapper.
//
// Exactly ONE top-level row — the currently loaded model — owns an injected subtree (the "scene"
// content: Animations, Armature → bones, Parts → Material → Textures/Values/Shaders). The subtree
// is owned Node memory, independent of the source; when the source resets (filter/sort/regroup)
// we relocate the host row by SNO and the subtree survives. Part nodes are checkable and act as
// the part-visibility source of truth (the old PARTS pane's job).
class ModelOutlinerModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Kind { AnimRoot, Armature, Bone, Part, Material,
                TexGroup, ValueGroup, ShaderGroup,          // groups (ref = detail-tab index)
                Texture, Value, Shader,                     // leaves (ref = row in the matching table)
                LookRoot, Look,                             // looks (Look: ref = look index, EXCLUSIVE eye)
                MatTiles, TexTiles,                         // per-channel tile strips (tiles[] below)
                Anim };                                     // clip leaf (aux = clip id, ref = list row)
    static constexpr int kTilePx  = 28;   // strip tile edge (shared with the view's hit-testing)
    static constexpr int kTileGap = 2;
    static constexpr int ExportRole = Qt::UserRole + 41;   // Part rows: bool — include in .glb export
    // The hierarchy (expander, node text, checkboxes) renders in the FILENAME column — the SNO
    // column is far too narrow to carry indented labels. The view must setTreePosition(kTreeCol).
    static constexpr int kTreeCol = 2;

    struct Node {
        Kind            kind;
        QString         text;
        int             ref = -1;      // Part: primitive index · Bone: joint index ·
                                       // Material: m_matModel row · Tex/Value/Shader group: detail-tab index
        quint32         hash = 0;      // Texture leaf: the texture SNO (for lazy thumbnail decode)
        QString         aux;           // Texture leaf: the texture asset name (for lazy thumbnail decode)
        QPixmap         icon;          // per-node thumbnail; falls back to the kind glyph when null
        QVector<QPair<QString, int>> tiles;   // Mat/TexTiles: (texName, sno) per tile
        QStringList     tileLabels;           // Mat/TexTiles: short caption per tile
        int             tilesDone = 0;        // lazy-decode progress into `tiles`
        bool            checkable = false;
        Qt::CheckState  check = Qt::Checked;
        bool            exportOn = true;      // Part only: camera toggle — include in .glb exports
        bool            translated = false;   // Bone only: text is a verified hash translation
        Node*           parent = nullptr;
        QVector<Node*>  kids;
        Node(Kind k, const QString& t, int r = -1) : kind(k), text(t), ref(r) {}
        ~Node() { qDeleteAll(kids); }
        Node* add(Node* n) { n->parent = this; kids.append(n); return n; }
    };

    explicit ModelOutlinerModel(SnoListModel* src, QObject* parent = nullptr);
    ~ModelOutlinerModel() override;

    // ── Subtree management (ModelsTab drives these) ──
    void  setSubtree(int sno, Node* root);   // takes ownership; root itself is hidden, its kids show under the host row
    void  clearSubtree();
    void  setFlatMode(bool flat);            // "List" display mode: subtree suppressed, flat browse rows only
    int   subtreeRow() const { return m_hostRow; }   // -1 = host filtered out / no subtree
    Node* node(const QModelIndex& ix) const;         // nullptr for top-level (browse) rows

    QList<int> partsUnder(const QModelIndex& ix) const;         // primitive indices at/under a node
    void  partChecks(QHash<int, bool>& out) const;              // primitive → checked (eye)
    void  partExportFlags(QHash<int, bool>& out) const;         // primitive → camera toggle
    void  togglePartExport(const QModelIndex& ix);              // delegate camera click
    void  setPartChecks(const QVector<bool>& flags, bool on);   // bulk (FX/SIM/GIB); silent — caller recomputes
    void  setPartCheck(int prim, bool on);                      // single part; silent — caller recomputes
    QModelIndex indexOfPart(int prim) const;                    // viewport double-click → tree node
    void  setNodeText(Kind kind, const QString& text);          // e.g. live "▶ <clip>" on the Animations node
    void  setNodeIcon(const QModelIndex& ix, const QPixmap& pm);        // texture thumbnail arrived
    void  setNodeTileImage(const QModelIndex& ix, int tile, const QImage& img);   // compose one strip tile
    void  setExclusiveLookCheck(int ref);                       // looks: exactly one eye on at a time
    QVector<QModelIndex> iconlessTextureLeaves() const;         // decode queue: thumbnails + tile strips
    static QPixmap kindIcon(Kind kind);                         // Blender-style type glyph (drawn, no assets)
    void  relabelParts(const std::function<QString(int)>& labelFor);   // look change renames parts in place
    void  setRowHeight(int h);                                  // top-level row height (tracks the icon size)

    // ── QAbstractItemModel ──
    QModelIndex index(int row, int col, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int         rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int         columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant    data(const QModelIndex& ix, int role = Qt::DisplayRole) const override;
    bool        setData(const QModelIndex& ix, const QVariant& v, int role) override;
    Qt::ItemFlags flags(const QModelIndex& ix) const override;
    QVariant    headerData(int s, Qt::Orientation o, int role) const override;
    void        sort(int col, Qt::SortOrder order) override;

signals:
    void partCheckChanged();          // a part eye-toggle changed (user click) → recompute GL visibility
    void lookToggled(int ref, bool on);   // a Look eye clicked — ModelsTab enforces exclusivity + applies

private:
    void relocateHost();       // source rows changed → find the host row again by SNO
    void collectParts(const Node* n, QList<int>& out) const;

    SnoListModel* m_src;
    Node*         m_root    = nullptr;   // owned container; kids are the visible child rows
    int           m_hostSno = -1;
    int           m_hostRow = -1;
    int           m_rowH    = 0;
    bool          m_flat    = false;   // List mode: report no children (subtree kept, just hidden)
};

// Delegate that renders part visibility as a Blender-style right-aligned EYE instead of the stock
// checkbox: open eye = visible, closed lid = hidden. Applies only to indexes carrying a
// CheckStateRole (the outliner's part nodes); every other row paints normally. Clicking the eye
// toggles it; clicks elsewhere in the row select as usual (the stock left-edge checkbox hit zone
// is disabled so text clicks can't accidentally toggle visibility).
class OutlinerDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    bool editorEvent(QEvent* ev, QAbstractItemModel* model, const QStyleOptionViewItem& option,
                     const QModelIndex& index) override;
    bool helpEvent(QHelpEvent* ev, QAbstractItemView* view, const QStyleOptionViewItem& option,
                   const QModelIndex& index) override;   // per-icon tooltips (eye vs export)
private:
    static QRect eyeRect(const QRect& rowRect);
};
