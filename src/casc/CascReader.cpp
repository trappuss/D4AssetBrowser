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
#include <QtEndian>
#include <QtGlobal>

#include <lz4.h>
#include <lz4frame.h>
#include <zlib.h>

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

QString firstActiveBuildInfo(const QString& gameDir, QHash<QString, QString>& row)
{
    QFile f(gameDir + "/.build.info");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
    if (lines.size() < 2) return {};
    QStringList headers;
    for (const QString& h : lines[0].split('|')) headers << h.split('!').first();
    for (int i = 1; i < lines.size(); ++i) {
        const QStringList vals = lines[i].split('|');
        if (vals.size() < headers.size()) continue;
        QHash<QString, QString> r;
        for (int k = 0; k < headers.size(); ++k) r.insert(headers[k], vals[k]);
        if (r.value("Active") == "1") { row = r; return r.value("Build Key").trimmed().toLower(); }
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

QString CascReader::gameVersion(const QString& gameDir)
{
    QHash<QString, QString> row;
    firstActiveBuildInfo(gameDir, row);
    return row.value(QStringLiteral("Version")).trimmed();
}

void CascReader::close()
{
    QMutexLocker lock(&m_mutex);
    m_ready = false;
    m_index.clear();
    m_keyProbe.clear();
    m_root.clear();
    m_gameDir.clear();
}

QHash<QString, QString> CascReader::locateConfig()
{
    // Steam: Data/.build.config
    QFile steam(m_gameDir + "/Data/.build.config");
    if (steam.open(QIODevice::ReadOnly | QIODevice::Text))
        return parseKvFile(QString::fromUtf8(steam.readAll()));

    // Battle.net: .build.info → Build Key → Data/config/xx/yy/<key>
    QHash<QString, QString> info;
    const QString buildKey = firstActiveBuildInfo(m_gameDir, info);
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
    const QStringList dirs{m_gameDir + "/Data/" + group, m_gameDir + "/Data/data", m_gameDir + "/Data"};
    for (const QString& d : dirs) {
        const QStringList cands{
            QStringLiteral("%1/data.%2").arg(d).arg(archiveNum, 5, 10, QLatin1Char('0')),
            QStringLiteral("%1/data.%2").arg(d).arg(archiveNum, 3, 10, QLatin1Char('0')),
            QStringLiteral("%1/%2").arg(d).arg(archiveNum, 4, 10, QLatin1Char('0')),
            QStringLiteral("%1/%2").arg(d).arg(archiveNum, 8, 10, QLatin1Char('0'))};
        for (const QString& p : cands)
            if (QFileInfo::exists(p)) return p;
    }
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

QByteArray CascReader::decryptFrame(const QByteArray& data, int blockIndex) const
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
    char bi[4]; qToLittleEndian(quint32(blockIndex), reinterpret_cast<uchar*>(bi));
    nonce.append(bi, 4);
    nonce = nonce.left(8);
    while (nonce.size() < 8) nonce.append('\0');
    const QByteArray ks = salsa20Keystream(key, nonce, enc.size());
    QByteArray dec(enc.size(), '\0');
    for (int i = 0; i < enc.size(); ++i) dec[i] = enc[i] ^ ks[i];
    return dec;
}

QByteArray CascReader::decompressFrame(quint8 type, const QByteArray& data, int blockIndex) const
{
    switch (type) {
    case 0x4E: return data;                       // 'N' raw
    case 0x5A: return zlibInflate(data);          // 'Z' zlib
    case 0x45: {                                  // 'E' encrypted (Salsa20)
        const QByteArray dec = decryptFrame(data, blockIndex);
        if (dec.isEmpty()) return {};
        return decompressFrame(quint8(dec[0]), dec.mid(1), blockIndex);
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
        const QByteArray dec = decompressFrame(quint8(frame[0]), frame.mid(1), bi);
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
    if (badFrames > 0)
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
QByteArray CascReader::frameKeyName(const IndexEntry& e) const
{
    const QString ap = findArchive(e.archive, e.group);
    if (ap.isEmpty()) return {};
    QFile f(ap);
    if (!f.open(QIODevice::ReadOnly)) return {};
    if (!f.seek(qint64(e.offset) + ARCHIVE_FRAME_HEADER)) return {};
    const QByteArray head = f.read(qMin<quint32>(e.size, 8192u));
    if (!head.startsWith("BLTE") || head.size() < 9) return {};
    const quint32 headerSize = rdBE32(head, 4);
    int frameStart;
    if (headerSize == 0) {
        frameStart = 8;
    } else {
        if (head.size() < 12) return {};
        frameStart = 12 + 24 * rdN_BE(head, 9, 3);
    }
    if (frameStart + 2 > head.size()) return {};
    if (quint8(head[frameStart]) != 0x45) return {};       // not 'E'-encrypted
    const int kp = frameStart + 1;
    const int keyLen = quint8(head[kp]);
    if (kp + 1 + keyLen > head.size()) return {};
    QByteArray keyName = head.mid(kp + 1, keyLen);
    std::reverse(keyName.begin(), keyName.end());          // stored little-endian
    return keyName;
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

    const QString path = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("casc_coverage.txt"));
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

QString CascReader::rootCacheSignature() const
{
    // buildId (vfs-root hash — changes every patch) + a digest of the TACT key NAMES — a new
    // key can expand containers the cached table lacks, so it must invalidate the cache.
    QByteArrayList names;
    names.reserve(m_tactKeys.size());
    for (auto it = m_tactKeys.constBegin(); it != m_tactKeys.constEnd(); ++it)
        names << it.key().toHex();
    std::sort(names.begin(), names.end());
    QCryptographicHash h(QCryptographicHash::Md5);
    for (const QByteArray& n : names) h.addData(n);
    return m_buildId + QLatin1Char(':') + QString::fromLatin1(h.result().toHex());
}

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

bool CascReader::open(const QString& gameDir, const QString& /*product*/)
{
    QMutexLocker lock(&m_mutex);
    m_ready = false;
    m_index.clear();
    m_keyProbe.clear();
    m_root.clear();
    m_buildId.clear();
    m_gameDir = gameDir;
    if (gameDir.isEmpty()) { m_lastError = "No game directory configured."; return false; }

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
    QByteArray d = readFile(QStringLiteral("base/payload/%1").arg(sno));
    if (d.isEmpty()) d = readFile(QStringLiteral("base/paylow/%1").arg(sno));
    return d;
}

QByteArray CascReader::readMetaBySno(quint64 sno)
{
    return readFile(QStringLiteral("base/meta/%1").arg(sno));
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

bool CascReader::haveTactKey(const QByteArray& keyName) const
{
    if (keyName.isEmpty()) return true;   // unencrypted needs no key
    QMutexLocker kl(&m_keysMutex);
    return !m_tactKeys.value(keyName).isEmpty();
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
    int added = 0;
    while (!f.atEnd()) {
        QString line = QString::fromUtf8(f.readLine());
        const int hash = line.indexOf(QLatin1Char('#'));
        if (hash >= 0) line.truncate(hash);
        static const QRegularExpression sep(QStringLiteral("[\\s;,]+"));
        const QStringList parts = line.simplified().split(sep, Qt::SkipEmptyParts);
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
        const QStringList files = QDir(keysPath).entryList(
            {QStringLiteral("*.txt"), QStringLiteral("*.csv")}, QDir::Files, QDir::Name);
        for (const QString& fn : files)
            added += loadKeysFromFile(QDir(keysPath).filePath(fn));
    } else {
        added = loadKeysFromFile(keysPath);
    }
    return added;
}
