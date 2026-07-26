#include "tex/BcDecode.h"
#include "tex/TexFormat.h"

#include <QDebug>
#include <QMutex>
#include <QSet>
#include <QString>
#include <array>
#include <cstring>

namespace {

struct RGBA { quint8 r, g, b, a; };

// Expand a 16-bit 565 colour to 8-bit RGB.
RGBA from565(quint16 c)
{
    const int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    return {quint8((r << 3) | (r >> 2)), quint8((g << 2) | (g >> 4)),
            quint8((b << 3) | (b >> 2)), 255};
}

quint16 rd16(const quint8* p) { return quint16(p[0]) | (quint16(p[1]) << 8); }

// Decode one BC1 colour block (8 bytes) into 16 RGBA texels (row-major 4x4).
// allowPunch: honour the c0<=c1 3-colour+transparent mode (true BC1).
void decodeColorBlock(const quint8* b, RGBA out[16], bool allowPunch)
{
    const quint16 c0 = rd16(b), c1 = rd16(b + 2);
    RGBA col[4];
    col[0] = from565(c0);
    col[1] = from565(c1);
    if (c0 > c1 || !allowPunch) {
        for (int k = 0; k < 3; ++k) {
            col[2].r = quint8((2 * col[0].r + col[1].r) / 3);
            col[2].g = quint8((2 * col[0].g + col[1].g) / 3);
            col[2].b = quint8((2 * col[0].b + col[1].b) / 3);
            col[3].r = quint8((col[0].r + 2 * col[1].r) / 3);
            col[3].g = quint8((col[0].g + 2 * col[1].g) / 3);
            col[3].b = quint8((col[0].b + 2 * col[1].b) / 3);
        }
        col[2].a = col[3].a = 255;
    } else {
        col[2].r = quint8((col[0].r + col[1].r) / 2);
        col[2].g = quint8((col[0].g + col[1].g) / 2);
        col[2].b = quint8((col[0].b + col[1].b) / 2);
        col[2].a = 255;
        col[3] = {0, 0, 0, 0};   // transparent black
    }
    quint32 bits = quint32(b[4]) | (quint32(b[5]) << 8) | (quint32(b[6]) << 16) | (quint32(b[7]) << 24);
    for (int i = 0; i < 16; ++i)
        out[i] = col[(bits >> (i * 2)) & 0x3];
}

// Decode one BC4/alpha block (8 bytes) into 16 channel values (row-major 4x4).
void decodeAlphaBlock(const quint8* b, quint8 out[16])
{
    const int a0 = b[0], a1 = b[1];
    int a[8];
    a[0] = a0; a[1] = a1;
    if (a0 > a1) {
        for (int k = 1; k <= 6; ++k) a[k + 1] = ((7 - k) * a0 + k * a1) / 7;
    } else {
        for (int k = 1; k <= 4; ++k) a[k + 1] = ((5 - k) * a0 + k * a1) / 5;
        a[6] = 0; a[7] = 255;
    }
    quint64 bits = 0;
    for (int i = 0; i < 6; ++i) bits |= quint64(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i)
        out[i] = quint8(a[(bits >> (i * 3)) & 0x7]);
}

// ── BC7 (port of the Python decoder, verified bit-exact vs Pillow) ──────────
// Covers modes 1/3/5/6/7 (verified on real format-50 payloads) plus single-
// subset 4. 3-subset modes 0/2 are rare and decode best-effort.
const quint16 kP2[64] = {
0xCCCC,0x8888,0xEEEE,0xECC8,0xC880,0xFEEC,0xFEC8,0xEC80,0xC800,0xFFEC,0xFE80,0xE800,0xFFE8,0xFF00,0xFFF0,0xF000,
0xF710,0x008E,0x7100,0x08CE,0x008C,0x7310,0x3100,0x8CCE,0x088C,0x3110,0x6666,0x366C,0x17E8,0x0FF0,0x718E,0x399C,
0xAAAA,0xF0F0,0x5A5A,0x33CC,0x3C3C,0x55AA,0x9696,0xA55A,0x73CE,0x13C8,0x324C,0x3BDC,0x6996,0xC33C,0x9966,0x0660,
0x0272,0x04E4,0x4E40,0x2720,0xC936,0x936C,0x39C6,0x639C,0x9336,0x9CC6,0x817E,0xE718,0xCCF0,0x0FCC,0x7744,0xEE22};
const quint8 kA2[64] = {
15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,2,8,2,2,8,8,15,2,8,2,2,8,8,2,2,
15,15,6,8,2,8,15,15,2,8,2,2,2,15,15,6,6,2,6,8,15,15,2,2,15,15,15,15,15,2,2,15};
// 3-subset partition table (modes 0 & 2): subset (0/1/2) for each of the 16 texels.
const quint8 kP3[64][16] = {
{0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2},{0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1},{0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1},{0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1},
{0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2},{0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2},{0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1},{0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1},
{0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2},{0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2},{0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2},{0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},
{0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2},{0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2},{0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2},{0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0},
{0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2},{0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0},{0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2},{0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1},
{0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2},{0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1},{0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2},{0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
{0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0},{0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2},{0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0},{0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1},
{0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2},{0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2},{0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1},{0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1},
{0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2},{0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1},{0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2},{0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},
{0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0},{0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0},{0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0},{0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1},
{0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1},{0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2},{0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1},{0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2},
{0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1},{0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1},{0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1},{0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
{0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2},{0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1},{0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2},{0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2},
{0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2},{0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2},{0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2},{0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2},
{0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2},{0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2},{0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2},{0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},
{0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1},{0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2},{0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2},{0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0}};
// Anchor texel index for the 2nd/3rd subset of a 3-subset block.
const quint8 kA3a[64] = {
3,3,15,15,8,3,15,15,8,8,6,6,6,5,3,3,3,3,8,15,3,3,6,10,5,8,8,6,8,5,15,15,
8,15,3,5,6,10,8,15,15,3,15,5,15,15,15,15,3,15,5,5,5,8,5,10,5,10,8,13,15,12,3,3};
const quint8 kA3b[64] = {
15,8,8,3,15,15,3,8,15,15,15,15,15,15,15,8,15,8,15,3,15,8,15,8,3,15,6,10,15,15,10,8,
15,3,15,10,10,8,9,10,6,15,8,15,3,6,6,8,15,3,15,15,15,15,15,15,15,15,15,15,3,15,15,8};
struct Bc7Mode { int NS,PB,RB,ISB,CB,AB,EPB,SPB,IB,IB2; };
const Bc7Mode kM[8] = {
{3,4,0,0,4,0,1,0,3,0},{2,6,0,0,6,0,0,1,3,0},{3,6,0,0,5,0,0,0,2,0},{2,6,0,0,7,0,1,0,2,0},
{1,0,2,1,5,6,0,0,2,3},{1,0,2,0,7,8,0,0,2,2},{1,0,0,0,7,7,1,0,4,0},{2,6,0,0,5,5,1,0,2,0}};
const int kW2[4]={0,21,43,64};
const int kW3[8]={0,9,18,27,37,46,55,64};
const int kW4[16]={0,4,9,13,17,21,26,30,34,38,43,47,51,55,60,64};
const int* weights(int n){ return n==2?kW2 : n==3?kW3 : kW4; }
inline int bc7unq(int v,int p){ return p>=8 ? v : ((v<<(8-p))|(v>>(2*p-8))); }

void decodeBC7Block(const quint8* blk, RGBA out[16])
{
    int pos = 0;
    auto get = [&](int n) -> int {
        int r = 0;
        for (int i = 0; i < n; ++i) { r |= ((blk[pos >> 3] >> (pos & 7)) & 1) << i; ++pos; }
        return r;
    };
    if (blk[0] == 0) { for (int i = 0; i < 16; ++i) out[i] = {0,0,0,255}; return; }
    int m = 0; while (!((blk[0] >> m) & 1)) ++m;
    get(m + 1);
    const Bc7Mode& M = kM[m];
    const int NS = M.NS, ne = NS * 2;
    const int part = M.PB ? get(M.PB) : 0;
    const int rot  = M.RB ? get(M.RB) : 0;
    const int isel = M.ISB ? get(M.ISB) : 0;

    int col[6][3] = {};
    for (int c = 0; c < 3; ++c) for (int e = 0; e < ne; ++e) col[e][c] = get(M.CB);
    int alpha[6] = {};
    if (M.AB) for (int e = 0; e < ne; ++e) alpha[e] = get(M.AB);
    int pb[6] = {};
    if (M.EPB) { for (int e = 0; e < ne; ++e) pb[e] = get(1); }
    else if (M.SPB) { int sp[3]={}; for (int s = 0; s < NS; ++s) sp[s]=get(1); for (int e=0;e<ne;++e) pb[e]=sp[e/2]; }

    int ep[6][4];
    const bool hasP = (M.EPB || M.SPB);
    const int cp = M.CB + (hasP ? 1 : 0);
    const int ap = M.AB ? (M.AB + (hasP ? 1 : 0)) : 0;
    for (int e = 0; e < ne; ++e) {
        int r=col[e][0], g=col[e][1], b=col[e][2], a=alpha[e];
        if (hasP) { r=(r<<1)|pb[e]; g=(g<<1)|pb[e]; b=(b<<1)|pb[e]; if (M.AB) a=(a<<1)|pb[e]; }
        ep[e][0]=bc7unq(r,cp); ep[e][1]=bc7unq(g,cp); ep[e][2]=bc7unq(b,cp);
        ep[e][3]=M.AB ? bc7unq(a,ap) : 255;
    }
    auto subOf = [&](int t){ return NS==2 ? ((kP2[part]>>t)&1) : (NS==3 ? int(kP3[part][t]) : 0); };
    auto anchorOf = [&](int sub){
        if (sub==0) return 0;
        if (NS==2) return int(kA2[part]);
        return sub==1 ? int(kA3a[part]) : int(kA3b[part]);   // NS==3
    };

    int idx[16], idx2[16];
    for (int t = 0; t < 16; ++t) { int sub=subOf(t); idx[t]=get(t==anchorOf(sub) ? M.IB-1 : M.IB); }
    if (M.IB2) for (int t = 0; t < 16; ++t) idx2[t]=get(t==0 ? M.IB2-1 : M.IB2);

    const int* wp  = weights(M.IB);
    const int* wp2 = M.IB2 ? weights(M.IB2) : nullptr;
    for (int t = 0; t < 16; ++t) {
        const int sub = subOf(t);
        const int* e0 = ep[sub*2]; const int* e1 = ep[sub*2+1];
        int r,g,b,a;
        if (M.IB2) {
            int ci=idx[t], ai=idx2[t];
            if (isel==1) { ci=idx2[t]; ai=idx[t]; }
            const int cw = (isel==0 ? wp : wp2)[ci];
            const int aw = (isel==0 ? wp2 : wp)[ai];
            r=((64-cw)*e0[0]+cw*e1[0]+32)>>6; g=((64-cw)*e0[1]+cw*e1[1]+32)>>6;
            b=((64-cw)*e0[2]+cw*e1[2]+32)>>6; a=((64-aw)*e0[3]+aw*e1[3]+32)>>6;
        } else {
            const int w = wp[idx[t]];
            r=((64-w)*e0[0]+w*e1[0]+32)>>6; g=((64-w)*e0[1]+w*e1[1]+32)>>6;
            b=((64-w)*e0[2]+w*e1[2]+32)>>6; a=((64-w)*e0[3]+w*e1[3]+32)>>6;
        }
        quint8 px[4] = {quint8(r),quint8(g),quint8(b),quint8(a)};
        if (rot==1) std::swap(px[0],px[3]);
        else if (rot==2) std::swap(px[1],px[3]);
        else if (rot==3) std::swap(px[2],px[3]);
        out[t] = {px[0],px[1],px[2],px[3]};
    }
}

}  // namespace

QImage BcDecode::decode(const QByteArray& data, int width, int height, int eTexFormat)
{
    if (width <= 0 || height <= 0)
        return {};
    const TexFormat::Codec codec = TexFormat::codec(eTexFormat, data.size(), width, height);
    if (!codec.valid) {
        // Future-proofing: an unrecognized eTexFormat after a patch should be LOUD in the log
        // (once per code, not per texture) instead of silently blank previews everywhere.
        static QSet<int> warned;
        static QMutex warnMtx;
        QMutexLocker lock(&warnMtx);
        if (!warned.contains(eTexFormat) && warned.size() < 32) {
            warned.insert(eTexFormat);
            qWarning("BcDecode: unrecognized eTexFormat %d (%dx%d, %lld bytes) — new format after "
                     "a game update? Add it to TexFormat::codec().",
                     eTexFormat, width, height, qlonglong(data.size()));
        }
        return {};
    }

    enum Kind { K_BC1, K_BC3, K_BC4, K_BC5, K_BC7, K_OTHER } kind = K_OTHER;
    switch (codec.glInternalFormat) {
    case TexFormat::GL_BC1: kind = K_BC1; break;
    case TexFormat::GL_BC3: kind = K_BC3; break;
    case TexFormat::GL_BC4: kind = K_BC4; break;
    case TexFormat::GL_BC5: kind = K_BC5; break;
    case TexFormat::GL_BC7: kind = K_BC7; break;
    default: return {};   // anything else: not decoded here
    }

    const int alignedW = TexFormat::alignedWidth(width, codec.bytesPerBlock);
    const int blocksPerRow = alignedW / 4;
    const int blockRows = (height + 3) / 4;
    const int blockBytes = codec.bytesPerBlock;
    const qint64 need = qint64(blocksPerRow) * blockRows * blockBytes;
    if (data.size() < need)
        return {};

    QImage img(width, height, QImage::Format_RGBA8888);
    img.fill(Qt::black);
    const quint8* base = reinterpret_cast<const quint8*>(data.constData());

    for (int by = 0; by < blockRows; ++by) {
        for (int bx = 0; bx < blocksPerRow; ++bx) {
            const quint8* blk = base + (qint64(by) * blocksPerRow + bx) * blockBytes;
            RGBA texel[16];
            if (kind == K_BC1) {
                decodeColorBlock(blk, texel, /*allowPunch=*/true);
            } else if (kind == K_BC3) {
                quint8 alpha[16];
                decodeAlphaBlock(blk, alpha);                 // 8 alpha bytes
                decodeColorBlock(blk + 8, texel, /*allowPunch=*/false);
                for (int i = 0; i < 16; ++i) texel[i].a = alpha[i];
            } else if (kind == K_BC4) {
                quint8 r[16];
                decodeAlphaBlock(blk, r);
                for (int i = 0; i < 16; ++i) texel[i] = {r[i], r[i], r[i], 255};
            } else if (kind == K_BC5) {
                quint8 r[16], g[16];
                decodeAlphaBlock(blk, r);
                decodeAlphaBlock(blk + 8, g);
                for (int i = 0; i < 16; ++i) texel[i] = {r[i], g[i], 0, 255};
            } else { // K_BC7
                decodeBC7Block(blk, texel);
            }
            for (int py = 0; py < 4; ++py) {
                const int y = by * 4 + py;
                if (y >= height) break;
                quint8* line = img.scanLine(y);
                for (int px = 0; px < 4; ++px) {
                    const int x = bx * 4 + px;
                    if (x >= width) continue;
                    const RGBA& t = texel[py * 4 + px];
                    quint8* o = line + x * 4;
                    o[0] = t.r; o[1] = t.g; o[2] = t.b; o[3] = t.a;
                }
            }
        }
    }
    return img;
}

QString BcDecode::selfTest()
{
    // --- BC7 partition/anchor table invariants (catches the 3-subset regression class) ---
    // 2-subset: texel 0 is always subset 0; the anchor sits in subset 1.
    for (int p = 0; p < 64; ++p) {
        if (((kP2[p] >> 0) & 1) != 0)
            return QStringLiteral("BC7 kP2[%1]: texel 0 not in subset 0").arg(p);
        const int a = kA2[p];
        if (a < 0 || a > 15) return QStringLiteral("BC7 kA2[%1] out of range").arg(p);
        if (((kP2[p] >> a) & 1) != 1)
            return QStringLiteral("BC7 kA2[%1]=%2 anchor not in subset 1").arg(p).arg(a);
    }
    // 3-subset: texel 0 in subset 0; all three subsets present; anchors in subsets 1 and 2.
    for (int p = 0; p < 64; ++p) {
        if (kP3[p][0] != 0) return QStringLiteral("BC7 kP3[%1]: texel 0 not in subset 0").arg(p);
        bool seen[3] = {false, false, false};
        for (int t = 0; t < 16; ++t) { const int s = kP3[p][t];
            if (s < 0 || s > 2) return QStringLiteral("BC7 kP3[%1][%2]=%3 invalid subset").arg(p).arg(t).arg(s);
            seen[s] = true; }
        if (!(seen[0] && seen[1] && seen[2]))
            return QStringLiteral("BC7 kP3[%1] missing a subset").arg(p);
        if (kP3[p][kA3a[p]] != 1) return QStringLiteral("BC7 kA3a[%1]=%2 not in subset 1").arg(p).arg(kA3a[p]);
        if (kP3[p][kA3b[p]] != 2) return QStringLiteral("BC7 kA3b[%1]=%2 not in subset 2").arg(p).arg(kA3b[p]);
    }

    // The decoder aligns each block row to a 256-byte pitch (D3D12), so even a 4x4 block needs a
    // 256-byte payload; the extra blocks are zero and never sampled (only x<4 is written).
    // --- BC4 round-trip: a0=255,a1=0; texel 0 index=1 (→0), rest index 0 (→255) ---
    {
        QByteArray blk(256, '\0');
        blk[0] = char(0xFF); blk[1] = 0x00; blk[2] = 0x01;   // index of texel 0 = 1
        const QImage im = decode(blk, 4, 4, 9);              // fmt 9 = BC4
        if (im.isNull()) return QStringLiteral("BC4 decode returned null");
        const QRgb t0 = im.pixel(0, 0), t1 = im.pixel(1, 0);
        if (qRed(t0) > 8)   return QStringLiteral("BC4 texel0 expected ~0, got %1").arg(qRed(t0));
        if (qRed(t1) < 247) return QStringLiteral("BC4 texel1 expected ~255, got %1").arg(qRed(t1));
    }
    // --- BC1 round-trip: c0=white, c1=black, all indices 0 → white ---
    {
        QByteArray blk(256, '\0');
        blk[0] = char(0xFF); blk[1] = char(0xFF);   // c0 = 0xFFFF (white)
        blk[2] = 0x00;       blk[3] = 0x00;         // c1 = 0x0000 (black)
        const QImage im = decode(blk, 4, 4, 10);    // fmt 10 = BC1
        if (im.isNull()) return QStringLiteral("BC1 decode returned null");
        const QRgb w = im.pixel(0, 0);
        if (qRed(w) < 240 || qGreen(w) < 240 || qBlue(w) < 240)
            return QStringLiteral("BC1 white texel too dark: (%1,%2,%3)").arg(qRed(w)).arg(qGreen(w)).arg(qBlue(w));
    }
    return QString();   // all good
}
