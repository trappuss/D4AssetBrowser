#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace GifEncoder {
// Encode animated GIF89a. Each frame is tightly-packed RGBA (4 bytes/pixel, row-major,
// width*height*4 bytes). All frames must be width x height. delayCs = per-frame delay in
// centiseconds (1/100 s, e.g. 4 => 25fps). loop=true => infinite loop (NETSCAPE2.0 ext).
// Returns true on success. Must be robust: handle up to ~600x600 and ~120 frames.
//
// transparentAlphaThreshold: <0 (default) => fully opaque, alpha ignored (byte-for-byte
// identical to the legacy behavior). 0..255 => enable 1-bit transparency: any pixel whose
// alpha byte is < threshold is treated as transparent. Exactly one palette entry is reserved
// as the transparent color index (opaque colors use up to 255 entries, quantized from opaque
// pixels only). Each frame's GCE sets the Transparent Color Flag and disposal method 2
// (restore to background) so transparent holes don't smear between frames.
// maxColors caps the quantized palette (2..256). Fewer colors → smaller files (and a coarser
// palette). Default 256 = the original full-palette behavior.
bool encode(const std::string& path,
            const std::vector<std::vector<uint8_t>>& framesRGBA,
            int width, int height, int delayCs, bool loop,
            int transparentAlphaThreshold = -1,
            int maxColors = 256,
            // Ordered (Bayer 8x8) dithering. The pattern depends ONLY on pixel position, so it is
            // identical in every frame — it breaks up palette banding WITHOUT introducing
            // frame-to-frame noise. (Error-diffusion dithering would look better on a still image
            // but changes with the content, which shimmers in animation.) Without it, smooth shaded
            // surfaces band, and on a moving garment those band edges crawl across the surface —
            // reads as jitter even when the simulation is perfectly deterministic.
            bool dither = true);

// Same encode, into memory. The size-budget path re-encodes with different palettes/scales until
// the result fits, so it needs to MEASURE a candidate without touching the disk — and then write
// only the one it keeps.
bool encodeToBuffer(std::vector<uint8_t>& out,
                    const std::vector<std::vector<uint8_t>>& framesRGBA,
                    int width, int height, int delayCs, bool loop,
                    int transparentAlphaThreshold = -1,
                    int maxColors = 256,
                    bool dither = true);

bool writeBuffer(const std::string& path, const std::vector<uint8_t>& bytes);
}
