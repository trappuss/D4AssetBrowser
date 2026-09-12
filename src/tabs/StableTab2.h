#pragma once
#include "tabs/BrowserTab.h"
#include "gl/GLModelWidget.h"
#include "model/ModelExporter.h"
#include "model/ModelGeometry.h"

#include <QHash>
#include <QImage>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <functional>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QFrame;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QShortcut;
class QSlider;
class QSplitter;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class PanelBox;   // right-column stacking panel, shared with Models/Wardrobe (PanelBox.h)

// Stable (rewrite): mount previewer modeled on the Wardrobe tab. A mount "look" is
// three layered appearance slots — Mount body, Barding (armor), Trophy — plus a Pet
// viewing mode. The three picks are parsed and combined onto one model, textured on
// the shared PBR pipeline, framed with the Lighting/Camera popups, and exported to
// .glb (mesh, or rig + clips only). Mounts are NOT dyeable in
// Diablo IV, so there is deliberately no dye control (unlike the Wardrobe tab).
class StableTab2 : public BrowserTab {
    Q_OBJECT
public:
    explicit StableTab2(QWidget* parent = nullptr);
    void refresh() override;
    // Reload/build change: forget everything keyed to the GAME BUILD, or it gets answered from
    // the previous one. Defined in the .cpp — it has to touch the on-disk thumbnail cache, and it
    // is long enough now that inlining it hid what it does NOT clear.
    void reset() override;
    void onSettingsChanged() override { fillGrid(); }    // re-stamp card icons (presence-badge toggles)
    void persistView() override { saveCameraState(); }   // flush camera/FOV on app close

    // Export hooks (top-level Export menu + preview capture).
    GLModelWidget* previewWidget() override { return m_view; }
    bool    hasExportSelection() const override { return m_lastGeo.valid && !m_lastGeo.primitives.isEmpty(); }
    void    exportSelection() override { exportMount(); }
    void    exportSelectionToLast() override { exportMount(QVector<int>(), QString(), true); }
    QString exportNoun() const override { return QStringLiteral("mount"); }

    // Contextual "anim export" action in the Export menu — rig + clips, no mesh.
    bool    hasAnimExport() const override;
    QString animExportLabel() const override;   // names the clip set it will actually write
    void    exportAnimations() override;

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void hideEvent(QHideEvent* ev) override;        // remember the camera for next launch

private:
    enum Slot { SlotMount = 0, SlotBarding = 1, SlotTrophy = 2, SlotCount = 3 };

    // One unlockable stable item resolved from game data (Item/*.itm.json): a mount
    // (MountItem → snoMount → actor: eMountType + appearance + colour look), an armor
    // piece (HorseArmor/CatArmor), a trophy (Trophy → snoActor), or a pet (CompanionItem
    // → snoCompanion). Localized name/description come from enUS_Text StringLists.
    struct StableEntry {
        QString item;          // item file base name (identity)
        QString name;          // localized Name ("Skartaran Basilisk") — falls back to item
        QString desc;          // localized Description
        QString appr;          // appearance name (material roster + ensembles)
        int     apprSno = 0;   // appearance SNO (mesh)
        quint32 look = 0;      // colour-variant look hash (mounts; 0 = default look)
        int     type = -1;     // eMountType: 0 Horse · 1 Cat · 2 Basilisk (armor: 0/1) · -1 n/a
    };

    // ── Discovery ────────────────────────────────────────────────────────────
    void ensurePetIndex();                          // background item scan (mounts+gear+pets, cached)
    void onPetsReady();                             // item scan finished → fill + auto-select
    bool petMode() const;                           // the selected mount is a pet
    QString mountCategory() const;                  // category token of the selected mount (horse/cat/…/pet)
    QVector<QPair<QString, int>> candidatesFor(int slot) const;   // (name, sno) for the active species
    QVector<StableEntry> entriesFor(int slot) const;   // item-driven candidates (when the scan is ready)
    static QString typeToken(int type);             // 0→"horse" 1→"cat" 2→"chimera"
    static QString typeLabel(const QString& token); // "chimera"→"Basilisk", else capitalized

    // ── Slot cells + card grid ───────────────────────────────────────────────
    void selectSlot(int slot);                      // make a slot active → refill the grid
    void refreshSlotCells();                        // redraw the 3 slot-cell buttons
    void fillGrid();                                // (re)build the browser cards for the active slot
    void rebuildCollections();                      // repopulate the collection filter

    // ── Assemble + texture ───────────────────────────────────────────────────
    void rebuildMount();                            // parse the 3 slots, merge, texture, show
    void scheduleRebuild();                         // 35 ms debounce for repeatable interactive changes
    void saveCurrent();                             // persist the live selection for next launch
    void restoreCurrent();                          // reload the last selection
    void saveCameraState();                         // remember orbit/zoom/fov/ortho
    void restoreCameraState();
    void rebuildPartList();
    void recomputePartVisibility();
    void fillMaterialsPanel();                      // MATERIALS table (# · Material · Tris)
    void updateTexTiles(int partIndex);             // selected part → 5 PBR-channel preview tiles
    // Original 2D portrait icon: mounts/pets have no icon HANDLE, but the game ships baked
    // "2DInventory_Bundle_*" atlases addressed by name. Resolve one by matching the appearance's
    // variant token (storNNN/amorNNN/…) + species/descriptor, then decode it. Null ⇒ none exists.
    QImage resolveOriginalIcon(const QString& appr);
    // Card thumbnails: fallback ONLY when no baked portrait exists — render a 3D silhouette,
    // lazily, disk-cached per appearance SNO.
    void queueThumb(int sno, const QString& appr);  // resolve baked portrait, else render a thumb
    void processThumbs();                           // timer: render a few queued thumbs per tick
    void setCardIcon(int sno, const QPixmap& pm);   // stamp a finished thumb onto its visible card
    QPixmap badgeIcon(int sno, const QPixmap& pm) const;   // overlay the ✓/✗ model-presence badge
    QString thumbPath(int sno) const;
    QList<int> primitivesOf(QTreeWidgetItem* it) const;
    QTreeWidgetItem* itemForPart(int part) const;   // parts-tree row for a merged primitive index
    // Everything that follows the parts-tree selection: the viewport outline and the texture
    // tiles. Named, so the viewport's click handler can edit the tree with signals blocked and
    // then run one sync rather than two — the tiles are six smooth rescales.
    void syncPartSelection();
    QList<int> selectedParts() const;
    void installCopyMenu(QWidget* view, int nameCol);   // right-click → Copy name / Copy all

    // ── Themed sets ("Equip Theme") ──────────────────────────────────────────
    // In game data a mount ships bundled with matching Mount Armor + Trophy inside a
    // StoreProduct "Bundle_*Mount*" (arBundledProducts). buildThemeMap scans those once to
    // map a mount appearance → its set's armor + trophy appearances; equipMountTheme applies
    // the whole set at once (right-click a mount card → "Equip matching set").
    void buildThemeMap();                              // scan StoreProduct bundles (lazy, once)
    void equipMountTheme(const StableEntry& mount);    // equip mount + matching armor + trophy
    void equipEntry(int slot, const StableEntry& e);   // equip one item into a slot (menu "Equip")
    bool matchingSetPiece(const StableEntry& mount, int slot, StableEntry& out);  // set's armor/trophy
    int  themeItemCount(const StableEntry& mount);     // # of matching set pieces we can equip
    void exportAppearanceModel(int sno, const QString& appr, bool toLast);   // export one item's mesh

    // (Saved "Stables" loadouts removed — not needed for a browser.)

    // ── Animations ───────────────────────────────────────────────────────────
    void buildAnimPanel();
    void populateAnims();                           // discover clips for the current mount rig
    // D4_DUMP_MNTTROPHYANIM=1 — one-shot: which mount trophy appearances own clips. Gates whether
    // reopening the trophy sub-rig placement problem (see seatTrophyOnMount) is worth anything.
    void dumpMountTrophyAnims();
    // D4_DUMP_PETANIM=1 — one-shot: which companions resolve to a carrier that owns clips.
    void dumpPetAnims();
    void fillAnimList();                            // apply the search filter to the cached rows
    void playAnimByName(const QString& name);
    void resetAnimToDefault();                      // "Reset to default": nav-idle clip + 1x + loop
    // Every clip carrying a species token, bucketed by the appearance that OWNS it. ONE directory
    // scan per token, cached for the session; carrier resolution reads the buckets instead of
    // trusting a name (see animCarrierFor).
    // The returned reference lives in m_clipTok: a LATER clipBuckets() call with an unseen token
    // inserts and can rehash, so never hold this across one — resolve the carrier first, then read.
    const QHash<int, QStringList>& clipBuckets(const QString& speciesTok);
    // Disk cache for the above (stable_anims_v2.json). The walk is 45,549 files and used to run
    // once per SPECIES TOKEN per session; this makes it once per d4data snapshot.
    static QString animCacheSig();
    void loadClipCache();
    void saveClipCache();
    QStringList currentClipRows();                  // rows for the live selection (the ONE row source)
    // Species token for the Anim/ scan. For a mount, its species; for a PET, the species too —
    // the last segment, not the first two. The game shares one clip family across every
    // variant of a species (CMP_dogLarge_nav_idle), which no variant-bearing token can match.
    static QString animTokenFor(const QString& appr, bool pet, const QString& catHint);
    // The appearance that owns `appr`'s clips. `catHint` is the authoritative eMountType token
    // when the caller has one (mountCategory()), empty to derive it from the name.
    int  animCarrierFor(const QString& appr, int apprSno, bool pet, const QString& catHint = QString());
    // The species hint for `appr`: mountCategory() when it is the live selection, else empty.
    QString hintForAppr(const QString& appr, bool pet) const;
    // Which clips an export carries (Settings ▸ Export scope), and the one decoder that turns
    // names into clips. Every export path goes through this pair, so they all ship the same set
    // for the same settings (clips that fail to decode are dropped, so a menu count is an upper
    // bound on what reaches the file).
    QStringList exportClipNames(const QString& appr, int apprSno, bool pet);
    void collectExportAnims(const ModelGeometry& geo, const QStringList& clipNames,
                            QVector<AnimParser::DecodedAnim>& anims, QStringList& names);
    void exportAnimLibrary();                      // rig + selected clips only (no mesh)
    QString exportMenuSuffix(const QString& appr, bool pet);             // "1 model + N anims + M raw" per settings
    void applyAnimSpeed();
    void tickAnimation();
    void clearAnim();                               // stop playback + reset
    int  animCarrierSno();                          // the appearance SNO that owns the clips

    // ── Lighting / Camera / Graphics / Shaders / Overlays popups ─────────────
    void buildLightingPanel();
    void buildCameraPanel();
    void buildGraphicsPanel();     // IBL · shadows · SSAO · tonemap · shading features · backdrop
    void buildShaderPanel();       // fur/mane shell-shader sliders + mesh FX
    void buildDetailPanel();       // detail-map discovery tool (global, stable2/detail/*)
    void buildPhysicsPanel();      // live cloth-sim tuning (mane/tail cloth)
    void applyLightRig();
    void applyGraphics();          // push saved stable2/gfx/* to the viewport
    void applyFur();               // push saved stable2/fur/* + stable2/fx/* to the viewport
    void applyDetailConfig();      // push saved stable2/detail/* to the viewport
    void applyClothParams();       // push saved stable2/cloth/* to the viewport
    // viewport AND parts panel. `groupPart` is any part belonging to the group whose HEADER was
    // right-clicked: part stays -1 (no single part was picked) but the model-level actions still
    // scope to that group's source item instead of falling back to the whole assembly.
    // "Why does this part look the way it does" — authored vs assumed values, roster source,
    // texture roles — in the shared read-only pane (util/TextReportDialog.h).
    void showMaterialReport(const QString& materialName, const QString& apprName, int apprSno);
    void showPartContextMenu(int part, const QPoint& globalPos, int groupPart = -1);

    void reapplyOverlays();        // re-push ALL overlay state (master gate + each box)
    void linkColliderToggles();    // mirror the Overlays + Physics "collision model" boxes
    void showPopup(QWidget* panel, QWidget* anchor);   // strip buttons open LEFTward
    void setSideCollapsed(bool on);// hide the whole right sidebar (N-strip arrow)

    // ── Undo (slot/look changes) ──────────────────────────────────────────────
    struct Snapshot { int sel[SlotCount]; QString name[SlotCount], disp[SlotCount], desc[SlotCount];
                      quint32 look[SlotCount]; int mountType; };
    void pushUndo();               // snapshot BEFORE a change
    void undo();                   // Ctrl+Z → restore + rebuild

    // ── Wardrobe-parity chrome: viewport N-strip, fullscreen, right sidebar ──
    void buildVpStrip();                            // floating button strip on the viewport edge
    void positionVpStrip();                         // pin it to the viewport's right edge
    void buildSidebar(QSplitter* mainSplit);        // PanelBox sidebar (PARTS · MATERIALS · TEXTURES · INFO · ANIMATIONS)
    void showSidePanel(int page, bool on);          // strip toggle → panel in/out
    void moveSidePanel(int page, int delta);        // ▲▼ reorder among the panels that are up
    void saveSidePanelLayout();                     // which panels, what order, what heights
    void updateSidebarCollapse();                   // no panels up → shrink the column to the strip
    void toggleFullscreen(bool on);                 // hide chrome, viewport fills the tab (Esc)

    // ── Export ───────────────────────────────────────────────────────────────
    QVector<int> partsOfSource(int part) const;   // merged parts sharing one equipped appearance
    void exportMount(const QVector<int>& keep = {}, const QString& label = {}, bool toLast = false);

    // Icon for an appearance SNO (inventory icon if the game has one; else null).
    QImage slotIcon(int sno) const;

    // ── State ────────────────────────────────────────────────────────────────
    int           m_activeSlot = SlotMount;
    int           m_slotSel[SlotCount] = { 0, 0, 0 };   // selected appearance SNO per slot (0 = none)
    QString       m_slotName[SlotCount];                // APPEARANCE name per slot (roster/ensembles)
    QString       m_slotDisp[SlotCount];                // localized display name per slot
    QString       m_slotDesc[SlotCount];                // localized description per slot
    quint32       m_slotLook[SlotCount] = { 0, 0, 0 };  // colour-variant look hash per slot
    int           m_mountType = -1;                     // selected mount's eMountType (-1 unknown/pet)
    QLabel*       m_infoLbl = nullptr;                  // name + description of the selected mount
    QToolButton*  m_slotCell[SlotCount] = { nullptr, nullptr, nullptr };
    QButtonGroup* m_slotCellGroup = nullptr;

    QLineEdit*    m_search = nullptr;
    QComboBox*    m_collFilter = nullptr;
    QScrollArea*  m_gridScroll = nullptr;
    QWidget*      m_gridContent = nullptr;
    QGridLayout*  m_gridLayout = nullptr;
    QButtonGroup* m_gridGroup = nullptr;

    QCheckBox*    m_wire = nullptr;
    QCheckBox*    m_grid = nullptr;
    QCheckBox*    m_fxChk = nullptr;
    QCheckBox*    m_simChk = nullptr;
    QCheckBox*    m_ovlChkColliders  = nullptr;   // Overlays panel "Collision model"
    QCheckBox*    m_physChkColliders = nullptr;   // Physics panel "Show collision models"
    bool          m_colliderTogglesLinked = false;
    QTreeWidget*  m_partTree = nullptr;
    QTreeWidget*  m_matTable = nullptr;             // MATERIALS panel (right sidebar)
    QLabel*       m_texTile[6] = {};                // TEXTURE PREVIEW tiles (COLOR/ROUGH/METAL/NORMAL/ALPHA/EMIS)
    QLabel*       m_status = nullptr;
    GLModelWidget* m_view = nullptr;

    QFrame*       m_lightPanel = nullptr;
    QFrame*       m_camPanel = nullptr;
    // Re-reads the viewport into the Camera panel's Yaw/Pitch controls. Built with the panel.
    std::function<void()> m_camOrbitSync;
    QFrame*       m_gfxPanel = nullptr;
    QFrame*       m_shaderPanel = nullptr;
    QFrame*       m_detailPanel = nullptr;
    QFrame*       m_physPanel = nullptr;
    QFrame*       m_overlayPanel = nullptr;         // Overlays "⌄" popup (toolbar)
    QComboBox*    m_channelCombo = nullptr;         // shading "⌄" channel viewer (Shaded/Base/Normal/…)
    QToolButton*  m_shadeMoreBtn = nullptr;         // shading "⌄" arrow (wheel over it cycles channel)
    QToolButton*  m_overlayBtn = nullptr;           // Overlays master sphere toggle (toolbar)
    bool          m_overlaysOn = true;              // overlays master gate (false forces every guide off)
    QVector<QPair<QCheckBox*, std::function<void(bool)>>> m_overlayChks;   // box → its GL applier
    QToolButton*  m_sideArrow = nullptr;            // N-strip: collapse the whole sidebar
    bool          m_sideCollapsed = false;
    QVector<Snapshot> m_undo;                       // slot/look history (Ctrl+Z)
    bool          m_restoring = false;              // guard: undo/rebuild must not re-snapshot

    // Wardrobe-parity chrome.
    QSplitter*    m_mainSplit = nullptr;            // left | center | sidebar
    QWidget*      m_toolbarW = nullptr;             // toolbar row (hidden in fullscreen)
    QWidget*      m_vpStrip = nullptr;              // floating N-strip on the viewport right edge
    QToolButton*  m_fsBtn = nullptr;                // fullscreen toggle (checkable)
    QShortcut*    m_fsEsc = nullptr;                // Esc → exit fullscreen
    bool          m_fullscreen = false;
    QWidget*      m_sidebarW = nullptr;             // strip + splitter container (pane 2)
    QSplitter*    m_rsplit = nullptr;               // stacked PanelBoxes
    QVector<PanelBox*>    m_rsections;              // registration order == page id
    QVector<QToolButton*> m_rpageBtns;              // sidebar strip toggles
    QVector<QString>      m_rids;                   // STABLE layout id per panel (never the title)
    QVector<bool>         m_rdefOn;                 // default up? — used only by the one-time migration
    QWidget*              m_rstripW = nullptr;      // the icon strip (updateSidebarCollapse sizes to it)
    bool                  m_panelRestore = false;   // replaying a layout — suppress writes

    // Animations
    QWidget*      m_animPanel = nullptr;
    QWidget*      m_timeline = nullptr;
    QListWidget*  m_anims = nullptr;
    QLineEdit*    m_animSearch = nullptr;
    QSlider*      m_animSlider = nullptr;
    QPushButton*  m_playBtn = nullptr;
    QPushButton*  m_resetBtn = nullptr;   // whole-mount reset — lives in the left picker column
    QComboBox*    m_speedCombo = nullptr;
    QCheckBox*    m_loopCheck = nullptr;
    QTimer*       m_animTimer = nullptr;
    float         m_animFps = 30.0f;
    QString       m_playingAnim;
    AnimParser::DecodedAnim m_curAnim;
    // Species token → (owning appearance SNO → clip rows). One entry per token scanned; every
    // clip consumer reads this, so the preview list and the export can never disagree.
    QHash<QString, QHash<int, QStringList>> m_clipTok;
    bool m_clipDiskLoaded = false;   // the disk cache is read once per session, not per token

    QVector<int>  m_partFx, m_partSim, m_partHidden;   // per-primitive FX / simulated / force-hidden (collision) flags
    // Per merged primitive: which of the up-to-three equipped appearances it came from. The mount,
    // barding and trophy are merged into one geometry, so without this a right-clicked part cannot
    // be traced back to the item that owns it.
    QStringList   m_partSource;                       // source appearance name
    QVector<int>  m_partSourceSno;                    // source appearance SNO
    QVector<int>  m_partSourceSlot;                   // SlotMount / SlotBarding / SlotTrophy
    ModelGeometry m_lastGeo;
    QVector<ModelExporter::ExportMaterial> m_exportMats;
    // Session-wide raw material-decode cache (bounded, in-memory only; cleared on reset()/reload).
    // Keyed by material name — decodes are deterministic, so re-equipping or switching mounts
    // reuses everything already decoded instead of re-running BC decode for the whole model.
    QHash<QString, QImage> m_cBase, m_cNorm, m_cOrm, m_cEmis, m_cMask, m_cTrans;

    QVector<QPair<QString, int>> m_pets;            // pet appearances (name, SNO) — legacy view of m_petItems
    QHash<int, quint32> m_iconByApp;                // mount/barding/trophy/pet appearance SNO → icon handle
    // Item-driven rosters (authoritative, from the background scan; empty until ready).
    QVector<StableEntry> m_mounts, m_armorItems, m_trophyItems, m_petItems;
    QVector<StableEntry> m_gridEntries;             // entries behind the current card grid (item path)
    // Themed sets (mount appearance name, lowercased → matching armor / trophy appearance names).
    QHash<QString, QString>     m_themeArmor;       // mount → its set's Mount Armor appearance
    QHash<QString, QStringList> m_themeTrophy;      // mount → its set's Trophy appearance(s)
    bool                        m_themesBuilt = false;
    QTimer* m_rebuildTimer = nullptr;               // scheduleRebuild()'s 35 ms coalescing timer
    QTimer* m_gridReflow = nullptr;                 // debounce grid rebuild on panel resize
    int  m_gridCols = 0;                            // last column count (reflow only when it changes)
    // Rendered card thumbnails (no inventory icons exist for mounts).
    QHash<int, QPixmap> m_thumbs;                    // appearance SNO → icon (baked portrait or rendered thumb)
    QVector<QPair<QString, int>> m_atlasIdx;         // 2DInventory/2DUI texture name → SNO (built once)
    bool                m_atlasBuilt = false;
    QVector<int>        m_thumbQueue;                // SNOs awaiting an icon
    QSet<int>           m_thumbQueued;               // dedupe the queue
    QHash<int, QString> m_thumbAppr;                 // SNO → appearance name (for portrait/roster lookup)
    QTimer*             m_thumbTimer = nullptr;      // renders a few per tick
    bool                m_thumbDirty = false;        // a batch rendered → restore the live mount after
    bool m_petReady = false;
    bool m_petBuilding = false;
    // Generation counter: reset() bumps it, and a scan already in flight compares before it
    // installs. Without this, fixing reset() turns a fingerprint change mid-scan into the OLD
    // roster overwriting the new one — the exact failure the counter exists to stop.
    int  m_petGen = 0;

    // Wired ONCE, not once per refresh(). reset() clears m_loaded so the tab repopulates on a new
    // build, so refresh() can run more than once — and its two singleton connections are lambdas,
    // which Qt::UniqueConnection cannot dedupe.
    bool m_signalsWired = false;
    bool m_loaded = false;
    bool m_framed = false;
};
