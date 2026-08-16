#include "index/IconIndex.h"

#include "app/AppPaths.h"

#include "casc/CascReader.h"
#include "tex/BcDecode.h"
#include "tex/FrameTable.h"
#include "tex/TexMeta.h"
#include "tex/TextureDefTable.h"

#include <QSet>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRect>
#include <QStandardPaths>

#include <algorithm>
#include <thread>

namespace {
// v4: the index now also carries handles recovered from CASC (2D_table.dat + the texture-def
// tables) for atlases d4data has no JSON for. A v3 file loads perfectly and satisfies the
// signature check, which would skip the new pass and leave those icons blank with no symptom.
constexpr int kCacheVersion = 4;
}

IconIndex& IconIndex::instance()
{
    static IconIndex inst;
    return inst;
}

void IconIndex::install(QHash<quint32, Frame> frames)
{
    m_frames = std::move(frames);
    m_building = false;
    m_ready = true;
    emit readyChanged();
}

void IconIndex::reset()
{
    m_ready = false;
    m_building = false;   // clear any in-flight build flag, else ensureBuilt() never rebuilds
    m_frames.clear();
    m_atlasCache.clear();
    const QString cachePath = AppPaths::dataDir()
                              + QStringLiteral("/icon_index_v%1.json").arg(kCacheVersion);
    QFile::remove(cachePath);
    // Superseded versions are pruned at startup by AppPaths::pruneOldCaches — see main.cpp, whose
    // version numbers must be kept in step with kCacheVersion above.
    emit readyChanged();
}

void IconIndex::ensureBuilt(const QString& d4dataDir, CascReader* reader)
{
    if (m_ready || m_building)
        return;
    const QString texDir = d4dataDir.isEmpty()
                               ? QString()
                               : d4dataDir + QStringLiteral("/json/base/meta/Texture");
    const bool haveJson = !texDir.isEmpty() && QDir(texDir).exists();
    // Used to return here outright, which meant no d4data ⇒ no icons, ever. The CASC pass alone is
    // a complete (if coarser) index, so a missing snapshot is no longer fatal.
    if (!haveJson && !reader)
        return;
    m_building = true;

    const QString cacheBase = AppPaths::dataDir();
    const QString cachePath = cacheBase + QStringLiteral("/icon_index_v%1.json").arg(kCacheVersion);

    // Everything on the worker — the atlas-count signature is a directory scan over thousands
    // of files and the cache is a sizeable JSON; neither belongs on the GUI thread.
    const QString td = texDir, cb = cacheBase, cp = cachePath;
    CascReader* const rd = reader;
    std::thread([this, td, cb, cp, rd]() {
        // Two DISTINCT numbers, deliberately not folded into one.
        //   nJson  — how many 2D*.tex.json files the scan will visit. The progress denominator.
        //   s      — the cache signature: nJson mixed with the game's own atlas count, so a patch
        //            that adds atlases without touching the d4data snapshot still invalidates.
        // A first cut added `atlases * 1000000` straight into `s` and reused it as the denominator.
        // That pinned the progress bar at 0% (the quotient is always zero once s is ~1e9) and
        // overflowed int past ~2,147 atlases — and FrameTable reserves for 8,192.
        int nJson = 0;
        if (!td.isEmpty()) {
            QDirIterator c(td, QStringList{"2D*.tex.json"}, QDir::Files);
            while (c.hasNext()) { c.next(); ++nJson; }
        }
        const qint64 atlasN = (rd && FrameTable::instance().ensureLoaded(rd))
                                  ? qint64(FrameTable::instance().atlases().size()) : 0;
        const int s = int(quint32(nJson) ^ (quint32(atlasN) * 2654435761u));
        if (QFile::exists(cp)) {
            QFile f(cp);
            if (f.open(QIODevice::ReadOnly)) {
                const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
                if (root.value("sig").toInt() == s) {
                    QHash<quint32, Frame> cached;
                    const QJsonObject jf = root.value("frames").toObject();
                    for (auto i = jf.constBegin(); i != jf.constEnd(); ++i) {
                        const QJsonArray a = i.value().toArray();
                        if (a.size() < 8) continue;
                        Frame fr;
                        fr.atlasSno = a.at(0).toInt(); fr.fmt = a.at(1).toInt();
                        fr.w = a.at(2).toInt();        fr.h = a.at(3).toInt();
                        fr.u0 = float(a.at(4).toDouble()); fr.v0 = float(a.at(5).toDouble());
                        fr.u1 = float(a.at(6).toDouble()); fr.v1 = float(a.at(7).toDouble());
                        fr.frameIdx = a.size() > 8 ? a.at(8).toInt() : -1;
                        cached.insert(i.key().toUInt(), fr);
                    }
                    QMetaObject::invokeMethod(this, [this, cached]() mutable {
                        install(std::move(cached));
                    }, Qt::QueuedConnection);
                    return;
                }
            }
        }
        QHash<quint32, Frame> frames;
        const int total = qMax(1, nJson);
        int seen = 0;
        QDirIterator it(td.isEmpty() ? QDir::tempPath() : td,
                        QStringList{"2D*.tex.json"}, QDir::Files);
        while (!td.isEmpty() && it.hasNext()) {
            if ((++seen % 128) == 0) {
                const int pct = seen * 100 / total;
                QMetaObject::invokeMethod(this, [this, pct]() { emit progress(pct); },
                                          Qt::QueuedConnection);
            }
            const QString fpath = it.next();
            QFile f(fpath);
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QJsonObject d = QJsonDocument::fromJson(f.readAll()).object();
            const QJsonArray frArr = d.value("ptFrame").toArray();
            if (frArr.isEmpty()) continue;
            const int sno = d.value("__snoID__").toInt();
            const int fmt = d.value("eTexFormat").toInt();
            int w = d.value("dwWidth").toInt();
            int h = d.value("dwHeight").toInt();
            // Correct stale atlas dimensions so the frame UVs crop the right icons after a game
            // patch (the .tex.json snapshot can lag the real atlas; UVs stay valid, dims don't).
            {
                const QString atlasName = QFileInfo(fpath).fileName().section(QStringLiteral(".tex.json"), 0, 0);
                int ow = 0, oh = 0;
                if (textureDimOverride(atlasName, ow, oh)) { w = ow; h = oh; }
            }
            int frameIdx = -1;
            for (const QJsonValue& fv : frArr) {
                ++frameIdx;   // position in ptFrame — matches d4analyzer's exported frame index
                const QJsonObject fo = fv.toObject();
                const quint32 hh = quint32(fo.value("hImageHandle").toDouble());
                if (!hh || frames.contains(hh)) continue;
                Frame fr;
                fr.atlasSno = sno; fr.fmt = fmt; fr.w = w; fr.h = h; fr.frameIdx = frameIdx;
                fr.u0 = float(fo.value("flU0").toDouble());
                fr.v0 = float(fo.value("flV0").toDouble());
                fr.u1 = float(fo.value("flU1").toDouble(1.0));
                fr.v1 = float(fo.value("flV1").toDouble(1.0));
                frames.insert(hh, fr);
            }
        }

        // ── CASC pass: atlases d4data has no .tex.json for ───────────────────────────────────────
        // 2D_table.dat gives handle → atlas + frame ORDINAL, and the texture-def tables give the
        // atlas's dimensions and format. What neither carries is the per-frame UV RECTANGLE.
        //
        // So this deliberately only claims the atlases where the rectangle is not in doubt: a
        // single-frame atlas is its own icon, UV 0..1. That is exactly the shape of the bundle art
        // (2DInventory_Bundle_HArmor_bar_stor251 and siblings) this exists to recover.
        //
        // Multi-frame atlases are COUNTED AND SKIPPED, not guessed. The Textures tab recovers their
        // rectangles by alpha-gutter segmentation, but its own comment records that pairing those
        // rectangles back to handles is approximate because 2D_table's authored order is not the
        // atlas's spatial order — and an icon silently showing the WRONG item is worse than one
        // showing nothing. Those stay served by data/icon_overrides.
        int fromCasc = 0, multiSkipped = 0, noDef = 0;
        if (rd && FrameTable::instance().isLoaded()) {
            TextureDefTable::instance().ensureBuilt(rd);
            for (const quint32 atlas : FrameTable::instance().atlases()) {
                const QVector<quint32> hs = FrameTable::instance().handles(atlas);
                if (hs.size() != 1) { if (hs.size() > 1) ++multiSkipped; continue; }
                const quint32 h = hs.at(0);
                if (!h || frames.contains(h)) continue;   // d4data already answered for this one
                const TextureDefTable::Def d = TextureDefTable::instance().lookup(int(atlas));
                if (!d.valid()) { ++noDef; continue; }
                Frame fr;
                fr.atlasSno = int(atlas); fr.fmt = d.format; fr.w = d.width; fr.h = d.height;
                fr.frameIdx = 0;
                fr.u0 = 0.0f; fr.v0 = 0.0f; fr.u1 = 1.0f; fr.v1 = 1.0f;
                frames.insert(h, fr);
                ++fromCasc;
            }
            qInfo("icon-index: CASC pass — %d single-frame atlas icon(s) added, %d multi-frame "
                  "atlas(es) skipped (no per-frame UVs; use icon_overrides), %d with no texture "
                  "definition", fromCasc, multiSkipped, noDef);
        }

        QMetaObject::invokeMethod(this, [this, frames, cb, cp, s]() {
            QDir().mkpath(cb);
            QJsonObject root, jf;
            for (auto i = frames.constBegin(); i != frames.constEnd(); ++i) {
                const Frame& fr = i.value();
                QJsonArray a;
                a.append(fr.atlasSno); a.append(fr.fmt); a.append(fr.w); a.append(fr.h);
                a.append(double(fr.u0)); a.append(double(fr.v0));
                a.append(double(fr.u1)); a.append(double(fr.v1));
                a.append(fr.frameIdx);
                jf.insert(QString::number(i.key()), a);
            }
            root.insert("sig", s);
            root.insert("frames", jf);
            QFile f(cp);
            if (f.open(QIODevice::WriteOnly))
                f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
            install(frames);
        }, Qt::QueuedConnection);
    }).detach();
}

QImage IconIndex::iconImage(quint32 handle, CascReader* reader) const
{
    const auto it = m_frames.constFind(handle);
    if (it == m_frames.constEnd())
        return {};
    // Exported-frame override wins: when the atlas ptFrame layout in d4data is stale after a patch,
    // a d4analyzer-exported icon PNG for this atlas+frame-index crops correctly regardless of UVs.
    {
        QImage ov = frameIconOverride(it.value().atlasSno, it.value().frameIdx);
        if (!ov.isNull())
            return ov;
    }
    if (!reader || !reader->isReady())
        return {};
    const Frame& fr = it.value();
    QImage atlas = m_atlasCache.value(fr.atlasSno);
    if (atlas.isNull()) {
        // An entire ATLAS failing takes out every icon on it at once — potentially hundreds of
        // rows going blank with no hint they share one root cause. Warned once per atlas, so the
        // log says "this atlas" rather than repeating for each icon.
        static QSet<int> warnedAtlas;
        auto warnOnce = [&](const char* what) {
            if (warnedAtlas.contains(fr.atlasSno)) return;
            warnedAtlas.insert(fr.atlasSno);
            qWarning("icons: atlas sno %d %s — every icon on it renders blank (%dx%d fmt %d)",
                     fr.atlasSno, what, fr.w, fr.h, fr.fmt);
        };
        const QByteArray payload = reader->readPayloadBySno(quint64(fr.atlasSno));
        if (payload.isEmpty()) {
            warnOnce("has no payload in CASC (usually a TACT key we do not hold)");
            return {};
        }
        atlas = BcDecode::decode(payload, fr.w, fr.h, fr.fmt);
        if (atlas.isNull()) {
            warnOnce("failed to decode");
            return {};
        }
        if (m_atlasCache.size() > 8)
            m_atlasCache.clear();            // simple cap; icons are also cached upstream
        m_atlasCache.insert(fr.atlasSno, atlas);
    }
    const int aw = atlas.width(), ah = atlas.height();
    int x0 = qRound(fr.u0 * aw), x1 = qRound(fr.u1 * aw);
    int y0 = qRound(fr.v0 * ah), y1 = qRound(fr.v1 * ah);
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1 = std::min(aw, x1); y1 = std::min(ah, y1);
    if (x1 <= x0 || y1 <= y0)
        return {};
    return atlas.copy(QRect(x0, y0, x1 - x0, y1 - y0));
}
