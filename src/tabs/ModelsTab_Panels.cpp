// ModelsTab_Panels.cpp — viewport control popups for the Models tab, giving the
// single-model preview parity with the Wardrobe preview toolbar: Camera, Lighting,
// Shaders, Detail maps, and Physics popups, plus Fullscreen and the Developer-mode
// gating of the debug panels. These are ModelsTab member definitions living in a
// second translation unit (the class declaration is in ModelsTab.h).
//
// Adapted from WardrobeTab2_Panels.cpp. Differences vs the Wardrobe original:
//   • drives the Models viewport (m_modelView) and its own models/* settings keys,
//     so the two tabs' viewport settings are independent;
//   • the Camera panel omits the outfit-slot–specific rows (Camera Snap / Follow /
//     hover-snap / snap-margin) that have no meaning for a single model;
//   • the Physics panel omits the "Use game cloth data" path (there's no equipped-
//     piece cloth tuning here) — the sliders always drive the sim directly.

#include "tabs/ModelsTab.h"

#include "gl/GLModelWidget.h"

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QCursor>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QClipboard>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>

// Position a settings panel to the LEFT of its N-strip button, over the viewport (Blender's
// N-panel side). Clamped to the window/screen so tall panels never run off-screen.
QPoint ModelsTab::panelPosLeftOf(QWidget* anchor, const QSize& sz) const
{
    QPoint pos = anchor->mapToGlobal(QPoint(-sz.width() - 6, 0));
    QRect bound = window() ? window()->frameGeometry() : QRect();
    QScreen* scr = QGuiApplication::screenAt(anchor->mapToGlobal(QPoint(0, 0)));
    if (!scr) scr = QGuiApplication::primaryScreen();
    if (scr) bound = bound.isNull() ? scr->availableGeometry() : bound.intersected(scr->availableGeometry());
    if (!bound.isNull()) {
        pos.setX(qBound(bound.left(), pos.x(), qMax(bound.left(), bound.right()  - sz.width())));
        pos.setY(qBound(bound.top(),  pos.y(), qMax(bound.top(),  bound.bottom() - sz.height())));
    }
    return pos;
}

// ── Camera popup (FOV · view angles · turntable · projection · presets) ───────
void ModelsTab::toggleCameraPanel()
{
    if (m_camPanel && m_camPanel->isVisible()) { m_camPanel->hide(); return; }
    if (m_lightPanel) m_lightPanel->hide();   // one preview popup open at a time
    if (m_vpPanel)    m_vpPanel->hide();
    if (!m_camPanel) buildCameraPanel();
    m_camPanel->adjustSize();
    m_camPanel->move(panelPosLeftOf(m_camBtn, m_camPanel->sizeHint()));
    m_camPanel->show();
    m_camPanel->raise();
}
void ModelsTab::buildCameraPanel()
{
    if (m_camPanel) return;
    QSettings s;
    m_camPanel = new QFrame(this, Qt::Popup);
    m_camPanel->setObjectName(QStringLiteral("camPanel"));
    m_camPanel->setProperty("hoverBtn", QVariant::fromValue<QObject*>(m_camBtn));
    m_camPanel->installEventFilter(this);   // clear the opener button's stuck hover on close
    m_camPanel->setStyleSheet(QStringLiteral(
        "QFrame#camPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
        "QLabel{color:#cccccc;} QCheckBox{color:#cccccc;}"));
    auto* pl = new QVBoxLayout(m_camPanel);
    pl->setContentsMargins(12, 10, 12, 10);
    pl->setSpacing(6);
    auto* hdr = new QLabel(QStringLiteral("Camera"), m_camPanel);
    hdr->setStyleSheet(QLatin1String(kHdrQss));
    pl->addWidget(hdr);

    // Camera field-of-view.
    auto* fovRow = new QHBoxLayout();
    fovRow->addWidget(new QLabel(QStringLiteral("FOV"), m_camPanel));
    m_fovSlider = new QSlider(Qt::Horizontal, m_camPanel);
    m_fovSlider->setRange(10, 100);
    m_fovSlider->setValue(s.value(QStringLiteral("models/fov"), 45).toInt());
    m_fovSlider->setToolTip(QStringLiteral("Camera field of view (degrees)"));
    connect(m_fovSlider, &QSlider::valueChanged, this, [this](int v) {
        QSettings().setValue(QStringLiteral("models/fov"), v);
        if (m_modelView) m_modelView->setFov(float(v));
    });
    fovRow->addWidget(m_fovSlider, 1);
    pl->addLayout(fovRow);

    // View-angle presets: orbit to a fixed angle around the model (keeps current zoom).
    pl->addWidget(new QLabel(QStringLiteral("View angle"), m_camPanel));
    auto* presetRow = new QHBoxLayout();
    presetRow->setSpacing(3);
    auto mkPreset = [&](const QString& text, float yaw, float pitch) {
        auto* b = new QPushButton(text, m_camPanel);
        b->setToolTip(QStringLiteral("Orbit to the %1 view").arg(text.toLower()));
        connect(b, &QPushButton::clicked, this, [this, yaw, pitch] {
            if (!m_modelView) return;
            m_modelView->followParts(QVector<int>{});   // a fixed angle around the whole model
            m_modelView->frameThreeQuarter(yaw, pitch, 0.12f);
        });
        presetRow->addWidget(b);
    };
    mkPreset(QStringLiteral("¾"),     0.9708f,  0.12f);
    mkPreset(QStringLiteral("Front"), 1.5708f,  0.05f);
    mkPreset(QStringLiteral("Back"), -1.5708f,  0.05f);
    mkPreset(QStringLiteral("Left"),  0.0f,     0.05f);
    mkPreset(QStringLiteral("Right"), 3.14159f, 0.05f);
    pl->addLayout(presetRow);

    auto* fullBtn = new QPushButton(QStringLiteral("Frame full model  (F)"), m_camPanel);
    fullBtn->setToolTip(QStringLiteral("Re-fit the camera to the whole (visible) model — ignores hidden "
                                       "parts — keeping your current angle."));
    connect(fullBtn, &QPushButton::clicked, this, [this] {
        if (m_modelView) m_modelView->frameAll(/*keepRotation=*/true);
    });
    pl->addWidget(fullBtn);

    // (The toolbar's old Spin toggle is gone — the turntable checkbox + speed slider further
    // down this panel were ALWAYS the richer control; they're the single home now.)

    // Whether double-clicking a part also snaps the camera to it (Blender's "Frame Selected").
    // Global key — GLModelWidget reads it live at pick time, so every viewport obeys the same
    // switch and selection sync (outliner highlight) keeps working regardless.
    auto* frameChk = new QCheckBox(QStringLiteral("Frame part on select"), m_camPanel);
    frameChk->setToolTip(QStringLiteral("Double-clicking a part in the viewport also zooms/centres the "
                                        "camera on it. Off = double-click only selects."));
    frameChk->setChecked(s.value(QStringLiteral("viewer/framePartOnPick"), true).toBool());
    connect(frameChk, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("viewer/framePartOnPick"), on);
    });
    pl->addWidget(frameChk);

    // Auto-frame the model on load: re-fit centre + zoom to the visible geometry while keeping the
    // orbit angle. Off = keep the exact camera (angle AND zoom/position) between models.
    auto* autoFrameChk = new QCheckBox(QStringLiteral("Auto-frame model on load (keep rotation)"), m_camPanel);
    autoFrameChk->setChecked(s.value(QStringLiteral("models/autoFrame"), true).toBool());
    autoFrameChk->setToolTip(QStringLiteral("When on, loading a model re-fits the camera to the visible "
                                            "geometry (hidden parts ignored) but keeps your viewing angle."));
    connect(autoFrameChk, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("models/autoFrame"), on);
    });
    pl->addWidget(autoFrameChk);

    // Auto-rotate turntable + speed.
    auto* spinChk = new QCheckBox(QStringLiteral("Auto-rotate (turntable)"), m_camPanel);
    spinChk->setChecked(s.value(QStringLiteral("models/turntable"), false).toBool());
    spinChk->setToolTip(QStringLiteral("Slowly rotate the model for a showcase view"));
    auto* spinRow = new QHBoxLayout();
    spinRow->addWidget(new QLabel(QStringLiteral("Speed"), m_camPanel));
    auto* spinSpeed = new QSlider(Qt::Horizontal, m_camPanel);
    spinSpeed->setRange(1, 100);
    spinSpeed->setValue(qBound(1, s.value(QStringLiteral("models/turntableSpeed"), 25).toInt(), 100));
    spinSpeed->setToolTip(QStringLiteral("Turntable rotation speed"));
    spinRow->addWidget(spinSpeed, 1);
    connect(spinChk, &QCheckBox::toggled, this, [this, spinSpeed](bool on) {
        QSettings().setValue(QStringLiteral("models/turntable"), on);
        if (!m_modelView) return;
        m_modelView->setSpinSpeed(float(spinSpeed->value()) / 1000.0f);
        m_modelView->setAutoSpin(on);
    });
    connect(spinSpeed, &QSlider::valueChanged, this, [this](int v) {
        QSettings().setValue(QStringLiteral("models/turntableSpeed"), v);
        if (m_modelView) m_modelView->setSpinSpeed(float(v) / 1000.0f);
    });
    pl->addWidget(spinChk);
    pl->addLayout(spinRow);

    // Orthographic vs perspective projection.
    auto* orthoChk = new QCheckBox(QStringLiteral("Orthographic projection"), m_camPanel);
    orthoChk->setChecked(s.value(QStringLiteral("models/ortho"), false).toBool());
    orthoChk->setToolTip(QStringLiteral("Flat, no-perspective projection — good for straight-on reference shots."));
    connect(orthoChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("models/ortho"), on);
        if (m_modelView) m_modelView->setOrthographic(on);
    });
    pl->addWidget(orthoChk);

    // Camera presets: three slots that store the current camera (angle/zoom/FOV/projection).
    auto* presetHdr = new QLabel(QStringLiteral("Camera presets"), m_camPanel);
    presetHdr->setStyleSheet(QStringLiteral("color:#aaa;"));
    pl->addWidget(presetHdr);
    for (int n = 1; n <= 3; ++n) {
        const QString key = QStringLiteral("models/campreset/%1/").arg(n);
        auto* prow = new QHBoxLayout(); prow->setSpacing(3);
        auto* loadBtn = new QPushButton(QStringLiteral("Preset %1").arg(n), m_camPanel);
        loadBtn->setToolTip(QStringLiteral("Load this saved camera (angle · zoom · FOV · projection)"));
        loadBtn->setEnabled(s.value(key + QStringLiteral("set"), false).toBool());
        auto* saveBtn = new QPushButton(QStringLiteral("Save"), m_camPanel);
        saveBtn->setToolTip(QStringLiteral("Overwrite this preset with the current camera"));
        connect(saveBtn, &QPushButton::clicked, this, [this, n, key, loadBtn] {
            if (!m_modelView) return;
            const GLModelWidget::CamState c = m_modelView->cameraState();
            QSettings st;
            st.setValue(key + QStringLiteral("yaw"), c.yaw);   st.setValue(key + QStringLiteral("pitch"), c.pitch);
            st.setValue(key + QStringLiteral("dist"), c.dist); st.setValue(key + QStringLiteral("fov"), c.fov);
            st.setValue(key + QStringLiteral("cx"), c.cx);     st.setValue(key + QStringLiteral("cy"), c.cy);
            st.setValue(key + QStringLiteral("cz"), c.cz);     st.setValue(key + QStringLiteral("ortho"), c.ortho);
            st.setValue(key + QStringLiteral("set"), true);
            loadBtn->setEnabled(true);
        });
        connect(loadBtn, &QPushButton::clicked, this, [this, key] {
            QSettings st;
            if (!m_modelView || !st.value(key + QStringLiteral("set"), false).toBool()) return;
            GLModelWidget::CamState c;
            c.yaw   = st.value(key + QStringLiteral("yaw"),   c.yaw).toFloat();
            c.pitch = st.value(key + QStringLiteral("pitch"), c.pitch).toFloat();
            c.dist  = st.value(key + QStringLiteral("dist"),  c.dist).toFloat();
            c.fov   = st.value(key + QStringLiteral("fov"),   c.fov).toFloat();
            c.cx    = st.value(key + QStringLiteral("cx"), 0.0).toFloat();
            c.cy    = st.value(key + QStringLiteral("cy"), 0.0).toFloat();
            c.cz    = st.value(key + QStringLiteral("cz"), 0.0).toFloat();
            c.ortho = st.value(key + QStringLiteral("ortho"), false).toBool();
            c.valid = true;
            m_modelView->setCameraState(c);
            if (m_fovSlider) m_fovSlider->setValue(int(c.fov));
        });
        prow->addWidget(loadBtn, 1);
        prow->addWidget(saveBtn);
        pl->addLayout(prow);
    }
}

// ── Lighting popup (three-point rig + surface + shadows + AO + colour grade) ───
void ModelsTab::toggleLightingPanel()
{
    if (m_lightPanel && m_lightPanel->isVisible()) { m_lightPanel->hide(); return; }
    if (m_camPanel)   m_camPanel->hide();
    if (m_vpPanel)    m_vpPanel->hide();
    if (!m_lightPanel) buildLightingPanel();
    m_lightPanel->adjustSize();
    m_lightPanel->move(panelPosLeftOf(m_lightBtn, m_lightPanel->sizeHint()));
    m_lightPanel->show();
    m_lightPanel->raise();
}
void ModelsTab::buildLightingPanel()
{
    if (m_lightPanel) return;
    QSettings s;
    m_lightPanel = new QFrame(this, Qt::Popup);
    m_lightPanel->setObjectName(QStringLiteral("lightPanel"));
    m_lightPanel->setProperty("hoverBtn", QVariant::fromValue<QObject*>(m_lightBtn));
    m_lightPanel->installEventFilter(this);
    m_lightPanel->setStyleSheet(QStringLiteral(
        "QFrame#lightPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
        "QLabel{color:#cccccc;} QCheckBox{color:#cccccc;}"));
    auto* pl = new QVBoxLayout(m_lightPanel);
    pl->setContentsMargins(12, 10, 12, 10);
    pl->setSpacing(5);
    auto* hdr = new QLabel(QStringLiteral("Lighting"), m_lightPanel);
    hdr->setStyleSheet(QLatin1String(kHdrQss));
    pl->addWidget(hdr);
    auto* sub = new QLabel(QStringLiteral("Three-point rig — real D4 character-screen values"), m_lightPanel);
    sub->setStyleSheet(QStringLiteral("color:#888;"));
    pl->addWidget(sub);

    auto* preRow = new QHBoxLayout();
    preRow->addWidget(new QLabel(QStringLiteral("Preset"), m_lightPanel));
    auto* preset = new QComboBox(m_lightPanel);
    preset->addItems({QStringLiteral("D4 Wardrobe (campfire)"),
                      QStringLiteral("Hero Direct (neutral)"),
                      QStringLiteral("Studio (cool 3-point)")});
    preset->setCurrentIndex(s.value(QStringLiteral("models/light/preset"), 1).toInt());   // default: Hero Direct
    connect(preset, &QComboBox::currentIndexChanged, this, [this](int i) {
        QSettings().setValue(QStringLiteral("models/light/preset"), i);
        applyLightRig();
    });
    preRow->addWidget(preset, 1);
    pl->addLayout(preRow);

    auto* reflChk = new QCheckBox(QStringLiteral("Reflections (game probe)"), m_lightPanel);
    reflChk->setChecked(s.value(QStringLiteral("models/light/reflections"), true).toBool());
    reflChk->setToolTip(QStringLiteral(
        "Use Diablo IV's real character-screen reflection cubemap for metal/gloss reflections"));
    connect(reflChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("models/light/reflections"), on);
        if (m_modelView) m_modelView->setReflectionEnabled(on);
    });
    pl->addWidget(reflChk);

    auto* lockChk = new QCheckBox(QStringLiteral("Lock lights to world"), m_lightPanel);
    lockChk->setChecked(s.value(QStringLiteral("models/light/lock"), false).toBool());
    lockChk->setToolTip(QStringLiteral(
        "Off: three-point rig tracks the camera. On: pins the lights in world space at the current orbit."));
    connect(lockChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("models/light/lock"), on);
        if (m_modelView) m_modelView->setLightLock(on);
    });
    pl->addWidget(lockChk);

    struct SRow { QSlider* sl; int def; QString key; };
    QVector<SRow> rows;
    auto slider = [&](const QString& key, const QString& label, int lo, int hi, int def,
                      const QString& tip) {
        auto* row = new QHBoxLayout();
        auto* lbl = new QLabel(label, m_lightPanel);
        lbl->setMinimumWidth(64);
        lbl->setToolTip(tip);
        row->addWidget(lbl);
        auto* sl = new QSlider(Qt::Horizontal, m_lightPanel);
        sl->setRange(lo, hi);
        const int init = s.value(QStringLiteral("models/light/") + key, def).toInt();
        sl->setValue(init);
        auto* val = new QLabel(QString::number(init), m_lightPanel);
        val->setMinimumWidth(30);
        val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        connect(sl, &QSlider::valueChanged, this, [this, key, val](int v) {
            QSettings().setValue(QStringLiteral("models/light/") + key, v);
            val->setText(QString::number(v));
            applyLightRig();
        });
        row->addWidget(sl, 1);
        row->addWidget(val);
        pl->addLayout(row);
        rows.append({sl, def, key});
    };
    auto section = [&](const QString& title) {
        auto* h = new QLabel(title, m_lightPanel);
        QFont f = h->font(); f.setBold(true); h->setFont(f);
        h->setStyleSheet(QStringLiteral("color:#e0a060; margin-top:7px;"));
        pl->addWidget(h);
    };
    section(QStringLiteral("Lights"));
    slider(QStringLiteral("key"),  QStringLiteral("Key %"),     0, 200, 100, QStringLiteral("Warm campfire key intensity"));
    slider(QStringLiteral("rim"),  QStringLiteral("Rim %"),     0, 200, 100, QStringLiteral("Cool back-rim intensity (edge separation)"));
    slider(QStringLiteral("fill"), QStringLiteral("Fill %"),    0, 200, 100, QStringLiteral("Cool front-fill intensity (shadow lift)"));
    slider(QStringLiteral("amb"),  QStringLiteral("Ambient %"), 0, 200, 100, QStringLiteral("Hemisphere-ambient (IBL) scale"));
    slider(QStringLiteral("exp"),  QStringLiteral("Exposure %"), 25, 300, 100, QStringLiteral("Overall exposure / brightness before tonemapping"));
    slider(QStringLiteral("az"),   QStringLiteral("Key L-R"),  -90,  90,  15, QStringLiteral("Key azimuth (degrees, + = camera-right)"));
    slider(QStringLiteral("el"),   QStringLiteral("Key U-D"),    0,  80,  25, QStringLiteral("Key elevation (degrees above the camera horizon)"));
    section(QStringLiteral("Surface"));
    slider(QStringLiteral("refl"),     QStringLiteral("Reflection %"), 0, 300, 100, QStringLiteral("Reflection / ambient-specular intensity (metal & gloss)"));
    slider(QStringLiteral("sss"),      QStringLiteral("Subsurface %"), 0, 200,  15, QStringLiteral("Skin subsurface scattering strength"));
    slider(QStringLiteral("skinwarm"), QStringLiteral("Skin warmth"),  0, 200, 100, QStringLiteral("Skin subsurface red-bleed hue"));
    slider(QStringLiteral("wetness"),  QStringLiteral("Wetness %"),    0, 100,   0, QStringLiteral("Rain-slick look; 0 = dry"));
    slider(QStringLiteral("snow"),     QStringLiteral("Snow %"),       0, 100,   0, QStringLiteral("Snow dusting on upward-facing surfaces; 0 = none"));
    slider(QStringLiteral("emis"),     QStringLiteral("Emissive %"),   0, 300,  50, QStringLiteral("Glow intensity of emissive armor (runes/gems)"));
    section(QStringLiteral("Shadows"));
    slider(QStringLiteral("shadowStr"),  QStringLiteral("Shadow %"),    0, 100,  60, QStringLiteral("Self-shadow darkness (key-light shadow map)"));
    slider(QStringLiteral("shadowSoft"), QStringLiteral("Shadow soft"), 0,  40,  15, QStringLiteral("Shadow edge softness (PCF radius, ÷10 texels)"));
    slider(QStringLiteral("shadowBias"), QStringLiteral("Shadow bias"), 0,  50,  18, QStringLiteral("Depth bias to avoid shadow acne (÷10000)"));
    slider(QStringLiteral("shadowNBias"), QStringLiteral("Shadow n-bias"), 0, 50, 10, QStringLiteral("Normal-offset bias (÷1000 of model size)"));
    slider(QStringLiteral("shadowRange"), QStringLiteral("Shadow range"), 100, 300, 130, QStringLiteral("Shadow frustum tightness (÷100)"));
    slider(QStringLiteral("shadowRes"),   QStringLiteral("Shadow res"),  1024, 4096, 2048, QStringLiteral("Shadow-map resolution"));
    section(QStringLiteral("Ambient occlusion"));
    slider(QStringLiteral("ssaoStr"),    QStringLiteral("Amb. occlusion %"), 0, 200, 100, QStringLiteral("Screen-space ambient occlusion darkness"));
    slider(QStringLiteral("ssaoRad"),    QStringLiteral("AO radius"),         5, 100,  30, QStringLiteral("SSAO sampling radius (÷100)"));
    section(QStringLiteral("Colour grade"));
    auto* gradeChk = new QCheckBox(QStringLiteral("Enable colour grade"), m_lightPanel);
    gradeChk->setToolTip(QStringLiteral("Post-tonemap contrast + saturation + split-tone (stylised)."));
    gradeChk->setChecked(s.value(QStringLiteral("models/light/grade"), false).toBool());
    connect(gradeChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("models/light/grade"), on); applyLightRig();
    });
    pl->addWidget(gradeChk);
    slider(QStringLiteral("gradeContrast"), QStringLiteral("Contrast"),  50, 200, 105, QStringLiteral("Contrast S-curve about mid-grey (÷100)."));
    slider(QStringLiteral("gradeSat"),      QStringLiteral("Saturation"), 0, 200, 110, QStringLiteral("Colour saturation (÷100)."));
    slider(QStringLiteral("gradeWarmth"),   QStringLiteral("Split-tone"), 0, 200,  30, QStringLiteral("Warm shadows / cool highlights strength (÷1000)."));

    const QVector<SRow> rowsCopy = rows;
    auto* btnRow = new QHBoxLayout();
    auto* dBtn = new QPushButton(QStringLiteral("Default"), m_lightPanel);
    auto* sBtn = new QPushButton(QStringLiteral("Save preset"), m_lightPanel);
    auto* rBtn = new QPushButton(QStringLiteral("Restore preset"), m_lightPanel);
    connect(dBtn, &QPushButton::clicked, this, [rowsCopy]{ for (const SRow& r : rowsCopy) r.sl->setValue(r.def); });
    connect(sBtn, &QPushButton::clicked, this, [rowsCopy]{
        QSettings q; for (const SRow& r : rowsCopy)
            q.setValue(QStringLiteral("models/preset/light/%1").arg(r.key), r.sl->value());
    });
    connect(rBtn, &QPushButton::clicked, this, [rowsCopy]{
        QSettings q; for (const SRow& r : rowsCopy)
            r.sl->setValue(q.value(QStringLiteral("models/preset/light/%1").arg(r.key), r.sl->value()).toInt());
    });
    btnRow->addWidget(dBtn); btnRow->addWidget(sBtn); btnRow->addWidget(rBtn);
    pl->addLayout(btnRow);
}
void ModelsTab::applyLightRig()
{
    if (!m_modelView) return;
    QSettings s;
    GLModelWidget::LightRig r;
    r.preset       = s.value(QStringLiteral("models/light/preset"), 1).toInt();   // default: Hero Direct
    r.keyInt       = s.value(QStringLiteral("models/light/key"),  100).toInt() / 100.0f;
    r.rimInt       = s.value(QStringLiteral("models/light/rim"),  100).toInt() / 100.0f;
    r.fillInt      = s.value(QStringLiteral("models/light/fill"), 100).toInt() / 100.0f;
    r.ambInt       = s.value(QStringLiteral("models/light/amb"),  100).toInt() / 100.0f;
    r.keyAzimuth   = float(s.value(QStringLiteral("models/light/az"), 15).toInt());
    r.keyElevation = float(s.value(QStringLiteral("models/light/el"), 25).toInt());
    m_modelView->setLightRig(r);
    m_modelView->setReflectionStrength(s.value(QStringLiteral("models/light/refl"),     100).toInt() / 100.0f);
    m_modelView->setSkinWarmth(        s.value(QStringLiteral("models/light/skinwarm"), 100).toInt() / 100.0f);
    m_modelView->setSssStrength(       s.value(QStringLiteral("models/light/sss"),       24).toInt() / 100.0f);
    m_modelView->setWetness(           s.value(QStringLiteral("models/light/wetness"),    0).toInt() / 100.0f);
    m_modelView->setSnow(              s.value(QStringLiteral("models/light/snow"),       0).toInt() / 100.0f);
    m_modelView->setEmissiveScale(     s.value(QStringLiteral("models/light/emis"),      50).toInt() / 100.0f);
    m_modelView->setShadowParams(      s.value(QStringLiteral("models/light/shadowStr"),  60).toInt() / 100.0f,
                                       s.value(QStringLiteral("models/light/shadowSoft"), 15).toInt() / 10.0f,
                                       s.value(QStringLiteral("models/light/shadowBias"), 18).toInt() / 10000.0f);
    m_modelView->setShadowExtra(       s.value(QStringLiteral("models/light/shadowRange"), 130).toInt() / 100.0f,
                                       s.value(QStringLiteral("models/light/shadowNBias"),  10).toInt() / 1000.0f,
                                       s.value(QStringLiteral("models/light/shadowRes"),  2048).toInt());
    m_modelView->setLightLock(         s.value(QStringLiteral("models/light/lock"), false).toBool());
    m_modelView->setExposure(          s.value(QStringLiteral("models/light/exp"),        100).toInt() / 100.0f);
    m_modelView->setColorGrade(        s.value(QStringLiteral("models/light/grade"),     false).toBool(),
                                       s.value(QStringLiteral("models/light/gradeContrast"), 105).toInt() / 100.0f,
                                       s.value(QStringLiteral("models/light/gradeSat"),      110).toInt() / 100.0f,
                                       s.value(QStringLiteral("models/light/gradeWarmth"),    30).toInt() / 1000.0f);
    m_modelView->setSsaoParams(        s.value(QStringLiteral("models/light/ssaoStr"),    100).toInt() / 100.0f,
                                       s.value(QStringLiteral("models/light/ssaoRad"),     30).toInt() / 100.0f);
}

// ── Shaders popup (shell-fur + mesh-FX) ───────────────────────────────────────
void ModelsTab::toggleShaderPanel()
{
    if (m_shaderPanel && m_shaderPanel->isVisible()) { m_shaderPanel->hide(); return; }
    if (!m_shaderPanel) buildShaderPanel();
    m_shaderPanel->adjustSize();
    m_shaderPanel->move(panelPosLeftOf(m_shaderBtn, m_shaderPanel->sizeHint()));
    m_shaderPanel->show();
    m_shaderPanel->raise();
}
void ModelsTab::buildShaderPanel()
{
    if (m_shaderPanel) return;
    QSettings s;
    m_shaderPanel = new QFrame(this, Qt::Popup);
    m_shaderPanel->setObjectName(QStringLiteral("shaderPanel"));
    m_shaderPanel->setProperty("hoverBtn", QVariant::fromValue<QObject*>(m_shaderBtn));
    m_shaderPanel->installEventFilter(this);
    m_shaderPanel->setStyleSheet(QStringLiteral(
        "QFrame#shaderPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
        "QLabel{color:#cccccc;} QCheckBox{color:#cccccc;}"));
    auto* pl = new QVBoxLayout(m_shaderPanel);
    pl->setContentsMargins(12, 10, 12, 10);
    pl->setSpacing(5);
    auto* hdr = new QLabel(QStringLiteral("Shaders"), m_shaderPanel);
    hdr->setStyleSheet(QLatin1String(kHdrQss));
    pl->addWidget(hdr);

    auto* furChk = new QCheckBox(QStringLiteral("Fur (shell displacement)"), m_shaderPanel);
    furChk->setChecked(s.value(QStringLiteral("models/viewport/fur"), true).toBool());
    furChk->setToolTip(QStringLiteral("Render auto-detected fur materials as extruded shell fur."));
    connect(furChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("models/viewport/fur"), on);
        if (m_modelView) m_modelView->setFurEnabled(on);
    });
    pl->addWidget(furChk);

    auto* furHdr = new QLabel(QStringLiteral("Fur detail"), m_shaderPanel);
    furHdr->setStyleSheet(QStringLiteral("color:#9ad; margin-top:4px;"));
    pl->addWidget(furHdr);

    struct SRow { QSlider* sl; int def; QString key; };
    QVector<SRow> furRows, fxRows;
    auto slider = [&](QVector<SRow>& rows, const QString& key, const QString& label, int lo, int hi,
                      int def, double scale, std::function<void(double)> apply) {
        auto* row = new QHBoxLayout();
        auto* lbl = new QLabel(label, m_shaderPanel);
        lbl->setMinimumWidth(54);
        row->addWidget(lbl);
        auto* sl = new QSlider(Qt::Horizontal, m_shaderPanel);
        sl->setRange(lo, hi);
        const int init = s.value(QStringLiteral("models/viewport/") + key, def).toInt();
        sl->setValue(init);
        auto* val = new QLabel(QString::number(init), m_shaderPanel);
        val->setMinimumWidth(26);
        val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        connect(sl, &QSlider::valueChanged, this, [this, key, apply, scale, val](int v) {
            QSettings().setValue(QStringLiteral("models/viewport/") + key, v);
            val->setText(QString::number(v));
            apply(v * scale);
        });
        row->addWidget(sl, 1);
        row->addWidget(val);
        pl->addLayout(row);
        rows.append({sl, def, key});
    };
    auto presetButtons = [&](const QString& section, const QVector<SRow>& rowsRef) {
        const QVector<SRow> rows = rowsRef;
        auto* row = new QHBoxLayout();
        auto* dBtn = new QPushButton(QStringLiteral("Default"), m_shaderPanel);
        auto* sBtn = new QPushButton(QStringLiteral("Save preset"), m_shaderPanel);
        auto* rBtn = new QPushButton(QStringLiteral("Restore preset"), m_shaderPanel);
        connect(dBtn, &QPushButton::clicked, this, [rows]{ for (const SRow& r : rows) r.sl->setValue(r.def); });
        connect(sBtn, &QPushButton::clicked, this, [rows, section]{
            QSettings q; for (const SRow& r : rows)
                q.setValue(QStringLiteral("models/preset/%1/%2").arg(section, r.key), r.sl->value());
        });
        connect(rBtn, &QPushButton::clicked, this, [rows, section]{
            QSettings q; for (const SRow& r : rows)
                r.sl->setValue(q.value(QStringLiteral("models/preset/%1/%2").arg(section, r.key), r.sl->value()).toInt());
        });
        row->addWidget(dBtn); row->addWidget(sBtn); row->addWidget(rBtn);
        pl->addLayout(row);
    };

    slider(furRows, QStringLiteral("furLength"),  QStringLiteral("Length"),  0,  60, 44, 0.0005,
           [this](double v) { if (m_modelView) m_modelView->setFurLength(float(v)); });
    slider(furRows, QStringLiteral("furDensity"), QStringLiteral("Density"), 16, 120, 30, 1.0,
           [this](double v) { if (m_modelView) m_modelView->setFurDensity(float(v)); });
    slider(furRows, QStringLiteral("furShells"),  QStringLiteral("Shells"),  4,  24, 20, 1.0,
           [this](double v) { if (m_modelView) m_modelView->setFurShells(int(v + 0.5)); });
    slider(furRows, QStringLiteral("furGravity"), QStringLiteral("Gravity"), 0,  40, 18, 0.00025,
           [this](double v) { if (m_modelView) m_modelView->setFurGravity(float(v)); });
    slider(furRows, QStringLiteral("furCurl"),    QStringLiteral("Comb"),    0,  40, 14, 0.00025,
           [this](double v) { if (m_modelView) m_modelView->setFurCurl(float(v)); });
    // Coverage: how much of a fur submesh actually grows shells. Higher = fuller (lowers the FurMask
    // density cutoff, so sparser mask areas still grow fur — fixes fur that "doesn't fully cover").
    slider(furRows, QStringLiteral("furCoverage"), QStringLiteral("Coverage"), 0, 60, 57, 0.01,
           [this](double v) { if (m_modelView) m_modelView->setFurCoverage(float(0.60 - v)); });
    presetButtons(QStringLiteral("fur"), furRows);

    auto* fxHdr = new QLabel(QStringLiteral("Mesh FX  (× authored game values)"), m_shaderPanel);
    fxHdr->setStyleSheet(QStringLiteral("color:#9ad; margin-top:6px;"));
    pl->addWidget(fxHdr);
    slider(fxRows, QStringLiteral("fxIntensity"), QStringLiteral("Bright"), 0, 40, 20, 0.05,
           [this](double v) { if (m_modelView) m_modelView->setFxIntensity(float(v)); });
    slider(fxRows, QStringLiteral("fxScroll"),    QStringLiteral("Scroll"), 0, 40, 20, 0.05,
           [this](double v) { if (m_modelView) m_modelView->setFxScrollSpeed(float(v)); });
    slider(fxRows, QStringLiteral("fxWobble"),    QStringLiteral("Wobble"), 0, 40, 20, 0.05,
           [this](double v) { if (m_modelView) m_modelView->setFxWobble(float(v)); });
    presetButtons(QStringLiteral("fx"), fxRows);
}

// ── Detail-maps discovery tool (global render config) ─────────────────────────
static GLModelWidget::DetailConfig mtb_loadDetailCfg()
{
    QSettings s;
    GLModelWidget::DetailConfig c;   // baked defaults
    auto key = [](const QString& k) { return QStringLiteral("models/detail/") + k; };
    c.autoMode = s.value(key(QStringLiteral("auto")), c.autoMode).toBool();
    for (int i = 0; i < 4; ++i) {
        c.zoneMap[i] = s.value(key(QStringLiteral("zone%1").arg(i)), c.zoneMap[i]).toInt();
        c.bands[i]   = float(s.value(key(QStringLiteral("band%1").arg(i)), c.bands[i]).toDouble());
    }
    c.metalThresh = float(s.value(key(QStringLiteral("metalThresh")), c.metalThresh).toDouble());
    c.metalRoute  = s.value(key(QStringLiteral("metalRoute")), c.metalRoute).toInt();
    return c;
}
void ModelsTab::applyDetailConfig()
{
    if (m_modelView) m_modelView->setDetailConfig(mtb_loadDetailCfg());
}
QString ModelsTab::detailConfigText() const
{
    const GLModelWidget::DetailConfig c = mtb_loadDetailCfg();
    auto layerName = [](int l) { return l < 0 ? QStringLiteral("none") : QStringLiteral("map%1").arg(l); };
    auto routeName = [](int r) {
        return r == -2 ? QStringLiteral("auto (by texture name)")
             : r == -1 ? QStringLiteral("off")
                       : QStringLiteral("force map%1").arg(r);
    };
    QString t = QStringLiteral("Detail-map config (global):\n");
    t += c.autoMode ? QStringLiteral("  MODE: Auto (per-item game data — values below are the manual fallback)\n")
                    : QStringLiteral("  MODE: Manual override (the values below apply to all items)\n");
    t += QStringLiteral("  zone→map:  zone0(unmasked)=none");
    for (int i = 1; i < 4; ++i)
        t += QStringLiteral(", zone%1=%2").arg(i).arg(layerName(c.zoneMap[i]));
    t += QStringLiteral("\n  dye bands: %1, %2, %3, %4\n")
             .arg(c.bands[0], 0, 'f', 3).arg(c.bands[1], 0, 'f', 3)
             .arg(c.bands[2], 0, 'f', 3).arg(c.bands[3], 0, 'f', 3);
    t += QStringLiteral("  metalness threshold: %1\n").arg(c.metalThresh, 0, 'f', 2);
    t += QStringLiteral("  metal routing: %1\n").arg(routeName(c.metalRoute));
    return t;
}
void ModelsTab::toggleDetailPanel()
{
    if (m_detailPanel && m_detailPanel->isVisible()) { m_detailPanel->hide(); return; }
    if (!m_detailPanel) buildDetailPanel();
    m_detailPanel->adjustSize();
    m_detailPanel->move(panelPosLeftOf(m_detailBtn, m_detailPanel->sizeHint()));
    m_detailPanel->show();
    m_detailPanel->raise();
}
void ModelsTab::buildDetailPanel()
{
    if (m_detailPanel) return;
    m_detailPanel = new QFrame(this, Qt::Popup);
    m_detailPanel->setObjectName(QStringLiteral("detailPanel"));
    m_detailPanel->setProperty("hoverBtn", QVariant::fromValue<QObject*>(m_detailBtn));
    m_detailPanel->installEventFilter(this);
    m_detailPanel->setStyleSheet(QStringLiteral(
        "QFrame#detailPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
        "QLabel{color:#cccccc;} QComboBox{color:#dddddd;background:#2b2b2b;border:1px solid #555;"
        "border-radius:3px;padding:1px 4px;} QComboBox QAbstractItemView{background:#2b2b2b;color:#ddd;"
        "selection-background-color:#8a1414;}"));
    auto* pl = new QVBoxLayout(m_detailPanel);
    pl->setContentsMargins(12, 10, 12, 10);
    pl->setSpacing(5);
    auto* hdr = new QLabel(QStringLiteral("Detail maps  (discovery tool — global)"), m_detailPanel);
    hdr->setStyleSheet(QLatin1String(kHdrQss));
    pl->addWidget(hdr);
    auto* note = new QLabel(QStringLiteral("Auto uses the rule derived from each item's game data.\n"
                                           "Turn it off to override and experiment, then Copy config."), m_detailPanel);
    note->setStyleSheet(QStringLiteral("color:#888;font-size:11px;"));
    pl->addWidget(note);

    auto setD = [](const QString& k, const QVariant& v) {
        QSettings().setValue(QStringLiteral("models/detail/") + k, v); };

    auto* autoChk = new QCheckBox(QStringLiteral("Auto (derive from game data)"), m_detailPanel);
    autoChk->setChecked(mtb_loadDetailCfg().autoMode);
    autoChk->setToolTip(QStringLiteral("Bands from the dye mask, zone→map from present maps, metal by name."));
    connect(autoChk, &QCheckBox::toggled, this, [this, setD](bool on) {
        setD(QStringLiteral("auto"), on); applyDetailConfig();
        if (m_manualDetailBox) m_manualDetailBox->setEnabled(!on);
    });
    pl->addWidget(autoChk);
    auto* manual = new QWidget(m_detailPanel);
    m_manualDetailBox = manual;
    manual->setEnabled(!autoChk->isChecked());
    auto* ml0 = new QVBoxLayout(manual); ml0->setContentsMargins(0, 0, 0, 0); ml0->setSpacing(5);
    pl->addWidget(manual);

    auto* zHdr = new QLabel(QStringLiteral("Dye-zone → detail map"), manual);
    zHdr->setStyleSheet(QStringLiteral("color:#9ad; margin-top:4px;"));
    ml0->addWidget(zHdr);
    const GLModelWidget::DetailConfig cur = mtb_loadDetailCfg();
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    const char* zoneLbl[4] = {"Zone 0 (bare)", "Zone 1", "Zone 2", "Zone 3"};
    for (int z = 1; z < 4; ++z) {
        auto* lbl = new QLabel(QString::fromLatin1(zoneLbl[z]), manual);
        auto* combo = new QComboBox(manual);
        combo->addItem(QStringLiteral("none"), -1);
        combo->addItem(QStringLiteral("map 0"), 0);
        combo->addItem(QStringLiteral("map 1"), 1);
        combo->addItem(QStringLiteral("map 2"), 2);
        const int idx = combo->findData(cur.zoneMap[z]);
        combo->setCurrentIndex(idx >= 0 ? idx : 0);
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, combo, z, setD](int) {
                    setD(QStringLiteral("zone%1").arg(z), combo->currentData().toInt());
                    applyDetailConfig();
                });
        grid->addWidget(lbl, z - 1, 0);
        grid->addWidget(combo, z - 1, 1);
    }
    ml0->addLayout(grid);

    auto* mHdr = new QLabel(QStringLiteral("Metalness routing"), manual);
    mHdr->setStyleSheet(QStringLiteral("color:#9ad; margin-top:6px;"));
    ml0->addWidget(mHdr);
    auto* mRow = new QHBoxLayout();
    mRow->addWidget(new QLabel(QStringLiteral("Metal uses"), manual));
    auto* mCombo = new QComboBox(manual);
    mCombo->addItem(QStringLiteral("auto (by name)"), -2);
    mCombo->addItem(QStringLiteral("off"), -1);
    mCombo->addItem(QStringLiteral("map 0"), 0);
    mCombo->addItem(QStringLiteral("map 1"), 1);
    mCombo->addItem(QStringLiteral("map 2"), 2);
    { const int idx = mCombo->findData(cur.metalRoute); mCombo->setCurrentIndex(idx >= 0 ? idx : 0); }
    connect(mCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, mCombo, setD](int) { setD(QStringLiteral("metalRoute"), mCombo->currentData().toInt()); applyDetailConfig(); });
    mRow->addWidget(mCombo, 1);
    ml0->addLayout(mRow);

    auto slider = [&](const QString& key, const QString& label, int lo, int hi, int init, double scale) {
        auto* row = new QHBoxLayout();
        auto* lbl = new QLabel(label, manual); lbl->setMinimumWidth(78); row->addWidget(lbl);
        auto* sl = new QSlider(Qt::Horizontal, manual); sl->setRange(lo, hi); sl->setValue(init);
        auto* val = new QLabel(QString::number(init * scale, 'f', 3), manual);
        val->setMinimumWidth(38); val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        connect(sl, &QSlider::valueChanged, this, [this, key, scale, val, setD](int v) {
            setD(key, v * scale); val->setText(QString::number(v * scale, 'f', 3)); applyDetailConfig(); });
        row->addWidget(sl, 1); row->addWidget(val);
        ml0->addLayout(row);
        return sl;
    };
    auto* tHdr = new QLabel(QStringLiteral("Thresholds"), manual);
    tHdr->setStyleSheet(QStringLiteral("color:#9ad; margin-top:6px;"));
    ml0->addWidget(tHdr);
    slider(QStringLiteral("metalThresh"), QStringLiteral("Metal ≥"), 0, 100, int(cur.metalThresh * 100 + 0.5), 0.01);
    for (int i = 0; i < 4; ++i)
        slider(QStringLiteral("band%1").arg(i), QStringLiteral("Band %1").arg(i), 0, 1000,
               int(cur.bands[i] * 1000 + 0.5), 0.001);

    auto* btnRow = new QHBoxLayout();
    auto* dBtn = new QPushButton(QStringLiteral("Reset to game default"), m_detailPanel);
    auto* cBtn = new QPushButton(QStringLiteral("Copy config"), m_detailPanel);
    connect(dBtn, &QPushButton::clicked, this, [this] {
        QSettings q; const QString p = QStringLiteral("models/detail/");
        for (const QString& k : q.allKeys()) if (k.startsWith(p)) q.remove(k);
        applyDetailConfig();
        m_manualDetailBox = nullptr;
        if (m_detailPanel) { m_detailPanel->hide(); m_detailPanel->deleteLater(); m_detailPanel = nullptr; }
    });
    connect(cBtn, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(detailConfigText());
        if (m_detailBtn) m_detailBtn->setText(QStringLiteral("Copied ✓"));
        QTimer::singleShot(1200, this, [this] { if (m_detailBtn) m_detailBtn->setText(QStringLiteral("Detail maps")); });
    });
    btnRow->addWidget(dBtn); btnRow->addWidget(cBtn);
    pl->addLayout(btnRow);
}

// ── Cloth-physics tuning popup (live, debug) ──────────────────────────────────
void ModelsTab::togglePhysicsPanel()
{
    if (m_physPanel && m_physPanel->isVisible()) { m_physPanel->hide(); return; }
    if (!m_physPanel) buildPhysicsPanel();
    m_physPanel->adjustSize();
    m_physPanel->move(panelPosLeftOf(m_physBtn, m_physPanel->sizeHint()));
    m_physPanel->show();
    m_physPanel->raise();
}
void ModelsTab::buildPhysicsPanel()
{
    if (m_physPanel) return;
    m_physPanel = new QFrame(this, Qt::Popup);
    m_physPanel->setObjectName(QStringLiteral("physPanel"));
    m_physPanel->setProperty("hoverBtn", QVariant::fromValue<QObject*>(m_physBtn));
    m_physPanel->installEventFilter(this);
    m_physPanel->setStyleSheet(QStringLiteral(
        "QFrame#physPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
        "QLabel{color:#cccccc;}"));
    auto* pl = new QVBoxLayout(m_physPanel);
    pl->setContentsMargins(12, 10, 12, 10);
    pl->setSpacing(4);
    auto* hdr = new QLabel(QStringLiteral("Cloth physics (live)"), m_physPanel);
    hdr->setStyleSheet(QLatin1String(kHdrQss));
    pl->addWidget(hdr);
    auto* warn = new QLabel(QStringLiteral("Off by default in the Models tab for stability — enabling runs the "
                                           "cloth solver on load."), m_physPanel);
    warn->setWordWrap(true);
    warn->setStyleSheet(QStringLiteral("color:#888;font-size:11px;"));
    pl->addWidget(warn);
    GLModelWidget::ClothParams d;

    // Master enable — shares the Models tab's models/clothSim key (default OFF).
    auto* enablePhys = new QCheckBox(QStringLiteral("Enable physics"), m_physPanel);
    enablePhys->setStyleSheet(QStringLiteral("QCheckBox{color:#fff;font-weight:bold;}"));
    enablePhys->setToolTip(QStringLiteral(
        "Master switch for cloth simulation. Off = the garment renders at its authored (skinned) shape."));
    enablePhys->setChecked(QSettings().value(QStringLiteral("models/clothSim"), false).toBool());
    connect(enablePhys, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("models/clothSim"), on);
        if (m_modelView) m_modelView->setClothEnabled(on);
    });
    pl->addWidget(enablePhys);

    auto* resetters = new QVector<std::function<void()>>();
    m_physPanel->connect(m_physPanel, &QObject::destroyed, [resetters] { delete resetters; });
    struct SliderRef { QSlider* sld; QString key; double scale; };
    auto* sliderRefs = new QVector<SliderRef>();
    m_physPanel->connect(m_physPanel, &QObject::destroyed, [sliderRefs] { delete sliderRefs; });

    auto section = [&](const QString& t) {
        auto* sl = new QLabel(t, m_physPanel);
        sl->setStyleSheet(QStringLiteral("color:#8ab4f8;font-weight:bold;margin-top:6px;"));
        pl->addWidget(sl);
    };
    auto row = [&](const QString& key, const QString& label, int lo, int hi,
                   double scale, double def, const QString& tip) {
        auto* rl = new QHBoxLayout();
        auto* name = new QLabel(label, m_physPanel); name->setFixedWidth(108);
        name->setToolTip(tip);
        auto* sld = new QSlider(Qt::Horizontal, m_physPanel);
        sld->setRange(lo, hi);
        sld->setToolTip(tip);
        auto* val = new QLabel(m_physPanel); val->setFixedWidth(56);
        val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        const double cur = QSettings().value(QStringLiteral("models/cloth/") + key, def).toDouble();
        sld->setValue(int(qRound(cur * scale)));
        val->setText(QString::number(cur, 'g', 3));
        connect(sld, &QSlider::valueChanged, this, [this, key, val, scale](int v) {
            const double fv = v / scale;
            QSettings().setValue(QStringLiteral("models/cloth/") + key, fv);
            val->setText(QString::number(fv, 'g', 3));
            applyClothParams();
        });
        rl->addWidget(name); rl->addWidget(sld, 1); rl->addWidget(val);
        pl->addLayout(rl);
        resetters->append([sld, scale, def] { sld->setValue(int(qRound(def * scale))); });
        sliderRefs->append(SliderRef{sld, key, scale});
    };

    section(QStringLiteral("Tracking & motion"));
    row(QStringLiteral("tracking"),QStringLiteral("Bone tracking"), 0, 100, 100.0, d.boneTracking,
        QStringLiteral("How strongly the cloth follows its authored bone pose each frame."));
    row(QStringLiteral("maxdist"), QStringLiteral("Max distance"),  0, 1000, 1000.0, d.maxDistance,
        QStringLiteral("Swing reach: scales the authored per-bone motion constraint."));
    row(QStringLiteral("damping"), QStringLiteral("Damping"),       800, 999, 1000.0, d.damping,
        QStringLiteral("Velocity retention per frame. Lower settles faster."));
    row(QStringLiteral("gravity"), QStringLiteral("Gravity"),       0, 400, 10000.0, -d.gravity,
        QStringLiteral("Downward pull. Higher droops more."));
    section(QStringLiteral("Stiffness"));
    row(QStringLiteral("bonestiff"), QStringLiteral("Bone stiffness"), 0, 200, 1000.0, d.boneStiffness,
        QStringLiteral("How strongly the cloth bones return to their authored shape."));
    row(QStringLiteral("stretch"), QStringLiteral("Stretch stiff"), 0, 100, 100.0, d.stretchStiffness,
        QStringLiteral("Structural tightness — resistance to stretching."));
    row(QStringLiteral("bend"),    QStringLiteral("Bend stiff"),    0, 100, 100.0, d.bendStiffness,
        QStringLiteral("Resistance to folding/creasing."));
    section(QStringLiteral("Aerodynamics"));
    row(QStringLiteral("drag"),    QStringLiteral("Drag"),          0, 100, 100.0, 0.0,
        QStringLiteral("Air resistance — settles billowing faster."));

    // (The "Preview" section — Phys Bones / Axis — is gone: those overlays live in the viewport's
    //  Overlays popup now, and duplicating them here meant two controls fighting over one setting.)
    section(QStringLiteral("Collision"));
    auto* showCol = new QCheckBox(QStringLiteral("Show collision models"), m_physPanel);
    showCol->setStyleSheet(QStringLiteral("QCheckBox{color:#ccc;}"));
    showCol->setToolTip(QStringLiteral("Draw the authored collision capsules (orange) the cloth collides against."));
    showCol->setChecked(QSettings().value(QStringLiteral("models/cloth/showColliders"), false).toBool());
    connect(showCol, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("models/cloth/showColliders"), on);
        if (m_modelView) m_modelView->setShowColliders(on);
    });
    pl->addWidget(showCol);
    row(QStringLiteral("capScale"), QStringLiteral("Capsule size"),  20, 220, 100.0, d.capsuleRadius,
        QStringLiteral("Scales ALL body-collision capsules (authored + fitted). ~0.52 (default) matches "
                       "the body mesh — the authored radii are larger than the visible body, so 1.0 "
                       "inflates it and splays garments open. Raise "
                       "to push cloth further off the body."));

    // Per-region capsule trim (see ClothParams::capRegion). The game authors a radius PER CAPSULE
    // PER BONE, so a skirt clipping the thighs is a LEGS problem; the global knob above also
    // inflates chest and arms, which is why tuning it alone never lands. 1.0 = authored size.
    row(QStringLiteral("capLegs"),  QStringLiteral("  · Legs"),  20, 300, 100.0, d.capRegion[0],
        QStringLiteral("Thigh / shin / ankle / foot capsules only. Raise to stop a skirt or hem "
                       "clipping through the legs without inflating the torso."));
    row(QStringLiteral("capWaist"), QStringLiteral("  · Waist"), 20, 300, 100.0, d.capRegion[1],
        QStringLiteral("Pelvis capsules only — where most skirts and loincloths anchor."));
    row(QStringLiteral("capTorso"), QStringLiteral("  · Torso"), 20, 300, 100.0, d.capRegion[2],
        QStringLiteral("Chest / centre capsules only — capes and tabards ride on these."));
    row(QStringLiteral("capArms"),  QStringLiteral("  · Arms"),  20, 300, 100.0, d.capRegion[3],
        QStringLiteral("Upper arm / forearm / hand capsules only."));
    row(QStringLiteral("capHead"),  QStringLiteral("  · Head"),  20, 300, 100.0, d.capRegion[4],
        QStringLiteral("Head capsules only — hoods, hair and feathers."));
    row(QStringLiteral("capOther"), QStringLiteral("  · Other"), 20, 300, 100.0, d.capRegion[5],
        QStringLiteral("Capsules on bones outside the shared player rig (mounts, monsters, props)."));
    row(QStringLiteral("margin"),  QStringLiteral("Collide margin"),0, 50, 1000.0, d.collisionMargin,
        QStringLiteral("Extra clearance kept from the body capsules."));
    row(QStringLiteral("friction"),QStringLiteral("Friction"),      0, 100, 100.0, d.friction,
        QStringLiteral("Grip at body contact."));
    row(QStringLiteral("backstop"),QStringLiteral("Backstop"),      0, 80, 1000.0, d.backstop,
        QStringLiteral("How far the cloth may sink toward the body behind its authored surface."));
    row(QStringLiteral("self"),    QStringLiteral("Self-collide"),  0, 60, 1000.0, d.selfCollision,
        QStringLiteral("Cloth thickness for self-collision. 0 disables it."));
    section(QStringLiteral("Interaction"));
    {
        // Orbiting the view reads as spinning the model, so feed that rotation into the sim:
        // the cloth trails when the turn starts/stops and fans out while it continues.
        auto* spinChk = new QCheckBox(QStringLiteral("React to rotation"), m_physPanel);
        spinChk->setStyleSheet(QStringLiteral("QCheckBox{color:#ccc;}"));
        spinChk->setToolTip(QStringLiteral(
            "Rotating the view swings the cloth: simulated parts lag behind as the turn starts and "
            "stops (momentum) and fan outward while it continues (centrifugal).\n"
            "Only simulated parts move — pins, collision and the authored limits still apply."));
        spinChk->setChecked(QSettings().value(QStringLiteral("models/cloth/userSpin"), false).toBool());
        connect(spinChk, &QCheckBox::toggled, this, [this, enablePhys](bool on) {
            QSettings().setValue(QStringLiteral("models/cloth/userSpin"), on);
            // Physics is OFF by default in this tab, so ticking this alone would silently do
            // nothing — turn the master switch on with it (the checkbox updates to match).
            if (on && !enablePhys->isChecked()) enablePhys->setChecked(true);   // → its own handler
            applyClothParams();
        });
        pl->addWidget(spinChk);
    }
    row(QStringLiteral("spinForce"), QStringLiteral("Rotation force"), 0, 500, 100.0, d.userSpinForce,
        QStringLiteral("How strongly view rotation pushes the cloth (needs 'React to rotation'). "
                       "0 = none · 0.1 = subtle (default) · higher = exaggerated swing."));

    section(QStringLiteral("Solver"));
    row(QStringLiteral("substeps"), QStringLiteral("Sub-steps"),    1, 4, 1.0, d.subSteps,
        QStringLiteral("Physics passes per frame. More = steadier under fast motion and much less "
                       "clipping; costs CPU in proportion. 2 is a good balance."));
    row(QStringLiteral("iters"),   QStringLiteral("Iterations"),    1, 20, 1.0, d.iterations,
        QStringLiteral("Constraint solver passes per frame. More = stiffer/stabler, costs more CPU."));

    auto* reset = new QPushButton(QStringLiteral("Reset to defaults"), m_physPanel);
    reset->setToolTip(QStringLiteral("Restore all cloth-physics values to their defaults."));
    connect(reset, &QPushButton::clicked, this, [resetters] { for (const auto& r : *resetters) r(); });
    pl->addWidget(reset);

    auto* presetRow = new QHBoxLayout();
    auto* saveBtn = new QPushButton(QStringLiteral("Save preset"), m_physPanel);
    auto* restoreBtn = new QPushButton(QStringLiteral("Restore preset"), m_physPanel);
    restoreBtn->setEnabled(QSettings().value(QStringLiteral("models/cloth/preset/exists"), false).toBool());
    connect(saveBtn, &QPushButton::clicked, this, [sliderRefs, restoreBtn] {
        QSettings s;
        for (const SliderRef& r : *sliderRefs)
            s.setValue(QStringLiteral("models/cloth/preset/") + r.key,
                       s.value(QStringLiteral("models/cloth/") + r.key));
        s.setValue(QStringLiteral("models/cloth/preset/exists"), true);
        restoreBtn->setEnabled(true);
    });
    connect(restoreBtn, &QPushButton::clicked, this, [sliderRefs] {
        QSettings s;
        if (!s.value(QStringLiteral("models/cloth/preset/exists"), false).toBool()) return;
        for (const SliderRef& r : *sliderRefs) {
            const QString pk = QStringLiteral("models/cloth/preset/") + r.key;
            if (!s.contains(pk)) continue;
            r.sld->setValue(int(qRound(s.value(pk).toDouble() * r.scale)));
        }
    });
    presetRow->addWidget(saveBtn); presetRow->addWidget(restoreBtn);
    pl->addLayout(presetRow);
}
void ModelsTab::applyClothParams()
{
    if (!m_modelView) return;
    QSettings s;
    GLModelWidget::ClothParams d;   // built-in defaults
    // Undo the v2 capsule-size migration (see WardrobeTab2::applyClothParams) — 1.0 inflates the
    // body and splays garments; ~0.52 matches the render.
    if (!s.value(QStringLiteral("cloth/capsuleFix_v3"), false).toBool()) {
        s.setValue(QStringLiteral("cloth/capsuleFix_v3"), true);
        if (s.value(QStringLiteral("cloth/capsuleFix_v2"), false).toBool())
            for (const char* k : {"wardrobe2/cloth/capScale", "models/cloth/capScale", "stable2/cloth/capScale"})
                if (qFuzzyCompare(s.value(QLatin1String(k), 0.52).toDouble(), 1.0))
                    s.setValue(QLatin1String(k), double(d.capsuleRadius));
    }
    auto f = [&](const QString& k, double def) {
        return float(s.value(QStringLiteral("models/cloth/") + k, def).toDouble()); };
    GLModelWidget::ClothParams p;
    p.gravity          = -f(QStringLiteral("gravity"), -d.gravity);   // stored as positive magnitude
    p.damping          = f(QStringLiteral("damping"),  d.damping);
    p.maxDistance      = f(QStringLiteral("maxdist"),  d.maxDistance);
    p.bendStiffness    = f(QStringLiteral("bend"),     d.bendStiffness);
    p.stretchStiffness = f(QStringLiteral("stretch"),  d.stretchStiffness);
    p.iterations       = s.value(QStringLiteral("models/cloth/iters"), d.iterations).toInt();
    p.subSteps         = s.value(QStringLiteral("models/cloth/substeps"), d.subSteps).toInt();
    p.selfCollision    = f(QStringLiteral("self"),     d.selfCollision);
    p.collisionMargin  = f(QStringLiteral("margin"),   d.collisionMargin);
    p.friction         = f(QStringLiteral("friction"), d.friction);
    p.backstop         = f(QStringLiteral("backstop"), d.backstop);
    p.capsuleRadius    = f(QStringLiteral("capScale"), d.capsuleRadius);   // one knob for all capsules
    p.capRegion[0]     = f(QStringLiteral("capLegs"),  d.capRegion[0]);
    p.capRegion[1]     = f(QStringLiteral("capWaist"), d.capRegion[1]);
    p.capRegion[2]     = f(QStringLiteral("capTorso"), d.capRegion[2]);
    p.capRegion[3]     = f(QStringLiteral("capArms"),  d.capRegion[3]);
    p.capRegion[4]     = f(QStringLiteral("capHead"),  d.capRegion[4]);
    p.capRegion[5]     = f(QStringLiteral("capOther"), d.capRegion[5]);
    p.boneTracking     = f(QStringLiteral("tracking"), d.boneTracking);
    p.actorTracking    = d.actorTracking;
    p.horizStiffness   = d.horizStiffness;
    p.shearStiffness   = d.shearStiffness;
    p.attachStiffness  = d.attachStiffness;
    p.dragFactor       = f(QStringLiteral("drag"), 0.0);
    p.boneStiffness    = f(QStringLiteral("bonestiff"), d.boneStiffness);
    // "React to rotation": orbit-driven inertia (see ClothParams::userSpin).
    p.userSpin         = s.value(QStringLiteral("models/cloth/userSpin"), false).toBool();
    p.userSpinForce    = f(QStringLiteral("spinForce"), d.userSpinForce);
    // This tab's master cloth switch defaults OFF (stability — see setClothEnabled at construction),
    // so "React to rotation" would silently do nothing whenever the two disagree. Asking for
    // rotation-driven cloth IS asking for the sim, so converge the state once and PERSIST it — the
    // master checkbox then shows the truth instead of an impossible combination.
    bool clothOn = s.value(QStringLiteral("models/clothSim"), false).toBool();
    if (p.userSpin && !clothOn) {
        clothOn = true;
        s.setValue(QStringLiteral("models/clothSim"), true);
    }
    m_modelView->setClothEnabled(clothOn);
    m_modelView->setClothParams(p);
    m_modelView->setShowColliders(s.value(QStringLiteral("models/cloth/showColliders"), false).toBool());
}

// ── Fullscreen ────────────────────────────────────────────────────────────────
// "Maximize viewport" (Blender's Ctrl+Space): hide this tab's chrome IN PLACE so the preview
// fills the tab — no OS fullscreen, no reparenting, no separate window. The old version took
// over the whole screen, which was needlessly intrusive and moved m_modelView out of the layout
// (dragging its overlay children — gizmo, N-strip, stats — along with it).
//
// Nothing is destroyed or reparented: we only flip visibility, so every overlay, panel and
// connection keeps working and exiting restores the exact prior layout.
void ModelsTab::toggleFullscreen()
{
    if (!m_modelView || !m_mainSplit) return;
    const bool max = !m_viewMaxed;
    m_viewMaxed = max;

    // Side columns (outliner + properties): panes 0 and 2 of the 3-column splitter. The right
    // pane only returns if the » arrow hasn't hidden it independently.
    if (QWidget* w = m_mainSplit->widget(0)) w->setVisible(!max);
    if (m_mainSplit->count() > 2)
        if (QWidget* w = m_mainSplit->widget(2)) w->setVisible(!max && !m_sideCollapsed);
    // Centre column chrome + the transport pane under the viewport.
    // (m_pvHeadW is deliberately NOT touched — it holds only hidden state-holders now and stays
    //  hidden always; re-showing it here would resurrect an empty row above the viewport.)
    if (m_viewBarW) m_viewBarW->setVisible(!max);
    if (m_bottomW)  m_bottomW->setVisible(!max);
    // Floating viewport UI (the settings strip). The axis gizmo + stats stay: they're overlays
    // the user governs from the Overlays toggle, not interface chrome.
    if (m_vpStrip) m_vpStrip->setVisible(!max);

    if (m_fsBtn) {
        m_fsBtn->setText(max ? QStringLiteral("Exit") : QStringLiteral("Fullscreen"));
        m_fsBtn->setToolTip(max
            ? QStringLiteral("Restore the panels (Esc)")
            : QStringLiteral("Maximize the preview — hides the panels and toolbars (Esc to exit)"));
    }
    // Esc exits. Created once, lives on the tab (the old shortcut died with the fullscreen host).
    if (max && !m_fsEsc) {
        m_fsEsc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        m_fsEsc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(m_fsEsc, &QShortcut::activated, this, [this] {
            if (m_viewMaxed) toggleFullscreen();
        });
    }
    if (m_fsEsc) m_fsEsc->setEnabled(max);

    // With the toolbar hidden there'd be no visible way back — Esc alone is a trap for anyone
    // who didn't read a tooltip. A small floating button sits where the strip used to be.
    if (max && !m_fsExitBtn) {
        m_fsExitBtn = new QToolButton(m_modelView);
        m_fsExitBtn->setText(QStringLiteral("✕  Exit fullscreen"));
        m_fsExitBtn->setToolTip(QStringLiteral("Restore the panels (Esc)"));
        m_fsExitBtn->setCursor(Qt::PointingHandCursor);
        m_fsExitBtn->setStyleSheet(QStringLiteral(
            "QToolButton{padding:3px 10px;border:1px solid #5a5a5a;border-radius:3px;"
            "background:rgba(35,35,35,200);color:#ccc;}"
            "QToolButton:hover{border-color:#b0453c;}"));
        connect(m_fsExitBtn, &QToolButton::clicked, this, [this] {
            if (m_viewMaxed) toggleFullscreen();
        });
    }
    if (m_fsExitBtn) {
        m_fsExitBtn->setVisible(max);
        if (max) {
            m_fsExitBtn->adjustSize();
            m_fsExitBtn->move(m_modelView->width() - m_fsExitBtn->width() - 8, 8);
            m_fsExitBtn->raise();
        }
    }
    m_modelView->setFocus(Qt::OtherFocusReason);   // so Esc reaches us, not the search box
}

// ── (Developer mode is retired — the debug viewport panels are always available.) ─────────────
void ModelsTab::applyViewportDevGating()
{
    if (m_shaderBtn) m_shaderBtn->setVisible(true);
    if (m_detailBtn) m_detailBtn->setVisible(true);
    if (m_physBtn)   m_physBtn->setVisible(true);
}
