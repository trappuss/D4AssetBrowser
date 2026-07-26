#pragma once
// Shared right-click menu for a PART picked in the 3D viewport (Models / Wardrobe / Stable).
//
// The three tabs previously built this menu independently and had drifted to the same three
// entries each — the identical failure mode the list/grid context menus had. One builder here
// means adding an action lights it up in every viewport at once, and the ordering/grouping stays
// consistent. Each tab supplies what it can resolve (a Models part has no owning outfit piece,
// a Stable part has no dye) and leaves the rest empty/null; empty fields and null callbacks are
// simply omitted, so no tab shows an action it cannot perform.

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QMenu>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>

namespace ViewportPartMenu {

// What the tab managed to resolve about the picked part. Empty strings / -1 = unavailable.
struct Info {
    int     part      = -1;        // primitive index (-1 = clicked empty space)
    QString materialName;          // material bound to this submesh
    QString partName;              // submesh / display name (often == materialName)
    QString sourceName;            // owning piece: outfit item, mount slot… (blank in Models)
    QString collection;            // collection / set name, if the tab knows one
    int     tris      = 0;
    int     sno       = -1;        // owning appearance SNO, if known
    bool    visible   = true;      // current viewport visibility of this part
    bool    isSim     = false;     // cloth simulation cage proxy
    bool    isFx      = false;     // FX submesh
};

// Everything the menu can DO. Leave a callback null to omit its action.
struct Actions {
    std::function<void(bool)> setVisible;   // show/hide just this part
    std::function<void()>     isolate;      // hide every other part
    std::function<void()>     showAll;
    std::function<void()>     hideAll;
    std::function<void()>     invert;
    std::function<void()>     frame;        // point the camera at this part
    std::function<void()>     selectInTree; // reveal + select the part in the tab's parts panel
    std::function<void()>     exportPart;   // export just this part
};

inline void copyText(const QString& s)
{
    if (!s.isEmpty()) QGuiApplication::clipboard()->setText(s);
}

// Build and execute the menu at `globalPos`. Returns immediately if there is nothing to show.
inline void exec(QWidget* parent, const QPoint& globalPos, const Info& in, const Actions& act)
{
    QMenu menu(parent);
    const bool hasPart = in.part >= 0;

    // ── Part ──────────────────────────────────────────────────────────────────────────────
    if (hasPart) {
        // A header line naming what was actually clicked: right-clicking a mesh should tell you
        // WHICH mesh, otherwise every action below is a guess.
        QString title = in.partName.isEmpty() ? in.materialName : in.partName;
        if (title.isEmpty()) title = QStringLiteral("part %1").arg(in.part);
        if (in.isSim)  title += QStringLiteral("  [SIM]");
        if (in.isFx)   title += QStringLiteral("  [FX]");
        if (in.tris > 0) title += QStringLiteral("  ·  %L1 tris").arg(in.tris);
        QAction* hdr = menu.addAction(title);
        hdr->setEnabled(false);
        menu.addSeparator();

        if (act.frame)        menu.addAction(QStringLiteral("Frame this part"), parent, act.frame);
        if (act.selectInTree) menu.addAction(QStringLiteral("Select in parts list"), parent, act.selectInTree);
        if (act.setVisible)
            menu.addAction(in.visible ? QStringLiteral("Hide this part")
                                      : QStringLiteral("Show this part"),
                           parent, [act, v = in.visible] { act.setVisible(!v); });
        if (act.isolate) menu.addAction(QStringLiteral("Isolate this part"), parent, act.isolate);
    }

    // ── Copy ──────────────────────────────────────────────────────────────────────────────
    if (hasPart) {
        menu.addSeparator();
        QMenu* copy = menu.addMenu(QStringLiteral("Copy"));
        if (!in.partName.isEmpty())
            copy->addAction(QStringLiteral("Part name"), parent, [n = in.partName] { copyText(n); });
        if (!in.materialName.isEmpty())
            copy->addAction(QStringLiteral("Material name"), parent, [n = in.materialName] { copyText(n); });
        if (!in.sourceName.isEmpty())
            copy->addAction(QStringLiteral("Source piece"), parent, [n = in.sourceName] { copyText(n); });
        if (!in.collection.isEmpty())
            copy->addAction(QStringLiteral("Collection"), parent, [n = in.collection] { copyText(n); });
        if (in.sno > 0)
            copy->addAction(QStringLiteral("SNO id"), parent,
                            [s = in.sno] { copyText(QString::number(s)); });
        copy->addSeparator();
        // One line with everything — what you actually want when pasting into notes or a bug report.
        copy->addAction(QStringLiteral("All details"), parent, [in] {
            QStringList f;
            if (!in.partName.isEmpty())     f << QStringLiteral("part=%1").arg(in.partName);
            if (!in.materialName.isEmpty()) f << QStringLiteral("material=%1").arg(in.materialName);
            if (!in.sourceName.isEmpty())   f << QStringLiteral("source=%1").arg(in.sourceName);
            if (!in.collection.isEmpty())   f << QStringLiteral("collection=%1").arg(in.collection);
            if (in.sno > 0)                 f << QStringLiteral("sno=%1").arg(in.sno);
            if (in.tris > 0)                f << QStringLiteral("tris=%1").arg(in.tris);
            if (in.isSim)                   f << QStringLiteral("[SIM]");
            if (in.isFx)                    f << QStringLiteral("[FX]");
            copyText(f.join(QStringLiteral("  ·  ")));
        });
    }

    // ── Visibility (works with no part under the cursor too) ──────────────────────────────
    if (act.showAll || act.hideAll || act.invert) {
        menu.addSeparator();
        if (act.showAll) menu.addAction(QStringLiteral("Show all parts"), parent, act.showAll);
        if (act.hideAll) menu.addAction(QStringLiteral("Hide all parts"), parent, act.hideAll);
        if (act.invert)  menu.addAction(QStringLiteral("Invert visibility"), parent, act.invert);
    }

    // ── Export ────────────────────────────────────────────────────────────────────────────
    if (hasPart && act.exportPart) {
        menu.addSeparator();
        menu.addAction(QStringLiteral("Export this part…"), parent, act.exportPart);
    }

    if (!menu.isEmpty()) menu.exec(globalPos);
}

}  // namespace ViewportPartMenu
