#pragma once

#include <QColor>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QWidget>

#include <cmath>
#include <functional>

// Lightweight HSV colour wheel (hue=angle, saturation=radius) with a value bar below.
// Header-only, no Q_OBJECT/moc — it reports changes through the onChanged callback.
// Shared by the Models tab and the Wardrobe 2 "Set Pigment" picker.
class DyeColorWheel : public QWidget {
public:
    explicit DyeColorWheel(QWidget* parent = nullptr) : QWidget(parent) { setMinimumSize(150, 170); }
    std::function<void(const QColor&)> onChanged;
    void setColor(const QColor& c) {
        const QColor h = c.toHsv();
        float hue = float(h.hsvHueF()); if (hue < 0) hue = 0;
        m_h = hue; m_s = float(h.hsvSaturationF()); m_v = float(h.valueF());
        update();
    }
    QColor color() const {
        return QColor::fromHsvF(qBound(0.0f, m_h, 1.0f), qBound(0.0f, m_s, 1.0f),
                                qBound(0.0f, m_v, 1.0f));
    }
protected:
    QSize sizeHint() const override { return QSize(160, 180); }
    void paintEvent(QPaintEvent*) override {
        static const float kPi = 3.14159265358979f;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const int wheel = qMax(8, qMin(width(), height() - 18));
        const float cx = wheel / 2.0f, cy = wheel / 2.0f, rad = wheel / 2.0f - 1.0f;
        QImage img(wheel, wheel, QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        for (int y = 0; y < wheel; ++y) {
            auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < wheel; ++x) {
                const float dx = x - cx, dy = y - cy;
                const float r = std::sqrt(dx * dx + dy * dy);
                if (r <= rad) {
                    float ang = std::atan2(dy, dx) / (2.0f * kPi);
                    if (ang < 0) ang += 1.0f;
                    line[x] = QColor::fromHsvF(ang, qMin(1.0f, r / rad), m_v).rgb();
                }
            }
        }
        p.drawImage(0, 0, img);
        const float ang = m_h * 2.0f * kPi, mr = m_s * rad;
        const QPointF mk(cx + std::cos(ang) * mr, cy + std::sin(ang) * mr);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Qt::black, 3)); p.drawEllipse(mk, 4, 4);
        p.setPen(QPen(Qt::white, 1.5)); p.drawEllipse(mk, 4, 4);
        const int by = wheel + 4, bh = 12;
        QLinearGradient g(0, by, wheel, by);
        g.setColorAt(0, QColor::fromHsvF(m_h, m_s, 0.0));
        g.setColorAt(1, QColor::fromHsvF(m_h, m_s, 1.0));
        p.setPen(QPen(QColor(0x88, 0x88, 0x88), 1)); p.setBrush(g);
        p.drawRect(0, by, wheel - 1, bh);
        const int vx = int(m_v * (wheel - 1));
        p.setPen(QPen(Qt::white, 2)); p.drawLine(vx, by, vx, by + bh);
    }
    void mousePressEvent(QMouseEvent* e) override {
        // Lock onto whichever control the press started in, so dragging off the wheel
        // onto the value bar (or vice-versa) doesn't hijack the other one.
        const int wheel = qMax(8, qMin(width(), height() - 18));
        m_drag = (e->pos().y() < wheel) ? 1 : 2;
        pick(e->pos());
    }
    void mouseMoveEvent(QMouseEvent* e) override { if (e->buttons() & Qt::LeftButton) pick(e->pos()); }
    void mouseReleaseEvent(QMouseEvent*) override { m_drag = 0; }
private:
    void pick(const QPoint& pos) {
        static const float kPi = 3.14159265358979f;
        const int wheel = qMax(8, qMin(width(), height() - 18));
        const float cx = wheel / 2.0f, cy = wheel / 2.0f, rad = wheel / 2.0f - 1.0f;
        if (m_drag == 2) {   // value bar
            m_v = qBound(0.0f, float(pos.x()) / float(qMax(1, wheel - 1)), 1.0f);
        } else {             // hue/saturation wheel
            const float dx = pos.x() - cx, dy = pos.y() - cy;
            float ang = std::atan2(dy, dx) / (2.0f * kPi);
            if (ang < 0) ang += 1.0f;
            m_h = ang;
            m_s = qBound(0.0f, std::sqrt(dx * dx + dy * dy) / rad, 1.0f);
        }
        update();
        if (onChanged) onChanged(color());
    }
    int   m_drag = 0;   // 0 = none, 1 = wheel, 2 = value bar
    float m_h = 0.0f, m_s = 0.0f, m_v = 1.0f;
};
