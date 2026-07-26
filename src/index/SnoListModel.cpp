#include "index/SnoListModel.h"

#include "index/AppearanceMeta.h"
#include "tabs/IconBadge.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QIcon>

#include <algorithm>

SnoListModel::SnoListModel(QObject* parent) : QAbstractTableModel(parent) {}

void SnoListModel::setEntries(const QVector<SnoEntry>& entries)
{
    beginResetModel();
    m_all = entries;
    rebuild();
    endResetModel();
}

void SnoListModel::setModelsColumns(bool on)
{
    if (on == m_modelsCols) return;
    beginResetModel();
    m_modelsCols = on;
    endResetModel();
}

void SnoListModel::metaColumnsUpdated()
{
    if (m_modelsCols && !m_rows.isEmpty())
        emit dataChanged(index(0, 3), index(m_rows.size() - 1, 4), {Qt::DisplayRole});
}

void SnoListModel::setFilter(const QString& text)
{
    const QString t = text.trimmed();
    if (t == m_filter)
        return;
    beginResetModel();
    m_filter = t;
    // Parse space-separated terms: a leading '-' excludes (name must NOT contain it), everything
    // else is a required substring (AND). e.g. "pandem -destroyed -pillar".
    m_incTerms.clear();
    m_excTerms.clear();
    for (const QString& tok : t.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (tok.startsWith(QLatin1Char('-')) && tok.size() > 1)
            m_excTerms << tok.mid(1);
        else if (!tok.startsWith(QLatin1Char('-')))
            m_incTerms << tok;
    }
    rebuild();
    endResetModel();
}

void SnoListModel::setSnoFilter(const QString& text)
{
    const QString t = text.trimmed();
    if (t == m_snoFilter)
        return;
    beginResetModel();
    m_snoFilter = t;
    rebuild();
    endResetModel();
}

void SnoListModel::setClassFilter(const QString& classCode)
{
    if (classCode == m_classFilter) return;
    beginResetModel(); m_classFilter = classCode; rebuild(); endResetModel();
}

void SnoListModel::setGenderFilter(const QString& g)
{
    if (g == m_genderFilter) return;
    beginResetModel(); m_genderFilter = g; rebuild(); endResetModel();
}

void SnoListModel::setTypeFilter(const QString& token)
{
    if (token == m_typeFilter) return;
    beginResetModel(); m_typeFilter = token; rebuild(); endResetModel();
}

void SnoListModel::setPredicate(std::function<bool(const SnoEntry&)> fn)
{
    beginResetModel(); m_predicate = std::move(fn); rebuild(); endResetModel();
}

void SnoListModel::setSearchBlob(std::function<QString(int)> fn)
{
    beginResetModel(); m_searchBlob = std::move(fn); rebuild(); endResetModel();
}

void SnoListModel::setSortValue(std::function<double(const SnoEntry&)> fn)
{
    beginResetModel(); m_sortValue = std::move(fn); rebuild(); endResetModel();
}

void SnoListModel::rebuild()
{
    const bool clsGen = !m_classFilter.isEmpty() || !m_genderFilter.isEmpty();
    m_visible.clear();
    m_visible.reserve(m_all.size());
    for (int i = 0; i < m_all.size(); ++i) {
        const SnoEntry& e = m_all[i];
        if (!m_incTerms.isEmpty() || !m_excTerms.isEmpty()) {
            // Match the name PLUS any extra searchable text (tags / collection) so include/exclude
            // terms like "-player" or "-armor" work on metadata, not just the file name.
            const QString hay = m_searchBlob ? (e.name + QLatin1Char(' ') + m_searchBlob(e.snoId))
                                             : e.name;
            bool ok = true;
            for (const QString& inc : m_incTerms)
                if (!hay.contains(inc, Qt::CaseInsensitive)) { ok = false; break; }
            if (ok) for (const QString& exc : m_excTerms)
                if (hay.contains(exc, Qt::CaseInsensitive)) { ok = false; break; }
            if (!ok) continue;
        }
        if (!m_snoFilter.isEmpty() && !QString::number(e.snoId).contains(m_snoFilter))
            continue;
        if (!m_typeFilter.isEmpty() && !e.name.contains(m_typeFilter, Qt::CaseInsensitive))
            continue;
        if (m_predicate && !m_predicate(e))
            continue;
        if (clsGen) {
            // Name convention: "<ccc><f|m>_…" (e.g. barF_, necM_).
            const QString n = e.name.toLower();
            const bool ok = n.size() >= 5 && n[4] == QLatin1Char('_')
                && n[0].isLetter() && n[1].isLetter() && n[2].isLetter()
                && (n[3] == QLatin1Char('f') || n[3] == QLatin1Char('m'));
            if (!ok) continue;
            if (!m_classFilter.isEmpty() && n.left(3) != m_classFilter) continue;
            if (!m_genderFilter.isEmpty() && n.mid(3, 1) != m_genderFilter) continue;
        }
        m_visible.append(i);
    }

    const int col = m_sortCol;
    const Qt::SortOrder ord = m_sortOrder;
    const bool snoSort = m_modelsCols ? (col == 0) : (col == 1);   // SNO column position
    std::sort(m_visible.begin(), m_visible.end(), [this, snoSort, ord](int a, int b) {
        const SnoEntry& ea = m_all[a];
        const SnoEntry& eb = m_all[b];
        int c;
        if (m_sortValue) {
            const double va = m_sortValue(ea), vb = m_sortValue(eb);
            c = (va < vb) ? -1 : (va > vb ? 1 : 0);
            if (c == 0) c = (ea.snoId < eb.snoId) ? -1 : (ea.snoId > eb.snoId ? 1 : 0);
        } else if (snoSort) {
            c = (ea.snoId < eb.snoId) ? -1 : (ea.snoId > eb.snoId ? 1 : 0);
        } else {
            c = ea.name.compare(eb.name, Qt::CaseInsensitive);
        }
        return ord == Qt::AscendingOrder ? c < 0 : c > 0;
    });

    // Build the display rows. Flat list unless a group key is set, in which case
    // entries cluster (stable within the column sort) under a header row per group.
    m_rows.clear();
    m_headerNames.clear();
    if (!m_groupKey) {
        m_rows = m_visible;
        return;
    }
    QHash<int, QString> g;
    g.reserve(m_visible.size());
    for (int i : m_visible) {
        QString k = m_groupKey(m_all[i]);
        g.insert(i, k.isEmpty() ? QStringLiteral("(none)") : k);
    }
    std::stable_sort(m_visible.begin(), m_visible.end(), [&g](int a, int b) {
        return g.value(a).compare(g.value(b), Qt::CaseInsensitive) < 0;
    });
    QString cur;
    bool first = true;
    for (int i : m_visible) {
        const QString gn = g.value(i);
        if (first || gn != cur) {
            cur = gn; first = false;
            m_headerNames.insert(m_rows.size(), gn);
            m_rows.append(-1);   // header marker
        }
        m_rows.append(i);
    }
}

void SnoListModel::setGroupKey(std::function<QString(const SnoEntry&)> fn)
{
    beginResetModel();
    m_groupKey = std::move(fn);
    rebuild();
    endResetModel();
}

Qt::ItemFlags SnoListModel::flags(const QModelIndex& index) const
{
    if (index.isValid() && index.row() >= 0 && index.row() < m_rows.size()
        && m_rows[index.row()] < 0)
        return Qt::ItemIsEnabled;   // header row: visible but not selectable
    return QAbstractTableModel::flags(index);
}

void SnoListModel::setIconProvider(std::function<QPixmap(int)> fn)
{
    m_iconProvider = std::move(fn);
    refreshIcons();
}

void SnoListModel::setIconPx(int px)
{
    if (px == m_iconPx) return;
    m_iconPx = px;
    refreshIcons();
}

void SnoListModel::setGridMode(bool on)
{
    if (on == m_gridMode) return;
    m_gridMode = on;
    if (!m_rows.isEmpty())   // decoration + alignment of the FILENAME column changed
        emit dataChanged(index(0, 0), index(m_rows.size() - 1, columnCount() - 1),
                         {Qt::DecorationRole, Qt::TextAlignmentRole});
}

void SnoListModel::refreshIcons()
{
    if (m_rows.isEmpty()) return;
    // Models layout: cover the Icon column (1) AND FILENAME (2) — the grid view draws its
    // thumbnail on column 2, so it must repaint too when thumbnails load/render.
    if (m_modelsCols)
        emit dataChanged(index(0, 1), index(m_rows.size() - 1, 2), {Qt::DecorationRole});
    else
        emit dataChanged(index(0, 0), index(m_rows.size() - 1, 0), {Qt::DecorationRole});
}

void SnoListModel::refreshIconForSno(int sno)
{
    // Repaint only the row that owns `sno`. A single-row DecorationRole dataChanged
    // repaints one cell without triggering a full items-relayout — so selecting/loading
    // a model no longer reflows the grid or resets the scroll position.
    for (int r = 0; r < m_rows.size(); ++r)
        if (m_rows[r] >= 0 && m_all[m_rows[r]].snoId == sno) {
            const int c1 = m_modelsCols ? 1 : 0;
            const int c2 = m_modelsCols ? 2 : 0;
            emit dataChanged(index(r, c1), index(r, c2), {Qt::DecorationRole});
            break;
        }
}

void SnoListModel::refreshIconRange(int firstRow, int lastRow)
{
    if (m_rows.isEmpty()) return;
    firstRow = qBound(0, firstRow, int(m_rows.size()) - 1);
    lastRow  = qBound(0, lastRow,  int(m_rows.size()) - 1);
    if (lastRow < firstRow) return;
    const int c1 = m_modelsCols ? 1 : 0;
    const int c2 = m_modelsCols ? 2 : 0;
    emit dataChanged(index(firstRow, c1), index(lastRow, c2), {Qt::DecorationRole});
}

void SnoListModel::refreshRowForSno(int sno)
{
    // Repaint one row across every column and role — used when a model's state changes
    // (e.g. it just got blocklisted) so the dim/⚠ styling appears immediately.
    for (int r = 0; r < m_rows.size(); ++r)
        if (m_rows[r] >= 0 && m_all[m_rows[r]].snoId == sno) {
            emit dataChanged(index(r, 0), index(r, columnCount() - 1));
            break;
        }
}

// (setThumbnail removed — thumbnails arrive through the icon provider, not per-row pokes.)

// Icon-column resolver. When a provider is installed it is authoritative (it is
// mode-aware and applies its own original/render fallback); otherwise the cached
// render thumbnail is used.
QVariant SnoListModel::iconData(int sno) const
{
    // Return a QIcon (not a raw QPixmap) so the view scales it to the current
    // iconSize and clips to the cell — a raw pixmap draws at native size and spills.
    QPixmap pm;
    if (m_iconProvider)
        pm = m_iconProvider(sno);
    else {
        const auto it = m_thumbs.constFind(sno);
        if (it != m_thumbs.constEnd()) pm = it.value();
    }
    if (pm.isNull()) return QVariant();
    // Scale to the requested icon size so the view can draw icons LARGER than the source pixmap
    // (a plain QIcon caps at the source size → the list could only ever shrink). 0 = unscaled.
    if (m_iconPx > 0 && (pm.width() != m_iconPx || pm.height() != m_iconPx))
        pm = pm.scaled(m_iconPx, m_iconPx, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // Model-presence badge (✓ has model / ✗ icon-only), gated by the per-tab settings.
    if (m_presence && IconBadge::anyEnabled(m_badgeTab))
        pm = IconBadge::withBadge(pm, m_presence(sno),
                                  IconBadge::showPresent(m_badgeTab), IconBadge::showMissing(m_badgeTab));
    return QVariant(QIcon(pm));
}

const SnoEntry* SnoListModel::entryAt(int row) const
{
    if (row < 0 || row >= m_rows.size() || m_rows[row] < 0)
        return nullptr;   // out of range or a group-header row
    return &m_all[m_rows[row]];
}

int SnoListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

int SnoListModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : (m_modelsCols ? 5 : 2);
}

QVariant SnoListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return {};
    // Group-header row: show the group name in the first text column, styled.
    if (index.row() >= 0 && index.row() < m_rows.size() && m_rows[index.row()] < 0) {
        const QString name = m_headerNames.value(index.row());
        const int textCol = m_modelsCols ? 2 : 0;   // FILENAME / Name column
        if (role == Qt::DisplayRole && index.column() == textCol)
            return QStringLiteral("▸ %1").arg(name);
        if (role == Qt::FontRole) {
            QFont f; f.setBold(true); return f;
        }
        if (role == Qt::BackgroundRole)
            return QBrush(QColor(0x2a, 0x2a, 0x2a));
        if (role == Qt::ForegroundRole)
            return QBrush(QColor(0xd8, 0xa2, 0x3a));
        return {};
    }
    const SnoEntry* e = entryAt(index.row());
    if (!e)
        return {};
    const int col = index.column();
    if (!m_modelsCols) {   // legacy 2-column Name | SNO
        if (role == Qt::DisplayRole)
            return col == 0 ? QVariant(e->name) : QVariant(e->snoId);
        if (role == Qt::DecorationRole && col == 0)
            return iconData(e->snoId);
        if (role == Qt::TextAlignmentRole && col == 1)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return {};
    }
    // Models layout: 0=SNO, 1=Icon, 2=FILENAME, 3=NAME, 4=COLLECTION
    const bool failed = m_failed && m_failed(e->snoId);   // blocklisted / no-geometry
    if (role == Qt::ForegroundRole && failed)
        return QBrush(QColor(0x88, 0x88, 0x88));   // dim the whole row so it reads as unavailable
    if (role == Qt::DisplayRole) {
        switch (col) {
        case 0: return e->snoId;
        case 2: return failed ? QStringLiteral("⚠ %1").arg(e->name) : e->name;
        case 3: return AppearanceMeta::instance().titleFor(e->snoId);
        case 4: return AppearanceMeta::instance().collectionFor(e->snoId);
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole && failed)
        return QStringLiteral("%1\n⚠ Couldn't be displayed — blocklisted so it can't crash the tool.\n"
                              "Right-click ▸ Un-block && render to retry, or clear the blocklist in "
                              "Settings ▸ Maintenance.").arg(e->name);
    // Icon on the Icon column always; in grid mode also on FILENAME so an IconMode QListView
    // (which draws one column) shows the thumbnail with the name beneath it.
    if (role == Qt::DecorationRole && (col == 1 || (m_gridMode && col == 2)))
        return iconData(e->snoId);
    if (role == Qt::ToolTipRole && col == 2)
        return e->name;   // full name on hover (useful when the cell elides)
    if (role == Qt::TextAlignmentRole && col == 0)
        return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::TextAlignmentRole && m_gridMode && col == 2)
        return int(Qt::AlignHCenter | Qt::AlignTop);   // centered caption under the grid icon
    return {};
}

QVariant SnoListModel::headerData(int section, Qt::Orientation orient, int role) const
{
    if (orient != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    if (m_modelsCols) {
        static const char* h[] = {"SNO", "Icon", "FILENAME", "NAME", "COLLECTION"};
        return (section >= 0 && section < 5) ? QString::fromLatin1(h[section]) : QString();
    }
    return section == 0 ? QStringLiteral("Name") : QStringLiteral("SNO ID");
}

void SnoListModel::sort(int column, Qt::SortOrder order)
{
    m_sortCol = column;
    m_sortOrder = order;
    beginResetModel();
    rebuild();
    endResetModel();
}
