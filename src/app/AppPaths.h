#pragma once
// Portable, self-contained storage. Everything the tool writes (settings INI, caches, thumbnails,
// logs, guards) lives in a "data" folder beside the executable — no Windows registry, no %AppData%.
// Copy the release folder anywhere (or a USB stick) and it runs and remembers its state, leaving zero
// traces on the host machine. main() points QSettings at data/ so every QSettings() default-ctor call
// resolves here too.
#include <QCoreApplication>
#include <QDir>
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

// A subdirectory inside data/ (created), e.g. "model_thumbs", "index_cache".
inline QString subDir(const QString& name)
{
    const QString d = QDir(dataDir()).filePath(name);
    QDir().mkpath(d);
    return d;
}

} // namespace AppPaths
