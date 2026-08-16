#pragma once
#include "tabs/BrowserTab.h"
#include "index/StoreProductIndex.h"

#include <QHash>
#include <QPixmap>
#include <QString>
#include <QVector>

class QCheckBox;
class QHBoxLayout;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTextBrowser;
class QTreeWidget;
class ModelsTab;
class TexturesTab;

// ── Catalogue — the Cosmetics Shop, browsable and exportable ────────────────────────────────────
//
// One bundle at a time: what the shop called it, its lore text, its card art, and every product
// inside it grouped by kind — armour and weapons, mounts, markings, emotes, emblems, headstones,
// portals, companions, dyes.
//
// Everything here is a VIEW over StoreProductIndex; this class owns no game-data parsing. Export
// reuses the existing pipelines rather than reimplementing them: ModelsTab::bulkExport for GLBs and
// TexturesTab::bulkExportTextures for PNGs, which is the same route Bulk Extract takes, so a
// Catalogue export and a Bulk Extract export of the same SNOs produce identical files.
class CatalogueTab : public BrowserTab {
    Q_OBJECT
public:
    explicit CatalogueTab(ModelsTab* models, TexturesTab* textures, QWidget* parent = nullptr);

    void refresh() override;
    void reset() override;
    // Select a bundle by SNO, clearing whatever filter is hiding it. Used by the Models tab's
    // "Sold in" links: arriving at a tab that silently shows nothing because a season filter is
    // still set would look like the jump failed.
    void revealBundle(int storeProductSno);

    // Export menu integration. This tab has NO export buttons of its own; everything goes through
    // the app's Export menu, as in every other tab.
    //
    // The SUBJECT follows the panes: with rows selected in the includes strip or the contents tree,
    // the menu exports those rows and says so ("Export 2 models…", "Export 1 image…"); with nothing
    // selected it exports the whole bundle. So the menu never offers to export something other than
    // what is highlighted in front of you.
    bool    hasExportSelection() const override;
    void    exportSelection() override;
    void    exportSelectionToLast() override;
    QString exportNoun() const override;
    // "Export all N matching bundles" — everything the search + filters currently list, not just
    // what is highlighted. In the Export menu with everything else; this tab has no export buttons.
    bool    hasExportAllFiltered() const override;
    QString exportAllFilteredLabel() const override;
    void    exportAllFiltered() override;
    void    exportAllFilteredToLast() override;

signals:
    // Double-clicking an item asks the host to open it in the MODELS tab. Deliberately not a second
    // 3D viewport embedded here: the Models tab already has the textured viewport, the parts tree,
    // animations, the part menu and the export paths, and a half-featured copy of it would be worse
    // than a jump. MainWindow already routes this shape of request for the Textures tab.
    void revealModelRequested(int appearanceSno);
    void revealTextureRequested(int textureSno);   // "View in Textures" on a shop-art row
    // Index-build progress for the app's floating toast — the same channel ModelsTab uses, so the
    // Catalogue's cold build is announced where users already look for indexing status instead of
    // only in this tab's own count label. Empty string = finished.
    void scanStatus(const QString& msg);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;   // hover preview + Ctrl+wheel zoom
    // Qt::ToolTip carries the Qt::Window bit, so hideChildren() SKIPS the popup — parenting it to
    // `this` is not enough to take it down with the tab. Without this, hovering a row and hitting
    // Ctrl+Tab inside the dwell pops it up over a different tab.
    void hideEvent(QHideEvent* e) override;

private:
    void buildUi();
    // ── List / grid, zoom, hover ────────────────────────────────────────────────────────────────
    // ONE QListWidget switching viewMode, not two views in a QStackedWidget as the Models tab does.
    // That arrangement is why contextSnos had to learn which view a click came from: the hidden
    // view keeps its geometry and silently answers hit-tests for the wrong row. With one widget
    // there is no second geometry to get wrong.
    void  setGridView(bool on);
    void  setThumbPx(int px);          // Ctrl+wheel; per-mode, persisted
    QSize thumbBox() const;            // the size thumbnails are decoded/scaled to right now
    void  showHoverPreview(int sno);   // dwell popup: big card + the bundle's facts
    void  popupPreview(const QPixmap& scaled);   // place at the cursor, clamped to the window
    void  hideHoverPreview();
    // One builder for all three panes, so the Catalogue's menus cannot drift from each other the
    // way the app's menus did before MenuText. `bundleSno` is always set; `appSno`/`texSno` are the
    // row-specific subjects and each action is omitted when its subject is absent — the rule
    // ViewportPartMenu already follows.
    void showRowMenu(QWidget* from, const QPoint& globalPos, int bundleSno, int appSno, int texSno,
                     const QString& subjectName);
    void reloadBundleList();          // (re)apply search + filters to the index
    void rebuildFilterChips();        // one removable pill per active filter
    void updateFunnelTint();          // gold border on the funnel while any filter is set
    void saveFilterState();           // no-op unless "Remember" is on
    void restoreFilterState();        // called once, after the combos are populated
    void showBundle(int sno);         // fill the detail pane
    // Every bundle highlighted in the list. Only ever more than one with "Multi select" on.
    QVector<int> selectedBundles() const;
    // Every bundle the current search + filters list (the whole left pane), in list order.
    QVector<int> filteredBundles() const;
    // Ask for a folder once, then write every selected bundle into it (the current one if none).
    void exportBundle(bool promptDir);
    void exportAllFiltered(bool promptDir);            // …everything the filter matches, confirmed
    void exportBundleList(QVector<int> bundles, bool promptDir);   // the shared batch writer
    // What one bundle produced, so the caller can total a batch and write ONE status line.
    // `skipped` = "only new" found a previous export that already covered what was asked for.
    struct Written { int models = 0, textures = 0, icons = 0, frames = 0; bool skipped = false; };
    Written writeBundle(const StoreProductIndex::Product& b, const QString& parentDir,
                        int nth, int total);
    // What the Export menu actually runs: the selected rows when there are any, else the bundle.
    void exportSelected(bool promptDir);
    // Rows — one or many — through the same ModelsTab/TexturesTab pipelines the bundle export uses,
    // so Settings ▸ Export applies identically.
    void exportRows(const QVector<int>& appSnos, const QVector<int>& texSnos, bool promptDir);
    // What `from` currently has selected, as appearance and texture SNOs. Handles both the
    // QListWidget panes and the QTreeWidget, since one row menu serves all three.
    void selectedAssets(QWidget* from, QVector<int>& appSnos, QVector<int>& texSnos) const;
    // The detail pane's selection as a whole — strip ∪ contents. The two are kept in step by
    // syncSelection(), so this is normally the same set counted once; the union is what makes it
    // true regardless of which pane the user last clicked in.
    void currentRowSelection(QVector<int>& appSnos, QVector<int>& texSnos) const;
    // Mirror a selection between the includes strip and the contents tree. Both list the same
    // products, so highlighting a piece in one and seeing nothing in the other made the panes look
    // unrelated — and made "what will Export act on?" depend on invisible focus. Products with a
    // female and a male appearance occupy TWO strip rows and one tree row, so the mapping is
    // deliberately many-to-one. Bundle-image rows exist only in the tree and simply clear the strip.
    void syncSelection(bool fromStrip);
    // Every frame of each given atlas, written into `dir`. Gated on Settings ▸ Export ▸ Catalogue
    // export. Returns how many files were written. Shared by the bundle and row export paths so
    // both obey the setting identically.
    int exportTexFrames(const QVector<QPair<int, QString>>& textures, const QString& dir);
    // The bundle's own shop textures, best-first: the tile, then details / background / web image,
    // then the inventory card. Named by convention in group 44 — 2DUI_Bundle_HArmor_bar_stor150
    // and friends — which is where nearly every bundle actually keeps its art. The UI image
    // handles are the fallback, not the other way round.
    QVector<QPair<int, QString>> shopTextures(const QString& bundleName) const;
    QImage decodeTexture(int sno, const QString& name) const;
    // ── How the shop itself lays this art out ───────────────────────────────────────────────────
    // Measured on Bundle_HArmor_sor_stor187:
    //   2DUI_Bundle_<name>              808 x 2888,  4 frames  — the CARD sheet: two full portrait
    //                                                            cards stacked plus two badges
    //   2DUI_Bundle_<name>_details     1600 x 600,   1 frame   — a wide banner
    //   2DUI_Bundle_<name>_background  5120 x 2160,  1 frame   — the HERO art behind the shop page
    //   2DUI_Bundle_<name>_WebImage    2560 x 1080,  1 frame   — the same hero, web-sized
    //   2DInventory_Bundle_<name>       768 x 384,  12 frames  — the per-ITEM icon sheet
    // So the first version was showing an entire 808x2888 sprite sheet squeezed into 260px, which
    // is why the preview looked like two stamps. Single-frame art is used whole; multi-frame sheets
    // are cropped to a frame.
    QImage heroImage(const QString& bundleName) const;   // the big single-frame banner
    QImage cardImage(const QString& bundleName) const;   // the portrait card, cropped out of the sheet
    // Crop `name`'s largest ptFrame. Returns the whole image when it has none (or when the frame
    // rectangles are unavailable, which is the case for encrypted atlases).
    QImage largestFrame(int sno, const QString& name) const;
    // Row thumbnails, decoded a few per tick for VISIBLE rows only. 1,628 bundles × one BC-encoded
    // shop tile each is far too much to decode while building the list.
    void renderVisibleThumbs();

    // What a bundle resolves to, in the terms the export pipelines want.
    struct Resolved {
        QVector<QPair<int, QString>> models;     // appearance sno -> name  (GLB)
        QVector<QPair<int, QString>> textures;   // texture sno -> name     (PNG)
        QVector<quint32>             artHandles; // UI image handles        (PNG via IconIndex)
        QStringList                  unresolved; // products we could not map, reported not hidden
        QHash<int, int>              appsPerChild;  // child product sno -> appearances found
        QHash<int, int>              firstApp;      // child product sno -> its first appearance SNO
        // child product sno -> every appearance it resolved to, in name order. Armour resolves to
        // one per GENDER (barF_… and barM_…), which the strip lists separately so both are
        // visible and openable — the shop shows the pair too.
        QHash<int, QVector<QPair<int, QString>>> appsOf;
    };
    // The payload name, looked up on demand for products recovered from CASC (which carry a raw
    // sno and no name). GUI thread only — it touches SnoIndex's lazy name cache.
    QString payloadNameOf(const StoreProductIndex::Product& p) const;
    Resolved resolveBundle(const StoreProductIndex::Product& b) const;
    // Appearance SNOs for one transmog product, via the NAME CONVENTION. Item -> Actor ->
    // Appearance is deliberately not used: it resolves for every item and lands on the proxy body
    // mesh (all seven classes' chest items point at appearance 217477). See
    // ENCRYPTED-CONTENT-HANDOFF.md — that was measured, not assumed.
    QVector<QPair<int, QString>> appearancesFor(const QString& itemName) const;
    // The armour convention above, then the item's own name — the rule weapons, mounts, trophies
    // and companions follow. Every product kind resolves through this one function.
    QVector<QPair<int, QString>> appearancesForProduct(const QString& payloadName) const;
    // The payload actor's hPortraitImage handle, or 0. Headstones keep their shop art there and
    // nowhere else; every other kind returns 0 and is unaffected. GUI thread only (m_portrait).
    quint32 portraitFor(const QString& payloadName) const;
    mutable QHash<QString, quint32> m_portrait;   // actor name -> handle (0 = probed, has none)

    // ── Name→SNO lookups, built ONCE ────────────────────────────────────────────────────────
    // The first cut rebuilt a QHash over the whole Appearance group (67,104 entries) inside
    // appearancesFor, and appearancesFor was called twice per transmog child — 14 rebuilds and
    // ~939,000 string allocations for one eight-item bundle. The art lookup was worse: a linear
    // scan of all 141,514 Texture entries per name candidate. Since reloadBundleList runs on every
    // KEYSTROKE in the search box, that was ~2M string operations per character typed.
    void ensureNameMaps() const;
    mutable QHash<QString, QPair<int, QString>> m_appByName;   // lower name -> (sno, canonical name)
    mutable QHash<QString, QPair<int, QString>> m_texByName;
    mutable bool m_nameMapsBuilt = false;

    ModelsTab*    m_models   = nullptr;
    TexturesTab*  m_textures = nullptr;
    class QTimer* m_searchDebounce = nullptr;
    class QTimer* m_thumbTimer = nullptr;
    // MEMORY only, never written to disk — the tool keeps its on-disk footprint minimal and a
    // decoded thumbnail is cheap to regenerate. Null pixmap = tried and failed, so a bundle with
    // no art is not retried on every scroll tick.
    // Cached at the CURRENT display size, so a zoom clears it and the ~20 visible rows re-decode.
    // Caching at full resolution instead would be hundreds of MB across 1,890 bundles.
    QHash<int, QPixmap> m_thumbs;
    bool          m_exporting = false;   // guards re-entry through processEvents()
    // Lines the export pipelines would otherwise have shown in a modal box each. Collected across
    // the whole batch and summarised once — see the BatchSink note in exportBundle.
    QStringList   m_exportLog;

    class QToolButton* m_gridBtn = nullptr;
    class QLabel*      m_hoverPopup = nullptr;   // plain QLabel with Qt::ToolTip, as Models uses
    class QTimer*      m_hoverTimer = nullptr;
    // Both built once and swapped. setItemDelegate does NOT delete the outgoing one, so allocating
    // per wheel notch — which is what the Models tab does — leaks a delegate per notch.
    class QStyledItemDelegate* m_listDelegate = nullptr;
    class QStyledItemDelegate* m_gridDelegate = nullptr;
    // One-entry decode cache. Scroll-zooming the popup re-enters showHoverPreview per notch, and
    // the source can be an 11-megapixel hero — decoding it again each time is not affordable.
    QImage m_hoverImg;
    int    m_hoverImgSno = -1;
    int  m_hoverSno   = -1;
    int  m_previewPx  = 192;   // hover-popup size (scroll-adjustable, per HoverInfo)
    int  m_gridPx     = 128;   // grid tile, Ctrl+wheel, "catalogue/gridPx"
    int  m_listPx     = 96;    // list row icon height, Ctrl+wheel, "catalogue/listPx"
    bool m_gridMode   = false;

    QLineEdit*    m_search   = nullptr;
    // The funnel and its popup. Every filter below lives INSIDE the popup — the bar itself is only
    // [funnel] search [grid], as in Models, Textures and Bulk Extract.
    class QToolButton* m_filtersToggle = nullptr;
    class QFrame*      m_filterPanel   = nullptr;
    QCheckBox*    m_multiSelect = nullptr;    // several bundles at once → several bundle exports
    QComboBox*    m_kindFilter = nullptr;
    QComboBox*    m_branchFilter = nullptr;
    QComboBox*    m_seasonFilter = nullptr;   // snoAssociatedSeason, by display name
    QComboBox*    m_sortCombo   = nullptr;    // name / season / patch
    QCheckBox*    m_latestChk   = nullptr;    // only bundles new in this game build
    QCheckBox*    m_rememberChk = nullptr;    // persist search + filters + sort
    QHBoxLayout*  m_chipRow     = nullptr;    // removable active-filter pills
    bool          m_filtersRestored = false;  // restoreFilterState runs once, after the combos fill
    // Re-entry guard for the strip↔contents selection mirror: setSelected() emits, so without this
    // each pane would answer the other's echo forever.
    bool          m_syncingSel = false;
    QListWidget*  m_list     = nullptr;
    QLabel*       m_countLbl = nullptr;

    QLabel*       m_title    = nullptr;
    QLabel*       m_subtitle = nullptr;
    QLabel*       m_art      = nullptr;   // wide hero banner (_background / _WebImage)
    QLabel*       m_card     = nullptr;   // portrait card, cropped from the 2DUI sheet
    QTextBrowser* m_lore     = nullptr;
    // "INCLUDES 8 ITEMS:" — the shop's horizontal contents strip. Scanning eight items across a
    // row is far quicker than reading a three-column tree, so the strip leads and the tree stays
    // beneath it for the SNOs and the "no appearance found" diagnostics the shop has no equivalent
    // of and which you need when an export comes up short.
    QLabel*       m_stripHdr = nullptr;
    QListWidget*  m_strip    = nullptr;
    QTreeWidget*  m_contents = nullptr;
    // (No export buttons: this tab drives the app's Export menu through the BrowserTab virtuals,
    // like every other tab. Two entry points for one operation is how they drift apart.)
    QLabel*       m_status   = nullptr;

    // child product sno -> its first appearance SNO, from the last resolveBundle. Lets the
    // double-click handlers open an item in the Models tab without re-resolving.
    QHash<int, int> m_childApp;
    int m_curBundle = -1;
    bool m_uiBuilt = false;
};
