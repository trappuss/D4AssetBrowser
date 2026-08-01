#pragma once
#include "index/CoreToc.h"   // SnoEntry

#include <QAbstractTableModel>
#include <QHash>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

// Lightweight Name | SNO model backed directly by a SnoEntry vector + a filtered
// index list. No per-row QStandardItem allocation, so loading a 140k-entry group is
// instant (vs. freezing the UI). Filtering and sorting operate on the index list.
class SnoListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit SnoListModel(QObject* parent = nullptr);

    void setEntries(const QVector<SnoEntry>& entries);
    // Models tab layout: SNO | Icon | FILENAME | NAME | COLLECTION (NAME/COLLECTION
    // pulled from AppearanceMeta). Default is the 2-column Name | SNO used elsewhere.
    void setModelsColumns(bool on);
    void metaColumnsUpdated();   // re-render NAME/COLLECTION after AppearanceMeta builds
    void setFilter(const QString& text);       // case-insensitive name substring
    void setSnoFilter(const QString& text);    // SNO id substring (digits)
    // Appearance class/gender derived from the name convention "<ccc><f|m>_…".
    // Empty string = no constraint. classCode is a 3-letter code (e.g. "bar"/"npc").
    void setClassFilter(const QString& classCode);
    void setGenderFilter(const QString& g);    // "f" / "m" / ""
    void setTypeFilter(const QString& token);  // lowercase name substring (slot/weapon)
    // Optional extra predicate (e.g. AppearanceMeta tag match). null = no constraint.
    void setPredicate(std::function<bool(const SnoEntry&)> fn);
    // Optional extra searchable text per SNO (tags / collection / title) appended to the name so
    // the name-box include/exclude terms also match metadata. null = match the name only.
    void setSearchBlob(std::function<QString(int)> fn);
    // Optional sort-value override: when set, rows sort by this value instead of the
    // active column (still honoring sort order). null = column-based sort.
    void setSortValue(std::function<double(const SnoEntry&)> fn);
    // Cache a thumbnail icon for an SNO (shown in column 0). Lazily populated as
    // models are viewed; rows without one show no icon.
    // Optional lazy icon source consulted first for the Icon column (original 2D
    // icons / mode-aware resolver). A null QPixmap result falls back to the cached
    // render thumbnail; a null function uses thumbnails only.
    void setIconProvider(std::function<QPixmap(int)> fn);
    void refreshIcons();   // re-request the Icon column for all visible rows
    void refreshIconForSno(int sno);          // repaint just the one row for `sno` (no relayout)
    void refreshIconRange(int firstRow, int lastRow); // repaint a contiguous row span only
    void refreshRowForSno(int sno);           // repaint every column/role of one row (state changed)
    // Predicate marking a model as "couldn't be displayed" (blocklisted / no geometry): such rows
    // are dimmed with a ⚠ prefix + explanatory tooltip so failures are visible before clicking.
    void setFailedPredicate(std::function<bool(int)> fn) { m_failed = std::move(fn); }
    // Model-presence badge: `fn` maps sno → +1 (has a renderable model), -1 (icon but no model),
    // 0 (unknown). `tab` selects the per-tab settings that gate the ✓/✗ overlays. Both optional.
    void setPresence(std::function<int(int)> fn, const QString& tab) { m_presence = std::move(fn); m_badgeTab = tab; }
    // Target icon size (px). When >0 the model scales each pixmap to this size so the view can
    // render icons LARGER than their source (a raw QIcon never upscales past its source pixmap,
    // which made the list's Ctrl+scroll only ever shrink icons). 0 = legacy (unscaled QIcon).
    void setIconPx(int px);
    // Grid mode: also put the thumbnail on the FILENAME column so an IconMode QListView shows
    // icon + caption. The table view ignores it (it has a dedicated Icon column).
    void setGridMode(bool on);
    int  totalCount() const { return m_all.size(); }   // unfiltered entry count (for the filter cue)
    // Optional grouping: rows sharing a key cluster under a non-selectable header
    // row. null = flat list. The key is derived from real metadata (collection/type…).
    void setGroupKey(std::function<QString(const SnoEntry&)> fn);
    const SnoEntry* entryAt(int row) const;   // nullptr for header rows / out of range
    int entryCount() const { return m_visible.size(); }   // entries only (no headers)

    Qt::ItemFlags flags(const QModelIndex& index) const override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orient, int role) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

private:
    void rebuild();   // recompute m_visible from m_all using m_filter + sort
    QVariant iconData(int sno) const;   // provider → thumbnail fallback for Icon column

    QVector<SnoEntry> m_all;
    QVector<int>      m_visible;   // filtered+sorted entry indices (no headers)
    QVector<int>      m_rows;      // display rows: >=0 entry index, -1 = group header
    QHash<int, QString> m_headerNames;   // display row → group header text
    std::function<QString(const SnoEntry&)> m_groupKey;
    QString           m_filter;        // raw text
    QStringList       m_incTerms;      // required substrings (AND)
    QStringList       m_excTerms;      // excluded substrings (NOT) — "-term" in the search box
    // "#term" / "-#term": the same AND/NOT logic but matched against the METADATA blob only
    // (tags / collection / title), never the file name. Kept separate because the '#' has to be
    // stripped before matching — leaving it on made every #tag query match nothing at all.
    QStringList       m_incTags;
    QStringList       m_excTags;
    QString           m_snoFilter;
    QString           m_classFilter;
    QString           m_genderFilter;
    QString           m_typeFilter;
    std::function<bool(const SnoEntry&)> m_predicate;
    std::function<QString(int)>          m_searchBlob;   // extra searchable text (tags…) per SNO
    std::function<double(const SnoEntry&)> m_sortValue;
    std::function<bool(int)>             m_failed;       // sno → "couldn't be displayed" (dim + ⚠)
    std::function<int(int)>              m_presence;     // sno → +1 has model / -1 icon-only / 0 unknown
    QString                              m_badgeTab;     // per-tab settings key for the ✓/✗ overlays
    std::function<QPixmap(int)>          m_iconProvider;
    QHash<int, QPixmap> m_thumbs;   // sno → thumbnail icon
    int               m_iconPx = 0;   // >0 → scale icons to this size (allows upscaling)
    bool              m_gridMode = false;   // icon-grid layout (decoration also on FILENAME col)
    int               m_sortCol = 0;
    Qt::SortOrder     m_sortOrder = Qt::AscendingOrder;
    bool              m_modelsCols = false;
};
