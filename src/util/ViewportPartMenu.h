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
#include <QPair>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QVector>
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
inline const QString kCopyMaterialName = QStringLiteral("Copy material name");
// The piece the part hangs under — the parts panel's PARENT row (PalF_sets50_LEG), where the
// material above is the CHILD row (PalM_sets50_LEG_mat). Both rows are things you want on the
// clipboard, and the menu used to offer only the material, under a label ("Copy part material
// name") that read as one run-on action for two different strings.
inline const QString kCopyPartName     = QStringLiteral("Copy part name");

// No ellipsis baked in: these get suffixes appended ("Export model (1,234 tris)"), and an ellipsis
// stranded mid-label reads as a typo. Wrap the FINISHED string in prompts() instead.
inline const QString kExportModel     = QStringLiteral("Export model");
inline const QString kExportModelLast = QStringLiteral("Export model to last folder");
// Built with verbParts() at the call site so the count lands in the noun: kExportPartVerb is
// "Export", giving "Export part" or "Export 3 parts".
inline const QString kExportPartVerb     = QStringLiteral("Export");
inline const QString kExportPartLastVerb = QStringLiteral("Export");

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

// "part" / "3 parts". One helper rather than a ternary at each of the seven sites, because seven
// hand-written plurals is how "Export 1 parts" ships.
inline QString parts(int n)
{
    return n == 1 ? QStringLiteral("part") : QStringLiteral("%1 parts").arg(QLocale().toString(n));
}

// "Export part" / "Export 3 parts". The verb stays put and only the noun changes, so a menu read
// top to bottom keeps its column of verbs.
inline QString verbParts(const QString& verb, int n)
{
    return QStringLiteral("%1 %2").arg(verb, parts(n));
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
    QString partMaterial;          // the value "Copy material name" copies
    // The parts panel's PARENT row for this part — the equipped piece / outfit item it belongs to.
    // Left empty by callers whose parts have no owning piece distinct from the model itself (the
    // Models tab), so "Copy part name" simply does not appear there rather than copying the model
    // name under a label that claims otherwise.
    QString partOwner;             // the value "Copy part name" copies
    // The SET this menu acts on. Empty means "just `part`" — every caller that has no concept of a
    // multi-selection can ignore all three of these and keeps the single-part wording.
    //
    // It exists because the menu was describing the part under the cursor while the actions ran on
    // the whole selection: "Export part (1,234 tris)…" then wrote eleven parts and 40,000 triangles.
    // A label that under-reports what it is about to do is worse than no label.
    QVector<int> selParts;         // acted-on parts; size drives every plural below
    int          selTris = 0;      // their combined triangle count
    QStringList  selMaterials;     // their material names, in selParts order, caller-deduped
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
    // "Why does this look like that?" — the roster source, the material, which of its values are
    // authored and which are stand-ins, and which texture definitions resolve. Lives on the shared
    // menu rather than in one tab because every tab renders the same materials the same way, and
    // the failures this answers were reported against three different tabs.
    std::function<void()>     explainMaterial;
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
    // How many parts the actions below will touch. 0 when nothing is under the cursor.
    const int nSel = !in.selParts.isEmpty() ? int(in.selParts.size()) : (hasPart ? 1 : 0);

    if (hasPart) {
        QString part;
        if (nSel > 1) {
            // The one material name is no longer the truth; say the count instead. The [SIM]/[FX]
            // tags are dropped for the same reason — they describe the clicked part only.
            part = QStringLiteral("%1 selected").arg(MenuText::parts(nSel));
        } else {
            part = in.partName.isEmpty() ? in.partMaterial : in.partName;
            if (part.isEmpty()) part = QStringLiteral("part %1").arg(in.part);
            if (in.isSim) part += QStringLiteral("  [SIM]");
            if (in.isFx)  part += QStringLiteral("  [FX]");
        }
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
    // Both the noun and the triangle count follow the selection. selTris is used whenever a set
    // was supplied, because partTris is the CLICKED part's count and would under-report the rest.
    const int partTris = (nSel > 1 && in.selTris > 0) ? in.selTris : in.partTris;
    if (hasPart && act.exportPartLastDir && !dir.isEmpty()) {
        menu.addAction(withValue(MenuText::verbParts(MenuText::kExportPartLastVerb, nSel)
                                     + QStringLiteral(" to last folder"), dir),
                       parent, act.exportPartLastDir);
        anyExport = true;
    }
    if (hasPart && act.exportPart) {
        menu.addAction(prompts(withCount(MenuText::verbParts(MenuText::kExportPartVerb, nSel),
                                         partTris)),
                       parent, act.exportPart);
        anyExport = true;
    }

    // ── Copy ──────────────────────────────────────────────────────────────────────────────
    // The whole block used to be gated on hasPart, so a right-click with no part under the cursor —
    // empty viewport space, or the outliner's ROOT row, which IS the loaded model — offered no way
    // to copy the model's own name or SNO even though all four values were sitting right there.
    // Only the two part-scoped entries — "Copy part name" and "Copy material name" — need a part.
    {
        // Deferred, so the block can be skipped when nothing survives the de-duplication below and
        // the separator is not left dangling over an empty section.
        QVector<QPair<QString, QString>> copies;   // label, value
        QStringList seen;
        auto addCopy = [&](const QString& label, const QString& value) {
            // Same VALUE twice = the same clipboard result under two names, which is noise however
            // the labels are worded. Every caller sets sourceFileName and sourceName from one
            // string (the Wardrobe and Stable tabs from m_partSource, the Models tab from
            // m_curName), so "Copy file name" and "Copy name" were a guaranteed duplicate pair in
            // all six entry points — and with partOwner filled they would now be a triple. First
            // label wins, because the list is ordered most-specific first.
            if (value.isEmpty() || seen.contains(value, Qt::CaseSensitive)) return;
            seen << value;
            copies.append({withValue(label, value), value});
        };
        // Most specific first: the two parts-panel rows this part actually occupies — its own
        // material (child row) and the piece it hangs under (parent row) — then the source values,
        // which are the SAME four the browse-row menus copy and carry the same four labels.
        if (hasPart) {
            addCopy(MenuText::kCopyPartName, in.partOwner);
            if (in.selMaterials.size() > 1) {
                // One clipboard entry, newline-separated — the same shape "Copy all" uses on the
                // list views, so pasting into a spreadsheet or a script gives one name per row.
                const QString joined = in.selMaterials.join(QLatin1Char('\n'));
                const QString label = QStringLiteral("Copy %1 material names")
                                          .arg(QLocale().toString(int(in.selMaterials.size())));
                // withValue() is skipped deliberately: a parenthesised preview of eleven names is
                // unreadable, and the count already says what will land on the clipboard.
                if (!joined.isEmpty() && !seen.contains(joined)) {
                    seen << joined;
                    copies.append({label, joined});
                }
            } else {
                addCopy(MenuText::kCopyMaterialName, in.partMaterial);
            }
        }
        addCopy(MenuText::kCopyFileName, in.sourceFileName);
        if (in.sno > 0) addCopy(MenuText::kCopySno, QString::number(in.sno));
        addCopy(MenuText::kCopyName,       in.sourceName);
        addCopy(MenuText::kCopyCollection, in.collection);
        if (!copies.isEmpty()) {
            if (anyExport) menu.addSeparator();
            for (const auto& c : copies)
                menu.addAction(c.first, parent, [v = c.second] { copyText(v); });
        }
    }

    // ── This part ─────────────────────────────────────────────────────────────────────────
    if (hasPart && (act.frame || act.selectPart || act.setVisible || act.isolate)) {
        menu.addSeparator();
        if (act.frame)
            menu.addAction(MenuText::verbParts(QStringLiteral("Frame"), nSel), parent, act.frame);
        if (act.selectPart)
            menu.addAction(MenuText::verbParts(QStringLiteral("Select"), nSel), parent, act.selectPart);
        // Singular only. "Explain this material" answers a question about ONE material, and with a
        // mixed selection there is no honest answer to give — offering it would mean silently
        // picking one of them.
        if (act.explainMaterial && nSel <= 1)
            menu.addAction(QStringLiteral("Explain this material…"), parent, act.explainMaterial);
        if (act.setVisible)
            menu.addAction(MenuText::verbParts(in.visible ? QStringLiteral("Hide")
                                                          : QStringLiteral("Show"), nSel),
                           parent, [act, v = in.visible] { act.setVisible(!v); });
        if (act.isolate)
            menu.addAction(MenuText::verbParts(QStringLiteral("Isolate"), nSel), parent, act.isolate);
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
