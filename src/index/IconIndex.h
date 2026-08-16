#pragma once
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>

class CascReader;

// Inventory-icon atlas index. The 2DInventory_* / 2DUI_* textures are sprite-sheet
// atlases; each carries a `ptFrame` list keyed by `hImageHandle` with a UV rect.
// This maps a UI image handle (Item.tInvImages.hDefaultImage, resolved per
// appearance by AppearanceMeta::iconFor) to the atlas texture (sno) + UV rect, by
// scanning every base/meta/Texture/2D*.tex.json once on a background thread (cached
// to disk). iconImage() decodes the atlas via the CPU BC decoder and crops the icon.
// Port of the Python IconIndex.
class IconIndex : public QObject {
    Q_OBJECT
public:
    static IconIndex& instance();

    bool ready() const { return m_ready; }
    bool building() const { return m_building; }
    bool has(quint32 handle) const { return m_frames.contains(handle); }

    // Build (or load from cache) on a background thread. No-op if ready/in-progress.
    //
    // `reader` is what makes ENCRYPTED atlases resolvable. The d4data scan below can only see
    // atlases the snapshot ships a 2D*.tex.json for, so every collab/store atlas contributed zero
    // handles and its cards rendered blank — the same shape of bug TexturesTab had, one layer up.
    // With a reader, a second pass reads the game's own base/Misc/2D_table.dat (handle → atlas +
    // frame) and the CASC texture tables (dimensions + format), neither of which needs d4data.
    // Null reader keeps the old JSON-only behaviour.
    void ensureBuilt(const QString& d4dataDir, CascReader* reader = nullptr);
    // Drop the in-memory index + delete the on-disk cache, so the next ensureBuilt rebuilds
    // from scratch. Called when the game build / d4data changes (stale-cache invalidation).
    void reset();

    // Decode the atlas for `handle` and crop its icon. Null QImage if the handle is
    // unknown, the atlas can't be read, or the format isn't decodable.
    QImage iconImage(quint32 handle, CascReader* reader) const;

signals:
    void readyChanged();
    void progress(int pct);   // 0..100 while scanning atlases (cache miss only)

private:
    explicit IconIndex(QObject* parent = nullptr) : QObject(parent) {}

    struct Frame {
        int   atlasSno = 0, fmt = 0, w = 0, h = 0;
        int   frameIdx = -1;   // position in the atlas ptFrame array (for exported-icon overrides)
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    };
    void install(QHash<quint32, Frame> frames);

    QHash<quint32, Frame> m_frames;
    // Decoded-atlas cache so many icons sharing one atlas decode it once. Used only
    // from the GUI thread (icon painting), so no locking. Capped to a few atlases.
    mutable QHash<int, QImage> m_atlasCache;
    bool m_ready = false;
    bool m_building = false;
};
