#pragma once
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QClipboard>
#include <QHash>
#include <QImage>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>

#include "casc/CascReader.h"
#include "index/AppearanceMeta.h"
#include "index/IconIndex.h"
// MenuText — the shared context-menu vocabulary. Comment above, not trailing: verify-src matches
// the include directive to end-of-line.
#include "util/ViewportPartMenu.h"

// The inventory icon for an appearance sno, and the two context-menu actions that go with it.
//
// Nothing here is wardrobe-specific — it is sno -> AppearanceMeta::iconFor -> IconIndex — but it
// lived as a file-static inside WardrobeTab2.cpp, so the Stable tab's cards had no way to reach it
// and simply went without. Shared here so a second copy never gets written: the two tabs show the
// same kind of card for the same kind of asset.
namespace LookIcon {

inline QImage image(int sno, CascReader* reader)
{
    if (sno <= 0 || !reader) return QImage();
    if (!AppearanceMeta::instance().ready() || !IconIndex::instance().ready()) return QImage();
    static QHash<int, QImage> cache;   // sno -> icon is stable once the indexes are ready
    const auto it = cache.constFind(sno);
    if (it != cache.constEnd()) return *it;
    const quint32 h = AppearanceMeta::instance().iconFor(sno);
    QImage img = h ? IconIndex::instance().iconImage(h, reader) : QImage();
    if (!img.isNull()) cache.insert(sno, img);   // don't cache misses — retry on the next refill
    return img;
}

// Copy image / Save image… — disabled rather than hidden when there is no icon, so the menu keeps
// a stable shape and the absence reads as "this item has no icon" instead of "this menu is
// different". Same wording and order as the Models tab's icon-grid menu.
// Takes the ICON, not an sno, deliberately. The two tabs resolve icons differently and must keep
// doing so: Stable's slotIcon() consults its own mount/barding/trophy/pet crawl (m_iconByApp)
// before falling back to AppearanceMeta, so resolving here via image() would hand it a wrong or
// empty icon for precisely the content it shows. What is worth sharing is the pair of actions —
// wording, order, and the disabled-not-hidden behaviour — not the lookup.
inline void addActions(QMenu& menu, QWidget* parent, const QImage& icon, const QString& fullName)
{
    menu.addSeparator();
    QAction* aCopy = menu.addAction(MenuText::kCopyImage, parent,
                                    [icon] { QGuiApplication::clipboard()->setImage(icon); });
    aCopy->setEnabled(!icon.isNull());
    QAction* aSave = menu.addAction(MenuText::kSaveImage, parent, [parent, icon, fullName] {
        QSettings st;
        QString dir = st.value(QStringLiteral("export/captureDir")).toString();
        if (dir.isEmpty()) dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        const QString f = QFileDialog::getSaveFileName(parent, QObject::tr("Save icon"),
            QDir(dir).filePath(fullName + QStringLiteral(".png")),
            QStringLiteral("PNG image (*.png)"));
        if (f.isEmpty()) return;
        st.setValue(QStringLiteral("export/captureDir"), QFileInfo(f).absolutePath());
        icon.save(f);
    });
    aSave->setEnabled(!icon.isNull());
}

}  // namespace LookIcon
