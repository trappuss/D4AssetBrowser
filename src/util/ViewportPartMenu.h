#pragma once
// Shared part context menu — used by the 3D VIEWPORT right-click AND the PARTS PANEL in
// Models / Wardrobe / Stable, so all six entry points offer the same actions in the same order.
//
// The three tabs previously built this menu independently and had drifted to the same three
// entries each — the identical failure mode the list/grid context menus had. One builder means
// adding an action lights it up everywhere at once.
//
// Each caller supplies only what it can resolve (a Models part has no owning outfit piece; a
// Stable part has no collection) and leaves the rest empty/null. Empty fields and null callbacks
// are omitted, so no menu ever shows an action it cannot perform. Labels carry their VALUE in
// parentheses — "Copy source name (barF_base03_TRS)" — so you can read what you are about to
// copy without invoking it.

#include <QAction>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QMenu>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>

// ── One vocabulary for every context menu in the app ────────────────────────────────────────────
// These labels were written independently per tab and drifted: the same clipboard action appeared
// as "Copy SNO id", "Copy SNO" and "Copy source SNO ID"; exporting to the remembered folder was
// "Export to last dir" in three tabs and "Export Model Last dir" in three others. Worst of the set,
// "Save image" saved silently to the last folder while "Save image…" opened a file dialog — one
// ellipsis apart, opposite behaviours.
//
// Rules, so additions stay consistent:
//   * Sentence case. "Export part", not "Export Part".
//   * A trailing "…" means AND ONLY MEANS "this opens a dialog". Never decorative.
//   * An action that writes somewhere remembered says where: "… to last folder (…/D4/exports)".
//   * Copy actions carry their value in parentheses via withValue(), so you can read what you are
//     about to copy without invoking it.
// Anything user-visible in a context menu belongs here rather than inline at the call site.
namespace MenuText {

inline const QString kCopySno        = QStringLiteral("Copy SNO");
inline const QString kCopyFileName   = QStringLiteral("Copy file name");
inline const QString kCopyName       = QStringLiteral("Copy name");
inline const QString kCopyCollection = QStringLiteral("Copy collection name");
// A submesh has NO name of its own in D4 — the format stores only dwSubObjectHash, and tNameInfo
// either restates the file name (gear) or is all zeros (monsters). Its material name is the only
// human-readable label there is, and it is what the outliner shows. So the action says MATERIAL:
// it was called "Copy part file name", which read as if it copied a file name for that part.
inline const QString kCopyPartMaterial = QStringLiteral("Copy part material name");

// No ellipsis baked in: these get suffixes appended ("Export model (1,234 tris)"), and an ellipsis
// stranded mid-label reads as a typo. Wrap the FINISHED string in prompts() instead.
inline const QString kExportModel     = QStringLiteral("Export model");
inline const QString kExportModelLast = QStringLiteral("Export model to last folder");
inline const QString kExportPart      = QStringLiteral("Export part");
inline const QString kExportPartLast  = QStringLiteral("Export part to last folder");

inline const QString kCopyImage    = QStringLiteral("Copy image");
inline const QString kSaveImage    = QStringLiteral("Save image…");              // prompts
inline const QString kSaveImageLast = QStringLiteral("Save image to last folder"); // silent

inline const QString kShowDeps = QStringLiteral("Show dependencies…");

// "C:/Users/me/Documents/D4/exports" → "…/D4/exports". A full path makes the menu unreadable; the
// last two components are what actually distinguishes one export folder from another.
inline QString condensePath(const QString& path)
{
    if (path.isEmpty()) return {};
    const QString clean = QDir::fromNativeSeparators(QDir::cleanPath(path));
    const QStringList parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() <= 2) return QDir::toNativeSeparators(clean);
    return QStringLiteral("…/%1/%2").arg(parts[parts.size() - 2], parts.last());
}

inline QString withCount(const QString& label, int n)
{
    return n > 0 ? QStringLiteral("%1 (%2 tris)").arg(label, QLocale().toString(n)) : label;
}

inline QString withValue(const QString& label, const QString& value)
{
    return value.isEmpty() ? label : QStringLiteral("%1 (%2)").arg(label, value);
}

// The ONLY way an ellipsis gets onto a label. Applied last, to the finished string.
inline QString prompts(const QString& label) { return label + QStringLiteral("…"); }

// List menus act on a COUNTED set of things that are not always models — "3 textures", "12 items".
// Blindly reusing kExportModel there produced "Export model… — 3 textures".
inline QString exportSetPrompt(const QString& what)
{
    return prompts(QStringLiteral("Export %1").arg(what));
}
inline QString exportSetLast(const QString& what, const QString& dir)
{
    return withValue(QStringLiteral("Export %1 to last folder").arg(what), dir);
}

}  // namespace MenuText

namespace ViewportPartMenu {

// What the caller resolved about the picked part. Empty strings / -1 = unavailable.
struct Info {
    int     part      = -1;        // primitive index (-1 = clicked empty space)
    QString sourceModel;           // owning MODEL name — the menu title's left half
    // A submesh has no authored name in D4 (only dwSubObjectHash), so BOTH of these are its
    // MATERIAL name — the same string the outliner labels the row with. Callers must pass the
    // resolved ROSTER name, not MeshPrimitive::materialName, which is "Material_<n>" placeholder
    // text nobody ever sees on screen.
    QString partName;              // the menu title's right half
    QString partFileName;          // the value "Copy part material name" copies
    QString sourceFileName;        // source model's file name (Copy)
    QString sourceName;            // human/display name of the source (Copy)
    QString collection;            // collection / set name (Copy)
    int     sno       = -1;        // source appearance SNO (Copy)
    int     partTris  = 0;         // triangles in THIS part
    int     modelTris = 0;         // triangles in the whole model
    QString lastExportDir;         // last-used export folder (shown condensed)
    bool    visible   = true;      // current viewport visibility of this part
    bool    isSim     = false;     // cloth simulation cage proxy
    bool    isFx      = false;     // FX submesh
};

// Leave a callback null to omit its action.
struct Actions {
    std::function<void()>     exportModelLastDir;  // whole model → last dir, no prompt
    std::function<void()>     exportModel;         // whole model → prompt
    std::function<void()>     exportPartLastDir;   // this part only → last dir, no prompt
    std::function<void()>     exportPart;          // this part only → prompt
    std::function<void()>     frame;               // point the camera at this part
    std::function<void()>     selectPart;          // select in the parts panel (no camera move)
    std::function<void(bool)> setVisible;
    std::function<void()>     isolate;
    std::function<void()>     showAll;
    std::function<void()>     hideAll;
    std::function<void()>     invert;
    // Called once the menu is dismissed, however it closes (action chosen, Esc, click-away).
    // Callers use it to drop the transient right-click outline: the menu highlights the part it
    // acts on, and without this the outline survived until the next right-click on empty space.
    std::function<void()>     closed;
};

inline void copyText(const QString& s)
{
    if (!s.isEmpty()) QGuiApplication::clipboard()->setText(s);
}

// The formatting helpers moved up into MenuText so the label vocabulary and the label FORMATTING
// live together. Re-exported here because five files already call them as ViewportPartMenu::…, and
// a using-declaration makes qualified lookup keep working without touching any of them.
using MenuText::condensePath;
using MenuText::withCount;
using MenuText::withValue;
using MenuText::prompts;

// Build and execute the menu at `globalPos`.
inline void exec(QWidget* parent, const QPoint& globalPos, const Info& in, const Actions& act)
{
    QMenu menu(parent);
    const bool hasPart = in.part >= 0;

    // ── Title: which model, which part ────────────────────────────────────────────────────
    if (hasPart) {
        QString part = in.partName.isEmpty() ? in.partFileName : in.partName;
        if (part.isEmpty()) part = QStringLiteral("part %1").arg(in.part);
        if (in.isSim) part += QStringLiteral("  [SIM]");
        if (in.isFx)  part += QStringLiteral("  [FX]");
        const QString title = in.sourceModel.isEmpty()
            ? part : QStringLiteral("%1  —  %2").arg(in.sourceModel, part);
        QAction* hdr = menu.addAction(title);
        hdr->setEnabled(false);
        menu.addSeparator();
    }

    // ── Export ────────────────────────────────────────────────────────────────────────────
    const QString dir = condensePath(in.lastExportDir);
    bool anyExport = false;
    if (act.exportModelLastDir && !dir.isEmpty()) {
        menu.addAction(withValue(MenuText::kExportModelLast, dir),
                       parent, act.exportModelLastDir);
        anyExport = true;
    }
    if (act.exportModel) {
        menu.addAction(prompts(withCount(MenuText::kExportModel, in.modelTris)),
                       parent, act.exportModel);
        anyExport = true;
    }
    if (hasPart && act.exportPartLastDir && !dir.isEmpty()) {
        menu.addAction(withValue(MenuText::kExportPartLast, dir),
                       parent, act.exportPartLastDir);
        anyExport = true;
    }
    if (hasPart && act.exportPart) {
        menu.addAction(prompts(withCount(MenuText::kExportPart, in.partTris)),
                       parent, act.exportPart);
        anyExport = true;
    }

    // ── Copy ──────────────────────────────────────────────────────────────────────────────
    // The whole block used to be gated on hasPart, so a right-click with no part under the cursor —
    // empty viewport space, or the outliner's ROOT row, which IS the loaded model — offered no way
    // to copy the model's own name or SNO even though all four values were sitting right there.
    // Only "Copy part file name" is genuinely part-scoped.
    {
        const bool anyCopy = (hasPart && !in.partFileName.isEmpty())
                          || !in.sourceFileName.isEmpty() || in.sno > 0
                          || !in.sourceName.isEmpty() || !in.collection.isEmpty();
        if (anyCopy) {
            if (anyExport) menu.addSeparator();
            // The part's MATERIAL name stays distinct because it really is a different string from
            // the model's. The other four are the SAME values the browse-row menus copy, so they
            // carry the same four labels; "source" only ever meant "the model this part came
            // from", which the disabled title above already says.
            if (hasPart && !in.partFileName.isEmpty())
                menu.addAction(withValue(MenuText::kCopyPartMaterial, in.partFileName),
                               parent, [n = in.partFileName] { copyText(n); });
            if (!in.sourceFileName.isEmpty())
                menu.addAction(withValue(MenuText::kCopyFileName, in.sourceFileName),
                               parent, [n = in.sourceFileName] { copyText(n); });
            if (in.sno > 0)
                menu.addAction(withValue(MenuText::kCopySno, QString::number(in.sno)),
                               parent, [s = in.sno] { copyText(QString::number(s)); });
            if (!in.sourceName.isEmpty())
                menu.addAction(withValue(MenuText::kCopyName, in.sourceName),
                               parent, [n = in.sourceName] { copyText(n); });
            if (!in.collection.isEmpty())
                menu.addAction(withValue(MenuText::kCopyCollection, in.collection),
                               parent, [n = in.collection] { copyText(n); });
        }
    }

    // ── This part ─────────────────────────────────────────────────────────────────────────
    if (hasPart && (act.frame || act.selectPart || act.setVisible || act.isolate)) {
        menu.addSeparator();
        if (act.frame)      menu.addAction(QStringLiteral("Frame part"), parent, act.frame);
        if (act.selectPart) menu.addAction(QStringLiteral("Select part"), parent, act.selectPart);
        if (act.setVisible)
            menu.addAction(in.visible ? QStringLiteral("Hide part") : QStringLiteral("Show part"),
                           parent, [act, v = in.visible] { act.setVisible(!v); });
        if (act.isolate)    menu.addAction(QStringLiteral("Isolate part"), parent, act.isolate);
    }

    // ── All parts (works with nothing under the cursor too) ───────────────────────────────
    if (act.showAll || act.hideAll || act.invert) {
        menu.addSeparator();
        if (act.showAll) menu.addAction(QStringLiteral("Show all"), parent, act.showAll);
        if (act.hideAll) menu.addAction(QStringLiteral("Hide all"), parent, act.hideAll);
        if (act.invert)  menu.addAction(QStringLiteral("Invert"), parent, act.invert);
    }

    if (!menu.isEmpty()) menu.exec(globalPos);   // blocking: returns once the menu is dismissed
    if (act.closed) act.closed();                // fires even for an empty menu, so state always clears
}

}  // namespace ViewportPartMenu
