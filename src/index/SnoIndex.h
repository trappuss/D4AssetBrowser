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

    // ── sno <-> name, for one group ──────────────────────────────────────────────────────────────
    //
    // ***GUI THREAD ONLY.*** Both of these are declared const and BOTH WRITE a mutable lazy cache,
    // so a `const SnoIndex*` handed to a worker silently gains write access to shared state. That
    // is not theoretical: StoreProductIndex's build worker was given exactly such a pointer, and it
    // raced the GUI thread (which calls nameForSno while filling texture panels) on the same outer
    // QHash — a rehash on one side while the other holds an iterator. It also raced
    // MainWindow::reload(), which rebuilds the whole index on its own thread.
    //
    // The fix there was to SNAPSHOT what the worker needed on the calling thread and pass it by
    // value. Do that, do not add a lock and do not "just be careful" — the const signature gives
    // no warning at the call site.
    //
    // entries() is sorted by NAME, so there is no way to answer "what is sno N called" without a
    // reverse map. Built lazily for the requested group ONLY, on first use, and dropped with the
    // index — memory, never disk.
    //
    // Needed because a material's texture list carries SNOs and no names (an encrypted texture has
    // none in the material binary), so the Models panels showed "~unnamed_2334281" for textures the
    // Textures tab happily lists as "DruM_stor235_HLM_Color". Same asset, same index, two different
    // answers — which reads as "the tool picked the wrong textures" when the SNOs were right.
    //
    // Returns an empty string when the group has no such sno, so callers keep their own fallback.
    QString nameForSno(int group, int sno) const;

    // The inverse, from the same lazily-built table. Case-insensitive, because the callers hold
    // names that came from other indexes' own casing. 0 when the group has no such name.
    //
    // Needed to cross indexes that store NAMES against ones that store SNOs — the Models tab knows
    // an appearance's item names, and store provenance is keyed by item sno.
    int snoForName(int group, const QString& name) const;

    static QString groupName(int group);   // "Texture", "Appearance", … or "Group N"
    static int groupIdByName(const QString& name, int fallback);   // reverse lookup, name-pinned
    static bool    isExcluded(int group);

    // ── "Latest" (what's new since the last game/d4data update) ──────────────────────────────────
    // Snapshots the full SNO set per build; when `sig` (the CASC build id) changes it diffs against
    // the previous snapshot and keeps the additions until the NEXT update. First run establishes a
    // baseline (nothing is "new" yet). Call once after the index loads. Needs a non-empty sig.
    // ── Encrypted-name recovery ──────────────────────────────────────────────────────────────────
    // Encrypted assets reach CoreTOC with their name BLANKED (CoreToc.cpp indexes them as
    // "~unnamed_<sno>"), and every roster in the tool is name-shaped — the wardrobe slot filter is
    // literally startsWith("barf") + endsWith("_trs") — so decrypting them is not enough to make
    // them appear. Measured with D4_DUMP_ENCRYPTED: the authored name IS in the Appearance payload,
    // in ClothData's 32-byte name field, e.g. "necM_stor245_TRS_cape", "palF_stor171_LEG_hipPlate",
    // "DruM_stor235_GLV_fur_HQO". Dropping the part token yields exactly the wardrobe's shape.
    //
    // Only cloth-bearing pieces carry one, so this recovers capes/skirts/chests and not plain boots.
    // Partial by nature, and honest about it: the candidate must match <cls><g>_<style>_<SLOT> or it
    // is left as ~unnamed rather than guessed at. Returns how many names were recovered. Runs before
    // saveToCache so the result rides the existing per-build index cache — no new cache.
    int recoverEncryptedNames(CascReader& casc);

    // ── Encrypted names, from the game's own dictionary ──────────────────────────────────────────
    // The AUTHORITATIVE answer to the problem recoverEncryptedNames only half-solves. D4 does not
    // throw an encrypted asset's name away when it blanks it in CoreTOC — it moves it. The install
    // ships base/EncryptedSNOs.dat (sno -> group + encryption key) and one
    // base/EncryptedNameDict-0x<keyName>.dat per key, each dict encrypted with the key it belongs
    // to, so holding a TACT key means holding the names of everything that key covers. There are
    // 189 dicts in the current build; we hold 8 keys, worth ~2,600 SNOs.
    //
    // This was written off once, and the reason is worth recording so it is not written off again:
    // d4data ships a json/base/EncryptedNameDict.dat.json with FOUR entries, which reads like the
    // file is useless. It is not — d4data's parse.js writes that JSON from INSIDE its per-file
    // loop, so each dict overwrites the last and the artefact only ever contains whichever one it
    // read last. The dicts themselves are complete.
    //
    // Format (parse.js:212, verified against the shipped files): u32 magic 0xABCD4567, u32 count,
    // count x { i32 snoGroup, i32 snoId }, then the names as packed NUL-terminated strings in the
    // same order. Only ~unnamed_ placeholders are overwritten — a name CoreTOC did supply always
    // wins, so a stale dict can never rename a live asset. Returns how many names were applied.
    int applyEncryptedNameDicts(CascReader& casc);

    // ── Build ledger ─────────────────────────────────────────────────────────────────────────────
    // One record per game build this tool has OPENED, newest last, holding the SNOs that appeared
    // in it. Deltas only: the additions for a patch are a few thousand ids (tens of KB), so years
    // of history costs a fraction of the one full snapshot updateLatest already keeps.
    //
    // Observational, and honest about it. Nothing in CoreTOC stamps an asset with the build that
    // introduced it — that information is not authored anywhere, so it can only be recorded by
    // being present when it happens. Two consequences worth stating in the UI rather than hiding:
    // a build you never opened the tool on can never be reconstructed (the local d4data checkout
    // is --depth 1 --filter=blob:none, so its git history cannot be mined for it either), and when
    // two observed builds are not consecutive the record covers the gap — hence prevGameVersion,
    // so it can read "added between 2.3.0 and 2.3.2" instead of pretending it was one patch.
    struct BuildRecord {
        QString   product;           // .build.info product ("fenris" retail); "" if unknown
        QString   buildId;           // TVFS root manifest hash — the exact identity
        QString   gameVersion;       // "2.3.1.65956" — what a human recognises
        QString   prevBuildId;       // what this was diffed against ("" = baseline, nothing added)
        QString   prevGameVersion;
        qint64    firstSeen = 0;     // secs since epoch, when this build was first opened here
        QSet<int> added;

        // No predecessor is recorded, so "added" cannot be attributed to a clean patch boundary.
        // Two distinct causes, both of which must read as "previous build unknown" in the UI
        // rather than being passed off as a normal patch:
        //   - the first build ever seen for this product  (added is empty)
        //   - a build whose diff predates the ledger       (added is populated, from the old
        //     single-slot snapshot, but nothing recorded what came before it)
        bool prevUnknown() const { return prevBuildId.isEmpty(); }
        // The genuine first-run baseline: nothing preceded it AND nothing was counted as added.
        bool isBaseline() const { return prevBuildId.isEmpty() && added.isEmpty(); }
    };
    // Newest first. Empty product returns every record.
    //
    // Read-only: the ledger is written as builds are observed and never rewritten from it. There is
    // deliberately no "point Latest at build X" setter — one existed briefly to drive a build
    // selector in the Models funnel, and the UI was dropped; a mutator with no caller is a liability,
    // not an option kept open. The recording stays because a build's additions can ONLY be captured
    // at the moment it is first opened, so losing the habit loses the data permanently.
    QVector<BuildRecord> buildHistory(const QString& product = QString()) const;

    // `lineage` separates baselines that must never be diffed against each other — the product
    // code from .build.info ("fenris" retail vs the PTR code). With a single shared baseline,
    // opening PTR diffed it against the RETAIL snapshot and then overwrote that snapshot with
    // PTR's SNO set; switching back diffed retail against PTR and reported nonsense, and the real
    // "new this patch" baseline was gone for good. Empty lineage keeps the old single-slot
    // behaviour, which is right for anyone who only ever runs one product.
    void updateLatest(const QString& sig, const QString& lineage = QString(),
                      const QString& gameVersion = QString());
    bool isNew(int sno) const { return m_newSnos.contains(sno); }
    const QSet<int>& newSnos() const { return m_newSnos; }
    bool hasLatest() const { return !m_newSnos.isEmpty(); }

private:
    bool ingest(QHash<int, QVector<SnoEntry>>& parsed);  // exclude + sort + store

    QHash<int, QVector<SnoEntry>> m_byGroup;
    // group -> (sno -> name). Mutable: nameForSno is a const query that fills its own cache. The
    // QStrings are implicitly shared with the entries they came from, so a group costs a hash node
    // per sno and not a second copy of every name.
    mutable QHash<int, QHash<int, QString>> m_snoNameCache;
    // group -> (lowercased name -> sno). Same lifetime and invalidation as the above.
    mutable QHash<int, QHash<QString, int>> m_nameSnoCache;
    QSet<int> m_newSnos;    // SNOs added in the most recent build change
    bool m_loaded = false;
    int  m_total  = 0;
};
