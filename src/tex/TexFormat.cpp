#include "tex/TexFormat.h"

#include <QHash>

namespace TexFormat {
namespace {

struct Base { int bpb; quint32 gl; const char* name; };

const QHash<int, Base>& table()
{
    static const QHash<int, Base> kT = {
        { 9, {  8, GL_BC4, "BC4_UNORM" }},
        {10, {  8, GL_BC1, "BC1_UNORM" }},
        {41, {  8, GL_BC4, "BC4_UNORM" }},
        {42, { 16, GL_BC5, "BC5_UNORM" }},
        {46, {  8, GL_BC1, "BC1_UNORM" }},
        {47, {  8, GL_BC1, "BC1_UNORM" }},
        {49, { 16, GL_BC3, "BC3_UNORM" }},   // may fall back to BC1 (see codec())
        {50, { 16, GL_BC7, "BC7_UNORM" }},
    };
    return kT;
}

inline int ceilDiv(int a, int b) { return (a + b - 1) / b; }

}  // namespace

Codec codec(int eTexFormat, qint64 payloadSize, int width, int height)
{
    auto it = table().constFind(eTexFormat);
    if (it == table().constEnd())
        return {};
    Base b = it.value();

    // fmt 49 packs either BC1 or BC3. Resolve by matching the payload against each codec's
    // 256-byte row-aligned mip0 size (the D3D12 placed-texture row pitch these ship in), NOT the
    // unpadded block count. Wide atlases (2DInventory_Items_*, width 488/976/1832…) pad the row,
    // so the old unpadded comparison never matched and they fell through to BC3 — then the decoder
    // needed 2× the bytes and bailed to a blank image. A BC1 payload is always smaller than a full
    // BC3 mip0 (16 bpb), so anything under that threshold is BC1.
    if (eTexFormat == 49 && payloadSize > 0 && width > 0 && height > 0) {
        const qint64 bc3mip0 = mip0Size(alignedWidth(width, 16), height, 16);
        if (payloadSize < bc3mip0)
            b = { 8, GL_BC1, "BC1_UNORM (fmt-49)" };
    }

    return Codec{ b.bpb, b.gl, QString::fromLatin1(b.name), true };
}

QString name(int eTexFormat)
{
    auto it = table().constFind(eTexFormat);
    return it != table().constEnd()
        ? QStringLiteral("%1 (%2)").arg(QString::fromLatin1(it.value().name)).arg(eTexFormat)
        : QStringLiteral("Unknown (%1)").arg(eTexFormat);
}

int alignedWidth(int width, int bytesPerBlock)
{
    if (bytesPerBlock <= 0)
        bytesPerBlock = 16;
    const int blockW   = ceilDiv(width, 4);
    const int rowBytes = blockW * bytesPerBlock;
    const int aligned  = ((rowBytes + 255) / 256) * 256;
    return (aligned / bytesPerBlock) * 4;
}

qint64 mip0Size(int alignedW, int height, int bytesPerBlock)
{
    return qint64(alignedW / 4) * ceilDiv(height, 4) * bytesPerBlock;
}

}  // namespace TexFormat
