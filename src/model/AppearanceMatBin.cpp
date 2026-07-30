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

QVector<AppearanceMatBin::Entry> AppearanceMatBin::read(const QByteArray& meta)
{
    QVector<Entry> out;
    if (meta.size() < kDescOff + 8) return out;
    const int dataOff = int(u32(meta, kDescOff));
    const int bytes   = int(u32(meta, kDescOff + 4));
    if (dataOff <= 0 || bytes <= 0 || bytes % kRecSize != 0) return out;
    const int n = bytes / kRecSize;
    if (n > kMaxRecords) return out;
    const int base = dataOff + kRecBias;
    if (base + n * kRecSize > meta.size()) return out;

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
            if (v != 0 && v != kNil) { e.sno = int(v); break; }
        }
        out.push_back(e);
    }
    return out;
}

QVector<int> AppearanceMatBin::snos(const QByteArray& meta)
{
    const QVector<Entry> e = read(meta);
    QVector<int> out;
    out.reserve(e.size());
    for (const Entry& x : e) out.push_back(x.sno);
    return out;
}
