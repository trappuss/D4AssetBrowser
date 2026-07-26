#pragma once
#include <QHash>
#include <QString>
#include <QVector>

class CascReader;

// Reader for the game's global 2D-atlas frame table: base/Misc/2D_table.dat.
//
// This is the authoritative source for how many TexFrames each 2DInventory_* /
// UI atlas has, their hImageHandle values, and their order — the same data
// d4analyzer's db::TexFrames::load(Storage&) consumes. Crucially it needs NO
// d4data: the per-texture .tex.json snapshot (ptFrame[]) is missing for
// seasonal/expansion atlases (e001/e002) and for any texture the d4data
// snapshot lags after a patch, but 2D_table.dat ships in CASC and covers every
// atlas.
//
// Binary layout (verified against the live file):
//   [16-byte header]
//   [ N × { u32 hImageHandle, u32 atlasSno, u32 frameIndex } ]   (12 bytes each)
// Records are grouped by atlas; frameIndex is 0-based within an atlas.
//
// The table does NOT contain the per-frame UV rectangles — those live in the
// texture's ptFrame definition, which is not present as a readable CASC blob
// for these atlases. The Textures tab recovers the rectangles by segmenting the
// decoded atlas image (alpha gutters) and uses this table for the frame count,
// handles and ordering.
class FrameTable {
public:
    static FrameTable& instance();

    // Load once. Tries, in order: CASC (base/Misc/2D_table.dat), a copy next to
    // the executable, then <AppData>/2D_table.dat. Safe to call repeatedly.
    // `reader` may be null (then only the file fallbacks are tried).
    bool ensureLoaded(CascReader* reader);

    bool isLoaded() const { return m_loaded; }
    QString source() const { return m_source; }   // where it was loaded from (for logging)

    // Number of frames the table records for an atlas sno (0 = not a known atlas).
    int frameCount(quint32 atlasSno) const;
    // hImageHandles for an atlas, indexed by frameIndex (may contain 0 holes).
    QVector<quint32> handles(quint32 atlasSno) const;
    bool contains(quint32 atlasSno) const { return m_byAtlas.contains(atlasSno); }

    // Reverse lookup: which atlas + frame index an icon handle belongs to.
    // Returns false if the handle isn't in the table.
    bool locate(quint32 handle, quint32& atlasSno, int& frameIndex) const;

private:
    FrameTable() = default;
    bool parse(const QByteArray& data);

    bool    m_loaded = false;
    QString m_source;
    QHash<quint32, QVector<quint32>> m_byAtlas;   // atlasSno -> handles[frameIndex]
    QHash<quint32, QPair<quint32, int>> m_byHandle;   // handle -> (atlasSno, frameIndex)
};
