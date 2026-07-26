#pragma once
// Small corner badge painted over an item icon to show whether the item actually has a
// renderable MODEL behind its (baked 2D) icon: a green ✓ when the model is present, a red ✗
// when only the icon exists (no mesh payload). Each iconated tab enables the two indicators
// independently via per-tab settings, so a user can, e.g., flag ONLY the icon-without-model
// cases. Header-only so every tab (list delegates + QToolButton cards) shares one look.

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QRect>
#include <QRectF>
#include <QSettings>
#include <QString>

namespace IconBadge {

// state: +1 = model present (✓), -1 = no model (✗), 0 = unknown (draw nothing).
inline void paint(QPainter& p, const QRect& iconRect, int state, bool showPresent, bool showMissing)
{
    if (state == 0) return;
    if (state > 0 && !showPresent) return;
    if (state < 0 && !showMissing) return;
    const int d = qMax(11, iconRect.width() / 4);
    // Bottom-centre of the icon.
    const QRect b(iconRect.center().x() - d / 2, iconRect.bottom() - d, d, d);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(0, 0, 0, 170), qMax(1, d / 12)));
    p.setBrush(state > 0 ? QColor(46, 160, 67) : QColor(206, 62, 62));   // green / red
    p.drawEllipse(b);
    QPen gp(Qt::white, qMax(1.4, d / 7.0));
    gp.setCapStyle(Qt::RoundCap);
    gp.setJoinStyle(Qt::RoundJoin);
    p.setPen(gp);
    const QRectF g = QRectF(b).adjusted(d * 0.27, d * 0.27, -d * 0.27, -d * 0.27);
    if (state > 0) {
        p.drawPolyline(QPolygonF({ QPointF(g.left(), g.center().y()),
                                   QPointF(g.left() + g.width() * 0.38, g.bottom()),
                                   QPointF(g.right(), g.top()) }));
    } else {
        p.drawLine(g.topLeft(), g.bottomRight());
        p.drawLine(g.topRight(), g.bottomLeft());
    }
    p.restore();
}

// Composite a badge onto a copy of `icon` (for QToolButton card icons).
inline QPixmap withBadge(const QPixmap& icon, int state, bool showPresent, bool showMissing)
{
    if (icon.isNull() || state == 0) return icon;
    if (state > 0 && !showPresent) return icon;
    if (state < 0 && !showMissing) return icon;
    QPixmap out = icon;
    QPainter p(&out);
    paint(p, out.rect(), state, showPresent, showMissing);
    return out;
}

// Per-tab settings. `tab` = "models" / "textures" / "wardrobe" / "stable". Off by default.
inline bool showPresent(const QString& tab)
{
    return QSettings().value(QStringLiteral("icons/%1/showPresent").arg(tab), false).toBool();
}
inline bool showMissing(const QString& tab)
{
    return QSettings().value(QStringLiteral("icons/%1/showMissing").arg(tab), false).toBool();
}
// True when either indicator is enabled for the tab (skip the presence probe entirely otherwise).
inline bool anyEnabled(const QString& tab) { return showPresent(tab) || showMissing(tab); }

} // namespace IconBadge
