#pragma once
// Central definition of the user-rebindable export/preview shortcuts. Shared by the
// Settings ▸ Hotkeys editor (which writes the QSettings values) and MainWindow (which
// reads them and applies each as a QAction shortcut). Header-only so both translation
// units share one source of truth without an extra build entry.
#include <QKeySequence>
#include <QSettings>
#include <QString>
#include <QVector>

namespace Hotkeys {

struct Def {
    QString key;    // QSettings key
    QString label;  // shown in the Settings editor
    QString def;    // default key sequence (portable text; empty = unbound)
};

inline QVector<Def> defs()
{
    return {
        { QStringLiteral("hotkeys/exportSelection"),  QStringLiteral("Export selection…"),        QStringLiteral("Ctrl+E") },
        { QStringLiteral("hotkeys/exportToLast"),     QStringLiteral("Export to last dir"),       QStringLiteral("Ctrl+Shift+E") },
        { QStringLiteral("hotkeys/exportAnimations"), QStringLiteral("Export animations only…"),  QStringLiteral("Ctrl+Shift+A") },
        { QStringLiteral("hotkeys/saveImage"),        QStringLiteral("Save preview image…"),      QStringLiteral("Ctrl+Shift+I") },
        { QStringLiteral("hotkeys/turntable"),        QStringLiteral("Turntable GIF…"),           QString() },
        { QStringLiteral("hotkeys/animLoop"),         QStringLiteral("Animation-loop GIF…"),      QString() },
    };
}

// Resolve one shortcut from settings (falling back to its default). Empty = unbound.
inline QKeySequence seq(const QString& key, const QString& def)
{
    const QString s = QSettings().value(key, def).toString();
    return s.isEmpty() ? QKeySequence() : QKeySequence(s);
}

} // namespace Hotkeys
