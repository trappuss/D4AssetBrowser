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

// ── Rebuilding the implied Z of a two-channel (BC5) texture ─────────────────────────────────
// BC5 stores TWO channels. The third is not "zero", it is IMPLIED — D4's own shader rebuilds it,
// and so does this app's viewport (nz = sqrt(1 - dot(nxy, nxy))), which is why the preview has
// always looked right while the exported PNG did not. decode() writes B = 0 because that is the
// honest representation of "this codec carried no third channel", and every DCC then reads it as
// Z = 0 and lights the surface wrong.
//
// Painting B white by hand is the usual workaround and it is NOT equivalent. Z = 1 with the
// stored X/Y, once the DCC renormalises, flattens the relief: measured on barM_P00_BOD_normal,
// 4.2% of the tilt lost on average and 23.6% on the steepest 1% of texels. withNormalZ writes
// the real value instead.
//
// Gated on the CODEC, never on the file name. The one thing it cannot know is whether a given
// BC5 texture is a normal map or a packed two-channel mask; a mask would get a fabricated blue
// channel, which is why the caller keeps this behind a setting the user can turn off.
bool   isTwoChannel(int eTexFormat);
QImage withNormalZ(const QImage& img, int eTexFormat);   // returns img unchanged if not BC5
}
