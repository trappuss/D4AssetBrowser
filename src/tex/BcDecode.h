#pragma once
#include <QByteArray>
#include <QImage>

// CPU block-compression decoder → RGBA8888 QImage. Used to embed material
// textures into exported .glb files (glTF carries PNG/JPEG, not BC blocks).
//
// Handles BC1 / BC3 / BC4 / BC5 (the formats used by D4 base-colour, AO, rough,
// metal and normal maps). BC7 is not decoded here yet — decode() returns a null
// QImage for unsupported formats so callers fall back to a factor-only material.
//
// Block rows follow the same D3D12 256-byte row-pitch alignment as the GPU
// preview path (TexFormat::alignedWidth); the output is cropped to width×height.
namespace BcDecode {
QImage decode(const QByteArray& data, int width, int height, int eTexFormat);

// Fast self-check of the block decoders (BC7 partition/anchor tables + a BC4/BC1
// round-trip). Returns an empty string on success, or a description of the first
// failure. Cheap enough to run at startup; guards against silent table regressions
// like the 3-subset BC7 gap.
QString selfTest();
}
