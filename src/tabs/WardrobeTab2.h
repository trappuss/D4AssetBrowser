#pragma once
#include "tabs/BrowserTab.h"
#include "model/ModelExporter.h"
#include "model/ModelGeometry.h"

#include <QCache>
#include <QHash>
#include <QImage>
#include <QMap>
#include <QMutex>
#include <QPair>
#include <QPoint>
#include <QString>
#include <QVector>
#include <QVector3D>
#include <QVector4D>
#include <array>
#include <functional>

class QColor;
class QMenu;
class QComboBox;
class QCheckBox;
class QButtonGroup;
class QGridLayout;
class QVBoxLayout;
class QScrollArea;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QSplitter;
class QTabWidget;
class QTimer;
class QToolButton;
class QAbstractButton;
class QTreeWidget;
class QTreeWidgetItem;
class QShortcut;
class GLModelWidget;
class PanelBox;   // right-column stacking panel, shared with the Models tab (PanelBox.h)

// Per-part decoded maps produced by the (thread-safe) texture-decode pass. Everything the apply
// phase needs from the decode loop, so the loop can run on a worker thread and hand this back.
struct WardrobeOutfitMaps {
    QVector<QImage> tex, norm, orm, dm, rp, furMask, furNoise, fxNoise, emis, trans;
    QVector<QImage> detailN[3], detailR[3];
    QVector<float>  metal, rough, emisMul, emisCol;
    QVector<float>  dNInt, dRInt, dROffV, dCAddV;
    QVector<QVector3D> dScaleV;
    QVector<int>    dMetalLayerV;
    QVector<QVector4D> dBandsV, dZoneMapV;
    QVector<int>    hair, skin, cloth, region, fur, fxAdd, eye, head;
    QVector<float>  hairParams;   // per part ×3: hero_hair (Hair Roughness, Hair Specular, Highlight Shift)
    QVector<float>  fxIntensity, fxWobble, fxFresnel, fxAlpha, fxSat;
    QVector<QString> matKey;      // per-part VRAM-pool key (material + colour epoch)
    QString         furProbe;
    QString         loadLogAdd;   // per-part diagnostics appended by the decode loop
};
// Geometry-phase context the apply phase needs alongside the decoded maps.
struct WardrobeBuildCtx {
    ModelGeometry merged;
    QVector<int>  primSlot;
    int           baseOutfitCount = 0;
    QString       loadLog, skelDbg, weapDbg;
    QVector<QPair<QString,int>> pieceList;
    QVector<int>                pieceSno;    // parallel to pieceList: each piece's appearance SNO,
                                             // so a part can be traced back to the item it came from
    QString       keepAnim;
    int           keepFrame = 0;
    bool          wasPlaying = false;
    int           pieceCount = 0;
    qint64        totalV = 0, totalT = 0;
};

// Wardrobe: assemble a full character outfit — pick class + gender, then an
// appearance per equipment slot (helm / torso / gloves / legs / boots) — and preview
// the combined, textured, dyeable model. Built on the shared model pipeline
// (ModelParser + mergeGeometries + MaterialDecode + GLModelWidget).
class WardrobeTab2 : public BrowserTab {
    Q_OBJECT
public:
    explicit WardrobeTab2(QWidget* parent = nullptr);
    void refresh() override;
    void persistView() override { saveCameraState(); }   // flush camera/FOV on app close
    GLModelWidget* previewWidget() override;   // Export-menu capture target
    // Export the assembled outfit (skinned .glb, optionally with the playing clip).
    bool hasExportSelection() const override;
    void exportSelection() override;
    void exportSelectionToLast() override;
    QString exportNoun() const override { return QStringLiteral("selected look"); }
    // Animation-library export (rig + selected clips, no mesh) surfaced as its own contextual item
    // in the top Export menu — reuses the anim-export hook with a Wardrobe-specific label.
    bool    hasAnimExport() const override;
    QString animExportLabel() const override { return QStringLiteral("Export animation library (.glb)…"); }
    void    exportAnimations() override;

private:
    void populateSlots();    // fill the slot combos for the current class/gender
    void rebuildOutfit() { rebuildOutfitImpl(false); }   // synchronous (init/restore/look-apply)
    void rebuildOutfitImpl(bool async);                  // async=true → decode on a worker thread
    void applyOutfit(const WardrobeBuildCtx& ctx, const WardrobeOutfitMaps& M);   // main-thread apply
    void scheduleRebuild();  // coalesce rapid interactive changes into one (async) rebuild
    QTimer* m_rebuildTimer = nullptr;   // debounce timer for scheduleRebuild()
    int     m_buildGen = 0;             // generation token → discard stale async results
    // Bounded, temporary VRAM-alike texture pool: caches RAW (pre-recolour) decodes keyed by
    // role|material so a single-slot change reuses unchanged materials' BC-decompressed images
    // instead of re-decoding the whole ensemble. Colour-independent (skin/eye/dye recolour is
    // applied afterward on a copy, never mutating the cached original), hard-capped LRU so it
    // never bloats, and gated by the Settings > Performance toggle. Mutex-guarded for the async
    // worker path.
    QCache<QString, QImage> m_texCache;
    QMutex                  m_texCacheMutex;
    void applyDye();         // push the selected pigment across the whole outfit
    void rebuildDyeCombo();  // (re)fill the dye dropdown from real DyeDefinitions
    void ensureWeaponIndex();// enumerate weapon appearances by name prefix (data-driven)
    void populateWeapons();
    static QString weaponKeyOf(const QComboBox* cb);   // unambiguous settings value for a weapon slot  // fill the weapon type/model combos (class-filtered)
    QString baseAppJsonPath() const;   // current class/gender base body .app.json path (case-resolved)
    // Real game attach data: body hardpoint hash → (bone index, bone-local TRS, z-up).
    QHash<quint32, QPair<int, std::array<float, 16>>> loadBodyHardpoints(const QString& d4);
    // Seat a parsed weapon into a hand (0=main, 1=off) using the body hardpoint + the
    // weapon ItemType grip offset; transforms the weapon verts/normals into world space.
    // forceHash != 0 seats the weapon rigidly at that exact body hardpoint (used for the
    // sheathed slots), bypassing the in-hand grip-offset/mirror pick and the held-roll fix.
    void seatWeapon(ModelGeometry& wgeo, int hand, const QString& itemType, const QString& gender,
                    const QHash<quint32, QPair<int, std::array<float, 16>>>& hp, QString& dbg,
                    quint32 forceHash = 0,
                    int* outBone = nullptr, std::array<float,16>* outMz = nullptr,
                    bool bake = true);
    void populateCreator();  // fill the 9 character-creator pickers for this class/gender
    void resetDefaults();    // reset every wardrobe selection to defaults
    void populateSets();     // group slot appearances into m_sets (feeds right-click Equip Theme)
    QString themeToken(const QString& appearanceName) const;   // middle id (e.g. "stor007")
    enum ThemeScope { ThemeAll, ThemeArmor, ThemeMarkings, ThemeWeapons };   // right-click theme actions
    struct ThemeResolved { QMap<int, QString> armor;    // slot -> appearance name
                           QMap<int, QString> creator;  // creator category -> stem
                           QStringList weapons; };       // weapon appearance names
    ThemeResolved resolveTheme(int sno, const QString& appearanceName);   // read-only: what a theme would equip
    QString themeNames(const ThemeResolved& r, ThemeScope scope);          // readable preview for the menu
    void equipTheme(int sno, const QString& appearanceName, ThemeScope scope);
    void restoreSelection(); // restore saved class/gender on load
    // ── Outfit undo (Ctrl+Z): a small history of look-states (the wardrobeLookKeys set). Each
    // rebuild snapshots the PREVIOUS state; undo re-applies it through the same restore chain
    // loadLook uses. ──
    QHash<QString, QVariant> snapshotLookState() const;
    void undoLook();
    QVector<QHash<QString, QVariant>> m_undoStack;   // oldest → newest previous-states (cap 30)
    QHash<QString, QVariant> m_lastLookState;        // state as of the last completed rebuild
    bool m_undoApplying = false;                     // suppress history pushes while undoing
    void exportAnimLibrary(bool toLast = false);  // rig + selected clips only (no mesh); toLast → remembered dir
    void exportItemModel(int sno, const QString& name, bool toLast = false);  // one item's model → .glb
    void onSettingsChanged() override;   // re-apply nude/sidebar settings live
    void onSettingsLiveChanged(bool rebuild) override;   // per-toggle live apply
    void rebuildPartList();  // (re)build the per-part visibility tree (grouped by piece)
    void recomputePartVisibility();        // tree checks ∧ FX/SIM toggles → visibility
    QList<int> primitivesOf(QTreeWidgetItem* it) const;
    QList<int> selectedParts() const;
    qint64 texSnoFor(const QString& texName);   // resolve a texture name → SNO (lazy index)

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void showEvent(QShowEvent* ev) override;
    void hideEvent(QHideEvent* ev) override;   // persist camera state for next launch

private:

    QString classPrefix() const;        // "<code><gender>" lowercase, e.g. "barf"
    // ── Auto Animate ──
    int  wardrobeWeaponClass() const;   // the game's eWeaponClass for the equipped hands
    void autoAnimateForLoadout();       // play the game's own wardrobe unsheathe -> idle
    void maybeAutoAnimate();            // after a rebuild: act only if the weapon class moved
    QString m_autoFollowClip;           // queued behind the current one-shot (see tickAnimation)
    int  m_lastAutoWeaponClass = -2;    // -2 = never computed; the first rebuild only arms it

    QComboBox* m_class  = nullptr;
    QComboBox* m_gender = nullptr;
    QWidget*     m_ensemblePanel = nullptr;   // "Ensembles" panel (saved full-character looks)
    QListWidget* m_ensembleList  = nullptr;   // tile grid of saved ensembles
    QString      m_activeLook;                // name of the currently-loaded ensemble (highlighted)
    QComboBox* m_slot[5] = {};   // HLM, TRS, GLV, LEG, BTS (hidden backing state for the icon grid)
    // Equipment icon-grid view: slot cells (each shows its equipped icon) + a per-slot grid of
    // real appearance icons. The grid drives the hidden m_slot[] combos (no rebuild-path changes).
    // Slots 0-4 = armour (m_slot[]), 5 = main weapon, 6 = off weapon,
    // 7 = sheathed main, 8 = sheathed off, 9 = back trophy (player back cosmetic).
    static constexpr int kSlotCount = 10;
    QToolButton* m_slotCells[kSlotCount] = {};
    // ── Per-slot pigments (Set Look / Set Pigment) ──
    struct SlotDye { QString name; QStringList hex; };   // chosen dye name + its 4 hex colours
    SlotDye      m_slotDye[kSlotCount];      // per-slot pigment ("" name = use global/none)
    QVector<int> m_partSlot;                 // per-merged-primitive slot tag (from rebuildOutfit)
    bool         m_pigmentMode = false;      // false = Set Look (appearances), true = Set Pigment (dyes)
    QPushButton* m_lookModeBtn = nullptr;    // "Set Look" toggle
    QPushButton* m_pigmentModeBtn = nullptr; // "Set Pigment" toggle
    void setPigmentMode(bool on);            // switch the grid between looks and pigments
    void fillPigmentGrid();                  // fill the grid with preset dye swatches for the active slot
    void applyAllDyes();                     // recompute + push per-part pigments to the view
    void restoreSlotDyes();                  // load each slot's saved pigment from settings
    // ── Custom pigment picker (ported from the Models tab) ──
    QWidget*     m_pigmentPanel = nullptr;   // region slots + colour wheel + hex + memory
    QCheckBox*   m_applyAllDye = nullptr;     // "Apply to all slots" toggle
    int          m_dyeRegion = 0;             // which DyeMask zone (0..3) the picker edits
    QToolButton* m_dyeRegionBtn[4] = {};
    QWidget*     m_dyeWheel = nullptr;        // DyeColorWheel
    QLabel*      m_dyeSwatch = nullptr;
    QLineEdit*   m_dyeHex = nullptr;
    QToolButton* m_dyeMem[8] = {};            // memorised colour swatches
    QPoint       m_dyeDragStart;              // press point for slot/memory drag
    void buildPigmentPanel();                // construct the custom picker (once)
    void refreshPigmentPanel();              // load the active slot's 4 colours into the picker
    void loadDyeRegionToPicker(int r);       // active region colour → wheel / hex / swatch
    void applyDyePicker();                    // wheel → active region colour (live)
    void setDyeSlotColor(int r, const QColor& c);   // set a region (drop / memory / reset / hex)
    void writeRegionColor(int r, const QColor& c);  // store region colour on active slot (or all) + persist
    void applyPresetDye(const QString& name, const QStringList& hex, bool allSlots, int slot = -1);  // preset → slot(s)
    void styleDyeRegionBtn(int r);           // restyle a region button from its colour
    QStringList slotHex(int slot) const;      // the 4 hex colours for a slot (white default)
    bool applyAllSlots() const;               // is the "apply to all slots" toggle on?
    SlotDye      m_dyeClip;                    // copy/paste pigment clipboard (right-click a slot)
    bool         m_dyeClipSet = false;
    bool         m_slotDyeable[kSlotCount] = {};   // does the equipped piece have a dyeable material?
    // Look grid: a QGridLayout of QToolButtons (the slot cells use the same widget and render
    // reliably; a QListWidget in IconMode nested in the scroll area did not).
    QScrollArea*  m_lookScroll = nullptr;
    QWidget*      m_lookContent = nullptr;
    QGridLayout*  m_lookLayout = nullptr;
    QButtonGroup* m_lookGroup = nullptr;
    QLabel*      m_lookHeader = nullptr;
    QLabel*      m_iconProgress = nullptr;   // "Indexing icons… N%" while the atlas index builds
    int          m_activeSlot = 0;
    int          m_lookCols = 0;             // current look-grid column count (for resize reflow)
    // Equipment browser: search + collection filter + lazy (chunked) card build.
    QLineEdit*   m_lookSearch = nullptr;
    QComboBox*   m_lookCollFilter = nullptr;
    QString      m_lookFilter;               // lowercased search text
    int          m_lookCollSlot = -1;        // slot the collection dropdown was last built for
    QVector<int> m_lookItems;                // filtered combo indices to display (in order)
    int          m_lookBuildPos = 0;         // next index into m_lookItems still to build
    int          m_lookBuildRow = 0, m_lookBuildCol = 0;
    QString      m_lookBuildGroup;           // current weapon-type divider group while building
    void appendLookCards(int maxCards);      // build the next chunk of look cards
    void rebuildLookCollections();           // repopulate the collection dropdown for the active slot
    QComboBox*   slotCombo(int i) const;     // backing combo for slot i (armour or weapon)
    int          slotItem(int i, QString* fullName = nullptr) const;   // equipped sno (0 = empty)
    // Image / export / copy actions for one equipped item — shared by the slot cells and the
    // look-grid cards so the two menus cannot drift apart.
    void         addItemActions(QMenu& menu, int sno, const QString& fullName);
    QString      slotLabel(int i) const;     // "Helm".."Boots", "Main", "Off"
    void updateWeaponSlotAvailability();     // grey weapon slots the class can't fill (data-driven)
    bool m_lastClassRestrict = true;         // last applied "class restricted" state (live-toggle diff)
    void selectSlot(int i);      // make slot i active → refill the look grid
    void fillLookGrid();         // populate the look grid from m_slot[m_activeSlot]'s items
    void refreshLookSelection(); // re-check the active card after an equip (no rebuild -> keeps scroll)
    ThemeResolved m_activeTheme; // theme of the last-selected item, for "matching" highlights
    void setActiveTheme(int sno, const QString& appearanceName);   // resolve + re-highlight grids
    void refreshLookHighlights();    // outline look cards that belong to the active theme
    void refreshCreatorHighlights(); // outline creator cards that belong to the active theme
    QString markingAuthoredColorStem() const;   // the selected Marking's own snoDefaultColor
    void    updateMarkingDefaultLabel();        // name it in the Marking colour "(default)" entry
    void refreshSlotCells();     // update each slot cell's equipped icon
    void updateLookHeader();     // "<SLOT> (N) — <title> · <collection> · <filename>"
    // Appearance page: the 9 creator categories as a cell grid + a per-category card browser
    // (mirrors the Equipment page; drives the hidden m_creator[] combos).
    QToolButton* m_creatorCells[9] = {};
    QScrollArea*  m_creatorScroll = nullptr;
    QWidget*      m_creatorContent = nullptr;
    QGridLayout*  m_creatorLayout = nullptr;
    QButtonGroup* m_creatorGroup = nullptr;
    QLabel*       m_creatorHeader = nullptr;
    int           m_activeCreator = 0;
    int           m_creatorCols = 0;          // current creator-grid column count (resize reflow)
    QLineEdit*    m_creatorSearch = nullptr;  // Appearance page search box
    QComboBox*    m_creatorSort = nullptr;    // sort order (Name A–Z / Z–A)
    QString       m_creatorFilter;            // lowercased creator search text
    void selectCreator(int i);
    void fillCreatorGrid();
    void updateCreatorHeader();   // "<Category> (count) - <selected>"
    // Arrow-key move+apply within a card grid (display order). Returns true if it consumed the key.
    bool navGrid(QScrollArea* scroll, QGridLayout* layout, int cols, int key);
    // Look grid: arrows move a highlight cursor (no rebuild); Enter/Space equips it.
    QAbstractButton* m_lookCursor = nullptr;
    bool navLookGrid(int key);
    void refreshCreatorCells();
    QImage creatorIconImage(int cat, const QString& stem) const;   // hIconImage / tIcons / colour swatch
    QComboBox* m_dyeCombo = nullptr;   // None + real player dyes (4-colour pigments)
    QComboBox* m_weaponType = nullptr; // main-hand weapon type
    QComboBox* m_weapon = nullptr;     // main-hand weapon model within the chosen type
    QComboBox* m_weaponType2 = nullptr;// off-hand weapon type (dual-wield / shield / focus)
    QComboBox* m_weapon2 = nullptr;    // off-hand weapon model
    QComboBox* m_weapon3 = nullptr;    // sheathed main-hand weapon (seated on its body sheath socket)
    QComboBox* m_weapon4 = nullptr;    // sheathed off-hand weapon
    QComboBox* m_backTrophy = nullptr; // back trophy (player back cosmetic, ItemType CosmeticBack)
    QMap<QString, QMap<int, QString>> m_sets;   // set label → (slot → appearance name)
    QComboBox* m_creator[9] = {};      // character-creator category pickers
    QComboBox* m_skinTone = nullptr;   // skin-tone picker (arSkinColorChoices)
    QComboBox* m_skinDetail = nullptr; // skin-detail overlay (None / Freckle / Vitiligo)
    QHash<QString, int> m_texSno;      // lazy texture-name → SNO map (group 44)
    QComboBox* m_env    = nullptr;
    QSlider*   m_fovSlider = nullptr;  // camera field-of-view
    QVector<int> m_partFx, m_partSim, m_partForm;  // per merged-part FX/SIM/FORM flags (for visibility)
    QVector<int> m_partCovered;        // per merged-part: base-body region hidden by equipped armour
    QVector<int> m_partEye;            // per merged-part: 1 = eyeball (Hero_Eye shader / eyeball mat)
    QStringList m_partSource;          // per merged-part source piece name (for grouping)
    QVector<int> m_partSourceSno;      // per merged-part source appearance SNO (context-menu export)
    // One attached model that kept its own rig and can therefore be animated. Several are live at
    // once — back item, main hand, off hand — and each plays its own clip on its own clock.
    struct Attached {
        QString appearance;   // appearance stem; the key everything else joins on
        QString label;        // localized item name for the panel, else the stem
        QString slot;         // "Back" / "Main hand" / "Off hand" — which socket it came from
        quint32 salt = 0;     // the bone-hash salt used when attaching; clips need the same one
        // The rig BEFORE its bones were re-hashed. Clips must be decoded against these original
        // hashes or the decoder substitutes a zero rest pose for every bone.
        QVector<ModelJoint> preSalt;
    };
    QVector<Attached> m_attached;
    // Mirrors GLModelWidget::AttachRange. Kept local so this header does not have to include the
    // viewport just to name a 4-field struct; converted at the one call site.
    struct AttachSpan { int from = 0; int count = 0; int frames = 0; float fps = 30.0f; };
    QVector<AttachSpan> m_btAttachRanges;
    // ATTACHED panel: the attached model's own clip list + transport.
    QLabel*      m_attachSep    = nullptr;   // "ATTACHED" rule inside the ANIMATIONS panel
    QListWidget* m_attachList   = nullptr;   // its clips, grouped by attachment
    bool         m_attachJustSelected = false;   // press already handled it; ignore the release
    void fillAttachList();          // attachments' clips, pinned under the character's
    void setAttachClip(const QString& appearance, const QString& clip);   // empty clip = stop it
    QTreeWidget* m_partTree = nullptr; // per-part visibility tree (piece → submeshes)
    QWidget* m_sidebar = nullptr;      // right-side panel column (strip + splitter)
    QSplitter* m_rsplit = nullptr;     // vertical splitter: the visible panels, drag to resize
    // ── Stacking toggle panels — same system as the Models tab (see PanelBox.h) ──
    QVector<PanelBox*>    m_rsections;   // REGISTRATION order — index == page id
    QVector<QString>      m_sectKeys;    // per-panel settings key (label text mutates!)
    QVBoxLayout*          m_rstripLay = nullptr;   // the vertical icon strip's layout
    QWidget*              m_rstripW   = nullptr;   // the strip widget (collapse sizing)
    QVector<QToolButton*> m_rpageBtns;   // checkable show/hide toggles
    bool m_panelRestore = false;         // ctor is replaying a saved layout — don't save over it
    void showSidePanel(int page, bool on);   // strip toggle → panel in/out of the splitter
    void moveSidePanel(int page, int delta); // ▲▼ reorder within the splitter
    void saveSidePanelLayout();              // which panels, what order, what heights
    void updateSidebarCollapse();            // no panels up → sidebar shrinks to just the strip
    QToolButton* m_sideArrow = nullptr;      // ‹/› atop the strip — collapse the whole column
    bool m_sideCollapsed = false;            // true = only the arrow shows (strip + panels hidden)
    void setSideCollapsed(bool on);
    QWidget* m_animPanel   = nullptr;  // ANIMATIONS PLAYER (timeline + clip list)
    QTabWidget*  m_matTabs = nullptr;  // App Materials / SubObject Apps / Materials / Vertex Buffers
    QTreeWidget* m_appMatList = nullptr;   // App Materials (#, material, source, tris)
    QTreeWidget* m_subObjList = nullptr;   // SubObject Apps (#, material, source, tris, 2-sided)
    QTreeWidget* m_matList = nullptr;  // Materials (name / sno / flags / cloth) — drives preview
    QTreeWidget* m_vbList  = nullptr;  // Vertex Buffers (#, verts, tris, source)
    QTabWidget*  m_texTabs = nullptr;  // Textures / Values / Shaders
    QTreeWidget* m_matTexList = nullptr;   // Textures (role / sno / name)
    QTreeWidget* m_matValList = nullptr;   // Values (name / value)
    QTreeWidget* m_shaderList = nullptr;   // Shaders (field / value)
    QTreeWidget* m_detailList = nullptr;   // Detail maps (map / texture / N.Int / R.Int / R.Off)
    QLabel*  m_chanLbl[6] = {};        // TEXTURE PREVIEW channel thumbnails (square tiles)
    QLabel*  m_chanCap[6] = {};        // caption overlaid on each tile (hidden on hover)
    QImage   m_chanImg[6];             // full-res channel images (for hover popup)
    QLabel*  m_chanPreview = nullptr;  // zoomed hover popup (delayed, wheel-resizable)
    QTimer*  m_hoverTimer = nullptr;   // 0.5s hover delay before the popup
    int      m_hoverChan = -1;         // channel currently hovered
    int      m_previewPx = 256;        // popup size (wheel-resizable)
    QVector<int> m_partTris;           // per-part triangle count (for the hierarchy)
    QStringList m_matNames;            // unique material names (rows of m_matList)
    void populateMaterials();          // fill all debug tables from the assembled model
    void showMaterial(const QString& matName);   // fill textures/values/shaders + channel preview
    void previewTexture(const QString& texName, qint64 texSno);   // single-texture RGBA split
    void setChanTile(int c, const QImage& img);  // aspect-preserving tile fill (Models-tab style)
    void setChanCaptions(const char* const labels[6]);   // relabel the six tile captions
    void showChanPreview(int chan);    // popup the zoomed channel image (clamped to window)
    void applySidebars();              // show/hide each section per settings
    bool m_restoring = false;          // suppress rebuilds while restoring saved state
    bool m_rebuilding = false;         // re-entrancy guard: a widget signal fired during a
                                       // rebuild must not recursively re-enter rebuildOutfit
    bool m_framed = false;             // camera framed once; keep view on later rebuilds
    QLabel*    m_status = nullptr;
    QPushButton* m_copyDebugBtn = nullptr;   // "Copy debug" (hidden unless the log toggle is on)
    GLModelWidget* m_view = nullptr;
    bool m_loaded = false;
    QVBoxLayout* m_centerLayout = nullptr;  // the preview column layout
    // Fullscreen = maximize-in-place (same as Models): hide the side columns + toolbar chrome,
    // let the viewport fill the tab. No top-level window, no context loss, Esc or ✕ restores.
    QSplitter*   m_mainSplit  = nullptr;    // the 3-column splitter (left | centre | sidebar)
    QWidget*     m_viewBarW   = nullptr;    // centre: view toolbar container (hidden as one unit)
    QToolButton* m_fsBtn      = nullptr;    // "Fullscreen" ⟷ "Exit"
    bool         m_viewMaxed  = false;      // true = chrome hidden, viewport fills the tab
    QShortcut*   m_fsEsc      = nullptr;    // Esc → restore (only enabled while maximized)
    QToolButton* m_fsExitBtn  = nullptr;    // floating "✕ Exit" while maximized
    void toggleFullscreen();                // maximize/restore the preview in place
    bool         m_d4View = false;          // "Camera Snap" on → camera zooms to the selected slot
    bool         m_camFollow = false;       // "Follow animation" on → keep re-centring on the slot each frame
    bool         m_hoverSnap = false;       // "Snap to slot on hover" opt-in (frame slot while hovering its cell)
    void saveCameraState();                 // persist orbit/zoom for "remember camera on relaunch"
    void restoreCameraState();              // re-apply the saved camera (deferred, after the model loads)
    // Full-character "Look" presets: snapshot/restore the whole config (class, gender, equipment,
    // appearance, skin, dyes, weapons) as a named preset in QSettings.
    void saveLook(const QString& name);
    void loadLook(const QString& name);
    void buildEnsemblePanel();              // build the Ensembles panel (list + Save/Overwrite/Delete/Rename)
    void refreshEnsembles();                // repopulate the ensemble list (icon stacks) from saved names
    QPixmap ensembleIconStrip(const QString& pfx) const;   // composite appearance + gear icon strip
    void deleteEnsemble(const QString& name);
    void renameEnsemble(const QString& oldName, const QString& newName);
    float        m_snapMargin = 0.06f;      // Camera Snap framing looseness (padding around the slot)
    // Zoom the camera onto a slot's equipped geometry. keepRotation = true preserves the
    // current orbit angle (only reposition/zoom); false sets the ¾ hero angle.
    void frameSlot(int slot, bool animate, bool keepRotation);
    // Frame the parts a theme scope touched: armour slots, weapon slots, torso/body for
    // markings, or the whole model for ThemeAll. Used after equipping a theme.
    void frameThemeScope(ThemeScope scope);

    // ── Camera popup (snap/follow/FOV/presets/turntable — next to Preview Settings) ──
    QToolButton* m_camBtn   = nullptr;      // "Camera" popup trigger
    QFrame*      m_camPanel  = nullptr;
    void toggleCameraPanel();
    void buildCameraPanel();

    // ── Models-tab-style preview controls (copied over) ──────────────────────
    QToolButton* m_vpBtn = nullptr;        // "Preview Settings" popup trigger
    QFrame*  m_vpPanel = nullptr;          // shading / exposure / background popup
    QVector<QToolButton*> m_viewToggleBtns;// Skeleton (hidden state-holder) / FX / SIM / FORM
    class QComboBox* m_channelCombo = nullptr;   // material-channel viewer (Shaded/Normal/Rough/…)
    void togglePreviewPanel();
    void buildPreviewPanel();
    // "Lighting" popup (three-point rig from D4's real character-screen light values).
    QToolButton* m_lightBtn = nullptr;
    QFrame*  m_lightPanel = nullptr;
    void toggleLightingPanel();
    void buildLightingPanel();
    void applyLightRig();                  // read wardrobe2/light/* → m_view->setLightRig
    void loadReflectionProbe();            // load D4's character-screen reflection cubemap → m_view
    bool m_reflLoaded = false;             // probe load attempted once (after CASC is ready)
    // "Shaders" popup (fur shell settings; sits between Preview Settings and Physics).
    QToolButton* m_shaderBtn = nullptr;
    QFrame*  m_shaderPanel = nullptr;
    void toggleShaderPanel();
    void buildShaderPanel();
    // "Detail maps" popup — experiment surface for the detail-map selection rule (dev tool, global;
    // shown only in Developer mode). Render path stays via applyDetailConfig() regardless.
    QToolButton* m_detailBtn = nullptr;
    QFrame*  m_detailPanel = nullptr;
    QWidget* m_manualDetailBox = nullptr;   // manual-override controls (greyed out while Auto is on)
    void toggleDetailPanel();
    void buildDetailPanel();
    void applyDetailConfig();               // load wardrobe2/detail/* → m_view->setDetailConfig
    QString detailConfigText() const;       // human-readable dump for Copy config
    // (The Rig button/popup is retired — its toggles live in the Overlays ▾ panel now, same as
    // the Models tab. m_rigChk* below ARE that panel's checkboxes; applyRigToggle still mirrors.)
    QWidget* m_vpStrip = nullptr;           // N-strip: settings-popover buttons on the viewport edge
    QPoint panelPosLeftOf(QWidget* anchor, const QSize& sz) const;   // N-strip popups open LEFTward
    QToolButton* m_shadeMoreBtn = nullptr;  // shading ⌄ — wheel over it cycles the channel
    QToolButton* m_overlayBtn   = nullptr;  // Overlays master toggle (view toolbar)
    QFrame*      m_overlayPanel = nullptr;  // persistent overlay-settings popup (▾ arrow)
    bool         m_overlaysOn   = true;     // master gate — false forces every guide off
    QVector<QPair<QCheckBox*, std::function<void(bool)>>> m_overlayChks;   // box → its GL applier
    // Central setter that keeps every mirror of a view-debug flag in sync (settings + view + all
    // duplicate checkboxes/buttons), so toggling skeleton/phys-bones/axis anywhere can't desync.
    void applyRigToggle(const QString& key, bool on);
    QToolButton* m_skelToggleBtn = nullptr;   // the always-visible centre "Skeleton" toggle (mirrored)
    // (Retired with the physics panel's "Preview" section — those overlays live in the viewport's
    //  Overlays popup. Kept as null members so applyRigToggle's mirror() calls stay valid.)
    QCheckBox* m_physChkBones = nullptr;
    QCheckBox* m_physChkAxis  = nullptr;
    QCheckBox* m_rigChkSkel = nullptr; QCheckBox* m_rigChkPhys = nullptr; QCheckBox* m_rigChkAxis = nullptr;
    QCheckBox* m_rigChkNames = nullptr; QCheckBox* m_rigChkTrans = nullptr;   // Rig-panel checkboxes
    QCheckBox* m_rigChkHideUnk = nullptr;   // Rig-panel "Hide unnamed bones"
    // Live cloth-physics tuning popup (debug).
    QToolButton* m_physBtn = nullptr;
    QFrame*  m_physPanel = nullptr;
    void togglePhysicsPanel();
    void buildPhysicsPanel();
    // Physics-slider registry (for live game-value display + the Unlocked-limits toggle).
    struct PhysSlider { QSlider* sld = nullptr; QLineEdit* val = nullptr; QString key;
                        double scale = 1.0; int lo = 0; int hi = 0; bool game = false; };
    QVector<PhysSlider> m_physSliders;
    QCheckBox*  m_physUnlocked = nullptr;   // "Unlocked limits" — widen all slider ranges
    void refreshGameDrivenSliders();        // show the real .clt values on the greyed game rows
    void applyUnlockedLimits(bool on);      // widen / restore every physics slider's range
    // Eye-map compositor (EyeColor def → base/normal/orm/emissive), from real Hero_Eye data.
    void composeEyeMaps(const QString& d4, const QString& stem, QImage& base, QImage& norm,
                        QImage& orm, QImage& emis, float& emisMul, float& irisRough);
    void applyClothParams();               // read wardrobe/cloth/* → m_view->setClothParams
    // viewport AND parts panel. `groupPart` is any part belonging to the group whose HEADER was
    // right-clicked: part stays -1 (no single part was picked) but the model-level actions still
    // scope to that group's source item instead of falling back to the whole assembly.
    void showPartContextMenu(int part, const QPoint& globalPos, int groupPart = -1);
    QVector<int> visibleParts() const;                          // parts drawn right now
    QVector<int> partsOfSource(int part) const;                 // parts sharing one source item
    void exportPartsSubset(const QVector<int>& parts, const QString& label, bool toLast);
    void reapplyOverlays();        // re-push ALL overlay state (master gate + each box)
    void loadClothTuning();                // read the equipped pieces' real Cloth/*.clt.json params
    void fillClothSimTuning(ModelGeometry& geo);   // per-ClothSim tuning from its Cloth/<name>_sim.clt.json
    bool  m_clothTuningFound = false;      // any equipped piece referenced a Cloth definition
    // Averaged authored NvCloth tuning (dmClothTuningMirror) of the equipped cloth pieces.
    struct GameClothTuning {
        float boneTrack = 0.45f;   // flBoneTrackingFactor  — blend toward the skinned (bone) pose
        float actorTrack = 0.6f;   // flActorTrackingFactor — inherit the character's rigid motion
        float stretch = 0.7f;      // flStretchingStiffness
        float horiz = 0.5f;        // flHorizontalStiffness
        float shear = 0.15f;       // flShearStiffness
        float bend = 0.3f;         // flBendingStiffness
        float damping = 0.25f;     // flDampingFactor
        float attach = 0.2f;       // flAttachmentStiffness
        float friction = 1.0f;     // flFrictionScale (0..3 in data)
        float density = 2.0f;      // flDensity
        // Aerodynamics (dmClothTuningMirror). windDir is the authored vSelfWind direction
        // (unit, y-up); the panel's Wind slider scales it. drag/lift are 0..1 factors.
        float windDirX = 0.0f, windDirY = 0.0f, windDirZ = 0.0f;
        float drag = 0.0f;         // flDragFactor
        float lift = 0.0f;         // flLiftFactor
    } m_gct;
    // Animation timeline + list.
    QWidget*  m_timeline = nullptr;
    QPushButton* m_playBtn = nullptr;
    QSlider*  m_animSlider = nullptr;
    class QSpinBox* m_frameSpin = nullptr;   // current-frame field (Models-parity transport)
    QLabel*   m_frameMax = nullptr;          // "/ N" frame count
    QLabel*   m_timeLabel = nullptr;
    QComboBox* m_speedCombo = nullptr;
    QCheckBox* m_loopCheck = nullptr;
    QLineEdit* m_animSearch = nullptr;
    QComboBox* m_animFilter = nullptr;     // category filter (Idle / Walk / Attack / UI / …)
    QComboBox* m_animSort = nullptr;       // sort order (name / frames)
    QListWidget* m_anims = nullptr;
    QTimer*   m_animTimer = nullptr;
    float     m_animFps = 30.0f;
    QString   m_playingAnim;
    bool      m_animJustSelected = false;   // selection changed on this click → don't let the
                                            // release's itemClicked toggle it back off
    QHash<QString, QStringList> m_animCache;   // class prefix → anim rows
    void populateAnims();                  // list animations for the current body rig
    void fillAnimList();                   // (re)build the list applying filter + sort + search
    void playAnimByName(const QString& animName);
    AnimParser::DecodedAnim decodeAnimByName(const QString& animName);
    // Appends every attachment's selected clip (each salted to match its own sub-rig).
    void mergeAttachmentClips(AnimParser::DecodedAnim& anim);
    // The clip the user picked for this attachment, validated against what it actually owns.
    QString selectedClipFor(const QString& appearance) const;
    // D4_DUMP_TROPHYANIM=1: report a trophy's own rig and whose bones its clips drive.
    void dumpTrophyAnim(const QString& appr, const QVector<ModelJoint>& trophySkel);
    // Same, against an explicit rig — lets a clip be inspected before the merged rig exists.
    AnimParser::DecodedAnim decodeAnimForSkeleton(const QString& animName,
                                                  const QVector<ModelJoint>& skel);  // meta+payload → clip (invalid on failure)
    // Gather clips per Settings ▸ Export ▸ "Animations to embed": the playing clip, or
    // EVERY clip in the (filtered) ANIMATIONS list. Falls back to the selected list entry
    // when nothing is playing, so "Embed animation" never silently exports none.
    void collectExportAnims(QVector<AnimParser::DecodedAnim>& anims, QStringList& names);
    static QString exportMenuExtras(const QString& suffix);   // suffix minus "1 model"
    QString exportMenuSuffix(int sno, const QString& appr);   // "1 model + N anims + M raw" per settings
    bool exportOutfitGlb(const QString& path, const QVector<int>& keep = {});   // shared writer for the two outfit-export entry points
    void applyAnimSpeed();
    void tickAnimation();
    void clearAnimationSelection();
    void remapAnimationForRig();           // class/gender change → swap clip to the new rig (or clear)
    void showDebugConsole();               // scrollable debug log window (Copy debug) + copies to clipboard

    // Weapon index: ItemType name → (appearance name → appearance SNO). Built once.
    QMap<QString, QMap<QString, int>> m_weapByType;
    bool m_weapReady = false;
    bool m_weapBuilding = false;

    // Body skeleton from the most recent assembly (for weapon hardpoint attachment).
    QVector<ModelJoint> m_bodySkeleton;

    // Last assembled geometry + per-material export images (for the Export button).
    ModelGeometry m_lastMerged;
    QVector<QImage> m_expBase, m_expNorm, m_expOrm, m_expEmis;   // per-part maps retained for .glb export
    QVector<QImage> m_expDyeMask, m_expDyeRamp;                  // + dye mask/ramp for optional dye-bake on export
    QVector<QImage> m_expDetN[3], m_expDetR[3];                  // + detail normal/rough maps for detail-bake on export
    QVector<QVector3D> m_expDScale;                             // per-part detail tiling (x/y/z = layer 0/1/2)
    QVector<QVector4D> m_expDZoneMap, m_expDBands;              // per-part zone→layer + dye bands
    QVector<int>    m_expDMetalLayer;                           // per-part metal detail layer (-1 = none)
    QVector<float>  m_expDNInt, m_expDRInt, m_expDROff;         // per-part detail normal/rough intensity + rough offset
    QVector<float>  m_expEmisMul, m_expEmisCol;       // per-part emissive strength (1/part) + colour (3/part)
    AnimParser::DecodedAnim m_curAnim;                // decoded clip currently loaded (for export)
    QVector<ModelExporter::ExportMaterial> m_exportMats;
};
