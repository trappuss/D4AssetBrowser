#include "util/CsvCopy.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QItemSelectionModel>
#include <QMenu>
#include <QAction>
#include <QWidget>
#include <QKeySequence>
#include <QSet>
#include <QShortcut>
#include <QStringList>
#include <algorithm>

namespace {

QString csvField(const QString& s)
{
    if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"'))
            || s.contains(QLatin1Char('\n')) || s.contains(QLatin1Char('\r'))) {
        QString q = s;
        q.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + q + QLatin1Char('"');
    }
    return s;
}

void copySelection(QAbstractItemView* view)
{
    QAbstractItemModel* model = view->model();
    if (!model)
        return;

    // Selected rows (deduped, sorted) — or every row if nothing is selected.
    QList<int> rows;
    QItemSelectionModel* sel = view->selectionModel();
    if (sel && sel->hasSelection()) {
        QSet<int> seen;
        const QModelIndexList idxs = sel->selectedIndexes();
        for (const QModelIndex& idx : idxs) {
            if (!seen.contains(idx.row())) {
                seen.insert(idx.row());
                rows.append(idx.row());
            }
        }
        std::sort(rows.begin(), rows.end());
    } else {
        for (int r = 0; r < model->rowCount(); ++r)
            rows.append(r);
    }

    const int cols = model->columnCount();
    QStringList lines;

    QStringList header;
    for (int c = 0; c < cols; ++c)
        header << csvField(model->headerData(c, Qt::Horizontal).toString());
    lines << header.join(QLatin1Char(','));

    for (int r : rows) {
        QStringList cells;
        for (int c = 0; c < cols; ++c)
            cells << csvField(model->index(r, c).data(Qt::DisplayRole).toString());
        lines << cells.join(QLatin1Char(','));
    }

    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
}

}  // namespace

void CsvCopy::install(QAbstractItemView* view)
{
    auto* sc = new QShortcut(QKeySequence::Copy, view);
    sc->setContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(sc, &QShortcut::activated, view, [view] { copySelection(view); });

    // A right-click Copy as well as Ctrl+C. Ten of the Models tab's detail tables had this
    // shortcut and NOTHING on right-click, while every neighbouring panel in Wardrobe and Stable
    // offered a menu — a keyboard-only affordance next to a dozen menus reads as "no copy here".
    // Installed here rather than per call site so the fourteen views that already ask for CsvCopy
    // all get it, and so a fifteenth cannot be added without it.
    // A view that already declared its own policy keeps its own menu — Ctrl+C above still applies.
    //
    // THE TRAP THIS GUARD DOES NOT CATCH: it only works if install() runs AFTER the view sets its
    // policy. Called first, it sees DefaultContextMenu, installs, and the caller's later connect()
    // adds a SECOND handler to the same signal — Qt runs both, this one is connected first, so its
    // menu wins and the caller's is unreachable. That is a silent, confusing failure: the code
    // looks right and the menu never changes. Two views in ModelsTab were wired that way.
    // Set your policy BEFORE calling install().
    if (view->contextMenuPolicy() != Qt::DefaultContextMenu) return;
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(view, &QWidget::customContextMenuRequested, view, [view](const QPoint& p) {
        QMenu menu;
        QItemSelectionModel* sel = view->selectionModel();
        const bool has = sel && sel->hasSelection();
        // "Copy" is the selection; "Copy all" ignores it. copySelection already falls back to every
        // row when nothing is selected, which is exactly what "Copy all" means — so the second
        // action just clears the selection first rather than duplicating the CSV walk.
        QAction* aSel = menu.addAction(QStringLiteral("Copy"));
        aSel->setEnabled(has);
        QObject::connect(aSel, &QAction::triggered, view, [view] { copySelection(view); });
        QAction* aAll = menu.addAction(QStringLiteral("Copy all"));
        QObject::connect(aAll, &QAction::triggered, view, [view, sel] {
            const QItemSelection keep = sel ? sel->selection() : QItemSelection();
            if (sel) sel->clearSelection();
            copySelection(view);
            if (sel) sel->select(keep, QItemSelectionModel::Select);
        });
        menu.exec(view->viewport()->mapToGlobal(p));
    });
}
