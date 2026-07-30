#pragma once
#include <QHash>
#include <QString>

class CascReader;

// ── Texture definitions, from CASC's bulk tables ────────────────────────────────────────────────
//
// WHY THIS EXISTS. Textures are the one asset type with NO per-sno meta entry: a texture sno
// resolves only to base/payload/<sno> and base/paylow/<sno>. Their dwWidth/dwHeight/eTexFormat live
// in bulk tables instead, which is why the tool has always taken them from d4data's
// Texture/<name>.tex.json — and why encrypted textures, absent from that dump, had no route at all.
// TexMeta.h flagged this as unfinished; this is that work.
//
// WHERE. base/texture-base-global.dat (34 MB decompressed, 141,829 definitions), plus per-key
// overlays base/texture-base-global-0x<hash>.dat. The overlay hash is the TACT KEY NAME BYTE-
// REVERSED: 0xb1aaab0ef7f159f1 is key f159f1f70eabaab1, the Doom collab key. So encrypted texture
// definitions ship in an overlay gated by the same key as their pixels, and holding the key gets
// you both.
//
// FORMAT — same indexed layout in the global table and the overlays:
//   [u32 magic][u32 count][ (u32 sno, u32 recordSize) x count ][ record blobs ]
// and within a record, relative to its sno field:
//   +0  sno          +12 eTexFormat
//   +16 volume slices (u16 x,y)         +20 width (u16)   +22 height (u16)   +24 depth
//   +32 importFlags
//
// VERIFIED against black.tex.json on five fields at once (eTexFormat 46, volume 1/1, 4x4, depth 1,
// importFlags 17), against a 256x256 texture in the global table, and against three encrypted
// stor245 textures whose sizes match the d4analyzer PNG export (1024x1024 fmt 47, 2048x2048 fmt 42,
// 1024x1024 fmt 41). The 4x4-vs-2048x2048 spread is what makes the u16 pair unambiguous.
class TextureDefTable {
public:
    struct Def {
        int width = 0, height = 0, format = -1;
        bool valid() const { return width > 0 && height > 0 && format >= 0; }
    };

    static TextureDefTable& instance();

    // Parses the global table and every readable overlay once. Cheap to call repeatedly.
    // Overlays whose TACT key is absent simply do not decode and are skipped.
    void ensureBuilt(CascReader* rd);
    void reset();
    bool ready() const { return m_ready; }
    int  count() const { return m_defs.size(); }

    Def lookup(int sno) const { return m_defs.value(sno); }

private:
    TextureDefTable() = default;
    void parseTable(const QByteArray& blob, const QString& label, int* added);
    QHash<int, Def> m_defs;
    bool m_ready = false;
};
