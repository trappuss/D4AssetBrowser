#pragma once
#include <QWidget>
#include <QString>

class CascReader;
class SnoIndex;
class GLModelWidget;

// ── Shared look for panel / page / section TITLES ────────────────────────────────────────────
// Deliberately NEUTRAL: the gold accent means something specific in this app — active filter
// counts, pulled animation clips, authored-vs-live divergence, "update available". Titles are
// structure, not state, so accenting every one of them just made the panels shout. Kept here
// (rather than in one tab's header) so every tab's panels agree without cross-including.
inline constexpr const char* kHdrQss    = "color:#dedede;font-weight:bold;";
inline constexpr const char* kSubHdrQss = "color:#9a9a9a;font-weight:bold;";

// ── Shared control skin for the view/header toolbars (Models + Wardrobe) ────────────────────
// One visual language: controls #2b2b2b/#555/3px with the red hover, panels a shade darker
// (#232323, #5a5a5a, 4px). Everything here is a named constant so a new control can't drift —
// that drift is exactly how one tab's bar ended up unstyled while the other was themed.
// TEXT buttons — the 8px side padding is what makes a label breathe.
inline constexpr const char* kToolBtnQss =
    "QToolButton{padding:2px 8px;border:1px solid #555;border-radius:3px;"
    "background:#2b2b2b;color:#bbb;}"
    "QToolButton:hover{border-color:#b0453c;}"
    "QToolButton:checked{background:#8a1414;color:#fff;border-color:#a01818;}"
    "QToolButton::menu-indicator{width:0px;}";   // we draw our own ⌄ where we want one
// ICON-ONLY buttons — same skin, but 8px of side padding would eat most of a small button and
// force Qt to shrink the icon into the leftovers (that's exactly what made these unreadable).
inline constexpr const char* kIconBtnQss =
    "QToolButton{padding:1px;border:1px solid #555;border-radius:3px;"
    "background:#2b2b2b;color:#ddd;}"
    "QToolButton:hover{border-color:#b0453c;}"
    "QToolButton:checked{background:#8a1414;color:#fff;border-color:#a01818;}"
    "QToolButton::menu-indicator{width:0px;}";
// The slim ⌄ affordances: no padding at all, or the glyph has nowhere to draw.
inline constexpr const char* kArrowBtnQss =
    "QToolButton{padding:0px;border:1px solid #555;border-radius:3px;"
    "background:#2b2b2b;color:#ddd;font-size:13px;}"
    "QToolButton:hover{border-color:#b0453c;}"
    "QToolButton::menu-indicator{width:0px;}";
inline constexpr const char* kPanelQss =
    "QFrame{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
    "QLabel{color:#cccccc;border:none;} QCheckBox{color:#cccccc;border:none;}";
inline constexpr int kBarH = 26;   // uniform control height across the header + view toolbars

// Common base for the four module tabs. Plain QWidget (no Q_OBJECT here — derived
// tabs declare their own) so MainWindow can hold them uniformly: hand each one the
// shared CascReader + SnoIndex and call refresh() when it first becomes visible.
class BrowserTab : public QWidget {
public:
    using QWidget::QWidget;
    void setReader(CascReader* reader) { m_reader = reader; }
    void setIndex(SnoIndex* index)     { m_index = index; }
    virtual void refresh() {}
    virtual void reset() {}   // drop cached state so the next refresh() reloads
    virtual void onSettingsChanged() {}   // re-read settings after the Settings dialog closes
    // Apply a Wardrobe/panel setting the instant it's toggled in the Settings dialog (live),
    // without waiting for OK. `rebuild` = the change affects geometry/textures. Default: full re-read.
    virtual void onSettingsLiveChanged(bool /*rebuild*/) { onSettingsChanged(); }
    // Flush any per-tab view state (camera orbit/zoom/FOV) to settings. Called for every tab on
    // app close, since hideEvent doesn't reliably fire for the visible tab when the window closes.
    virtual void persistView() {}

    // ── Export hooks (used by the top-level Export menu) ─────────────────────────
    // A live 3D preview to capture (Save image / Turntable GIF / Anim-loop GIF), or null.
    virtual GLModelWidget* previewWidget() { return nullptr; }
    // Whether the tab currently has a selection it can export (models → .glb, textures → images).
    virtual bool hasExportSelection() const { return false; }
    virtual void exportSelection() {}         // prompt for a folder / save-as, then export
    virtual void exportSelectionToLast() {}   // re-export to the remembered folder
    // Context noun for the Export menu labels, e.g. "selected model(s)" / "selected texture(s)"
    // / "selected look" — count-aware where the tab knows it.
    virtual QString exportNoun() const { return QStringLiteral("selection"); }
    // Rig-only animation export (no mesh) — Models tab only. The menu item is shown only when
    // hasAnimExport() is true, labelled by animExportLabel() (singular/plural by selection).
    virtual bool    hasAnimExport() const { return false; }
    virtual QString animExportLabel() const { return QStringLiteral("Export animations only (.glb)…"); }
    virtual void    exportAnimations() {}
    // TexFrame export — Textures tab only. The two menu items show only when hasFrameExport() is true
    // (the selected texture has frames): "selected" exports the frame-list selection, "all" every frame.
    virtual bool    hasFrameExport() const { return false; }
    virtual void    exportFramesSelected() {}
    virtual void    exportFramesSelectedToLast() {}   // re-export the selection to the remembered dir
    virtual void    exportFramesAll() {}
    virtual void    exportFramesAllToLast() {}         // re-export every frame to the remembered dir
protected:
    CascReader* m_reader = nullptr;
    SnoIndex*   m_index  = nullptr;
};
