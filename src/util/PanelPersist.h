#pragma once
#include <QByteArray>
#include <QSettings>
#include <QSplitter>

// Splitter size / column-width persistence, gated by the global "Remember panel sizes" setting
// (Settings ▸ View). When enabled: restores the saved layout on bind() and re-saves on every user
// drag. When disabled: the splitter keeps its code-set default and nothing is written.
//
// Call bind() AFTER the splitter's default setSizes(), so the default stands when nothing has been
// remembered yet (an empty/missing blob leaves the sizes untouched).
namespace PanelPersist {

inline bool enabled()
{
    return QSettings().value(QStringLiteral("view/rememberPanels"), true).toBool();
}

inline void bind(QSplitter* s, const QString& key)
{
    if (!s) return;
    if (enabled()) {
        const QByteArray blob = QSettings().value(key).toByteArray();
        if (!blob.isEmpty()) s->restoreState(blob);
    }
    QObject::connect(s, &QSplitter::splitterMoved, s, [s, key](int, int) {
        if (enabled()) QSettings().setValue(key, s->saveState());
    });
}

}  // namespace PanelPersist
