#pragma once
// Shared "Yaw / Pitch" rows for every Camera popup (Models, Wardrobe, Stable).
//
// Why a shared builder rather than three copies: convention 6 in the codebase notes — panels that
// exist in more than one tab come from one place, so the three cannot drift in range, labels,
// clamping or wiring. It is header-only for the same reason PanelPersist and ViewportPartMenu are:
// it owns no state, only widget construction.
//
// The pair is a slider (coarse drag) plus a spin box (exact number). Both edit the same value; the
// spin box is what makes the control precise, which a 1-pixel-per-degree slider cannot be.
//
// Saving is deliberately NOT re-implemented here. Yaw and pitch are already part of the camera the
// tabs persist (`wardrobe2/cam/*`, `stable2/cam/*`) and of the Camera presets every panel already
// offers, so a separate key for the same state would be exactly the duplicate-key bug convention 2
// warns about. Move the sliders, save a preset — the angle is in it.

#include "gl/GLModelWidget.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <functional>

namespace CameraOrbit {

// Radians <-> degrees, kept local so the panels never do the conversion themselves.
inline double toDeg(float rad) { return double(rad) * 180.0 / 3.14159265358979323846; }
inline float  toRad(double deg) { return float(deg * 3.14159265358979323846 / 180.0); }

// Wrap into (-180, 180] so a camera that has been orbited several turns still lands inside the
// slider's range instead of pinning to an end stop. Setting the wrapped value back is a no-op for
// the camera — it differs from the raw angle only by whole turns.
inline double wrapDeg(double d)
{
    d = std::fmod(d + 180.0, 360.0);
    if (d < 0.0) d += 360.0;
    return d - 180.0;
}

// Append the two rows to `lay`. Returns a callable that re-reads the viewport into the widgets —
// call it when the panel is shown.
//
// `autoSpin` is the panel's own "Auto-rotate (turntable)" checkbox, or nullptr. Editing an angle by
// hand unticks it, because the turntable rewrites yaw every 33 ms and the control would otherwise
// look broken — the value would spring back before the user let go of the slider. Unticking goes
// through the checkbox rather than calling setAutoSpin directly so the box, the QSettings key and
// the viewport cannot disagree.
//
// The widgets also poll while visible. An earlier version synced only on show, reasoning that a
// Qt::Popup grabs the mouse so the camera cannot move behind it — but the View angle presets, the
// turntable and any in-flight camera glide all live in or run under that same popup, and every one
// of them moves the camera while it is open. Polling is what keeps the numbers honest, and it costs
// two float reads at 8 Hz.
inline std::function<void()> addRows(QWidget* parent, QVBoxLayout* lay, GLModelWidget* view,
                                     QCheckBox* autoSpin = nullptr)
{
    auto* hdr = new QLabel(QStringLiteral("Rotation"), parent);
    hdr->setStyleSheet(QStringLiteral("color:#aaa;"));
    lay->addWidget(hdr);

    auto* yawS   = new QSlider(Qt::Horizontal, parent);
    auto* yawB   = new QDoubleSpinBox(parent);
    auto* pitchS = new QSlider(Qt::Horizontal, parent);
    auto* pitchB = new QDoubleSpinBox(parent);

    yawS->setRange(-180, 180);
    yawB->setRange(-180.0, 180.0);
    // Pitch stops just inside the viewport's own ~89 degree gimbal guard, so the control cannot ask
    // for an angle the camera will silently refuse.
    pitchS->setRange(-89, 89);
    pitchB->setRange(-89.0, 89.0);
    for (QDoubleSpinBox* b : {yawB, pitchB}) {
        b->setDecimals(1);
        b->setSingleStep(1.0);
        b->setSuffix(QStringLiteral("°"));
        b->setKeyboardTracking(false);   // apply on commit, not on every keystroke of "-1 2 0"
        b->setFixedWidth(72);
    }
    yawS->setToolTip(QStringLiteral("Orbit angle around the model. Type an exact value in the box."));
    pitchS->setToolTip(QStringLiteral("Camera height angle. Positive looks down from above; "
                                      "negative looks up from below."));
    yawB->setToolTip(yawS->toolTip());
    pitchB->setToolTip(pitchS->toolTip());

    auto row = [&](const QString& text, QSlider* s, QDoubleSpinBox* b) {
        auto* r = new QHBoxLayout();
        auto* l = new QLabel(text, parent);
        l->setFixedWidth(38);
        r->addWidget(l);
        r->addWidget(s, 1);
        r->addWidget(b);
        lay->addLayout(r);
    };
    row(QStringLiteral("Yaw"),   yawS,   yawB);
    row(QStringLiteral("Pitch"), pitchS, pitchB);

    QPointer<GLModelWidget> v(view);
    QPointer<QCheckBox>     spin(autoSpin);

    // Push both boxes' current values at the viewport. Signals are blocked on the widget being
    // mirrored, so slider -> box -> slider cannot loop.
    auto apply = [v, spin, yawB, pitchB]() {
        if (!v) return;
        if (spin && spin->isChecked()) spin->setChecked(false);   // hand control to the user
        v->setOrbitAngles(toRad(yawB->value()), toRad(pitchB->value()));
    };

    QObject::connect(yawS, &QSlider::valueChanged, parent, [yawB, apply](int val) {
        QSignalBlocker blk(yawB);
        yawB->setValue(double(val));
        apply();
    });
    QObject::connect(yawB, &QDoubleSpinBox::valueChanged, parent, [yawS, apply](double val) {
        QSignalBlocker blk(yawS);
        yawS->setValue(int(std::lround(val)));
        apply();
    });
    QObject::connect(pitchS, &QSlider::valueChanged, parent, [pitchB, apply](int val) {
        QSignalBlocker blk(pitchB);
        pitchB->setValue(double(val));
        apply();
    });
    QObject::connect(pitchB, &QDoubleSpinBox::valueChanged, parent, [pitchS, apply](double val) {
        QSignalBlocker blk(pitchS);
        pitchS->setValue(int(std::lround(val)));
        apply();
    });

    auto sync = [v, yawS, yawB, pitchS, pitchB]() {
        if (!v) return;
        // Never fight the user: a slider being dragged, or a spin box mid-edit, owns the value.
        if (yawS->isSliderDown() || pitchS->isSliderDown()) return;
        if (yawB->hasFocus() || pitchB->hasFocus()) return;
        const double y = wrapDeg(toDeg(v->camYaw()));
        const double p = toDeg(v->camPitch());
        QSignalBlocker b1(yawS), b2(yawB), b3(pitchS), b4(pitchB);
        yawS->setValue(int(std::lround(y)));
        yawB->setValue(y);
        pitchS->setValue(int(std::lround(p)));
        pitchB->setValue(p);
    };

    auto* poll = new QTimer(parent);
    poll->setInterval(120);
    QObject::connect(poll, &QTimer::timeout, parent, [parent, sync]() {
        if (parent->isVisible()) sync();
    });
    poll->start();

    return sync;
}

}  // namespace CameraOrbit
