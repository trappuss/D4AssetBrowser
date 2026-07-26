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

namespace ViewportPartMenu {

// What the caller resolved about the picked part. Empty strings / -1 = unavailable.
struct Info {
    int     part      = -1;        // primitive index (-1 = clicked empty space)
    QString sourceModel;           // owning MODEL name — the menu title's left half
    QString partName;              // this submesh's name — the menu title's right half
    QString partFileName;          // part's own file/material name (Copy)
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

// "C:/Users/me/Documents/D4/exports" → ".../D4/exports". A full path makes the menu unreadable;
// the last two components are what actually distinguishes one export folder from another.
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
        menu.addAction(withValue(QStringLiteral("Export Model Last dir"), dir),
                       parent, act.exportModelLastDir);
        anyExport = true;
    }
    if (act.exportModel) {
        menu.addAction(withCount(QStringLiteral("Export Model"), in.modelTris),
                       parent, act.exportModel);
        anyExport = true;
    }
    if (hasPart && act.exportPartLastDir && !dir.isEmpty()) {
        menu.addAction(withValue(QStringLiteral("Export Part Last dir"), dir),
                       parent, act.exportPartLastDir);
        anyExport = true;
    }
    if (hasPart && act.exportPart) {
        menu.addAction(withCount(QStringLiteral("Export Part"), in.partTris),
                       parent, act.exportPart);
        anyExport = true;
    }

    // ── Copy ──────────────────────────────────────────────────────────────────────────────
    if (hasPart) {
        QStringList copies;
        if (!in.partFileName.isEmpty())   copies << QStringLiteral("part");
        if (!in.sourceFileName.isEmpty()) copies << QStringLiteral("srcfile");
        if (in.sno > 0)                   copies << QStringLiteral("sno");
        if (!in.sourceName.isEmpty())     copies << QStringLiteral("srcname");
        if (!in.collection.isEmpty())     copies << QStringLiteral("coll");
        if (!copies.isEmpty()) {
            if (anyExport) menu.addSeparator();
            if (!in.partFileName.isEmpty())
                menu.addAction(withValue(QStringLiteral("Copy part file name"), in.partFileName),
                               parent, [n = in.partFileName] { copyText(n); });
            if (!in.sourceFileName.isEmpty())
                menu.addAction(withValue(QStringLiteral("Copy part source file name"), in.sourceFileName),
                               parent, [n = in.sourceFileName] { copyText(n); });
            if (in.sno > 0)
                menu.addAction(withValue(QStringLiteral("Copy source SNO ID"), QString::number(in.sno)),
                               parent, [s = in.sno] { copyText(QString::number(s)); });
            if (!in.sourceName.isEmpty())
                menu.addAction(withValue(QStringLiteral("Copy source name"), in.sourceName),
                               parent, [n = in.sourceName] { copyText(n); });
            if (!in.collection.isEmpty())
                menu.addAction(withValue(QStringLiteral("Copy source collection name"), in.collection),
                               parent, [n = in.collection] { copyText(n); });
        }
    }

    // ── This part ─────────────────────────────────────────────────────────────────────────
    if (hasPart && (act.frame || act.selectPart || act.setVisible || act.isolate)) {
        menu.addSeparator();
        if (act.frame)      menu.addAction(QStringLiteral("Frame Part"), parent, act.frame);
        if (act.selectPart) menu.addAction(QStringLiteral("Select Part"), parent, act.selectPart);
        if (act.setVisible)
            menu.addAction(in.visible ? QStringLiteral("Hide Part") : QStringLiteral("Show Part"),
                           parent, [act, v = in.visible] { act.setVisible(!v); });
        if (act.isolate)    menu.addAction(QStringLiteral("Isolate Part"), parent, act.isolate);
    }

    // ── All parts (works with nothing under the cursor too) ───────────────────────────────
    if (act.showAll || act.hideAll || act.invert) {
        menu.addSeparator();
        if (act.showAll) menu.addAction(QStringLiteral("Show All"), parent, act.showAll);
        if (act.hideAll) menu.addAction(QStringLiteral("Hide All"), parent, act.hideAll);
        if (act.invert)  menu.addAction(QStringLiteral("Invert"), parent, act.invert);
    }

    if (!menu.isEmpty()) menu.exec(globalPos);   // blocking: returns once the menu is dismissed
    if (act.closed) act.closed();                // fires even for an empty menu, so state always clears
}

}  // namespace ViewportPartMenu
