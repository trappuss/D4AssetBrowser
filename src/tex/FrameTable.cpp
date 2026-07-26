#include "tex/FrameTable.h"

#include "app/AppPaths.h"

#include "casc/CascReader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

FrameTable& FrameTable::instance()
{
    static FrameTable inst;
    return inst;
}

bool FrameTable::ensureLoaded(CascReader* reader)
{
    if (m_loaded)
        return true;

    QByteArray data;
    QString src;

    // 1) CASC — the ideal source (always current with the game build). Our reader
    //    resolves mostly numeric paths, so the named Misc path may be absent; try a
    //    couple of spellings, then fall back to a local copy.
    if (reader && reader->isReady()) {
        for (const char* p : { "base/Misc/2D_table.dat", "base/misc/2d_table.dat",
                               "base\\Misc\\2D_table.dat" }) {
            data = reader->readFile(QString::fromLatin1(p));
            if (!data.isEmpty()) { src = QStringLiteral("CASC:%1").arg(QString::fromLatin1(p)); break; }
        }
    }

    // 2) A copy next to the executable (extract once with rustydemon:
    //    `export --path "base/Misc/2D_table.dat" --flat`).
    if (data.isEmpty()) {
        const QString p = QDir(QCoreApplication::applicationDirPath())
                              .filePath(QStringLiteral("2D_table.dat"));
        QFile f(p);
        if (f.open(QIODevice::ReadOnly)) { data = f.readAll(); src = QStringLiteral("file:%1").arg(p); }
    }

    // 3) AppData.
    if (data.isEmpty()) {
        const QString p = AppPaths::dataDir()
                          + QStringLiteral("/2D_table.dat");
        QFile f(p);
        if (f.open(QIODevice::ReadOnly)) { data = f.readAll(); src = QStringLiteral("file:%1").arg(p); }
    }

    if (data.isEmpty())
        return false;

    if (!parse(data))
        return false;

    m_loaded = true;
    m_source = src;
    return true;
}

bool FrameTable::parse(const QByteArray& data)
{
    // 16-byte header, then 12-byte records { u32 handle, u32 atlasSno, u32 frameIndex }.
    constexpr int kHeader = 16;
    constexpr int kRec    = 12;
    if (data.size() < kHeader + kRec || (data.size() - kHeader) % kRec != 0)
        return false;

    const uchar* d = reinterpret_cast<const uchar*>(data.constData());
    auto u32 = [d](qint64 o) -> quint32 {
        return quint32(d[o]) | (quint32(d[o + 1]) << 8)
             | (quint32(d[o + 2]) << 16) | (quint32(d[o + 3]) << 24);
    };

    const int n = int((data.size() - kHeader) / kRec);
    m_byAtlas.clear();
    m_byHandle.clear();
    m_byAtlas.reserve(8192);
    m_byHandle.reserve(n);
    for (int i = 0; i < n; ++i) {
        const qint64 o = qint64(kHeader) + qint64(i) * kRec;
        const quint32 handle = u32(o);
        const quint32 sno    = u32(o + 4);
        const quint32 idx    = u32(o + 8);
        if (idx > 100000)            // guard against a misaligned/garbage record
            continue;
        QVector<quint32>& v = m_byAtlas[sno];
        if (int(idx) >= v.size())
            v.resize(int(idx) + 1);
        v[int(idx)] = handle;
        if (handle)
            m_byHandle.insert(handle, qMakePair(sno, int(idx)));
    }
    return !m_byAtlas.isEmpty();
}

bool FrameTable::locate(quint32 handle, quint32& atlasSno, int& frameIndex) const
{
    const auto it = m_byHandle.constFind(handle);
    if (it == m_byHandle.constEnd())
        return false;
    atlasSno   = it.value().first;
    frameIndex = it.value().second;
    return true;
}

int FrameTable::frameCount(quint32 atlasSno) const
{
    const auto it = m_byAtlas.constFind(atlasSno);
    return it == m_byAtlas.constEnd() ? 0 : it.value().size();
}

QVector<quint32> FrameTable::handles(quint32 atlasSno) const
{
    return m_byAtlas.value(atlasSno);
}
