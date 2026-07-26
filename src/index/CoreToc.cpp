#include "index/CoreToc.h"

namespace {
constexpr quint32 kNewMagic = 0xBCDE6611u;
constexpr int     kMaxName  = 256;

inline quint32 u32(const uchar* d, qint64 off)
{
    return quint32(d[off])
         | (quint32(d[off + 1]) << 8)
         | (quint32(d[off + 2]) << 16)
         | (quint32(d[off + 3]) << 24);
}
inline qint32 i32(const uchar* d, qint64 off) { return static_cast<qint32>(u32(d, off)); }
}

QHash<int, QVector<SnoEntry>> parseCoreToc(const QByteArray& data)
{
    QHash<int, QVector<SnoEntry>> out;
    const qint64 dlen = data.size();
    if (dlen < 8)
        return out;
    const uchar* d = reinterpret_cast<const uchar*>(data.constData());

    bool newFormat = false;
    qint64 tocOffset = 4;
    quint32 groupCount = u32(d, 0);
    if (groupCount == kNewMagic) {
        newFormat = true;
        tocOffset = 8;
        groupCount = u32(d, 4);
    }
    if (groupCount == 0 || groupCount > 4096)
        return out;

    const qint64 need = tocOffset + 16LL * groupCount;
    if (dlen < need)
        return out;

    const qint64 countsBase  = tocOffset;
    const qint64 offsetsBase = tocOffset + 4LL * groupCount;
    const qint64 dataStart   = (newFormat ? 12 : 8) + qint64(newFormat ? 16 : 12) * groupCount;

    for (quint32 c = 0; c < groupCount; ++c) {
        const quint32 count = u32(d, countsBase + 4LL * c);
        if (count == 0)
            continue;
        const qint64 base     = dataStart + u32(d, offsetsBase + 4LL * c);
        const qint64 nameBase = base + 12LL * count;
        if (base < 0 || nameBase > dlen)
            continue;

        for (quint32 i = 0; i < count; ++i) {
            const qint64 off = base + qint64(i) * 12;
            if (off + 12 > dlen)
                break;
            const qint32 snoGroup = i32(d, off);
            const qint32 snoId    = i32(d, off + 4);
            const qint32 nameRel  = i32(d, off + 8);

            const qint64 p = qint64(nameRel) + nameBase;
            if (p < 0 || p >= dlen)
                continue;

            // NUL-terminated name, capped at kMaxName. If no terminator falls in
            // range, skip the record (matches the reference parser).
            const qint64 limit = qMin<qint64>(p + kMaxName, dlen);
            qint64 end = p;
            while (end < limit && d[end] != 0)
                ++end;
            if (end >= limit)
                continue;

            const QString name = QString::fromUtf8(
                reinterpret_cast<const char*>(d + p), int(end - p)).trimmed();
            if (name.isEmpty())
                continue;

            out[snoGroup].append(SnoEntry{snoId, name});
        }
    }
    return out;
}
