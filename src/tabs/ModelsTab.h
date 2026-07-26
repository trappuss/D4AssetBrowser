#pragma once
#include "model/ModelGeometry.h"
#include "model/AnimParser.h"
#include "model/Attachments.h"
#include "tabs/BrowserTab.h"

#include <array>
#include <QCache>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QModelIndex>
#include <QPair>
#include <functional>
#include <QPixmap>
#include <QPoint>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

class QAction;
class QLineEdit;
class QMenu;
class QTableView;
class QTreeView;
class QStandardItemModel;
class SnoListModel;
class ModelOutlinerModel;
class QLabel;
class QPushButton;
class QToolButton;
class QTimer;
class QListWidget;
class QListView;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QComboBox;
class QCheckBox;
class QShortcut;
class QSlider;
class QSpinBox;
class QSplitter;
class QTabWidget;
class QFrame;
class QVBoxLayout;
class GLTextureWidget;
class GLModelWidget;
class QScrollArea;
class PanelBox;   // right-column stacking panel, shared with the Wardrobe tab (PanelBox.h)

// (kHdrQss / kSubHdrQss now live in BrowserTab.h — every tab's panels share them.)

// Models module: the full appearance → material → texture → GPU-preview chain.
//   • group 9 list (left)
//   • selected appearance → looks + material bindings
//   • selected material → its texture bindings (Role | Slot | Texture | SNO)
//   • selected texture → BC GPU preview (reusing GLTextureWidget)
//   • "Export .glb" → ModelParser (native .app geometry) → ModelExporter (.glb)
// (A 3D mesh viewport is the remaining model milestone; it consumes the same
// ModelGeometry the exporter does. Skinning/textures in the .glb are follow-ups.)
class ModelsTab : public BrowserTab {
    Q_OBJECT
public:
    explicit ModelsTab(QWidget* parent = nullptr);
    void refresh() override;
    void reset() override;
    void onSettingsChanged() override;   // re-sync the Tex/Anim toggles with export settings
    void selectModelBySno(int sno);   // cross-tab jump: select + load a model
    void forceLoadSno(int sno);       // double-click: load now, even if the row is already current
    int  currentSno() const { return m_curSno; }   // for the Ctrl+K palette / nav history
    // Bulk extractor entry point: export a matched (sno,name) set to dir, reusing the batch pipeline.
    // onlyNew skips items already present in dir (by <name>.glb or the _bulk_manifest.json ledger).
    void bulkExport(const QVector<QPair<int, QString>>& items, const QString& dir, bool onlyNew,
                    const struct BatchSink* sink = nullptr);

    // ── Filter service (shared with the Bulk Extract tab so its filters are IDENTICAL) ──────────
    // A snapshot of a filter selection, evaluated against the model/texture index by queryEntries().
    struct FilterSpec {
        QString category;       // m_catCombo data: a real category tag, or a usage facet
                                // ("__animated__" / "__rigged__" / "__orphaned__")
        QString classCode;      // "bar"… ; empty = any
        QString gender;         // "f"/"m"; empty = any
        QString typeVal;        // authoritative type tag once meta-ready, else a name token
        QString collection;     // collection substring
        QString nameSearch;     // NAME box text: space = AND include, leading '-' = exclude
        QSet<QString> tagSel;   // funnel multi-tag selection
        bool tagOr = false;     // false = ALL selected tags (narrow) · true = ANY (widen)
        bool hideUnrenderable = false;
    };
    // The live dropdown contents (label,data) so another tab can build IDENTICAL combos — these
    // track the meta-ready state (class/gender are curated + stable; category/type follow the tags).
    QVector<QPair<QString, QString>>     filterCategoryItems() const;
    QVector<QPair<QString, QString>>     filterClassItems() const;
    QVector<QPair<QString, QString>>     filterGenderItems() const;
    QVector<QPair<QString, QString>>     filterTypeItems() const;
    QVector<QPair<QString, QStringList>> filterTagGroups() const;   // funnel panel: group → tag values
    void ensureFilterIndexes(const QString& category);             // kick anim/rig/entity as needed
    // Authoritative match: every entry of `group` (9 models · 44 textures) passing `f`, as (sno,name).
    QVector<QPair<int, QString>> queryEntries(int group, const FilterSpec& f);
    // Public wrapper so other tabs (Bulk Extract) can open the read-only dependency tree.
    void showModelDependencies(int sno, const QString& name);

    // Export-menu hooks (see BrowserTab).
    GLModelWidget* previewWidget() override;
    bool hasExportSelection() const override;
    void exportSelection() override;         // → exportSelectedGlb() (prompts)
    void exportSelectionToLast() override;   // batch-export the selection to models/lastExportDir
    QString exportNoun() const override;     // "selected model(s)"
    bool hasAnimExport() const override;     // rig loaded + clips available
    QString animExportLabel() const override;// "Export animation(s) only…" (count-aware)
    void exportAnimations() override;        // → exportAnimationsOnly()
    void startModelDrag();                   // drag selected model(s) out as temp .glb files
    QPoint m_dragPressPos;                   // press position (drag threshold)
    bool   m_dragPrimed = false;             // pressed on an already-selected top-level row

signals:
    void scanStatus(const QString& msg);   // merged background-scan status → the app's floating toast
    void filtersChanged();                 // combos / tag groups were rebuilt (meta ready) → mirrors re-sync

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;   // hover-hide tile captions
    void showEvent(QShowEvent* ev) override;               // first show → render in-view 3D icons

private:
    void loadList();
    void onAppearanceSelected();
    void onMaterialSelected();
    void onMatTexSelected();
    void showAppearance(int sno, const QString& name);
    void showMaterialTextures(const QString& materialName);
    void highlightMaterialsForLook(int look);   // tint material rows for SOA index = look
    void highlightMaterialsForParts(const QList<int>& parts);   // tint rows for parts' materials
    QList<int> selectedParts() const;            // primitives of all selected outliner nodes
    void buildOutlinerSubtree();                 // after load: hang anim/armature/parts off the model row
    void onOutlinerNodeSelected(const QModelIndex& ix);   // subtree node → drive the detail panels
    void updateBreadcrumb();                     // model › part › material path above the properties column
    void queueTextureIcons();                    // lazy thumbnail decode for the outliner's texture leaves
    QLabel* addRightPage(const QString& title, QWidget* content, const QPixmap& icon,
                         const QString& tip);    // register a page + its strip button ("" title = none)
    void showOutlinerTexPreview();               // hover popup for outliner texture leaves
    void applyDisplayMode(int mode);             // 0 List · 1 Outliner · 2 Grid (header dropdown)
    void applyListDensity();                     // List-only: hide icons, tighten rows + font
    void updateTagButtonTint();                  // funnel turns red while a tag filter is active
    QPoint panelPosLeftOf(QWidget* anchor, const QSize& sz) const;   // N-strip popups open LEFTward
    void refreshHistPopup();                     // (re)fill + place + show/hide the history dropdown
    void saveFilterState();                      // persist search + tags + combos (opt-in)
    void restoreFilterState();                   // re-apply them at launch
    void previewTexture(const QString& texName, int texSno);
    // TEXTURE PREVIEW channel strip (COLOR/ROUGHNESS/METAL/NORMAL/ALPHA/EMISSIVE).
    QImage decodeTexImage(const QString& texName, int texSno) const;  // BC → RGBA8888
    QImage baseColorForMaterial(const QString& matName);   // material → decoded BASE_COLOR
    QImage normalForMaterial(const QString& matName);      // material → decoded NORMAL
    QImage ormForMaterial(const QString& matName);         // pack rough(G)/metal(B)/ao(R)
    QImage emissiveForMaterial(const QString& matName);    // material → decoded EMISSIVE
    QImage textureByRole(const QString& matName, const char* role);   // first tex of a role
    bool   materialHasRole(const QString& matName, const char* role); // role present?
    QString bodySkinMaterial();              // class body-skin mat for "fill skin" (cached/prefix)
    QHash<QString, QString> m_bodySkinByPrefix;   // "barf" → "barF_P00_BOD" ("" = none found)
    void   buildPreviewPanel();          // construct the Graphics popup (grouped, Wardrobe-parity)
    void   togglePreviewPanel();         // show/hide it under the button
    void   buildDyePanel();              // construct the Pigment (dye-zone) popup
    void   toggleDyePanel();             // show/hide it under the Pigment button
    void   applyModelRig();              // push saved models/rig/* flags to m_modelView
    void   reapplyOverlays();            // re-push ALL overlay state (master gate + each box)
    // Shared row context-menu builders — list AND thumbnail grid compose from these,
    // so the two views can never drift apart again.
    void   addRowImageActions(QMenu& menu, const QList<int>& snos);
    void   addRowExportCopyActions(QMenu& menu, const QList<int>& snos);
    // ── Viewport control panels — parity with the Wardrobe preview toolbar ──
    void   buildCameraPanel();     void toggleCameraPanel();     // FOV · angles · turntable · presets
    void   buildLightingPanel();   void toggleLightingPanel();   // three-point rig + surface + shadows
    void   applyLightRig();        // push models/light/* → m_modelView
    void   buildShaderPanel();     void toggleShaderPanel();     // shell-fur + mesh-FX (dev)
    void   buildDetailPanel();     void toggleDetailPanel();     // detail-map discovery tool (dev)
    void   applyDetailConfig();    QString detailConfigText() const;
    void   buildPhysicsPanel();    void togglePhysicsPanel();    // live cloth-physics tuning (dev)
    void   applyClothParams();     // push models/cloth/* → m_modelView
    void   toggleFullscreen();     // maximize-in-place: hide chrome, viewport fills the tab (Esc/F)
    void   applyViewportDevGating();   // (dev mode retired) all debug panels stay visible
    void   loadDyeRegionToPicker(int r); // active region colour → wheel / hex / swatch
    void   applyDyePicker();             // wheel → active region colour (live)
    void   setDyeSlotColor(int r, const QColor& c);   // set a slot (drop / memory / reset)
    void   styleDyeSlot(int r);          // restyle a slot (colour + used/unused)
    void   updateDyeSlotUsage();         // mark which slots the current model uses
    void   dumpDyeDebug();               // decode + visualize DyeMask/DyeRamp values
    void   rebuildDyeCombo();            // repopulate: Custom + real dyes + saved pigments
    void   recomputePartVisibility();   // combine parts-tree checks with FX/SIM toggles
    void   setFlaggedPartsChecked(const QVector<bool>& partFlags, bool checked);   // FX/SIM/GIB toggle → tree checks
    void   clearAnimationSelection();   // stop + clear the playing animation/selection
    void   applyPartMaterials();        // decode + push per-part textures for m_appMatNames
    void   applyLook(int look);         // switch the active SOA/look and re-texture
    QString partLabel(int primIndex) const;   // parts-tree row label for a primitive
    void   setChannelTile(int idx, const QImage& img);   // scaled into tile idx
    void   setTileCaptions(const char* const labels[6]); // relabel the six tiles
    void   clearTexturePreview();                        // blank all 6 tiles
    void   showMaterialChannels();                       // fill tiles from m_matTexModel
    void loadGeometry();
    void applyLoadedGeometry(std::shared_ptr<ModelGeometry> geo, int token);   // worker → UI
    // ── Attachments: the other models an actor holds/spawns (weapons, shields, props) ──
    void buildAttachPage();          // build the right-side "Attachments" tree panel
    void resetAttachments();         // clear panel + base snapshot (new/failed load, tab reset)
    void scanAttachments();          // background scan of the current appearance's actors
    void populateAttachTree();       // fill the tree from m_attachAll (grouped actor → trigger)
    void onAttachItemChanged(QTreeWidgetItem* item, int col);   // checkbox → attach/detach
    void rebuildAssembledGeometry(); // base + active attachments → seated + textured in the viewport
    void loadDeferredMeta();
    void exportSelectedGlb();
    void showDependencies(int sno, const QString& name);   // model → materials → textures tree
    void setInfo(const QString& key, const QString& value);
    void applyCategoryFilter();   // class/gender/type → model (tag-based when ready)
    QString modelSearchBlob(int sno);   // cached tags/collection/title text for the name-box search
    void ensureAnimatedIndex();   // lazily background-scan Anim/*.ani.json → set of animated SNOs
    void ensureEntityIndex();     // lazily background-scan Actor/*+Item/* → used-by / family / items
    void updateEntityInfo(int sno);   // fill the "Family / Used by / Items" info rows for a model
    void ensureRigIndex();        // cheap: family prefixes of every *_base<NN> appearance (the "Rigged" filter)
    // A model's playable clips = the ones it directly owns PLUS the ones its shared base rig owns
    // (resolved by name family). Used by the per-model ANIMATIONS panel and the Animatable filter.
    // Fill the ANIMATIONS list, colouring clips inherited from a shared base rig differently
    // (with a tooltip naming the base) so it's obvious which clips aren't the model's own.
    void        populateAnimList(int sno, const QString& nameLower);
    // Phase-2: match a loaded model's skeleton (bone-name hashes) to EVERY base-rig family whose
    // clips it can play (overlap >= minScore, fraction of the smaller bone set). Rig-siblings with
    // unrelated names (cmp_base000_quillrat / wildlife_quillrat_A / S14_wildlife_quillrat) all share
    // one skeleton, so their clip sets are unioned rather than picking a single best family.
    QStringList animFamiliesBySkeleton(const QVector<ModelJoint>& skeleton, double minScore) const;
    void applyGrouping();         // install the list group-key from the Group-by combo
    void onMetaReady();           // AppearanceMeta finished: authoritative Type dropdown
    void updateIconMode();        // install the mode-aware list icon provider
    QPixmap originalIcon(int sno);// decode+cache the 2D inventory icon (null if none)
    QPixmap listIconPixmap(int sno);  // mode-aware icon (original / render) at native res
    void renderIcons(const QList<int>& snos, bool force, bool quiet = false);   // render/re-render thumbs
    void scheduleVisibleIconRender();   // debounce → renderVisibleIcons() (3D/both modes)
    void renderVisibleIcons();          // auto-render thumbs for rows currently in view
    void recoverFromRenderCrash();      // startup: blocklist+revert if a render crashed last run
    void renderGuardStage(int sno, const QString& name, const char* stage);  // write crash sentinel
    void endRenderGuard();
    // Runtime recovery: a hardware fault (access violation) was CAUGHT mid-load/-render.
    // Quarantine the model, clear the guard, log it, and show a hint — all without
    // killing the process. `stage` is a short label for the log/overlay.
    void handleModelFault(int sno, const QString& name, const QString& stage);
    QList<int> contextSnos(const QPoint& viewportPt) const; // selection, or row under cursor
    void copyIconImage(int sno);                            // icon → clipboard
    void saveIconImages(const QList<int>& snos, bool chooseDir);   // icon(s) → PNG
    void saveTileImage(int tile, bool chooseDir);           // texture-preview tile → PNG
    void exportModels(const QVector<QPair<int, QString>>& models, const QString& dir,
                      const struct BatchSink* sink = nullptr,
                      QStringList* failures = nullptr);   // "name — reason" per failure
    void setListIconSize(int px); // icon size + matching row height + column width
    void setGridThumbPx(int px);  // Grid thumbnail size (Ctrl+scroll), persisted
    void showColumnMenu(const QPoint& globalPos);   // table column show/hide menu (header + Columns button)
    void setGridView(bool on);    // switch the list between table and thumbnail-grid layouts
    void showToast(const QString& msg);   // brief, non-blocking bottom-centre notification
    void showIconPreview(int sno);// hover popup of the icon under the cursor
    void showTilePreview(int idx);// hover popup of a texture-preview tile
    void popupPreview(const QPixmap& scaled);   // show + clamp inside the app window
    void hideIconPreview();
    void playAnimationByName(const QString& animName);   // load + decode + start
    void playAnimationSet(const QStringList& clips);     // queue clips → play back-to-back
    void openItemBrowser();                              // pick a gear item → jump to its model
    void pullAnimsFromModel();                           // pick another model → also list its anims here
    void pullSuggestedAnims();                           // auto-pull from this model's own base rig
    int  suggestedAnimSource(QString* why = nullptr) const;   // base-rig SNO for the current model, or -1
    QString apprNameForSno(int sno) const;               // appearance name for a SNO (cached)
    QStringList clipFamiliesFor(int sno, const QString& nameLower) const;   // families a model may play
    // The rows a model owns/plays. Used for the panel AND for "Pull from…", so a pull yields
    // exactly what the source model shows.
    QStringList modelAnimRows(int sno, const QString& nameLower, bool* fallbackOut = nullptr) const;
    mutable QHash<int, QString> m_apprNameCache;         // sno → appearance name (built on first use)
    bool m_suppressSkelFallback = false;                 // "Clear pulls" → own clips only; per-model
    void showVariantsMenu();                             // popup of the current model's skin variants → jump
    AnimParser::DecodedAnim decodeAnimByName(const QString& animName) const;   // decode only (no UI)
    AnimParser::DecodedAnim decodeAnimForSkeleton(const QString& animName,
                                                  const ModelGeometry& geo) const;   // batch variant
    QStringList animClipsFor(int sno, const QString& nameLower) const;   // own + inherited clip names
    void collectExportAnims(QVector<AnimParser::DecodedAnim>& anims, QStringList& names) const;  // export scope
    void exportAnimationsOnly(bool toLast = false);   // rig + selected clips only (no mesh); toLast → remembered dir
    void tickAnimation();         // advance the playback frame
    void applyAnimSpeed();        // timer interval from fps × speed-combo factor
    void updateCount();           // "N models" label
    void rebuildFilterChips();    // inline removable active-filter pills
    void updateIndexStatus();     // clear the transient (load/render) scan messages
    void setScan(const QString& key, const QString& msg);   // set/clear a scan message → emit merged status
    void updateTabCounts();       // live (N) on each material/texture detail tab

    // ── Left column: filters + list ──
    QLineEdit*             m_search    = nullptr;   // Name / #tag
    QLineEdit*             m_snoSearch = nullptr;
    QLineEdit*             m_collSearch = nullptr;   // COLLECTION substring filter
    QComboBox*             m_catCombo   = nullptr;   // entity Category (Actor-derived)
    QComboBox*             m_groupCombo = nullptr;   // "Group by" (collection/set/…)
    QComboBox*             m_classCombo = nullptr;
    QComboBox*             m_genderCombo = nullptr;
    QComboBox*             m_typeCombo  = nullptr;
    QCheckBox*             m_onlyDecrypted = nullptr;
    QLabel*                m_countLabel = nullptr;
    QHash<QString, QString> m_scan;                   // active scan key → message (merged into the toast)
    int                    m_metaPct = -1;            // AppearanceMeta crawl progress
    int                    m_iconPct = -1;            // IconIndex scan progress
    QComboBox*             m_iconModeCombo = nullptr;
    QTimer*                m_visIconTimer = nullptr;   // debounce auto-render of in-view thumbnails
    QSet<int>              m_renderBlocklist;          // SNOs that crashed the renderer — never retried
    QSet<int>              m_noRenderSnos;             // SNOs tried but yielded no thumbnail — don't re-attempt (prevents freeze loop)
    QSet<int>              m_animatedSnos;             // appearance SNOs that directly own ≥1 animation
    QHash<int, QStringList> m_animRowsBySno;           // owner appearance SNO → its animation rows ("name  ·  N frames")
    // Authoritative AnimSet index (base/meta/AnimSet/*.ans.json): the game's own clip grouping.
    // Each set's ptPowerEntryList maps snoPower → snoAnim (+ optional snoFemaleOverrideAnim), so we
    // record every clip's owning set (provenance + display grouping) and which clips are the female
    // variant. This is exact game data — no name/skeleton guesswork — used to label & order the list.
    QHash<QString, QString> m_clipSet;                 // clip name (lower) → its AnimSet display name
    QHash<QString, QStringList> m_setClips;            // AnimSet name → its clip rows ("name  ·  N frames")
    QHash<QString, QString> m_clipPower;               // clip name (lower) → its snoPower name (the action it plays)
    QSet<QString>          m_femaleClips;              // clip names (lower) that are female-override variants
    QHash<QString, QString> m_femalePair;              // base clip (lower) → its female-override clip name
    // Sequential "play whole set" queue + a female-preview toggle (swaps in snoFemaleOverrideAnim clips).
    QStringList            m_playQueue;                // clips queued for back-to-back playback
    int                    m_playQueueIdx = -1;        // current index into m_playQueue (-1 = no queue)
    bool                   m_advancingQueue = false;   // guard: queue-advance re-entry into playAnimationByName
    bool                   m_previewFemale = false;    // play/preview the female-override variant when available
    // Manual override: extra appearance SNOs the user picked to also pull animations from (retargeted
    // to the current model's skeleton). Reset when the selected model changes.
    QList<int>             m_pullSources;
    QToolButton*           m_pullClearBtn = nullptr;   // enabled while manual pull-sources are active
    QToolButton*           m_variantsBtn = nullptr;    // "Variants ▾" — jump to a model's skin siblings
    // Shared-rig ("base") resolution: clips are authored on a base body appearance (e.g. barF_base00,
    // npcF_S14_Dannica_base00) and every piece skinned to that rig plays them (bone-hash retarget).
    // Map each base's family prefix (name minus _base<NN>/slot) → its clips, and keep the prefix set
    // so a rigged piece like *_TRS or *_Gizmo can resolve/inherit its base's animations.
    QSet<QString>          m_animFamilyPrefixes;       // base family prefixes that own clips
    QHash<QString, QStringList> m_animFamilyRows;      // family prefix → merged clip rows
    QHash<QString, QString> m_animFamilyOwner;         // family prefix → a base appearance name (for the tooltip)
    QHash<QString, QSet<quint32>> m_familyBones;       // family prefix → union of its base rig's bone-name hashes (Phase-2 skeleton fallback)
    QSet<QString>          m_rigFamilyPrefixes;        // family prefixes of every *_base<NN> body (the "Rigged" filter)
    bool                   m_rigIndexBuilt = false;    // ensureRigIndex() has run
    QHash<int, QString>    m_searchBlobCache;          // sno → lowercased tags/collection/title (name-box search)
    bool                   m_animatedScanned = false;  // the Anim scan has completed
    bool                   m_animatedScanning = false; // the Anim scan is running (background)
    // Entity index (Actor + Item metadata): who actually uses each appearance in-game. Actors carry
    // snoAppearance + snoMonsterFamily; Items carry snoActor. One background scan resolves each model
    // to the NPCs/monsters that wear it, its monster family, and the gear items that render it — turning
    // a cryptic filename into real context. Exact game data (no inference).
    QHash<int, QStringList> m_apprActors;               // appearance sno → using actor names (capped)
    QHash<QString, QStringList> m_appSetsByName;        // appearance name(lower) → AppearanceSet names
    bool                   m_appSetsLoaded = false;     // lazy: only ~20 .aps.json files exist
    void ensureAppearanceSets();
    QHash<int, int>         m_apprActorN;               // appearance sno → true count of using actors
    // AUTHORITATIVE animation assignment: the AnimSets of every Actor that uses this appearance
    // (Actor.arAnimSets, keyed to Actor.snoAppearance + its add-on skin appearances). This is exactly
    // what the game plays on the model — it replaces the old name/skeleton guesswork for any appearance
    // that has an actor. Appearances with no actor (runtime-applied costumes) fall back to the skeleton
    // bridge, clearly marked as "compatible" rather than confirmed.
    QHash<int, QStringList> m_apprSets;                 // appearance sno → AnimSet names its actors use
    QHash<int, QStringList> m_apprVariants;             // appearance sno → sibling skin-variant names (same actor)
    QHash<int, QList<int>>  m_apprVariantSnos;          // appearance sno → sibling variant SNOs (for jump)
    QHash<int, QString>     m_apprName;                 // appearance sno → short name (variant menu labels)
    QHash<int, QString>     m_apprFamily;               // appearance sno → monster family name
    QHash<int, QStringList> m_apprItems;                // appearance sno → item names (capped)
    QHash<int, int>         m_apprItemN;                // appearance sno → true count of items
    QHash<QString, int>     m_itemAppr;                 // item name (original case) → appearance sno (browse-by-item)
    bool                   m_entityScanned = false;     // the Actor/Item scan has completed
    bool                   m_entityScanning = false;    // the Actor/Item scan is running (background)
    int                    m_renderCrashSno = -1;      // recovered-from crash suspect (warn once)
    QString                m_renderCrashName;
    QPushButton*           m_clearBtn = nullptr;
    QCheckBox*             m_multiSelect = nullptr;
    QTreeView*             m_list      = nullptr;   // outliner: browse rows + loaded model's subtree
    SnoListModel*          m_listModel = nullptr;   // flat source (shared model class — untouched)
    ModelOutlinerModel*    m_treeModel = nullptr;   // tree wrapper the views actually sit on
    bool                   m_skelViaOutliner = false;   // skeleton overlay only on while armature selected
    QString                m_hoverTexName;              // outliner texture-leaf hover preview state
    int                    m_hoverTexSno = -1;
    QImage                 m_hoverTexImg;               // decoded once per hovered texture (wheel reuses)
    int                    m_hoverTexImgSno = -1;
    int                    m_hoverChan = -1;            // -1 = full image · 0..3 = R/G/B/A channel tile
    bool                   m_hoverIconArea = false;     // cursor is on an ICON (icon column / grid cell)
    QLabel*                m_breadcrumb = nullptr;      // model › part › material (right column header)
    QVector<QPersistentModelIndex> m_texIconQueue;      // texture leaves awaiting a thumbnail decode
    QTimer*                m_texIconTimer = nullptr;    // throttles the decodes (one per tick)
    QListView*             m_gridView  = nullptr;   // alternate icon-grid layout over the same model
    QStackedWidget*        m_viewStack = nullptr;   // holds m_list (table) + m_gridView (grid)
    QToolButton*           m_gridBtn   = nullptr;   // List/Grid toggle
    QToolButton*           m_displayBtn = nullptr;  // header dropdown: List / Outliner / Grid
    int                    m_displayMode = 1;       // 0 List · 1 Outliner · 2 Grid
    QList<QAction*>        m_kindActs;              // "Outliner shows" toggles (visible in Outliner mode)
    QFrame*                m_tagPanel  = nullptr;   // funnel popup PANEL — stays open while toggling
    QWidget*               m_tagPanelBody = nullptr;   // scrollable group area (filled on meta-ready)
    QSet<QString>          m_tagFilter;             // active tags (AND by default, OR when m_tagOrMode)
    QHash<QString, QCheckBox*> m_tagChecks;         // tag → its checkbox (chip removal unchecks it)
    QToolButton*           m_tagBtn    = nullptr;   // the funnel button (tinted while a tag filter is on)
    bool                   m_tagOrMode = false;     // false = must carry ALL tags · true = ANY
    QLineEdit*             m_hdrSearch = nullptr;   // the ONE smart search box (parses into the hidden fields)
    QSpinBox*              m_frameSpin = nullptr;   // timeline: editable current frame (Blender-style)
    QLabel*                m_frameMax  = nullptr;   // timeline: "/ N" end-frame label
    QLabel*                m_animsHdr  = nullptr;   // "ANIMATIONS · N" header
    QVector<QPersistentModelIndex> m_breadcrumbIx;  // breadcrumb link targets (chain: model→…→node) tag
    QToolButton*           m_colBtn    = nullptr;   // Columns show/hide menu
    QToolButton*           m_filtersToggle = nullptr;   // "Filters ▾" — expands the filter section, shows active count
    QCheckBox*             m_hideBrokenChk = nullptr;   // "Hide un-renderable" (member so a chip can clear it)
    QWidget*               m_filterChips   = nullptr;   // inline removable active-filter pills
    QLabel*                m_toast     = nullptr;   // transient notification label
    QTimer*                m_toastTimer = nullptr;  // auto-hides the toast

    // ── Center column: preview + info + export + timeline + animations ──
    QVector<QToolButton*>  m_viewToggleBtns;   // preview toggles (objectName = settings key)
    QToolButton*           m_autoLoadBtn = nullptr;   // load 3D preview on selection?
    bool                   m_autoLoad  = true;
    bool                   m_skipNextAutoLoad = false;   // suppress 3D load on startup restore
    QPushButton*           m_vpBtn     = nullptr;   // "Graphics" toggle (was "Preview Settings")
    QFrame*                m_vpPanel   = nullptr;   // popup settings panel
    QPushButton*           m_dyeBtn    = nullptr;   // "Pigment" toggle (dye-zone picker)
    QFrame*                m_dyePanel  = nullptr;   // popup pigment/dye panel
    // Viewport toolbar parity with Wardrobe: extra right-side buttons + their popups.
    QToolButton*           m_fsBtn        = nullptr;   // "Fullscreen"
    bool                   m_everFramed = false;       // has any model been framed yet (force first frame)
    bool                   m_hideUnrenderable = false;  // filter out blocklisted / no-geometry models
    QToolButton*           m_camBtn       = nullptr;   // "Camera"
    QToolButton*           m_lightBtn     = nullptr;   // "Lighting"
    QToolButton*           m_shaderBtn    = nullptr;   // "Shaders" (dev-only)
    QToolButton*           m_detailBtn    = nullptr;   // "Detail maps" (dev-only)
    QToolButton*           m_physBtn      = nullptr;   // "Physics" (dev-only)
    QFrame*                m_camPanel     = nullptr;
    QFrame*                m_lightPanel   = nullptr;
    QFrame*                m_shaderPanel  = nullptr;
    QFrame*                m_detailPanel  = nullptr;
    QFrame*                m_physPanel    = nullptr;
    QWidget*               m_manualDetailBox = nullptr;   // greyed while Detail "Auto" is on
    QComboBox*             m_channelCombo = nullptr;   // Shaded / raw material channel viewer
    QSlider*               m_fovSlider    = nullptr;   // Camera-panel FOV (reflected by presets)
    QSplitter*             m_viewSplit    = nullptr;   // vertical splitter holding the viewport
    // ── "Maximize viewport": hide the tab's chrome in place instead of going OS-fullscreen ──
    QSplitter*             m_mainSplit    = nullptr;   // the 3-column splitter (left | centre | right)
    QWidget*               m_pvHeadW      = nullptr;   // centre: preview header row container
    QWidget*               m_viewBarW     = nullptr;   // centre: view toolbar container
    QWidget*               m_bottomW      = nullptr;   // centre: the transport pane under the viewport
    bool                   m_viewMaxed    = false;     // true = chrome hidden, viewport fills the tab
    QShortcut*             m_fsEsc        = nullptr;   // Esc → restore (only enabled while maximized)
    QToolButton*           m_fsExitBtn    = nullptr;   // floating "✕ Exit" while maximized
    int                    m_geoToken = 0;          // async-load request sequence
    int                    m_dyeRegion = 0;         // dye region the picker edits
    int                    m_dyeRegionsUsed = 0;    // distinct dyeable materials in model
    bool                   m_dyeRegionUsed[4] = {};
    QString                m_dyeRegionName[4];       // material name per used region
    QLabel*                m_dyeUsageLbl = nullptr;
    QComboBox*             m_dyeCombo = nullptr;     // Custom + real dye list
    QToolButton*           m_dyeRegionBtn[4] = {};
    QWidget*               m_dyeWheel = nullptr;   // HSV colour wheel (DyeColorWheel)
    QLabel*                m_dyeSwatch = nullptr;
    QLineEdit*             m_dyeHex = nullptr;
    QToolButton*           m_dyeMem[8] = {};       // memorised colour swatches
    QPoint                 m_dyeDragStart;         // press point for slot drag
    GLModelWidget*         m_modelView  = nullptr;
    QWidget*               m_infoOverlay = nullptr;   // FILE INFO overlaid on the preview
    QHash<QString, QLabel*> m_infoVals;          // FILE INFO key → value label (viewport overlay)
    QHash<QString, QLabel*> m_dataVals;          // DATA page key → value label (superset of the overlay)
    QLabel*                m_rstackHint = nullptr;   // "No model loaded" veil over the right pages
    QWidget*               m_animPage   = nullptr;   // built with the center column, REGISTERED at ctor end
    QListWidget*           m_histList   = nullptr;   // search-history dropdown (child widget, never grabs)
    QLabel*                m_texFacts   = nullptr;   // "2048×2048 · mips · format" under the channel tiles
    QLabel*                m_statsOv    = nullptr;   // viewport Statistics overlay (Blender-style)
    void updateStatsOverlay();                       // recount verts/tris/parts/bones for what's visible
    QTableView*            m_clothView  = nullptr;   // CLOTH page: authored per-piece sim tuning
    QStandardItemModel*    m_clothModel = nullptr;
    QLabel*                m_clothHdr   = nullptr;
    void fillClothPage();                            // read each cloth part's Cloth/<name>.clt.json
    QTableView*            m_partsView  = nullptr;   // PARTS page: per-part visibility + facts
    QStandardItemModel*    m_partsModel = nullptr;
    QLabel*                m_partsHdr   = nullptr;
    bool                   m_partsPageSync = false;  // guard: rebuilding rows must not re-toggle
    void fillPartsPage();                            // rebuild from m_curGeo + live visibility
    QWidget*               m_vpStrip    = nullptr;   // N-strip: settings-popover buttons on the viewport edge
    QToolButton*           m_shadeMoreBtn = nullptr; // shading ⌄ — wheel over it cycles the channel
    QToolButton*           m_overlayBtn = nullptr;   // Overlays master toggle (view toolbar)
    QFrame*                m_overlayPanel = nullptr; // persistent overlay-settings popup (▾ arrow)
    bool                   m_overlaysOn = true;      // master gate — false forces every guide off
    QVector<QPair<QCheckBox*, std::function<void(bool)>>> m_overlayChks;   // box → its GL applier
    QPushButton*           m_exportBtn  = nullptr;   // Export model(s)
    QCheckBox*             m_exportTex  = nullptr;
    QCheckBox*             m_exportAnim = nullptr;
    QWidget*               m_timeline   = nullptr;
    QPushButton*           m_playBtn    = nullptr;
    QSlider*               m_animSlider = nullptr;
    QLabel*                m_timeLabel  = nullptr;
    QComboBox*             m_speedCombo = nullptr;
    QCheckBox*             m_loopCheck  = nullptr;
    QTimer*                m_animTimer  = nullptr;
    float                  m_animFps    = 30.0f;   // base clip rate (pre speed-combo)
    QLineEdit*             m_animSearch = nullptr;
    QListWidget*           m_anims      = nullptr;
    int                    m_animCount  = 0;         // last populateAnimList size (outliner badge)
    QHash<int, QStringList> m_animCache;
    QString                m_playingAnim;   // currently-selected animation (empty = none)
    AnimParser::DecodedAnim m_curAnim;      // the decoded clip currently loaded (for .glb export)

    // ── Right column: looks + materials + material textures ──
    QTableView*            m_looksView  = nullptr;
    QStandardItemModel*    m_looksModel = nullptr;
    QLabel*                m_looksHdr   = nullptr;
    QLabel*                m_matsHdr    = nullptr;
    QLabel*                m_texHdr     = nullptr;
    QSplitter*             m_rstack     = nullptr;   // right column: visible panels, drag to resize
    QVector<PanelBox*>     m_rsections;              // REGISTRATION order — index == page id
    QVector<QString>       m_sectKeys;               // per-panel settings key (label text mutates!)
    QVBoxLayout*           m_rstripLay  = nullptr;   // the strip's layout (late pages insert before the stretch)
    QVector<QToolButton*>  m_rpageBtns;              // vertical icon strip — checkable show/hide toggles
    bool                   m_panelRestore = false;   // ctor is replaying a saved layout — don't save over it
    QToolButton*           m_sideArrow  = nullptr;   // ‹/› atop the strip — collapse the column
    bool                   m_sideCollapsed = false;  // true = only the arrow shows
    void setSideCollapsed(bool on);
    void showPanel(int page, bool on);               // strip toggle → panel in/out of the splitter
    void sizeNewPanel(PanelBox* box);                // give a freshly-opened panel a fitting height
    void movePanel(int page, int delta);             // ▲▼ reorder within the splitter
    void savePanelLayout();                          // which panels, what order, what heights
    QTabWidget*            m_matTabs    = nullptr;   // App Materials / Materials / Vertex Buffers
    QTabWidget*            m_detailTabs = nullptr;   // Textures / Values / Shaders
    QTableView*            m_mats       = nullptr;
    QStandardItemModel*    m_matModel   = nullptr;
    QTableView*            m_matListView = nullptr;   // "Materials" sub-tab (by SNO)
    QStandardItemModel*    m_matListModel = nullptr;
    QTableView*            m_vbView     = nullptr;    // "Vertex Buffers" sub-tab
    QStandardItemModel*    m_vbModel    = nullptr;
    QTableView*            m_subObjView = nullptr;    // "SubObject Apps" sub-tab (LOD0)
    QStandardItemModel*    m_subObjModel = nullptr;
    QTableView*            m_matTex     = nullptr;
    QStandardItemModel*    m_matTexModel = nullptr;
    QTableView*            m_matValView = nullptr;    // "Values" tab (full SNO|Name|Value)
    QStandardItemModel*    m_matValModel = nullptr;
    QTableView*            m_shaderView = nullptr;    // "Shaders" tab (ShaderMap refs)
    QStandardItemModel*    m_shaderModel = nullptr;
    QLabel*                m_matValues  = nullptr;
    QLabel*                m_chanImg[6] = {};   // tile images
    QLabel*                m_chanCap[6] = {};   // tile captions (relabeled per mode)
    QImage                 m_chanFull[6];       // native-resolution channel images (for zoom)
    QTimer*                m_geoTimer   = nullptr;   // debounce geometry loads

    QHash<int, QPixmap>    m_origIconCache;   // sno → original 2D icon (null = miss)
    QHash<int, QPixmap>    m_renderCache;     // sno → cached 3D-render thumbnail
    // Perf caches: avoid re-reading CASC + re-BC-decoding on every re-select / look change.
    // Both are bounded by cost (KB) so memory stays capped no matter how many models are viewed.
    QCache<int, std::shared_ptr<ModelGeometry>> m_geoCache;   // sno → parsed geometry
    QCache<QString, QImage> m_texCache;                       // "ROLE|material" → decoded texture
    QTimer*                m_hoverTimer = nullptr;   // 0.5s dwell before icon preview
    QLabel*                m_iconPreview = nullptr;  // floating hover preview popup
    int                    m_hoverSno   = -1;
    int                    m_hoverTile  = -1;        // texture-preview tile under cursor
    int                    m_previewPx  = 192;       // hover-preview size (scroll-adjustable)
    int                    m_iconPx     = 48;        // Outliner icon size (Ctrl+scroll)
    int                    m_gridPx     = 88;        // Grid thumbnail size (Ctrl+scroll)
    QFont                  m_listBaseFont;           // normal list font (restored for Outliner/Grid)

    int                    m_curSno     = -1;
    QString                m_curName;
    ModelGeometry          m_curGeo;          // cached parse for view + export
    QVector<QString>       m_appMatNames;     // appearance material roster (active look)
    QVector<QVector<QString>> m_soaNames;     // per material index: name for each SOA/look
    int                    m_currentLook = 0; // selected look = SOA index applied to parts
    QSet<int>              m_clothMats;       // roster indices that are cloth-sim
    QSet<int>              m_fxMats;          // roster indices that are fx/effect
    QSet<int>              m_gibMats;         // roster indices that are gore/flesh/gib (mostly NPC)
    QSet<int>              m_lookHiddenMats;  // roster indices with no material this look
    QVector<bool>          m_partIsSim, m_partIsFx, m_partIsGib;   // per-part flags (by primitive)
    bool                   m_showFx = true, m_showSim = true, m_showGib = true;   // View ▸ FX / SIM / GIB
    bool                   m_loaded     = false;

    // ── Attachments panel (models an actor holds/spawns) ──
    QTreeWidget*                      m_attachTree = nullptr;   // grouped actor → trigger → child, checkable
    QLabel*                           m_attachHdr  = nullptr;   // panel header (attachment count)
    QVector<ModelAttach::Attachment>  m_attachAll;              // every attachment scanned for this model
    QSet<QString>                     m_attachActive;           // attachment keys currently seated in the view
    int                               m_attachScanToken = 0;    // async attachment-scan sequence
    int                               m_attachPage = -1;        // right-panel page id of the ATTACHMENTS panel
    // Per-appearance attachment cache (immutable per game build) → re-selecting a model is instant
    // instead of re-reading up to 80 actor JSONs. Cleared on data reload (reset()).
    QHash<int, QVector<ModelAttach::Attachment>> m_attachCache;
    // Pristine base snapshot so attach/detach always re-assembles from the clean model.
    ModelGeometry                     m_baseGeo;
    QVector<QString>                  m_baseMatNames;
    QVector<QVector<QString>>         m_baseSoaNames;
    QSet<int>                         m_baseClothMats, m_baseFxMats, m_baseGibMats;   // base FX/SIM/GIB roster sets
    bool                              m_haveBase = false;
    QHash<quint32, QPair<int, std::array<float, 16>>> m_baseHpMap;   // parent hardpoint hash → bone+local
};
