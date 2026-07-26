#include "util/CsvCopy.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QItemSelectionModel>
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
}
