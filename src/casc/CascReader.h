#pragma once
#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

class QFile;

// Native Diablo IV CASC reader — a C++ port of the proven pure-Python reader
// (Diablo4AssetBrowser2/src/d4ab2/formats/casc_reader.py). D4 stores every asset
// inside a nested TVFS whose internal paths are NUMERIC (base/payload/<sno>,
// base/meta/<sno>, base/paylow/<sno>) — there is no group sub-folder or
// extension. CascLib (as built here) did not resolve those paths, so we parse
// the build config + .idx index + TVFS root ourselves and BLTE-decode (raw /
// zlib / lz4 / Salsa20-encrypted) on read.
class CascReader {
public:
    struct Entry {
        QString  name;
        quint64  size = 0;
        quint32  fileDataId = 0;
        bool     available = false;
    };

    CascReader() = default;
    ~CascReader() = default;
    CascReader(const CascReader&) = delete;
    CascReader& operator=(const CascReader&) = delete;

    // Load all CASC metadata from the install root (folder with .build.info or
    // Data/.build.config). `product` selects among MULTIPLE active .build.info rows (retail vs
    // PTR vs a beta share the file format); empty, or no matching row, falls back to the first
    // active row. It was ignored until the PTR work — see firstActiveBuildInfo.
    bool open(const QString& gameDir, const QString& product = QString());
    void close();
    bool isReady() const { return m_ready; }
    // The product code of the row that was actually opened ("fenris" for retail), straight out
    // of .build.info — NOT the configured setting, which may not match. Empty for Steam installs
    // (Data/.build.config carries no product). Used as the per-install identity for anything that
    // must not mix products, e.g. the "Latest" baseline.
    QString product() const { return m_product; }
    // Human game version of the opened row ("2.3.1.65956"-style), or empty.
    QString openedVersion() const { return m_gameVersion; }
    // Stable identity for "which install is this", for anything that must not mix two of them —
    // the "Latest" baseline above all. Prefers the .build.info Product code, and falls back to the
    // install FOLDER NAME.
    //
    // The fallback is not defensive padding: measured on a real Battle.net install, .build.info
    // parses Version correctly but carries no Product column at all, so product() is empty for
    // retail AND for PTR. Keyed on that alone both installs collapse into one slot and overwrite
    // each other's baseline, which is the precise failure this identity exists to prevent. The
    // folder name is what the user actually chose in Settings and is necessarily distinct between
    // two installs ("Diablo IV" vs "Diablo IV Public Test").
    QString lineageKey() const;
    // A fingerprint of the loaded game build (the TVFS root manifest hash). Changes every
    // game patch — used to invalidate stale on-disk caches when the game updates.
    QString buildId() const { return m_buildId; }
    // Human game version from .build.info's active row ("2.3.1.65956"-style), or empty.
    // Static so the staleness banner can compare game-vs-d4data before/without open().
    static QString gameVersion(const QString& gameDir);
    // Container manifests skipped because their TACT key is missing — a growing number after a
    // patch is the "your keys file is stale" signal (drives the key-staleness warning).
    int missingKeyCount() const { return m_covEncrypted.size(); }
    int tactKeyCount() const { return m_tactKeys.size(); }

    // Register TACT decryption keys from a community key file. Returns count.
    // `keysPath` may be a single key file OR a folder (all *.txt/*.csv inside are loaded).
    int applyTactKeys(const QString& keysPath);

    // Read + BLTE-decode a file by its CASC virtual path (e.g. base/payload/<sno>).
    QByteArray readFile(const QString& name);

    // Read an asset payload by SNO id: base/payload/<sno>, then base/paylow/<sno>.
    QByteArray readPayloadBySno(quint64 sno);
    // Read an asset meta blob by SNO id: base/meta/<sno>.
    QByteArray readMetaBySno(quint64 sno);

    // True if the sno's payload is unencrypted, or Salsa20-encrypted with a TACT key we hold.
    // Cheap probe: reads only the BLTE header + the first chunk's encryption block (no decode).

    // Stored (compressed) payload size for an SNO via index lookup only (no read).
    quint64 payloadSize(quint64 sno);
    // The TACT key an sno's payload is encrypted with, or empty when it is not encrypted. Read from
    // the BLTE frame header only — no decode, no decompression — so it is cheap enough to call once
    // per row while filtering a list. Second form says whether that key is actually loaded.
    // Which payload file an sno's geometry actually came from. readPayloadBySno tries
    // base/payload/<sno> and FALLS BACK to base/paylow/<sno>, so a piece whose full-detail payload
    // is encrypted under a key we lack can silently be served the low-detail one instead — a valid
    // blob that does not match the meta describing it. Stored (compressed) sizes; 0 when absent.
    // No decode, so it is safe to call from a failure path.
    // Every TVFS path this sno appears under, with its stored size. readFile only ever tries
    // base/payload, base/paylow and base/meta; textures return nothing from all three, so their
    // definition lives somewhere else and guessing the path is what this exists to avoid.
    QStringList rootPathsFor(quint64 sno);
    // Namespace census: every distinct path PREFIX (segments before the trailing id) with a count
    // and one example. Textures have no base/meta/<sno>, so their definition is somewhere readFile
    // never looks — this shows what namespaces exist instead of guessing one at a time.
    QStringList rootPrefixCensus();
    // EVERY TVFS path with its stored size, written to a file. One complete dump beats a rebuild
    // per guessed path: with the whole table on disk, any question about where a definition lives
    // is answered offline. Returns the count written.
    int dumpAllRootPaths(const QString& outPath);
    // Every root path starting with a prefix. Lets a probe pull a whole family of files (the 137
    // texture-base-global-0x<hash>.dat overlays) without naming each one.
    QStringList rootPathsWithPrefix(const QString& prefix);

    struct PayloadVariants { quint64 payload = 0; quint64 paylow = 0; };
    PayloadVariants payloadVariants(quint64 sno);

    QByteArray tactKeyFor(quint64 sno);
    bool       haveTactKey(const QByteArray& keyName) const;

    // ── Which SNOs ship encrypted, answered by the game rather than probed ───────────────────────
    // sno -> the 8-byte TACT key name it is encrypted with. Built once from base/EncryptedSNOs.dat,
    // the manifest the client itself reads to know which key to ask for; ~14k entries under ~197
    // keys in the current build. Memory only, dropped with the reader.
    //
    // This exists because the obvious alternative does not scale. Deciding "is this encrypted" from
    // the BLTE frame header means an index lookup and an archive open PER SNO, and findArchive
    // stats up to twelve candidate paths each time — over a 141k-entry group that is millions of
    // filesystem calls and minutes of wall time. One 226 KB manifest answers the same question for
    // every SNO at once.
    //
    // It is also the MORE correct answer. A container whose key we lack never expands into the path
    // table at all, so a frame-header probe cannot see those assets — it silently under-reports
    // exactly the locked content the filter exists to reveal. The manifest lists them regardless.
    //
    // The key names are stored in the same byte order as the wowdev key files (verified against all
    // nine of ours), so a value here goes straight to haveTactKey with no swap. Note this is NOT
    // the order used by the EncryptedNameDict-0x<id>.dat filenames, which are byte-reversed.
    const QHash<int, QByteArray>& encryptedSnos();

    // ── Shared payloads: assets that deliberately have no payload of their own ───────────────────
    // source sno -> the sno whose payload it actually uses. From base/CoreTOCSharedPayloadsMapping.dat
    // (layout from d4data parse.js:140-147: u32 unread, u32 count, count x { u32 source, u32 dest }).
    //
    // 36,930 entries in the live build (34,852 in the July d4data snapshot),
    // over 16,000 of them textures. D4 deduplicates aggressively:
    // every *_FurMask that is uniformly rough points at roughness_default, every unused *_Emissive
    // at RenderTest_Black_Metal, and a female piece routinely shares the male one's AO/Rough/DyeMask
    // (RogF_stor235_BTS_AO -> RogM_stor235_BTS_AO).
    //
    // Not consulting this was a real gap, not an optimisation: those SNOs exist and are named, so
    // they resolve to a valid texture definition and then have no pixels anywhere in the install.
    // That is exactly the "no payload exists anywhere in this install" verdict the texture warning
    // was reporting - the pixels were never missing, they live under another sno.
    const QHash<int, int>& sharedPayloads();

    // The sno whose payload readPayloadBySno will ACTUALLY return for `sno` — itself, or the
    // shared-payload destination.
    //
    // Anything that pairs a payload with metadata must resolve this FIRST and describe the result
    // with the destination's metadata, not the source's. A FurMask redirected to roughness_default
    // is 4x4; decoding those bytes as the 1024x1024 the source's texture definition claims yields
    // garbage (BcDecode's size check turns most of it into a blank, which reads as "still broken").
    quint64 payloadSourceSno(quint64 sno);
    // The 8-byte key NAMES we hold (not the keys). The game ships one
    // base/EncryptedNameDict-0x<keyName>.dat per encryption key, each itself encrypted with that
    // key, so this is how the name-dict pass knows which of the 189 dicts are worth opening
    // instead of failing to decode 181 of them. See SnoIndex::applyEncryptedNameDicts.
    QVector<QByteArray> tactKeyNames() const;

    // buildId + a digest of the TACT key NAMES. THE signature for any cache whose CONTENT depends
    // on which keys we hold — not just on the game build. The TVFS path table already used this
    // (a key gates which containers expand); the CoreTOC index cache did NOT, and it needs it just
    // as much: the EncryptedNameDict pass can only name what its key decrypts, so adding a key
    // changes the index. Keyed on buildId alone, a newly-added key silently did nothing until the
    // next game patch — the cache loaded clean and the naming pass never re-ran.
    QString buildAndKeySignature() const;

    // ── Candidate-key verification ──────────────────────────────────────────────────────────────
    // Community key dumps are published per-product and get mixed up constantly — CascLib's
    // KeyService.cs, for instance, is Overwatch + WoW keys despite being the obvious place to look.
    // Guessing which of a list belongs to Diablo IV is not necessary: the install ships one
    // base/EncryptedNameDict-0x<keyName>.dat per key, encrypted with that key, and a correct key
    // decrypts it to a 0xABCD4567 header. So a key is D4's, and ours, exactly when its dict decodes.
    //
    // `keyFile` is a wowdev-format list (KEYNAME<space>KEY, 16 + 32 hex). Keys are registered as a
    // side effect, so a verified list is immediately usable. Returns one human-readable line per
    // candidate plus a summary; nothing is written to disk.
    QStringList verifyTactKeys(const QString& keyFile);
    // Stored size of a file by virtual path, or 0 if absent.

    // Iterate TVFS paths matching a simple prefix/substring mask ("*"/empty = all).
    int enumerate(const QString& mask, const std::function<bool(const Entry&)>& fn);

    QString lastError() const { return m_lastError; }

private:
    struct IndexEntry { quint32 archive = 0; quint64 offset = 0; quint32 size = 0; QString group; };

    QHash<QString, QString> locateConfig();
    bool loadAllIndices();
    bool loadIndexFile(const QString& path);
    QString findArchive(quint32 archiveNum, const QString& group) const;
    QByteArray readArchive(const IndexEntry& e) const;
    QByteArray readByEKeyHex(const QString& ekeyHex) const;
    void parseTvfs(const QByteArray& data, const QString& prefix,
                   QHash<QString, QVector<QByteArray>>& out) const;
    void expandNestedManifests();
    // Disk cache for the fully-expanded path table (m_root). Rebuilding it costs seconds of
    // TVFS/BLTE work per launch; the result depends ONLY on the vfs-root manifest (= m_buildId)
    // and the TACT key set, so those two form the cache signature.
    QString rootCacheSignature() const;
    bool loadRootCache(const QString& path, const QString& sig);
    void saveRootCache(const QString& path, const QString& sig) const;
    // Same recipe for the .idx index (signature = the .idx files' name/size/mtime set).
    bool loadIndexCacheFile(const QString& path, const QString& sig);
    void saveIndexCacheFile(const QString& path, const QString& sig) const;
    // First BLTE frame's Salsa20 key name (8 bytes) if the entry's first chunk is
    // 'E'-encrypted; empty when unencrypted/unreadable. Header-only probe, no decode.
    QByteArray frameKeyName(const IndexEntry& e) const;
    // Same probe against an ALREADY-OPEN archive, so a bulk scan opens each archive once. Also
    // reads only the ~12 + ~24 bytes it actually needs instead of an 8 KB window — over a whole
    // group that is the difference between a gigabyte of I/O and a few megabytes.
    static QByteArray frameKeyNameFrom(QFile& f, const IndexEntry& e);

    int loadKeysFromFile(const QString& path);   // parse one key file into m_tactKeys (caller holds mutex)

    // BLTE + frame decode
    QByteArray blteDecode(const QByteArray& data) const;
    // expectUncomp: the chunk header's declared uncompressed length, or -1 when unknown. Used to
    // VERIFY an 'E' frame decrypted correctly rather than assuming it did — see kNonceVariants.
    QByteArray decompressFrame(quint8 type, const QByteArray& data, int blockIndex,
                               int expectUncomp = -1) const;
    // variant selects how the Salsa20 nonce combines the IV with the block index. Frame 0 cannot
    // tell the constructions apart (block index 0 is a no-op in both), which is why a bug here only
    // shows on multi-frame files.
    QByteArray decryptFrame(const QByteArray& data, int blockIndex, int variant = 0) const;

    bool        m_ready = false;
    QString     m_gameDir;
    QString     m_wantProduct;   // product code REQUESTED by settings (may not exist in the install)
    QString     m_product;       // product code actually opened, from .build.info
    QString     m_gameVersion;   // version string of the opened row
    QString     m_buildId;     // TVFS root manifest hash = per-build fingerprint
    QString     m_lastError;
    QMutex      m_mutex;
    // Guards ONLY m_tactKeys: readFile() resolves index entries under m_mutex, then does the raw
    // archive read + BLTE inflate OUTSIDE it (each read opens its own QFile), so parallel bulk
    // workers actually decompress concurrently. decryptFrame's key lookup takes this tiny lock.
    mutable QMutex m_keysMutex;

    QHash<QByteArray, IndexEntry>          m_index;     // 9-byte EKey → location
    QHash<QString, QVector<QByteArray>>    m_root;      // path.toLower() → EKeys
    QHash<QByteArray, QByteArray>          m_tactKeys;  // keyName(8) → key(16)
    // Memory-only, guarded by m_mutex. tactKeyFor opens an archive and seeks per sno; filtering a
    // 40k-entry group would otherwise redo all of it on every list rebuild. Dropped with the reader,
    // never written to disk. Empty QByteArray = probed and found unencrypted.
    QHash<quint64, QByteArray>             m_keyProbe;
    QHash<int, QByteArray>                 m_encSnos;        // base/EncryptedSNOs.dat, sno -> key name
    bool                                   m_encSnosBuilt = false;
    QHash<int, int>                        m_sharedPay;      // source sno -> sno holding the payload
    bool                                   m_sharedPayBuilt = false;
    // Depth-limited so a malformed (or cyclic) mapping cannot recurse forever.
    QByteArray readPayloadBySno(quint64 sno, int depth);
    // findArchive stats up to twelve candidate paths per call and is on the path of EVERY asset
    // read, so the answer is remembered. Keyed on (archive, group); guarded by its own mutex
    // because readArchive deliberately runs outside m_mutex so bulk workers decompress in parallel.
    mutable QHash<QString, QString>        m_archivePath;
    mutable QMutex                         m_archiveMutex;

    // Coverage diagnostics (populated during expandNestedManifests, dumped by open()):
    // which container manifests expanded, which were skipped oversized, and which were
    // encrypted with a TACT key we don't hold. Written to casc_coverage.txt.
    QStringList m_covExpanded, m_covOversized, m_covEncrypted;
    void writeCoverageReport() const;   // dump the path/prefix breakdown + skip lists
};
