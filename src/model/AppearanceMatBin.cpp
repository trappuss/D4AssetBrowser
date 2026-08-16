#include "model/AppearanceMatBin.h"

namespace {

constexpr int kDescOff   = 0xC8;   // fixed header slot holding (dataOffset, byteSize)
constexpr int kRecBias   = 16;     // first record sits dataOffset + 16 (measured, all samples)
constexpr int kRecSize   = 32;
constexpr int kRecSoaOff = 24;     // u32 soaOffset within a record
constexpr quint32 kNil   = 0xFFFFFFFFu;
// SOA field offsets, in the preference order appearanceRoster uses.
constexpr int kSoaOverride = 24, kSoaMaterial = 20, kSoaCloth = 28, kSoaHqCloth = 32;
// A sane ceiling: the largest observed is 29 materials, so 256 is generous while still rejecting a
// garbage byteSize before it turns into a huge loop.
constexpr int kMaxRecords = 256;

inline quint32 u32(const QByteArray& b, int off)
{
    if (off < 0 || off + 4 > b.size()) return 0;
    return quint32(uchar(b[off])) | quint32(uchar(b[off + 1])) << 8
         | quint32(uchar(b[off + 2])) << 16 | quint32(uchar(b[off + 3])) << 24;
}

}  // namespace

QVector<AppearanceMatBin::Entry> AppearanceMatBin::read(const QByteArray& meta, QString* why)
{
    QVector<Entry> out;
    auto bail = [&](const QString& reason) { if (why) *why = reason; return out; };
    if (meta.size() < kDescOff + 8)
        return bail(QStringLiteral("meta too small (%1 bytes)").arg(meta.size()));
    const int dataOff = int(u32(meta, kDescOff));
    const int bytes   = int(u32(meta, kDescOff + 4));
    const QString hdr = QStringLiteral("meta=%1 desc@0xC8 dataOff=%2 bytes=%3")
                            .arg(meta.size()).arg(dataOff).arg(bytes);
    if (dataOff <= 0 || bytes <= 0)
        return bail(hdr + QStringLiteral(" -> empty/absent material array"));
    if (bytes % kRecSize != 0)
        return bail(hdr + QStringLiteral(" -> byteSize not a multiple of %1").arg(kRecSize));
    const int n = bytes / kRecSize;
    if (n > kMaxRecords)
        return bail(hdr + QStringLiteral(" -> %1 records exceeds the %2 ceiling").arg(n).arg(kMaxRecords));
    const int base = dataOff + kRecBias;
    if (base + n * kRecSize > meta.size())
        return bail(hdr + QStringLiteral(" -> records run past EOF (base=%1 n=%2)").arg(base).arg(n));
    if (why) *why = hdr + QStringLiteral(" -> %1 record(s)").arg(n);

    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        const int rec = base + i * kRecSize;
        const int soa = int(u32(meta, rec + kRecSoaOff));
        Entry e;
        e.hash = u32(meta, rec);
        // Cloth-only entries are normal (necF_stor245_TRS record 0), so an unset snoMaterial is not
        // a failure — fall through the same order the JSON route prefers.
        for (int f : {kSoaOverride, kSoaMaterial, kSoaCloth, kSoaHqCloth}) {
            const quint32 v = u32(meta, soa + f);
            if (v != 0 && v != kNil) {
                e.sno = int(v);
                e.cloth = (f == kSoaCloth || f == kSoaHqCloth);
                break;
            }
        }
        out.push_back(e);
    }
    return out;
}

QVector<int> AppearanceMatBin::snos(const QByteArray& meta, QString* why)
{
    const QVector<Entry> e = read(meta, why);
    QVector<int> out;
    out.reserve(e.size());
    for (const Entry& x : e) out.push_back(x.sno);
    return out;
}
