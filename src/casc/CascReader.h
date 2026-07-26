#pragma once
#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

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
    // Data/.build.config). `product` is accepted for API compatibility, unused.
    bool open(const QString& gameDir, const QString& product = QString());
    void close();
    bool isReady() const { return m_ready; }
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

    int loadKeysFromFile(const QString& path);   // parse one key file into m_tactKeys (caller holds mutex)

    // BLTE + frame decode
    QByteArray blteDecode(const QByteArray& data) const;
    QByteArray decompressFrame(quint8 type, const QByteArray& data, int blockIndex) const;
    QByteArray decryptFrame(const QByteArray& data, int blockIndex) const;

    bool        m_ready = false;
    QString     m_gameDir;
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

    // Coverage diagnostics (populated during expandNestedManifests, dumped by open()):
    // which container manifests expanded, which were skipped oversized, and which were
    // encrypted with a TACT key we don't hold. Written to casc_coverage.txt.
    QStringList m_covExpanded, m_covOversized, m_covEncrypted;
    void writeCoverageReport() const;   // dump the path/prefix breakdown + skip lists
};
