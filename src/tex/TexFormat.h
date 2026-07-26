#pragma once
#include <QString>
#include <QtGlobal>

// eTexFormat → BC codec / OpenGL compressed internal format, plus the D3D12
// 256-byte row-pitch alignment used to upload the top mip. Ported (and verified)
// from the Python fork's texture_decoder.py + gl_texture.py.
//
//   9,41 → BC4 (8 bpb)   42 → BC5 (16)   10,46,47 → BC1 (8)
//   49   → BC3 (16, may be BC1 — resolved by payload size)   50 → BC7 (16)
namespace TexFormat {

struct Codec {
    int     bytesPerBlock = 0;     // 8 for BC1/BC4, 16 for BC3/BC5/BC7
    quint32 glInternalFormat = 0;  // GL_COMPRESSED_* enum
    QString name;
    bool    valid = false;
};

// GL compressed internal-format constants (avoid pulling a GL header here).
constexpr quint32 GL_BC1 = 0x83F1;  // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
constexpr quint32 GL_BC3 = 0x83F3;  // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
constexpr quint32 GL_BC4 = 0x8DBB;  // GL_COMPRESSED_RED_RGTC1
constexpr quint32 GL_BC5 = 0x8DBC;  // GL_COMPRESSED_RG_RGTC2
constexpr quint32 GL_BC7 = 0x8E8C;  // GL_COMPRESSED_RGBA_BPTC_UNORM

// Resolve an eTexFormat. For fmt 49, pass payloadSize/width/height so BC3↔BC1 can
// be disambiguated by the top-mip byte count (0 payloadSize keeps the BC3 default).
Codec codec(int eTexFormat, qint64 payloadSize = 0, int width = 0, int height = 0);

QString name(int eTexFormat);

// GPU-aligned block-row width in pixels (D3D12 256-byte row pitch).
int alignedWidth(int width, int bytesPerBlock);

// Byte size of mip 0 at the aligned width — exactly what glCompressedTexImage2D
// must be given as imageSize (the payload also holds the smaller mips).
qint64 mip0Size(int alignedW, int height, int bytesPerBlock);

}  // namespace TexFormat
