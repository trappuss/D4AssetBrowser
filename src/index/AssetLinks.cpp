#include "index/AssetLinks.h"

#include "app/AppPaths.h"

#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

#include <atomic>
#include <thread>
#include <vector>

namespace {
constexpr int kCacheVersion = 1;
}

AssetLinks& AssetLinks::instance()
{
    static AssetLinks a;
    return a;
}

QString AssetLinks::slotRole(int slot)
{
    switch (slot) {
        case 1: case 11: case 13: case 19: return QStringLiteral("BASE_COLOR");
        case 3: case 47: case 48:          return QStringLiteral("NORMAL");
        case 54:                           return QStringLiteral("DYE_MASK");
        case 56:                           return QStringLiteral("DYE_RAMP");
        case 62: case 112: case 113:       return QStringLiteral("ROUGHNESS");
        case 63:                           return QStringLiteral("METALLIC");
        case 81:                           return QStringLiteral("AO");
        case 86:                           return QStringLiteral("EMISSIVE");
        case 96:                           return QStringLiteral("MASK_PRIMARY");
        case 97:                           return QStringLiteral("NOISE_PROCEDURAL");
        case 104:                          return QStringLiteral("TRANSLUCENCY");
        case 108:                          return QStringLiteral("DYE_MASK_2");
        case 145:                          return QStringLiteral("SKIN_MASK");
        case 212: case 213: case 214:      return QStringLiteral("DETAIL_NORMAL");
        case 218: case 219: case 220:      return QStringLiteral("DETAIL_ROUGHNESS");
        default:                           return QStringLiteral("SLOT_%1").arg(slot);
    }
}

QVector<AssetLinks::MatLink> AssetLinks::linksForTexture(int texSno) const
{
    QVector<MatLink> out;
    for (int matSno : m_texToMats.value(texSno)) {
        MatLink l;
        l.matSno = matSno;
        l.texPairs = m_matTextures.value(matSno);
        l.apps = m_matToApps.value(matSno);
        out.append(l);
    }
    return out;
}

void AssetLinks::install(QHash<int, QVector<QPair<int, int>>> matTex,
                         QHash<int, QVector<int>> matToApps)
{
    m_matTextures = std::move(matTex);
    m_matToApps = std::move(matToApps);
    m_texToMats.clear();
    for (auto it = m_matTextures.constBegin(); it != m_matTextures.constEnd(); ++it)
        for (const auto& tp : it.value())
            m_texToMats[tp.first].append(it.key());
    m_building = false;
    m_ready = true;
    emit readyChanged();
}

void AssetLinks::reset()
{
    m_ready = false;
    m_building = false;   // clear any in-flight build flag, else ensureBuilt() never rebuilds
    m_matTextures.clear();
    m_texToMats.clear();
    m_matToApps.clear();
    const QString cachePath = AppPaths::dataDir()
                              + QStringLiteral("/asset_links_v%1.bin").arg(kCacheVersion);
    QFile::remove(cachePath);
    emit readyChanged();
}

void AssetLinks::ensureBuilt(const QString& d4dataDir)
{
    if (m_ready || m_building || d4dataDir.isEmpty())
        return;
    m_building = true;

    const QString cacheBase = AppPaths::dataDir();
    const QString cachePath = cacheBase + QStringLiteral("/asset_links_v%1.bin").arg(kCacheVersion);

    // Everything on the worker — the binary cache holds ~167k links, and even its QDataStream
    // read is file I/O + hash construction that has no business on the GUI thread.
    const QString d4 = d4dataDir, cb = cacheBase, cp = cachePath;
    std::thread([this, d4, cb, cp]() {
        constexpr quint32 kMagic = 0xA55E711Cu;   // (binary cache — robust for large maps)
        if (QFile::exists(cp)) {
            QFile f(cp);
            if (f.open(QIODevice::ReadOnly)) {
                QDataStream ds(&f);
                quint32 magic = 0; ds >> magic;
                if (magic == kMagic) {
                    QHash<int, QVector<QPair<int, int>>> matTex;
                    QHash<int, QVector<int>> matApps;
                    ds >> matTex >> matApps;
                    if (ds.status() == QDataStream::Ok && !matTex.isEmpty()) {
                        QMetaObject::invokeMethod(this, [this, matTex, matApps]() mutable {
                            install(std::move(matTex), std::move(matApps));
                        }, Qt::QueuedConnection);
                        return;
                    }
                }
            }
        }
        const QString meta = d4 + QStringLiteral("/json/base/meta");
        // ~167k files (100k materials + 66k appearances) — full JSON DOM parsing is
        // far too slow, so scan the needed fields with regex across all CPU cores.
        const QStringList matFiles = QDir(meta + QStringLiteral("/Material"))
            .entryList(QStringList{QStringLiteral("*.mat.json")}, QDir::Files);
        const QStringList appFiles = QDir(meta + QStringLiteral("/Appearance"))
            .entryList(QStringList{QStringLiteral("*.app.json")}, QDir::Files);
        const int grand = qMax(1, matFiles.size() + appFiles.size());
        std::atomic<int> done{0};
        auto bump = [this, &done, grand]() {
            const int d = done.fetch_add(1) + 1;
            if ((d % 2000) == 0) {
                const int pct = d * 100 / grand;
                QMetaObject::invokeMethod(this, [this, pct]() { emit progress(pct); },
                                          Qt::QueuedConnection);
            }
        };
        const int nT = qBound(1, int(std::thread::hardware_concurrency()), 8);

        // ── Materials → texture roster (regex byte-scan, sharded by thread) ──
        QVector<QHash<int, QVector<QPair<int, int>>>> matParts(nT);
        {
            static const QRegularExpression rxSno(QStringLiteral("\"__snoID__\":\\s*(\\d+)"));
            static const QRegularExpression rxSlot(QStringLiteral("\"eShaderTex\":\\s*(-?\\d+)"));
            static const QRegularExpression rxTex(QStringLiteral(
                "\"snoTex\":\\s*(?:null|\\{[^{}]*?\"__raw__\":\\s*(\\d+))"));
            std::vector<std::thread> ws;
            for (int t = 0; t < nT; ++t)
                ws.emplace_back([&, t]() {
                    for (int i = t; i < matFiles.size(); i += nT) {
                        bump();
                        QFile f(meta + QStringLiteral("/Material/") + matFiles[i]);
                        if (!f.open(QIODevice::ReadOnly)) continue;
                        const QString raw = QString::fromUtf8(f.readAll());
                        const auto sm = rxSno.match(raw);
                        if (!sm.hasMatch()) continue;
                        const int sno = sm.captured(1).toInt();
                        if (sno <= 0) continue;
                        QVector<int> slotIds, texs;   // 'slots' is a Qt keyword macro
                        auto si = rxSlot.globalMatch(raw);
                        while (si.hasNext()) slotIds.append(si.next().captured(1).toInt());
                        auto ti = rxTex.globalMatch(raw);
                        while (ti.hasNext()) { const auto m = ti.next();
                            texs.append(m.captured(1).isEmpty() ? 0 : m.captured(1).toInt()); }
                        if (slotIds.size() != texs.size()) continue;   // ambiguous layout
                        QVector<QPair<int, int>> pairs;
                        for (int k = 0; k < slotIds.size(); ++k)
                            if (texs[k] > 0) pairs.append({texs[k], slotIds[k]});
                        if (!pairs.isEmpty()) matParts[t].insert(sno, pairs);
                    }
                });
            for (auto& w : ws) w.join();
        }
        QHash<int, QVector<QPair<int, int>>> matTex;
        for (auto& p : matParts)
            for (auto it = p.constBegin(); it != p.constEnd(); ++it) matTex.insert(it.key(), it.value());

        // ── Appearances → materials they reference (regex byte-scan, sharded) ──
        QVector<QHash<int, QVector<int>>> appParts(nT);
        {
            static const QRegularExpression rxSno(QStringLiteral("\"__snoID__\":\\s*(\\d+)"));
            static const QRegularExpression rxMat(QStringLiteral(
                "\"snoMaterial\":\\s*\\{[^{}]*?\"__raw__\":\\s*(\\d+)"));
            std::vector<std::thread> ws;
            for (int t = 0; t < nT; ++t)
                ws.emplace_back([&, t]() {
                    for (int i = t; i < appFiles.size(); i += nT) {
                        bump();
                        QFile f(meta + QStringLiteral("/Appearance/") + appFiles[i]);
                        if (!f.open(QIODevice::ReadOnly)) continue;
                        const QString raw = QString::fromUtf8(f.readAll());
                        const auto sm = rxSno.match(raw);
                        if (!sm.hasMatch()) continue;
                        const int app = sm.captured(1).toInt();
                        if (app <= 0) continue;
                        QSet<int> mats;
                        auto mi = rxMat.globalMatch(raw);
                        while (mi.hasNext()) { const int ms = mi.next().captured(1).toInt();
                            if (ms > 0) mats.insert(ms); }
                        for (int ms : mats) appParts[t][ms].append(app);
                    }
                });
            for (auto& w : ws) w.join();
        }
        QHash<int, QVector<int>> matToApps;
        for (auto& p : appParts)
            for (auto it = p.constBegin(); it != p.constEnd(); ++it) matToApps[it.key()] += it.value();

        // Persist the cache (compact binary — fast + size-robust vs JSON).
        QDir().mkpath(cb);
        QFile out(cp);
        if (out.open(QIODevice::WriteOnly)) {
            QDataStream ds(&out);
            ds << quint32(0xA55E711Cu) << matTex << matToApps;
            out.flush();
        }

        QMetaObject::invokeMethod(this, [this, matTex, matToApps]() mutable {
            install(std::move(matTex), std::move(matToApps));
        }, Qt::QueuedConnection);
    }).detach();
}
