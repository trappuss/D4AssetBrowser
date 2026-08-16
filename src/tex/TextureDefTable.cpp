#include "tex/TextureDefTable.h"

#include "casc/CascReader.h"

#include <QDebug>

namespace {
constexpr int kRecSno = 0, kRecFormat = 12, kRecWidth = 20, kRecHeight = 22;
constexpr int kMinRec = 32;   // a record must reach importFlags for the fields above to exist

inline quint32 u32(const QByteArray& b, int o)
{
    if (o < 0 || o + 4 > b.size()) return 0;
    return quint32(uchar(b[o])) | quint32(uchar(b[o + 1])) << 8
         | quint32(uchar(b[o + 2])) << 16 | quint32(uchar(b[o + 3])) << 24;
}
inline quint16 u16(const QByteArray& b, int o)
{
    if (o < 0 || o + 2 > b.size()) return 0;
    return quint16(quint16(uchar(b[o])) | quint16(uchar(b[o + 1])) << 8);
}
}  // namespace

TextureDefTable& TextureDefTable::instance()
{
    static TextureDefTable inst;
    return inst;
}

void TextureDefTable::reset()
{
    QMutexLocker lock(&m_mutex);
    m_defs.clear();
    m_ready = false;
}

void TextureDefTable::parseTable(const QByteArray& b, const QString& label, int* added)
{
    if (b.size() < 8) return;
    const int count = int(u32(b, 4));
    if (count <= 0 || count > 4000000) return;
    const int idxEnd = 8 + count * 8;
    if (idxEnd > b.size()) return;

    // Records follow the index, each preceded by its own sno field. Rather than accumulate sizes
    // (which did not sum to the file length — there is alignment padding between blobs), each
    // record is located by scanning forward for its sno. The index still supplies the sno LIST and
    // the expected record size, so this is a seek, not a search of the whole file.
    int cursor = idxEnd;
    int local = 0;
    for (int i = 0; i < count; ++i) {
        const int sno  = int(u32(b, 8 + i * 8));
        const int size = int(u32(b, 12 + i * 8));
        if (sno <= 0 || size < kMinRec) continue;
        // The record for this sno starts at the next occurrence of its own id at or after cursor.
        int at = -1;
        for (int o = cursor; o + kMinRec <= b.size(); o += 4)
            if (int(u32(b, o + kRecSno)) == sno) { at = o; break; }
        if (at < 0) continue;
        cursor = at + 4;
        Def d;
        d.format = int(u32(b, at + kRecFormat));
        d.width  = int(u16(b, at + kRecWidth));
        d.height = int(u16(b, at + kRecHeight));
        // Guard rails: a plausible texture is power-of-two-ish and not absurd. A record that fails
        // this is a misalignment, and inserting it would silently produce garbled decodes later.
        if (d.width <= 0 || d.height <= 0 || d.width > 16384 || d.height > 16384) continue;
        if (d.format < 0 || d.format > 255) continue;
        m_defs.insert(sno, d);
        ++local;
    }
    if (added) *added += local;
    qInfo("texture-defs: %s — %d of %d record(s) parsed", qPrintable(label), local, count);
}

int TextureDefTable::count() const
{
    QMutexLocker lock(&m_mutex);
    return m_defs.size();
}

TextureDefTable::Def TextureDefTable::lookup(int sno) const
{
    QMutexLocker lock(&m_mutex);
    return m_defs.value(sno);
}

void TextureDefTable::ensureBuilt(CascReader* rd)
{
    // Reached from several worker threads at once (the icon-index build, texture-grid thumbnails,
    // the wardrobe piece loader). The m_ready flag alone was not enough: it is set BEFORE the 34 MB
    // parse, so a second thread would sail past it and read m_defs while the first was still
    // filling it. Held across the whole build; parseTable is called with the lock held.
    QMutexLocker lock(&m_mutex);
    if (m_ready || !rd || !rd->isReady()) return;
    m_ready = true;   // one attempt; a failure must not retry on every texture load

    int added = 0;
    const QByteArray global = rd->readFile(QStringLiteral("base/texture-base-global.dat"));
    if (!global.isEmpty()) parseTable(global, QStringLiteral("global"), &added);

    // Per-key overlays. The hash in the filename is the TACT key name byte-reversed, so an overlay
    // we hold no key for simply returns empty — skipped without comment, since that is the normal
    // state for most of the 137.
    int overlays = 0;
    for (const QString& p : rd->rootPathsWithPrefix(QStringLiteral("base/texture-base-global-"))) {
        const QByteArray blob = rd->readFile(p);
        if (blob.isEmpty()) continue;
        ++overlays;
        parseTable(blob, p.section(QLatin1Char('-'), -1), &added);
    }
    qInfo("texture-defs: %d definition(s) from the global table + %d readable overlay(s)",
          int(m_defs.size()), overlays);
}
