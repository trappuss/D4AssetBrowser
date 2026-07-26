// WardrobeTab2_Panels.cpp — split out of WardrobeTab2.cpp for maintainability.
// Holds the viewport popup panels (Preview/Graphics, Camera, Lighting, Shaders, Detail maps,
// Physics) plus panelPosLeftOf (N-strip popups open LEFTward, over the viewport) and loadDetailCfg
// and the detail-map config apply/dump. These are WardrobeTab2 member definitions living in a
// second translation unit; the class declaration is unchanged (see WardrobeTab2.h).

#include "tabs/WardrobeTab2.h"

#include "app/Config.h"
#include "app/SettingsDialog.h"
#include "casc/CascReader.h"
#include "gl/GLModelWidget.h"
#include "app/ExportCapture.h"
#include <QHideEvent>
#include <QDateTime>
#include <QProgressDialog>
#include "index/AppearanceMeta.h"
#include "index/IconIndex.h"
#include "index/CoreToc.h"
#include "index/SnoIndex.h"
#include "model/AnimParser.h"
#include "model/Material.h"
#include "model/MaterialDecode.h"
#include "tex/TexMeta.h"
#include "model/ModelParser.h"
#include "util/DyeColorWheel.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QFont>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDrag>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDebug>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QCursor>
#include <QColorDialog>
#include <QContextMenuEvent>
#include <QDataStream>
#include <QDirIterator>
#include <QFrame>
#include <QGroupBox>
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QLineEdit>
#include <QListWidget>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPair>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QGridLayout>
#include <QListView>
#include <QScrollArea>
#include <QShortcut>
#include <QStackedWidget>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QThreadPool>
#include <utility>
#include <QWheelEvent>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QToolButton>
#include <QMenu>
#include <QBuffer>
#include <QScrollBar>
#include <QRandomGenerator>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVector3D>
#include <QVector4D>

#include <algorithm>
#include <cmath>
#include <functional>
#include <thread>

// Position a settings panel to the LEFT of its N-strip button, over the viewport (Blender's
// N-panel side) — the buttons live on the viewport's right edge now, same as the Models tab.
// Clamped to the window/screen so tall panels never run off-screen.
QPoint WardrobeTab2::panelPosLeftOf(QWidget* anchor, const QSize& sz) const
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
void WardrobeTab2::toggleCameraPanel()
{
    if (m_camPanel && m_camPanel->isVisible()) { m_camPanel->hide(); return; }
    if (m_vpPanel) m_vpPanel->hide();          // one preview popup open at a time
    if (m_lightPanel) m_lightPanel->hide();
    if (!m_camPanel) buildCameraPanel();
    m_camPanel->adjustSize();
    m_camPanel->move(panelPosLeftOf(m_camBtn, m_camPanel->sizeHint()));
    m_camPanel->show();
    m_camPanel->raise();
}
void WardrobeTab2::buildCameraPanel()
{
    if (m_camPanel) return;
    QSettings s;
    m_camPanel = new QFrame(this, Qt::Popup);
    m_camPanel->setProperty("hoverBtn", QVariant::fromValue<QObject*>(m_camBtn));
    m_camPanel->installEventFilter(this);
    m_camPanel->setObjectName(QStringLiteral("camPanel"));
    m_camPanel->setStyleSheet(QStringLiteral(
        "QFrame#camPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
        "QLabel{color:#cccccc;} QCheckBox{color:#cccccc;}"));
    auto* pl = new QVBoxLayout(m_camPanel);
    pl->setContentsMargins(12, 10, 12, 10);
    pl->setSpacing(6);
    auto* hdr = new QLabel(QStringLiteral("Camera"), m_camPanel);
    hdr->setStyleSheet(QLatin1String(kHdrQss));
    pl->addWidget(hdr);

    // Frame part on select — the same GLOBAL key the Models tab's Camera panel writes
    // (viewer/framePartOnPick); GLModelWidget reads it live at pick time, so one switch
    // governs every viewport.
    auto* frameChk = new QCheckBox(QStringLiteral("Frame part on select"), m_camPanel);
    frameChk->setToolTip(QStringLiteral("Double-clicking a part in the viewport also zooms/centres the "
                                        "camera on it. Off = double-click only selects."));
    frameChk->setChecked(s.value(QStringLiteral("viewer/framePartOnPick"), true).toBool());
    connect(frameChk, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("viewer/framePartOnPick"), on);
    });
    pl->addWidget(frameChk);

    // Camera Snap: zoom the camera to the selected slot (helm→head, etc.), keeping your angle.
    auto* camSnap = new QCheckBox(QStringLiteral("Camera Snap (zoom to selected slot)"), m_camPanel);
    camSnap->setChecked(m_d4View);
    camSnap->setToolTip(QStringLiteral(
        "Zoom the camera to the selected slot, keeping your viewing angle. Even mid-animation it\n"
        "snaps to where the slot actually is right now (not the bind pose). Enable “Follow\n"
        "animation” below to keep tracking it every frame."));
    // "Follow animation" is a sub-option of Camera Snap.
    auto* followChk = new QCheckBox(QStringLiteral("    Follow animation"), m_camPanel);
    followChk->setChecked(m_camFollow);
    followChk->setEnabled(m_d4View);
    followChk->setToolTip(QStringLiteral(
        "While an animation plays, keep re-centring on the slot's live position so the camera\n"
        "tracks the pose (e.g. the head as the character sits). Off = snap once, then hold still."));
    connect(camSnap, &QCheckBox::toggled, this, [this, followChk](bool on) {
        m_d4View = on;
        QSettings().setValue(QStringLiteral("wardrobe2/cameraSnap"), on);
        followChk->setEnabled(on);
        if (!m_view) return;
        if (on) {
            if (m_fovSlider) m_fovSlider->setValue(32);   // narrow lens
            else             m_view->setFov(32.0f);
            frameSlot(m_activeSlot, /*animate=*/true, /*keepRotation=*/false);   // start at ¾ hero angle
        } else {
            if (m_fovSlider) m_fovSlider->setValue(45);   // back to the default lens
            else             m_view->setFov(45.0f);
            m_view->followParts(QVector<int>{});          // stop any follow
            m_view->resetView();
        }
    });
    connect(followChk, &QCheckBox::toggled, this, [this](bool on) {
        m_camFollow = on;
        QSettings().setValue(QStringLiteral("wardrobe2/cameraFollow"), on);
        if (m_view && m_d4View) frameSlot(m_activeSlot, /*animate=*/true, /*keepRotation=*/true);
    });
    pl->addWidget(camSnap);
    pl->addWidget(followChk);

    // Snap zoom margin: how much padding Camera Snap leaves around the slot (loose ↔ tight).
    auto* marginRow = new QHBoxLayout();
    marginRow->addWidget(new QLabel(QStringLiteral("Snap margin"), m_camPanel));
    auto* marginSlider = new QSlider(Qt::Horizontal, m_camPanel);
    marginSlider->setRange(0, 60);                       // 0.00 – 0.60 padding
    marginSlider->setValue(qBound(0, int(m_snapMargin * 100.0f + 0.5f), 60));
    marginSlider->setToolTip(QStringLiteral("How loosely Camera Snap frames a slot (left = tight crop)"));
    connect(marginSlider, &QSlider::valueChanged, this, [this](int v) {
        m_snapMargin = float(v) / 100.0f;
        QSettings().setValue(QStringLiteral("wardrobe2/snapMargin"), m_snapMargin);
        if (m_view && m_d4View) frameSlot(m_activeSlot, /*animate=*/true, /*keepRotation=*/true);
    });
    marginRow->addWidget(marginSlider, 1);
    pl->addLayout(marginRow);

    // Camera field-of-view (moved here from Preview Settings).
    auto* fovRow = new QHBoxLayout();
    fovRow->addWidget(new QLabel(QStringLiteral("FOV"), m_camPanel));
    m_fovSlider = new QSlider(Qt::Horizontal, m_camPanel);
    m_fovSlider->setRange(10, 100);
    m_fovSlider->setValue(s.value(QStringLiteral("wardrobe2/fov"), 45).toInt());
    m_fovSlider->setToolTip(QStringLiteral("Camera field of view (degrees)"));
    connect(m_fovSlider, &QSlider::valueChanged, this, [this](int v) {
        QSettings().setValue(QStringLiteral("wardrobe2/fov"), v);
        if (m_view) m_view->setFov(float(v));
    });
    fovRow->addWidget(m_fovSlider, 1);
    pl->addLayout(fovRow);

    // View-angle presets: orbit to a fixed angle around the whole model (keeps current zoom).
    pl->addWidget(new QLabel(QStringLiteral("View angle"), m_camPanel));
    auto* presetRow = new QHBoxLayout();
    presetRow->setSpacing(3);
    auto mkPreset = [&](const QString& text, float yaw, float pitch) {
        auto* b = new QPushButton(text, m_camPanel);
        b->setToolTip(QStringLiteral("Orbit to the %1 view").arg(text.toLower()));
        connect(b, &QPushButton::clicked, this, [this, yaw, pitch] {
            if (!m_view) return;
            m_view->followParts(QVector<int>{});   // a fixed angle around the whole model
            m_view->frameThreeQuarter(yaw, pitch, 0.12f);
        });
        presetRow->addWidget(b);
    };
    // Character faces +X, so the camera is on +X for Front (yaw π/2). Left side faces +Z
    // (yaw 0), right faces −Z (yaw π) — confirmed on screen. ¾ swings the front toward Left.
    mkPreset(QStringLiteral("¾"),     0.9708f,  0.12f);
    mkPreset(QStringLiteral("Front"), 1.5708f,  0.05f);
    mkPreset(QStringLiteral("Back"), -1.5708f,  0.05f);
    mkPreset(QStringLiteral("Left"),  0.0f,     0.05f);
    mkPreset(QStringLiteral("Right"), 3.14159f, 0.05f);
    pl->addLayout(presetRow);

    auto* fullBtn = new QPushButton(QStringLiteral("Frame full body  (F)"), m_camPanel);
    fullBtn->setToolTip(QStringLiteral("Zoom back out to the whole model, keeping your current angle. Shortcut: F.\n"
                                       "Tip: double-click any part in the viewport to focus it."));
    connect(fullBtn, &QPushButton::clicked, this, [this] {
        if (m_view) m_view->frameAll(/*keepRotation=*/true);
    });
    pl->addWidget(fullBtn);

    // Auto-rotate turntable: slowly spin the model, with an adjustable speed.
    auto* spinChk = new QCheckBox(QStringLiteral("Auto-rotate (turntable)"), m_camPanel);
    spinChk->setChecked(s.value(QStringLiteral("wardrobe2/turntable"), false).toBool());
    spinChk->setToolTip(QStringLiteral("Slowly rotate the model for a showcase view"));
    auto* spinRow = new QHBoxLayout();
    spinRow->addWidget(new QLabel(QStringLiteral("Speed"), m_camPanel));
    auto* spinSpeed = new QSlider(Qt::Horizontal, m_camPanel);
    spinSpeed->setRange(1, 100);                          // ×0.001 rad/tick → 0.001–0.100
    spinSpeed->setValue(qBound(1, s.value(QStringLiteral("wardrobe2/turntableSpeed"), 25).toInt(), 100));
    spinSpeed->setToolTip(QStringLiteral("Turntable rotation speed"));
    spinRow->addWidget(spinSpeed, 1);
    connect(spinChk, &QCheckBox::toggled, this, [this, spinSpeed](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe2/turntable"), on);
        if (!m_view) return;
        m_view->setSpinSpeed(float(spinSpeed->value()) / 1000.0f);
        m_view->setAutoSpin(on);
    });
    connect(spinSpeed, &QSlider::valueChanged, this, [this](int v) {
        QSettings().setValue(QStringLiteral("wardrobe2/turntableSpeed"), v);
        if (m_view) m_view->setSpinSpeed(float(v) / 1000.0f);
    });
    pl->addWidget(spinChk);
    pl->addLayout(spinRow);

    // Orthographic vs perspective projection.
    auto* orthoChk = new QCheckBox(QStringLiteral("Orthographic projection"), m_camPanel);
    orthoChk->setChecked(s.value(QStringLiteral("wardrobe2/ortho"), false).toBool());
    orthoChk->setToolTip(QStringLiteral("Flat, no-perspective projection — good for straight-on reference shots."));
    connect(orthoChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe2/ortho"), on);
        if (m_view) m_view->setOrthographic(on);
    });
    pl->addWidget(orthoChk);

    // Snap to slot on hover (opt-in): frame a slot while hovering its cell, snap back on leave.
    auto* hoverChk = new QCheckBox(QStringLiteral("Snap to slot on hover"), m_camPanel);
    hoverChk->setChecked(m_hoverSnap);
    hoverChk->setToolTip(QStringLiteral("Frame a slot when you hover its cell (the Helm/Torso/… row), keeping your\n"
                                        "angle. The camera stays on the last-hovered slot."));
    connect(hoverChk, &QCheckBox::toggled, this, [this](bool on) {
        m_hoverSnap = on;
        QSettings().setValue(QStringLiteral("wardrobe2/hoverSnap"), on);
    });
    pl->addWidget(hoverChk);

    // Remember camera on relaunch.
    auto* rememberChk = new QCheckBox(QStringLiteral("Remember camera on relaunch"), m_camPanel);
    rememberChk->setChecked(s.value(QStringLiteral("wardrobe2/rememberCam"), true).toBool());
    rememberChk->setToolTip(QStringLiteral("Restore the last orbit angle / zoom next time you open the app."));
    connect(rememberChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe2/rememberCam"), on);
        if (on) saveCameraState();   // capture the current view as the baseline
    });
    pl->addWidget(rememberChk);

    // Camera presets: three slots that store the current camera (angle/zoom/FOV/projection).
    // "Save" overwrites the slot with the current view; the numbered button loads it back.
    auto* presetHdr = new QLabel(QStringLiteral("Camera presets"), m_camPanel);
    presetHdr->setStyleSheet(QStringLiteral("color:#aaa;"));
    pl->addWidget(presetHdr);
    for (int n = 1; n <= 3; ++n) {
        const QString key = QStringLiteral("wardrobe2/campreset/%1/").arg(n);
        auto* prow = new QHBoxLayout(); prow->setSpacing(3);
        auto* loadBtn = new QPushButton(QStringLiteral("Preset %1").arg(n), m_camPanel);
        loadBtn->setToolTip(QStringLiteral("Load this saved camera (angle · zoom · FOV · projection)"));
        loadBtn->setEnabled(s.value(key + QStringLiteral("set"), false).toBool());
        auto* saveBtn = new QPushButton(QStringLiteral("Save"), m_camPanel);
        saveBtn->setToolTip(QStringLiteral("Overwrite this preset with the current camera"));
        connect(saveBtn, &QPushButton::clicked, this, [this, n, key, loadBtn] {
            if (!m_view) return;
            const GLModelWidget::CamState c = m_view->cameraState();
            QSettings st;
            st.setValue(key + QStringLiteral("yaw"), c.yaw);   st.setValue(key + QStringLiteral("pitch"), c.pitch);
            st.setValue(key + QStringLiteral("dist"), c.dist); st.setValue(key + QStringLiteral("fov"), c.fov);
            st.setValue(key + QStringLiteral("cx"), c.cx);     st.setValue(key + QStringLiteral("cy"), c.cy);
            st.setValue(key + QStringLiteral("cz"), c.cz);     st.setValue(key + QStringLiteral("ortho"), c.ortho);
            st.setValue(key + QStringLiteral("set"), true);
            loadBtn->setEnabled(true);
            if (m_status) m_status->setText(QStringLiteral("Saved camera preset %1").arg(n));
        });
        connect(loadBtn, &QPushButton::clicked, this, [this, n, key] {
            QSettings st;
            if (!m_view || !st.value(key + QStringLiteral("set"), false).toBool()) return;
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
            m_view->setCameraState(c);
            if (m_fovSlider) m_fovSlider->setValue(int(c.fov));   // reflect the preset's FOV in the slider
            if (m_status) m_status->setText(QStringLiteral("Loaded camera preset %1").arg(n));
        });
        prow->addWidget(loadBtn, 1);
        prow->addWidget(saveBtn);
        pl->addLayout(prow);
    }
    // (Screenshot / Turntable GIF capture buttons removed from Camera — they belong with Export.)
}
void WardrobeTab2::togglePreviewPanel()
{
    if (m_vpPanel && m_vpPanel->isVisible()) { m_vpPanel->hide(); return; }
    if (m_camPanel) m_camPanel->hide();        // one preview popup open at a time
    if (m_lightPanel) m_lightPanel->hide();
    if (!m_vpPanel) buildPreviewPanel();
    m_vpPanel->adjustSize();
    m_vpPanel->move(panelPosLeftOf(m_vpBtn, m_vpPanel->sizeHint()));
    m_vpPanel->show();
    m_vpPanel->raise();
}
void WardrobeTab2::buildPreviewPanel()
{
    if (m_vpPanel) return;
    QSettings s;
    m_vpPanel = new QFrame(this, Qt::Popup);
    m_vpPanel->setProperty("hoverBtn", QVariant::fromValue<QObject*>(m_vpBtn));
    m_vpPanel->installEventFilter(this);
    m_vpPanel->setObjectName(QStringLiteral("vpPanel"));
    m_vpPanel->setStyleSheet(QStringLiteral(
        "QFrame#vpPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
        "QLabel{color:#cccccc;} QCheckBox{color:#cccccc;}"));
    auto* pl = new QVBoxLayout(m_vpPanel);
    pl->setContentsMargins(12, 10, 12, 10);
    pl->setSpacing(5);
    auto* hdr = new QLabel(QStringLiteral("Graphics"), m_vpPanel);
    hdr->setStyleSheet(QLatin1String(kHdrQss));
    pl->addWidget(hdr);

    // (Environment dropdown removed — the ambient gradient is now owned by the Lighting
    // panel's preset, so Environment was redundant. See GLModelWidget::applyRig.)

    // Feature toggles, grouped by concern. addChk() adds a checkbox into the given
    // subgroup's layout; each writes its wardrobe2/viewport/<key> setting live and applies.
    auto addChkTo = [&](QVBoxLayout* into, const QString& key, const QString& label, bool def,
                        std::function<void(bool)> apply) {
        auto* cb = new QCheckBox(label, m_vpPanel);
        cb->setChecked(s.value(QStringLiteral("wardrobe2/viewport/") + key, def).toBool());
        connect(cb, &QCheckBox::toggled, this, [this, key, apply](bool on) {
            QSettings().setValue(QStringLiteral("wardrobe2/viewport/") + key, on);
            apply(on);
        });
        into->addWidget(cb);
    };
    auto addGroup = [&](const QString& title) -> QVBoxLayout* {
        auto* box = new QGroupBox(title, m_vpPanel);
        auto* gl  = new QVBoxLayout(box);
        gl->setContentsMargins(8, 4, 8, 4);
        pl->addWidget(box);
        return gl;
    };

    auto* gLight = addGroup(QStringLiteral("Scene & shadows"));   // env light/shadows/AO/tonemap (the artistic light rig lives in the separate Lighting panel)
    addChkTo(gLight, QStringLiteral("ibl"), QStringLiteral("Environment lighting (IBL)"), true,
             [this](bool on) { if (m_view) m_view->setFeatureIbl(on); });
    addChkTo(gLight, QStringLiteral("shadow"), QStringLiteral("Self-shadows"), true,
             [this](bool on) { if (m_view) m_view->setShadowEnabled(on); });
    addChkTo(gLight, QStringLiteral("ssao"), QStringLiteral("Ambient occlusion (SSAO)"), true,
             [this](bool on) { if (m_view) m_view->setSsaoEnabled(on); });
    addChkTo(gLight, QStringLiteral("tonemap"), QStringLiteral("Tonemap (ACES) + sRGB"), true,
             [this](bool on) { if (m_view) m_view->setFeatureTonemap(on); });

    auto* gShade = addGroup(QStringLiteral("Shading"));
    addChkTo(gShade, QStringLiteral("detail"), QStringLiteral("Detail maps"), true,
             [this](bool on) { if (m_view) m_view->setFeatureDetail(on); });
    addChkTo(gShade, QStringLiteral("subsurface"), QStringLiteral("Subsurface / translucency"), true,
             [this](bool on) { if (m_view) m_view->setFeatureSubsurface(on); });
    addChkTo(gShade, QStringLiteral("hair"), QStringLiteral("Hair anisotropy"), true,
             [this](bool on) { if (m_view) m_view->setFeatureHair(on); });
    addChkTo(gShade, QStringLiteral("specaa"), QStringLiteral("Specular anti-aliasing"), true,
             [this](bool on) { if (m_view) m_view->setFeatureSpecAA(on); });

    auto* gGeom = addGroup(QStringLiteral("Geometry & debug"));
    addChkTo(gGeom, QStringLiteral("mask"), QStringLiteral("Primary mask"), false,
             [this](bool on) { if (m_view) m_view->setFeatureMask(on); });
    // (Ensembles panel toggle lives in File ▸ Settings ▸ Wardrobe ▸ Toggleable panels, not here.)

    // Exposure moved to the Lighting panel (covered by its Default/Save/Restore presets).
    // FOV + Camera Snap/follow moved to the dedicated Camera popup (buildCameraPanel).

    // ── Backdrop: one-click studio presets + optional vertical gradient + custom colour —
    // mirrors the Models Graphics panel exactly.
    {
        auto* gBg = addGroup(QStringLiteral("Backdrop"));
        auto* row = new QHBoxLayout();
        row->setSpacing(4);
        auto chip = [&](const char* name, const QColor& c) {
            auto* b = new QToolButton(m_vpPanel);
            b->setFixedSize(24, 20);
            b->setToolTip(QString::fromLatin1(name));
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(QStringLiteral("QToolButton{background:%1;border:1px solid #555;"
                                            "border-radius:3px;}QToolButton:hover{border-color:#b0453c;}")
                                 .arg(c.name()));
            connect(b, &QToolButton::clicked, this, [this, c]() {
                if (m_view) m_view->setBackgroundColor(c);
                QSettings().setValue(QStringLiteral("wardrobe2/viewport/bg"), c.name());
            });
            row->addWidget(b);
        };
        chip("Dark",     QColor(0x10, 0x10, 0x10));
        chip("Charcoal", QColor(0x23, 0x23, 0x23));
        chip("Grey",     QColor(0x4b, 0x4b, 0x4b));
        chip("Light",    QColor(0xa6, 0xa6, 0xa6));
        auto* custom = new QToolButton(m_vpPanel);
        custom->setText(QStringLiteral("…"));
        custom->setToolTip(QStringLiteral("Custom background colour"));
        custom->setFixedSize(24, 20);
        custom->setCursor(Qt::PointingHandCursor);
        custom->setStyleSheet(QLatin1String(kArrowBtnQss));
        connect(custom, &QToolButton::clicked, this, [this] {
            if (!m_view) return;
            const QColor c = QColorDialog::getColor(m_view->backgroundColor(), m_vpPanel,
                                                    QStringLiteral("Viewport background"));
            if (c.isValid()) {
                m_view->setBackgroundColor(c);
                QSettings().setValue(QStringLiteral("wardrobe2/viewport/bg"), c.name());
            }
        });
        row->addWidget(custom);
        row->addStretch(1);
        gBg->addLayout(row);
        auto* grad = new QCheckBox(QStringLiteral("Gradient (lighter top, darker floor)"), m_vpPanel);
        grad->setToolTip(QStringLiteral("Studio-style vertical wash derived from the backdrop colour."));
        grad->setChecked(QSettings().value(QStringLiteral("wardrobe2/viewport/bgGradient"), false).toBool());
        connect(grad, &QCheckBox::toggled, this, [this](bool on) {
            QSettings().setValue(QStringLiteral("wardrobe2/viewport/bgGradient"), on);
            if (m_view) m_view->setBackgroundGradient(on);
        });
        gBg->addWidget(grad);
    }
}
void WardrobeTab2::toggleLightingPanel()
{
    if (m_lightPanel && m_lightPanel->isVisible()) { m_lightPanel->hide(); return; }
    if (m_camPanel) m_camPanel->hide();        // one preview popup open at a time
    if (m_vpPanel) m_vpPanel->hide();
    if (!m_lightPanel) buildLightingPanel();
    m_lightPanel->adjustSize();
    m_lightPanel->move(panelPosLeftOf(m_lightBtn, m_lightPanel->sizeHint()));
    m_lightPanel->show();
    m_lightPanel->raise();
}
void WardrobeTab2::buildLightingPanel()
{
    if (m_lightPanel) return;
    QSettings s;
    m_lightPanel = new QFrame(this, Qt::Popup);
    m_lightPanel->setProperty("hoverBtn", QVariant::fromValue<QObject*>(m_lightBtn));
    m_lightPanel->installEventFilter(this);
    m_lightPanel->setObjectName(QStringLiteral("lightPanel"));
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

    // Preset selects the key/rim/fill COLOURS (intensities + key direction are the sliders below).
    auto* preRow = new QHBoxLayout();
    preRow->addWidget(new QLabel(QStringLiteral("Preset"), m_lightPanel));
    auto* preset = new QComboBox(m_lightPanel);
    preset->addItems({QStringLiteral("D4 Wardrobe (campfire)"),
                      QStringLiteral("Hero Direct (neutral)"),
                      QStringLiteral("Studio (cool 3-point)")});
    preset->setCurrentIndex(s.value(QStringLiteral("wardrobe2/light/preset"), 1).toInt());   // default: Hero Direct
    connect(preset, &QComboBox::currentIndexChanged, this, [this](int i) {
        QSettings().setValue(QStringLiteral("wardrobe2/light/preset"), i);
        applyLightRig();
    });
    preRow->addWidget(preset, 1);
    pl->addLayout(preRow);

    auto* reflChk = new QCheckBox(QStringLiteral("Reflections (game probe)"), m_lightPanel);
    reflChk->setChecked(s.value(QStringLiteral("wardrobe2/light/reflections"), true).toBool());
    reflChk->setToolTip(QStringLiteral(
        "Use Diablo IV's real character-screen reflection cubemap for metal/gloss reflections"));
    connect(reflChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe2/light/reflections"), on);
        if (m_view) m_view->setReflectionEnabled(on);
    });
    pl->addWidget(reflChk);

    auto* lockChk = new QCheckBox(QStringLiteral("Lock lights to world"), m_lightPanel);
    lockChk->setChecked(s.value(QStringLiteral("wardrobe2/light/lock"), false).toBool());
    lockChk->setToolTip(QStringLiteral(
        "Off: three-point rig tracks the camera (consistent from any angle).\n"
        "On: snapshots the lights at the current camera orbit and pins them in world space — orbit to\n"
        "light the character how you like, then tick this to freeze it. Re-tick to re-capture."));
    connect(lockChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe2/light/lock"), on);
        if (m_view) m_view->setLightLock(on);
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
        const int init = s.value(QStringLiteral("wardrobe2/light/") + key, def).toInt();
        sl->setValue(init);
        auto* val = new QLabel(QString::number(init), m_lightPanel);
        val->setMinimumWidth(30);
        val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        connect(sl, &QSlider::valueChanged, this, [this, key, val](int v) {
            QSettings().setValue(QStringLiteral("wardrobe2/light/") + key, v);
            val->setText(QString::number(v));
            applyLightRig();
        });
        row->addWidget(sl, 1);
        row->addWidget(val);
        pl->addLayout(row);
        rows.append({sl, def, key});
    };
    auto section = [&](const QString& title) {   // bold section header to group the sliders
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
    slider(QStringLiteral("sss"),      QStringLiteral("Subsurface %"), 0, 200,  15, QStringLiteral("Skin subsurface scattering strength (0 = matte/Lambert, 100 = full SSS)"));
    slider(QStringLiteral("skinwarm"), QStringLiteral("Skin warmth"),  0, 200, 100, QStringLiteral("Skin subsurface red-bleed hue"));
    slider(QStringLiteral("wetness"),  QStringLiteral("Wetness %"),    0, 100,   0, QStringLiteral("Rain-slick look: darkens the diffuse and sharpens reflections (D4 'Wetness Bias'). 0 = dry"));
    slider(QStringLiteral("snow"),     QStringLiteral("Snow %"),       0, 100,   0, QStringLiteral("Snow dusting on upward-facing surfaces — shoulders, head, ledges (D4 'Use Snowiness'). 0 = none"));
    slider(QStringLiteral("emis"),     QStringLiteral("Emissive %"),   0, 300,  50, QStringLiteral("Glow intensity of emissive armor (runes/gems); lower keeps colour, higher blows toward white"));
    section(QStringLiteral("Shadows"));
    slider(QStringLiteral("shadowStr"),  QStringLiteral("Shadow %"),    0, 100,  60, QStringLiteral("Self-shadow darkness (key-light shadow map)"));
    slider(QStringLiteral("shadowSoft"), QStringLiteral("Shadow soft"), 0,  40,  15, QStringLiteral("Shadow edge softness (PCF radius, ÷10 texels)"));
    slider(QStringLiteral("shadowBias"), QStringLiteral("Shadow bias"), 0,  50,  18, QStringLiteral("Depth bias to avoid shadow acne (÷10000); raise if surfaces self-shadow, lower if shadows detach"));
    slider(QStringLiteral("shadowNBias"), QStringLiteral("Shadow n-bias"), 0, 50, 10, QStringLiteral("Normal-offset bias (÷1000 of model size) — slope-aware acne fix; more forgiving than depth bias"));
    slider(QStringLiteral("shadowRange"), QStringLiteral("Shadow range"), 100, 300, 130, QStringLiteral("Shadow frustum tightness (÷100). Lower = sharper shadows but may clip at the edges; higher = looser coverage"));
    slider(QStringLiteral("shadowRes"),   QStringLiteral("Shadow res"),  1024, 4096, 2048, QStringLiteral("Shadow-map resolution (snaps to ×512). Higher = crisper shadow edges, a little slower"));
    section(QStringLiteral("Ambient occlusion"));
    slider(QStringLiteral("ssaoStr"),    QStringLiteral("Amb. occlusion %"), 0, 200, 100, QStringLiteral("Screen-space ambient occlusion darkness in creases/contact areas"));
    slider(QStringLiteral("ssaoRad"),    QStringLiteral("AO radius"),         5, 100,  30, QStringLiteral("SSAO sampling radius in world units (÷100); larger = broader, softer occlusion"));
    section(QStringLiteral("Colour grade"));
    auto* gradeChk = new QCheckBox(QStringLiteral("Enable colour grade"), m_lightPanel);
    gradeChk->setToolTip(QStringLiteral("Post-tonemap contrast + saturation + warm-shadow/cool-highlight split-tone. "
                                        "Off by default — a stylised approximation of D4's grade, not the exact game LUT."));
    gradeChk->setChecked(s.value(QStringLiteral("wardrobe2/light/grade"), false).toBool());
    connect(gradeChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe2/light/grade"), on); applyLightRig();
    });
    pl->addWidget(gradeChk);
    slider(QStringLiteral("gradeContrast"), QStringLiteral("Contrast"),  50, 200, 105, QStringLiteral("Contrast S-curve about mid-grey (÷100)."));
    slider(QStringLiteral("gradeSat"),      QStringLiteral("Saturation"), 0, 200, 110, QStringLiteral("Colour saturation (÷100)."));
    slider(QStringLiteral("gradeWarmth"),   QStringLiteral("Split-tone"), 0, 200,  30, QStringLiteral("Warm shadows / cool highlights strength (÷1000)."));
    // Point this at the REAL D4 grade LUT texture (a 256×16 strip) to apply the authentic grade
    // instead of the stylised one. Find its name in the Textures tab (search "lut"/"grade").
    auto* lutRow = new QHBoxLayout();
    auto* lutLbl = new QLabel(QStringLiteral("LUT tex"), m_lightPanel); lutLbl->setMinimumWidth(64);
    lutRow->addWidget(lutLbl);
    auto* lutEdit = new QLineEdit(s.value(QStringLiteral("wardrobe2/light/lutName")).toString(), m_lightPanel);
    lutEdit->setPlaceholderText(QStringLiteral("real D4 LUT texture name (256×16) — blank = stylised"));
    lutEdit->setToolTip(QStringLiteral("Name of the real D4 colour-grade LUT texture. Find it in the Textures tab "
                                       "(search 'lut'/'grade'/'colorgrad'); a 256×16 strip. Blank uses the stylised grade."));
    connect(lutEdit, &QLineEdit::editingFinished, this, [this, lutEdit] {
        QSettings().setValue(QStringLiteral("wardrobe2/light/lutName"), lutEdit->text().trimmed());
        applyLightRig();
    });
    lutRow->addWidget(lutEdit, 1);
    pl->addLayout(lutRow);

    // Default → baked defaults; Save/Restore → a named preset persisted in QSettings.
    const QVector<SRow> rowsCopy = rows;
    auto* btnRow = new QHBoxLayout();
    auto* dBtn = new QPushButton(QStringLiteral("Default"), m_lightPanel);
    auto* sBtn = new QPushButton(QStringLiteral("Save preset"), m_lightPanel);
    auto* rBtn = new QPushButton(QStringLiteral("Restore preset"), m_lightPanel);
    connect(dBtn, &QPushButton::clicked, this, [rowsCopy]{ for (const SRow& r : rowsCopy) r.sl->setValue(r.def); });
    connect(sBtn, &QPushButton::clicked, this, [rowsCopy]{
        QSettings q; for (const SRow& r : rowsCopy)
            q.setValue(QStringLiteral("wardrobe2/preset/light/%1").arg(r.key), r.sl->value());
    });
    connect(rBtn, &QPushButton::clicked, this, [rowsCopy]{
        QSettings q; for (const SRow& r : rowsCopy)
            r.sl->setValue(q.value(QStringLiteral("wardrobe2/preset/light/%1").arg(r.key), r.sl->value()).toInt());
    });
    btnRow->addWidget(dBtn); btnRow->addWidget(sBtn); btnRow->addWidget(rBtn);
    pl->addLayout(btnRow);
}
void WardrobeTab2::toggleShaderPanel()
{
    if (m_shaderPanel && m_shaderPanel->isVisible()) { m_shaderPanel->hide(); return; }
    if (!m_shaderPanel) buildShaderPanel();
    m_shaderPanel->adjustSize();
    m_shaderPanel->move(panelPosLeftOf(m_shaderBtn, m_shaderPanel->sizeHint()));
    m_shaderPanel->show();
    m_shaderPanel->raise();
}
void WardrobeTab2::buildShaderPanel()
{
    if (m_shaderPanel) return;
    QSettings s;
    m_shaderPanel = new QFrame(this, Qt::Popup);
    m_shaderPanel->setProperty("hoverBtn", QVariant::fromValue<QObject*>(m_shaderBtn));
    m_shaderPanel->installEventFilter(this);
    m_shaderPanel->setObjectName(QStringLiteral("shaderPanel"));
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
    furChk->setChecked(s.value(QStringLiteral("wardrobe2/viewport/fur"), true).toBool());
    furChk->setToolTip(QStringLiteral("Render auto-detected fur materials as extruded shell fur."));
    connect(furChk, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe2/viewport/fur"), on);
        if (m_view) m_view->setFurEnabled(on);
    });
    pl->addWidget(furChk);

    auto* furHdr = new QLabel(QStringLiteral("Fur detail"), m_shaderPanel);
    furHdr->setStyleSheet(QStringLiteral("color:#9ad; margin-top:4px;"));
    pl->addWidget(furHdr);

    // Each row shows a live numeric readout; rows are recorded per-section for the preset buttons.
    struct SRow { QSlider* sl; int def; QString key; };
    QVector<SRow> furRows, fxRows;
    auto furSlider = [&](QVector<SRow>& rows, const QString& key, const QString& label, int lo, int hi,
                         int def, double scale, std::function<void(double)> apply) {
        auto* row = new QHBoxLayout();
        auto* lbl = new QLabel(label, m_shaderPanel);
        lbl->setMinimumWidth(54);
        row->addWidget(lbl);
        auto* sl = new QSlider(Qt::Horizontal, m_shaderPanel);
        sl->setRange(lo, hi);
        const int init = s.value(QStringLiteral("wardrobe2/viewport/") + key, def).toInt();
        sl->setValue(init);
        auto* val = new QLabel(QString::number(init), m_shaderPanel);
        val->setMinimumWidth(26);
        val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        connect(sl, &QSlider::valueChanged, this, [this, key, apply, scale, val](int v) {
            QSettings().setValue(QStringLiteral("wardrobe2/viewport/") + key, v);
            val->setText(QString::number(v));
            apply(v * scale);
        });
        row->addWidget(sl, 1);
        row->addWidget(val);
        pl->addLayout(row);
        rows.append({sl, def, key});
    };
    // Default → baked defaults; Save/Restore → a named preset persisted in QSettings.
    auto presetButtons = [&](const QString& section, const QVector<SRow>& rowsRef) {
        const QVector<SRow> rows = rowsRef;            // copy by value (slider pointers stay valid)
        auto* row = new QHBoxLayout();
        auto* dBtn = new QPushButton(QStringLiteral("Default"), m_shaderPanel);
        auto* sBtn = new QPushButton(QStringLiteral("Save preset"), m_shaderPanel);
        auto* rBtn = new QPushButton(QStringLiteral("Restore preset"), m_shaderPanel);
        connect(dBtn, &QPushButton::clicked, this, [rows]{ for (const SRow& r : rows) r.sl->setValue(r.def); });
        connect(sBtn, &QPushButton::clicked, this, [rows, section]{
            QSettings q; for (const SRow& r : rows)
                q.setValue(QStringLiteral("wardrobe2/preset/%1/%2").arg(section, r.key), r.sl->value());
        });
        connect(rBtn, &QPushButton::clicked, this, [rows, section]{
            QSettings q; for (const SRow& r : rows)
                r.sl->setValue(q.value(QStringLiteral("wardrobe2/preset/%1/%2").arg(section, r.key), r.sl->value()).toInt());
        });
        row->addWidget(dBtn); row->addWidget(sBtn); row->addWidget(rBtn);
        pl->addLayout(row);
    };

    furSlider(furRows, QStringLiteral("furLength"),  QStringLiteral("Length"),  0,  60, 44, 0.0005,
              [this](double v) { if (m_view) m_view->setFurLength(float(v)); });
    furSlider(furRows, QStringLiteral("furDensity"), QStringLiteral("Density"), 16, 120, 30, 1.0,
              [this](double v) { if (m_view) m_view->setFurDensity(float(v)); });
    furSlider(furRows, QStringLiteral("furShells"),  QStringLiteral("Shells"),  4,  24, 20, 1.0,
              [this](double v) { if (m_view) m_view->setFurShells(int(v + 0.5)); });
    furSlider(furRows, QStringLiteral("furGravity"), QStringLiteral("Gravity"), 0,  40, 18, 0.00025,
              [this](double v) { if (m_view) m_view->setFurGravity(float(v)); });
    furSlider(furRows, QStringLiteral("furCurl"),    QStringLiteral("Comb"),    0,  40, 14, 0.00025,
              [this](double v) { if (m_view) m_view->setFurCurl(float(v)); });
    // Coverage: higher = fuller (lowers the FurMask density cutoff so sparser areas still grow fur).
    furSlider(furRows, QStringLiteral("furCoverage"), QStringLiteral("Coverage"), 0, 60, 57, 0.01,
              [this](double v) { if (m_view) m_view->setFurCoverage(float(0.60 - v)); });
    presetButtons(QStringLiteral("fur"), furRows);

    auto* fxHdr = new QLabel(QStringLiteral("Mesh FX  (× authored game values)"), m_shaderPanel);
    fxHdr->setStyleSheet(QStringLiteral("color:#9ad; margin-top:6px;"));
    pl->addWidget(fxHdr);
    furSlider(fxRows, QStringLiteral("fxIntensity"), QStringLiteral("Bright"), 0, 40, 20, 0.05,
              [this](double v) { if (m_view) m_view->setFxIntensity(float(v)); });
    furSlider(fxRows, QStringLiteral("fxScroll"),    QStringLiteral("Scroll"), 0, 40, 20, 0.05,
              [this](double v) { if (m_view) m_view->setFxScrollSpeed(float(v)); });
    furSlider(fxRows, QStringLiteral("fxWobble"),    QStringLiteral("Wobble"), 0, 40, 20, 0.05,
              [this](double v) { if (m_view) m_view->setFxWobble(float(v)); });
    presetButtons(QStringLiteral("fx"), fxRows);
}
static GLModelWidget::DetailConfig loadDetailCfg()
{
    QSettings s;
    GLModelWidget::DetailConfig c;   // baked defaults
    auto key = [](const QString& k) { return QStringLiteral("wardrobe2/detail/") + k; };
    c.autoMode = s.value(key(QStringLiteral("auto")), c.autoMode).toBool();
    for (int i = 0; i < 4; ++i) {
        c.zoneMap[i] = s.value(key(QStringLiteral("zone%1").arg(i)), c.zoneMap[i]).toInt();
        c.bands[i]   = float(s.value(key(QStringLiteral("band%1").arg(i)), c.bands[i]).toDouble());
    }
    c.metalThresh = float(s.value(key(QStringLiteral("metalThresh")), c.metalThresh).toDouble());
    c.metalRoute  = s.value(key(QStringLiteral("metalRoute")), c.metalRoute).toInt();
    return c;
}
void WardrobeTab2::applyDetailConfig()
{
    if (m_view) m_view->setDetailConfig(loadDetailCfg());
}
QString WardrobeTab2::detailConfigText() const
{
    const GLModelWidget::DetailConfig c = loadDetailCfg();
    auto layerName = [](int l) {
        return l < 0 ? QStringLiteral("none")
                     : QStringLiteral("map%1").arg(l);
    };
    auto routeName = [](int r) {
        return r == -2 ? QStringLiteral("auto (by texture name)")
             : r == -1 ? QStringLiteral("off")
                       : QStringLiteral("force map%1").arg(r);
    };
    QString t = QStringLiteral("Detail-map config (global):\n");
    if (c.autoMode)
        t += QStringLiteral("  MODE: Auto (per-item game data — values below are the manual fallback)\n");
    else
        t += QStringLiteral("  MODE: Manual override (the values below apply to all items)\n");
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
void WardrobeTab2::toggleDetailPanel()
{
    if (m_detailPanel && m_detailPanel->isVisible()) { m_detailPanel->hide(); return; }
    if (!m_detailPanel) buildDetailPanel();
    m_detailPanel->adjustSize();
    m_detailPanel->move(panelPosLeftOf(m_detailBtn, m_detailPanel->sizeHint()));
    m_detailPanel->show();
    m_detailPanel->raise();
}
void WardrobeTab2::buildDetailPanel()
{
    if (m_detailPanel) return;
    m_detailPanel = new QFrame(this, Qt::Popup);
    m_detailPanel->setProperty("hoverBtn", QVariant::fromValue<QObject*>(m_detailBtn));
    m_detailPanel->installEventFilter(this);
    m_detailPanel->setObjectName(QStringLiteral("detailPanel"));
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
        QSettings().setValue(QStringLiteral("wardrobe2/detail/") + k, v); };

    // Auto (game-data) master toggle. When on, the shader uses each part's detected bands + derived
    // zone→map; the manual controls below only take effect when it's off.
    auto* autoChk = new QCheckBox(QStringLiteral("Auto (derive from game data)"), m_detailPanel);
    autoChk->setChecked(loadDetailCfg().autoMode);
    autoChk->setToolTip(QStringLiteral("Bands from the dye mask, zone→map from present maps, metal by name."));
    connect(autoChk, &QCheckBox::toggled, this, [this, setD](bool on) {
        setD(QStringLiteral("auto"), on); applyDetailConfig();
        if (m_manualDetailBox) m_manualDetailBox->setEnabled(!on);
    });
    pl->addWidget(autoChk);
    // Everything manual goes in a container we can grey out while Auto is on.
    auto* manual = new QWidget(m_detailPanel);
    m_manualDetailBox = manual;
    manual->setEnabled(!autoChk->isChecked());
    auto* ml0 = new QVBoxLayout(manual); ml0->setContentsMargins(0, 0, 0, 0); ml0->setSpacing(5);
    pl->addWidget(manual);

    // Zone → map selectors (zone0 is the unmasked/bare band → always none).
    auto* zHdr = new QLabel(QStringLiteral("Dye-zone → detail map"), manual);
    zHdr->setStyleSheet(QStringLiteral("color:#9ad; margin-top:4px;"));
    ml0->addWidget(zHdr);
    const GLModelWidget::DetailConfig cur = loadDetailCfg();
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    const char* zoneLbl[4] = {"Zone 0 (bare)", "Zone 1", "Zone 2", "Zone 3"};
    for (int z = 1; z < 4; ++z) {   // zone0 fixed to none
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

    // Metal routing.
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

    // Sliders: metalness threshold + the four dye-band centres.
    auto slider = [&](const QString& key, const QString& label, int lo, int hi, int init,
                      double scale) {
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
    QVector<QSlider*> allSl;
    allSl << slider(QStringLiteral("metalThresh"), QStringLiteral("Metal ≥"), 0, 100,
                    int(cur.metalThresh * 100 + 0.5), 0.01);
    for (int i = 0; i < 4; ++i)
        allSl << slider(QStringLiteral("band%1").arg(i), QStringLiteral("Band %1").arg(i), 0, 1000,
                        int(cur.bands[i] * 1000 + 0.5), 0.001);

    // Actions: reset to shipped defaults, and copy the config text to the clipboard.
    auto* btnRow = new QHBoxLayout();
    auto* dBtn = new QPushButton(QStringLiteral("Reset to game default"), m_detailPanel);
    auto* cBtn = new QPushButton(QStringLiteral("Copy config"), m_detailPanel);
    connect(dBtn, &QPushButton::clicked, this, [this] {
        QSettings q; const QString p = QStringLiteral("wardrobe2/detail/");
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
// (toggleRigPanel / buildRigPanel are GONE — the Rig popup's checkboxes moved into the toolbar's
// Overlays ▾ panel, exactly like the Models tab. m_rigChk* now point at those overlay boxes, so
// applyRigToggle's mirroring keeps working unchanged.)
void WardrobeTab2::togglePhysicsPanel()
{
    if (m_physPanel && m_physPanel->isVisible()) { m_physPanel->hide(); return; }
    if (!m_physPanel) buildPhysicsPanel();
    m_physPanel->adjustSize();
    m_physPanel->move(panelPosLeftOf(m_physBtn, m_physPanel->sizeHint()));
    m_physPanel->show();
    m_physPanel->raise();
}
void WardrobeTab2::buildPhysicsPanel()
{
    if (m_physPanel) return;
    m_physSliders.clear();   // (re)built below; registry for game-value display + unlocked limits
    m_physPanel = new QFrame(this, Qt::Popup);
    m_physPanel->setProperty("hoverBtn", QVariant::fromValue<QObject*>(m_physBtn));
    m_physPanel->installEventFilter(this);
    m_physPanel->setObjectName(QStringLiteral("physPanel"));
    m_physPanel->setStyleSheet(QStringLiteral(
        "QFrame#physPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
        "QLabel{color:#cccccc;}"));
    auto* pl = new QVBoxLayout(m_physPanel);
    pl->setContentsMargins(12, 10, 12, 10);
    pl->setSpacing(4);
    auto* hdr = new QLabel(QStringLiteral("Cloth physics (live)"), m_physPanel);
    hdr->setStyleSheet(QLatin1String(kHdrQss));
    pl->addWidget(hdr);
    GLModelWidget::ClothParams d;
    // Sliders whose value is overridden by the authored game data (in applyClothParams):
    // disabled while "Use game cloth data" is on, so it's clear the game drives them.
    auto* gameWidgets = new QVector<QWidget*>();
    m_physPanel->connect(m_physPanel, &QObject::destroyed, [gameWidgets] { delete gameWidgets; });

    // ── Master toggles ──────────────────────────────────────────────────────────
    auto* enablePhys = new QCheckBox(QStringLiteral("Enable physics"), m_physPanel);
    enablePhys->setStyleSheet(QStringLiteral("QCheckBox{color:#fff;font-weight:bold;}"));
    enablePhys->setToolTip(QStringLiteral(
        "Master switch for cloth simulation. Off = the garment renders at its authored "
        "(skinned) shape with no sim — useful to compare, or if a set misbehaves."));
    enablePhys->setChecked(QSettings().value(QStringLiteral("wardrobe2/cloth/enabled"), true).toBool());
    connect(enablePhys, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe2/cloth/enabled"), on);
        if (m_view) m_view->setClothEnabled(on);
    });
    pl->addWidget(enablePhys);

    // 1:1 cage solver — simulate the game's authored sim cages (pins + distance constraints
    // + capsules) and drive the render cloth from them. The accurate path; on by default.

    auto* useGame = new QCheckBox(QStringLiteral("Use game cloth data"), m_physPanel);
    useGame->setStyleSheet(QStringLiteral("QCheckBox{color:#ccc;}"));
    useGame->setToolTip(QStringLiteral(
        "Drive bone-tracking, stiffness, friction + drag from each equipped piece's real Cloth "
        "definition (Cloth/*.clt.json / dmClothTuningMirror), so every armor set matches the "
        "game. Uncheck to tune the behavioural sliders manually."));
    useGame->setChecked(QSettings().value(QStringLiteral("wardrobe2/cloth/useGameData"), true).toBool());
    connect(useGame, &QCheckBox::toggled, this, [this, gameWidgets](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe2/cloth/useGameData"), on);
        for (QWidget* w : *gameWidgets) w->setEnabled(!on);   // grey out the game-driven rows
        applyClothParams();
        refreshGameDrivenSliders();   // show the real .clt numbers (or the manual ones when off)
    });
    pl->addWidget(useGame);

    // Unlocked limits: remove the slider caps so any value can be dialled past its normal range
    // (e.g. Max distance > 1, or a negative). Off by default so the ranges stay sensible.
    auto* unlocked = new QCheckBox(QStringLiteral("Unlocked limits"), m_physPanel);
    unlocked->setStyleSheet(QStringLiteral("QCheckBox{color:#ccc;}"));
    unlocked->setToolTip(QStringLiteral(
        "Let the sliders go past their normal min/max — set values like Max distance > 1, or "
        "negative. A couple of params are still clamped inside the solver for stability."));
    unlocked->setChecked(QSettings().value(QStringLiteral("wardrobe2/cloth/unlocked"), false).toBool());
    m_physUnlocked = unlocked;
    connect(unlocked, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe2/cloth/unlocked"), on);
        applyUnlockedLimits(on);
        refreshGameDrivenSliders();   // keep the real .clt numbers on the greyed rows
    });
    pl->addWidget(unlocked);

    // Each slider resets in place (no menu close) → collect a resetter per row.
    auto* resetters = new QVector<std::function<void()>>();
    m_physPanel->connect(m_physPanel, &QObject::destroyed, [resetters] { delete resetters; });
    // Slider key+scale refs so Save/Restore can capture and push values back into the sliders.
    struct SliderRef { QSlider* sld; QString key; double scale; };
    auto* sliderRefs = new QVector<SliderRef>();
    m_physPanel->connect(m_physPanel, &QObject::destroyed, [sliderRefs] { delete sliderRefs; });

    // A bold light-blue section header that groups the controls below it.
    auto section = [&](const QString& t) {
        auto* s = new QLabel(t, m_physPanel);
        s->setStyleSheet(QStringLiteral("color:#8ab4f8;font-weight:bold;margin-top:6px;"));
        pl->addWidget(s);
    };
    // A labelled slider whose int value maps to a float via /scale, persisted + live.
    // `tip` is shown as a tooltip on hover (≈1s) over the title and the slider.
    auto row = [&](const QString& key, const QString& label, int lo, int hi,
                   double scale, double def, const QString& tip, bool gameDriven = false) {
        auto* rl = new QHBoxLayout();
        auto* name = new QLabel(label, m_physPanel); name->setFixedWidth(108);
        name->setToolTip(tip);
        auto* sld = new QSlider(Qt::Horizontal, m_physPanel);
        sld->setRange(lo, hi);
        sld->setToolTip(tip);
        // Editable value field — click and type a number (Enter applies). Accepts values beyond
        // the slider's range too (the slider knob just clamps; the typed value is stored + applied).
        auto* val = new QLineEdit(m_physPanel); val->setFixedWidth(56);
        val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        val->setStyleSheet(QStringLiteral(
            "QLineEdit{background:#2c2c2c;border:1px solid #4a4a4a;border-radius:2px;color:#ddd;padding:0 2px;}"
            "QLineEdit:focus{border-color:#8ab4f8;}"));
        const double cur = QSettings().value(QStringLiteral("wardrobe2/cloth/") + key, def).toDouble();
        sld->setValue(int(qRound(cur * scale)));
        val->setText(QString::number(cur, 'g', 3));
        connect(sld, &QSlider::valueChanged, this, [this, key, val, scale](int v) {
            const double fv = v / scale;
            QSettings().setValue(QStringLiteral("wardrobe2/cloth/") + key, fv);
            val->setText(QString::number(fv, 'g', 3));
            applyClothParams();
        });
        connect(val, &QLineEdit::editingFinished, this, [this, key, val, sld, scale] {
            bool ok = false; const double fv = val->text().toDouble(&ok);
            if (!ok) { val->setText(QString::number(                          // bad input → restore
                QSettings().value(QStringLiteral("wardrobe2/cloth/") + key).toDouble(), 'g', 3)); return; }
            QSettings().setValue(QStringLiteral("wardrobe2/cloth/") + key, fv);
            { QSignalBlocker b(sld); sld->setValue(int(qRound(fv * scale))); }  // move knob (clamped), no re-persist
            val->setText(QString::number(fv, 'g', 3));
            applyClothParams();
        });
        rl->addWidget(name); rl->addWidget(sld, 1); rl->addWidget(val);
        pl->addLayout(rl);
        if (gameDriven) { *gameWidgets << name << sld << val; }   // greyed when game data drives it
        resetters->append([sld, scale, def] { sld->setValue(int(qRound(def * scale))); });
        sliderRefs->append(SliderRef{sld, key, scale});
        // Registry for live game-value display + the Unlocked-limits range toggle.
        m_physSliders.append(PhysSlider{sld, val, key, scale, lo, hi, gameDriven});
    };

    section(QStringLiteral("Tracking & motion"));
    row(QStringLiteral("tracking"),QStringLiteral("Bone tracking"), 0, 100, 100.0, d.boneTracking,
        QStringLiteral("How strongly the cloth follows its authored bone pose each frame (the game's "
                       "flBoneTrackingFactor, ~0.5-0.85). Higher = tracks the body tightly (less clipping, "
                       "stiffer); lower = floppier and more physics.\n(Driven by game data — uncheck 'Use game cloth data' to edit.)"), true);
    row(QStringLiteral("maxdist"), QStringLiteral("Max distance"),  0, 1000, 1000.0, d.maxDistance,
        QStringLiteral("Swing reach: scales the AUTHORED per-bone motion constraint (ptAttachmentLengths). "
                       "Each bone may stray attachLen×this from its skinned pose — 0 at the pinned edge up to "
                       "this at the free hem. Higher = the cape/hem swings farther; lower = tighter to the body."));
    row(QStringLiteral("damping"), QStringLiteral("Damping"),       800, 999, 1000.0, d.damping,
        QStringLiteral("Velocity retention per frame (flDampingFactor). Lower settles faster (stiffer); higher keeps more swing."));
    row(QStringLiteral("gravity"), QStringLiteral("Gravity"),       0, 400, 10000.0, -d.gravity,
        QStringLiteral("Downward pull (the 'downforce'). Higher droops more — needed for capes/horizontal "
                       "cloth to fall; vertical cloth (skirts) is barely affected since gravity runs along it."));

    section(QStringLiteral("Stiffness"));
    row(QStringLiteral("bonestiff"), QStringLiteral("Bone stiffness"), 0, 200, 1000.0, d.boneStiffness,
        QStringLiteral("How strongly the cloth bones return to their authored (skinned) shape. "
                       "Higher = the cape/skirt droops less and holds its shape; lower = hangs "
                       "freely under gravity. Raise this if the cloth sags too low."));
    row(QStringLiteral("stretch"), QStringLiteral("Stretch stiff"), 0, 100, 100.0, d.stretchStiffness,
        QStringLiteral("Structural tightness — how strongly the weave resists stretching (flStretchingStiffness). Higher = firmer."
                       "\n(Driven by game data — uncheck 'Use game cloth data' to edit.)"), true);
    row(QStringLiteral("bend"),    QStringLiteral("Bend stiff"),    0, 100, 100.0, d.bendStiffness,
        QStringLiteral("Resistance to folding/creasing (flBendingStiffness). Higher holds a stiffer, leathery shape; reduces self-clipping."
                       "\n(Driven by game data — uncheck 'Use game cloth data' to edit.)"), true);

    section(QStringLiteral("Aerodynamics"));
    row(QStringLiteral("wind"),    QStringLiteral("Wind"),          0, 100, 100.0, 0.0,
        QStringLiteral("Gentle breeze along the garment's authored self-wind direction (vSelfWind). "
                       "0 = still. Opt-in, because the game's wind magnitude doesn't map 1:1 to our per-frame units."));
    row(QStringLiteral("drag"),    QStringLiteral("Drag"),          0, 100, 100.0, 0.0,
        QStringLiteral("Air resistance — settles billowing faster (the game's flDragFactor). "
                       "Auto-set from game data when 'Use game cloth data' is on."), true);

    // (The "Preview" section — Phys Bones / Axis — is gone: those overlays live in the viewport's
    //  Overlays popup now. The mirroring in applyRigToggle is null-safe, so the Rig panel and the
    //  Overlays popup keep working with the physics-panel copies removed.)
    section(QStringLiteral("Collision"));
    auto* showCol = new QCheckBox(QStringLiteral("Show collision models"), m_physPanel);
    showCol->setStyleSheet(QStringLiteral("QCheckBox{color:#ccc;}"));
    showCol->setToolTip(QStringLiteral("Draw the authored collision capsules (orange) the cloth collides against."));
    showCol->setChecked(QSettings().value(QStringLiteral("wardrobe2/cloth/showColliders"), false).toBool());
    connect(showCol, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe2/cloth/showColliders"), on);
        if (m_view) m_view->setShowColliders(on);
    });
    pl->addWidget(showCol);

    // Live capsule-axis cycler (X/Y/Z) — for dialing in the authored-capsule orientation
    // against the collision viz, since the long-axis convention isn't stored in the data.
    auto* axisBtn = new QPushButton(m_physPanel);
    axisBtn->setStyleSheet(QStringLiteral("QPushButton{color:#ccc;text-align:left;padding:2px 6px;}"));
    axisBtn->setToolTip(QStringLiteral("Cycle the authored capsule's long axis (X/Y/Z) if its orientation looks wrong against the viz."));
    auto setAxisLabel = [axisBtn](int a) {
        static const char* const kAxisName[4] = {"X", "Y", "Z", "bone"};
        axisBtn->setText(QStringLiteral("Capsule axis: %1  (click to cycle)").arg(QLatin1String(kAxisName[a & 3])));
    };
    setAxisLabel(QSettings().value(QStringLiteral("wardrobe2/cloth/capAxis"), 3).toInt());
    connect(axisBtn, &QPushButton::clicked, this, [this, axisBtn, setAxisLabel] {
        int a = (QSettings().value(QStringLiteral("wardrobe2/cloth/capAxis"), 3).toInt() + 1) & 3;
        QSettings().setValue(QStringLiteral("wardrobe2/cloth/capAxis"), a);
        setAxisLabel(a);
        if (m_view) m_view->setCapsuleAxis(a);
    });
    pl->addWidget(axisBtn);
    row(QStringLiteral("capScale"), QStringLiteral("Capsule size"),  20, 220, 100.0, d.capsuleRadius,
        QStringLiteral("Scales ALL body-collision capsules (the orange shapes) — the game's authored "
                       "capsules on skirts/capes AND the fitted ones on plain models.\n"
                       "~0.52 (the default) is what matches the body mesh: the authored radii describe a "
                       "collision volume larger than the visible body, so 1.0 inflates it and splays "
                       "garments open. Raise a little if a piece clips; lower if it stands off the body."));
    row(QStringLiteral("margin"),  QStringLiteral("Collide margin"),0, 50, 1000.0, d.collisionMargin,
        QStringLiteral("Extra clearance kept from the body capsules. Raise if limbs poke through the cloth."));
    row(QStringLiteral("friction"),QStringLiteral("Friction"),      0, 100, 100.0, d.friction,
        QStringLiteral("Grip at body contact (flFrictionScale). Higher makes the cloth cling to the body instead of sliding off."
                       "\n(Driven by game data — uncheck 'Use game cloth data' to edit.)"), true);
    row(QStringLiteral("backstop"),QStringLiteral("Backstop"),      0, 80, 1000.0, d.backstop,
        QStringLiteral("How far the cloth may sink toward the body behind its authored (cloth-bone) "
                       "surface. Lower = the cloth-bone shape acts as a hard body collider. 0 disables it."));
    row(QStringLiteral("self"),    QStringLiteral("Self-collide"),  0, 60, 1000.0, d.selfCollision,
        QStringLiteral("Cloth thickness for self-collision — stops the cloth passing through itself. 0 disables it."));

    section(QStringLiteral("Interaction"));
    {
        // Orbiting the camera reads as spinning the model, so feed that rotation into the sim:
        // the cloth trails when you start/stop turning and fans out while you keep turning.
        auto* spinChk = new QCheckBox(QStringLiteral("React to rotation"), m_physPanel);
        spinChk->setStyleSheet(QStringLiteral("QCheckBox{color:#ccc;}"));
        spinChk->setToolTip(QStringLiteral(
            "Rotating the view swings the cloth: free parts lag behind as the turn starts and stops "
            "(momentum) and fan outward while it continues (centrifugal).\n"
            "Only the simulated parts move — pins, collision and the authored limits still apply."));
        spinChk->setChecked(QSettings().value(QStringLiteral("wardrobe2/cloth/userSpin"), false).toBool());
        connect(spinChk, &QCheckBox::toggled, this, [this, enablePhys](bool on) {
            QSettings().setValue(QStringLiteral("wardrobe2/cloth/userSpin"), on);
            if (on && !enablePhys->isChecked()) enablePhys->setChecked(true);   // can't do nothing
            applyClothParams();
        });
        pl->addWidget(spinChk);
    }
    row(QStringLiteral("spinForce"), QStringLiteral("Rotation force"), 0, 500, 100.0, d.userSpinForce,
        QStringLiteral("How strongly view rotation pushes the cloth (needs 'React to rotation'). "
                       "0 = none · 0.1 = subtle (default) · higher = exaggerated swing."));

    section(QStringLiteral("Solver"));
    row(QStringLiteral("substeps"), QStringLiteral("Sub-steps"),    1, 4, 1.0, d.subSteps,
        QStringLiteral("Physics passes per frame. More = far steadier under fast motion (spins, "
                       "animation snaps) and much less clipping through the body; costs CPU "
                       "roughly in proportion. 2 is a good balance."));
    row(QStringLiteral("iters"),   QStringLiteral("Iterations"),    1, 20, 1.0, d.iterations,
        QStringLiteral("Constraint solver passes per frame (nIterations). More = stiffer and more stable, but costs more CPU."));

    // Apply the initial enabled/disabled state of the game-driven sliders.
    { const bool gOn = useGame->isChecked(); for (QWidget* w : *gameWidgets) w->setEnabled(!gOn); }

    auto* reset = new QPushButton(QStringLiteral("Reset to defaults"), m_physPanel);
    reset->setToolTip(QStringLiteral("Restore all cloth-physics values to their defaults (keeps the menu open)."));
    connect(reset, &QPushButton::clicked, this, [resetters] {
        for (const auto& r : *resetters) r();   // sets each slider → persists + applies live
    });
    pl->addWidget(reset);

    // ── Save / Restore your own preset ───────────────────────────────────────────
    auto* presetRow = new QHBoxLayout();
    auto* saveBtn = new QPushButton(QStringLiteral("Save preset"), m_physPanel);
    saveBtn->setToolTip(QStringLiteral("Save the current cloth settings as your preset (overwrites any previous one)."));
    auto* restoreBtn = new QPushButton(QStringLiteral("Restore preset"), m_physPanel);
    restoreBtn->setToolTip(QStringLiteral("Load your saved preset back into the sliders."));
    restoreBtn->setEnabled(QSettings().value(QStringLiteral("wardrobe2/cloth/preset/exists"), false).toBool());
    connect(saveBtn, &QPushButton::clicked, this, [sliderRefs, restoreBtn] {
        QSettings s;
        for (const SliderRef& r : *sliderRefs)
            s.setValue(QStringLiteral("wardrobe2/cloth/preset/") + r.key,
                       s.value(QStringLiteral("wardrobe2/cloth/") + r.key));
        s.setValue(QStringLiteral("wardrobe2/cloth/preset/exists"), true);
        restoreBtn->setEnabled(true);
    });
    connect(restoreBtn, &QPushButton::clicked, this, [this, sliderRefs] {
        QSettings s;
        if (!s.value(QStringLiteral("wardrobe2/cloth/preset/exists"), false).toBool()) return;
        for (const SliderRef& r : *sliderRefs) {
            const QString pk = QStringLiteral("wardrobe2/cloth/preset/") + r.key;
            if (!s.contains(pk)) continue;
            r.sld->setValue(int(qRound(s.value(pk).toDouble() * r.scale)));  // → persists + applies live
        }
    });
    presetRow->addWidget(saveBtn); presetRow->addWidget(restoreBtn);
    pl->addLayout(presetRow);

    // Apply the saved "unlocked limits" state, then show the real game values on the greyed rows.
    if (m_physUnlocked && m_physUnlocked->isChecked()) applyUnlockedLimits(true);
    refreshGameDrivenSliders();
}

// Show the actual authored .clt values on the game-driven (greyed) rows when "Use game cloth data"
// is on, so you can SEE what the game sets. When off, restore the row's own manual value. Display
// only — never persists (signals blocked), so toggling the checkbox brings the manual values back.
void WardrobeTab2::refreshGameDrivenSliders()
{
    const bool useGame = m_clothTuningFound
        && QSettings().value(QStringLiteral("wardrobe2/cloth/useGameData"), true).toBool();
    auto gv = [&](const QString& k) -> double {
        if (k == QLatin1String("tracking")) return m_gct.boneTrack;
        if (k == QLatin1String("stretch"))  return m_gct.stretch;
        if (k == QLatin1String("bend"))     return m_gct.bend;
        if (k == QLatin1String("friction")) return m_gct.friction;
        if (k == QLatin1String("drag"))     return m_gct.drag;
        return 0.0;
    };
    for (const PhysSlider& p : m_physSliders) {
        if (!p.game || !p.sld || !p.val) continue;
        const double v = useGame ? gv(p.key)
            : QSettings().value(QStringLiteral("wardrobe2/cloth/") + p.key,
                                p.sld->value() / p.scale).toDouble();
        QSignalBlocker b(p.sld);                       // display only — don't overwrite the setting
        p.sld->setValue(int(qRound(v * p.scale)));     // knob (clamped to range)
        p.val->setText(QString::number(v, 'g', 3));    // label shows the TRUE value, even past range
    }
}

// Widen every physics slider's range (Unlocked limits) so values can go past the normal caps and
// negative; restoring re-clamps to the design range. The stored value is re-applied either way.
void WardrobeTab2::applyUnlockedLimits(bool on)
{
    for (const PhysSlider& p : m_physSliders) {
        if (!p.sld) continue;
        QSignalBlocker b(p.sld);
        if (on) p.sld->setRange(int(-10.0 * p.scale), int(10.0 * p.scale));
        else    p.sld->setRange(p.lo, p.hi);
        const double v = QSettings().value(QStringLiteral("wardrobe2/cloth/") + p.key,
                                           p.sld->value() / p.scale).toDouble();
        p.sld->setValue(int(qRound(v * p.scale)));
        if (p.val) p.val->setText(QString::number(v, 'g', 3));
    }
}
