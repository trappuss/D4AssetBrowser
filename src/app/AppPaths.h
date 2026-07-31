#pragma once
// Portable, self-contained storage. Everything the tool writes (settings INI, caches, thumbnails,
// logs, guards) lives in a "data" folder beside the executable — no Windows registry, no %AppData%.
// Copy the release folder anywhere (or a USB stick) and it runs and remembers its state, leaving zero
// traces on the host machine. main() points QSettings at data/ so every QSettings() default-ctor call
// resolves here too.
#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QRegularExpression>
#include <QString>

namespace AppPaths {

// The portable data directory beside the exe (created on first use). Falls back to the current
// working directory if applicationDirPath() isn't available yet (shouldn't happen post-QApplication).
inline QString dataDir()
{
    static const QString d = [] {
        QString base = QCoreApplication::applicationDirPath();
        if (base.isEmpty()) base = QDir::currentPath();
        const QString dir = QDir(base).filePath(QStringLiteral("data"));
        QDir().mkpath(dir);
        return dir;
    }();
    return d;
}

// A file directly inside data/ (e.g. "checkmark.png", "model_render.guard").
inline QString file(const QString& name) { return QDir(dataDir()).filePath(name); }

// Delete superseded versions of a versioned cache. Caches are versioned in the FILENAME (see
// util/CacheVersioning.h) so an old build's file can never be read by a new one — but nothing was
// deleting them, so every bump left its predecessor behind forever. back_trophy_v1..v4 were all
// still on disk, and appearance_meta is 2 MB and icon_index 3 MB per version.
//
// Call with the stem and the CURRENT version, e.g. pruneOldCaches("back_trophy_v", 4, ".json").
// Only files matching <stem><digits><ext> with a LOWER number are removed — an unrelated file that
// happens to share the prefix cannot match, and the current one is never touched.
inline void pruneOldCaches(const QString& stem, int currentVersion, const QString& ext)
{
    const QDir d(dataDir());
    const QRegularExpression re(QStringLiteral("^%1(\\d+)%2$")
                                    .arg(QRegularExpression::escape(stem),
                                         QRegularExpression::escape(ext)));
    for (const QString& fn : d.entryList(QDir::Files)) {
        const QRegularExpressionMatch m = re.match(fn);
        if (!m.hasMatch()) continue;
        const int v = m.captured(1).toInt();
        if (v > 0 && v < currentVersion && QFile::remove(d.filePath(fn)))
            qInfo().noquote() << "cache: removed superseded" << fn;
    }
}

// A subdirectory inside data/ (created), e.g. "model_thumbs", "index_cache".
inline QString subDir(const QString& name)
{
    const QString d = QDir(dataDir()).filePath(name);
    QDir().mkpath(d);
    return d;
}

} // namespace AppPaths
