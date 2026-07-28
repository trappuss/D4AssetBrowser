#include "GifEncoder.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <functional>
#include <thread>

namespace GifEncoder {

namespace {

// ---------------------------------------------------------------------------
// Median-cut color quantization
// ---------------------------------------------------------------------------

struct RGB {
    uint8_t r, g, b;
};

struct Box {
    std::vector<uint32_t>* pixels; // shared pool
    size_t begin;                  // [begin,end) into the pool
    size_t end;
    uint8_t rmin, rmax, gmin, gmax, bmin, bmax;
};

static inline uint8_t chan(uint32_t px, int c) {
    return static_cast<uint8_t>((px >> (c * 8)) & 0xFF);
}

// Compute the color bounds of a box.
static void shrinkBox(Box& box) {
    uint8_t rmin = 255, rmax = 0, gmin = 255, gmax = 0, bmin = 255, bmax = 0;
    const std::vector<uint32_t>& pool = *box.pixels;
    for (size_t i = box.begin; i < box.end; ++i) {
        uint32_t px = pool[i];
        uint8_t r = chan(px, 0), g = chan(px, 1), b = chan(px, 2);
        rmin = std::min(rmin, r); rmax = std::max(rmax, r);
        gmin = std::min(gmin, g); gmax = std::max(gmax, g);
        bmin = std::min(bmin, b); bmax = std::max(bmax, b);
    }
    box.rmin = rmin; box.rmax = rmax;
    box.gmin = gmin; box.gmax = gmax;
    box.bmin = bmin; box.bmax = bmax;
}

// Build a palette of up to maxColors entries from the sampled pixels.
// Pixels are packed as 0x00BBGGRR (r in low byte).
static std::vector<RGB> medianCut(std::vector<uint32_t>& pool, int maxColors) {
    std::vector<RGB> palette;
    if (pool.empty()) {
        palette.push_back({0, 0, 0});
        return palette;
    }

    std::vector<Box> boxes;
    Box first{&pool, 0, pool.size(), 0, 0, 0, 0, 0, 0};
    shrinkBox(first);
    boxes.push_back(first);

    while (static_cast<int>(boxes.size()) < maxColors) {
        // Find the box with the largest single-channel extent and >1 pixel.
        int bestIdx = -1;
        int bestExtent = -1;
        for (size_t i = 0; i < boxes.size(); ++i) {
            const Box& b = boxes[i];
            if (b.end - b.begin < 2) continue;
            int er = b.rmax - b.rmin;
            int eg = b.gmax - b.gmin;
            int eb = b.bmax - b.bmin;
            int e = std::max({er, eg, eb});
            if (e > bestExtent) { bestExtent = e; bestIdx = static_cast<int>(i); }
        }
        if (bestIdx < 0) break; // all boxes have a single unique pixel

        Box box = boxes[bestIdx];
        int er = box.rmax - box.rmin;
        int eg = box.gmax - box.gmin;
        int eb = box.bmax - box.bmin;
        int channel = 0;
        if (eg >= er && eg >= eb) channel = 1;
        else if (eb >= er && eb >= eg) channel = 2;
        else channel = 0;

        // Sort the box's pixel range by the chosen channel.
        std::sort(pool.begin() + box.begin, pool.begin() + box.end,
                  [channel](uint32_t a, uint32_t c) {
                      return chan(a, channel) < chan(c, channel);
                  });

        size_t mid = box.begin + (box.end - box.begin) / 2;
        // Ensure both halves are non-empty (guaranteed since size>=2).

        Box left{&pool, box.begin, mid, 0, 0, 0, 0, 0, 0};
        Box right{&pool, mid, box.end, 0, 0, 0, 0, 0, 0};
        shrinkBox(left);
        shrinkBox(right);

        boxes[bestIdx] = left;
        boxes.push_back(right);
    }

    // Average each box to produce a palette color.
    palette.reserve(boxes.size());
    for (const Box& b : boxes) {
        uint64_t sr = 0, sg = 0, sb = 0;
        size_t n = b.end - b.begin;
        for (size_t i = b.begin; i < b.end; ++i) {
            uint32_t px = pool[i];
            sr += chan(px, 0);
            sg += chan(px, 1);
            sb += chan(px, 2);
        }
        if (n == 0) { palette.push_back({0, 0, 0}); continue; }
        palette.push_back({static_cast<uint8_t>(sr / n),
                           static_cast<uint8_t>(sg / n),
                           static_cast<uint8_t>(sb / n)});
    }
    return palette;
}

static inline int nearestColor(const std::vector<RGB>& pal, int r, int g, int b) {
    int best = 0;
    long bestDist = 1L << 30;
    for (size_t i = 0; i < pal.size(); ++i) {
        int dr = r - pal[i].r;
        int dg = g - pal[i].g;
        int db = b - pal[i].b;
        long d = static_cast<long>(dr) * dr + static_cast<long>(dg) * dg +
                 static_cast<long>(db) * db;
        if (d < bestDist) { bestDist = d; best = static_cast<int>(i); if (d == 0) break; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Bit / byte output with 255-byte sub-block chunking (LSB-first packing)
// ---------------------------------------------------------------------------

class BlockWriter {
public:
    explicit BlockWriter(std::vector<uint8_t>& out) : out_(out) {}

    void writeBits(uint32_t code, int nbits) {
        bitBuffer_ |= (static_cast<uint32_t>(code) << bitCount_);
        bitCount_ += nbits;
        while (bitCount_ >= 8) {
            pushByte(static_cast<uint8_t>(bitBuffer_ & 0xFF));
            bitBuffer_ >>= 8;
            bitCount_ -= 8;
        }
    }

    void flush() {
        if (bitCount_ > 0) {
            pushByte(static_cast<uint8_t>(bitBuffer_ & 0xFF));
            bitBuffer_ = 0;
            bitCount_ = 0;
        }
        // Emit any remaining partial sub-block.
        if (!chunk_.empty()) {
            out_.push_back(static_cast<uint8_t>(chunk_.size()));
            out_.insert(out_.end(), chunk_.begin(), chunk_.end());
            chunk_.clear();
        }
        // Block terminator.
        out_.push_back(0x00);
    }

private:
    void pushByte(uint8_t byte) {
        chunk_.push_back(byte);
        if (chunk_.size() == 255) {
            out_.push_back(255);
            out_.insert(out_.end(), chunk_.begin(), chunk_.end());
            chunk_.clear();
        }
    }

    std::vector<uint8_t>& out_;
    std::vector<uint8_t> chunk_;
    uint32_t bitBuffer_ = 0;
    int bitCount_ = 0;
};

// ---------------------------------------------------------------------------
// LZW compression (GIF variant)
// ---------------------------------------------------------------------------

// Reusable LZW dictionary. The table is 4096*256 entries; clearing it with std::fill costs a
// 4 MB memset, and the encoder did that once per frame PLUS once per mid-frame table reset — on a
// 600x600x120 export that is hundreds of megabytes of pure memset and it dominated encode time.
// An epoch stamp makes "clear" a single increment: an entry counts only when its stamp matches the
// current epoch. Identical output, no clearing.
struct LzwScratch {
    static constexpr int kMaxCode = 4096;
    std::vector<uint32_t> stamp;   // epoch an entry was written in
    std::vector<int>      code;    // the code itself
    uint32_t              epoch = 0;

    void begin() {
        if (stamp.empty()) { stamp.assign(size_t(kMaxCode) * 256, 0); code.resize(size_t(kMaxCode) * 256); }
        clear();
    }
    void clear() {
        if (++epoch == 0) {   // wrapped after 4 billion resets — the only time a real wipe is due
            std::fill(stamp.begin(), stamp.end(), 0);
            epoch = 1;
        }
    }
};

static void lzwEncode(std::vector<uint8_t>& out,
                      const std::vector<uint8_t>& indices,
                      int minCodeSize,
                      LzwScratch& scratch) {
    const int clearCode = 1 << minCodeSize;
    const int endCode = clearCode + 1;

    out.push_back(static_cast<uint8_t>(minCodeSize));

    BlockWriter bw(out);

    const int kMaxCode = LzwScratch::kMaxCode;
    scratch.begin();

    int codeSize = minCodeSize + 1;
    int nextCode = endCode + 1;

    auto resetDict = [&]() {
        scratch.clear();
        codeSize = minCodeSize + 1;
        nextCode = endCode + 1;
    };

    bw.writeBits(clearCode, codeSize);

    if (indices.empty()) {
        bw.writeBits(endCode, codeSize);
        bw.flush();
        return;
    }

    int prefix = indices[0];
    for (size_t i = 1; i < indices.size(); ++i) {
        int k = indices[i];
        int idx = prefix * 256 + k;
        int found = (scratch.stamp[idx] == scratch.epoch) ? scratch.code[idx] : -1;
        if (found != -1) {
            prefix = found;
        } else {
            bw.writeBits(prefix, codeSize);
            if (nextCode < kMaxCode) {
                scratch.stamp[idx] = scratch.epoch;
                scratch.code[idx]  = nextCode;
                ++nextCode;
                // Increase code size when we've filled the current range.
                if (nextCode > (1 << codeSize) && codeSize < 12) {
                    ++codeSize;
                }
            } else {
                // Table full: emit clear and reset.
                bw.writeBits(clearCode, codeSize);
                resetDict();
            }
            prefix = k;
        }
    }

    bw.writeBits(prefix, codeSize);
    bw.writeBits(endCode, codeSize);
    bw.flush();
}

// ---------------------------------------------------------------------------
// Little-endian helpers
// ---------------------------------------------------------------------------

static void putU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

} // namespace

bool encodeToBuffer(std::vector<uint8_t>& out,
                    const std::vector<std::vector<uint8_t>>& framesRGBA,
                    int width, int height, int delayCs, bool loop,
                    int transparentAlphaThreshold, int maxColors, bool dither) {
    out.clear();
    if (width <= 0 || height <= 0 || framesRGBA.empty()) return false;

    const size_t expected = static_cast<size_t>(width) * height * 4;
    for (const auto& f : framesRGBA) {
        if (f.size() != expected) return false;
    }
    if (delayCs < 0) delayCs = 0;
    if (maxColors < 2)   maxColors = 2;
    if (maxColors > 256) maxColors = 256;

    // Transparency mode: a pixel is transparent when its alpha < threshold.
    const bool useTransparency = transparentAlphaThreshold >= 0;

    // ── Inter-frame differencing ──────────────────────────────────────────────────────────────
    // Every frame was stored whole, so a 120-frame loop of a character who moves an arm re-encoded
    // the entire body 120 times. GIF allows a frame to be a SUB-RECTANGLE composited over what is
    // already on screen, and to mark individual pixels transparent so the previous frame shows
    // through. Restricting each frame to the region that changed, and carrying over the pixels that
    // did not, is lossless — the composed canvas is identical — and on a mostly-static animation it
    // is worth far more than any palette reduction.
    //
    // It needs a palette slot of its own for "carry over", and it needs disposal 1 (leave the
    // canvas in place). A 1-bit-alpha export cannot have either: its transparent index already
    // means "a hole in the model", and holes require disposal 2 (restore to background) or they
    // smear from frame to frame. So this applies to opaque exports only, which is also where GIFs
    // of this tool are largest.
    const bool interFrame = !useTransparency && framesRGBA.size() > 1;

    // Reserve exactly one palette slot for the transparent color when enabled, so
    // opaque colors get at most maxColors-1 entries and the transparent index is distinct.
    const int maxOpaqueColors = (useTransparency || interFrame) ? (maxColors - 1) : maxColors;

    // --- Build a shared global palette via median-cut on sampled pixels. ---
    // Sample ~32k pixels across all frames to bound quantization cost. When
    // transparency is enabled, only OPAQUE pixels (alpha >= threshold) are sampled.
    const size_t totalPixels = static_cast<size_t>(width) * height * framesRGBA.size();
    const size_t targetSamples = 32768;
    size_t stride = totalPixels / targetSamples;
    if (stride < 1) stride = 1;

    std::vector<uint32_t> samples;
    samples.reserve(std::min(totalPixels, targetSamples) + 16);
    size_t counter = 0;
    for (const auto& frame : framesRGBA) {
        const uint8_t* p = frame.data();
        size_t pixCount = static_cast<size_t>(width) * height;
        for (size_t i = 0; i < pixCount; ++i) {
            bool transparent = useTransparency &&
                               (p[i * 4 + 3] < transparentAlphaThreshold);
            if (counter % stride == 0 && !transparent) {
                uint8_t r = p[i * 4 + 0];
                uint8_t g = p[i * 4 + 1];
                uint8_t b = p[i * 4 + 2];
                uint32_t packed = static_cast<uint32_t>(r) |
                                  (static_cast<uint32_t>(g) << 8) |
                                  (static_cast<uint32_t>(b) << 16);
                samples.push_back(packed);
            }
            ++counter;
        }
    }

    std::vector<RGB> palette = medianCut(samples, maxOpaqueColors);
    if (palette.empty()) palette.push_back({0, 0, 0});

    // Number of opaque palette entries actually produced by median-cut. Nearest-color
    // mapping must only consider these (never the reserved transparent slot).
    const int opaqueCount = static_cast<int>(palette.size());

    // The reserved slot: an actual hole for a 1-bit-alpha export, "carry the previous frame's pixel
    // through" for a differenced one. Either way its RGB is never displayed; use black.
    int transparentIndex = -1;
    if (useTransparency || interFrame) {
        transparentIndex = opaqueCount;
        palette.push_back({0, 0, 0});
    }

    // Determine global color table size: smallest power of two >= palette size,
    // minimum 2 entries. sizeExp: table has 2^(sizeExp+1) entries.
    int tableEntries = 1;
    int sizeExp = 0;
    while ((1 << (sizeExp + 1)) < static_cast<int>(palette.size())) ++sizeExp;
    tableEntries = 1 << (sizeExp + 1);
    // sizeExp in [0..7] -> table 2..256

    // Pad palette to tableEntries.
    while (static_cast<int>(palette.size()) < tableEntries) palette.push_back({0, 0, 0});

    // minCodeSize must be at least 2 for GIF LZW.
    int minCodeSize = std::max(2, sizeExp + 1);

    // --- Assemble the GIF byte stream. ---
    // Reserved for EVERY frame, not one: the old figure was a single frame's RGBA size halved, so a
    // 120-frame export grew the vector by repeated doubling and copied itself the whole way up.
    out.reserve(expected / 2 * framesRGBA.size() + 4096);

    // Header
    const char* hdr = "GIF89a";
    out.insert(out.end(), hdr, hdr + 6);

    // Logical Screen Descriptor
    putU16(out, static_cast<uint16_t>(width));
    putU16(out, static_cast<uint16_t>(height));
    // Packed: global color table flag (1), color resolution (7 -> 111),
    // sort flag (0), size of GCT (sizeExp).
    uint8_t packed = 0x80 | (0x70) | static_cast<uint8_t>(sizeExp & 0x07);
    out.push_back(packed);
    out.push_back(0x00); // background color index
    out.push_back(0x00); // pixel aspect ratio

    // Global Color Table
    for (int i = 0; i < tableEntries; ++i) {
        out.push_back(palette[i].r);
        out.push_back(palette[i].g);
        out.push_back(palette[i].b);
    }

    // NETSCAPE2.0 looping extension.
    if (loop) {
        out.push_back(0x21); // extension introducer
        out.push_back(0xFF); // application extension label
        out.push_back(0x0B); // block size (11)
        const char* app = "NETSCAPE2.0";
        out.insert(out.end(), app, app + 11);
        out.push_back(0x03); // sub-block size
        out.push_back(0x01); // sub-block id
        putU16(out, 0x0000); // loop count 0 = infinite
        out.push_back(0x00); // block terminator
    }

    // Nearest-color search must only consider the opaque palette entries, never the
    // reserved transparent slot (which has a cosmetic RGB and would corrupt matching).
    // A palette view limited to the opaque entries is used for the lookup.
    std::vector<RGB> opaquePalette(palette.begin(), palette.begin() + opaqueCount);

    // Ordered (Bayer 8x8) dither matrix, normalised to a signed -0.5..+0.5 bias. Position-only, so
    // every frame receives the IDENTICAL pattern — banding is broken up without any temporal noise.
    static const int kBayer8[64] = {
         0, 32,  8, 40,  2, 34, 10, 42,
        48, 16, 56, 24, 50, 18, 58, 26,
        12, 44,  4, 36, 14, 46,  6, 38,
        60, 28, 52, 20, 62, 30, 54, 22,
         3, 35, 11, 43,  1, 33,  9, 41,
        51, 19, 59, 27, 49, 17, 57, 25,
        15, 47,  7, 39, 13, 45,  5, 37,
        63, 31, 55, 23, 61, 29, 53, 21 };
    // Dither amplitude tracks how coarse the palette is: a 256-colour palette needs only a nudge,
    // a 32-colour one needs a real spread. Bounded so it can never look like noise.
    const int ditherAmp = dither
        ? std::max(2, std::min(24, 256 / std::max(1, opaqueCount) * 3))
        : 0;

    // Nearest-palette lookup for every 15-bit (5:5:5) colour, resolved ONCE up front rather than
    // memoised on demand. The table is 32768 entries and filling it costs one palette scan each;
    // in exchange every pixel becomes a single array read with no branch and no palette walk. The
    // old lazy cache degenerated exactly where it mattered: dithering biases each pixel by its
    // Bayer offset, so one source colour hits up to 64 different keys and most of the table ends
    // up populated anyway — but one miss at a time, interleaved with the encode.
    std::vector<uint8_t> lut(32768);
    for (int key = 0; key < 32768; ++key) {
        // Reconstruct the bucket CENTRE (+4), not its floor: quantising to the floor biased every
        // lookup toward darker palette entries by half a bucket.
        const int r = std::min(255, ((key >> 10) & 31) * 8 + 4);
        const int g = std::min(255, ((key >>  5) & 31) * 8 + 4);
        const int b = std::min(255, ( key        & 31) * 8 + 4);
        lut[key] = static_cast<uint8_t>(nearestColor(opaquePalette, r, g, b));
    }

    const size_t pixCount = static_cast<size_t>(width) * height;
    const size_t nFrames  = framesRGBA.size();

    // Frames are independent — the palette is global and read-only, and each frame's LZW stream is
    // self-contained (its own clear code, its own dictionary). Both passes below run in parallel
    // and the blocks are stitched in order; the output is bit-identical to doing it serially.
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    // Each worker keeps an 8 MB LZW scratch, so the thread count is capped rather than taking
    // every core on a many-core machine for what is a memory-bound job anyway.
    const unsigned nThreads = std::max(1u, std::min<unsigned>({hw, 8u, unsigned(nFrames)}));

    auto parallelFor = [&](size_t n, const std::function<void(size_t, size_t)>& body) {
        if (nThreads <= 1 || n <= 1) { body(0, n); return; }
        std::vector<std::thread> pool;
        pool.reserve(nThreads);
        const size_t chunk = (n + nThreads - 1) / nThreads;
        for (unsigned t = 0; t < nThreads; ++t) {
            const size_t from = size_t(t) * chunk;
            if (from >= n) break;
            pool.emplace_back(body, from, std::min(n, from + chunk));
        }
        for (std::thread& th : pool) th.join();
    };

    // ── Pass 1: quantise every frame to palette indices. ──
    // Differencing compares INDICES, not source colours: two RGB values that quantise to the same
    // entry are the same pixel in the output, so comparing indices finds strictly more carry-over
    // than comparing the source would — and is what actually has to match for this to be lossless.
    std::vector<std::vector<uint8_t>> idxFrames(nFrames);
    parallelFor(nFrames, [&](size_t from, size_t to) {
        for (size_t fi = from; fi < to; ++fi) {
            idxFrames[fi].resize(pixCount);
            uint8_t* dst = idxFrames[fi].data();
            const uint8_t* p = framesRGBA[fi].data();
            for (size_t i = 0; i < pixCount; ++i) {
                if (useTransparency && (p[i * 4 + 3] < transparentAlphaThreshold)) {
                    dst[i] = static_cast<uint8_t>(transparentIndex);
                    continue;
                }
                int r = p[i * 4 + 0], g = p[i * 4 + 1], b = p[i * 4 + 2];
                if (ditherAmp > 0) {
                    // x,y drive the ordered pattern (position-only ⇒ stable across frames).
                    const int x = static_cast<int>(i % static_cast<size_t>(width));
                    const int y = static_cast<int>(i / static_cast<size_t>(width));
                    const int d = ((kBayer8[((y & 7) << 3) | (x & 7)] * 2) - 63) * ditherAmp / 63;
                    r = std::min(255, std::max(0, r + d));
                    g = std::min(255, std::max(0, g + d));
                    b = std::min(255, std::max(0, b + d));
                }
                dst[i] = lut[((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)];
            }
        }
    });

    // ── Pass 2: per-frame sub-rectangle + LZW. ──
    // With disposal 1 and carry-over, the canvas after frame i is exactly frame i's full index
    // buffer: every pixel was either written by frame i or left holding the value frame i-1 put
    // there, which the carry-over rule only allows when the two agree. So frame i diffs against
    // frame i-1 directly and there is no sequential dependency — this pass parallelises too.
    struct Rect { int x = 0, y = 0, w = 0, h = 0; };
    std::vector<Rect> rects(nFrames);
    std::vector<char> useCarry(nFrames, 0);   // per frame: did it actually use carry-over?
    std::vector<std::vector<uint8_t>> lzwBlocks(nFrames);

    // Restricting a frame to its bounding box is unconditionally good — strictly fewer pixels to
    // compress. Substituting the UNCHANGED pixels inside that box for the carry-over index is not:
    // it pays off when they form coherent regions and costs when they are scattered, because an
    // unrelated index dropped into a run is exactly what LZW cannot absorb. Measured on a sequence
    // where every pixel changes every frame, 44% of pixels still matched by coincidence and
    // carrying them over made the file 12.7% LARGER than not differencing at all.
    //
    // Neither the matching fraction nor the mean run length separates the two cases cleanly — the
    // pathological sequence measured 44% at a mean run of 4.2 against 56% at 10.0 for the case
    // where carry-over wins 15x, which is far too close to hang a constant on. So it is measured
    // rather than predicted: encode a few sample frames both ways and keep whichever is smaller.
    // Animations are homogeneous enough that the sample decides for the whole sequence, so this
    // costs a handful of extra LZW passes rather than one per frame.
    bool carryDecision = false;
    if (interFrame && nFrames > 1) {
        const size_t nSample = std::min<size_t>(4, nFrames - 1);
        std::vector<size_t> onBytes(nSample, 0), offBytes(nSample, 0);
        parallelFor(nSample, [&](size_t from, size_t to) {
            LzwScratch scratch;
            std::vector<uint8_t> probe;
            for (size_t k = from; k < to; ++k) {
                // Spread through the sequence: consecutive frames at the start would miss an
                // animation whose motion is concentrated elsewhere.
                const size_t fi = 1 + (nFrames - 1) * k / nSample;
                const uint8_t* cur = idxFrames[fi].data();
                const uint8_t* prv = idxFrames[fi - 1].data();
                for (int variant = 0; variant < 2; ++variant) {
                    probe.assign(cur, cur + pixCount);
                    if (variant == 0)
                        for (size_t i = 0; i < pixCount; ++i)
                            if (prv[i] == cur[i]) probe[i] = static_cast<uint8_t>(transparentIndex);
                    std::vector<uint8_t> tmp;
                    tmp.reserve(pixCount / 2 + 64);
                    lzwEncode(tmp, probe, minCodeSize, scratch);
                    (variant == 0 ? onBytes : offBytes)[k] = tmp.size();
                }
            }
        });
        size_t on = 0, off = 0;
        for (size_t k = 0; k < nSample; ++k) { on += onBytes[k]; off += offBytes[k]; }
        carryDecision = on < off;
    }

    parallelFor(nFrames, [&](size_t from, size_t to) {
        LzwScratch scratch;
        std::vector<uint8_t> sub;
        for (size_t fi = from; fi < to; ++fi) {
            const uint8_t* cur = idxFrames[fi].data();
            Rect r{0, 0, width, height};
            if (interFrame && fi > 0) {
                const uint8_t* prv = idxFrames[fi - 1].data();
                int x0 = width, y0 = height, x1 = -1, y1 = -1;
                for (int y = 0; y < height; ++y) {
                    const size_t row = size_t(y) * size_t(width);
                    for (int x = 0; x < width; ++x)
                        if (cur[row + size_t(x)] != prv[row + size_t(x)]) {
                            if (x < x0) x0 = x;
                            if (x > x1) x1 = x;
                            if (y < y0) y0 = y;
                            if (y > y1) y1 = y;
                        }
                }
                // Nothing moved: a 1x1 placeholder that only carries the frame's delay. A GIF frame
                // cannot have zero extent, and dropping it would shorten the animation.
                if (x1 < 0) r = Rect{0, 0, 1, 1};
                else        r = Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
            }
            rects[fi] = r;

            const uint8_t* prv = (interFrame && fi > 0) ? idxFrames[fi - 1].data() : nullptr;
            const size_t boxArea = size_t(r.w) * size_t(r.h);
            const bool carry = prv && carryDecision;
            useCarry[fi] = carry;

            sub.resize(boxArea);
            for (int y = 0; y < r.h; ++y) {
                const size_t srcRow = size_t(y + r.y) * size_t(width) + size_t(r.x);
                uint8_t* dstRow = sub.data() + size_t(y) * size_t(r.w);
                for (int x = 0; x < r.w; ++x) {
                    const uint8_t v = cur[srcRow + size_t(x)];
                    // Unchanged pixels become carry-over. That is what turns a sparse change into
                    // long transparent runs, which is where the compression is — the bounding box
                    // alone would still re-encode everything inside it.
                    dstRow[x] = (carry && prv[srcRow + size_t(x)] == v)
                                    ? static_cast<uint8_t>(transparentIndex) : v;
                }
            }
            lzwBlocks[fi].reserve(sub.size() / 2 + 64);
            lzwEncode(lzwBlocks[fi], sub, minCodeSize, scratch);
        }
    });

    // The reserved index means "hole" for an alpha export and "carry over" for a differenced one;
    // both set the transparent flag, but they need OPPOSITE disposals — restore-to-background so
    // holes do not smear, versus leave-in-place so there is something to carry over.
    const int disposal = useTransparency ? 2 : (interFrame ? 1 : 0);

    for (size_t fi = 0; fi < nFrames; ++fi) {
        // Per frame, because a differenced frame that opted out of carry-over writes every pixel in
        // its rectangle and must NOT declare an index transparent — the reserved entry is a real
        // colour to a decoder otherwise.
        const bool flagTransparent = useTransparency || useCarry[fi];
        // Graphic Control Extension
        out.push_back(0x21); // extension introducer
        out.push_back(0xF9); // graphic control label
        out.push_back(0x04); // block size
        out.push_back(static_cast<uint8_t>((disposal << 2) | (flagTransparent ? 0x01 : 0x00)));
        putU16(out, static_cast<uint16_t>(delayCs));
        out.push_back(flagTransparent ? static_cast<uint8_t>(transparentIndex)
                                      : static_cast<uint8_t>(0)); // transparent color index
        out.push_back(0x00); // block terminator

        // Image Descriptor
        const Rect& r = rects[fi];
        out.push_back(0x2C); // image separator
        putU16(out, static_cast<uint16_t>(r.x));
        putU16(out, static_cast<uint16_t>(r.y));
        putU16(out, static_cast<uint16_t>(r.w));
        putU16(out, static_cast<uint16_t>(r.h));
        out.push_back(0x00); // no local color table, not interlaced

        out.insert(out.end(), lzwBlocks[fi].begin(), lzwBlocks[fi].end());
    }

    // Trailer
    out.push_back(0x3B);
    return true;
}

bool encode(const std::string& path,
            const std::vector<std::vector<uint8_t>>& framesRGBA,
            int width, int height, int delayCs, bool loop,
            int transparentAlphaThreshold, int maxColors, bool dither) {
    std::vector<uint8_t> bytes;
    if (!encodeToBuffer(bytes, framesRGBA, width, height, delayCs, loop,
                        transparentAlphaThreshold, maxColors, dither))
        return false;
    return writeBuffer(path, bytes);
}

bool writeBuffer(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), fp);
    std::fclose(fp);
    return written == bytes.size();
}

} // namespace GifEncoder

