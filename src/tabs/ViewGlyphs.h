#pragma once
// ── Painter-drawn viewport-toolbar glyphs, shared by the Models and Wardrobe tabs ────────────
// Blender-style: the four shading spheres and the two-circle Overlays toggle. Header-only so
// both toolbars draw from the same source — a tweak to a ball retunes both tabs at once.

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRadialGradient>
#include <cmath>

#include "ModelOutliner.h"   // kindIcon — the N-strip reuses the outliner glyph set where it fits

// Blender's shading spheres — 0 wireframe · 1 flat/solid · 2 shaded · 3 rendered.
// 20px canvas: at bar height anything smaller reads as a featureless dot.
inline QPixmap shadeBallGlyph(int mode)
{
    QPixmap pm(20, 20);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF c(10.0, 10.0);
    const double r = 8.4;
    if (mode == 0) {          // wire sphere: outline + equator + meridian
        p.setPen(QPen(QColor(210, 200, 165), 1.3));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, r, r);
        p.drawEllipse(c, r, r * 0.42);
        p.drawEllipse(c, r * 0.42, r);
    } else if (mode == 1) {   // flat: plain matte ball
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xa8, 0xa8, 0xa8));
        p.drawEllipse(c, r, r);
    } else if (mode == 2) {   // shaded: soft-lit ball
        QRadialGradient g(QPointF(7.0, 6.8), 13.0);
        g.setColorAt(0.0, QColor(0xe4, 0xe4, 0xe4));
        g.setColorAt(1.0, QColor(0x4a, 0x4a, 0x50));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(c, r, r);
    } else {                  // rendered: warm glossy ball + specular hit
        QRadialGradient g(QPointF(7.0, 6.8), 13.0);
        g.setColorAt(0.0, QColor(0xff, 0xf4, 0xd8));
        g.setColorAt(0.45, QColor(0xe0, 0xa8, 0x3e));
        g.setColorAt(1.0, QColor(0x4a, 0x33, 0x18));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(c, r, r);
        p.setBrush(QColor(255, 255, 255, 235));
        p.drawEllipse(QPointF(7.0, 6.4), 1.9, 1.9);
    }
    return pm;
}

// N-strip button glyphs — the settings popovers that live as icon buttons on the viewport's
// right edge (below the axis gizmo). 0 Graphics · 1 Pigment · 2 Camera · 3 Lighting ·
// 4 Shaders · 5 Detail maps · 6 Rig · 7 Physics. Outliner glyphs are reused where a kind
// matches, so the strip and the outliner speak the same icon language.
inline QPixmap stripGlyph(int k)
{
    switch (k) {
    case 0: return ModelOutlinerModel::kindIcon(ModelOutlinerModel::ValueGroup);  // Graphics: sliders
    case 4: return ModelOutlinerModel::kindIcon(ModelOutlinerModel::Shader);      // Shaders: hexagon
    case 5: return ModelOutlinerModel::kindIcon(ModelOutlinerModel::TexGroup);    // Detail: checker
    case 6: return ModelOutlinerModel::kindIcon(ModelOutlinerModel::Bone);        // Rig: bone
    default: break;
    }
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c(200, 190, 150);
    if (k == 1) {          // Pigment: droplet
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(150, 100, 200));
        QPainterPath d;
        d.moveTo(7.0, 1.5);
        d.cubicTo(10.5, 6.0, 11.5, 8.0, 11.5, 9.5);
        d.arcTo(QRectF(2.5, 5.0, 9.0, 9.0), 0, -180);
        d.cubicTo(2.5, 8.0, 3.5, 6.0, 7.0, 1.5);
        p.drawPath(d);
    } else if (k == 2) {   // Camera
        p.setPen(QPen(c, 1.3));
        p.drawRoundedRect(QRectF(1.5, 4.0, 11.0, 7.5), 1.5, 1.5);
        p.drawEllipse(QPointF(7.0, 7.8), 2.3, 2.3);
        p.drawLine(QPointF(4.0, 4.0), QPointF(5.5, 2.0));
        p.drawLine(QPointF(5.5, 2.0), QPointF(8.5, 2.0));
        p.drawLine(QPointF(8.5, 2.0), QPointF(10.0, 4.0));
    } else if (k == 3) {   // Lighting: sun
        p.setPen(QPen(QColor(230, 200, 110), 1.3));
        p.drawEllipse(QPointF(7, 7), 3.0, 3.0);
        for (int i = 0; i < 8; ++i) {
            const double a = i * 3.14159265 / 4.0;
            p.drawLine(QPointF(7 + 4.4 * std::cos(a), 7 + 4.4 * std::sin(a)),
                       QPointF(7 + 6.2 * std::cos(a), 7 + 6.2 * std::sin(a)));
        }
    } else if (k == 7) {   // Physics: spring wave
        p.setPen(QPen(QColor(120, 190, 220), 1.5, Qt::SolidLine, Qt::RoundCap));
        QPainterPath w;
        w.moveTo(1.5, 7.0);
        w.cubicTo(4.0, 1.5, 6.0, 12.5, 8.5, 7.0);
        w.cubicTo(10.0, 3.5, 11.5, 10.5, 12.5, 7.0);
        p.drawPath(w);
    }
    return pm;
}

// Blender's Overlays toggle: two overlapping circles (20px to match the shading balls).
inline QPixmap overlayGlyph()
{
    QPixmap g(20, 20);
    g.fill(Qt::transparent);
    QPainter p(&g);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(215, 210, 195), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(7.6, 10.0), 5.6, 5.6);
    p.drawEllipse(QPointF(12.4, 10.0), 5.6, 5.6);
    return g;
}
