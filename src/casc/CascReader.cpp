#include "casc/CascReader.h"
#include "app/AppPaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTextStream>
#include <QtEndian>
#include <QtGlobal>

#include <lz4.h>
#include <lz4frame.h>
#include <zlib.h>

#include <atomic>

#include <algorithm>
#include <cstring>
#include <functional>

// ── Port of Diablo4AssetBrowser2/src/d4ab2/formats/casc_reader.py ────────────
// Big-endian / little-endian helpers operate on QByteArray with bounds-light
// access (callers validate sizes first).

namespace {

quint32 rdBE32(const QByteArray& d, int o) { return qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(d.constData() + o)); }
quint32 rdLE32(const QByteArray& d, int o) { return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(d.constData() + o)); }

int rdN_BE(const QByteArray& d, int o, int n)
{
    quint64 v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | quint8(d[o + i]);
    return int(v);
}

// ── Salsa20 (port of _salsa20_keystream / _decrypt_frame) ──
quint32 rotl32(quint32 v, int c) { return (v << c) | (v >> (32 - c)); }

QByteArray salsa20Keystream(const QByteArray& key, const QByteArray& nonce, int nbytes)
{
    const char* sigma = "expand 32-byte k";
    const char* tau   = "expand 16-byte k";
    QByteArray kfull = key;
    const char* cst = (key.size() == 16) ? tau : sigma;
    if (key.size() == 16) kfull = key + key;     // expand to 32
    quint32 c[4], kk[8], nn[2];
    for (int i = 0; i < 4; ++i) c[i] = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(cst + i * 4));
    for (int i = 0; i < 8; ++i) kk[i] = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(kfull.constData() + i * 4));
    for (int i = 0; i < 2; ++i) nn[i] = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(nonce.constData() + i * 4));

    QByteArray out;
    quint64 counter = 0;
    while (out.size() < nbytes) {
        quint32 s[16] = {
            c[0], kk[0], kk[1], kk[2],
            kk[3], c[1], nn[0], nn[1],
            quint32(counter & 0xFFFFFFFF), quint32((counter >> 32) & 0xFFFFFFFF), c[2], kk[4],
            kk[5], kk[6], kk[7], c[3]};
        quint32 x[16];
        std::memcpy(x, s, sizeof(x));
        for (int r = 0; r < 10; ++r) {
            x[4]^=rotl32(x[0]+x[12],7);  x[8]^=rotl32(x[4]+x[0],9);
            x[12]^=rotl32(x[8]+x[4],13); x[0]^=rotl32(x[12]+x[8],18);
            x[9]^=rotl32(x[5]+x[1],7);   x[13]^=rotl32(x[9]+x[5],9);
            x[1]^=rotl32(x[13]+x[9],13); x[5]^=rotl32(x[1]+x[13],18);
            x[14]^=rotl32(x[10]+x[6],7); x[2]^=rotl32(x[14]+x[10],9);
            x[6]^=rotl32(x[2]+x[14],13); x[10]^=rotl32(x[6]+x[2],18);
            x[3]^=rotl32(x[15]+x[11],7); x[7]^=rotl32(x[3]+x[15],9);
            x[11]^=rotl32(x[7]+x[3],13); x[15]^=rotl32(x[11]+x[7],18);
            x[1]^=rotl32(x[0]+x[3],7);   x[2]^=rotl32(x[1]+x[0],9);
            x[3]^=rotl32(x[2]+x[1],13);  x[0]^=rotl32(x[3]+x[2],18);
            x[6]^=rotl32(x[5]+x[4],7);   x[7]^=rotl32(x[6]+x[5],9);
            x[4]^=rotl32(x[7]+x[6],13);  x[5]^=rotl32(x[4]+x[7],18);
            x[11]^=rotl32(x[10]+x[9],7); x[8]^=rotl32(x[11]+x[10],9);
            x[9]^=rotl32(x[8]+x[11],13); x[10]^=rotl32(x[9]+x[8],18);
            x[12]^=rotl32(x[15]+x[14],7);  x[13]^=rotl32(x[12]+x[15],9);
            x[14]^=rotl32(x[13]+x[12],13); x[15]^=rotl32(x[14]+x[13],18);
        }
        for (int i = 0; i < 16; ++i) {
            quint32 v = x[i] + s[i];
            char b[4]; qToLittleEndian(v, reinterpret_cast<uchar*>(b));
            out.append(b, 4);
        }
        ++counter;
    }
    return out.left(nbytes);
}

// Streaming zlib inflate (no known output size).
QByteArray zlibInflate(const QByteArray& in)
{
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK) return {};
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData()));
    zs.avail_in = uInt(in.size());
    QByteArray out;
    char buf[65536];
    int ret = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) break;
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (zs.avail_out != 0 && ret != Z_STREAM_END && ret == Z_BUF_ERROR) break;
    } while (ret != Z_STREAM_END);
    inflateEnd(&zs);
    return out;
}

constexpr int ARCHIVE_FRAME_HEADER = 30;
const char* TVFS_MAGIC = "TVFS";
constexpr quint32 TVFS_FOLDER_NODE = 0x80000000u;
constexpr quint32 TVFS_FOLDER_SIZE_MASK = 0x7FFFFFFFu;

int tvfsOffsetFieldSize(int tableSize)
{
    if (tableSize > 0xFFFFFF) return 4;
    if (tableSize > 0xFFFF)   return 3;
    if (tableSize > 0xFF)     return 2;
    return 1;
}

// Pick a build row out of .build.info.
//
// `wantProduct` (e.g. "fenris" retail, or the PTR product code) selects among MULTIPLE active
// rows — an install root can carry more than one, and a PTR install is a different Product on
// the same file format. This used to take the first Active row and ignore the Product column
// entirely, which made the Settings "Game build (CASC product)" dropdown decorative: it was
// saved, passed down here and discarded. A control that looks like it works and does not is
// worse than no control.
//
// Fallbacks, in order: exact product match → first Active row → nothing. Falling back rather
// than failing matters because most installs have exactly one active row and the product code
// there is whatever Blizzard chose; refusing to open on a mismatch would break every user whose
// setting does not happen to match their install.
QString firstActiveBuildInfo(const QString& gameDir, QHash<QString, QString>& row,
                             const QString& wantProduct = QString())
{
    QFile f(gameDir + "/.build.info");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
    if (lines.size() < 2) return {};
    QStringList headers;
    for (const QString& h : lines[0].split('|')) headers << h.split('!').first();

    QHash<QString, QString> firstActive;
    for (int i = 1; i < lines.size(); ++i) {
        const QStringList vals = lines[i].split('|');
        if (vals.size() < headers.size()) continue;
        QHash<QString, QString> r;
        for (int k = 0; k < headers.size(); ++k) r.insert(headers[k], vals[k]);
        if (r.value("Active") != "1") continue;
        if (firstActive.isEmpty()) firstActive = r;
        if (!wantProduct.isEmpty()
            && r.value("Product").trimmed().compare(wantProduct.trimmed(), Qt::CaseInsensitive) == 0) {
            row = r;
            return r.value("Build Key").trimmed().toLower();
        }
    }
    if (!firstActive.isEmpty()) {
        row = firstActive;
        return firstActive.value("Build Key").trimmed().toLower();
    }
    return {};
}

QHash<QString, QString> parseKvFile(const QString& text)
{
    QHash<QString, QString> r;
    for (const QString& raw : text.split('\n')) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const int eq = line.indexOf('=');
        if (eq < 0) continue;
        r.insert(line.left(eq).trimmed().toLower(), line.mid(eq + 1).trimmed());
    }
    return r;
}

}  // namespace

QString CascReader::lineageKey() const
{
    if (!m_product.isEmpty()) return m_product;
    // Normalised so a trailing slash or a drive-letter case difference cannot look like a second
    // install. Only the leaf name is used — moving the folder to another drive should keep the
    // baseline, and the full path would throw it away.
    const QString dir = QDir(m_gameDir).dirName().trimmed().toLower();
    return dir.isEmpty() ? QString() : QStringLiteral("dir:") + dir;
}

QString CascReader::gameVersion(const QString& gameDir)
{
    QHash<QString, QString> row;
    firstActiveBuildInfo(gameDir, row);
    return row.value(QStringLiteral("Version")).trimmed();
}

// NOTE: nothing in src/ calls this today — open() does its own reset, so that is the path that
// actually runs. It is kept because it is the honest counterpart to open(), and it is maintained in
// step with it deliberately: the two reset lists must not drift, or whichever one a future caller
// picks decides whether stale state survives.
void CascReader::close()
{
    QMutexLocker lock(&m_mutex);
    m_ready = false;
    m_index.clear();
    m_keyProbe.clear();
    m_root.clear();
    m_gameDir.clear();
    // Identity of the install being dropped. open() clears these too; close() did not, so anything
    // calling it would keep answering lineageKey()/product()/openedVersion() with the OLD install —
    // and lineageKey() is what keeps retail and PTR from overwriting each other's "Latest" baseline.
    m_product.clear();
    m_wantProduct.clear();
    m_gameVersion.clear();
    m_buildId.clear();
    // Both are derived from the install we are dropping. The archive paths are absolute, so leaving
    // them would point a re-open at the OLD install's files — the retail/PTR switch is exactly the
    // case that would hit, and it would look like corrupt assets rather than a stale cache.
    m_encSnos.clear();
    m_encSnosBuilt = false;
    m_sharedPay.clear();
    m_sharedPayBuilt = false;
    {
        QMutexLocker cl(&m_archiveMutex);
        m_archivePath.clear();
    }
}

QHash<QString, QString> CascReader::locateConfig()
{
    // Steam: Data/.build.config
    QFile steam(m_gameDir + "/Data/.build.config");
    if (steam.open(QIODevice::ReadOnly | QIODevice::Text))
        return parseKvFile(QString::fromUtf8(steam.readAll()));

    // Battle.net: .build.info → Build Key → Data/config/xx/yy/<key>
    QHash<QString, QString> info;
    const QString buildKey = firstActiveBuildInfo(m_gameDir, info, m_wantProduct);
    if (!buildKey.isEmpty() && buildKey.size() >= 4) {
        const QString cfgPath = QStringLiteral("%1/Data/config/%2/%3/%4")
            .arg(m_gameDir, buildKey.mid(0, 2), buildKey.mid(2, 2), buildKey);
        QFile cf(cfgPath);
        if (cf.open(QIODevice::ReadOnly | QIODevice::Text))
            return parseKvFile(QString::fromUtf8(cf.readAll()));
    }
    if (!info.isEmpty()) {
        QHash<QString, QString> lower;
        for (auto it = info.constBegin(); it != info.constEnd(); ++it)
            lower.insert(it.key().toLower(), it.value());
        return lower;
    }
    return {};
}

bool CascReader::loadIndexFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = f.readAll();
    if (data.size() < 24) return false;

    const int headerHashSize = int(rdLE32(data, 0));
    const int padPos = (8 + headerHashSize + 0x0F) & ~0x0F;
    if (padPos + 8 > data.size()) return false;
    const int entryStart = padPos + 8;

    QString group = QFileInfo(path).dir().dirName();
    if (group != "data" && group != "ecache" && group != "fenris") group = "data";

    constexpr int ENTRY = 18, KEY = 9;
    const int n = (data.size() - entryStart) / ENTRY;
    for (int i = 0; i < n; ++i) {
        const int off = entryStart + i * ENTRY;
        const QByteArray ekey = data.mid(off, KEY);
        if (ekey == QByteArray(KEY, '\0')) continue;
        const quint8 indexHigh = quint8(data[off + 9]);
        const quint32 indexLow = rdBE32(data, off + 10);
        IndexEntry e;
        e.archive = (quint32(indexHigh) << 2) | ((indexLow & 0xC0000000u) >> 30);
        e.offset  = indexLow & 0x3FFFFFFFu;
        e.size    = rdLE32(data, off + 14);
        e.group   = group;
        m_index.insert(ekey, e);
    }
    return true;
}

bool CascReader::loadAllIndices()
{
    const QString dataRoot = m_gameDir + "/Data";
    if (!QDir(dataRoot).exists()) { m_lastError = "Data/ not found"; return false; }
    // Collect the .idx set FIRST — its (name,size,mtime) list is the cache signature. The game
    // rewrites .idx files on patch/repair/background download, so any change rebuilds; parsing
    // ~1.15M entries costs ~2 s per launch that the cache turns into a bulk read.
    QStringList files;
    {
        QDirIterator it(dataRoot, QStringList{"*.idx"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) files << it.next();
    }
    files.sort();   // deterministic order for BOTH the signature and the parse
    QCryptographicHash sigH(QCryptographicHash::Md5);
    for (const QString& fp : files) {
        const QFileInfo fi(fp);
        sigH.addData(fi.fileName().toUtf8());
        sigH.addData(QByteArray::number(fi.size()));
        sigH.addData(QByteArray::number(fi.lastModified().toMSecsSinceEpoch()));
    }
    const QString sig = QString::fromLatin1(sigH.result().toHex());
    // Version is in the FILENAME on purpose — see util/CacheVersioning.h. Bump it whenever what
    // this file MEANS changes, so a previous build's cache cannot be read at all.
    const QString cachePath = AppPaths::dataDir() + QStringLiteral("/casc_index_v1.bin");
    QElapsedTimer et; et.start();
    if (loadIndexCacheFile(cachePath, sig)) {
        qInfo("CASC: .idx index from cache — %d entries in %lld ms",
              int(m_index.size()), et.elapsed());
        return !m_index.isEmpty();
    }
    for (const QString& f : files) loadIndexFile(f);
    qInfo("CASC: parsed %d .idx files under %s (%lld ms)", int(files.size()),
          qPrintable(dataRoot), et.elapsed());
    if (!m_index.isEmpty()) saveIndexCacheFile(cachePath, sig);
    return !m_index.isEmpty();
}

// ── .idx index disk cache — same compact-binary recipe as the TVFS path table ────────────────
// magic · sig · count · per entry [u8 keyLen + key, u32 archive, u64 offset, u32 size,
// u8 groupLen + utf8 group].
static constexpr quint32 kIdxCacheMagic = 0x49445831;   // 'IDX1'

bool CascReader::loadIndexCacheFile(const QString& path, const QString& sig)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray all = f.readAll();
    const char* p = all.constData();
    const char* end = p + all.size();
    auto need = [&](qint64 n) { return end - p >= n; };
    if (!need(8)) return false;
    if (qFromLittleEndian<quint32>(p) != kIdxCacheMagic) return false;
    const quint32 sigLen = qFromLittleEndian<quint32>(p + 4);
    p += 8;
    if (!need(sigLen)) return false;
    if (QString::fromUtf8(p, int(sigLen)) != sig) return false;   // .idx set changed
    p += sigLen;
    if (!need(4)) return false;
    const quint32 count = qFromLittleEndian<quint32>(p);
    p += 4;
    QHash<QByteArray, IndexEntry> index;
    index.reserve(int(count));
    for (quint32 i = 0; i < count; ++i) {
        if (!need(1)) return false;
        const quint8 kl = quint8(*p++);
        if (!need(kl + 16 + 1)) return false;
        QByteArray key(p, kl); p += kl;
        IndexEntry e;
        e.archive = qFromLittleEndian<quint32>(p); p += 4;
        e.offset  = qFromLittleEndian<quint64>(p); p += 8;
        e.size    = qFromLittleEndian<quint32>(p); p += 4;
        const quint8 gl = quint8(*p++);
        if (!need(gl)) return false;
        e.group = QString::fromUtf8(p, gl); p += gl;
        index.insert(std::move(key), std::move(e));
    }
    m_index = std::move(index);
    return !m_index.isEmpty();
}

void CascReader::saveIndexCacheFile(const QString& path, const QString& sig) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QString part = path + QStringLiteral(".part");
    QFile f(part);
    if (!f.open(QIODevice::WriteOnly)) return;
    QByteArray out;
    out.reserve(48 * 1024 * 1024);
    auto put32 = [&](quint32 v) { quint32 le = qToLittleEndian(v); out.append(reinterpret_cast<const char*>(&le), 4); };
    auto put64 = [&](quint64 v) { quint64 le = qToLittleEndian(v); out.append(reinterpret_cast<const char*>(&le), 8); };
    put32(kIdxCacheMagic);
    const QByteArray sigU = sig.toUtf8();
    put32(quint32(sigU.size()));
    out.append(sigU);
    put32(quint32(m_index.size()));
    for (auto it = m_index.constBegin(); it != m_index.constEnd(); ++it) {
        const QByteArray gu = it.value().group.toUtf8();
        if (it.key().size() > 0xFF || gu.size() > 0xFF) continue;   // absurd entry — skip
        out.append(char(quint8(it.key().size())));
        out.append(it.key());
        put32(it.value().archive);
        put64(it.value().offset);
        put32(it.value().size);
        out.append(char(quint8(gu.size())));
        out.append(gu);
    }
    f.write(out);
    f.close();
    QFile::remove(path);
    QFile::rename(part, path);
    qInfo("CASC: .idx index cached (%lld MB)", qint64(out.size()) / (1024 * 1024));
}

QString CascReader::findArchive(quint32 archiveNum, const QString& group) const
{
    // Up to twelve QFileInfo::exists calls to locate one archive, and readArchive calls this for
    // EVERY asset read — so the answer is remembered. Measured cost of not doing so: the "only
    // encrypted" filter probing a 141k-entry group spent minutes almost entirely in these stats.
    // Its own mutex, because readArchive deliberately runs outside m_mutex so parallel bulk workers
    // decompress concurrently; taking m_mutex here would serialise them again.
    const QString cacheKey = group + QLatin1Char('/') + QString::number(archiveNum);
    {
        QMutexLocker cl(&m_archiveMutex);
        const auto hit = m_archivePath.constFind(cacheKey);
        if (hit != m_archivePath.constEnd()) return hit.value();
    }
    const QStringList dirs{m_gameDir + "/Data/" + group, m_gameDir + "/Data/data", m_gameDir + "/Data"};
    for (const QString& d : dirs) {
        const QStringList cands{
            QStringLiteral("%1/data.%2").arg(d).arg(archiveNum, 5, 10, QLatin1Char('0')),
            QStringLiteral("%1/data.%2").arg(d).arg(archiveNum, 3, 10, QLatin1Char('0')),
            QStringLiteral("%1/%2").arg(d).arg(archiveNum, 4, 10, QLatin1Char('0')),
            QStringLiteral("%1/%2").arg(d).arg(archiveNum, 8, 10, QLatin1Char('0'))};
        for (const QString& p : cands)
            if (QFileInfo::exists(p)) {
                QMutexLocker cl(&m_archiveMutex);
                m_archivePath.insert(cacheKey, p);
                return p;
            }
    }
    // A miss is cached too. Without that, every read of a missing archive redoes all twelve stats,
    // which is the exact case a locked-content scan hits hardest.
    QMutexLocker cl(&m_archiveMutex);
    m_archivePath.insert(cacheKey, QString());
    return {};
}

QByteArray CascReader::readArchive(const IndexEntry& e) const
{
    const QString path = findArchive(e.archive, e.group);
    if (path.isEmpty()) return {};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    if (!f.seek(qint64(e.offset))) return {};
    const QByteArray comp = f.read(e.size);
    if (comp.size() <= ARCHIVE_FRAME_HEADER) return {};
    return blteDecode(comp.mid(ARCHIVE_FRAME_HEADER));
}

// How the Salsa20 nonce is built from the frame's IV and its block index. Both constructions are in
// circulation in CASC implementations and they AGREE for block index 0, so a single-frame file
// cannot distinguish them — which is precisely why this went unnoticed until a 27-frame payload
// turned up. Rather than pick one by argument, both are tried and the chunk header's declared
// uncompressed length decides, so the choice is verified against the data every time.
//   0 = append the block index to the IV, then take 8 bytes (the original behaviour)
//   1 = XOR the block index into the IV's first 4 bytes, then pad to 8
static constexpr int kNonceVariants = 2;

QByteArray CascReader::decryptFrame(const QByteArray& data, int blockIndex, int variant) const
{
    if (data.isEmpty()) return {};
    const int keyLen = quint8(data[0]);
    if (1 + keyLen + 1 > data.size()) return {};
    QByteArray keyName = data.mid(1, keyLen);
    std::reverse(keyName.begin(), keyName.end());   // stored little-endian
    int pos = 1 + keyLen;
    const int ivLen = quint8(data[pos]);
    const QByteArray iv = data.mid(pos + 1, ivLen);
    pos += 1 + ivLen;
    if (pos >= data.size()) return {};
    const char encType = data[pos];
    const QByteArray enc = data.mid(pos + 1);
    if (encType != 'S') return {};
    QByteArray key;
    { QMutexLocker kl(&m_keysMutex); key = m_tactKeys.value(keyName); }
    if (key.isEmpty()) {
        // A missing TACT key returned empty here and nothing else, so the asset simply failed to
        // load and looked identical to a parse bug. It is not one — the content is encrypted and
        // this build has no key for it. Named once per key so a run reports exactly which keys
        // would unlock more content rather than leaving it to guesswork.
        //
        // Scale, from the snapshot's own EncryptedSNOs manifest: 14101 SNOs are encrypted under 197
        // distinct keys — 4053 textures, 1184 materials, 614 appearances, 415 cloth. The shipped
        // key list carries 8 of those keys, so most encrypted content stays locked and every one of
        // those assets was failing silently.
        static QMutex warnMx;
        static QSet<QByteArray> warned;
        QMutexLocker wl(&warnMx);
        if (!warned.contains(keyName)) {
            warned.insert(keyName);
            qWarning("CASC: encrypted content needs TACT key %s, which is not loaded — those assets "
                     "cannot be decoded (add the key to the TACT key file to unlock them)",
                     keyName.toHex().constData());
        }
        return {};
    }
    QByteArray nonce = iv;
    if (variant == 0) {
        char bi[4]; qToLittleEndian(quint32(blockIndex), reinterpret_cast<uchar*>(bi));
        nonce.append(bi, 4);
        nonce = nonce.left(8);
    } else {
        for (int i = 0; i < 4 && i < nonce.size(); ++i)
            nonce[i] = char(uchar(nonce[i]) ^ uchar((quint32(blockIndex) >> (8 * i)) & 0xFF));
    }
    while (nonce.size() < 8) nonce.append('\0');
    nonce = nonce.left(8);
    const QByteArray ks = salsa20Keystream(key, nonce, enc.size());
    QByteArray dec(enc.size(), '\0');
    for (int i = 0; i < enc.size(); ++i) dec[i] = enc[i] ^ ks[i];
    return dec;
}

QByteArray CascReader::decompressFrame(quint8 type, const QByteArray& data, int blockIndex,
                                       int expectUncomp) const
{
    switch (type) {
    case 0x4E: return data;                       // 'N' raw
    case 0x5A: return zlibInflate(data);          // 'Z' zlib
    case 0x45: {                                  // 'E' encrypted (Salsa20)
        // Try each nonce construction and keep the one the chunk header vouches for. A wrong nonce
        // yields plausible-looking bytes whose first byte is then read as a frame type, so testing
        // the DECOMPRESSED LENGTH is the only check that actually discriminates — an inner type
        // byte of 'N' passes by luck roughly once in 256.
        // The winning construction is a property of the game build, not of a frame, so the first
        // VERIFIED win is remembered and tried first from then on. Without this every encrypted
        // frame pays two Salsa20 passes plus two inflates — a needless doubling on the hot path for
        // a question that was settled once. Memory-only, and never written from an unverified guess.
        static std::atomic<int> preferred{0};
        const int first = preferred.load(std::memory_order_relaxed);
        QByteArray fallback;
        for (int n = 0; n < kNonceVariants; ++n) {
            const int v = (first + n) % kNonceVariants;
            const QByteArray dec = decryptFrame(data, blockIndex, v);
            if (dec.isEmpty()) continue;          // no key — no variant will help
            const QByteArray out = decompressFrame(quint8(dec[0]), dec.mid(1), blockIndex,
                                                   expectUncomp);
            if (expectUncomp >= 0 && out.size() == expectUncomp) {
                if (v != first) {
                    preferred.store(v, std::memory_order_relaxed);
                    qInfo("BLTE: Salsa20 nonce variant %d confirmed by frame length — preferring it "
                          "from here (variant 0 appends the block index to the IV, 1 XORs it in)", v);
                }
                return out;
            }
            if (fallback.isEmpty()) fallback = out;
            if (expectUncomp < 0) return out;     // nothing to verify against — first result stands
        }
        return fallback;
    }
    case 0x46: {                                  // 'F' LZ4 frame
        LZ4F_decompressionContext_t ctx = nullptr;
        if (LZ4F_isError(LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION))) return {};
        QByteArray out;
        const char* src = data.constData();
        size_t srcRemaining = size_t(data.size());
        size_t srcPos = 0;
        char buf[65536];
        size_t ret = 1;
        while (srcPos < size_t(data.size()) && ret != 0) {
            size_t dstSize = sizeof(buf);
            size_t srcSize = srcRemaining;
            ret = LZ4F_decompress(ctx, buf, &dstSize, src + srcPos, &srcSize, nullptr);
            if (LZ4F_isError(ret)) { out.clear(); break; }
            out.append(buf, int(dstSize));
            srcPos += srcSize;
            srcRemaining -= srcSize;
            if (dstSize == 0 && srcSize == 0) break;
        }
        LZ4F_freeDecompressionContext(ctx);
        return out;
    }
    case 0x42: {                                  // 'B' LZ4 block (4-byte LE size prefix)
        if (data.size() < 4) return {};
        const quint32 uncomp = rdLE32(data, 0);
        QByteArray out(int(uncomp), '\0');
        const int n = LZ4_decompress_safe(data.constData() + 4, out.data(),
                                          data.size() - 4, int(uncomp));
        if (n < 0) return {};
        out.resize(n);
        return out;
    }
    default: return data;                          // unknown ⇒ likely missing key
    }
}

QByteArray CascReader::blteDecode(const QByteArray& data) const
{
    if (!data.startsWith("BLTE")) return data;     // already raw
    if (data.size() < 8) return {};
    const quint32 headerSize = rdBE32(data, 4);
    if (headerSize == 0) {
        if (data.size() < 9) return {};
        return decompressFrame(quint8(data[8]), data.mid(9), 0);
    }
    if (data.size() < 12) return {};
    const int chunkCount = rdN_BE(data, 9, 3);     // skip flags@8
    int pos = 12;
    QVector<QPair<int, int>> chunks;               // (comp, uncomp)
    for (int i = 0; i < chunkCount; ++i) {
        if (pos + 24 > data.size()) return {};
        const int comp = int(rdBE32(data, pos));
        const int uncomp = int(rdBE32(data, pos + 4));
        pos += 24;                                  // 4 + 4 + 16(md5)
        chunks.append({comp, uncomp});
    }
    QByteArray out;
    int dataPos = pos;
    // Every chunk header carries its UNCOMPRESSED length and we were ignoring it, so a frame that
    // failed to decode appended nothing and the loop carried on. The caller received a short buffer
    // with no indication it was short — which is exactly how necF_stor245_TRS presented: a meta
    // asking for 1666548 payload bytes against a 954470-byte read, and a parser blamed for it.
    // A partial asset is a FAILURE, not a smaller asset. Now measured per frame and reported.
    int badFrames = 0, firstBad = -1;
    char firstBadType = 0;
    QByteArray firstBadKey;
    qint64 wantTotal = 0;
    for (int bi = 0; bi < chunks.size(); ++bi) {
        const int comp = chunks[bi].first, uncomp = chunks[bi].second;
        wantTotal += uncomp;
        if (dataPos + comp > data.size()) {
            qWarning("BLTE: archive truncated at frame %d of %d — need %d byte(s) have %d",
                     bi, int(chunks.size()), comp, int(data.size()) - dataPos);
            ++badFrames;
            break;
        }
        const QByteArray frame = data.mid(dataPos, comp);
        dataPos += comp;
        if (frame.isEmpty()) continue;
        const QByteArray dec = decompressFrame(quint8(frame[0]), frame.mid(1), bi, uncomp);
        if (dec.size() != uncomp) {
            if (++badFrames == 1) {
                firstBad = bi;
                firstBadType = frame[0];
                // For an 'E' frame the key name is the first field, little-endian. Naming it turns
                // "some asset failed" into "this specific key would fix it" — and frames within one
                // file can use DIFFERENT keys, which a first-frame-only probe cannot see.
                if (frame[0] == 'E' && frame.size() > 2) {
                    const int keyLen = quint8(frame[1]);
                    if (2 + keyLen <= frame.size()) {
                        firstBadKey = frame.mid(2, keyLen);
                        std::reverse(firstBadKey.begin(), firstBadKey.end());
                    }
                }
            }
        }
        out.append(dec);
    }
    // A frame we hold no key for is LOCKED CONTENT, not a fault: expected, unfixable here, and
    // already reported once per key by decryptFrame. Warning again per asset produced 258 lines in
    // one run — enough to bury the case this check exists to catch, which is a frame that failed
    // WITH the key held (a wrong key, a bad nonce, a corrupt archive).
    bool keyHeld = true;
    if (!firstBadKey.isEmpty()) {
        QMutexLocker kl(&m_keysMutex);
        keyHeld = !m_tactKeys.value(firstBadKey).isEmpty();
    }
    if (badFrames > 0 && keyHeld)
        qWarning("BLTE: INCOMPLETE decode — %d of %d frame(s) failed, %lld of %lld byte(s) "
                 "recovered; first failure frame %d type '%c'%s%s",
                 badFrames, int(chunks.size()), qint64(out.size()), wantTotal, firstBad,
                 firstBadType ? firstBadType : '?',
                 firstBadKey.isEmpty() ? "" : " key ",
                 firstBadKey.isEmpty() ? "" : firstBadKey.toHex().constData());
    return out;
}

// (isDecryptable removed — no callers; decryptability is tracked by the index flags.)

QByteArray CascReader::readByEKeyHex(const QString& ekeyHex) const
{
    if (ekeyHex.isEmpty()) return {};
    const QByteArray full = QByteArray::fromHex(ekeyHex.toLatin1());
    const QByteArray key = full.left(9);
    auto it = m_index.constFind(key);
    if (it == m_index.constEnd()) return {};
    return readArchive(it.value());
}

void CascReader::parseTvfs(const QByteArray& data, const QString& prefix,
                           QHash<QString, QVector<QByteArray>>& out) const
{
    if (data.size() < 0x2C || !data.startsWith(TVFS_MAGIC)) return;
    const int ekeySize = quint8(data[6]);

    const int pathOff = rdN_BE(data, 12, 4), pathSize = rdN_BE(data, 16, 4);
    const int vfsOff = rdN_BE(data, 20, 4), vfsSize = rdN_BE(data, 24, 4);
    const int cftOff = rdN_BE(data, 28, 4), cftSize = rdN_BE(data, 32, 4);

    const QByteArray P = data.mid(pathOff, pathSize);
    const QByteArray V = data.mid(vfsOff, vfsSize);
    const QByteArray CF = data.mid(cftOff, cftSize);
    const int cftOs = tvfsOffsetFieldSize(cftSize);
    const int plen = P.size();
    const QByteArray pfx = prefix.toUtf8();

    auto readSpans = [&](int vfsPos) -> QVector<QByteArray> {
        QVector<QByteArray> eks;
        if (vfsPos >= V.size()) return eks;
        const int spanCount = quint8(V[vfsPos]);
        int pos = vfsPos + 1;
        for (int s = 0; s < spanCount; ++s) {
            if (pos + 8 + cftOs > V.size()) break;
            const int cftPos = rdN_BE(V, pos + 8, cftOs);
            pos += 8 + cftOs;
            const QByteArray ek = CF.mid(cftPos, ekeySize);
            if (ek.size() == ekeySize) eks.append(ek);
        }
        return eks;
    };

    // Recursive walk of the path file table.
    std::function<void(int, int, const QByteArray&)> walk =
        [&](int start, int end, const QByteArray& path) {
        int p = start;
        while (p < end) {
            bool pre = (quint8(P[p]) == 0);
            if (pre) ++p;
            QByteArray name;
            if (p < end && quint8(P[p]) != 0xFF) {
                const int ln = quint8(P[p]);
                name = P.mid(p + 1, ln);
                p += 1 + ln;
            }
            bool post = false;
            if (p < end && quint8(P[p]) == 0) { post = true; ++p; }
            bool hasNode = false; quint32 node = 0;
            if (p < end && quint8(P[p]) == 0xFF && p + 5 <= P.size()) {
                node = rdBE32(P, p + 1);
                p += 5;
                hasNode = true;
            }
            QByteArray seg;
            if (pre) seg.append('/');
            seg.append(name);
            if (post) seg.append('/');
            const QByteArray newPath = path + seg;

            if (hasNode) {
                if (node & TVFS_FOLDER_NODE) {
                    const int folderEnd = p + int(node & TVFS_FOLDER_SIZE_MASK) - 4;
                    walk(p, qMin(folderEnd, end), newPath);
                    p = folderEnd;
                } else {
                    const QVector<QByteArray> eks = readSpans(int(node));
                    if (!eks.isEmpty()) {
                        QByteArray full = pfx + newPath;
                        out.insert(QString::fromLatin1(full).toLower(), eks);
                    }
                }
            }
        }
    };

    if (plen >= 5 && quint8(P[0]) == 0xFF) {
        const quint32 node = rdBE32(P, 1);
        if (node & TVFS_FOLDER_NODE) {
            walk(5, qMin(5 + int(node & TVFS_FOLDER_SIZE_MASK) - 4, plen), QByteArray());
            return;
        }
    }
    walk(0, plen, QByteArray());
}

// First BLTE frame's Salsa20 key name (8 bytes) if the entry's first chunk is
// 'E'-encrypted; empty when unencrypted/unreadable. Header-only probe, no decode.
QByteArray CascReader::frameKeyNameFrom(QFile& f, const IndexEntry& e)
{
    const qint64 base = qint64(e.offset) + ARCHIVE_FRAME_HEADER;
    if (!f.seek(base)) return {};
    // Two targeted reads, not one big window. The BLTE header is 12 bytes and tells us where the
    // first frame's header sits; the frame header we need is 2 bytes plus a key name of at most 16.
    // The old 8 KB read was ~200x more data than that, which only mattered once a caller started
    // probing a whole group.
    const QByteArray head = f.read(qMin<quint32>(e.size, 12u));
    if (!head.startsWith("BLTE") || head.size() < 9) return {};
    const quint32 headerSize = rdBE32(head, 4);
    int frameStart;
    if (headerSize == 0) {
        frameStart = 8;
    } else {
        if (head.size() < 12) return {};
        frameStart = 12 + 24 * rdN_BE(head, 9, 3);
    }
    if (frameStart < 0 || quint32(frameStart) + 2 > e.size) return {};
    if (!f.seek(base + frameStart)) return {};
    const QByteArray fh = f.read(qMin<quint32>(e.size - quint32(frameStart), 24u));
    if (fh.size() < 2) return {};
    if (quint8(fh[0]) != 0x45) return {};                  // not 'E'-encrypted
    const int keyLen = quint8(fh[1]);
    if (keyLen <= 0 || 2 + keyLen > fh.size()) return {};
    QByteArray keyName = fh.mid(2, keyLen);
    std::reverse(keyName.begin(), keyName.end());          // stored little-endian
    return keyName;
}

QByteArray CascReader::frameKeyName(const IndexEntry& e) const
{
    const QString ap = findArchive(e.archive, e.group);
    if (ap.isEmpty()) return {};
    QFile f(ap);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return frameKeyNameFrom(f, e);
}

void CascReader::expandNestedManifests()
{
    // Some TVFS entries are containers (nested manifests), not files: the root
    // holds "base" plus locale/seasonal/event packs, and packs can nest further.
    // The old single-pass walk also skipped every leaf containing '_' — which is
    // precisely where encrypted seasonal/event trees (e.g. collab content) live —
    // so those subtrees never entered m_root at all. Now: walk RECURSIVELY over a
    // worklist, probing every entry whose leaf looks like a container (no '.'
    // extension, not a numeric sno). Only the known-irrelevant locale text/speech
    // packs are skipped to keep the path table lean.
    //
    // Encrypted containers decode only if their TACT key is already loaded —
    // MainWindow applies keys BEFORE open() for exactly this reason. A container
    // whose first BLTE frame is 'E'-encrypted with an unknown key is reported
    // with its key id, so a missing key shows up in the log instead of the
    // subtree silently vanishing.
    static const QRegularExpression localePackRe(
        QStringLiteral("^[a-z]{4}_(text|speech|cutscene|video|movies?)$"));
    // Container-name heuristic: no '.' extension, contains at least one LETTER
    // (container names are words like "base"/"zhhm_base"; data files are numeric —
    // base/meta/<sno> and base/child/<sno>-<idx>, whose '-' is NOT a letter), and
    // not a locale text/speech/cutscene pack (irrelevant or huge). The letter
    // requirement matters: the first run probed 16k base/child/<sno>-<idx> payload
    // chunks (multi-MB BLTE decodes each) and spent ~15 s on it.
    // Opt-in: also expand locale text/speech/cutscene/video packs (off by default — they add
    // millions of localized names that models/textures don't need, and slow indexing).
    const bool includeLocale = QSettings().value(QStringLiteral("casc/includeLocalePacks"), false).toBool();
    auto looksContainer = [includeLocale](const QString& p) -> bool {
        const QString lf = p.section('/', -1);
        if (lf.isEmpty() || lf.contains('.')) return false;
        if (!includeLocale && localePackRe.match(lf).hasMatch()) return false;
        for (const QChar c : lf)
            if (c.isLetter()) return true;   // has a word-like name → container-ish
        return false;                        // digits/punctuation only = data file
    };
    // TVFS manifests are small (the root is ~12 KB; even 'base' with ~1M entries is
    // tens of MB stored). Anything bigger is a DATA pack — BLTE-decoding it just to
    // probe would materialise its whole uncompressed body and can OOM-crash the app
    // (this is exactly what happened with a multi-GB pack on first run).
    constexpr quint32 kMaxManifestStored = 64u * 1024u * 1024u;
    m_covExpanded.clear(); m_covOversized.clear(); m_covEncrypted.clear();
    QList<QString> work = m_root.keys();
    QSet<QString> visited;
    QSet<QByteArray> missingKeys;
    int expanded = 0, encryptedSkipped = 0;
    // Deeper worklist (was 4): seasonal/event/collab trees can nest several levels.
    for (int depth = 0; depth < 8 && !work.isEmpty(); ++depth) {
        QList<QString> next;
        const QList<QString> pass = work;   // snapshot; `next` collects the new layer
        qInfo("CASC: manifest expansion pass %d — %d candidate path(s)", depth, int(pass.size()));
        for (const QString& path : pass) {
            if (!looksContainer(path)) continue;
            if (visited.contains(path)) continue;
            visited.insert(path);
            const QVector<QByteArray> eks = m_root.value(path);
            if (eks.isEmpty()) continue;
            auto it = m_index.constFind(eks.first());
            if (it == m_index.constEnd()) continue;
            if (it.value().size > kMaxManifestStored) {
                qInfo("CASC: skipping oversized container probe '%s' (%u bytes stored)",
                      qPrintable(path), it.value().size);
                m_covOversized << QStringLiteral("%1  (%2 MB stored)")
                                     .arg(path).arg(it.value().size / 1048576.0, 0, 'f', 1);
                continue;
            }
            if (it.value().size > 4u * 1024u * 1024u)   // breadcrumb for future crash triage
                qInfo("CASC: probing large container '%s' (%u bytes stored)",
                      qPrintable(path), it.value().size);
            const QByteArray nested = readArchive(it.value());
            if (!nested.startsWith(TVFS_MAGIC)) {
                if (nested.isEmpty()) {
                    const QByteArray kn = frameKeyName(it.value());
                    if (!kn.isEmpty() && !m_tactKeys.contains(kn)) {
                        ++encryptedSkipped;
                        missingKeys.insert(kn);
                        m_covEncrypted << QStringLiteral("%1  [needs TACT key %2]")
                                             .arg(path, QString::fromLatin1(kn.toHex()));
                    }
                }
                continue;
            }
            QHash<QString, QVector<QByteArray>> sub;
            parseTvfs(nested, path + "/", sub);
            int fresh = 0;
            for (auto sit = sub.constBegin(); sit != sub.constEnd(); ++sit) {
                if (!m_root.contains(sit.key())) {
                    ++fresh;
                    // Only container-looking paths go on the next pass's worklist —
                    // queueing a million file paths just to skip them wastes memory.
                    if (looksContainer(sit.key())) next.append(sit.key());
                }
                m_root.insert(sit.key(), sit.value());
            }
            qInfo("CASC: nested manifest '%s' -> %d paths (%d new)",
                  qPrintable(path), int(sub.size()), fresh);
            m_covExpanded << QStringLiteral("%1  -> %2 paths (%3 new)")
                                .arg(path).arg(sub.size()).arg(fresh);
            ++expanded;
        }
        work = next;
    }
    if (expanded == 0)
        qWarning("CASC: no nested manifests expanded (base container not found?)");
    if (encryptedSkipped) {
        QStringList ids;
        for (const QByteArray& k : missingKeys) ids << QString::fromLatin1(k.toHex());
        ids.sort();
        qWarning("CASC: %d container manifest(s) undecryptable — missing TACT key id(s): %s "
                 "(add them to the keys folder and File > Reload)",
                 encryptedSkipped, qPrintable(ids.join(QStringLiteral(", "))));
    }
}

// Dump a human-readable coverage breakdown to casc_coverage.txt next to the exe, so the
// gap between surfaced names and the true archive can be diagnosed without a debugger: a
// per-prefix path count, the nested manifests that expanded, and — crucially — the
// containers that were skipped because they're encrypted (missing TACT key) or oversized.
void CascReader::writeCoverageReport() const
{
    QMap<QString, int> byPrefix;   // first two path segments, e.g. "base/meta"
    for (auto it = m_root.constBegin(); it != m_root.constEnd(); ++it) {
        const QString& k = it.key();
        const int s1 = k.indexOf('/');
        QString pfx;
        if (s1 < 0) pfx = k;
        else { const int s2 = k.indexOf('/', s1 + 1); pfx = (s2 < 0) ? k.left(s1) : k.left(s2); }
        byPrefix[pfx] += 1;
    }
    QList<QPair<QString, int>> prefixes;
    for (auto it = byPrefix.constBegin(); it != byPrefix.constEnd(); ++it)
        prefixes.append({it.key(), it.value()});
    std::sort(prefixes.begin(), prefixes.end(),
              [](const QPair<QString, int>& a, const QPair<QString, int>& b) { return a.second > b.second; });

    QString out;
    out += QStringLiteral("Diablo4AssetBrowser — CASC coverage report\n");
    out += QStringLiteral("build (vfs-root ekey): %1\n").arg(m_buildId);
    out += QStringLiteral("raw index entries (EKeys): %1\n").arg(m_index.size());
    out += QStringLiteral("resolved TVFS paths (names): %1\n\n").arg(m_root.size());

    out += QStringLiteral("Paths by prefix (top 40):\n");
    for (int i = 0; i < prefixes.size() && i < 40; ++i)
        out += QStringLiteral("  %1  %2\n").arg(prefixes[i].second, 10).arg(prefixes[i].first);
    out += QLatin1Char('\n');

    out += QStringLiteral("Nested container manifests expanded: %1\n").arg(m_covExpanded.size());
    for (const QString& s : m_covExpanded) out += QStringLiteral("  + %1\n").arg(s);
    out += QLatin1Char('\n');

    out += QStringLiteral("Containers SKIPPED — encrypted, missing TACT key: %1\n").arg(m_covEncrypted.size());
    if (!m_covEncrypted.isEmpty())
        out += QStringLiteral("  (obtain these key ids, add them to the TACT keys folder, then File > Reload)\n");
    for (const QString& s : m_covEncrypted) out += QStringLiteral("  ! %1\n").arg(s);
    out += QLatin1Char('\n');

    out += QStringLiteral("Containers SKIPPED — oversized (probe skipped to avoid OOM): %1\n").arg(m_covOversized.size());
    for (const QString& s : m_covOversized) out += QStringLiteral("  ~ %1\n").arg(s);
    out += QLatin1Char('\n');

    out += QStringLiteral("Locale text/speech/cutscene/video packs: %1.\n")
               .arg(QSettings().value(QStringLiteral("casc/includeLocalePacks"), false).toBool()
                        ? QStringLiteral("INCLUDED (casc/includeLocalePacks on)")
                        : QStringLiteral("excluded by default — enable \"Include locale packs\" in "
                                         "Settings for the full ~2.6M name set (slower indexing)"));

    // data\, not beside the exe. This is written on EVERY cold TVFS build — i.e. after every game
    // patch — so it is the one stray write a normal user would actually hit, and it contradicts
    // the portability claim the README, the Settings help and the smoke test all make.
    const QString path = AppPaths::file(QStringLiteral("casc_coverage.txt"));
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write(out.toUtf8());
        f.close();
        qInfo("CASC: coverage report written to %s", qPrintable(path));
    }
}

// ── TVFS path-table disk cache ───────────────────────────────────────────────────────────────
// Compact custom binary (NOT QDataStream's QString/QHash encoding — UTF-16 strings would double
// the file): magic · sig · count · per entry [u16 utf8-path-len, bytes, u8 nEKeys, per ekey
// u8 len + bytes]. ~50 MB for 1.1M paths, loads in a fraction of the expansion time.
static constexpr quint32 kRootCacheMagic = 0x54564653;   // 'TVFS'

QString CascReader::buildAndKeySignature() const
{
    // buildId (vfs-root hash — changes every patch) + a digest of the TACT key NAMES — a new
    // key can expand containers, and can name assets, that a cache built without it lacks.
    QMutexLocker kl(&m_keysMutex);
    QByteArrayList names;
    names.reserve(m_tactKeys.size());
    for (auto it = m_tactKeys.constBegin(); it != m_tactKeys.constEnd(); ++it)
        names << it.key().toHex();
    std::sort(names.begin(), names.end());
    QCryptographicHash h(QCryptographicHash::Md5);
    for (const QByteArray& n : names) h.addData(n);
    return m_buildId + QLatin1Char(':') + QString::fromLatin1(h.result().toHex());
}

// Same signature, kept as the TVFS path table's own name for readability at its call site.
QString CascReader::rootCacheSignature() const { return buildAndKeySignature(); }

bool CascReader::loadRootCache(const QString& path, const QString& sig)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray all = f.readAll();
    const char* p = all.constData();
    const char* end = p + all.size();
    auto need = [&](qint64 n) { return end - p >= n; };
    if (!need(8)) return false;
    if (qFromLittleEndian<quint32>(p) != kRootCacheMagic) return false;
    const quint32 sigLen = qFromLittleEndian<quint32>(p + 4);
    p += 8;
    if (!need(sigLen)) return false;
    if (QString::fromUtf8(p, int(sigLen)) != sig) return false;   // stale build / key set
    p += sigLen;
    if (!need(4)) return false;
    const quint32 count = qFromLittleEndian<quint32>(p);
    p += 4;
    QHash<QString, QVector<QByteArray>> root;
    root.reserve(int(count));
    for (quint32 i = 0; i < count; ++i) {
        if (!need(3)) return false;
        const quint16 pl = qFromLittleEndian<quint16>(p); p += 2;
        if (!need(pl + 1)) return false;
        QString key = QString::fromUtf8(p, pl); p += pl;
        const quint8 n = quint8(*p++);
        QVector<QByteArray> eks;
        eks.reserve(n);
        for (quint8 k = 0; k < n; ++k) {
            if (!need(1)) return false;
            const quint8 el = quint8(*p++);
            if (!need(el)) return false;
            eks.append(QByteArray(p, el)); p += el;
        }
        root.insert(std::move(key), std::move(eks));
    }
    m_root = std::move(root);
    return !m_root.isEmpty();
}

void CascReader::saveRootCache(const QString& path, const QString& sig) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QString part = path + QStringLiteral(".part");
    QFile f(part);
    if (!f.open(QIODevice::WriteOnly)) return;
    QByteArray out;
    out.reserve(64 * 1024 * 1024);
    auto put32 = [&](quint32 v) { quint32 le = qToLittleEndian(v); out.append(reinterpret_cast<const char*>(&le), 4); };
    auto put16 = [&](quint16 v) { quint16 le = qToLittleEndian(v); out.append(reinterpret_cast<const char*>(&le), 2); };
    put32(kRootCacheMagic);
    const QByteArray sigU = sig.toUtf8();
    put32(quint32(sigU.size()));
    out.append(sigU);
    put32(quint32(m_root.size()));
    for (auto it = m_root.constBegin(); it != m_root.constEnd(); ++it) {
        const QByteArray k = it.key().toUtf8();
        if (k.size() > 0xFFFF || it.value().size() > 0xFF) continue;   // absurd entry — skip
        put16(quint16(k.size()));
        out.append(k);
        out.append(char(quint8(it.value().size())));
        for (const QByteArray& ek : it.value()) {
            out.append(char(quint8(ek.size())));
            out.append(ek);
        }
    }
    f.write(out);
    f.close();
    QFile::remove(path);
    QFile::rename(part, path);
    qInfo("CASC: TVFS path table cached (%lld MB) — next launch skips the expansion",
          qint64(out.size()) / (1024 * 1024));
}

bool CascReader::open(const QString& gameDir, const QString& product)
{
    QMutexLocker lock(&m_mutex);
    m_ready = false;
    m_index.clear();
    m_keyProbe.clear();
    m_root.clear();
    m_buildId.clear();
    m_product.clear();
    // Same reset close() does — open() does not route through it, so anything added to one has to
    // be added to the other or a re-open silently inherits the previous install's data.
    m_encSnos.clear();
    m_encSnosBuilt = false;
    m_sharedPay.clear();
    m_sharedPayBuilt = false;
    {
        QMutexLocker cl(&m_archiveMutex);
        m_archivePath.clear();
    }
    m_gameVersion.clear();
    m_gameDir = gameDir;
    m_wantProduct = product;   // consulted by locateConfig → firstActiveBuildInfo
    if (gameDir.isEmpty()) { m_lastError = "No game directory configured."; return false; }

    // Record which build row actually won, so the log states the product and version rather than
    // echoing back the SETTING (which may not match what was opened — that mismatch is precisely
    // what makes a wrong install hard to spot when running retail and PTR side by side).
    {
        QHash<QString, QString> row;
        firstActiveBuildInfo(gameDir, row, product);
        m_product     = row.value(QStringLiteral("Product")).trimmed();
        m_gameVersion = row.value(QStringLiteral("Version")).trimmed();
        if (!m_product.isEmpty()) {
            qInfo("CASC: .build.info product=%s version=%s%s", qPrintable(m_product),
                  qPrintable(m_gameVersion),
                  (!product.isEmpty()
                   && m_product.compare(product, Qt::CaseInsensitive) != 0)
                      ? "  [NOTE: differs from the configured product — that row was not present]"
                      : "");
        } else {
            // Say so LOUDLY, and say what was actually there. An empty product is not cosmetic:
            // it is the lineage key that keeps retail and PTR "Latest" baselines apart, so a blank
            // one silently puts both installs back in the same slot — the exact overwrite this was
            // written to prevent. Printing the column names turns "why is it empty" into a
            // one-glance answer instead of another build cycle.
            QStringList cols = row.keys();
            cols.sort();
            qInfo("CASC: .build.info carries no Product column for %s (columns: %s) — "
                  "identifying this install by folder name instead: %s",
                  qPrintable(QDir(gameDir).dirName()),
                  cols.isEmpty() ? "none; file missing, unreadable, or no Active row"
                                 : qPrintable(cols.join(QStringLiteral(", "))),
                  qPrintable(lineageKey()));
        }
    }

    const QHash<QString, QString> cfg = locateConfig();
    qInfo("CASC: build config keys=%d hasVfsRoot=%d", int(cfg.size()), cfg.contains("vfs-root") ? 1 : 0);
    if (cfg.isEmpty()) { m_lastError = "No build config found."; return false; }
    if (!loadAllIndices()) { m_lastError = "CASC index empty."; return false; }
    qInfo("CASC: index entries=%d", int(m_index.size()));

    const QString rootRaw = cfg.value("vfs-root");
    const QStringList parts = rootRaw.split(' ', Qt::SkipEmptyParts);
    const QString rootEKey = parts.size() >= 2 ? parts[1] : (parts.isEmpty() ? QString() : parts[0]);
    qInfo("CASC: vfs-root ekey=%s", qPrintable(rootEKey.left(18)));
    if (rootEKey.isEmpty()) { m_lastError = "vfs-root missing in build config."; return false; }
    m_buildId = rootEKey;   // per-build fingerprint (changes every game patch)

    // ── Path-table cache: the expanded TVFS table costs ~5 s to rebuild (parse + nested BLTE
    // expansion over a million paths) yet is a pure function of the vfs-root manifest (whose
    // hash IS m_buildId) and the TACT key set (keys gate which encrypted containers expand).
    // Cache it on disk keyed by both; a game patch or a new key rebuilds automatically.
    // Version in the filename — see util/CacheVersioning.h.
    const QString cachePath = AppPaths::dataDir() + QStringLiteral("/tvfs_paths_v1.bin");
    const QString cacheSig = rootCacheSignature();
    QElapsedTimer tvfsT; tvfsT.start();
    if (loadRootCache(cachePath, cacheSig)) {
        qInfo("CASC: TVFS path table from cache — %d paths in %lld ms (nested expand skipped; "
              "delete tvfs_paths_v1.bin to force a fresh expansion + coverage report)",
              int(m_root.size()), tvfsT.elapsed());
    } else {
        const QByteArray rootBytes = readByEKeyHex(rootEKey);
        qInfo("CASC: root manifest bytes=%lld", qint64(rootBytes.size()));
        if (rootBytes.isEmpty()) { m_lastError = "VFS root EKey not in index."; return false; }
        parseTvfs(rootBytes, QString(), m_root);
        qInfo("CASC: root TVFS paths=%d (before nested expand)", int(m_root.size()));
        expandNestedManifests();
        qInfo("CASC: total TVFS paths=%d (after nested expand, %lld ms)",
              int(m_root.size()), tvfsT.elapsed());
        writeCoverageReport();   // casc_coverage.txt: per-prefix breakdown + skipped containers
        saveRootCache(cachePath, cacheSig);
    }

    // Sample a few resolved paths to confirm the numeric base/payload/<sno> scheme.
    int shown = 0;
    for (auto it = m_root.constBegin(); it != m_root.constEnd() && shown < 6; ++it, ++shown)
        qInfo("CASC: sample path: %s", qPrintable(it.key()));

    m_ready = !m_root.isEmpty();
    if (!m_ready) m_lastError = "TVFS root parsed to 0 paths.";
    return m_ready;
}

QByteArray CascReader::readFile(const QString& name)
{
    // Resolve the index entries under the lock, but do the raw archive read + BLTE inflate
    // OUTSIDE it: readArchive() opens its own QFile per call and blteDecode is pure CPU on
    // local buffers (the TACT-key lookup takes its own tiny m_keysMutex), so concurrent bulk
    // workers genuinely read+decompress in parallel instead of taking turns on m_mutex.
    QVector<IndexEntry> entries;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_ready) return {};
        const QVector<QByteArray> eks = m_root.value(name.toLower());
        if (eks.isEmpty()) return {};
        entries.reserve(eks.size());
        for (const QByteArray& ek : eks) {
            auto it = m_index.constFind(ek);
            if (it != m_index.constEnd()) entries.append(it.value());
        }
    }
    for (const IndexEntry& e : entries) {
        const QByteArray raw = readArchive(e);
        if (!raw.isEmpty()) return raw;
    }
    return {};
}

QByteArray CascReader::readPayloadBySno(quint64 sno)
{
    return readPayloadBySno(sno, 0);
}

QByteArray CascReader::readPayloadBySno(quint64 sno, int depth)
{
    const QByteArray d = readFile(QStringLiteral("base/payload/%1").arg(sno));
    if (!d.isEmpty()) return d;

    // Fall back to the low-detail payload ONLY when the full-detail one genuinely is not in this
    // install. readFile returns empty for BOTH "no such path" and "the read failed", and the
    // difference matters enormously here.
    //
    // If base/payload EXISTS but came back empty, the read failed — for encrypted content, a TACT
    // key we do not hold or a bad decrypt. Serving base/paylow in its place hands the caller a
    // perfectly VALID BLOB THAT DOES NOT DESCRIBE THE SAME MESH: the meta still carries the full
    // model's vertex/index counts and offsets, so the parser reads them out of a smaller buffer and
    // tries to allocate the difference. That is precisely the "bad allocation during parse" fault
    // in model_render_crashes.log, and every entry in it is an encrypted piece
    // (druF_stor251_TRS and friends).
    //
    // Returning empty instead means the piece reports as undecodable, which it honestly is.
    if (payloadVariants(sno).payload > 0) {
        static QMutex warnMutex;
        static QSet<quint64> warned;
        bool first = false;
        { QMutexLocker wl(&warnMutex); first = !warned.contains(sno); if (first) warned.insert(sno); }
        if (first) {
            const QByteArray kn = tactKeyFor(sno);
            qWarning("payload sno %llu: base/payload exists but could not be read%s — NOT falling "
                     "back to base/paylow, whose geometry does not match this asset's meta and has "
                     "faulted the parser before",
                     sno,
                     kn.isEmpty() ? "" : (haveTactKey(kn) ? " (encrypted; we hold the key, so the "
                                                            "decrypt itself failed)"
                                                          : " (encrypted with a TACT key we do not "
                                                            "hold)"));
        }
        return {};
    }

    // No payload of its own. Before giving up, ask the game where it actually lives: D4
    // deduplicates hard, and 36,930 assets deliberately carry no payload because they share
    // another sno's. Checked BEFORE paylow, because this is the authoritative full-detail source
    // while paylow is only a low-detail variant.
    if (depth < 4) {
        const QHash<int, int>& shared = sharedPayloads();
        const auto it = shared.constFind(int(sno));
        if (it != shared.constEnd() && quint64(it.value()) != sno) {
            const QByteArray via = readPayloadBySno(quint64(it.value()), depth + 1);
            if (!via.isEmpty()) return via;
        }
    }
    return readFile(QStringLiteral("base/paylow/%1").arg(sno));
}

QByteArray CascReader::readMetaBySno(quint64 sno)
{
    return readFile(QStringLiteral("base/meta/%1").arg(sno));
}

QStringList CascReader::rootPathsWithPrefix(const QString& prefix)
{
    QMutexLocker lock(&m_mutex);
    QStringList out;
    const QString p = prefix.toLower();
    for (auto it = m_root.constBegin(); it != m_root.constEnd(); ++it)
        if (it.key().startsWith(p)) out << it.key();
    out.sort();
    return out;
}

int CascReader::dumpAllRootPaths(const QString& outPath)
{
    QMutexLocker lock(&m_mutex);
    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return -1;
    QTextStream ts(&f);
    QStringList keys = m_root.keys();
    keys.sort();
    for (const QString& k : keys) {
        quint64 bytes = 0;
        int chunks = 0;
        for (const QByteArray& ek : m_root.value(k)) {
            auto e = m_index.constFind(ek);
            if (e != m_index.constEnd()) { bytes += e.value().size; ++chunks; }
        }
        ts << k << '\t' << bytes << '\t' << chunks << '\n';
    }
    return int(keys.size());
}

QStringList CascReader::rootPrefixCensus()
{
    QMutexLocker lock(&m_mutex);
    QHash<QString, int> counts;
    QHash<QString, QString> example;
    for (auto it = m_root.constBegin(); it != m_root.constEnd(); ++it) {
        const QString& k = it.key();
        const int slash = k.lastIndexOf(QLatin1Char('/'));
        // Group by prefix, but only collapse the trailing segment when it is a bare number (a sno).
        // Collapsing named leaves too would hide exactly the by-NAME namespace we are looking for.
        QString pfx = k;
        if (slash > 0) {
            bool numeric = slash + 1 < k.size();
            for (int i = slash + 1; i < k.size() && numeric; ++i)
                numeric = k[i].isDigit();
            if (numeric) pfx = k.left(slash) + QLatin1String("/<sno>");
            else pfx = k.left(slash) + QLatin1String("/<name>");
        }
        ++counts[pfx];
        if (!example.contains(pfx)) example.insert(pfx, k);
    }
    QStringList out;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
        out << QStringLiteral("%1  x%2   e.g. %3").arg(it.key(), -44).arg(it.value(), 7)
                   .arg(example.value(it.key()));
    out.sort();
    return out;
}

QStringList CascReader::rootPathsFor(quint64 sno)
{
    QMutexLocker lock(&m_mutex);
    QStringList out;
    if (!m_ready) return out;
    const QString suffix = QLatin1Char('/') + QString::number(sno);
    for (auto it = m_root.constBegin(); it != m_root.constEnd(); ++it) {
        if (!it.key().endsWith(suffix)) continue;
        quint64 bytes = 0;
        for (const QByteArray& ek : it.value()) {
            auto e = m_index.constFind(ek);
            if (e != m_index.constEnd()) bytes += e.value().size;
        }
        out << QStringLiteral("%1 (%2 B stored)").arg(it.key()).arg(bytes);
    }
    out.sort();
    return out;
}

CascReader::PayloadVariants CascReader::payloadVariants(quint64 sno)
{
    QMutexLocker lock(&m_mutex);
    PayloadVariants v;
    if (!m_ready) return v;
    quint64* slot[2] = { &v.payload, &v.paylow };
    const QString paths[2] = { QStringLiteral("base/payload/%1").arg(sno),
                               QStringLiteral("base/paylow/%1").arg(sno) };
    for (int i = 0; i < 2; ++i) {
        for (const QByteArray& ek : m_root.value(paths[i].toLower())) {
            auto it = m_index.constFind(ek);
            if (it != m_index.constEnd()) *slot[i] += it.value().size;
        }
    }
    return v;
}

QByteArray CascReader::tactKeyFor(quint64 sno)
{
    QMutexLocker lock(&m_mutex);
    if (!m_ready) return {};
    const auto cached = m_keyProbe.constFind(sno);
    if (cached != m_keyProbe.constEnd()) return cached.value();
    auto remember = [this, sno](const QByteArray& kn) { m_keyProbe.insert(sno, kn); return kn; };
    for (const QString& path : {QStringLiteral("base/payload/%1").arg(sno),
                                QStringLiteral("base/paylow/%1").arg(sno),
                                QStringLiteral("base/meta/%1").arg(sno)}) {
        const QVector<QByteArray> eks = m_root.value(path.toLower());
        for (const QByteArray& ek : eks) {
            auto it = m_index.constFind(ek);
            if (it == m_index.constEnd()) continue;
            const QByteArray kn = frameKeyName(it.value());
            if (!kn.isEmpty()) return remember(kn);
        }
    }
    return remember(QByteArray());
}

const QHash<int, QByteArray>& CascReader::encryptedSnos()
{
    // The read MUST happen with no lock held: readFile takes m_mutex itself, and QMutex is not
    // recursive, so building this under the lock would deadlock on the first call.
    {
        QMutexLocker lock(&m_mutex);
        if (m_encSnosBuilt || !m_ready) return m_encSnos;
    }
    const QByteArray blob = readFile(QStringLiteral("base/EncryptedSNOs.dat"));

    QMutexLocker lock(&m_mutex);
    if (m_encSnosBuilt) return m_encSnos;   // another thread got there first
    m_encSnosBuilt = true;                  // set before any early return: a malformed manifest
                                            // must not be re-read on every call
    if (blob.size() < 8) {
        qWarning("CASC: base/EncryptedSNOs.dat missing or empty — the \"only encrypted\" filters "
                 "have nothing to filter on and will come back empty");
        return m_encSnos;
    }
    // Layout, from d4data's parse.js:194-205 and verified against the shipped file:
    //   u32 (version/magic, unread), u32 count, count x { i32 group, i32 sno, u8 keyName[8] }
    // Note the 16-byte stride — the EncryptedNameDict tables use 8 and are a different file.
    const uchar* d = reinterpret_cast<const uchar*>(blob.constData());
    const qint64 count = qint64(qFromLittleEndian<quint32>(d + 4));
    if (count <= 0 || 8 + count * 16 > blob.size()) {
        qWarning("CASC: base/EncryptedSNOs.dat declares %lld entries but is only %lld bytes — "
                 "ignoring it", count, qint64(blob.size()));
        return m_encSnos;
    }
    m_encSnos.reserve(int(count));
    for (qint64 i = 0; i < count; ++i) {
        const uchar* r = d + 8 + i * 16;
        const int sno = int(qFromLittleEndian<qint32>(r + 4));
        m_encSnos.insert(sno, QByteArray(reinterpret_cast<const char*>(r + 8), 8));
    }
    QSet<QByteArray> distinct;
    for (auto it = m_encSnos.constBegin(); it != m_encSnos.constEnd(); ++it) distinct.insert(it.value());
    qInfo("CASC: EncryptedSNOs — %d encrypted asset(s) under %d key(s)",
          m_encSnos.size(), distinct.size());
    return m_encSnos;
}

const QHash<int, int>& CascReader::sharedPayloads()
{
    // Same lock discipline as encryptedSnos: readFile takes m_mutex, so the read happens unlocked.
    {
        QMutexLocker lock(&m_mutex);
        if (m_sharedPayBuilt || !m_ready) return m_sharedPay;
    }
    const QByteArray blob = readFile(QStringLiteral("base/CoreTOCSharedPayloadsMapping.dat"));

    QMutexLocker lock(&m_mutex);
    if (m_sharedPayBuilt) return m_sharedPay;
    m_sharedPayBuilt = true;
    if (blob.size() < 8) {
        qWarning("CASC: base/CoreTOCSharedPayloadsMapping.dat missing — assets that share another "
                 "asset's payload will read as having no data at all");
        return m_sharedPay;
    }
    const uchar* d = reinterpret_cast<const uchar*>(blob.constData());
    const qint64 count = qint64(qFromLittleEndian<quint32>(d + 4));
    if (count <= 0 || 8 + count * 8 > blob.size()) {
        qWarning("CASC: CoreTOCSharedPayloadsMapping declares %lld entries but is only %lld bytes — "
                 "ignoring it", count, qint64(blob.size()));
        return m_sharedPay;
    }
    m_sharedPay.reserve(int(count));
    for (qint64 i = 0; i < count; ++i) {
        const uchar* r = d + 8 + i * 8;
        const int src = int(qFromLittleEndian<quint32>(r));
        const int dst = int(qFromLittleEndian<quint32>(r + 4));
        if (src > 0 && dst > 0 && src != dst) m_sharedPay.insert(src, dst);
    }
    qInfo("CASC: shared payloads — %d asset(s) read their data from another sno", m_sharedPay.size());
    return m_sharedPay;
}

quint64 CascReader::payloadSourceSno(quint64 sno)
{
    if (payloadVariants(sno).payload > 0) return sno;   // has its own — no redirect involved
    const QHash<int, int>& shared = sharedPayloads();
    quint64 cur = sno;
    for (int hop = 0; hop < 4; ++hop) {                 // same depth bound readPayloadBySno uses
        const auto it = shared.constFind(int(cur));
        if (it == shared.constEnd() || quint64(it.value()) == cur) break;
        cur = quint64(it.value());
        if (payloadVariants(cur).payload > 0) break;
    }
    return cur;
}

bool CascReader::haveTactKey(const QByteArray& keyName) const
{
    if (keyName.isEmpty()) return true;   // unencrypted needs no key
    QMutexLocker kl(&m_keysMutex);
    return !m_tactKeys.value(keyName).isEmpty();
}

QStringList CascReader::verifyTactKeys(const QString& keyFile)
{
    QStringList out;
    if (!m_ready) { out << QStringLiteral("CASC not open."); return out; }

    // Which dict files this build actually ships, by their hex id.
    QSet<QString> dictIds;
    for (const QString& p : rootPathsWithPrefix(QStringLiteral("base/encryptednamedict-"))) {
        const qsizetype at = p.lastIndexOf(QLatin1String("-0x"));
        if (at >= 0) dictIds.insert(p.mid(at + 3, 16).toLower());
    }
    out << QStringLiteral("build ships %1 EncryptedNameDict file(s)").arg(dictIds.size());

    const int before = tactKeyCount();
    // ADD the candidates, do not REPLACE. applyTactKeys() begins with m_tactKeys.clear() — it is the
    // "load the configured key file" entry point — so calling it here silently threw away every key
    // the user has configured for the rest of the session. Everything encrypted that decoded a
    // moment earlier would stop, and the summary line below read as additive while the held count
    // went DOWN. loadKeysFromFile is the same parser without the clear; both mutexes are held here
    // because that is the contract writers observe (decryptFrame readers take only m_keysMutex).
    int added = 0;
    {
        QMutexLocker lock(&m_mutex);
        QMutexLocker keysLock(&m_keysMutex);
        added = loadKeysFromFile(keyFile);
    }
    out << QStringLiteral("candidate file: %1 key(s) parsed (held %2 -> %3)")
               .arg(added).arg(before).arg(tactKeyCount());

    int valid = 0, noDict = 0, badDecode = 0;
    for (const QByteArray& kn : tactKeyNames()) {
        QByteArray rev = kn;
        std::reverse(rev.begin(), rev.end());
        // The filename hex is the key name BYTE-SWAPPED, but both orders are tried rather than
        // trusting that convention across a patch.
        QString path, id;
        for (const QByteArray& cand : {rev, kn}) {
            const QString h = QString::fromLatin1(cand.toHex()).toLower();
            if (dictIds.contains(h)) { id = h; path = QStringLiteral("base/encryptednamedict-0x%1.dat").arg(h); break; }
        }
        const QString keyHex = QString::fromLatin1(kn.toHex()).toUpper();
        if (path.isEmpty()) { ++noDict;
            out << QStringLiteral("  %1  — no dict in this build (not a D4 key, or unused here)").arg(keyHex);
            continue; }
        const QByteArray blob = readFile(path);
        const bool ok = blob.size() >= 8
                     && qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(blob.constData()))
                            == 0xABCD4567u;
        if (!ok) { ++badDecode;
            out << QStringLiteral("  %1  — dict present but did NOT decode (wrong key value)").arg(keyHex);
            continue; }
        ++valid;
        const int n = int(qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(blob.constData()) + 4));
        out << QStringLiteral("  %1  ✓ VALID — names %2 SNO(s)").arg(keyHex).arg(n);
    }
    out << QStringLiteral("RESULT: %1 valid · %2 with no dict here · %3 present-but-wrong  "
                          "(coverage %1/%4 dicts)")
               .arg(valid).arg(noDict).arg(badDecode).arg(dictIds.size());
    for (const QString& l : out) qInfo().noquote() << "tact-verify:" << l;
    return out;
}

QVector<QByteArray> CascReader::tactKeyNames() const
{
    QMutexLocker kl(&m_keysMutex);
    QVector<QByteArray> out;
    out.reserve(m_tactKeys.size());
    for (auto it = m_tactKeys.constBegin(); it != m_tactKeys.constEnd(); ++it)
        out.append(it.key());
    return out;
}

quint64 CascReader::payloadSize(quint64 sno)
{
    QMutexLocker lock(&m_mutex);
    if (!m_ready) return 0;
    for (const QString& path : {QStringLiteral("base/payload/%1").arg(sno),
                                QStringLiteral("base/paylow/%1").arg(sno)}) {
        const QVector<QByteArray> eks = m_root.value(path.toLower());
        if (eks.isEmpty()) continue;
        quint64 total = 0; bool ok = false;
        for (const QByteArray& ek : eks) {
            auto it = m_index.constFind(ek);
            if (it != m_index.constEnd()) { total += it.value().size; ok = true; }
        }
        if (ok) return total;
    }
    return 0;
}

// (fileSize removed — no callers.)

int CascReader::enumerate(const QString& mask, const std::function<bool(const Entry&)>& fn)
{
    QMutexLocker lock(&m_mutex);
    if (!m_ready) return 0;
    const QString needle = (mask.isEmpty() || mask == "*") ? QString()
                                                           : mask.toLower().remove('*');
    int count = 0;
    for (auto it = m_root.constBegin(); it != m_root.constEnd(); ++it) {
        if (!needle.isEmpty() && !it.key().contains(needle)) continue;
        Entry e;
        e.name = it.key();
        e.available = true;
        if (!it.value().isEmpty()) {
            auto ie = m_index.constFind(it.value().first());
            if (ie != m_index.constEnd()) e.size = ie.value().size;
        }
        ++count;
        if (!fn(e)) break;
    }
    return count;
}

// Parse one key file into m_tactKeys. Accepts "KEYID KEYVALUE" separated by
// space/tab/';'/',' per line; '#' comments and short/garbage lines are skipped.
// Caller holds m_mutex. Returns the number of keys added.
int CascReader::loadKeysFromFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return 0;

    // TWO formats, because the two upstreams publish differently and folder mode should accept a
    // file dropped in as-downloaded rather than making people convert it by hand:
    //
    //   wowdev / rustydemon   KEYNAME(16 hex) <sep> KEY(32 hex)
    //   CascLib KeyService.cs [0xKEYNAME] = "KEY".FromHexString(),   // TactKeyId N
    //
    // CascLib's file is a C# source carrying Overwatch, WoW AND Diablo IV keys in one table. When
    // its "Diablo IV" section marker is present we take ONLY that section: the other ~600 keys can
    // never match a D4 dict, and loading them would make tactKeyCount() and the verify report
    // meaningless. No marker (a hand-trimmed file) falls back to parsing the whole thing.
    static const QRegularExpression kCsLine(
        QStringLiteral("^\\s*\\[0x([0-9A-Fa-f]{16})\\]\\s*=\\s*\"([0-9A-Fa-f]{32})\""));
    static const QRegularExpression kSep(QStringLiteral("[\\s;,]+"));

    const QByteArray all = f.readAll();
    const bool isCs = all.contains("FromHexString");
    int sectionFrom = 0, sectionTo = all.size();
    if (isCs) {
        const int d4 = all.indexOf("Diablo IV");
        if (d4 >= 0) {
            sectionFrom = all.indexOf('\n', d4) + 1;
            // The section ends at the next comment header that is NOT a per-key "// TactKeyId N"
            // trailer. Scanning forward line-by-line is clearer than a regex over the whole blob.
            int p = sectionFrom;
            while (p < all.size()) {
                const int nl = all.indexOf('\n', p);
                const int end = nl < 0 ? all.size() : nl;
                const QByteArray ln = all.mid(p, end - p).trimmed();
                if (ln.startsWith("//") || ln.startsWith("};")) { sectionTo = p; break; }
                if (nl < 0) break;
                p = nl + 1;
            }
        }
    }

    int added = 0;
    const QList<QByteArray> lines = all.mid(sectionFrom, sectionTo - sectionFrom).split('\n');
    for (const QByteArray& raw : lines) {
        QString line = QString::fromUtf8(raw);
        if (isCs) {
            const auto m = kCsLine.match(line);
            if (!m.hasMatch()) continue;
            const QByteArray name = QByteArray::fromHex(m.captured(1).toLatin1());
            const QByteArray key  = QByteArray::fromHex(m.captured(2).toLatin1());
            if (name.size() == 8 && key.size() == 16) { m_tactKeys.insert(name, key); ++added; }
            continue;
        }
        const int hash = line.indexOf(QLatin1Char('#'));
        if (hash >= 0) line.truncate(hash);
        const QStringList parts = line.simplified().split(kSep, Qt::SkipEmptyParts);
        if (parts.size() < 2 || parts[0].size() != 16 || parts[1].size() != 32) continue;
        const QByteArray name = QByteArray::fromHex(parts[0].toLatin1());
        const QByteArray key = QByteArray::fromHex(parts[1].toLatin1());
        if (name.size() == 8 && key.size() == 16) { m_tactKeys.insert(name, key); ++added; }
    }
    return added;
}

int CascReader::applyTactKeys(const QString& keysPath)
{
    QMutexLocker lock(&m_mutex);
    QMutexLocker keysLock(&m_keysMutex);   // writers hold both; decryptFrame readers hold only this
    m_tactKeys.clear();
    if (keysPath.isEmpty()) return 0;

    const QFileInfo fi(keysPath);
    int added = 0;
    if (fi.isDir()) {
        // Folder mode: load every key file in the directory, so keys can be
        // dropped in / updated / removed without re-pointing at a single file.
        // *.keys included: upstream (rustydemon) reorganised its key list to keys/d4.keys, and a
        // folder-mode setup pointed at a directory containing it would silently load nothing —
        // no error, just fewer decryptable assets, which is the hardest kind of failure to notice.
        const QStringList files = QDir(keysPath).entryList(
            {QStringLiteral("*.txt"), QStringLiteral("*.csv"), QStringLiteral("*.keys")},
            QDir::Files, QDir::Name);
        for (const QString& fn : files)
            added += loadKeysFromFile(QDir(keysPath).filePath(fn));
    } else {
        added = loadKeysFromFile(keysPath);
    }
    return added;
}
