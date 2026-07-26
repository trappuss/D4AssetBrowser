#pragma once
#include "index/CoreToc.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QVector>

class CascReader;

// The live SNO asset index: reads base/CoreTOC.dat straight out of CASC (so it
// always matches the installed build) and exposes assets grouped by type. Mirrors
// the Python fork's SnoIndex: the same 132-group name map and the same excluded
// groups (Powers/Quests/Adventure-challenge) kept out of the browser.
class SnoIndex {
public:
    // Load the SNO index. CASC reads the live game's binary CoreTOC.dat (newest,
    // but often absent in CASC); d4data reads the pre-parsed json/base/CoreTOC.dat.json
    // (the reliable source the metadata layer also uses). MainWindow tries CASC then
    // falls back to d4data.
    bool loadFromCasc(CascReader& casc);
    bool loadFromD4data(const QString& d4dataDir);
    // Post-ingest disk cache (already excluded + sorted): skips the CASC read + the ~820k-entry
    // CoreTOC parse + the per-group sorts on warm launches. `sig` = the CASC build id.
    bool loadFromCache(const QString& sig);
    void saveToCache(const QString& sig) const;
    void clear();
    bool isLoaded() const { return m_loaded; }
    int  totalCount() const { return m_total; }

    // Group ids present in the index, excluding EXCLUDED groups, sorted ascending.
    QList<int> groups() const;
    // Assets in a group (empty if absent). Stable reference for the model.
    const QVector<SnoEntry>& entries(int group) const;

    static QString groupName(int group);   // "Texture", "Appearance", … or "Group N"
    static int groupIdByName(const QString& name, int fallback);   // reverse lookup, name-pinned
    static bool    isExcluded(int group);

    // ── "Latest" (what's new since the last game/d4data update) ──────────────────────────────────
    // Snapshots the full SNO set per build; when `sig` (the CASC build id) changes it diffs against
    // the previous snapshot and keeps the additions until the NEXT update. First run establishes a
    // baseline (nothing is "new" yet). Call once after the index loads. Needs a non-empty sig.
    void updateLatest(const QString& sig);
    bool isNew(int sno) const { return m_newSnos.contains(sno); }
    const QSet<int>& newSnos() const { return m_newSnos; }
    bool hasLatest() const { return !m_newSnos.isEmpty(); }

private:
    bool ingest(QHash<int, QVector<SnoEntry>>& parsed);  // exclude + sort + store

    QHash<int, QVector<SnoEntry>> m_byGroup;
    QSet<int> m_newSnos;    // SNOs added in the most recent build change
    bool m_loaded = false;
    int  m_total  = 0;
};
