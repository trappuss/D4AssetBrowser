#pragma once
#include "tabs/BrowserTab.h"
#include "tex/TexMeta.h"

#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QSet>
#include <QVector>

class QLineEdit;
class QComboBox;
class QCheckBox;
class QTableView;
class QListView;
class QTreeView;
class QPlainTextEdit;
class SnoListModel;
class QStandardItemModel;
class QLabel;
class QPushButton;
class QToolButton;
class QFrame;
class QStackedWidget;
class QTabWidget;
class QScrollArea;
class QGridLayout;
class QVBoxLayout;
class QTimer;
class QModelIndex;
class GLTextureWidget;

// Textures module: lists SNO group 44 (from SnoIndex). Selecting a texture reads
// its descriptor from d4data (<name>.tex.json) and its BC payload from CASC, then
// uploads the top mip to the GPU (GLTextureWidget) — d4analyzer's instant preview.
// Layout mirrors the Python tool: left filters/list, middle info+export+preview
// (with channel isolation), right TEXFRAMES / ASSOCIATED MODELS / MIPMAPS panels.
class TexturesTab : public BrowserTab {
    Q_OBJECT
public:
    explicit TexturesTab(QWidget* parent = nullptr);
    void refresh() override;
    void reset() override;
    // Bulk extractor entry point: decode + save each matched (sno,name) texture into dir as PNG/JPG
    // (per Settings ▸ Export tex format). onlyNew skips already-present files / _bulk_manifest.json entries.
    void bulkExportTextures(const QVector<QPair<int, QString>>& items, const QString& dir, bool onlyNew,
                            const struct BatchSink* sink = nullptr);
    // ── The ONLY correct way to get a texture's dimensions/format ──────────────────────────────
    // (moved to public so the Catalogue tab can crop atlas frames without re-rolling the lookup.)
    // Reading Texture/<name>.tex.json directly, as nine call sites in this file used to, silently
    // excludes every ENCRYPTED texture: d4data has no JSON for them, so meta.valid stayed false
    // and the tab showed nothing. CASC's bulk texture tables carry the same fields and DO cover
    // encrypted textures — see TextureDefTable. Static so worker lambdas can call it.
    //
    // NOTE: the CASC fallback fills width/height/format only. `frames` (ptFrame) is not in those
    // tables, so an encrypted atlas resolved this way decodes whole and cannot be sliced.
    static TexMeta texMetaFor(CascReader* rd, const QString& d4, const QString& name, int sno);
    // ── The ONLY correct way to get an atlas's FRAME RECTANGLES ────────────────────────────────
    // Three routes, in the order this tab uses them:
    //   1. ptFrame from the .tex.json (texMetaFor) — absent for anything newer than the snapshot;
    //   2. the game's own 2D_table.dat for the frame COUNT and handles;
    //   3. alpha-gutter segmentation of the decoded atlas for the actual rectangles, because
    //      2D_table does NOT carry UVs.
    // `decoded` is the whole atlas image; pass a null QImage to get route 1 only.
    //
    // Static and public because the Catalogue exports atlas frames too, and its first attempt
    // reimplemented this as "ask IconIndex for each handle" — which returns nothing for an atlas
    // IconIndex has not indexed, so seasonal bundles exported zero frames while this tab showed
    // them correctly. One implementation, two callers.
    static QVector<TexFrame> atlasFramesFor(CascReader* rd, const QString& d4, const QString& name,
                                            int sno, const QImage& decoded);
    // Texture category filter, shared with the Bulk Extract tab so its funnel can offer the same
    // texture-appropriate facets the Textures tab uses. `bulkTexCategories()` lists the name-based
    // categories (no "Latest" — that's SNO/isNew-based, handled by the caller); `bulkTexInCategory`
    // tests a LOWER-CASED texture name against one of them.
    static QStringList bulkTexCategories();
    static bool bulkTexInCategory(const QString& lowerName, const QString& cat);
    void onSettingsChanged() override;   // re-sync the trim toggle with export settings
    // Cross-tab navigation (Ctrl+K palette / back-forward history).
    int  currentSno() const { return m_currentSno; }
    void selectBySno(int sno);           // find the row, select + scroll to it (loads the preview)

    // Export-menu hooks (see BrowserTab). No 3D preview, so previewWidget() stays null.
    bool hasExportSelection() const override;
    void exportSelection() override;         // → exportSelected(false) (textures)
    void exportSelectionToLast() override;   // → exportTexture(true) (last folder)
    QString exportNoun() const override;     // "selected texture(s)"
    bool hasFrameExport() const override;    // true when the selected texture has frames
    void exportFramesSelected() override;        // → exportSelectedFrames(false, false)
    void exportFramesSelectedToLast() override;  // → exportSelectedFrames(false, true)
    void exportFramesAll() override;             // → exportSelectedFrames(true, false)
    void exportFramesAllToLast() override;       // → exportSelectedFrames(true, true)

signals:
    void revealModelRequested(int appSno);   // double-clicked an .app in Associated Models
    void exportSettingsRequested();           // Options… → open the shared Export settings dialog

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;   // gallery cell click/hover + resize

private:
    QWidget* buildLeft();
    QWidget* buildMiddle();
    QWidget* buildRight();
    void loadList();
    void onSelectionChanged();
    void showTexture(int sno, const QString& name);
    void setInfo(const QString& key, const QString& value);
    void updateSelLabel();
    void applyNameFilter();         // unified predicate: name/#tag/SNO + tags + format + orphan
    void applyListDensity();        // compact text-only rows, matching the Models tab's List mode
    QFont m_listBaseFont;           // the view's font before compacting — never shrink twice
    void refillTagPanel();          // (re)populate the funnel's tag-group checkboxes
    void updateFunnelTint();        // gold-tint the funnel while any filter is active
    void rebuildFilterChips();      // inline removable active-filter pills
    void applySort();               // Name / SNO / Size / Dimensions
    void ensureFmtIndex();          // background scan of tex.json → format + dims
    QString texBlob(int sno);       // cached lowercased "tags/titles" blob for a texture
    void setChannel(int mode);          // 0=RGB, 1=R, 2=G, 3=B, 4=A
    void onPreviewHover(QPointF uv);    // pixel inspector readout
    void startImageDrag(const QImage& img, const QString& baseName);   // drag-to-export
    void exportTexture(bool toLast);
    void exportSelected(bool frames, bool toLast = false);   // batch textures/frames to a chosen/last dir
    QImage decodeTexture(int sno, const QString& name);
    // ── The ONLY correct way to get a texture's dimensions/format in this tab ──────────────────
    // Reading Texture/<name>.tex.json directly, as nine call sites here used to, silently excludes
    // every ENCRYPTED texture: d4data has no JSON for them, so meta.valid stayed false and the tab
    // showed nothing — no thumbnail, no preview, no bulk export, and a format filter that could not
    // see them. That is the entire 2D icon set for collab/store content (2DInventory_Bundle_HArmor_
    // bar_stor251 and its siblings), which d4analyzer exports without trouble.
    //
    // CASC's bulk texture tables carry the same fields and DO cover encrypted textures — see
    // TextureDefTable, whose per-key overlay is gated by the same TACT key as the pixels. This is
    // the identical mistake MaterialDecode::texturesFor was written to end for materials; the
    // texture tab simply never got the same treatment. Static so worker lambdas can call it.

    void onFrameSelected();
    void exportSelectedFrames(bool all = false, bool toLast = false);   // all→every frame; toLast→remembered dir
    QImage croppedFrame(int row, bool trim);
    void uploadFaceMip();               // slice m_curPayload by face → preview (top mip)
    void populateAssociated(int sno);   // texture ← material ← appearance tree
    void onAssocDoubleClick(const QModelIndex& index);
    void showAssocMenu(const QPoint& pos);   // right-click: reveal/copy
    void buildGallery();                // cropped-frame thumbnail grid (TexFrames tab)
    void buildChannelStrip(QVBoxLayout* lay);   // 6-tile TEXTURE PREVIEW strip
    void setChannelTile(int idx, const QImage& img);
    void setTileCaptions(const char* const labels[6]);
    void clearChannelStrip();
    void populateChannelView();         // fill tiles per the view dropdown
    void refreshChannelCombo(int texSno);   // "RGBA" + associated materials
    QImage decodeTexCpu(int sno);       // CPU BcDecode by SNO (no GPU preview touch)
    void popupPreview(const QPixmap& scaled);   // floating zoom popup at the cursor
    void showTilePreview(int idx);              // hover popup of channel tile idx
    void hideTilePreview();

    // ── Left: filters + list ──
    QLineEdit*    m_search     = nullptr;   // one box: name/#tag; a pure-digit query = SNO
    QComboBox*    m_sortCombo  = nullptr;   // Name / SNO / Size / Dimensions
    QCheckBox*    m_orphanCheck = nullptr;  // only textures with no material/model
    QCheckBox*    m_onlyDecrypted = nullptr;
    QCheckBox*    m_onlyEncrypted = nullptr;  // the inverse: only TACT-gated textures
    QCheckBox*    m_rememberFilters = nullptr;   // persist + restore the whole filter state
    void saveTexFilterState();
    void restoreTexFilterState();
    QHash<int, int>    m_texFmt;            // sno → eTexFormat (background-built)
    QHash<int, quint32> m_texDim;           // sno → (w<<16 | h)
    bool          m_fmtReady   = false;
    bool          m_fmtBuilding = false;
    QCheckBox*    m_multiSelect   = nullptr;
    // ── Funnel popup (Models/Wardrobe-style grouped tag checkboxes, texture-relevant) ──
    QToolButton*  m_filtersToggle = nullptr;   // the funnel icon button (opens the popup)
    QFrame*       m_filterPanel   = nullptr;   // the popup (search · OR · toggles · format · tags)
    QWidget*      m_tagPanelBody  = nullptr;   // scroll body: grouped tag checkboxes
    QLineEdit*    m_tagSearch     = nullptr;   // "Search tags…" — live-filters the checkbox list
    QCheckBox*    m_tagOrChk      = nullptr;   // match ANY (OR) vs ALL (AND) selected tags
    QSet<QString> m_tagFilter;                 // selected appearance-derived tags
    QSet<QString> m_catFilter;                 // selected texture categories (2D UI / Items / Map / …)
    QSet<QString> m_fmtFilter;                 // selected texture formats (BC1/3/4/5/7); empty = any
    bool          m_tagOrMode     = false;
    QHash<QString, QCheckBox*> m_tagChecks;    // tag value → checkbox (Clear / restore)
    QHash<QString, QCheckBox*> m_catChecks;    // category → checkbox
    QHash<QString, QCheckBox*> m_fmtChecks;    // format → checkbox
    QWidget*      m_filterChips   = nullptr;   // inline removable active-filter pills
    QLabel*       m_selLabel   = nullptr;
    QTableView*   m_view       = nullptr;
    SnoListModel* m_model      = nullptr;
    // ── Grid view (thumbnail layout, mirrors the Models tab) ──
    QStackedWidget* m_browserStack = nullptr;   // 0 = table (m_view), 1 = grid (m_grid)
    QListView*      m_grid      = nullptr;
    QToolButton*    m_gridBtn   = nullptr;
    QSet<int>       m_gridPending;               // snos with a decode in flight (dedupe)
    int             m_gridPx     = 88;           // grid tile icon size (Ctrl+wheel resizes; persisted)
    int             m_hoverGridSno = 0;          // grid/list row under the cursor (0 = none)
    bool            m_hoverIconArea = false;     // cursor is on an ICON (grid cell) → include the image
    int             m_gridPreviewSno = -1;       // sno of the cached hover-preview image
    QImage          m_gridPreviewImg;            // decoded image for the hover popup (rescaled on wheel)
    QTimer*         m_gridScrollTimer = nullptr; // debounce: request thumbs only after scrolling settles
    void  setGridView(bool on);
    void  setGridIconPx(int px);                 // resize grid tiles (Ctrl+wheel), persisted
    void  showGridPreview(int sno);              // dwell → scaled, bounds-aware popup
    QPixmap gridThumb(int sno);                  // icon provider — cache-only, never enqueues
    void  requestGridThumb(int sno);             // enqueue a background decode (cache + dedupe)
    void  queueVisibleGridThumbs();              // decode ONLY the cells currently in view
    void  onGridThumbReady(int sno, const QImage& img);   // worker→GUI: QImage in, QPixmap made here
    void  showBrowserMenu(class QAbstractItemView* view, const QPoint& viewportPos);   // table+grid

    // ── Middle: info + export + preview ──
    QHash<QString, QLabel*> m_infoVals;
    int           m_selFrame    = -1;        // selected texframe (highlighted in gallery)
    int           m_galleryWidth = -1;       // last viewport width the gallery laid out at
    // (Texture / TexFrame export lives in the top menu-bar Export menu — no tab-local export button.)
    QTabWidget*   m_previewTabs    = nullptr;
    QScrollArea*  m_galleryScroll  = nullptr;
    QStackedWidget* m_texStack     = nullptr;
    GLTextureWidget* m_preview     = nullptr;
    QLabel*       m_chanLabel      = nullptr;   // isolated-channel grayscale view
    QPushButton*  m_chanBtns[5]    = {};
    QPushButton*  m_checkerBtn     = nullptr;   // alpha checkerboard toggle
    QComboBox*    m_faceCombo      = nullptr;   // cubemap face (faceCount>1)
    QLabel*       m_pixelLabel     = nullptr;   // pixel inspector readout
    int           m_channel        = 0;
    QPoint        m_dragStartPos;               // press point for drag-to-export
    QByteArray    m_curPayload;                 // cached CASC payload (face slicing)
    int           m_curFace        = 0;
    QComboBox*    m_chanViewCombo  = nullptr;   // RGBA / per-material PBR view selector
    QLabel*       m_chanImg[6]     = {};        // 6-tile TEXTURE PREVIEW strip
    QLabel*       m_chanCap[6]     = {};
    QImage        m_chanFull[6];                // native-res tile images (copy/save)
    QVector<int>  m_chanMatSnos;                // combo index>0 → material SNO
    QLabel*       m_iconPreview    = nullptr;   // floating hover-zoom popup
    QTimer*       m_hoverTimer     = nullptr;   // 0.5s dwell before the popup
    int           m_hoverTile      = -1;        // channel tile under the cursor
    int           m_previewPx      = 256;       // hover-popup size (wheel-adjustable)

    // ── Right: texframes + associated + mipmaps ──
    QLabel*             m_tfTitle    = nullptr;
    QStandardItemModel* m_framesModel = nullptr;
    QTreeView*          m_frames      = nullptr;
    QCheckBox*          m_trimCheck   = nullptr;
    QPushButton*        m_exportFrame = nullptr;
    QLabel*             m_assocTitle  = nullptr;
    QStandardItemModel* m_assocModel  = nullptr;
    QTreeView*          m_assocView   = nullptr;
    QPlainTextEdit*     m_texLog      = nullptr;   // debug console (load / decode diagnostics)
    void                logTex(const QString& msg, bool err = false);   // append a timestamped line

    QHash<int, QString> m_snoName;        // lazy sno → name (for the associated tree)
    QHash<int, QString> m_texBlob;        // cached sno → reverse-link tag/title blob
    QImage            m_fullImage;        // cached decoded atlas (top mip)
    QVector<TexFrame> m_frameDefs;
    TexMeta           m_curMeta;
    QString           m_currentName;
    int               m_currentSno = -1;
    bool              m_loaded  = false;
};
