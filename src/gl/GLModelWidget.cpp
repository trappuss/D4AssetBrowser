#include "gl/GLModelWidget.h"
#include "model/Retarget.h"
#include "model/ModelParser.h"   // resolveClothTuning — shared per-piece tuning resolution
#include "app/Config.h"
#include <QJsonDocument>
#include <QJsonObject>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineF>
#include <QMatrix4x4>
#include <QDebug>
#include <QFont>
#include <QHash>
#include <QPainter>
#include <QPaintEvent>
#include <QVector4D>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <memory>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <climits>
#include <cmath>
#include <vector>

namespace {

// Blender-style axis-orientation gizmo: a small ball in the viewport's top-right that projects
// the world axes with the SAME yaw/pitch math paintGL uses. Clicking an axis end glides the
// camera to that axis view (centre + zoom preserved). A plain child widget — no GL state
// interplay, and it naturally receives the clicks so they never reach the orbit-drag handler.
// No Q_OBJECT (no signals/slots) → no moc needed.
class AxisGizmoOverlay : public QWidget {
public:
    explicit AxisGizmoOverlay(GLModelWidget* host) : QWidget(host), m_host(host) {
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Orbit to an axis view (click a ball)"));
        setMouseTracking(true);
    }
protected:
    struct End { QPointF p; float toward; int axis; bool pos; int idx; };   // toward > 0 = faces the camera;
                                                                            // idx = canonical id (axis*2+neg)

    QVector<End> ends() const
    {
        // Camera basis — mirrors paintGL: dir = (cp·sy, sp, cp·cy), eye = centre + dir·dist,
        // looking along -dir with world-up (0,1,0).
        const float cp = std::cos(m_host->camPitch()), sp = std::sin(m_host->camPitch());
        const float cy = std::cos(m_host->camYaw()),   sy = std::sin(m_host->camYaw());
        const QVector3D dir(cp * sy, sp, cp * cy);
        const QVector3D f = -dir;                                        // camera forward
        QVector3D r = QVector3D::crossProduct(f, QVector3D(0, 1, 0));
        if (r.lengthSquared() < 1e-6f) r = QVector3D(1, 0, 0);           // looking straight up/down
        r.normalize();
        const QVector3D u = QVector3D::crossProduct(r, f);
        const QPointF c(width() / 2.0, height() / 2.0);
        const float R = qMin(width(), height()) / 2.0f - 9.0f;
        QVector<End> out;
        static const QVector3D ax[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (int a = 0; a < 3; ++a)
            for (int s = 0; s < 2; ++s) {
                const QVector3D w = s == 0 ? ax[a] : -ax[a];
                out << End{c + QPointF(QVector3D::dotProduct(w, r) * R,
                                       -QVector3D::dotProduct(w, u) * R),
                           -QVector3D::dotProduct(w, f), a, s == 0, a * 2 + s};
            }
        return out;
    }

    // Nearest axis end within grab range, or -1. Canonical idx (axis*2+neg), for hover + clicks.
    int endAt(const QPointF& p) const
    {
        const QVector<End> e = ends();
        int best = -1; double bd = 1e9;
        for (const End& n : e) {
            const double d = QLineF(p, n.p).length();
            if (d < bd) { bd = d; best = n.idx; }
        }
        return bd <= 9.5 ? best : -1;
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        // Blender: the gizmo is quiet until the cursor comes to it — semi-transparent at rest,
        // fully lit (with its backdrop disc) on hover.
        if (!m_hover) g.setOpacity(0.35);
        g.setPen(Qt::NoPen);
        if (m_hover) {
            g.setBrush(QColor(255, 255, 255, 26));   // backdrop disc only while engaged
            g.drawEllipse(rect().adjusted(1, 1, -1, -1));
        }
        static const QColor col[3] = {QColor(226, 84, 77),     // X — Blender red
                                      QColor(118, 183, 66),    // Y — Blender green
                                      QColor(74, 132, 222)};   // Z — Blender blue
        QVector<End> e = ends();
        std::sort(e.begin(), e.end(), [](const End& a, const End& b) { return a.toward < b.toward; });
        const QPointF c(width() / 2.0, height() / 2.0);
        for (const End& n : e) {
            const bool hot = m_hover && n.idx == m_hotEnd;   // the ball under the cursor
            if (n.pos) { g.setPen(QPen(col[n.axis], 1.6)); g.drawLine(c, n.p); }
            g.setPen(Qt::NoPen);
            if (n.pos) {
                const qreal rad = hot ? 8.0 : 6.5;
                g.setBrush(hot ? col[n.axis].lighter(125) : col[n.axis]);
                g.drawEllipse(n.p, rad, rad);
                if (hot) {   // white ring — unmistakably "this one"
                    g.setPen(QPen(QColor(255, 255, 255, 220), 1.3));
                    g.setBrush(Qt::NoBrush);
                    g.drawEllipse(n.p, rad, rad);
                    g.setPen(Qt::NoPen);
                }
                g.setPen(QColor(25, 25, 25));
                QFont f = g.font(); f.setPointSizeF(7.5); f.setBold(true); g.setFont(f);
                g.drawText(QRectF(n.p.x() - rad, n.p.y() - rad, rad * 2, rad * 2), Qt::AlignCenter,
                           QString(QLatin1Char('X' + n.axis)));
            } else if (hot) {   // hovered negative end: fills and labels itself (−X / −Y / −Z)
                g.setBrush(col[n.axis].lighter(115));
                g.drawEllipse(n.p, 7.5, 7.5);
                g.setPen(QPen(QColor(255, 255, 255, 220), 1.3));
                g.setBrush(Qt::NoBrush);
                g.drawEllipse(n.p, 7.5, 7.5);
                g.setPen(QColor(25, 25, 25));
                QFont f = g.font(); f.setPointSizeF(6.5); f.setBold(true); g.setFont(f);
                g.drawText(QRectF(n.p.x() - 7.5, n.p.y() - 7.5, 15, 15), Qt::AlignCenter,
                           QStringLiteral("−") + QString(QLatin1Char('X' + n.axis)));
            } else {   // negative end at rest: hollow, dimmer, no label (Blender's look)
                g.setBrush(col[n.axis].darker(230));
                g.drawEllipse(n.p, 5.0, 5.0);
                g.setPen(QPen(col[n.axis].darker(140), 1.2));
                g.setBrush(Qt::NoBrush);
                g.drawEllipse(n.p, 5.0, 5.0);
            }
        }
    }

    void enterEvent(QEnterEvent*) override { m_hover = true;  update(); }
    void leaveEvent(QEvent*) override      { m_hover = false; m_hotEnd = -1; update(); }

    void mouseMoveEvent(QMouseEvent* ev) override
    {
        const int hot = endAt(ev->position());
        if (hot != m_hotEnd) { m_hotEnd = hot; update(); }
    }

    void mousePressEvent(QMouseEvent* ev) override
    {
        const QPointF p = ev->position();
        const QVector<End> e = ends();
        int best = -1; double bd = 1e9;
        for (int i = 0; i < e.size(); ++i) {
            const double d = QLineF(p, e[i].p).length();
            if (d < bd) { bd = d; best = i; }
        }
        if (best < 0 || bd > 9.5) return;   // not on a ball — ignore (widget swallows the click)
        // Look FROM that axis end toward the centre: camera dir = the clicked axis.
        // dir = (cp·sy, sp, cp·cy)  ⇒  X:(yaw ±π/2, 0) · Z:(yaw 0/π, 0) · Y: pitch ±(~89°).
        constexpr float kPi = 3.14159265f, kTop = 1.55f;
        float yaw = m_host->camYaw(), pitch = 0.0f;
        const bool pos = e[best].pos;
        switch (e[best].axis) {
        case 0: yaw = pos ? kPi / 2 : -kPi / 2; break;
        case 1: pitch = pos ? kTop : -kTop;     break;   // top/bottom keep the current yaw
        case 2: yaw = pos ? 0.0f : kPi;         break;
        }
        m_host->orbitToAxis(yaw, pitch);
        ev->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent* ev) override
    {
        // Blender: the gizmo doubles as the ortho/perspective switch. Double-click the ball's
        // BODY (not an axis end — those orbit) to toggle projection.
        const QVector<End> e = ends();
        for (const End& n : e)
            if (QLineF(ev->position(), n.p).length() <= 9.5) { ev->accept(); return; }
        m_host->setOrthographic(!m_host->orthographic());
        setToolTip(m_host->orthographic()
                       ? QStringLiteral("Orthographic — double-click for perspective")
                       : QStringLiteral("Orbit to an axis view (click a ball) · double-click: ortho"));
        ev->accept();
    }

private:
    GLModelWidget* m_host;
    bool m_hover  = false;   // cursor is over the gizmo → full opacity + backdrop disc
    int  m_hotEnd = -1;      // canonical idx of the hovered axis ball, or -1
};

// Transparent overlay child that paints bone-name labels at pre-projected screen positions. No
// Q_OBJECT (no signals/slots) so it needs no moc; click-through so it never eats viewport input.
class BoneLabelOverlay : public QWidget {
public:
    explicit BoneLabelOverlay(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
    }
    struct Item { QPoint pos; QString text; bool translated; };
    QVector<Item> items;
protected:
    void paintEvent(QPaintEvent*) override {
        if (items.isEmpty()) return;
        QPainter g(this);
        g.setRenderHint(QPainter::TextAntialiasing, true);
        QFont f = g.font(); f.setPointSizeF(8.0); g.setFont(f);
        for (const auto& it : items) {
            const QRect r(it.pos + QPoint(4, -7), QSize(240, 14));
            g.setPen(QColor(0, 0, 0, 190));            // shadow for legibility over any background
            g.drawText(r.translated(1, 1), Qt::AlignLeft | Qt::AlignVCenter, it.text);
            // Identified (translated) bones = cyan-green so they stand out; raw bone_<hash> = muted amber.
            g.setPen(it.translated ? QColor(110, 235, 165) : QColor(190, 180, 150));
            g.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, it.text);
        }
    }
};

const char* kVert = R"(#version 450 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform int   uFurEnabled;       // shell-fur pass active
uniform float uFurShell;         // 0 = skin surface, (0,1] = this shell's height
uniform float uFurExtrusion;     // max fur length (model units)
uniform float uFurGravity;       // outer shells sag downward (model -Y)
uniform float uFurCurl;          // lateral comb along the tangent (model units)
uniform float uFurCoverage;      // density threshold: fur grows where FurMask.R >= this (lower = fuller)
uniform sampler2D uFurMask;      // R = per-vertex fur length / density
uniform int   uFxMode;           // mesh-FX pass active
uniform float uFxWobble;         // FX vertex undulation amount (model units)
uniform float uTime;
uniform sampler2D uFxNoise;
// Selection silhouette: shifts the projected vertex by a whole number of screen pixels so the
// outline has CONSTANT width. Multiplying by w cancels the perspective divide, so the offset is
// in NDC/screen space, not model space — unlike a normal-push hull it cannot tear on the split
// normals D4 meshes carry at UV seams.
uniform vec2  uNdcOffset;
out vec3 vNormal;
out vec2 vUV;
out vec3 vTangent;
out vec3 vWorldPos;
out float vFurShell;
void main() {
    vNormal = mat3(uModel) * aNormal;
    vTangent = mat3(uModel) * aTangent;
    vUV = aUV;
    vec3 pos = aPos;
    vFurShell = 0.0;
    if (uFurEnabled == 1 && uFurShell > 0.0) {
        float m = texture(uFurMask, aUV).r;                   // fur coverage / length map
        float len = (m < uFurCoverage) ? 0.0 : mix(0.55, 1.0, m);   // present fur lofts 55–100% (no flat patches)
        float s = uFurShell;
        vec3 disp = aNormal * (uFurExtrusion * s * len);      // extrude along the normal
        float tl = length(aTangent);                          // comb sideways toward the tips
        if (tl > 1e-4) disp += (aTangent / tl) * (uFurCurl * s * s * len);
        disp.y -= uFurGravity * s * s * len;                  // quadratic droop toward the tips
        pos += disp;
        vFurShell = s;
    }
    if (uFxMode == 1 && uFxWobble > 0.0) {                     // FX undulation (vfx_*_vertexAnim)
        float w = texture(uFxNoise, aUV*2.0 + vec2(0.0, uTime*0.15)).r;
        pos += aNormal * (uFxWobble * (w - 0.5) * 2.0);
    }
    vWorldPos = vec3(uModel * vec4(pos, 1.0));
    gl_Position = uMVP * vec4(pos, 1.0);
    gl_Position.xy += uNdcOffset * gl_Position.w;   // silhouette jitter (zero for every other pass)
}
)";

const char* kFrag = R"(#version 450 core
in vec3 vNormal;
in vec2 vUV;
in vec3 vTangent;
in vec3 vWorldPos;
out vec4 FragColor;
uniform int  uSolid;        // 1 = emit uBase flat and skip all lighting (selection silhouette)
uniform vec3 uLightDir;     // key light direction (world), surface → light
uniform vec3 uViewPos;      // camera world position
uniform vec3 uBase;
uniform sampler2D uTex;        uniform int uHasTex;
uniform sampler2D uNormalTex;  uniform int uHasNormal;
uniform int   uPbr;
uniform float uMetal;
uniform float uRough;
uniform sampler2D uOrmTex;     uniform int uHasOrm;       // AO(R)/rough(G)/metal(B)
uniform sampler2D uEmissiveTex;uniform int uHasEmissive;
uniform float uEmisMul;   // authored emissive multiplier (MaterialValue "emissive multiplier")
uniform float uEmisScale; // global emissive-intensity slider (tames tonemap whiteout)
uniform vec3  uEmisColor; // authored "emissive color" (the emissive texture is often a mask)
uniform int   uIsEye;     // 1 = eyeball part (wet cornea: forced low roughness for a live catchlight)
uniform float uEyeRough;  // EyeColor flIrisRoughness (cornea wetness)
// Up-to-3 tiled detail maps; one is selected PER TEXEL by the dye-mask region (D4 doesn't blend
// them — leather region → leather map, fabric → fabric map, metal → none).
uniform sampler2D uDetN0, uDetN1, uDetN2;  uniform int uHasDetN0, uHasDetN1, uHasDetN2;
uniform sampler2D uDetR0, uDetR1, uDetR2;  uniform int uHasDetR0, uHasDetR1, uHasDetR2;
uniform int uHasDetailN;     // any detail-normal map present
uniform int uHasDetailR;     // any detail-rough map present
uniform float uDetailNInt;   // per-material authored detail-NORMAL intensity (dominant map)
uniform float uDetailRInt;   // per-material authored detail-ROUGHNESS intensity (dominant map)
uniform float uDetailScale;  // (legacy single tiling — kept for compatibility)
uniform vec3  uDetailScales; // per-map tiling from ptTexAnim flUScale (x/y/z = map0/1/2)
uniform int   uDetailMetalLayer; // detail-map index that is a Metal library map (-1 = none)
uniform ivec4 uZoneMap;       // dye-zone band index → detail-map layer (-1 none) [Detail-maps panel]
uniform vec4  uDyeBands;      // dye-mask value band centres (default 0.063/0.345/0.596/0.831)
uniform float uMetalThresh;   // metalness above which a texel is treated as metal (default 0.5)
uniform int   uMetalRoute;    // -2 auto (uDetailMetalLayer), -1 off, 0/1/2 force a map
uniform float uDetailNormalMul;  // global detail-normal strength (defaults 1.0)
uniform float uDetailRoughMul;   // global detail-roughness strength (defaults 1.0)
uniform float uDetailROffset;    // per-material authored roughness bias (Σ present maps' offsets)
uniform float uDetailColorAdd;   // "Color Add Intensity - Detail Map" — detail faintly tints albedo
uniform sampler2D uTransTex;   uniform int uHasTrans;     // translucency / SSS colour
uniform sampler2D uMaskTex;    uniform int uHasMask;      // MASK_PRIMARY
uniform sampler2D uDyeMaskTex; uniform int uHasDyeMask;   // DYE_MASK (R = dyeable weight)
uniform sampler2D uDyeRampTex; uniform int uHasDyeRamp;   // DYE_RAMP (R = gradient position)
uniform sampler2D uDyeGradTex; uniform int uHasDyeGrad;   // real dye gradient (arColorSamples)
uniform int   uDyeMode;        // 0 = custom colour, 1 = real dye gradient
uniform vec3  uDyeColor[4];    // dye colour per material region (custom mode)
uniform int   uDyeRegion;      // which dye colour this part uses (0..3)
uniform int   uIsHair;         // per-part: anisotropic specular
uniform vec3  uHairParams;     // hero_hair MaterialValues: (Hair Roughness, Hair Specular, Highlight Shift)
uniform int   uIsSkin;         // per-part: subsurface wrap
uniform int   uIsHead;         // per-part: face skin (warm Fresnel rim); body skin has none (D4 data)
uniform int   uIsCloth;        // per-part: grazing sheen
// Shell fur (drawn as N concentric extruded layers; this shader runs per layer).
uniform int   uFurEnabled;
uniform sampler2D uFurNoise;   uniform int uHasFurNoise;   // dual-noise strand pattern
uniform sampler2D uFurMask;    // R = density / length (MASK_PRIMARY)
uniform float uFurCoverage;    // density threshold: shells grow where FurMask.R >= this (lower = fuller)
uniform float uFurTiling;      // strand noise tiling
uniform float uFurAniso;       // anisotropic sheen strength (Fur Aniso Strength)
uniform float uFurRootRough, uFurTipRough;   // roughness root→tip
uniform float uFurSecondary;   // dual-noise blend (Fur Secondary Noise Strength)
uniform vec3  uFurRootColor, uFurTipColor;   // albedo multiplier root→tip
in float vFurShell;
// Mesh FX (vfx_actor_*): unlit, alpha-blended, scrolling-UV, fresnel-edged, two-sided.
uniform int   uFxMode;
uniform sampler2D uFxNoise;    uniform int uHasFxNoise;
uniform float uFxTiling;
uniform vec2  uFxScroll;
uniform float uFxIntensity;     // authored Color Intensity × Bright multiplier (emissive)
uniform float uFxAlpha;         // authored Alpha Brightness Global (opacity)
uniform float uFxSaturation;    // authored Color Saturation
uniform float uFxFresnel;       // authored Fresnel Slope (edge exponent)
uniform float uTime;
uniform int   uFDetail, uFSubsurf, uFHair, uFIbl, uFMask, uFTonemap, uFDye;
uniform int   uSpecAA;   // geometric specular anti-aliasing toggle (Preview Settings)
uniform int   uViewChannel;   // 0 shaded · 1 base colour · 2 normal · 3 rough · 4 metal · 5 AO · 6 emissive
uniform float uSkinCurv;      // pre-integrated-skin curvature scale (= model radius)
uniform float uExposure;
uniform int   uColorGrade;    // 1 = apply the post-tonemap colour grade (off by default)
uniform float uCgContrast;    // S-curve contrast around 0.5 pivot (~1.05)
uniform float uCgSat;         // saturation (~1.10)
uniform float uCgWarmth;      // split-tone: warm shadows / cool highlights (~0.03)
uniform sampler2D uLut;       // real D4 colour-grade LUT (16³ unwrapped to 256×16), when loaded
uniform int   uHasLut;        // 1 = a valid LUT is bound → use it instead of the stylised grade
uniform vec3  uEnvSky, uEnvHor, uEnvGnd;   // environment gradient
uniform vec3  uLightCol;                   // KEY light colour × intensity
uniform vec3  uRimDir,  uRimCol;           // cool back rim  (surface→light dir, colour×intensity)
uniform vec3  uFillDir, uFillCol;          // cool front fill (surface→light dir, colour×intensity)
uniform float uAmbScale;                   // hemisphere-ambient (IBL) multiplier
uniform samplerCube uReflCube;             // real D4 reflection probe (prefiltered HDR cubemap)
uniform int   uHasReflCube;                // 1 = sample uReflCube for ambient specular
uniform float uReflMaxMip;                 // top mip index of uReflCube (roughness → mip)
uniform float uReflStrength;               // ambient-specular (reflection) intensity multiplier
uniform float uSkinWarm;                   // skin SSS red-bleed amount (0 = none, 1 = default)
uniform float uSssStrength;                // skin subsurface strength (0 = plain Lambert, 1 = full SSS)
uniform float uWetness;                    // rain-slick: darker albedo + sharper (glossier) reflection
uniform float uSnow;                       // snow dusting on upward-facing surfaces (D4 'Use Snowiness')
uniform sampler2D uShadowMap;              // depth from the key light's POV (self-shadowing)
uniform mat4  uLightMVP;
uniform int   uShadowOn;
uniform float uShadowStr;                  // shadow darkness (authored pathShadowIntensity)
uniform float uShadowSoft;                 // PCF radius in texels (softness)
uniform float uShadowBias;
uniform float uShadowNBias;                // world-space normal offset (slope-aware acne fix)
uniform float uShadowSize;                 // shadow-map resolution
uniform mat4  uMVP;                        // view-projection (model = identity) — projects SSAO samples
uniform sampler2D uPosTex;                 // SSAO G-buffer: world-space position (xyz), a=1 geometry
uniform int   uSsaoOn;
uniform float uSsaoStr;                    // SSAO darkness multiplier
uniform float uSsaoRad;                    // SSAO sample radius (world units)
const float PI = 3.14159265359;

float distGGX(float NoH, float a) { float a2=a*a; float d=NoH*NoH*(a2-1.0)+1.0; return a2/max(PI*d*d,1e-7); }
float gSchlick(float x, float k) { return x/(x*(1.0-k)+k); }
float gSmith(float NoV, float NoL, float r) { float k=(r+1.0); k=k*k/8.0; return gSchlick(NoV,k)*gSchlick(NoL,k); }
vec3 fres(float c, vec3 F0) { return F0 + (1.0-F0)*pow(clamp(1.0-c,0.0,1.0),5.0); }
// Roughness-aware Fresnel for ambient reflection (so rough metals don't over-reflect at grazing).
vec3 fresRough(float c, vec3 F0, float r) {
    return F0 + (max(vec3(1.0-r), F0) - F0) * pow(clamp(1.0-c,0.0,1.0),5.0);
}
// Karis split-sum environment BRDF (the analytic "DFG" LUT) → scale/bias for ambient specular.
vec2 envDFG(float NoV, float r) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.040, -0.04);
    vec4 rr = r * c0 + c1;
    float a004 = min(rr.x*rr.x, exp2(-9.28*NoV))*rr.x + rr.y;
    return vec2(-1.04, 1.04) * a004 + rr.zw;
}
// Lagarde specular occlusion: AO shouldn't fully kill grazing reflections.
float specOcclusion(float NoV, float ao, float r) {
    return clamp(pow(NoV + ao, exp2(-16.0*r - 1.0)) - 1.0 + ao, 0.0, 1.0);
}
// One directional rig light (key/rim/fill): energy-conserving Cook-Torrance with the same
// multiscatter compensation as the key. specMul lets fur damp its hard highlight (avoids a
// white sheen line). Returns the light's diffuse+specular contribution; zero colour → no-op.
vec3 shadeLight(vec3 N, vec3 V, vec3 Ldir, vec3 lcol, vec3 albedo, vec3 F0,
                float rough, float metal, float specMul) {
    if (dot(lcol, lcol) < 1e-8) return vec3(0.0);
    vec3 L = normalize(Ldir);
    float NoL = max(dot(N, L), 0.0);
    if (NoL <= 0.0) return vec3(0.0);
    vec3 H = normalize(V + L);
    float NoV = max(dot(N, V), 1e-4), NoH = max(dot(N, H), 0.0), VoH = max(dot(V, H), 0.0);
    float D = distGGX(NoH, rough*rough);
    float G = gSmith(NoV, NoL, rough);
    vec3  F = fres(VoH, F0);
    vec3 spec = (D*G*F)/max(4.0*NoV*NoL, 1e-4);
    vec2 dfg = envDFG(NoV, rough);
    spec *= 1.0 + F0 * (1.0/max(dfg.x + dfg.y, 1e-3) - 1.0);
    spec *= specMul;
    vec3 kd = (vec3(1.0) - F)*(1.0 - clamp(metal, 0.0, 1.0));
    return (kd*albedo/PI + spec) * lcol * NoL;
}
// Key-light shadow: project the world position into the light's depth map and PCF-compare.
// Returns a lit factor 1..(1-strength). 1 = fully lit; lower = in shadow.
float keyShadow(vec3 worldPos, vec3 nrm) {
    if (uShadowOn == 0) return 1.0;
    vec4 lp = uLightMVP * vec4(worldPos + nrm * uShadowNBias, 1.0);
    vec3 p = lp.xyz / lp.w * 0.5 + 0.5;
    if (p.z > 1.0 || p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) return 1.0;   // outside the map
    float texel = 1.0 / max(uShadowSize, 1.0);
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
        float d = texture(uShadowMap, p.xy + vec2(x, y) * texel * uShadowSoft).r;
        lit += (p.z - uShadowBias <= d) ? 1.0 : 0.0;
    }
    lit /= 9.0;
    return 1.0 - (1.0 - lit) * clamp(uShadowStr, 0.0, 1.0);
}
)" R"(
// Screen-space ambient occlusion: sample a hemisphere of world-space offsets around the
// fragment, project each into the world-position G-buffer, and count how many land behind a
// nearer surface. Returns an ambient multiplier in 1..(1-strength).
float ssaoFactor(vec3 P, vec3 Nw) {
    if (uSsaoOn == 0) return 1.0;
    const vec3 kern[16] = vec3[16](
        vec3( 0.05,  0.02, 0.06), vec3(-0.11,  0.07, 0.10), vec3( 0.14, -0.09, 0.13),
        vec3(-0.07, -0.15, 0.17), vec3( 0.20,  0.13, 0.21), vec3(-0.22,  0.05, 0.25),
        vec3( 0.09, -0.24, 0.29), vec3(-0.28, -0.14, 0.33), vec3( 0.31,  0.22, 0.38),
        vec3(-0.12,  0.36, 0.42), vec3( 0.38, -0.20, 0.47), vec3(-0.40, -0.30, 0.52),
        vec3( 0.18,  0.48, 0.57), vec3(-0.52,  0.16, 0.62), vec3( 0.44, -0.44, 0.68),
        vec3(-0.30,  0.55, 0.75));
    float rnd = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    float ca = cos(rnd * 6.2831853), sa = sin(rnd * 6.2831853);
    vec3 up = abs(Nw.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, Nw));
    vec3 B = cross(Nw, T);
    float occ = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec3 k = kern[i];
        vec3 kr = vec3(k.x*ca - k.y*sa, k.x*sa + k.y*ca, k.z);   // rotate about the normal
        vec3 sPos = P + (T*kr.x + B*kr.y + Nw*kr.z) * uSsaoRad;
        vec4 clip = uMVP * vec4(sPos, 1.0);
        if (clip.w <= 0.0) continue;
        vec2 suv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) continue;
        vec4 sc = texture(uPosTex, suv);
        if (sc.a < 0.5) continue;                               // sample hit the background
        float sampleDist = distance(uViewPos, sPos);
        float sceneDist  = distance(uViewPos, sc.xyz);
        float rangeCheck = smoothstep(0.0, 1.0, uSsaoRad / max(abs(sampleDist - sceneDist), 1e-4));
        occ += (sceneDist < sampleDist - 0.02 * uSsaoRad) ? rangeCheck : 0.0;
    }
    return clamp(1.0 - (occ / 16.0) * uSsaoStr, 0.0, 1.0);
}
// Hemisphere environment: sky tint up, ground tint down, by world-up component.
vec3 envColor(vec3 dir) {
    float t = clamp(dir.y*0.5+0.5, 0.0, 1.0);
    return (t>0.5) ? mix(uEnvHor,uEnvSky,(t-0.5)*2.0) : mix(uEnvGnd,uEnvHor,t*2.0);
}
// Dye: classify one DyeMask texel to its zone colour (or base where undyed).
vec3 dyeZoneColor(vec2 uv, float sh, vec3 baseRgb) {
    float mv = textureLod(uDyeMaskTex, uv, 0.0).r;
    if (mv <= 0.02) return baseRgb;
    float bands[4] = float[4](0.063, 0.345, 0.596, 0.831);
    int zone = 0; float best = 2.0;
    for (int k = 0; k < 4; ++k) { float d = abs(mv - bands[k]); if (d < best) { best = d; zone = k; } }
    return clamp(uDyeColor[zone] * sh, 0.0, 1.0);
}

)" R"(
void main() {
    // Flat unlit fill for the selection silhouette — the outline must be one solid colour, not a
    // shaded surface, or the ring reads as another lit copy of the mesh.
    if (uSolid==1) {
        // Same thresholds as the opaque cutout below (no fwidth coverage — the silhouette wants a
        // hard in/out decision), so the outline follows the visible strands, not the card quad.
        if (uHasTex==1 && texture(uTex, vUV).a < ((uIsHair==1) ? 0.16 : 0.35)) discard;
        FragColor = vec4(uBase, 1.0);
        return;
    }
    vec4 base = (uHasTex==1) ? texture(uTex, vUV) : vec4(uBase, 1.0);
    // Mesh FX: unlit emissive, alpha = texture-alpha × scrolling noise × fresnel edge. Drawn in
    // a separate blended, two-sided pass; bypasses lighting and the opaque alpha cutout below.
    if (uFxMode==1) {
        vec3  Nfx = normalize(vNormal);
        vec3  Vfx = normalize(uViewPos - vWorldPos);
        float edge = pow(1.0 - clamp(abs(dot(Nfx, Vfx)), 0.0, 1.0), uFxFresnel);
        float a = (uHasTex==1) ? base.a : 1.0;
        if (uHasFxNoise==1) {
            float n1 = texture(uFxNoise, vUV*uFxTiling + uTime*uFxScroll).r;
            float n2 = texture(uFxNoise, vUV*uFxTiling*1.7 - uTime*uFxScroll*0.6).g;
            a *= mix(n1, n1*n2, 0.5);
        }
        a *= mix(0.35, 1.0, edge) * uFxAlpha;      // edge fade × authored Alpha Brightness
        a = clamp(a, 0.0, 1.0);
        if (a < 0.01) discard;
        vec3 fxCol = base.rgb * uFxIntensity;      // emissive colour × Color Intensity
        float luma = dot(fxCol, vec3(0.299, 0.587, 0.114));
        fxCol = max(mix(vec3(luma), fxCol, uFxSaturation), 0.0);   // authored Color Saturation
        FragColor = vec4(fxCol, a);
        return;
    }
    float cutCov = 1.0;                          // alpha-cutout coverage (1 = solid)
    if (uHasTex==1) {
        // Smooth alpha test: turn the hard cutout into screen-space coverage so alpha-to-coverage
        // (MSAA) anti-aliases the edge instead of a jagged fringe. Hair keeps fainter wisps: a
        // lower threshold (thin strand tips survive) + a WIDER fade band so the tips dither out
        // softly over several pixels (flyaways) rather than clipping to a hard card silhouette.
        float thr = (uIsHair==1) ? 0.16 : 0.35;
        float aa  = max(fwidth(base.a), 1e-4);
        if (uIsHair==1) aa = max(aa, 0.10);      // broaden the soft tip band for wispy hair
        cutCov = clamp((base.a - thr) / aa + 0.5, 0.0, 1.0);
        if (cutCov <= 0.0) discard;
    }
    // Shell-fur strand test: each strand has a height from the (dual) noise; on shell layer
    // vFurShell it only exists where the strand is taller than this layer, so outer shells
    // become sparse → tapered strands. Density (furMask) gates where fur grows at all. The
    // soft band near the strand end (furCover) feeds alpha-to-coverage → anti-aliased tips.
    float furCover = 1.0;
    vec2  furJit = vec2(0.0);   // per-strand normal scatter (breaks the coherent specular streak)
    if (uFurEnabled==1 && vFurShell > 0.0) {
        float dens = texture(uFurMask, vUV).r;
        if (dens < uFurCoverage) discard;                  // FurMask gates WHERE fur grows (uFurCoverage = threshold)
        float n1 = texture(uFurNoise, vUV*uFurTiling).r;
        float n2 = texture(uFurNoise, vUV*uFurTiling*2.17 + vec2(0.37,0.11)).g;
        // Strand height comes from the noise alone (full 0..1 range) so fur stays DENSE even
        // where the mask is mid-grey — multiplying by the mask made those areas sparse/patchy.
        float strand = n1 * mix(1.0, n2, uFurSecondary);
        furCover = smoothstep(0.0, 0.14, strand - vFurShell);
        if (furCover <= 0.0) discard;
        furJit = (vec2(n1, n2) * 2.0 - 1.0) * vFurShell;   // more scatter toward the tips
    }
    vec3 N = normalize(vNormal);
    bool furShell = (uFurEnabled==1 && vFurShell > 0.0);
    // Smooth vertex-tangent basis — no per-pixel derivative artifacts (a full cotangent frame
    // speckled fur and drew black outlines along alpha / thin two-sided card edges). Handedness
    // is corrected for MIRRORED UV islands via the SIGN of the UV Jacobian determinant, which
    // still removes the dark normal-map seam along symmetry lines while keeping thin/alpha/fur
    // geometry clean (only a sign is taken from the derivatives, never the frame direction).
    vec3 Tv = vTangent - N*dot(N, vTangent);
    if (dot(Tv, Tv) < 1e-8) Tv = cross(N, vec3(0.0, 1.0, 0.0));
    Tv = normalize(Tv);
    vec3 Bv = cross(N, Tv);
    if (!furShell) {
        vec2 du1 = dFdx(vUV), du2 = dFdy(vUV);
        if (du1.x*du2.y - du1.y*du2.x < 0.0) Bv = -Bv;   // mirrored UV island → flip bitangent
    }
    mat3 tbn = mat3(Tv, Bv, N);
    vec3 T = Tv;
    vec3 B = Bv;
    if (uHasNormal==1) {
        vec2 nxy = texture(uNormalTex, vUV).xy*2.0-1.0;
        float nz = sqrt(max(0.0, 1.0-dot(nxy,nxy)));
        N = normalize(tbn * vec3(nxy, nz));
    }
    // Fur: jitter each strand's normal so the key-light highlight scatters into soft sparkle
    // instead of reinforcing into one bright line across all 16 identically-normalled shells.
    if (uFurEnabled==1 && vFurShell > 0.0)
        N = normalize(N + (T*furJit.x + B*furJit.y) * 0.7);
    float metal=uMetal, rough=uRough, ao=1.0;
    if (uHasOrm==1) { vec3 o=texture(uOrmTex,vUV).rgb; ao=o.r; rough=o.g; metal=o.b; }
    // D4 skin is SMOOTH (hero_opaque_skin "Skin Roughness" = 0.1) → a soft dewy sheen, not matte.
    // Pull skin part-way toward that so faces catch a gentle highlight instead of reading dry/chalky.
    if (uFSubsurf==1 && uIsSkin==1) {
        rough = mix(rough, 0.22, 0.55);
        // Cavity roughness (D4 'Cavity Map Roughness Blend' ~0.2): pores/creases — where the skin
        // normal tilts — read slightly rougher, breaking up the uniform dewy sheen in close-ups.
        if (uHasNormal==1) {
            vec2 nxyC = texture(uNormalTex, vUV).xy * 2.0 - 1.0;
            rough = clamp(rough + clamp(length(nxyC), 0.0, 1.0) * 0.20, 0.04, 1.0);
        }
    }
    // Detail maps: D4 picks ONE tiled detail map PER TEXEL from the dye-mask region (it does NOT
    // blend them). Classify the dye-mask value into a zone, map zone→detail layer, sample that
    // one map, and apply it (bounded normal blend + roughness offset). Metal regions get none.
    if (uPbr==1 && uFDetail==1 && (uHasDetailN==1 || uHasDetailR==1)) {
        int layer = uZoneMap[1];             // no dye mask → whatever zone-1 maps to (fallback)
        if (uHasDyeMask==1) {
            float mv = texture(uDyeMaskTex, vUV).r;
            int zone = 0; float best = 2.0;
            for (int k = 0; k < 4; ++k) { float e = abs(mv - uDyeBands[k]); if (e < best) { best = e; zone = k; } }
            layer = uZoneMap[zone];          // configurable zone→map (default zone1→0, zone2→1, zone3→2)
            if (mv <= 0.02) layer = -1;      // unmasked (bare) → no detail
        }
        // The dye mask segments dyeable REGIONS, not material types, so metal and leather can share
        // a zone. Metalness is the true metal signal: route metal texels to the material's Metal
        // detail map so they stop catching a leather grain; and a NON-metal texel must never pick
        // up the metal map the zone might have pointed it at.
        int metalLayer = (uMetalRoute == -2) ? uDetailMetalLayer : uMetalRoute;   // -2 auto, -1 off, 0..2 force
        float metalMask = 1.0;
        if (metal > uMetalThresh) {
            if (metalLayer >= 0) layer = metalLayer;                       // metal → metal detail map
            else metalMask = 1.0 - smoothstep(uMetalThresh-0.15, uMetalThresh+0.15, metal);  // no map → fade off
        } else if (metalLayer >= 0 && layer == metalLayer) {
            layer = -1;                                                    // leather must not use the metal grain
        }
        // Polished metal must not inherit leather/fabric grain — it roughens the sharp reflections
        // into a flat wash. Whenever the detail being applied is NOT the material's own metal map,
        // fade it out continuously as metalness rises (catches semi-metal the hard threshold misses).
        if (metalLayer < 0 || layer != metalLayer)
            metalMask *= 1.0 - smoothstep(0.30, 0.60, metal);
        // Effective "is this metal?" for the reflection guards below: true if the texel reads metallic
        // OR it's using the material's own metal detail map (D4 authored it as metal even when the
        // metalness map reads low). Keying on BOTH makes the guard robust across every model — a metal
        // surface keeps its reflections regardless of how its metalness happens to be authored.
        float metalG = max(metal, (metalLayer >= 0 && layer == metalLayer) ? 1.0 : 0.0);
        // Each detail map has its OWN tiling (ptTexAnim flUScale) — this is the real grain size.
        float sc = (layer == 0) ? uDetailScales.x : (layer == 1) ? uDetailScales.y : uDetailScales.z;
        vec2 uv = vUV * sc;
        vec2 dnxy = vec2(0.5); float drg = 0.5; bool okN = false, okR = false;
        // Samplers can't be dynamically indexed portably, so branch on the selected layer.
        if (layer == 0)      { if (uHasDetN0==1) { dnxy = texture(uDetN0, uv).xy; okN = true; }
                               if (uHasDetR0==1) { drg  = texture(uDetR0, uv).g;  okR = true; } }
        else if (layer == 1) { if (uHasDetN1==1) { dnxy = texture(uDetN1, uv).xy; okN = true; }
                               if (uHasDetR1==1) { drg  = texture(uDetR1, uv).g;  okR = true; } }
        else if (layer == 2) { if (uHasDetN2==1) { dnxy = texture(uDetN2, uv).xy; okN = true; }
                               if (uHasDetR2==1) { drg  = texture(uDetR2, uv).g;  okR = true; } }
        if (okN && metalMask > 0.001) {
            vec2 dn = dnxy * 2.0 - 1.0;
            float dz = sqrt(max(0.0, 1.0 - dot(dn, dn)));
            vec3 Nd = normalize(T*dn.x + B*dn.y + N*dz);
            // Ease the detail normal on metal: a heavy brushed pattern scatters the plate's mirror
            // reflection and reads flat. Keep some brush for character, but let metal stay reflective.
            float detN = clamp(uDetailNInt * uDetailNormalMul, 0.0, 1.0) * metalMask;
            detN *= 1.0 - 0.55 * smoothstep(0.35, 0.70, metalG);
            N = normalize(mix(N, Nd, detN));
            // Color Add: faint micro-AO from the detail — darken where the detail normal tilts
            // (grooves/grain), scaled by the authored Color Add Intensity. Subtle by design.
            if (uDetailColorAdd > 0.001)
                base.rgb *= 1.0 - uDetailColorAdd * (1.0 - dz) * metalMask * 0.6;
        }
        if (okR) {
            float rs = clamp(uDetailRInt, 0.0, 4.0) * uDetailRoughMul;
            float dRough = ((drg - 0.5) * rs + uDetailROffset) * metalMask;
            // Metal keeps its reflections: a brushed-metal detail may SMOOTH the plate but must not
            // roughen it into a flat matte. Cap positive (roughening) detail on metallic texels; let
            // it smooth freely, and leave non-metal (leather/fabric) to roughen normally.
            if (dRough > 0.0) dRough *= 1.0 - 0.85 * smoothstep(0.35, 0.70, metalG);
            rough = clamp(rough + dRough, 0.04, 1.0);
        }
    }
    float furAO = 1.0;
    if (uFurEnabled==1 && vFurShell > 0.0) {
        rough = mix(uFurRootRough, uFurTipRough, vFurShell);   // tips rougher (Fur Root/Tip Roughness)
        furAO = mix(0.45, 1.0, vFurShell);                     // roots self-shadowed
    }
    rough=clamp(rough,0.04,1.0);
    if (uIsEye==1 && uHasOrm==0) rough = clamp(uEyeRough, 0.02, 1.0);   // wet-cornea fallback (no roughness map)
    vec3 V = normalize(uViewPos - vWorldPos);
    // Two-sided: flip only genuine BACK-faces (winding), not front-faces whose detail-perturbed
    // normal happens to graze past the viewer — the old dot(N,V)<0 test tipped fine detail into
    // dark bands at glancing angles. gl_FrontFacing is view- and detail-independent.
    if (!gl_FrontFacing) N = -N;
    // Geometric specular anti-aliasing (Kaplanyan): where the shading normal changes fast across
    // a pixel (fine normal-map detail, curved grazing edges), widen roughness so the highlight
    // stops sparkling/shimmering when the model is zoomed or spinning. Toggle in Preview Settings.
    if (uSpecAA==1) {
        vec3 dNx = dFdx(N), dNy = dFdy(N);
        float normalVar = 0.5 * (dot(dNx, dNx) + dot(dNy, dNy));
        rough = clamp(sqrt(rough*rough + min(normalVar, 0.18)), 0.04, 1.0);
    }
    vec3 emis = (uHasEmissive==1) ? texture(uEmissiveTex,vUV).rgb * uEmisColor * uEmisMul * uEmisScale : vec3(0.0);

    // D4 dye: single-channel DyeMask.r = how dyeable the texel is; single-channel
    // DyeRamp.r = position along the dye's gradient (shading). The dye colour itself
    // comes from the equipped dye — approximated here by the picked region colour,
    // shaded by the ramp so creases stay dark and highlights stay bright.
    if (uFDye==1 && uHasDyeMask==1) {
        // D4 DyeMask is value-banded into 4 colour zones. Classify each sampled mask
        // texel to its zone colour, then anti-alias: bilinear blend up close
        // (magnification), box-average over the texel footprint when far (minification).
        float ramp = (uHasDyeRamp==1) ? texture(uDyeRampTex, vUV).r : 0.5;
        float sh = mix(0.35, 1.40, ramp);
        vec3 baseRgb = base.rgb;
        vec2 ts = vec2(textureSize(uDyeMaskTex, 0));
        vec2 fw = abs(dFdx(vUV)) + abs(dFdy(vUV));        // footprint in UV
        float texels = max(fw.x * ts.x, fw.y * ts.y);     // footprint in mask texels
        if (texels <= 1.5) {                              // magnification → bilinear
            vec2 uvt = vUV * ts - 0.5;
            vec2 fr = fract(uvt);
            vec2 b0 = (floor(uvt) + 0.5) / ts;
            vec2 tx = vec2(1.0 / ts.x, 0.0), ty = vec2(0.0, 1.0 / ts.y);
            vec3 c00 = dyeZoneColor(b0, sh, baseRgb), c10 = dyeZoneColor(b0 + tx, sh, baseRgb);
            vec3 c01 = dyeZoneColor(b0 + ty, sh, baseRgb), c11 = dyeZoneColor(b0 + tx + ty, sh, baseRgb);
            base.rgb = mix(mix(c00, c10, fr.x), mix(c01, c11, fr.x), fr.y);
        } else {                                          // minification → box-average
            vec2 hx = dFdx(vUV) * 0.5, hy = dFdy(vUV) * 0.5;
            vec3 acc = vec3(0.0);
            for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i)
                acc += dyeZoneColor(vUV + float(i) * hx + float(j) * hy, sh, baseRgb);
            base.rgb = acc / 9.0;
        }
    }

)" R"(
    // Material-channel viewer (the "Color" dropdown): output one raw channel, unlit.
    if (uViewChannel > 0) {
        vec3 dbg;
        if      (uViewChannel == 1) dbg = base.rgb;        // Base Colour
        else if (uViewChannel == 2) dbg = N*0.5 + 0.5;     // Normal (final shading normal, incl. detail)
        else if (uViewChannel == 3) dbg = vec3(rough);     // Roughness
        else if (uViewChannel == 4) dbg = vec3(metal);     // Metallic
        else if (uViewChannel == 5) dbg = vec3(ao);        // Ambient occlusion
        else if (uViewChannel == 6) dbg = emis;            // Emissive
        else if (uViewChannel == 7) {                      // Detail SELECT: which map applies per region
            int layer = uZoneMap[1];
            if (uHasDyeMask==1) {
                float mv = texture(uDyeMaskTex, vUV).r;
                int zone = 0; float best = 2.0;
                for (int k = 0; k < 4; ++k) { float e = abs(mv - uDyeBands[k]); if (e < best) { best = e; zone = k; } }
                layer = uZoneMap[zone]; if (mv <= 0.02) layer = -1;
            }
            // Mirror the real selection: metal texels route to the metal detail map (or fade off if
            // there is none); non-metal texels never use the metal map.
            int metalLayer = (uMetalRoute == -2) ? uDetailMetalLayer : uMetalRoute;
            bool faded = false;
            if (metal > uMetalThresh) {
                if (metalLayer >= 0) layer = metalLayer;
                else faded = true;
            } else if (metalLayer >= 0 && layer == metalLayer) {
                layer = -1;
            }
            if (faded || layer < 0)        dbg = vec3(0.05);                 // no detail (metal-no-map/unmasked)
            else if (layer == 0)           dbg = (uHasDetN0==1) ? vec3(1.0,0.3,0.3) : vec3(0.05);  // map0 (red)
            else if (layer == 1)           dbg = (uHasDetN1==1) ? vec3(0.3,1.0,0.3) : vec3(0.05);  // map1 (green)
            else                           dbg = (uHasDetN2==1) ? vec3(0.3,0.5,1.0) : vec3(0.05);  // map2 (blue)
        }
        else if (uViewChannel == 8) {                      // Dye/material-mask zones (detail region selector?)
            if (uHasDyeMask==1) {
                float mv = texture(uDyeMaskTex, vUV).r;
                int zone = 0; float best = 2.0;
                for (int k = 0; k < 4; ++k) { float e = abs(mv - uDyeBands[k]); if (e < best) { best = e; zone = k; } }
                vec3 zc[4] = vec3[4](vec3(1.0,0.2,0.2), vec3(0.2,1.0,0.2), vec3(0.3,0.5,1.0), vec3(1.0,0.9,0.2));
                dbg = (mv <= 0.02) ? vec3(0.05) : zc[zone];   // near-black = un-masked (e.g. metal)
            } else dbg = vec3(0.1);
        }
        else                        dbg = emis;
        FragColor = vec4(dbg, furCover);
        return;
    }

    if (uPbr==0) {   // PBR off → flat two-sided Lambert
        float d=max(dot(N, normalize(uLightDir)),0.0);
        FragColor=vec4(base.rgb*(0.2+0.8*d)+emis, 1.0); return;
    }

    vec3 albedo=base.rgb;
    // Fur root→tip albedo gradient (applied after dye so dyed fur keeps the depth cue).
    if (uFurEnabled==1 && vFurShell > 0.0)
        albedo *= mix(uFurRootColor, uFurTipColor, vFurShell);
    // sRGB-correct: when tonemapping, light in linear space (albedo/emissive are
    // sRGB-encoded textures) and re-encode at the end.
    if (uFTonemap==1) { albedo = pow(albedo, vec3(2.2)); emis = pow(emis, vec3(2.2)); }
    // Wetness (rain-slick): a wet surface soaks in — its diffuse darkens — and its microsurface fills
    // with water, so it turns glossier (lower roughness) and reflects more. D4 authors a per-material
    // "Wetness Bias"; this global slider stands in for it. Eyes are already wet, so skip them.
    if (uWetness > 0.001 && uIsEye==0) {
        albedo *= mix(1.0, 0.62, uWetness);                    // wet darkens the diffuse
        rough   = mix(rough, rough * 0.25 + 0.02, uWetness);   // and sharpens the reflection
    }
    // Snow: a cool-white dusting settles on UP-facing surfaces (world normal points up) — shoulders,
    // head, ledges — turning them matte and non-metallic. None on undersides. (D4 'Use Snowiness'.)
    if (uSnow > 0.001 && uIsEye==0) {
        float snow = uSnow * smoothstep(0.25, 0.75, N.y);
        albedo = mix(albedo, vec3(0.90, 0.92, 0.96), snow);
        rough  = mix(rough, 0.85, snow);
        metal  = mix(metal, 0.0, snow);
    }
    vec3 F0=mix(vec3(0.04), albedo, clamp(metal,0.0,1.0));
    vec3 L=normalize(uLightDir), H=normalize(V+L);
    float NoL=max(dot(N,L),0.0), NoV=max(dot(N,V),1e-4);
    float NoH=max(dot(N,H),0.0), VoH=max(dot(V,H),0.0);

    vec3 spec;
    if (uFHair==1 && uIsHair==1) {           // Scheuermann shifted dual-highlight (real hair sheen)
        // The strand tangent is nudged along the surface normal by +/- an offset (a small base offset
        // plus the material's authored Hair Highlight Shift), producing a tight primary sheen and a
        // broader secondary sheen that sit APART and slide as the view rotates — the moving highlight
        // real hair has (vs one static KK band).
        // A little per-strand break-up from the normal map keeps it from reading as a clean stripe.
        // Strong per-strand break-up: on flat front-facing locks a coherent tangent makes the
        // whole lock light up as one bleached stripe; scattering the shift per-strand (from the
        // normal map) turns that into fine glints that ride individual strands like real hair.
        float jitter = (uHasNormal==1) ? (texture(uNormalTex, vUV).x - 0.5) * 0.28 : 0.0;
        // Fully data-driven from the hero_hair material (uHairParams): x = Hair Roughness (governs the
        // sheen width), y = Hair Specular (reflectance/intensity), z = Highlight Shift (how far apart the
        // two Scheuermann lobes sit). No hardcoded look constants — each hairstyle reads at its own values.
        float hairRough = clamp(uHairParams.x, 0.04, 1.0);
        float hairSpec  = max(uHairParams.y, 0.0);
        float shift     = uHairParams.z;
        float exP  = mix(50.0, 170.0, 1.0 - hairRough);     // rougher hair ⇒ smaller exponent ⇒ broader sheen
        float atten;
        // primary: tight glint, nudged by +Highlight Shift (plus a small base offset that keeps the
        // dual-lobe character even when the authored shift is 0)
        vec3  Tp   = normalize(T + (shift + 0.03 + jitter) * N);
        float dTHp = dot(Tp, H);
        float sinp = sqrt(max(0.0, 1.0 - dTHp*dTHp));
        atten      = smoothstep(-1.0, 0.0, dTHp);           // no wrap to the back side
        float p    = atten * pow(sinp, exP);
        // secondary: broader albedo-tinted lobe, opposite shift
        vec3  Ts   = normalize(T + (-shift - 0.04 + jitter) * N);
        float dTHs = dot(Ts, H);
        float sins = sqrt(max(0.0, 1.0 - dTHs*dTHs));
        float s    = atten * pow(sins, exP * 0.34);
        // Hair Specular scales both lobes (0.15/0.30 × spec reproduces the old 0.045/0.09 at spec=0.3).
        spec = vec3(0.15 * hairSpec) * p + (0.30 * hairSpec) * albedo * s;
    } else {                                 // Cook-Torrance microfacet
        float D=distGGX(NoH, rough*rough);
        float G=gSmith(NoV,NoL,rough);
        vec3  F=fres(VoH,F0);
        spec=(D*G*F)/max(4.0*NoV*NoL,1e-4);
        // Multiscatter energy compensation: single-scatter GGX loses energy on rough/metal
        // surfaces (they look too dark); add back the missing inter-reflections (Fdez-Aguera).
        vec2 dfg=envDFG(NoV, rough);
        spec *= 1.0 + F0 * (1.0/max(dfg.x + dfg.y, 1e-3) - 1.0);
    }
    // Brushed-metal anisotropy: D4 authors a 'Fur Aniso Strength' on some armour — a fine directional
    // grain. Add a TIGHT, dim tangent-aligned glint, gated to smooth + metallic texels, so polished
    // plate picks up a faint brushed streak without the isotropic highlight being washed out.
    if (uFHair==0 && uIsEye==0) {
        float aniso = smoothstep(0.35, 0.70, metal) * (1.0 - rough);
        if (aniso > 0.001) {
            float ToL = dot(T, L), ToV = dot(T, V);
            float kk = max(0.0, sqrt(max(0.0,1.0-ToL*ToL))*sqrt(max(0.0,1.0-ToV*ToV)) - ToL*ToV);
            spec += F0 * pow(kk, 28.0) * aniso * 0.55;   // tinted like the metal, subtle
        }
    }
    if (uFMask==1 && uHasMask==1) spec *= texture(uMaskTex,vUV).r;
    // Eyes: the wet cornea is a strong specular reflector — boost the catchlight so the eye reads
    // live and moist (a bright glint of the key light) instead of a flat, dead disc.
    if (uIsEye==1) spec *= 1.9;
    // Fur is a soft dielectric — keep its microfacet highlight low so it doesn't read as a
    // hard glossy band (the jittered normals already scatter what remains into sparkle).
    if (uFurEnabled==1 && vFurShell > 0.0) spec *= 0.20;

    vec3 kd=(vec3(1.0)-fres(VoH,F0))*(1.0-clamp(metal,0.0,1.0));
    vec3 lightColor=uLightCol;
    // Diffuse term. Skin uses pre-integrated subsurface scattering: surface curvature softens
    // and reddens the N·L terminator (light scatters under the skin; red bleeds furthest), so
    // faces read as fleshy rather than waxy. Everything else uses plain Lambert N·L.
    vec3 diffNoL = vec3(NoL);
    if (uFSubsurf==1 && uIsSkin==1) {
        float curv = clamp(length(fwidth(N)) / max(length(fwidth(vWorldPos)), 1e-5) * uSkinCurv, 0.0, 6.0);
        vec3 w = vec3(0.45, 0.22, 0.12) * (0.6 + curv);          // R scatters widest, B sharpest
        vec3 wrapNoL = clamp((vec3(NoL) + w) / (1.0 + w), 0.0, 1.0);
        vec3 tint = mix(vec3(1.0), vec3(1.0, 0.55, 0.42), clamp(curv*0.6*uSkinWarm, 0.0, 1.0));
        // Metals don't subsurface-scatter: fade SSS out by metalness so a metallic marking (gold
        // foil) on skin renders as clean metal instead of being softened/warmed like flesh.
        diffNoL = mix(vec3(NoL), wrapNoL * tint, uSssStrength * (1.0 - clamp(metal, 0.0, 1.0)));
    }
    vec3 Lo=(kd*albedo/PI)*lightColor*diffNoL + spec*lightColor*NoL;

    // Subsurface / translucency: light bleeding through thin parts (skin / cloth). D4's real skin
    // "Translucency Color Hero" is blood-RED (0.117,0.0008,0) — the light under skin reads red, not
    // peachy — at Translucency Intensity 0.4.
    if (uFSubsurf==1 && (uIsSkin==1 || uHasTrans==1)) {
        vec3 tc;
        if (uIsSkin==1) {
            // Skin: the _Trans map is a THIN-SKIN MASK (ears/nostrils); the transmission COLOUR is the
            // fixed red Translucency Color Hero. Map modulates where the red scatter shows.
            float tmask = (uHasTrans==1) ? texture(uTransTex,vUV).r : 1.0;
            tc = vec3(0.90, 0.17, 0.09) * tmask;
        } else {
            tc = texture(uTransTex,vUV).rgb;   // non-skin (foliage/etc.) uses the map colour directly
        }
        float back=pow(clamp(dot(V,-L),0.0,1.0),2.0)*0.5 + max(0.0,dot(-N,L))*0.3;
        // Head Translucency Intensity 0.4, BODY 1.0 (real D4 values) — the body flesh is more
        // translucent than the face.
        float ti = (uIsSkin==1) ? (uIsHead==1 ? 0.55 : 0.9) : 1.0;
        Lo += albedo*tc*back*lightColor*ti;
    }
    // D4 skin Character Fresnel (Strength 3.6, Slope 1.44, toggle on): a soft warm rim at grazing
    // angles that gives faces their fleshy edge-glow. Tied to the key light so it reads as rim light,
    // not a flat halo, and faded on the shadow side.
    if (uFSubsurf==1 && uIsSkin==1 && uIsHead==1) {   // real D4 data: FACE Fresnel +3.6, BODY −2.7 (none)
        float rim = pow(1.0 - NoV, 2.6);
        Lo += albedo * rim * 0.16 * lightColor * (0.35 + 0.65*NoL);
    }
    // Eyes: a glossy cornea Fresnel — a wet sheen at the eyeball's grazing edge so it reads moist
    // and rounded rather than a matte painted disc.
    if (uIsEye==1) {
        float wet = pow(1.0 - NoV, 3.5);
        Lo += wet * 0.12 * lightColor;
    }
    // Hair translucency: thin strands transmit their own colour when back-lit — the warm rim glow
    // that lights up a ponytail/lock from behind. D4 hair carries no transmission map, so it glows
    // with the (gradient-mapped) hair albedo, strongest when looking toward the light through it.
    if (uFSubsurf==1 && uFHair==1 && uIsHair==1) {
        float back = pow(clamp(dot(V, -L), 0.0, 1.0), 3.0);   // view aligned with the light dir
        Lo += albedo * back * 0.30 * lightColor;              // real D4 Hair Scattering = 0.30
    }
    // Cloth: soft grazing-angle sheen (fuzz on fabric edges).
    if (uFSubsurf==1 && uIsCloth==1) {
        float sheen = pow(1.0 - NoV, 4.0);
        Lo += albedo * sheen * 0.6 * NoL * lightColor;
    }
    // Fur: a broad, gentle directional sheen along the tangent (Kajiya-Kay). Kept soft and
    // low — a tight/bright lobe here reads as a hard white line across the collar.
    if (uFurEnabled==1 && vFurShell > 0.0 && uFurAniso > 0.0) {
        vec3 Tn = normalize(vTangent);
        float ToL = dot(Tn, L), ToV = dot(Tn, V);
        float kk = max(0.0, sqrt(max(0.0,1.0-ToL*ToL))*sqrt(max(0.0,1.0-ToV*ToV)) - ToL*ToV);
        Lo += uFurAniso * pow(kk, 8.0) * vFurShell * 0.18 * albedo * lightColor * NoL;
    }

    // Self-shadow the whole key-light contribution (diffuse + spec + SSS + cloth/fur sheen); the
    // rim/fill are separate lights and stay unshadowed.
    Lo *= keyShadow(vWorldPos, N);

    // Secondary rig lights — cool back rim + cool front fill (real D4 character-screen colours).
    // Fur damps their hard spec so they read as soft edge light, not a glossy band.
    float specMul = (uFurEnabled==1 && vFurShell > 0.0) ? 0.20 : 1.0;
    Lo += shadeLight(N, V, uRimDir,  uRimCol,  albedo, F0, rough, metal, specMul);
    Lo += shadeLight(N, V, uFillDir, uFillCol, albedo, F0, rough, metal, specMul);

    // Ambient: analytic hemisphere IBL — split-sum specular (env-BRDF DFG) + diffuse irradiance,
    // with energy-correct Fresnel and specular occlusion. Diffuse is AO-darkened; specular uses
    // its own grazing-aware occlusion so reflections aren't flattened by cavity AO.
    vec3 ambDiff, ambSpec;
    if (uFIbl==1) {
        vec3 irr=envColor(N);
        // Ambient specular reflection: the real D4 reflection-probe cubemap (prefiltered, so
        // roughness picks a blur mip) when one is loaded; otherwise the analytic hemisphere.
        vec3 R = reflect(-V, N);
        vec3 pref = (uHasReflCube==1) ? textureLod(uReflCube, R, rough*uReflMaxMip).rgb
                                      : mix(envColor(R), irr, rough);
        vec2 dfg=envDFG(NoV, rough);
        vec3 kdA=(vec3(1.0)-fresRough(NoV,F0,rough))*(1.0-clamp(metal,0.0,1.0));
        ambDiff = irr*albedo*kdA;
        ambSpec = pref*(F0*dfg.x + dfg.y) * specOcclusion(NoV, ao, rough);
    } else {
        ambDiff = albedo*(1.0-clamp(metal,0.0,1.0))*0.22;
        ambSpec = F0*0.18;
    }
    // Hair is a fibrous dielectric — in-game it reads MATTE, not a glossy shell. Kill most of the
    // environment reflection so the crown stops looking wet/waxy; the directional Scheuermann sheen
    // (key light) still supplies the hair highlight.
    if (uFHair==1 && uIsHair==1) ambSpec *= 0.10;
    ambSpec *= uReflStrength;   // reflection-intensity slider
    float aoSS = ssaoFactor(vWorldPos, normalize(N));   // screen-space ambient occlusion
    // Emissive: a subtle slow pulse so glowing runes/gems read alive (preview only — export writes
    // the steady authored value). Only pulses where there's actual glow.
    vec3 emisP = emis * (0.90 + 0.10 * sin(uTime * 2.2));
    vec3 col = ((ambDiff*ao*aoSS + ambSpec*aoSS)*uAmbScale + Lo*mix(1.0,ao,0.35) + emisP) * furAO;

    col *= uExposure;                        // exposure always applies (key control of the rig)
    if (uFTonemap==1) {                      // ACES filmic → sRGB
        col = clamp((col*(2.51*col+0.03))/(col*(2.43*col+0.59)+0.14), 0.0, 1.0);
        // Optional D4-style colour grade (post-tonemap, pre-gamma): contrast S-curve, saturation,
        // and a subtle split-tone (warm shadows / cool highlights). Off unless enabled.
        if (uColorGrade==1) {
            if (uHasLut==1) {
                // Real D4 LUT: 16×16×16 unwrapped to a 256×16 strip (16 blue-tiles across). Bilinear
                // in R/G within a tile, linear across the two bracketing blue tiles.
                vec3 c = clamp(col, 0.0, 1.0);
                float b = c.b * 15.0; float b0 = floor(b); float b1 = min(b0 + 1.0, 15.0); float f = b - b0;
                vec2 uvA = vec2((b0*16.0 + c.r*15.0 + 0.5)/256.0, (c.g*15.0 + 0.5)/16.0);
                vec2 uvB = vec2((b1*16.0 + c.r*15.0 + 0.5)/256.0, (c.g*15.0 + 0.5)/16.0);
                col = mix(texture(uLut, uvA).rgb, texture(uLut, uvB).rgb, f);
            } else {                                                        // stylised fallback grade
                col = clamp(mix(vec3(0.5), col, uCgContrast), 0.0, 1.0);     // contrast about mid-grey
                float l = dot(col, vec3(0.2126, 0.7152, 0.0722));           // luma
                col = clamp(mix(vec3(l), col, uCgSat), 0.0, 1.0);          // saturation
                col += vec3( 0.9, 0.35, -0.6) * uCgWarmth * (1.0 - l);      // warm the shadows
                col += vec3(-0.3, 0.0,   0.9) * uCgWarmth * l;              // cool the highlights
                col = clamp(col, 0.0, 1.0);
            }
        }
        col = pow(col, vec3(1.0/2.2));
    }
    FragColor=vec4(col, furCover * cutCov);   // furCover<1 at soft strand tips; cutCov<1 at cutout edges
}
)";

// Shadow depth pass: render the model from the key light's POV, writing depth only.
const char* kShadowVert = R"(#version 450 core
layout(location=0) in vec3 aPos;
uniform mat4 uLightMVP;
void main() { gl_Position = uLightMVP * vec4(aPos, 1.0); }
)";
const char* kShadowFrag = R"(#version 450 core
void main() {}
)";

// SSAO position prepass: render the model's world-space position into an RGBA16F G-buffer
// (xyz = world pos, a = 1 marks geometry vs cleared background) for screen-space AO.
const char* kPosVert = R"(#version 450 core
layout(location=0) in vec3 aPos;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vWorld;
void main() {
    vWorld = vec3(uModel * vec4(aPos, 1.0));
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";
const char* kPosFrag = R"(#version 450 core
in vec3 vWorld;
out vec4 oPos;
void main() { oPos = vec4(vWorld, 1.0); }
)";

// ── Column-major 4x4 helpers for CPU skinning ──
using Mat4 = std::array<float, 16>;

Mat4 mat4mul(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[col * 4 + k];
            r[col * 4 + row] = s;
        }
    return r;
}

// Compose a column-major matrix from a D4-native TRS, applying z_up_to_y_up —
// the same transform ModelParser uses for the rest/inverse-bind matrices, so the
// animated locals live in the viewport's Y-up space.
Mat4 composeTRS(const float q[4], const float t[3], const float s[3])
{
    float tx = t[0], ty = t[2], tz = -t[1];                 // (x, z, -y)
    float qx = q[0], qy = q[2], qz = -q[1], qw = q[3];      // quat axis swap
    const float qm = std::sqrt(qx*qx + qy*qy + qz*qz + qw*qw);
    if (qm < 1e-9f) { qx = qy = qz = 0; qw = 1; } else { qx/=qm; qy/=qm; qz/=qm; qw/=qm; }
    const float sx = s[0], sy = s[2], sz = s[1];
    const float xx=qx*qx, yy=qy*qy, zz=qz*qz, xy=qx*qy, xz=qx*qz, yz=qy*qz;
    const float wx=qw*qx, wy=qw*qy, wz=qw*qz;
    const float m[4][4] = {
        {(1-2*(yy+zz))*sx, (2*(xy-wz))*sy,   (2*(xz+wy))*sz,   tx},
        {(2*(xy+wz))*sx,   (1-2*(xx+zz))*sy, (2*(yz-wx))*sz,   ty},
        {(2*(xz-wy))*sx,   (2*(yz+wx))*sy,   (1-2*(xx+yy))*sz, tz},
        {0, 0, 0, 1}};
    Mat4 out; int k = 0;
    for (int col = 0; col < 4; ++col) for (int row = 0; row < 4; ++row) out[k++] = m[row][col];
    return out;
}

}  // namespace

GLModelWidget::GLModelWidget(QWidget* parent) : QOpenGLWidget(parent)
{
    applyRig();
    // Viewport guides — global settings so every viewport (Models/Wardrobe/Stable) matches;
    // the Graphics-panel checkboxes call the setters live and persist the keys.
    QSettings s;
    m_showAxisGizmo   = s.value(QStringLiteral("viewer/axisGizmo"), true).toBool();
    m_gridAxisColors  = s.value(QStringLiteral("viewer/gridAxisColors"), true).toBool();
    m_gizmo = new AxisGizmoOverlay(this);
    m_gizmo->setVisible(m_showAxisGizmo);
}

GLModelWidget::~GLModelWidget()
{
    if (context()) {
        makeCurrent();
        destroyBuffers();
        if (m_prog) glDeleteProgram(m_prog);
        if (m_bgProg) glDeleteProgram(m_bgProg);
        if (m_bgVao) glDeleteVertexArrays(1, &m_bgVao);
        // Auxiliary GL objects created lazily elsewhere and previously never released.
        if (m_shadowProg) glDeleteProgram(m_shadowProg);
        if (m_posProg)    glDeleteProgram(m_posProg);
        if (m_shadowFbo)  glDeleteFramebuffers(1, &m_shadowFbo);
        if (m_posFbo)     glDeleteFramebuffers(1, &m_posFbo);
        if (m_posDepth)   glDeleteRenderbuffers(1, &m_posDepth);
        if (m_shadowTex)  glDeleteTextures(1, &m_shadowTex);
        if (m_posTex)     glDeleteTextures(1, &m_posTex);
        if (m_reflCube)   glDeleteTextures(1, &m_reflCube);
        if (m_lutTex)     glDeleteTextures(1, &m_lutTex);
        const GLuint vaos[] = { m_skelVao, m_hpVao, m_physVao, m_colVao, m_gridVao };
        const GLuint vbos[] = { m_skelVbo, m_hpVbo, m_physVbo, m_colVbo, m_gridVbo };
        for (GLuint v : vaos) if (v) glDeleteVertexArrays(1, &v);
        for (GLuint b : vbos) if (b) glDeleteBuffers(1, &b);
        doneCurrent();
    }
}

// Memoized glGetUniformLocation: first query per (program, name) hits the driver; every later
// frame reads the cached location. Returns -1 for a null program (matches GL's "not found").
GLint GLModelWidget::uni(GLuint prog, const char* name)
{
    if (!prog) return -1;
    QHash<QByteArray, GLint>& m = m_uniLoc[prog];
    // fromRawData avoids allocating a QByteArray on the (common) cache-hit path.
    const auto it = m.constFind(QByteArray::fromRawData(name, int(qstrlen(name))));
    if (it != m.constEnd()) return it.value();
    const GLint loc = glGetUniformLocation(prog, name);
    m.insert(QByteArray(name), loc);   // owning key copy stored on first miss only
    return loc;
}


// Fill per-piece authored tuning (.clt.json) for any ClothSim not already tuned. Runs for
// EVERY tab via setGeometry — previously only the Wardrobe assembler did this, so Models/
// Stable pieces simulated with default tuning (wrong gravity: zero-gravity feathers fell,
// etc.). Resolution is the SHARED ModelParser::resolveClothTuning: the game's own
// per-item snoCloth link first (authoritative — cross-gender refs like rogF piece →
// rogM cloth only resolve this way), then the embedded-name suffix/prefix fallback.
static void fillClothTuningFromD4(QVector<ClothSim>& sims)
{
    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty()) return;
    for (ClothSim& s : sims) {
        if (s.tuned || (s.name.isEmpty() && s.srcApp.isEmpty())) continue;
        const QJsonObject t = ModelParser::resolveClothTuning(d4, s);
        if (t.isEmpty()) continue;
        auto g = [&](const char* k, double def) { return t.value(QLatin1String(k)).toDouble(def); };
        s.boneTrack = qBound(0.0f, float(g("flBoneTrackingFactor", 0.5)), 1.0f);
        const QJsonObject gv = t.value(QStringLiteral("vGravity")).toObject();
        const double gmag = -gv.value(QStringLiteral("z")).toDouble(-20.0);
        // SIGNED: authored positive vGravity.z (upward force, e.g. sorF_stor211 feathers
        // z=+10) yields a negative scale; the solver's gAuth flips sign accordingly.
        s.gravScale = qBound(-3.0f, float(gmag / 20.0), 3.0f);
        s.attachStiff = qBound(0.0f, float(g("flAttachmentStiffness", 0.3)), 1.0f);   // NO floor: authored zero is real
        // Authored per-class constraint stiffness (see ClothSim::clsStiff).
        s.clsStiff[0] = qBound(0.05f, float(g("flStretchingStiffness", 0.8)), 1.0f);
        s.clsStiff[1] = qBound(0.05f, float(g("flHorizontalStiffness", 0.8)), 1.0f);
        s.clsStiff[2] = qBound(0.05f, float(g("flShearStiffness",      0.5)), 1.0f);
        s.clsStiff[3] = qBound(0.05f, float(g("flBendingStiffness",    0.5)), 1.0f);
        s.dragF = qBound(0.0f, float(g("flDragFactor", 0.0)), 1.0f);
        const QJsonObject sw = t.value(QStringLiteral("vSelfWind")).toObject();
        const double wf = g("flWindFactor", 1.0), sc = 0.0006;
        const double sx = sw.value(QStringLiteral("x")).toDouble(),
                     sy = sw.value(QStringLiteral("y")).toDouble(),
                     sz = sw.value(QStringLiteral("z")).toDouble();
        s.windX = float(sx*wf*sc); s.windY = float(sz*wf*sc); s.windZ = float(-sy*wf*sc);
        s.tuned = true;
    }
}

void GLModelWidget::setGeometry(const ModelGeometry& geo, bool keepView)
{
    m_verts.clear();
    m_indices.clear();
    m_parts.clear();
    m_followParts.clear();   // stale part indices from the previous model
    m_vJoints.clear();
    m_vWeights.clear();
    m_bindVerts.clear();   // IMPORTANT: clear before clearAnimation(), else it restores
                           // the *previous* model's bind pose into m_verts and the new
                           // vertices get appended after it (indices then point at stale data).
    m_skeleton = geo.skeleton;
    m_baseBones = geo.nBaseBones;
    m_authoredCaps = geo.clothCapsules;   // game's real bone-bound collision capsules
    m_clothSims    = geo.clothSims;        // game's real low-poly sim cages (1:1 path)
    fillClothTuningFromD4(m_clothSims);    // per-piece authored tuning for ALL tabs (no-op when already tuned)
    m_pinnedBones  = geo.pinnedBones;      // skeletally-attached prop base bones → held rigid
    m_clothBuilt = false;   // cloth-sim topology must be rederived for the new geometry
    m_sbBuilt = false;      // spring-bone chains must be rederived for the new skeleton
    ensureClothTimer();     // keep the cloth settling while paused (for live slider tweaks)
    clearAnimation();   // resets m_hasAnim/m_frame
    QVector3D mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);

    // Pre-reserve the flatten buffers from the known totals — otherwise a big mesh
    // reallocates these vectors many times mid-loop (this runs synchronously on the UI thread).
    {
        qsizetype totalVerts = 0, totalIdx = 0;
        for (const MeshPrimitive& p : geo.primitives) { totalVerts += p.vertices.size(); totalIdx += p.indices.size(); }
        m_verts.reserve(totalVerts * 11);   // 11 interleaved floats per vertex
        m_indices.reserve(totalIdx);
        m_vJoints.reserve(size_t(totalVerts));
        m_vWeights.reserve(size_t(totalVerts));
    }

    quint32 base = 0;
    int pi = 0;
    for (const MeshPrimitive& p : geo.primitives) {
        // Per-vertex tangents (Lengyel) from positions + UVs, for normal mapping.
        QVector<QVector3D> tan(p.vertices.size(), QVector3D(0, 0, 0));
        for (int t = 0; t + 2 < p.indices.size(); t += 3) {
            const int i0 = int(p.indices[t]), i1 = int(p.indices[t + 1]), i2 = int(p.indices[t + 2]);
            if (i0 < 0 || i1 < 0 || i2 < 0
                || i0 >= p.vertices.size() || i1 >= p.vertices.size() || i2 >= p.vertices.size()) continue;
            const MeshVertex& a = p.vertices[i0]; const MeshVertex& b = p.vertices[i1]; const MeshVertex& c = p.vertices[i2];
            const QVector3D e1(b.px - a.px, b.py - a.py, b.pz - a.pz);
            const QVector3D e2(c.px - a.px, c.py - a.py, c.pz - a.pz);
            const float du1 = b.u - a.u, dv1 = b.v - a.v, du2 = c.u - a.u, dv2 = c.v - a.v;
            const float det = du1 * dv2 - du2 * dv1;
            const float r = (std::fabs(det) > 1e-8f) ? 1.0f / det : 0.0f;
            const QVector3D tg = (e1 * dv2 - e2 * dv1) * r;
            tan[i0] += tg; tan[i1] += tg; tan[i2] += tg;
        }
        for (int vi = 0; vi < p.vertices.size(); ++vi) {
            const MeshVertex& v = p.vertices[vi];
            const QVector3D n(v.nx, v.ny, v.nz);
            QVector3D tg = tan[vi] - n * QVector3D::dotProduct(n, tan[vi]);   // orthogonalize
            if (tg.lengthSquared() < 1e-12f) {
                tg = QVector3D::crossProduct(n, QVector3D(0, 1, 0));
                if (tg.lengthSquared() < 1e-12f) tg = QVector3D::crossProduct(n, QVector3D(1, 0, 0));
            }
            tg.normalize();
            m_verts << v.px << v.py << v.pz << v.nx << v.ny << v.nz << v.u << v.v
                    << tg.x() << tg.y() << tg.z();
            // Clamp every joint index into the unified skeleton's range. Weapon verts
            // (rigid-skinned to a body bone, then merged with the weapon's own skeleton
            // cleared) and any piece whose joints weren't fully remapped can otherwise
            // carry an index >= skeleton.size(); that garbage index flows into the cloth
            // classifier/collision build and corrupts memory. Neutralizing it here — the
            // one choke point all geometry passes through — makes every downstream
            // skinning/cloth path safe without changing valid behaviour.
            const int nbJ = m_skeleton.size();
            std::array<quint16, 4> jcl{{v.joints[0], v.joints[1], v.joints[2], v.joints[3]}};
            for (int jk = 0; jk < 4; ++jk)
                if (nbJ <= 0 || jcl[jk] >= quint16(nbJ)) jcl[jk] = 0;
            m_vJoints.push_back(jcl);
            m_vWeights.push_back({{v.weights[0], v.weights[1], v.weights[2], v.weights[3]}});
            mn.setX(qMin(mn.x(), v.px)); mn.setY(qMin(mn.y(), v.py)); mn.setZ(qMin(mn.z(), v.pz));
            mx.setX(qMax(mx.x(), v.px)); mx.setY(qMax(mx.y(), v.py)); mx.setZ(qMax(mx.z(), v.pz));
        }
        Part part;
        part.offset = m_indices.size();
        for (quint32 idx : p.indices)
            m_indices << (idx + base);
        part.count = m_indices.size() - part.offset;
        part.name = p.materialName.isEmpty() ? QStringLiteral("part %1").arg(pi) : p.materialName;
        m_parts.push_back(part);
        base += quint32(p.vertices.size());
        ++pi;
    }

    if (m_verts.isEmpty()) {
        clearGeometry();
        return;
    }
    m_bindVerts = m_verts;   // immutable bind pose; animation re-skins from this

    // ── Detail-mask probe (diagnostic, harmless) ──────────────────────────────
    // D4's uber shader blends tiled Detail Map 1/2/3 by the per-vertex COLOR_0 RGB
    // weights. Before building that blend, confirm the mesh actually carries the mask:
    // write per-part colour stats next to the exe (readable through the sandbox mount
    // while the app runs). meanRGB near (1,1,1) with zones≈1 ⇒ no real mask; distinct
    // means / zones≈2-3 ⇒ COLOR_0 is the leather/fabric/metal detail selector.
    {
        QString rep = QStringLiteral("detail-mask probe v2 — %1 part(s)\n"
                                     "Finding where the per-region detail-blend mask lives. Columns:\n"
                                     "  C0=COLOR_0 mean RGBA, zones=C0 RGB octants >=5%%, C1=COLOR_1 mean RGBA, uv1=second UV present\n\n")
                          .arg(geo.primitives.size());
        for (const MeshPrimitive& p : geo.primitives) {
            if (p.vertices.isEmpty()) continue;
            double sr=0,sg=0,sb=0,sa=0, s2r=0,s2g=0,s2b=0,s2a=0; int hc=0,hc1=0,huv1=0; int z[8]={0};
            for (const MeshVertex& v : p.vertices) {
                sr+=v.cr; sg+=v.cg; sb+=v.cb; sa+=v.ca; if (v.hasColor) ++hc;
                s2r+=v.c2r; s2g+=v.c2g; s2b+=v.c2b; s2a+=v.c2a; if (v.hasColor1) ++hc1;
                if (v.hasUv1) ++huv1;
                const int b = (v.cr>=0.5?1:0)|(v.cg>=0.5?2:0)|(v.cb>=0.5?4:0);
                ++z[b];
            }
            const int n = p.vertices.size();
            int zones=0; for (int k=0;k<8;++k) if (z[k]*20 >= n) ++zones;
            rep += QStringLiteral("  %1  n=%2\n"
                                  "      C0=(%3,%4,%5,%6) zones=%7 | C1=(%8,%9,%10,%11) has1=%12%% | uv1=%13%%\n")
                       .arg(p.materialName.isEmpty() ? QStringLiteral("(unnamed)") : p.materialName).arg(n)
                       .arg(sr/n,0,'f',2).arg(sg/n,0,'f',2).arg(sb/n,0,'f',2).arg(sa/n,0,'f',2).arg(zones)
                       .arg(s2r/n,0,'f',2).arg(s2g/n,0,'f',2).arg(s2b/n,0,'f',2).arg(s2a/n,0,'f',2)
                       .arg(hc1*100/qMax(1,n)).arg(huv1*100/qMax(1,n));
        }
        QFile f(QDir(QCoreApplication::applicationDirPath())
                    .filePath(QStringLiteral("detail_mask_probe.txt")));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { f.write(rep.toUtf8()); f.close(); }
    }

    m_gridVerts = 0;   // rebuild grid for the new bounds on next paint
    m_skelVerts = 0;   // rebuild skeleton for the new model
    m_physVerts = 0;   // rebuild physics-bone overlay for the new model
    // Always refresh the model's home centre + radius (grid sizing, fallbacks) for the new geometry.
    m_homeCenter = (mn + mx) * 0.5f;
    m_radius = qMax(0.001f, (mx - mn).length() * 0.5f);
    if (!keepView) {   // auto-frame the camera, unless preserving the current view (orbit angle)
        m_center = m_homeCenter;
        m_dist = m_radius * 2.6f;
        m_yaw = 0.6f;
        m_pitch = 0.25f;
    }
    m_hasPending = true;
    m_error.clear();
    update();
}

void GLModelWidget::clearGeometry()
{
    m_hasPending = false;
    m_verts.clear();
    m_indices.clear();
    m_parts.clear();
    m_partDyeOn.clear();
    m_partDyeColor.clear();
    m_bindVerts.clear();
    m_vJoints.clear();
    m_vWeights.clear();
    m_skeleton.clear();
    m_hasAnim = false;
    m_anim = {};
    m_animByHash.clear();
    if (context()) {
        makeCurrent();
        destroyBuffers();
        doneCurrent();
    }
    m_indexCount = 0;
    update();
}

void GLModelWidget::setAttachAnimRanges(const QVector<AttachRange>& ranges)
{
    m_attachRanges.clear();
    m_attachFrame.clear();
    // Flattened to a per-track lookup once, here, instead of scanning the ranges for every bone of
    // every frame in the skinning loop.
    m_trackClock.assign(m_anim.bones.size(), -1);
    for (const AttachRange& r : ranges) {
        if (r.count <= 0 || r.frames <= 0 || r.from < 0) continue;
        const int clock = m_attachRanges.size();
        m_attachRanges.push_back(r);
        m_attachFrame.push_back(0);
        for (int i = r.from; i < r.from + r.count && i < m_trackClock.size(); ++i)
            m_trackClock[i] = clock;
    }
}

void GLModelWidget::setPlaybackTimer(QTimer* t) { m_playbackTimer = t; }

bool GLModelWidget::animPlaying() const
{
    // Non-const target: qobject_cast is specified over pointers to non-const QObject subclasses.
    const QTimer* t = qobject_cast<QTimer*>(m_playbackTimer.data());
    return t && t->isActive();
}

void GLModelWidget::setAnimation(const AnimParser::DecodedAnim& anim)
{
    // A new clip invalidates every attached range; the caller re-declares them after installing.
    m_attachRanges.clear(); m_attachFrame.clear(); m_trackClock.clear();
    m_anim = anim;
    m_animByHash.clear();
    for (int i = 0; i < anim.bones.size(); ++i)
        m_animByHash.insert(anim.bones[i].boneHash, i);
    m_hasAnim = anim.valid && !m_skeleton.isEmpty() && anim.frameCount > 0;
    m_clothSeeded = false;   // re-seed the cloth sim from the new clip's first pose
    m_sbSeeded = false;      // re-seed the spring bones from the new clip's first pose
    m_sbAnimMovesBuilt = false;   // which cloth bones this clip really animates → recompute
    setFrame(0);
}

void GLModelWidget::clearAnimation()
{
    // Same invariant setAnimation keeps: these are sized against m_anim.bones, so they must not
    // outlive the clip they describe.
    m_attachRanges.clear(); m_attachFrame.clear(); m_trackClock.clear();
    m_hasAnim = false;
    m_frame = 0;
    m_anim = {};
    m_animByHash.clear();
    m_clothSeeded = false;
    m_sbAnimMovesBuilt = false;
    m_followParts.clear();   // nothing animating to follow
    if (!m_bindVerts.isEmpty()) { m_verts = m_bindVerts; m_hasPending = true; update(); }
}

void GLModelWidget::setAutoSpin(bool on)
{
    if (on) {
        // A camera glide (frameRegion animate) eases m_yaw toward a fixed target every tick.
        // If it's still running when the turntable starts, the two fight over m_yaw and the
        // spin stutters / never progresses. Finalise and stop any in-flight glide first so the
        // turntable owns m_yaw cleanly. (This is what the manual off/on toggle was doing.)
        if (m_camAnim && m_camAnim->isActive()) {
            m_center = m_tgtCenter; m_dist = m_tgtDist; m_pitch = m_tgtPitch;
            m_camAnim->stop();
        }
        if (!m_spinTimer) {
            m_spinTimer = new QTimer(this);
            connect(m_spinTimer, &QTimer::timeout, this, [this]() {
                m_yaw += m_spinSpeed;
                update();
            });
        }
        m_spinTimer->start(33);   // ~30 fps turntable
    } else if (m_spinTimer) {
        m_spinTimer->stop();
    }
}

// The viewport's m_verts hold the LIVE pose (setFrame → applySkinning skins on the CPU), in the
// exact vertex order setGeometry flattened the primitives — so a pose snapshot is a straight
// copy back, no re-skinning. Skinning weights are zeroed: the exporter then writes a static mesh.
bool GLModelWidget::snapshotPose(ModelGeometry& geo) const
{
    constexpr int stride = 11;
    qsizetype total = 0;
    for (const MeshPrimitive& p : geo.primitives) total += p.vertices.size();
    if (total == 0 || m_verts.size() != total * stride) return false;
    qsizetype vi = 0;
    for (MeshPrimitive& p : geo.primitives)
        for (MeshVertex& v : p.vertices) {
            const float* s = m_verts.constData() + vi * stride;
            v.px = s[0]; v.py = s[1]; v.pz = s[2];
            v.nx = s[3]; v.ny = s[4]; v.nz = s[5];
            v.joints[0] = v.joints[1] = v.joints[2] = v.joints[3] = 0;
            v.weights[0] = v.weights[1] = v.weights[2] = v.weights[3] = 0.0f;
            ++vi;
        }
    return true;
}

// Start (once) the idle timer that keeps the cloth simulating while the animation is paused, so
// physics-panel slider changes settle live instead of being frozen until the next frame advance.
void GLModelWidget::ensureClothTimer()
{
    if (!m_clothClock.isValid()) m_clothClock.start();
    if (m_clothTimer) return;
    m_clothTimer = new QTimer(this);
    connect(m_clothTimer, &QTimer::timeout, this, [this]() {
        // Hidden/off → don't burn CPU. NOTE: no animation is fine — a rigged cloth model still
        // simulates at its rest pose so gravity and user rotation can move the garment.
        if (!isVisible() || !m_clothEnabled) return;
        if (!m_hasAnim && !simAtRestActive()) return;
        // "No cloth on this model" — but ONLY once the topology has actually been derived.
        // m_sbOrder / m_cages are populated INSIDE applySkinning, so testing them before the
        // first pass deadlocked: the timer skipped, so applySkinning never ran, so they stayed
        // empty forever. (Playing a clip called applySkinning directly via setFrame and broke
        // the deadlock — which is why the sim only ever started after scrubbing an animation.)
        if (m_clothBuilt && m_sbBuilt && m_sbOrder.isEmpty() && m_cages.isEmpty()) return;
        // While a user-rotation swing is still decaying, keep stepping even mid-playback: the
        // orbit-induced motion has to settle visibly rather than freeze at the last drag position.
        const bool spinning = m_cloth.userSpin && std::fabs(m_spinOmega) > 1e-4f;
        if (!spinning && m_clothClock.elapsed() - m_lastFrameStep < 60) return;
        applySkinning();     // one Verlet step at the frozen frame
        // The phys-bone overlay is coloured by live contact state, so it must follow the idle
        // sim too (not just frame changes) or the colours freeze at the last scrubbed frame.
        if (m_showPhysBones) m_physVerts = 0;
        m_hasPending = true;
        update();
    });
    m_clothTimer->start(33);   // ~30 fps idle settle
}

void GLModelWidget::setFrame(int f)
{
    if (!m_hasAnim) return;
    m_frame = qBound(0, f, m_anim.frameCount - 1);
    if (!m_clothClock.isValid()) m_clothClock.start();
    // Wall-clock, each wrapped on ITS OWN clip length: an attachment neither restarts when the body
    // clip loops nor stretches to match it, and two attachments of different lengths stay
    // independent of each other as well.
    if (!m_attachRanges.isEmpty()) {
        const qint64 ms = m_clothClock.elapsed();
        for (int i = 0; i < m_attachRanges.size() && i < m_attachFrame.size(); ++i) {
            const AttachRange& r = m_attachRanges[i];
            const qint64 fr = qint64(double(ms) * double(r.fps > 1.0f ? r.fps : 30.0f) / 1000.0);
            m_attachFrame[i] = int(fr % qint64(qMax(1, r.frames)));
        }
    }
    m_lastFrameStep = m_clothClock.elapsed();   // mark playback so the idle timer stands down
    applySkinning();
    // Camera Snap + follow: pan the target to the followed slot's live position (keep zoom/angle),
    // so the camera tracks e.g. the head as a sit animation moves it.
    if (!m_followParts.isEmpty()) {
        QVector3D c; float r;
        if (partsBounds(m_followParts, c, r)) {
            if (m_camAnim) m_camAnim->stop();   // don't let an in-flight glide fight the follow
            m_center = c; m_tgtCenter = c;
        }
    }
    if (m_showSkeleton) m_skelVerts = 0;   // rebuild the overlay for the new frame
    if (m_showHardpoints) m_hpAxisVerts = 0;   // sockets follow the pose too
    if (m_showPhysBones) m_physVerts = 0;
    m_hasPending = true;
    update();
}

// Column-major 3x3 rotation (r[col*3+row]) that rotates unit vector a onto unit vector b
// (Rodrigues' formula; handles the parallel / anti-parallel degenerate cases).
static void rotFromTo(const float a[3], const float b[3], float r[9])
{
    float ax=a[0],ay=a[1],az=a[2], bx=b[0],by=b[1],bz=b[2];
    float la=std::sqrt(ax*ax+ay*ay+az*az); if (la>1e-8f){ax/=la;ay/=la;az/=la;}
    float lb=std::sqrt(bx*bx+by*by+bz*bz); if (lb>1e-8f){bx/=lb;by/=lb;bz/=lb;}
    const float vx=ay*bz-az*by, vy=az*bx-ax*bz, vz=ax*by-ay*bx;   // cross
    const float c=ax*bx+ay*by+az*bz, s2=vx*vx+vy*vy+vz*vz;        // dot, sin²
    if (s2 < 1e-12f) {                                            // parallel / anti-parallel
        if (c >= 0.0f) { r[0]=1;r[1]=0;r[2]=0; r[3]=0;r[4]=1;r[5]=0; r[6]=0;r[7]=0;r[8]=1; return; }
        float px = (std::fabs(ax) < 0.9f) ? 1.0f : 0.0f, py = (px==0.0f) ? 1.0f : 0.0f;
        float kx=ay*0-az*py, ky=az*px-ax*0, kz=ax*py-ay*px;       // axis ⟂ a
        const float kl=std::sqrt(kx*kx+ky*ky+kz*kz); if (kl>1e-8f){kx/=kl;ky/=kl;kz/=kl;}
        r[0]=2*kx*kx-1; r[1]=2*ky*kx;   r[2]=2*kz*kx;             // 180° about k
        r[3]=2*kx*ky;   r[4]=2*ky*ky-1; r[5]=2*kz*ky;
        r[6]=2*kx*kz;   r[7]=2*ky*kz;   r[8]=2*kz*kz-1;
        return;
    }
    const float k = 1.0f/(1.0f+c);
    r[0]=1+(-vz*vz-vy*vy)*k;  r[1]=vz+(vx*vy)*k;       r[2]=-vy+(vx*vz)*k;
    r[3]=-vz+(vx*vy)*k;       r[4]=1+(-vz*vz-vx*vx)*k; r[5]=vx+(vy*vz)*k;
    r[6]=vy+(vx*vz)*k;        r[7]=-vx+(vy*vz)*k;      r[8]=1+(-vy*vy-vx*vx)*k;
}

// Identify the cloth-bone chains (skel index ≥ baseBones) and their rest lengths/children,
// so springBoneStep can simulate them. Cheap; rebuilt when the skeleton changes.
void GLModelWidget::buildSpringBones()
{
    m_sbBuilt = true; m_sbSeeded = false;
    m_sbOrder.clear();
    const int nb = m_skeleton.size();
    m_sbIsCloth.fill(0, nb); m_sbChild.fill(-1, nb); m_sbLenParent.fill(0.0f, nb);
    m_sbSimHead.fill(0.0f, nb*3); m_sbPrevHead.fill(0.0f, nb*3);
    m_sbHair.fill(0, nb);   // per-bone hair flag (always sized/cleared; populated below)
    // Rigid-link chain bones come straight off the rig (ModelJoint::chain, set when a physics-chain
    // weapon is attached). Nothing else sets it, so this cannot touch a garment.
    m_sbChain.fill(0, nb);
    for (int j = 0; j < nb && j < m_skeleton.size(); ++j)
        if (m_skeleton[j].chain) m_sbChain[j] = 1;
    m_cages.clear();        // cage-level sim state (rebuilt below when cages exist)
    m_sbAnchorPiece.fill(-1, nb); m_sbAnchorVert.fill(-1, nb); m_sbDriven.fill(0, nb);
    m_sbAnchorW.fill(0.0f, nb);
    m_sbContact.fill(0, nb);   // per-bone contact state for the overlay (filled each sim step)
    // ONE definitive line per build: everything needed to tell whether this model can simulate.
    qInfo("cloth-build: bones=%d baseBones=%d cages=%d authoredCaps=%d clothEnabled=%d",
          nb, m_baseBones, int(m_clothSims.size()), int(m_authoredCaps.size()), int(m_clothEnabled));
    if (nb <= 0) { qInfo("cloth-build: ABORT — no skeleton"); return; }
    // baseBones == 0 is the STANDALONE-PIECE case: a single .app parsed on its own (Models tab)
    // carries no authored base-bone count, and only the multi-piece merge path synthesises one —
    // so the old "m_baseBones <= 0 → abort" killed cloth for every model in that tab, while the
    // identical piece simulated fine in Wardrobe (merged onto base00, baseBones=65). With no
    // split to trust, the authored sim cages identify the cloth bones instead (below).
    if (m_baseBones <= 0 && m_clothSims.isEmpty()) {
        qInfo("cloth-build: ABORT — no base-bone count and no authored sim cages");
        return;
    }
    // NO base/cloth split at all (m_baseBones == nb): the parser's gear fallback — a STANDALONE
    // gear piece (Models tab) has no authored nBaseBoneCount, so every bone was counted as base
    // and this function used to return here, which is why cloth physics never ran on any
    // single-piece model while the identical piece worked in Wardrobe (where base00 is the first
    // piece and the split is real). When the piece carries AUTHORED sim cages, the cages
    // themselves tell us which bones are cloth: derive the simulated set from cage-vertex
    // matching below instead of the (absent) index split.
    // "No usable split": either every bone was counted as base, or no count exists at all (0).
    // Both mean the index split can't tell cloth from body — the authored cages do it instead.
    const bool noSplit = (m_baseBones <= 0 || m_baseBones >= nb);
    if (noSplit && m_clothSims.isEmpty()) {
        qInfo("cloth-build: ABORT — no usable base/cloth split (%d of %d) and no authored sim cages",
              m_baseBones, nb);
        return;   // genuinely nothing to simulate
    }
    // Guard against a bad base/cloth split: some rigs (seen on Warlock) carry a base skeleton
    // smaller than the real body rig, so genuine base bones fall past m_baseBones and would be
    // sprung — the whole body then droops/swings with physics on. Real cloth is only a small
    // fraction of a FULL body rig… but STANDALONE cosmetic pieces (the Models tab loads a helm/
    // cape on its own little rig) legitimately have MORE cloth bones than base bones — a plumed
    // helm can be 10 base + 40 cloth. The blanket ratio check therefore killed physics for every
    // single-piece model in the Models tab (while the same piece worked in Wardrobe, where it
    // rides base00's full skeleton). Resolution: when the model carries AUTHORED cloth data
    // (sim cages / capsules — proof those bones are real cloth), proceed, and on a bad ratio
    // restrict the simulated set below to the bones the authored cages actually claim, so a
    // genuinely mis-split rig still can't spring its body. Only a bad ratio WITHOUT any authored
    // evidence keeps the old hard stop.
    // A "bad split" is one where the BASE COUNT ITSELF is implausible — a prop (base=1) or a rig
    // whose base count is far too small to be a character body. The ratio alone is NOT enough:
    // a full body rig (~190–293 bones) wearing a heavy outfit legitimately carries MORE cloth
    // bones than base bones (measured: 598 total / 293 base = 305 cloth), and treating that as
    // suspect culled ~135 real cloth bones down to the cage-claimed subset — so half a cape
    // simulated and half stayed rigid, which reads as bones sticking out at wrong angles.
    // Require both signals: too much cloth AND no credible body rig underneath it.
    constexpr int kMinBodyRig = 64;   // smallest plausible character base rig
    const bool badSplit = !noSplit && (nb - m_baseBones > nb / 2) && (m_baseBones < kMinBodyRig);
    if (badSplit && m_clothSims.isEmpty() && m_authoredCaps.isEmpty()) {
        qInfo("cloth-build: ABORT — bad base/cloth ratio (%d base of %d) with no authored data",
              m_baseBones, nb);
        return;
    }
    // ── AUTHORED per-bone classification (ModelJoint::cloth, from BoneData's
    // nBaseBoneCount/nClothBoneCount split, ORed across pieces by mergeGeometries).
    // When present it SUPERSEDES every index heuristic below: the single merged-index
    // boundary guess broke classes whose BODY carries cloth (spiritborn/druid) — the
    // body's authored cloth bones landed below the guessed boundary (never simulated)
    // while other pieces' bones above it simulated undriven and free-fell to the
    // divergence cap (the measured cloth_cage_diag: body cage followers=116 assigned=0
    // rej[notCloth=89]). Pieces without the authored split (older assets) keep the
    // index-split path unchanged.
    int authoredFlagged = 0, firstFlagged = nb;
    for (int j = 1; j < nb; ++j)
        if (m_skeleton[j].cloth) { ++authoredFlagged; firstFlagged = qMin(firstFlagged, j); }
    const bool authoredSplit = authoredFlagged > 0;
    // Everything below scans "candidate cloth bones": the authored flagged range when
    // present; past the split normally; or the WHOLE skeleton (bar the root) when there
    // is no split and the cages must identify them.
    const int scanStart = authoredSplit ? firstFlagged : (noSplit ? 1 : m_baseBones);
    std::vector<Mat4> restG(nb);
    for (int j = 0; j < nb; ++j) {
        const ModelJoint& jt = m_skeleton[j];
        const Mat4 L = composeTRS(jt.restQ.data(), jt.restT.data(), jt.restS.data());
        const int p = jt.parent; restG[j] = (p >= 0 && p < j) ? mat4mul(restG[p], L) : L;
    }
    if (authoredSplit) {   // the game's own per-bone cloth set (union across pieces)
        for (int j = 1; j < nb; ++j)
            if (m_skeleton[j].cloth) { m_sbIsCloth[j] = 1; m_sbOrder.push_back(j); }   // hierarchical
    } else if (!noSplit)   // with no split the order is derived AFTER cage matching (below)
        for (int j = m_baseBones; j < nb; ++j) { m_sbIsCloth[j] = 1; m_sbOrder.push_back(j); }   // hierarchical
    for (int j = scanStart; j < nb; ++j) {                            // representative child + rest length
        const int p = m_skeleton[j].parent;
        if (p >= 0 && p < nb) {
            if (m_sbChild[p] < 0) m_sbChild[p] = j;
            const float dx=restG[j][12]-restG[p][12], dy=restG[j][13]-restG[p][13], dz=restG[j][14]-restG[p][14];
            m_sbLenParent[j] = std::sqrt(dx*dx + dy*dy + dz*dz);
        }
    }
    // ── Map the authored cloth-cage constraint network + invMass pins onto the bones ──
    // The cage particles coincide with the cloth bones (~1:1, sub-mm), so each cage vertex
    // resolves to a bone; the authored distance constraints then become bone-bone constraints
    // (the 2-D network that holds the cloth as a sheet) and invMass-0 verts pin their bone.
    m_sbConA.clear(); m_sbConB.clear(); m_sbConRest.clear();
    m_sbPin.fill(0, nb);
    m_sbAttach.fill(1.0f, nb);   // default fully free; overwritten per matched bone below
    m_sbSim.fill(-1, nb);
    int cageMatched = 0, cageTotal = 0, cageReSpaced = 0;
    for (int si = 0; si < m_clothSims.size(); ++si) {
        const ClothSim& cs = m_clothSims[si];
        QVector<int> v2b(cs.vertCount, -1);
        // An ATTACHMENT's cage is authored around the trophy's own origin while its bones sit
        // wherever the placement bone put them. Comparing the two directly is how 93% of a back
        // trophy's mesh ends up on bones the solver never claims: every particle misses the 3cm
        // test by metres, so the piece renders rigid. spaceBone names the bone whose rest frame
        // the cage is in; applying it puts particles and bones back in one frame. Identity for
        // every body piece, which have spaceBone -1.
        const bool reSpace = (cs.spaceBone >= 0 && cs.spaceBone < nb);
        const Mat4 toModel = reSpace ? restG[cs.spaceBone] : Mat4{};
        if (reSpace) ++cageReSpaced;
        // Only REAL particles map to bones — entries past nRealVerts are SIMD padding
        // parked at the bind origin, which could otherwise claim a bone near the feet.
        const int v2bReal = (cs.nRealVerts > 0 && cs.nRealVerts <= cs.vertCount)
                                ? cs.nRealVerts : cs.vertCount;
        for (int k = 0; k < v2bReal; ++k) {
            const float* raw = cs.bindVerts.constData() + k*3;
            float cp[3] = { raw[0], raw[1], raw[2] };
            if (reSpace) {
                cp[0] = toModel[0]*raw[0] + toModel[4]*raw[1] + toModel[8] *raw[2] + toModel[12];
                cp[1] = toModel[1]*raw[0] + toModel[5]*raw[1] + toModel[9] *raw[2] + toModel[13];
                cp[2] = toModel[2]*raw[0] + toModel[6]*raw[1] + toModel[10]*raw[2] + toModel[14];
            }
            int best = -1; float bd = 0.03f*0.03f;     // ≤3cm = same point
            for (int b = scanStart; b < nb; ++b) {
                if (authoredSplit && !m_skeleton[b].cloth) continue;   // base bones stay unclaimable
                const float dx=restG[b][12]-cp[0], dy=restG[b][13]-cp[1], dz=restG[b][14]-cp[2];
                const float d2 = dx*dx+dy*dy+dz*dz; if (d2 < bd) { bd = d2; best = b; }
            }
            ++cageTotal;
            if (best >= 0) ++cageMatched;
            v2b[k] = best;
            if (best >= 0) {
                m_sbSim[best] = si;   // this bone belongs to cloth piece si (its tuning)
                if (k < cs.invMasses.size() && cs.invMasses[k] == 0.0f) m_sbPin[best] = 1;
                if (k < cs.attachLen.size()) m_sbAttach[best] = cs.attachLen[k];   // per-bone motion constraint
            }
        }
        for (int e = 0; e < cs.constraintLen.size() && e*2+1 < cs.constraintIdx.size(); ++e) {
            const int a = cs.constraintIdx[e*2], b = cs.constraintIdx[e*2+1];
            if (a < cs.vertCount && b < cs.vertCount) {
                const int ba = v2b[a], bb = v2b[b];
                if (ba >= 0 && bb >= 0 && ba != bb) {
                    m_sbConA.push_back(ba); m_sbConB.push_back(bb); m_sbConRest.push_back(cs.constraintLen[e]);
                }
            }
        }
    }
    // NO split (standalone gear): the simulated set IS the cage-claimed bones — build the order
    // from them now. BAD ratio: trust only cage-claimed bones, everything else drops back to
    // normal skinning. Either way, on a mis-split body rig the unclaimed bones stay rigid.
    // Cage coverage, always printed: a cage that claims nothing is silent otherwise — the piece
    // just renders rigid, which is indistinguishable from "it has no cloth" without this line.
    if (cageTotal > 0)
        qInfo("cloth-cage: %d of %d particle(s) claimed a bone (%.0f%%) · %d attachment cage(s) re-spaced",
              cageMatched, cageTotal, 100.0 * cageMatched / cageTotal, cageReSpaced);
    if (authoredSplit) {
        qInfo("cloth: AUTHORED bone split — %d cloth bone(s) flagged (first %d), index heuristics bypassed",
              authoredFlagged, firstFlagged);
    } else if (noSplit) {
        for (int j = scanStart; j < nb; ++j)
            if (m_sbSim[j] >= 0) { m_sbIsCloth[j] = 1; m_sbOrder.push_back(j); }
        qInfo("cloth: no base/cloth split (standalone piece, %d bones) — %d cage-claimed bone(s) simulate",
              nb, int(m_sbOrder.size()));
    } else if (badSplit) {
        QVector<int> kept;
        kept.reserve(m_sbOrder.size());
        for (int j : m_sbOrder) {
            if (j < m_sbSim.size() && m_sbSim[j] >= 0) kept.push_back(j);
            else m_sbIsCloth[j] = 0;
        }
        m_sbOrder = kept;
        qInfo("cloth: bad base/cloth ratio (%d base / %d total) — restricted to %d cage-claimed bone(s)",
              m_baseBones, nb, int(m_sbOrder.size()));
    }
    qInfo("cloth-build: RESULT — %d simulated bone(s), %d constraint(s), %d pinned",
          int(m_sbOrder.size()), int(m_sbConA.size()),
          int(std::count(m_sbPin.constBegin(), m_sbPin.constEnd(), quint8(1))));
    // Authored plane colliders (ptPlaneDefs = dmClothPlaneDefMirror: localTransform + stiffness +
    // friction + boneIndex — NO explicit normal). The plane passes through the transform's point;
    // its normal is one of the frame axes, but the def doesn't say which. We DERIVE it data-driven:
    // at rest the whole piece must lie on the +normal side, so among the frame's six axes (±x,±y,±z)
    // we pick the one for which the piece's rest cage is the cleanest supporting plane. If no axis
    // is clearly supporting (>=90% of verts on the + side), the plane is skipped — graceful, never
    // shoves the cloth. (Replaces the earlier blind +Z guess that pushed dlux100 to the floor.)
    m_planeBone.clear(); m_planePtBind.clear(); m_planeNmBind.clear();
    for (const ClothSim& cs : m_clothSims) {
        const int nv = cs.bindVerts.size() / 3;
        if (nv < 3) continue;
        for (const ClothPlane& pn : cs.planes) {
            const int b = pn.boneIndex; if (b < 0 || b >= nb) continue;
            const Mat4& m = restG[b];
            auto xf=[&](const float* in, float* o){ o[0]=m[0]*in[0]+m[4]*in[1]+m[8]*in[2]+m[12];
                o[1]=m[1]*in[0]+m[5]*in[1]+m[9]*in[2]+m[13]; o[2]=m[2]*in[0]+m[6]*in[1]+m[10]*in[2]+m[14]; };
            const float lp[3]={pn.localP[0],pn.localP[1],pn.localP[2]};
            float pt[3]; xf(lp, pt);
            // The plane-frame axis columns (from the plane quat), brought to world via restG[b].
            const float* q=pn.localQ.data(); const float qx=q[0],qy=q[1],qz=q[2],qw=q[3];
            const float cols[3][3]={
                {1-2*(qy*qy+qz*qz), 2*(qx*qy+qz*qw),   2*(qx*qz-qy*qw)},
                {2*(qx*qy-qz*qw),   1-2*(qx*qx+qz*qz), 2*(qy*qz+qx*qw)},
                {2*(qx*qz+qy*qw),   2*(qy*qz-qx*qw),   1-2*(qx*qx+qy*qy)} };
            float wax[3][3];
            for (int a=0;a<3;++a){ const float le[3]={lp[0]+cols[a][0],lp[1]+cols[a][1],lp[2]+cols[a][2]};
                float e[3]; xf(le,e); wax[a][0]=e[0]-pt[0]; wax[a][1]=e[1]-pt[1]; wax[a][2]=e[2]-pt[2];
                const float l=std::sqrt(wax[a][0]*wax[a][0]+wax[a][1]*wax[a][1]+wax[a][2]*wax[a][2]);
                if (l>1e-6f){wax[a][0]/=l;wax[a][1]/=l;wax[a][2]/=l;} }
            // Choose the supporting normal: maximise verts on the + side, tie-break by clearance.
            float bNx=0,bNy=0,bNz=0; int bScore=-1; float bMin=-1e9f;
            for (int a=0;a<3;++a) for (int s=-1;s<=1;s+=2){
                const float nx=wax[a][0]*s, ny=wax[a][1]*s, nz=wax[a][2]*s;
                int score=0; float mn=1e9f;
                for (int v=0; v<nv; ++v){ const float* P=cs.bindVerts.constData()+v*3;
                    const float sd=(P[0]-pt[0])*nx+(P[1]-pt[1])*ny+(P[2]-pt[2])*nz;
                    if (sd >= -0.01f) ++score; if (sd<mn) mn=sd; }
                if (score>bScore || (score==bScore && mn>bMin)){ bScore=score; bMin=mn; bNx=nx; bNy=ny; bNz=nz; }
            }
            if (bScore < int(0.9f*nv)) continue;   // no clearly-supporting axis → skip (safe)
            m_planeBone.push_back(b);
            m_planePtBind << pt[0] << pt[1] << pt[2];
            m_planeNmBind << pt[0]+bNx << pt[1]+bNy << pt[2]+bNz;
        }
    }
    m_planePt.fill(0.0f, m_planePtBind.size()); m_planeNm.fill(0.0f, m_planeNmBind.size());

    // ── Hair physics bones (data-driven, from skinning) ──────────────────────────────────
    // D4 hair (ponytails/braids) has physics bones just like cloth, so the loop above sweeps
    // them into the sim. But hair is NOT a cape: it tracks the head tightly with small, well-
    // damped secondary motion. Without its own authored NvCloth cage it would otherwise inherit
    // the equipped ARMOUR's averaged cloth tuning (low tracking, high swing) and, being a long
    // low-mass chain at those settings, self-oscillate ("wiggles on its own"). Flag every bone a
    // hair PART is skinned to; springBoneStep gives those bones hair-class params UNLESS an
    // authored cage already tunes them (authored data always wins). Same part→vertex→bone method
    // buildClothSim uses — no bone-name guessing.
    m_sbHair.fill(0, nb);
    const int vcount = m_bindVerts.size() / 11;
    if (vcount > 0 && !m_indices.isEmpty() && !m_parts.isEmpty()) {
        for (int i = 0; i < m_parts.size(); ++i) {
            if (i >= m_partHair.size() || !m_partHair[i]) continue;
            const Part& p = m_parts[i];
            for (int k = p.offset; k < p.offset + p.count && k < m_indices.size(); ++k) {
                const quint32 vi = m_indices[k];
                if (int(vi) >= vcount || int(vi) >= m_vJoints.size()) continue;
                const auto& J = m_vJoints[vi]; const auto& W = m_vWeights[vi];
                for (int t = 0; t < 4; ++t)
                    if (W[t] > 0.0f && J[t] < nb && m_sbIsCloth[J[t]]) m_sbHair[J[t]] = 1;
            }
        }
    }
    // Force-pin the base bones of any skeletally-attached prop (mount trophy with its own rig):
    // they sit past m_baseBones but are the RIGID mount of the prop, so they must hold the animated
    // pose while only the prop's cloth bones swing. (springBoneStep treats m_sbPin as "follow pose".)
    for (int b : m_pinnedBones) if (b >= 0 && b < nb) m_sbPin[b] = 1;

    // ── Cage runtime (the game's NvCloth path) ────────────────────────────────────────────
    // Build the per-piece cage state springBoneStep simulates at CAGE-VERTEX density. Each
    // cage particle borrows skinning from the nearest cloth-skinned render vert (the sim
    // submesh lives in the same fabric, sub-cm away), so its skinned target tracks the body
    // every frame; particles with no render vert nearby follow their nearest cloth bone.
    {
        const int vcount2 = m_bindVerts.size() / 11;
        QVector<int> clothSkinned;   // render verts with any cloth-bone weight (fabric verts)
        for (int v = 0; v < vcount2 && v < m_vJoints.size(); ++v) {
            const auto& J = m_vJoints[v]; const auto& W = m_vWeights[v];
            for (int k = 0; k < 4; ++k)   // isCloth covers both the split and the cage-claimed modes
                if (W[k] > 0.0f && J[k] < nb && m_sbIsCloth[J[k]]) { clothSkinned.push_back(v); break; }
        }
        QString cageDiag;
        const bool diagOn = qEnvironmentVariableIsSet("D4_DUMP_CLOTH");
        for (int si = 0; si < m_clothSims.size(); ++si) {
            const ClothSim& cs = m_clothSims[si];
            const int nv = cs.vertCount;
            if (nv <= 0 || cs.bindVerts.size() < nv*3) continue;
            if (cs.constraintLen.isEmpty()) {
                if (diagOn) cageDiag += QStringLiteral("SKIP '%1' nv=%2 (no constraintLen)\n").arg(cs.name).arg(nv);
                continue;   // no authored network → bone path handles it
            }
            CageRt rt; rt.simIdx = si;
            rt.J.resize(nv); rt.W.resize(nv);
            rt.pos.fill(0.0f, nv*3); rt.prev.fill(0.0f, nv*3); rt.target.fill(0.0f, nv*3);
            int matched = 0, authoredSkinned = 0;
            // AUTHORED cage skinning first (ptDriverInfluences + ptWeights against the bones
            // ptDriverMap binds the driver frames to — see tools/d4cloth/FINDINGS.md F2 /
            // ROOTCAUSE.md). Per-piece by construction: a cape target can never come from a
            // skirt bone. The old nearest-render-vert borrow (below) crossed garment layers
            // on multi-piece outfits and was the measured root cause of the jutting cape
            // bones (bone 326's jut predicted to 3 decimals from the borrowed-target error).
            const bool authoredSkin = cs.drvInf.size() == nv*4 && cs.drvW.size() == nv*4
                                      && !cs.drvBone.isEmpty();
            for (int k = 0; k < nv; ++k) {
                const float* cp = cs.bindVerts.constData() + k*3;
                if (authoredSkin) {
                    bool ok = true;
                    std::array<quint16,4> J {{0,0,0,0}}; std::array<float,4> W {{0,0,0,0}};
                    for (int t = 0; t < 4; ++t) {
                        const float w = cs.drvW[k*4+t];
                        if (w <= 0.0f) continue;
                        const quint16 di = cs.drvInf[k*4+t];
                        const int b = (di < cs.drvBone.size()) ? cs.drvBone[di] : -1;
                        if (b < 0 || b >= nb) { ok = false; break; }
                        J[t] = quint16(b); W[t] = w;
                    }
                    if (ok) { rt.J[k] = J; rt.W[k] = W; ++authoredSkinned; continue; }
                }
                // Fallback — borrow the skinning of the NEAREST fabric render vert (20cm):
                // kept for assets without the authored arrays. See the pre-port comment in
                // .Backups/src_20260725_pre-d4cloth-port for its original rationale.
                int bestV = -1; float bd = 0.20f*0.20f;
                for (int v : clothSkinned) {
                    const float* q = m_bindVerts.constData() + v*11;
                    const float dx=q[0]-cp[0], dy=q[1]-cp[1], dz=q[2]-cp[2];
                    const float d2=dx*dx+dy*dy+dz*dz; if (d2 < bd) { bd = d2; bestV = v; }
                }
                if (bestV >= 0) { rt.J[k] = m_vJoints[bestV]; rt.W[k] = m_vWeights[bestV]; ++matched; }
                else {
                    int bb = -1; float bbd = 1e30f;
                    for (int b = m_baseBones; b < nb; ++b) {
                        const float dx=restG[b][12]-cp[0], dy=restG[b][13]-cp[1], dz=restG[b][14]-cp[2];
                        const float d2=dx*dx+dy*dy+dz*dz; if (d2 < bbd) { bbd = d2; bb = b; }
                    }
                    rt.J[k] = {{quint16(qMax(bb, 0)), 0, 0, 0}};
                    rt.W[k] = {{bb >= 0 ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f}};
                }
            }
            if (diagOn) cageDiag += QStringLiteral("CAGE '%1' nv=%2 authoredSkin=%3 matchedRenderVert=%4 boneFallback=%5\n")
                                        .arg(cs.name).arg(nv).arg(authoredSkinned).arg(matched)
                                        .arg(nv - authoredSkinned - matched);
            m_cages.push_back(rt);
        }
        // ── MOTION CONSTRAINTS: synthesize when the authored array is missing ──────────────
        // ptAttachmentLengths is per-vert, 0 = locked to the skinned pose, 1 = completely free.
        // The parser only fills it when the payload array length matches the cage vert count
        // exactly; otherwise attachLen stays EMPTY and every read site fell back to 1.0f — i.e.
        // "unconstrained". Cloth then drifts until some unrelated safety clamp catches it, which
        // is what parks the cape bones at a fixed distance from the body every frame.
        //
        // A missing array is a parse gap, not an authoring statement, so rebuild the intent:
        // distance from the nearest PINNED particle, normalized 0..1. That reproduces the shape
        // the game authors — locked along the attached edge, progressively freer toward the hem.
        // Per-sim REACH: how far the farthest particle sits from the pinned edge, in bind space.
        // This is the world-space length that a normalized attachLen of 1.0 refers to.
        m_cageSpan.fill(0.0f, m_clothSims.size());
        for (int si = 0; si < m_clothSims.size(); ++si) {
            const ClothSim& cs3 = m_clothSims[si];
            const int nv3 = cs3.vertCount;
            if (nv3 <= 0 || nv3*3 > cs3.bindVerts.size()) continue;
            QVector<int> pins3;
            for (int k = 0; k < nv3 && k < cs3.invMasses.size(); ++k)
                if (cs3.invMasses[k] == 0.0f) pins3.push_back(k);
            float span = 0.0f;
            if (pins3.isEmpty()) {   // no pinned edge → fall back to the cage's bounding radius
                float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
                for (int k = 0; k < nv3; ++k)
                    for (int c = 0; c < 3; ++c) {
                        const float v = cs3.bindVerts[k*3+c];
                        mn[c] = qMin(mn[c], v); mx[c] = qMax(mx[c], v);
                    }
                const float dx = mx[0]-mn[0], dy = mx[1]-mn[1], dz = mx[2]-mn[2];
                span = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
            } else {
                for (int k = 0; k < nv3; ++k) {
                    const float* p = cs3.bindVerts.constData() + k*3;
                    float best = 1e30f;
                    for (int pi : pins3) {
                        const float* q = cs3.bindVerts.constData() + pi*3;
                        const float dx=p[0]-q[0], dy=p[1]-q[1], dz=p[2]-q[2];
                        best = qMin(best, dx*dx + dy*dy + dz*dz);
                    }
                    span = qMax(span, std::sqrt(best));
                }
            }
            m_cageSpan[si] = span;
        }
        {
            int synth = 0, authored = 0;
            for (ClothSim& cs2 : m_clothSims) {
                const int nv2 = cs2.vertCount;
                if (nv2 <= 0 || (nv2 * 3) > cs2.bindVerts.size()) continue;
                if (cs2.attachLen.size() == nv2) { ++authored; continue; }
                cs2.attachLen.clear();
                QVector<int> pins;
                for (int k = 0; k < nv2 && k < cs2.invMasses.size(); ++k)
                    if (cs2.invMasses[k] == 0.0f) pins.push_back(k);
                if (pins.isEmpty()) {
                    // No pinned edge to measure from: a uniform, modest allowance beats "free".
                    cs2.attachLen.fill(0.35f, nv2);
                } else {
                    cs2.attachLen.resize(nv2);
                    float maxD = 1e-6f;
                    for (int k = 0; k < nv2; ++k) {
                        const float* p = cs2.bindVerts.constData() + k*3;
                        float best = 1e30f;
                        for (int pi : pins) {
                            const float* q = cs2.bindVerts.constData() + pi*3;
                            const float dx=p[0]-q[0], dy=p[1]-q[1], dz=p[2]-q[2];
                            best = qMin(best, dx*dx + dy*dy + dz*dz);
                        }
                        cs2.attachLen[k] = std::sqrt(best);
                        maxD = qMax(maxD, cs2.attachLen[k]);
                    }
                    for (int k = 0; k < nv2; ++k) cs2.attachLen[k] = qBound(0.0f, cs2.attachLen[k]/maxD, 1.0f);
                }
                ++synth;
            }
            if (synth > 0)
                qInfo("cloth-build: motion constraints — %d cage(s) authored, %d SYNTHESIZED "
                      "(authored array absent; previously defaulted to fully free)", authored, synth);
        }
        // ── AUTHORED FOLLOWERS (ptFollowerIndices) — the game's own particle→bone map ──
        // Each cage names, per particle, the exact bone that follows it (rest alignment
        // measured ≤5 µm; no bone is ever named twice; strictly per-piece — see
        // tools/d4cloth/FINDINGS.md F2). This replaces the nearest-particle-within-10cm
        // anchor search wholesale: that search crossed garments on layered outfits (the
        // measured shared-cage defect — bones 329/475 both on cage 5 vert 16 — and the
        // hood's phantom rig latching onto the stor161 cape). Pieces without cloth data
        // (e.g. the Fur-Lined Hood) are simply never cage-driven, matching the game.
        // The old search is kept ONLY as a fallback for assets whose ClothData carries no
        // follower table (none in the test matrix; D4_DUMP_CLOTH reports the path taken).
        int followedBones = 0;
        for (int ci = 0; ci < m_cages.size(); ++ci) {
            const ClothSim& cs = m_clothSims[m_cages[ci].simIdx];
            int rejRange = 0, rejNotCloth = 0, rejPin = 0, asg = 0, authoredNone = 0;
            for (int k = 0; k < cs.followerBone.size(); ++k) {
                const int b = cs.followerBone[k];
                if (b < 0)          { ++authoredNone; continue; }   // authored "no bone"
                if (b >= nb)        { ++rejRange;     continue; }
                if (!m_sbIsCloth[b]){ ++rejNotCloth;  continue; }
                if (m_sbPin[b])     { ++rejPin;       continue; }
                m_sbAnchorPiece[b] = ci; m_sbAnchorVert[b] = k;
                m_sbDriven[b] = 1; m_sbAnchorW[b] = 1.0f;   // exact by construction
                ++followedBones; ++asg;
            }
            // WHY-rejected accounting (D4_DUMP_CLOTH): a cage whose followers are all
            // rejected leaves its bones to free-fall on the spring path — measured on
            // spiF outfits (body skirt: 45/45 driven alone, 0/45 in the outfit). The
            // counters + resolved-driver range name the failing link in one run.
            if (diagOn) {
                int dUnres = 0, dMin = INT_MAX, dMax = -1;
                for (int b2 : cs.drvBone) {
                    if (b2 < 0) { ++dUnres; continue; }
                    dMin = qMin(dMin, b2); dMax = qMax(dMax, b2);
                }
                cageDiag += QStringLiteral(
                    "FOLLOW '%1' (src '%2') followers=%3 assigned=%4 authoredNone=%5 "
                    "rej[range=%6 notCloth=%7 pinned=%8]  drvBone n=%9 unresolved=%10 unified=[%11..%12] "
                    "isClothSpan=[%13..%14) pinnedBones=%15\n")
                    .arg(cs.name, cs.srcApp).arg(cs.followerBone.size()).arg(asg).arg(authoredNone)
                    .arg(rejRange).arg(rejNotCloth).arg(rejPin)
                    .arg(cs.drvBone.size()).arg(dUnres).arg(dMax >= 0 ? dMin : -1).arg(dMax)
                    .arg(m_baseBones).arg(nb)
                    .arg([this]{ int n=0; for (int j=0;j<m_sbPin.size();++j) if (m_sbPin[j]) ++n; return n; }());
            }
        }
        if (followedBones == 0) {
            // FALLBACK (no authored follower data anywhere): the pre-port nearest-particle
            // search, unchanged. Full original comments (10cm radius rationale, drive-weight
            // taper) live in .Backups/src_20260725_pre-d4cloth-port.
            for (int j = scanStart; j < nb; ++j) {
                if (m_sbPin[j] || !m_sbIsCloth[j]) continue;
                const bool hair = (j < m_sbHair.size() && m_sbHair[j]);
                int bp = -1, bv = -1;
                float bd = hair ? 0.03f*0.03f : 0.10f*0.10f;
                const int pj        = m_skeleton[j].parent;
                const int prefCage  = (pj >= 0 && pj < m_sbAnchorPiece.size()) ? m_sbAnchorPiece[pj] : -1;
                constexpr float kForeignPenalty = 4.0f;
                for (int ci = 0; ci < m_cages.size(); ++ci) {
                    const ClothSim& cs = m_clothSims[m_cages[ci].simIdx];
                    const float pen = (prefCage < 0 || ci == prefCage) ? 1.0f : kForeignPenalty;
                    for (int k = 0; k < cs.vertCount; ++k) {
                        const float* cp = cs.bindVerts.constData() + k*3;
                        const float dx=restG[j][12]-cp[0], dy=restG[j][13]-cp[1], dz=restG[j][14]-cp[2];
                        const float d2=(dx*dx+dy*dy+dz*dz) * pen;
                        if (d2 < bd) { bd = d2; bp = ci; bv = k; }
                    }
                }
                if (bp >= 0) {
                    m_sbAnchorPiece[j] = bp; m_sbAnchorVert[j] = bv; m_sbDriven[j] = 1;
                    const float d      = std::sqrt(qMax(0.0f, bd));
                    constexpr float kFull = 0.03f, kFar = 0.10f, kMinW = 0.25f;
                    float w = 1.0f;
                    if (d > kFull) {
                        const float t = qBound(0.0f, (d - kFull) / (kFar - kFull), 1.0f);
                        w = 1.0f - (1.0f - kMinW) * t * t;
                    }
                    m_sbAnchorW[j] = qBound(kMinW, w, 1.0f);
                }
            }
        }
        if (diagOn) {
            int dn = 0; for (int j = scanStart; j < nb; ++j) if (m_sbDriven[j]) ++dn;
            const QString head = QStringLiteral(
                "nb=%1 baseBones=%2 physBones=%3 clothSkinnedRenderVerts=%4 sims=%5 cages=%6 "
                "cageDriven=%7 capsules=%8 authoredCaps=%9\n")
                .arg(nb).arg(m_baseBones).arg(nb - m_baseBones).arg(clothSkinned.size())
                .arg(m_clothSims.size()).arg(m_cages.size()).arg(dn)
                .arg(m_colR0.size()).arg(m_colAuthored ? 1 : 0);
            QFile f(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("cloth_cage_diag.txt")));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                f.write((head + cageDiag).toUtf8()); f.close();
            }
        }
    }
    m_orphanLegacy = qEnvironmentVariableIsSet("D4_CLOTH_LEGACY_ORPHANS");

    // ── CAGE-LESS cloth bones: give them a GEOMETRY-DERIVED tether ───────────────────────────
    // A bone no cage claimed keeps the synthetic m_sbAttach default of 1.0, which under the
    // absolute-tether semantics (FINDINGS F1) means a ~1 wu leash — they dangled limply. The
    // response was a hard clamp (hairTight 0.10 x noCage) that pinned them to within ~7-100 mm of
    // the animated pose, i.e. RIGID: measured on barF_base08_LEG, 122 of 329 cloth bones (37%) are
    // cage-less and their cloth-orphan displacement sits at d = 2-45 mm. Both extremes are wrong.
    //
    // A tether is a LENGTH, so derive it from the chain: how far this bone sits from its chain root
    // (first non-cloth ancestor) in the REST pose. A hem bone 40 cm down the chain may swing on the
    // order of 40 cm; a bone 2 cm from its anchor may not. No per-model constants — it is measured
    // from the rig. Genuine hair keeps its tight clamp (handled by m_sbHair downstream).
    for (int j = scanStart; j < nb; ++j) {
        if (j >= m_sbIsCloth.size() || !m_sbIsCloth[j]) continue;
        if (m_sbSim[j] >= 0) continue;                    // cage-matched: authored tether stands
        if (j < m_sbPin.size() && m_sbPin[j]) continue;
        int r = j, guard = 0;                              // walk to the first non-cloth ancestor
        while (guard++ < 256) {
            const int p = m_skeleton[r].parent;
            if (p < 0 || p >= nb) break;
            if (p < m_sbIsCloth.size() && !m_sbIsCloth[p]) { r = p; break; }
            r = p;
        }
        const float dx = restG[j][12]-restG[r][12], dy = restG[j][13]-restG[r][13],
                    dz = restG[j][14]-restG[r][14];
        const float len = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (len > 1e-4f) m_sbAttach[j] = len;              // absolute wu, same semantics as authored
    }

    int hairBones = 0, hairLoose = 0, drivenBones = 0;
    for (int j = scanStart; j < nb; ++j) {
        if (m_sbHair[j]) { ++hairBones; if (m_sbSim[j] < 0) ++hairLoose; }
        if (j < m_sbDriven.size() && m_sbDriven[j]) ++drivenBones;
    }
    qInfo().noquote() << "SPRINGBONES nb=" << nb << "base=" << m_baseBones
                      << "physBones=" << (nb - m_baseBones) << "hairBones=" << hairBones
                      << "hairLoose(untuned)=" << hairLoose
                      << "cages=" << m_cages.size() << "cageDriven=" << drivenBones;
}

// One spring-bone step: each cloth bone is a Verlet particle (inertia + stiffness toward the
// animated pose + gravity), kept at rigid length from its parent and pushed out of the body
// capsules; the result is written into the bone globals so the skinned mesh swings.
// Decide, per cloth bone, whether the CURRENT clip genuinely animates it (real per-frame motion)
// or just carries a static/rest track. Cloth bones with static tracks are left to NvCloth in the
// game, so we simulate them here — otherwise a rest track makes them "look animated" and they
// rigidly ride the body (the skirt chains that clipped the thighs). Cheap; cached per clip.
void GLModelWidget::computeAnimMoves()
{
    m_sbAnimMovesBuilt = true;
    const int nb = m_skeleton.size();
    m_sbAnimMoves.fill(0, nb);
    if (!m_hasAnim) return;
    constexpr float kMoveT = 0.005f;   // >5mm of travel over the clip = really animated
    constexpr float kMoveR = 0.9990f;  // min |q0·qf| (~2.6°) before we call it real rotation
    // Only the SIMULATED bones matter here (m_sbIsCloth covers both the index split and the
    // cage-claimed set used when a standalone piece has no split).
    for (int j = qMax(0, m_baseBones); j < nb; ++j) {
        if (j < m_sbIsCloth.size() && !m_sbIsCloth[j]) continue;
        const int ai = m_animByHash.value(m_skeleton[j].nameHash, -1);
        if (ai < 0 || ai >= m_anim.bones.size()) continue;   // no track → simulate (leave 0)
        const auto& ba = m_anim.bones[ai];
        bool moves = false;
        const int nt = ba.translations.size();
        if (nt > 1) {
            const auto& t0 = ba.translations[0];
            for (int f = 1; f < nt && !moves; ++f) {
                const auto& t = ba.translations[f];
                const float dx=t[0]-t0[0], dy=t[1]-t0[1], dz=t[2]-t0[2];
                if (dx*dx+dy*dy+dz*dz > kMoveT*kMoveT) moves = true;
            }
        }
        const int nr = ba.rotations.size();
        if (!moves && nr > 1) {
            const auto& q0 = ba.rotations[0];
            for (int f = 1; f < nr && !moves; ++f) {
                const auto& q = ba.rotations[f];
                const float d = std::fabs(q0[0]*q[0]+q0[1]*q[1]+q0[2]*q[2]+q0[3]*q[3]);
                if (d < kMoveR) moves = true;
            }
        }
        m_sbAnimMoves[j] = moves ? 1 : 0;
    }
    if (qEnvironmentVariableIsSet("D4_DUMP_CLOTH")) {
        int total = 0, moving = 0; QString simList;
        for (int j = m_baseBones; j < nb; ++j) {
            ++total;
            if (m_sbAnimMoves[j]) ++moving;
            else if (simList.count(QLatin1Char('\n')) < 40)
                simList += QStringLiteral("  SIM bone %1 '%2'\n").arg(j).arg(m_skeleton[j].name);
        }
        const QString head = QStringLiteral("clothBones=%1 animatedFollow=%2 simulated=%3\n")
                                 .arg(total).arg(moving).arg(total - moving);
        QFile f(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("cloth_animmoves.txt")));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            f.write((head + simList).toUtf8()); f.close();
        }
    }
}

void GLModelWidget::springBoneStep(QVector<Mat4>& global)
{
    if (!m_sbBuilt) buildSpringBones();
    if (!m_sbAnimMovesBuilt) computeAnimMoves();
    if (m_sbOrder.isEmpty()) return;
    const int nb = m_skeleton.size();
    const QVector<Mat4> animG = global;   // animated targets + base rotations
    if (!m_sbSeeded) {
        for (int j : m_sbOrder) {
            m_sbSimHead[j*3]=m_sbPrevHead[j*3]=animG[j][12];
            m_sbSimHead[j*3+1]=m_sbPrevHead[j*3+1]=animG[j][13];
            m_sbSimHead[j*3+2]=m_sbPrevHead[j*3+2]=animG[j][14];
        }
        m_sbSeeded = true; return;
    }
    // Return-to-rest spring (user-tunable "Bone stiffness"). Higher holds the authored shape /
    // droops less; the pins + constraint network do the anchoring/shaping, so this is a gentle
    // pull toward the skinned pose layered on top.
    const float stiff = qBound(0.0f, m_cloth.boneStiffness, 1.0f);
    const float keep  = qBound(0.0f, m_cloth.damping, 0.999f);             // inertia retention
    const float grav  = m_cloth.gravity;
    const float kMargin = m_cloth.collisionMargin;
    // Absolute divergence ceiling, in world units. m_radius is HALF THE BOUNDING-BOX DIAGONAL, so
    // for a ~1.8-tall character it is ~0.97 — meaning the old `m_radius * 1.5` emergency clamp let a
    // cloth bone sit 80% of the body's height away from its skinned pose before anything objected.
    // Real cloth never needs that: a cape hem swings maybe 10-15% of body scale. 0.28 is generous
    // for the longest authored capes and still bounds a diverging solver to something plausible.
    const float kDivergeMax = qMax(0.001f, m_radius) * 0.28f;
    const int kIters = qBound(6, m_cloth.iterations * 4, 30);
    // Sub-steps per frame (see the cage loop). Forces are divided by this; the solve runs N times.
    const int   subSteps = qBound(1, m_cloth.subSteps, 4);
    const float subF     = 1.0f / float(subSteps);
    // One "Capsule size" knob scales every collision capsule. Authored capsules store the game's
    // exact radii and skin-fit capsules already bake in the 0.55 heuristic, so 1.0 = natural size
    // for both; raising it pushes cloth further off the body.
    // AUTHORED CAPSULES vs THE SLIDER. m_colAuthored was introduced to stop the slider shrinking
    // the game's exact radii (see the comment where it is set, and GLModelWidget.h:495) — but it
    // was only ever set and logged, never read here, so authored capsules have been running at the
    // 0.52 default ever since: the game's 70-331 mm radii applied at 52%. Measured across the
    // corpus, authored capsules carry no scale/hide/solver variation (all 143 are scale=1, hide=0,
    // solver=2, friction=0.10), so radius is the only authored quantity that matters — which makes
    // this the whole of the discrepancy. Opt-in for now (D4_CAPS_FULL=1) because 0.52 is visually
    // verified on the current build and a blind flip regressed before; A/B it, then decide.
    const float rScale = (m_colAuthored && m_capsFullSize) ? 1.0f : m_cloth.capsuleRadius;
    // PER-CAPSULE scale = global rScale x the capsule's body-region trim. The region was resolved
    // once at build time (m_colRegion); this is a array lookup per capsule, not a per-frame search.
    auto capScale = [this, rScale](int i) {
        const quint8 rg = (i < m_colRegion.size()) ? m_colRegion[i] : quint8(ClothParams::CapOther);
        return rScale * ((rg < ClothParams::CapRegionCount) ? m_cloth.capRegion[rg] : 1.0f);
    };

    // ── User-driven inertia ("React to rotation") ───────────────────────────────────────────
    // Orbiting the camera is, to the eye, spinning the MODEL — so give the cloth the fictitious
    // forces of that spin about the model's vertical (Y) axis. Per step we measure the yaw delta,
    // smooth it into an angular velocity (so a flick keeps swinging after the mouse stops), then
    // per particle add:
    //   · Euler/lag term  −ω̇ × r : the fabric trails when the turn starts or stops;
    //   · centrifugal     ω² · r  : it fans outward while the turn continues.
    // Both are accelerations added to the Verlet step, so damping/constraints/collision still own
    // the result — this can only nudge the sim, never teleport it.
    float spinOmega = 0.0f, spinAlpha = 0.0f;
    const bool spinOn = m_cloth.userSpin && m_cloth.userSpinForce > 0.0f;
    if (spinOn) {
        float d = m_yaw - m_spinPrevYaw;
        while (d >  3.14159265f) d -= 6.2831853f;   // shortest way round
        while (d < -3.14159265f) d += 6.2831853f;
        if (!m_spinSeeded) { d = 0.0f; m_spinSeeded = true; }
        m_spinPrevYaw = m_yaw;
        const float prevOmega = m_spinOmega;
        m_spinOmega = m_spinOmega * 0.80f + d * 0.20f;   // smooth → carries past the drag
        if (std::fabs(m_spinOmega) < 1e-5f) m_spinOmega = 0.0f;
        spinOmega = m_spinOmega;
        spinAlpha = m_spinOmega - prevOmega;             // angular acceleration (the "kick")
    } else {
        m_spinOmega = 0.0f; m_spinPrevYaw = m_yaw; m_spinSeeded = false;
    }
    // Perceptual force curve: the raw terms were tuned around 1.0, which crammed every usable
    // value into the bottom of a 0–5 slider (0.1 already looked strong). Square the input so the
    // low end is fine-grained — 0.1 → 0.01× (a whisper), 1.0 → 1× (natural), 5 → 25× (dramatic) —
    // and drop the base multipliers accordingly.
    const float spinIn = qBound(0.0f, m_cloth.userSpinForce, 5.0f);
    const float spinK = spinIn * spinIn;
    // Cap the induced acceleration so a violent mouse flick can't inject enough energy to punch
    // the cloth through the body — collision quality degrades fast past this, and the swing looks
    // no better. Scaled to the model so it's size-independent.
    const float spinAccelCap = qMax(0.002f, m_radius * 0.05f);
    // Air drag also damps the rotation response, so it settles like real cloth instead of ringing.
    const float spinDrag = 1.0f - qBound(0.0f, m_cloth.dragFactor, 1.0f) * 0.5f;
    // Spin axis = vertical through the MODEL's framed centre. (Not m_center — that's the camera
    // orbit target, which right-drag panning moves off the model; using it made the lever arm
    // wrong, and after a pan it would fling the cloth.) A minimum lever arm keeps compact or
    // centre-hugging pieces (helm plumes, collars) responsive instead of dead on the axis.
    const float spinCx = m_homeCenter.x(), spinCz = m_homeCenter.z();
    const float spinMinR = qMax(0.05f, m_radius * 0.25f);
    // Adds the rotation-induced acceleration for a particle at P into (ax, az).
    auto spinAccel = [&](const float* P, float& ax, float& az) {
        if (!spinOn || (spinOmega == 0.0f && spinAlpha == 0.0f)) return;
        float rx = P[0] - spinCx, rz = P[2] - spinCz;
        const float r = std::sqrt(rx*rx + rz*rz);
        if (r < spinMinR) {   // near the axis: keep the direction, floor the lever arm
            if (r > 1e-6f) { const float s = spinMinR / r; rx *= s; rz *= s; }
            else           { rx = spinMinR; rz = 0.0f; }
        }
        // Tangent of the rotation about +Y at this radius: ω × r = (ω·rz, 0, −ω·rx).
        float sx = (-spinAlpha * rz) * spinK * 60.0f       // lag behind the change in spin
                 + (spinOmega * spinOmega * rx) * spinK * 15.0f;   // fan outward while spinning
        float sz = ( spinAlpha * rx) * spinK * 60.0f
                 + (spinOmega * spinOmega * rz) * spinK * 15.0f;
        // NOTE: no sub-step division here — only the CAGE path sub-steps, and it scales its own
        // forces. Applying subF unconditionally halved the rotation force on every bone-path
        // (non-caged) piece, which runs exactly one integration per frame.
        sx *= spinDrag; sz *= spinDrag;
        const float m2 = sx*sx + sz*sz;                     // cap: never enough energy to tunnel
        if (m2 > spinAccelCap*spinAccelCap) {
            const float s = spinAccelCap / std::sqrt(m2);
            sx *= s; sz *= s;
        }
        ax += sx; az += sz;
    };

    // ── Contact resolution ─────────────────────────────────────────────────────────────────────
    // The old resolver was a bare positional push to the NEAREST capsule surface. Three failure
    // modes followed, and together they are the "bones stuck inside the collision boxes" bug:
    //   · no velocity fix — Verlet derives velocity from (P − prev), so shoving P outward without
    //     touching prev MANUFACTURES outward speed; the particle rebounds, re-enters, and buzzes
    //     (or tunnels) forever;
    //   · nearest-surface exit — once a particle is past the capsule axis the nearest way out is
    //     the FAR side, so a deep penetration gets pushed out the wrong side and is now trapped;
    //   · no swept test — one fast step (an animation snap, or the rotation impulse) can carry a
    //     particle clean through a thin limb without ever sampling it as inside.
    // This resolver fixes all three: it remembers which side the particle came from (prev), sweeps
    // the segment prev→P so fast motion still registers, pushes out along the ENTRY normal, then
    // corrects prev so the normal component of velocity is cancelled and the tangential component
    // is scaled by friction (the standard PhysX/NvCloth contact response). `friction` 0..1.
    // IMPORTANT — velocity correction runs ONCE per step, never inside the iteration loop. Between
    // iterations the constraint solver moves P by amounts that are NOT motion; treating (P − prev)
    // as velocity there and rewriting prev feeds constraint displacement back in as energy, and
    // over ~60 calls a frame the cloth detonates (bones flung out to the clamp radius). So:
    //   · inside the solver loop → positional push only  (fixVel = false)
    //   · once after the loop    → the velocity/friction response (fixVel = true)
    const float fricK = qBound(0.0f, m_cloth.friction, 1.0f);
    int   contactCount = 0;      // diagnostics (worst penetration this build)
    float worstPen = 0.0f;
    // Max distance any particle may travel in one step: a fraction of the THINNEST collider, so a
    // step can never straddle a limb. Computed once per sim step (cheap) and floored so a model
    // with tiny colliders doesn't freeze the cloth solid.
    float minColR = 1e30f;
    for (int i = 0; i < m_colR0.size(); ++i)
        minColR = qMin(minColR, qMin(m_colR0[i], m_colR1[i]) * capScale(i));
    const float maxStep = (minColR < 1e29f) ? qMax(0.01f, minColR * 0.75f) : 1e30f;
    auto clampStep = [maxStep](const float* from, float& nx, float& ny, float& nz) {
        const float dx=nx-from[0], dy=ny-from[1], dz=nz-from[2];
        const float d2 = dx*dx+dy*dy+dz*dz;
        if (d2 <= maxStep*maxStep || d2 < 1e-12f) return;
        const float s = maxStep / std::sqrt(d2);
        nx = from[0] + dx*s; ny = from[1] + dy*s; nz = from[2] + dz*s;
    };
    auto collide = [&](float* P, float* Q, bool fixVel) {
        for (int i = 0; i < m_colR0.size(); ++i) {
            const float* p0=m_colP0.constData()+i*3; const float* p1=m_colP1.constData()+i*3;
            const float sx=p1[0]-p0[0], sy=p1[1]-p0[1], sz=p1[2]-p0[2]; const float sl2=sx*sx+sy*sy+sz*sz;
            auto axisT = [&](const float* X) {
                float t = sl2 > 1e-8f ? ((X[0]-p0[0])*sx + (X[1]-p0[1])*sy + (X[2]-p0[2])*sz) / sl2 : 0.f;
                return qBound(0.f, t, 1.f);
            };
            const float t  = axisT(P);
            const float cx=p0[0]+sx*t, cy=p0[1]+sy*t, cz=p0[2]+sz*t;
            float dx=P[0]-cx, dy=P[1]-cy, dz=P[2]-cz;
            float d = std::sqrt(dx*dx+dy*dy+dz*dz);
            const float r = (m_colR0[i]+(m_colR1[i]-m_colR0[i])*t)*capScale(i) + kMargin;
            if (d >= r) continue;                       // outside this capsule — nothing to do
            // ENTRY normal: the direction the particle occupied one step ago. Using it instead of
            // the (possibly reversed) current offset is what stops deep penetrations exiting the
            // far side and getting wedged inside a limb.
            if (Q) {
                const float qt = axisT(Q);
                const float qcx=p0[0]+sx*qt, qcy=p0[1]+sy*qt, qcz=p0[2]+sz*qt;
                float ex=Q[0]-qcx, ey=Q[1]-qcy, ez=Q[2]-qcz;
                const float el = std::sqrt(ex*ex+ey*ey+ez*ez);
                if (el > 1e-5f) {
                    ex/=el; ey/=el; ez/=el;
                    if (d <= 1e-5f || (dx*ex + dy*ey + dz*ez) < 0.0f) {   // reversed / degenerate
                        dx = ex; dy = ey; dz = ez; d = qMax(d, 1e-5f);
                    }
                }
            }
            if (d <= 1e-5f) { dx = 0.0f; dy = 1.0f; dz = 0.0f; d = 1e-5f; }   // dead centre fallback
            const float nx=dx/d, ny=dy/d, nz=dz/d;      // unit surface normal
            const float pen = r - d;
            if (pen > worstPen) worstPen = pen;
            ++contactCount;
            P[0] = cx + nx*r; P[1] = cy + ny*r; P[2] = cz + nz*r;   // place ON the surface
            if (!Q || !fixVel) continue;   // positional-only while iterating (see note above)
            // Velocity response: kill the inward normal component (no rebound → no buzzing), keep
            // the tangential slide scaled by friction. Implemented by moving prev, since Verlet
            // reads velocity as (P − prev).
            float vx=P[0]-Q[0], vy=P[1]-Q[1], vz=P[2]-Q[2];
            const float vn = vx*nx + vy*ny + vz*nz;
            float tvx = vx - vn*nx, tvy = vy - vn*ny, tvz = vz - vn*nz;   // tangential
            const float keepT = 1.0f - fricK;
            tvx *= keepT; tvy *= keepT; tvz *= keepT;
            const float outN = qMax(0.0f, vn);          // outward motion may continue; inward dies
            Q[0] = P[0] - (tvx + outN*nx);
            Q[1] = P[1] - (tvy + outN*ny);
            Q[2] = P[2] - (tvz + outN*nz);
        }
    };
    // Swept guard: if the step from Q to P crosses a capsule entirely (tunnelling), pull P back to
    // the entry point first so the resolver above sees the contact. Cheap segment-vs-capsule test,
    // only run when the particle actually moved a meaningful distance.
    auto sweep = [&](float* P, float* Q) {
        const float mx=P[0]-Q[0], my=P[1]-Q[1], mz=P[2]-Q[2];
        const float mv2 = mx*mx+my*my+mz*mz;
        if (mv2 < 1e-8f) return;
        for (int i = 0; i < m_colR0.size(); ++i) {
            const float* p0=m_colP0.constData()+i*3; const float* p1=m_colP1.constData()+i*3;
            const float r = qMax(m_colR0[i], m_colR1[i])*capScale(i) + kMargin;
            // Coarse reject: segment midpoint vs capsule bounding sphere.
            const float bcx=(p0[0]+p1[0])*0.5f, bcy=(p0[1]+p1[1])*0.5f, bcz=(p0[2]+p1[2])*0.5f;
            const float hx=p1[0]-bcx, hy=p1[1]-bcy, hz=p1[2]-bcz;
            const float bR = std::sqrt(hx*hx+hy*hy+hz*hz) + r;
            const float ex=Q[0]-bcx, ey=Q[1]-bcy, ez=Q[2]-bcz;
            const float far = std::sqrt(ex*ex+ey*ey+ez*ez) - std::sqrt(mv2) - bR;
            if (far > 0.0f) continue;
            // Only guard TUNNELLING — the start point must be genuinely OUTSIDE this capsule.
            // A settled particle rests exactly on the surface, so without this test the first
            // sample of its next step reads as "inside" and the step gets clamped to a quarter,
            // every frame: cloth resting on the body could no longer slide along it.
            {
                float qt = 0.f;
                const float qax=p1[0]-p0[0], qay=p1[1]-p0[1], qaz=p1[2]-p0[2];
                const float qal2=qax*qax+qay*qay+qaz*qaz;
                if (qal2 > 1e-8f) qt = qBound(0.f, ((Q[0]-p0[0])*qax+(Q[1]-p0[1])*qay+(Q[2]-p0[2])*qaz)/qal2, 1.f);
                const float qcx=p0[0]+qax*qt, qcy=p0[1]+qay*qt, qcz=p0[2]+qaz*qt;
                const float qdx=Q[0]-qcx, qdy=Q[1]-qcy, qdz=Q[2]-qcz;
                const float qr=(m_colR0[i]+(m_colR1[i]-m_colR0[i])*qt)*capScale(i) + kMargin;
                if (qdx*qdx+qdy*qdy+qdz*qdz <= qr*qr) continue;   // started inside → not tunnelling
            }
            // Sample the segment; first sample inside → step back to just before it.
            constexpr int kS = 4;
            for (int s = 1; s <= kS; ++s) {
                const float u = float(s) / float(kS);
                const float X[3] = { Q[0]+mx*u, Q[1]+my*u, Q[2]+mz*u };
                float tt = 0.f;
                const float ax=p1[0]-p0[0], ay=p1[1]-p0[1], az=p1[2]-p0[2];
                const float al2=ax*ax+ay*ay+az*az;
                if (al2 > 1e-8f) tt = qBound(0.f, ((X[0]-p0[0])*ax+(X[1]-p0[1])*ay+(X[2]-p0[2])*az)/al2, 1.f);
                const float ccx=p0[0]+ax*tt, ccy=p0[1]+ay*tt, ccz=p0[2]+az*tt;
                const float ddx=X[0]-ccx, ddy=X[1]-ccy, ddz=X[2]-ccz;
                const float rr=(m_colR0[i]+(m_colR1[i]-m_colR0[i])*tt)*capScale(i) + kMargin;
                if (ddx*ddx+ddy*ddy+ddz*ddz < rr*rr) {   // entered here → clamp the step
                    P[0]=X[0]; P[1]=X[1]; P[2]=X[2];
                    return;
                }
            }
        }
    };
    // ── CAGE-LEVEL SIM (the game's NvCloth path) ─────────────────────────────────
    // Simulate each authored cage at cage-vertex density: authored invMass pins follow the
    // skinned pose; free particles Verlet-integrate with per-piece tuning; the authored
    // constraint network + capsule/plane collision + authored per-particle motion limits
    // (ptAttachmentLengths) iterate together. Bones anchored to a cage are then DRIVEN from
    // it (below) instead of being simulated — collision happens where the fabric actually
    // is, so a walking thigh pushes the whole skirt panel out instead of slipping between
    // sparse bone heads.
    QVector<Mat4> cagePal; QVector<quint8> cagePalDone;   // lazy palette for cage targets
    if (!m_cages.isEmpty()) { cagePal.resize(nb); cagePalDone.fill(0, nb); }
    auto cagePalGet = [&](int b) -> const Mat4& {
        if (!cagePalDone[b]) { cagePal[b] = mat4mul(animG[b], m_skeleton[b].inverseBind); cagePalDone[b] = 1; }
        return cagePal[b];
    };
    // CONTINUOUS COLLIDERS: interpolate capsule poses prev→current across the sub-steps,
    // the way the game feeds NvCloth start/end collider transforms. Posing once per frame
    // made a walking thigh TELEPORT past the fabric (several cm per frame at walk speed) —
    // the resolver then found the cloth already inside/behind the capsule: the strong
    // leg-through-skirt clipping under animation. A margin can't fix temporal aliasing.
    const QVector<float> colCur0 = m_colP0, colCur1 = m_colP1;
    const bool colLerp = m_colPrevValid && m_colP0Prev.size() == m_colP0.size()
                         && m_colP1Prev.size() == m_colP1.size();
    for (CageRt& rt : m_cages) {
        if (rt.simIdx < 0 || rt.simIdx >= m_clothSims.size()) continue;
        const ClothSim& cs = m_clothSims[rt.simIdx];
        // REAL particle set: cs.vertCount is the array CAPACITY; the authored count is
        // nRealVerts. Particles past it are SIMD padding parked at the bind origin — they
        // used to simulate (and were legal 10cm-anchor targets near the model origin).
        const int nv = (cs.nRealVerts > 0 && cs.nRealVerts <= cs.vertCount) ? cs.nRealVerts
                                                                            : cs.vertCount;
        if (nv <= 0 || rt.pos.size() < nv*3) continue;
        // Skinned target per cage particle — where the authored garment shape puts it now.
        for (int k = 0; k < nv; ++k) {
            const float* bp = cs.bindVerts.constData() + k*3;
            float tx=0, ty=0, tz=0, ws=0;
            for (int t = 0; t < 4; ++t) {
                const float w = rt.W[k][t]; if (w <= 0.0f) continue;
                const int b = rt.J[k][t];   if (b < 0 || b >= nb) continue;
                const Mat4& m = cagePalGet(b);
                tx += w*(m[0]*bp[0]+m[4]*bp[1]+m[8]*bp[2] +m[12]);
                ty += w*(m[1]*bp[0]+m[5]*bp[1]+m[9]*bp[2] +m[13]);
                tz += w*(m[2]*bp[0]+m[6]*bp[1]+m[10]*bp[2]+m[14]);
                ws += w;
            }
            float* T = rt.target.data() + k*3;
            if (ws > 1e-6f) { T[0]=tx/ws; T[1]=ty/ws; T[2]=tz/ws; }
            else            { T[0]=bp[0]; T[1]=bp[1]; T[2]=bp[2]; }
        }
        if (!rt.seeded) { rt.pos = rt.target; rt.prev = rt.target; rt.seeded = true; continue; }
        // Per-piece authored tuning (.clt.json): gravity scale + self-wind; bone-tracking
        // scales the motion limit exactly like the bone path so the sliders keep their feel.
        const bool tuned = cs.tuned;
        auto pinnedAt = [&](int k) { return k < cs.invMasses.size() && cs.invMasses[k] == 0.0f; };
        // SUB-STEPPING: split the frame into N smaller integrate+solve passes with the forces
        // divided by N. Small steps converge far better under fast motion (animation snaps, the
        // rotation impulse) than one big step at any tuning — this is the single biggest stability
        // win available. Cost scales linearly, so it's user-selectable and defaults to 2.
        for (int ss = 0; ss < subSteps; ++ss) {
        if (colLerp) {   // pose colliders at this sub-step's time slice
            const float aT = float(ss + 1) / float(subSteps);
            for (int i = 0; i < colCur0.size(); ++i) {
                m_colP0[i] = m_colP0Prev[i] + (colCur0[i] - m_colP0Prev[i]) * aT;
                m_colP1[i] = m_colP1Prev[i] + (colCur1[i] - m_colP1Prev[i]) * aT;
            }
        }
        // AUTHORED GRAVITY. The game's cloth always falls (vGravity, typically z=-22 —
        // .clt gravScale is |z|/20, so magnitude = gravScale×20 wu/s² = gravScale/180 per
        // frame² at 60 Hz). Pre-port the cape only LOOKED draped because the motion limit
        // glued particles to the skinned pose; the authored tether removed that glue, so a
        // zero Gravity slider left the cage floating rigidly in its bind shape. Slider
        // semantics now: 0 = authored gravity (matches the game); non-zero = manual override.
        const float gAuth = -(tuned ? cs.gravScale : 1.0f) * (20.0f / 3600.0f);
        // GRAVITY SLIDER UNITS, fixed: the slider historically stored a RAW per-frame
        // acceleration (default 0.025) from the pre-port era when cloth was glued to its
        // targets. Authored game gravity is 20 wu/s² = 0.0056/frame² — so a persisted
        // 0.025 silently ran ~4.5× game gravity, blasting skirts through the leg
        // capsules to full tether (the measured line-through-body hang; every stiffness/
        // collision fix "looked the same" because gravity dominated). The slider is now a
        // MULTIPLIER anchored in game units: |0.025| ≡ exactly 1.0× this piece's authored
        // magnitude, so existing saved settings land on authentic behaviour; 0 = authored.
        const float gj = (grav != 0.0f ? gAuth * (grav / -0.025f) : gAuth) * subF;
        const float wx = cs.windX*subF, wy = cs.windY*subF, wz = cs.windZ*subF;
        // Per-piece authored air drag (harness-validated): keep x= (1 - flDragFactor*0.1).
        const float keepP = tuned ? keep * (1.0f - cs.dragF * 0.1f) : keep;
        for (int k = 0; k < nv; ++k) {
            float* P = rt.pos.data()+k*3; float* Q = rt.prev.data()+k*3;
            const float* T = rt.target.constData()+k*3;
            if (pinnedAt(k)) { Q[0]=P[0];Q[1]=P[1];Q[2]=P[2]; P[0]=T[0];P[1]=T[1];P[2]=T[2]; continue; }
            float sax = 0.0f, saz = 0.0f;
            spinAccel(P, sax, saz);   // user rotation → lag + centrifugal
            sax *= subF; saz *= subF;   // this path sub-steps: take its share of the force
            float n0=P[0]+(P[0]-Q[0])*keepP + wx + sax, n1=P[1]+(P[1]-Q[1])*keepP + gj + wy, n2=P[2]+(P[2]-Q[2])*keepP + wz + saz;
            // AUTHORED ATTACHMENT SPRING (flAttachmentStiffness): the game's per-piece pull
            // back toward the skinned pose — what makes a crest rigid (0.9) while a cape
            // (0.3) drapes. Quadratic mapping keeps low/mid values gentle (cape 0.3 → 0.045,
            // barely above the global 0.02 slider) while high values dominate (0.9 → 0.40 →
            // near-rigid), floored at the user's Bone-stiffness slider.
            const float stiffJ = tuned ? qMax(stiff, cs.attachStiff * cs.attachStiff * 0.5f) : stiff;
            n0+=(T[0]-n0)*stiffJ; n1+=(T[1]-n1)*stiffJ; n2+=(T[2]-n2)*stiffJ;
            // Per-step travel clamp: never move further than a fraction of the thinnest collider in
            // one step. A hard guarantee against tunnelling (and against a violent spin impulse
            // launching the cloth), independent of frame rate or how hard the sliders are pushed.
            clampStep(P, n0, n1, n2);
            Q[0]=P[0];Q[1]=P[1];Q[2]=P[2]; P[0]=n0;P[1]=n1;P[2]=n2;
            sweep(P, Q);   // fast motion: stop at the entry point so the resolver sees the contact
        }
        const float trk = tuned ? qBound(0.0f, cs.boneTrack, 1.0f)
                                : qBound(0.0f, m_cloth.boneTracking, 1.0f);
        const float tnorm = qBound(0.0f, (1.0f - trk) / 0.55f, 1.0f);
        // UNITS, corrected (tools/d4cloth/FINDINGS.md F1): attachLen is the authored TETHER
        // length in ABSOLUTE world units — the allowed rope distance from the particle's
        // kinematic-root particle (r=1.0000 against the summed parent-chain length on every
        // block measured). It is NOT normalized and NOT relative to the skinned pose, so the
        // old cageSpan/tnorm² scaling double-scaled it and referenced the wrong point. The
        // Max-distance slider survives as a plain scale on the authored length (1.0 = game).
        // mdScale is kept ONLY for the no-root fallback below and the safety re-bound.
        const float span    = (rt.simIdx < m_cageSpan.size() && m_cageSpan[rt.simIdx] > 1e-4f)
                              ? m_cageSpan[rt.simIdx] : 1.0f;
        // Max-distance slider, SANITY-CLAMPED to [0.05, 2]x for authored-data paths: the
        // slider multiplies AUTHORED tether lengths (1.0 = game). Values like 10 (saved
        // while diagnosing the old explosion) silently disabled the tether entirely and
        // with it every authored-limit fix. Extremes now cannot escape authored bounds.
        const float mdSlider = qBound(0.05f, m_cloth.maxDistance, 2.0f);
        const float mdScale = mdSlider * qMax(0.05f, tnorm * tnorm) * span;
        const bool hasRoots = cs.kinRoots.size() >= nv;
        // PER-CLASS STIFFNESS (warp/weft/shear/bend clusters + the .clt-driven class
        // stiffnesses already on the sliders). Full-stiffness corrections over 30
        // iterations made the cage a near-rigid shell that kept its bind curvature —
        // gravity could only rotate the "board", leaving the hem flared/pointing up.
        // PBD-correct per-iteration factor: k_it = 1-(1-k)^(1/N) — naive per-iteration k
        // still converges to rigid over N iterations (1-(1-0.21)^30 ≈ 0.999).
        // Class→slider mapping (assumed, flagged in FINDINGS): warp=stretch, weft=horiz.
        float kCls[4];
        {
            // AUTHORED per-piece class stiffness when tuned (flStretching/Horizontal/
            // Shear/BendingStiffness — skirts author stretch ≈0.85); the global sliders
            // only cover untuned pieces. Running tuned pieces at the slider default
            // (stretch 0.21) let skirt warp chains stretch under authored gravity until
            // the garment hung as a vertical line through the body (measured: the
            // harness with authored 0.85 drapes the same data cleanly).
            const float raw[4] = {
                tuned ? cs.clsStiff[0] : m_cloth.stretchStiffness,
                tuned ? cs.clsStiff[1] : m_cloth.horizStiffness,
                tuned ? cs.clsStiff[2] : m_cloth.shearStiffness,
                tuned ? cs.clsStiff[3] : m_cloth.bendStiffness };
            for (int c = 0; c < 4; ++c) {
                const float k = qBound(0.05f, raw[c], 1.0f);
                kCls[c] = (k >= 0.999f) ? 1.0f
                                        : 1.0f - std::pow(1.0f - k, 1.0f / float(kIters));
            }
        }
        for (int it = 0; it < kIters; ++it) {
            // Authored distance-constraint network (the sheet) — invMass-weighted. Pairs with
            // an endpoint past nRealVerts (SIMD padding, incl. self-pairs) are skipped by the
            // a/b >= nv guard now that nv is the REAL count.
            for (int e = 0; e < cs.constraintLen.size() && e*2+1 < cs.constraintIdx.size(); ++e) {
                const int a = cs.constraintIdx[e*2], b = cs.constraintIdx[e*2+1];
                if (a >= nv || b >= nv || a == b) continue;
                float* A = rt.pos.data()+a*3; float* B = rt.pos.data()+b*3;
                float dx=B[0]-A[0], dy=B[1]-A[1], dz=B[2]-A[2];
                const float len=std::sqrt(dx*dx+dy*dy+dz*dz); if (len < 1e-6f) continue;
                const float wa=pinnedAt(a)?0.f:1.f, wb=pinnedAt(b)?0.f:1.f, wsum=wa+wb;
                if (wsum < 1e-6f) continue;
                const quint8 cls = (e < cs.conClass.size()) ? cs.conClass[e] : quint8(255);
                const float kIt = (cls < 4) ? kCls[cls] : 1.0f;
                const float diff=(len-cs.constraintLen[e])/len * kIt, sa=diff*wa/wsum, sb=diff*wb/wsum;
                A[0]+=dx*sa;A[1]+=dy*sa;A[2]+=dz*sa; B[0]-=dx*sb;B[1]-=dy*sb;B[2]-=dz*sb;
            }
            // Authored TETHER (ptAttachmentLengths + ptKinematicRoots): rope limit around the
            // kinematic root particle's CURRENT position — the game's LRA model. Tether-taut
            // is the natural state of hanging cloth, not a clamp smell. Before collision so
            // collision has the final say each iteration (NvCloth order). Verts without a
            // root keep the old target-relative clamp as a bounded fallback.
            for (int k = 0; k < nv; ++k) {
                if (pinnedAt(k)) continue;
                float* P = rt.pos.data()+k*3;
                const float al = (k < cs.attachLen.size()) ? cs.attachLen[k] : 1.0f;
                const int root = (hasRoots && cs.kinRoots[k] < nv) ? cs.kinRoots[k] : -1;
                if (root >= 0) {
                    const float* R = rt.pos.constData()+root*3;   // pinned roots sit on their target
                    const float lim = al * mdSlider;   // ABSOLUTE wu × slider (clamped, see above)
                    const float dx=P[0]-R[0], dy=P[1]-R[1], dz=P[2]-R[2];
                    const float d=std::sqrt(dx*dx+dy*dy+dz*dz);
                    if (d > lim && d > 1e-8f) { const float s=lim/d; P[0]=R[0]+dx*s; P[1]=R[1]+dy*s; P[2]=R[2]+dz*s; }
                } else {
                    const float* T = rt.target.constData()+k*3;
                    const float md = al * mdScale;
                    const float dx=P[0]-T[0], dy=P[1]-T[1], dz=P[2]-T[2];
                    const float d=std::sqrt(dx*dx+dy*dy+dz*dz);
                    if (d > md && d > 1e-8f) { const float s=md/d; P[0]=T[0]+dx*s; P[1]=T[1]+dy*s; P[2]=T[2]+dz*s; }
                }
            }
            // Authored plane colliders — DISABLED BY DEFAULT (D4_CLOTH_PLANES=1 re-enables
            // for research). The def stores only a bone-local transform; WHICH axis is the
            // half-space normal is NOT in the data, and both derivations tried so far have
            // failed a piece class (the +Z guess floored dlux100; the rest-pose-fit guess
            // builds an invisible wall behind spiritborn/druid/necro skirts — planeCount=1
            // on those pieces, 0 on barbarian, matching exactly who squishes flat when the
            // cloth swings behind the character). The game also ships *_noPlane_override
            // cloth variants, so planes are situational even when authored. Until the axis
            // convention is decoded against in-game reference, no plane beats a wrong plane.
            static const bool kPlanesOn = qEnvironmentVariableIsSet("D4_CLOTH_PLANES");
            const float planeMargin = 0.005f;
            for (int pi = 0; kPlanesOn && pi < m_planeBone.size(); ++pi) {
                const float* pp = m_planePt.constData()+pi*3; const float* pe = m_planeNm.constData()+pi*3;
                float nx=pe[0]-pp[0], ny=pe[1]-pp[1], nz=pe[2]-pp[2];
                const float nl=std::sqrt(nx*nx+ny*ny+nz*nz); if (nl < 1e-6f) continue; nx/=nl; ny/=nl; nz/=nl;
                for (int k = 0; k < nv; ++k) {
                    if (pinnedAt(k)) continue;
                    float* P = rt.pos.data()+k*3;
                    const float sd=(P[0]-pp[0])*nx + (P[1]-pp[1])*ny + (P[2]-pp[2])*nz;
                    if (sd < planeMargin) { const float push=planeMargin-sd; P[0]+=nx*push; P[1]+=ny*push; P[2]+=nz*push; }
                }
            }
            // Capsule collision LAST at every free cage particle → the particle always ends the
            // iteration outside the body, so the driven skirt clears the leg. Runs INSIDE the
            // solver loop (not once after it), so the constraint network can't pull a resolved
            // particle back into the body on a later iteration. prev is passed so the resolver
            // can use the entry side and cancel the inward velocity.
            for (int k = 0; k < nv; ++k) {
                if (pinnedAt(k)) continue;
                collide(rt.pos.data()+k*3, rt.prev.data()+k*3, /*fixVel=*/false);
            }
        }
        // ONE velocity/friction response per sub-step, after the solve has converged.
        for (int k = 0; k < nv; ++k) {
            if (pinnedAt(k)) continue;
            collide(rt.pos.data()+k*3, rt.prev.data()+k*3, /*fixVel=*/true);
        }
        // SAFETY BOUND. Collision now runs last (so the motion clamp can't undo it) — but that
        // also removed the only thing bounding a particle's distance from its skinned pose. A
        // wrong-side push or a bad normal could then walk a particle away unopposed, which is what
        // flung cage-driven bones out to the emergency radius. Re-clamp here at a GENEROUS multiple
        // of the authored limit: collision still wins locally (1.5× leaves it room), but nothing
        // can diverge. Velocity is rebased with the position so the clamp adds no energy.
        for (int k = 0; k < nv; ++k) {
            if (pinnedAt(k)) continue;
            float* P = rt.pos.data()+k*3; float* Q = rt.prev.data()+k*3;
            const float* T = rt.target.constData()+k*3;
            const float al = (k < cs.attachLen.size()) ? cs.attachLen[k] : 1.0f;
            // With the tether as the real bound, this is a pure divergence NET (never a
            // mechanism): the al×mdScale×1.5 formula referenced the skinned target, which
            // would fight the root-relative tether. al kept for the no-root fallback verts.
            const float lim = (hasRoots && cs.kinRoots[k] < nv)
                                  ? kDivergeMax + kMargin
                                  : qMin(al * mdScale * 1.5f, kDivergeMax) + kMargin;
            const float dx=P[0]-T[0], dy=P[1]-T[1], dz=P[2]-T[2];
            const float d2 = dx*dx+dy*dy+dz*dz;
            if (!std::isfinite(d2)) { P[0]=T[0];P[1]=T[1];P[2]=T[2]; Q[0]=T[0];Q[1]=T[1];Q[2]=T[2]; continue; }
            if (d2 > lim*lim && d2 > 1e-12f) {
                const float s = lim / std::sqrt(d2);
                const float vx=P[0]-Q[0], vy=P[1]-Q[1], vz=P[2]-Q[2];
                P[0]=T[0]+dx*s; P[1]=T[1]+dy*s; P[2]=T[2]+dz*s;
                Q[0]=P[0]-vx;   Q[1]=P[1]-vy;   Q[2]=P[2]-vz;   // keep velocity, move the frame
            }
        }
        }   // sub-step
    }
    m_colP0 = colCur0; m_colP1 = colCur1;   // bone path + overlay use the end-of-frame pose
    // Drive anchored bones from their cage particle: bone head = animated head + the
    // particle's sim offset from its own skinned target. The bone then swings the skinned
    // mesh through the normal pass-2/palette path — no new mesh machinery.
    for (int j : m_sbOrder) {
        if (j >= m_sbDriven.size() || !m_sbDriven[j]) continue;
        const int ci = m_sbAnchorPiece[j], kv = m_sbAnchorVert[j];
        if (ci < 0 || ci >= m_cages.size() || kv < 0) continue;
        const CageRt& rt = m_cages[ci];
        if (!rt.seeded || (kv+1)*3 > rt.pos.size()) continue;
        float* S=m_sbSimHead.data()+j*3; float* R=m_sbPrevHead.data()+j*3;
        R[0]=S[0];R[1]=S[1];R[2]=S[2];
        // AUTHORED FOLLOWERS: the bone head IS its particle (rest alignment ≤5 µm), so the
        // bone takes the particle's position directly. The old form — anim + (pos − target)
        // × driveW — ADDED any target error to a correct bone: with a cross-garment target
        // that arithmetic reproduced the jutting cape bones to 3 decimals
        // (tools/d4cloth/ROOTCAUSE.md link 4). driveW stays 1.0 on followed bones.
        // Fallback-anchored bones (no follower data) keep the delta form, tapered by
        // m_sbAnchorW, since their anchor particle is NOT at the bone.
        const ClothSim& fcs = m_clothSims[rt.simIdx];
        const bool exact = kv < fcs.followerBone.size() && fcs.followerBone[kv] == j;
        if (exact) {
            // AUTHORED BLEND (ptBlendWeights): the bone takes sim blended toward the
            // skinned target — the game's anti-clip: most of the garment stays near the
            // authored (clip-free) animated drape, sim strongest where blendW→1. Output-
            // only (sim state untouched), so it adds no energy and can't fight the solve.
            const float bw = (kv < fcs.blendW.size()) ? qBound(0.0f, fcs.blendW[kv], 1.0f) : 1.0f;
            S[0]=rt.target[kv*3]   + (rt.pos[kv*3]   - rt.target[kv*3])   * bw;
            S[1]=rt.target[kv*3+1] + (rt.pos[kv*3+1] - rt.target[kv*3+1]) * bw;
            S[2]=rt.target[kv*3+2] + (rt.pos[kv*3+2] - rt.target[kv*3+2]) * bw;
        } else {
            const float aw = (j < m_sbAnchorW.size() && m_sbAnchorW[j] > 0.0f) ? m_sbAnchorW[j] : 1.0f;
            S[0]=animG[j][12] + (rt.pos[kv*3]   - rt.target[kv*3])   * aw;
            S[1]=animG[j][13] + (rt.pos[kv*3+1] - rt.target[kv*3+1]) * aw;
            S[2]=animG[j][14] + (rt.pos[kv*3+2] - rt.target[kv*3+2]) * aw;
        }
    }
    // Integrate: PINNED bones (authored invMass 0) follow the animated/skinned pose exactly;
    // free bones Verlet under gravity + inertia + a weak return-to-pose spring.
    for (int j : m_sbOrder) {
        float* S=m_sbSimHead.data()+j*3; float* R=m_sbPrevHead.data()+j*3;
        // Cage-driven bones were already positioned from the simulated cage above — skip.
        if (j < m_sbDriven.size() && m_sbDriven[j]) continue;
        // A bone the current clip GENUINELY animates (real motion — e.g. misclassified body bones)
        // must FOLLOW the animation, not be sprung. A static/rest track does NOT count (the game
        // hands those to the cloth sim), so it stays simulated → the skirt chains collide instead
        // of rigidly riding the leg.
        const bool animated = m_hasAnim && j < m_sbAnimMoves.size() && m_sbAnimMoves[j];
        if (m_sbPin[j] || animated) { R[0]=S[0];R[1]=S[1];R[2]=S[2]; S[0]=animG[j][12]; S[1]=animG[j][13]; S[2]=animG[j][14]; continue; }
        const float ah0=animG[j][12], ah1=animG[j][13], ah2=animG[j][14];
        // Per-piece authored tuning: this bone's cloth piece scales gravity and adds its self-wind.
        const int si = (j < m_sbSim.size()) ? m_sbSim[j] : -1;
        const bool tuned = (si >= 0 && si < m_clothSims.size() && m_clothSims[si].tuned);
        // Same slider-unit fix as the cage path: non-zero slider = multiple of the
        // authored magnitude (|0.025| ≡ 1.0×); zero keeps this path's no-gravity idle.
        const float gj = (grav != 0.0f)
            ? -(tuned ? m_clothSims[si].gravScale : 1.0f) * (20.0f / 3600.0f) * (grav / -0.025f)
            : 0.0f;
        float wx=0,wy=0,wz=0;
        if (si>=0 && si<m_clothSims.size()) { wx=m_clothSims[si].windX; wy=m_clothSims[si].windY; wz=m_clothSims[si].windZ; }
        // Hair-class physics for UNTUNED hair bones (no authored cage): track the head tightly and
        // damp hard so a ponytail/braid follows with subtle secondary motion instead of inheriting
        // the armour's floppy cloth tuning and self-oscillating. Authored cages keep their own
        // params. All three overrides only REDUCE motion (stiffer return, more damping, less
        // gravity) → they can never add energy, so worst case is slightly-too-stiff hair, never wiggle.
        // CAGE-LESS cloth bones (si < 0): no cage claimed them, so NO authored sim data
        // exists for them in this outfit — measured case: spiF_base01_TRS's 45 fur-panel
        // bones (its rig authors nCloth=45 but the piece ships NO ClothData/snoCloth; the
        // cage that drives those bones lives on spiF_base01_LEG, which an equipped legs
        // item replaces). The game's only authored truth left for them is the SKINNED
        // pose — so track it tightly with subtle lag (hair treatment) instead of loose
        // cloth swing, which left the pelt hanging flatly through the striding leg.
        // `|| si < 0` (cage-less ⇒ hair treatment) is what made 37% of a garment's cloth bones
        // rigid: snap-back, heavy damping and 0.35x gravity, on top of the tight leash below. It
        // was a workaround for the synthetic 1.0 tether; those bones now carry a geometry-derived
        // tether (see buildSpringBones), so the workaround is off by default. D4_CLOTH_LEGACY_ORPHANS=1
        // restores it. GENUINE hair (m_sbHair) is unaffected either way.
        // A CHAIN link is a pendulum, not fabric: it holds a rigid distance to its parent and is
        // otherwise free. The cage-less path's job is to hug the skinned pose, which for a flail
        // meant the links neither held their spacing (the first one floated off the handle) nor
        // swung (they were sprung back to one fixed orientation). Both are what was reported.
        // Return-to-pose is dropped entirely and the link length is enforced below, so the only
        // forces left are gravity, inertia and damping — which is what a chain has.
        const bool chainBone = (j < m_sbChain.size() && m_sbChain[j]);
        const bool hairBone = !chainBone
                              && ((j < m_sbHair.size() && m_sbHair[j] && !tuned) || (si < 0 && m_orphanLegacy));
        const float stiffJ = chainBone ? 0.0f : (hairBone ? qMax(stiff, 0.55f) : stiff);
        const float keepJ  = hairBone ? qMin(keep, 0.45f)  : keep;    // heavier damping → settles fast
        const float gjJ    = hairBone ? gj * 0.35f          : gj;      // hair droops far less than cloth
        // NOTE: return-to-pose stays at the global stiffness for CLOTH — do NOT boost it for high-
        // tracking pieces. Jewelry (low authored gravity) swings from inertial LAG when the head
        // moves; a stronger snap-back kills that lag and leaves it rigidly upright. Separation is
        // instead bounded by the per-piece max-distance clamp below.
        float sax = 0.0f, saz = 0.0f;
        spinAccel(S, sax, saz);   // user rotation → lag + centrifugal (same terms as the cage path)
        float n0=S[0]+(S[0]-R[0])*keepJ + wx + sax, n1=S[1]+(S[1]-R[1])*keepJ + gjJ + wy, n2=S[2]+(S[2]-R[2])*keepJ + wz + saz;
        n0+=(ah0-n0)*stiffJ; n1+=(ah1-n1)*stiffJ; n2+=(ah2-n2)*stiffJ;
        clampStep(S, n0, n1, n2);   // per-step travel clamp (anti-tunnelling)
        R[0]=S[0];R[1]=S[1];R[2]=S[2]; S[0]=n0;S[1]=n1;S[2]=n2;
        // Rigid link length. m_sbLenParent has always been computed from the rest pose and never
        // used; for a chain it is the constraint that matters. m_sbOrder is hierarchical, so the
        // parent has already moved this step — one Gauss-Seidel pass from the root outward is
        // enough to hold the whole chain together, and it anchors the first link to the grip
        // because that parent is the attachment root, which is animation-driven rather than
        // simulated.
        if (chainBone && j < m_skeleton.size()) {
            const int p = m_skeleton[j].parent;
            const float len = (j < m_sbLenParent.size()) ? m_sbLenParent[j] : 0.0f;
            if (p >= 0 && p < nb && len > 1e-5f) {
                const bool pSim = (p < m_sbChain.size() && m_sbChain[p]);
                const float px = pSim ? m_sbSimHead[p*3+0] : animG[p][12];
                const float py = pSim ? m_sbSimHead[p*3+1] : animG[p][13];
                const float pz = pSim ? m_sbSimHead[p*3+2] : animG[p][14];
                float dx = S[0]-px, dy = S[1]-py, dz = S[2]-pz;
                const float d = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (d > 1e-6f) {
                    const float k = len / d;
                    S[0] = px + dx*k; S[1] = py + dy*k; S[2] = pz + dz*k;
                }
            }
        }
        sweep(S, R);                // stop at the entry point on fast motion
    }
    // Solve the AUTHORED constraint network (the 2-D mesh that holds the cloth as a sheet) +
    // capsule collision, iterated. Pinned bones are infinite mass (only the free end moves).
    // Cage-driven bones are immovable here too — the cage already solved them at full density.
    auto held = [&](int j) { return m_sbPin[j] || (j < m_sbDriven.size() && m_sbDriven[j]); };
    for (int it = 0; it < kIters; ++it) {
        for (int e = 0; e < m_sbConA.size(); ++e) {
            const int ia=m_sbConA[e], ib=m_sbConB[e];
            float* A=m_sbSimHead.data()+ia*3; float* B=m_sbSimHead.data()+ib*3;
            float dx=B[0]-A[0], dy=B[1]-A[1], dz=B[2]-A[2]; const float len=std::sqrt(dx*dx+dy*dy+dz*dz);
            if (len < 1e-6f) continue;
            const float wa=held(ia)?0.f:1.f, wb=held(ib)?0.f:1.f, ws=wa+wb; if (ws<1e-6f) continue;
            const float diff=(len-m_sbConRest[e])/len, sa=diff*wa/ws, sb=diff*wb/ws;
            A[0]+=dx*sa;A[1]+=dy*sa;A[2]+=dz*sa; B[0]-=dx*sb;B[1]-=dy*sb;B[2]-=dz*sb;
        }
        // AUTHORED per-bone motion constraint (ptAttachmentLengths): clamp each free bone to
        // within attachLen × range of its skinned pose. 0 ⇒ stays on the body (the cape's top
        // edge), 1 ⇒ free to swing (the hem). This is the per-bone physics strength the game
        // uses, and it bounds the droop so the cloth can't sag arbitrarily far.
        for (int j : m_sbOrder) if (!held(j)) {
            // Per-piece max distance: authored per-bone attachLen × the global slider, then scaled
            // by the piece's own bone-tracking. Normalised so a draping piece (~0.45 tracking)
            // keeps the slider value while high-tracking chains/jewelry clamp much tighter — so a
            // single slider suits both a swinging skirt and a chain that must stay put.
            const int si2 = (j < m_sbSim.size()) ? m_sbSim[j] : -1;
            const bool tuned2 = (si2 >= 0 && si2 < m_clothSims.size() && m_clothSims[si2].tuned);
            float trk = tuned2 ? qBound(0.0f, m_clothSims[si2].boneTrack, 1.0f)
                               : qBound(0.0f, m_cloth.boneTracking, 1.0f);
            // Untuned hair bones track the head tightly (high tracking → tight clamp), so the
            // ponytail stays put rather than swinging on the armour's loose cloth tracking.
            if (j < m_sbHair.size() && m_sbHair[j] && !tuned2) trk = qMax(trk, 0.85f);
            // Squared falloff: a draping piece (~0.45 tracking) keeps the full slider value, but
            // high-tracking rigid pieces (chains, jewelry ~0.83) drop to ~0.1× so the links stay
            // bunched and don't separate even at a global max-distance of 1.0.
            // UNITS, corrected (tools/d4cloth/FINDINGS.md F1): m_sbAttach is an authored
            // ABSOLUTE tether length — apply the slider only (1.0 = game). The old tnorm²
            // tracking scale treated it as a normalized fraction. Hair keeps a tight clamp.
            const float hairTight = ((j < m_sbHair.size() && m_sbHair[j] && !tuned2)
                                     || (si2 < 0 && m_orphanLegacy)) ? 0.10f : 1.0f;
            // CAGE-LESS cloth bones (si2 < 0 — no cage claimed them, so m_sbAttach is the
            // synthetic default 1.0, NOT an authored tether): the absolute-units clamp gave
            // them a ~1 wu leash, so the fur/tassel chains of pieces that ship no cage for
            // them dangled limply through the walking legs (the long straight overlay lines
            // on spiF_stor214). Restore the pre-port TIGHT pose-tracking clamp for exactly
            // these bones: tnorm² of the tracking slider ≈ 0.15 wu — they ride the animated
            // pose with subtle secondary swing, which is also what they did before the port.
            // Bones WITH an authored tether (cage-matched) keep the absolute F1 semantics.
            const float tnorm2 = qBound(0.0f, (1.0f - trk) / 0.55f, 1.0f);
            // Cage-less bones now carry a real (geometry-derived) tether, so this extra squeeze
            // is legacy-only; with it applied on top, a hem bone was leashed to ~7-100 mm.
            const float noCage = (si2 < 0 && m_orphanLegacy) ? qMax(0.05f, tnorm2 * tnorm2) : 1.0f;
            const float md = m_sbAttach[j] * qBound(0.05f, m_cloth.maxDistance, 2.0f) * hairTight * noCage;
            float* S=m_sbSimHead.data()+j*3;
            const float dx=S[0]-animG[j][12], dy=S[1]-animG[j][13], dz=S[2]-animG[j][14];
            const float d=std::sqrt(dx*dx+dy*dy+dz*dz);
            if (d > md) { const float s=md/d; S[0]=animG[j][12]+dx*s; S[1]=animG[j][13]+dy*s; S[2]=animG[j][14]+dz*s; }
        }
        // AUTHORED plane colliders: keep free bones on the front (+normal) side of each plane.
        // Tiny margin (5mm) so the cloth can rest right at the plane (e.g. on the ground) rather
        // than being lifted off it — only an actual crossing gets pushed back.
        static const bool kPlanesOn2 = qEnvironmentVariableIsSet("D4_CLOTH_PLANES");
        const float planeMargin = 0.005f;   // (planes default-off — see the cage path)
        for (int pi = 0; kPlanesOn2 && pi < m_planeBone.size(); ++pi) {
            const float* pp = m_planePt.constData()+pi*3; const float* pe = m_planeNm.constData()+pi*3;
            float nx=pe[0]-pp[0], ny=pe[1]-pp[1], nz=pe[2]-pp[2];
            const float nl=std::sqrt(nx*nx+ny*ny+nz*nz); if (nl < 1e-6f) continue; nx/=nl; ny/=nl; nz/=nl;
            for (int j : m_sbOrder) if (!held(j)) {
                float* S=m_sbSimHead.data()+j*3;
                const float sd=(S[0]-pp[0])*nx + (S[1]-pp[1])*ny + (S[2]-pp[2])*nz;
                if (sd < planeMargin) { const float push=planeMargin-sd; S[0]+=nx*push; S[1]+=ny*push; S[2]+=nz*push; }
            }
        }
        // Body collision LAST — matching the cage path and NvCloth's own ordering. It used to run
        // BEFORE the motion-limit clamp and the plane colliders, so every iteration finished by
        // dragging the bone back toward its skinned pose… which sits INSIDE the lifted leg. That
        // is the heavy leg-through-skirt clipping: collision was being solved, then immediately
        // undone. Now nothing moves a bone after it has been pushed out of the body.
        for (int j : m_sbOrder) if (!held(j)) {
            // Positional push only while iterating — prev is still read (entry-side normal), but
            // NOT rewritten; the velocity response happens once after the loop.
            collide(m_sbSimHead.data()+j*3, m_sbPrevHead.data()+j*3, /*fixVel=*/false);
        }
    }
    // ONE velocity/friction response after the solve, plus the overlay's contact state.
    for (int j : m_sbOrder) if (!held(j)) {
        const float before  = worstPen;
        const int   nBefore = contactCount;
        collide(m_sbSimHead.data()+j*3, m_sbPrevHead.data()+j*3, /*fixVel=*/true);
        if (j < m_sbContact.size()) {
            if (contactCount == nBefore)                 m_sbContact[j] = 0;   // free
            else if (worstPen - before > kMargin * 4.0f) m_sbContact[j] = 2;   // deep
            else                                          m_sbContact[j] = 1;   // touching
        }
    }
    // SAFETY BOUND (see the cage path): collision runs last now, so re-bound each bone at a
    // generous 1.5× its authored motion limit. Collision keeps room to win; divergence can't.
    for (int j : m_sbOrder) if (!held(j)) {
        const int si2 = (j < m_sbSim.size()) ? m_sbSim[j] : -1;
        const bool tuned2 = (si2 >= 0 && si2 < m_clothSims.size() && m_clothSims[si2].tuned);
        float trk = tuned2 ? qBound(0.0f, m_clothSims[si2].boneTrack, 1.0f)
                           : qBound(0.0f, m_cloth.boneTracking, 1.0f);
        if (j < m_sbHair.size() && m_sbHair[j] && !tuned2) trk = qMax(trk, 0.85f);
        // UNITS, corrected (FINDINGS F1): attachLen is absolute; the bound is 1.5x the
        // authored tether, still capped by kDivergeMax so it means something on every rig.
        // Cage-less bones use the same tight tracking-scaled leash as the clamp above.
        const float tn2 = qBound(0.0f, (1.0f - trk) / 0.55f, 1.0f);
        const float noCage2 = (si2 < 0) ? qMax(0.05f, tn2 * tn2) : 1.0f;
        const float lim = qMin(m_sbAttach[j] * qBound(0.05f, m_cloth.maxDistance, 2.0f) * noCage2 * 1.5f,
                               kDivergeMax) + kMargin;   // attachLen is absolute (FINDINGS F1)
        float* S=m_sbSimHead.data()+j*3; float* R=m_sbPrevHead.data()+j*3;
        const float ax0=animG[j][12], ay0=animG[j][13], az0=animG[j][14];
        const float dx=S[0]-ax0, dy=S[1]-ay0, dz=S[2]-az0;
        const float d2=dx*dx+dy*dy+dz*dz;
        if (!std::isfinite(d2)) { S[0]=ax0;S[1]=ay0;S[2]=az0; R[0]=ax0;R[1]=ay0;R[2]=az0; continue; }
        if (d2 > lim*lim && d2 > 1e-12f) {
            const float s = lim/std::sqrt(d2);
            const float vx=S[0]-R[0], vy=S[1]-R[1], vz=S[2]-R[2];
            S[0]=ax0+dx*s; S[1]=ay0+dy*s; S[2]=az0+dz*s;
            R[0]=S[0]-vx;  R[1]=S[1]-vy;  R[2]=S[2]-vz;
        }
    }
    // Hard safety clamp: on some rigs (seen on Warlock male) the cloth spring can diverge and
    // fling a bone to infinity/NaN, splaying the skirt/cape across the screen. Keep every cloth
    // bone within a bounded distance of its skinned pose, and snap non-finite positions back.
    {
        const float hardMax = kDivergeMax;
        int clamped = 0; float worstDiv = 0.0f; int worstBone = -1;
        for (int j : m_sbOrder) {
            if (m_sbPin[j]) continue;
            float* S = m_sbSimHead.data() + j*3;
            float* R = m_sbPrevHead.data() + j*3;
            const float ax = animG[j][12], ay = animG[j][13], az = animG[j][14];
            const float dx = S[0]-ax, dy = S[1]-ay, dz = S[2]-az;
            const float d2 = dx*dx + dy*dy + dz*dz;
            if (!std::isfinite(d2)) { S[0]=ax; S[1]=ay; S[2]=az; R[0]=ax; R[1]=ay; R[2]=az; continue; }
            const float d = std::sqrt(d2);
            if (d > worstDiv) { worstDiv = d; worstBone = j; }
            if (d2 > hardMax*hardMax) {
                const float s = hardMax/d;
                // Rebase the previous head with the current one. Without this the bone keeps its
                // outward velocity, re-violates the clamp every frame and sits PINNED AT THE CLAMP
                // RADIUS forever — which is precisely the fan of equal-length spokes seen on capes.
                const float vx=S[0]-R[0], vy=S[1]-R[1], vz=S[2]-R[2];
                S[0]=ax+dx*s; S[1]=ay+dy*s; S[2]=az+dz*s;
                R[0]=S[0]-vx*0.25f; R[1]=S[1]-vy*0.25f; R[2]=S[2]-vz*0.25f;   // bleed off the runaway
                ++clamped;
            }
        }
        if (qEnvironmentVariableIsSet("D4_DUMP_CLOTH")) {
            // ORPHAN DUMP: the worst cage-less cloth bones vs their animated pose. If a
            // trim/fur panel still sits wrongly with these bones NEAR their anim pose
            // (small d), the ANIMATED POSE ITSELF is the wrong input (anim decode /
            // hierarchy fallback) — not the sim. That is the discriminating readout.
            static qint64 lastOrph = 0;
            const qint64 nowO = m_clothClock.isValid() ? m_clothClock.elapsed() : 0;
            if (nowO - lastOrph > 2000) {
                lastOrph = nowO;
                QVector<QPair<float,int>> worst;
                for (int j : m_sbOrder) {
                    if (j < m_sbSim.size() && m_sbSim[j] >= 0) continue;   // cage-matched
                    const float* S = m_sbSimHead.constData() + j*3;
                    const float dx=S[0]-animG[j][12], dy=S[1]-animG[j][13], dz=S[2]-animG[j][14];
                    worst.push_back({dx*dx+dy*dy+dz*dz, j});
                }
                std::sort(worst.begin(), worst.end(), [](auto&a, auto&b){ return a.first > b.first; });
                for (int i = 0; i < worst.size() && i < 6; ++i) {
                    const int j = worst[i].second;
                    qInfo("cloth-orphan: bone %d '%s' d=%.3f anim(%.2f %.2f %.2f) sim(%.2f %.2f %.2f) moves=%d pin=%d",
                          j, qPrintable(m_skeleton[j].name), std::sqrt(worst[i].first),
                          animG[j][12], animG[j][13], animG[j][14],
                          m_sbSimHead[j*3], m_sbSimHead[j*3+1], m_sbSimHead[j*3+2],
                          (j < m_sbAnimMoves.size()) ? int(m_sbAnimMoves[j]) : -1,
                          (j < m_sbPin.size()) ? int(m_sbPin[j]) : -1);
                }
            }
        }
        if (clamped > 0 && qEnvironmentVariableIsSet("D4_DUMP_CLOTH")) {
            static qint64 lastDiv = 0;
            const qint64 now = m_clothClock.isValid() ? m_clothClock.elapsed() : 0;
            if (now - lastDiv > 1000) {
                lastDiv = now;
                const bool drv = (worstBone >= 0 && worstBone < m_sbDriven.size()) ? (bool)m_sbDriven[worstBone] : false;
                qInfo("cloth-diverge: %d bone(s) hit the %.3f cap | worst %.3f on bone %d (%s)",
                      clamped, hardMax, worstDiv, worstBone, drv ? "cage-driven" : "spring");
            }
        }
    }
    // Collision health (opt-in diagnostic): worst penetration + contact count for this step. If
    // worstPen hovers near the margin the capsules fit; a large value means "Capsule size" is
    // wrong for this piece (or the authored capsules don't match the mesh) — far more actionable
    // than eyeballing the model. Throttled so it can't spam at 30 fps.
    if (contactCount > 0 && qEnvironmentVariableIsSet("D4_DUMP_CLOTH")) {
        static qint64 lastLog = 0;
        const qint64 now = m_clothClock.isValid() ? m_clothClock.elapsed() : 0;
        if (now - lastLog > 1000) {
            lastLog = now;
            qInfo("cloth-collide: %d contact(s), worst penetration %.4f (margin %.4f, capsule x%.2f, substeps %d)",
                  contactCount, double(worstPen), double(kMargin), double(rScale), subSteps);
        }
    }
    // Pass 2: rebuild bone globals from the simulated heads (swing the bone to point at its
    // simulated child; leaves inherit the parent's swing). Root→tip so parent swing is ready.
    QVector<std::array<float,9>> swing(nb);
    for (int j : m_sbOrder) {
        // Genuinely-animated bones keep their exact animated global — never swing them. (A static
        // rest track doesn't count; those are simulated, matching the integration guard above.)
        if (m_hasAnim && j < m_sbAnimMoves.size() && m_sbAnimMoves[j]) { global[j] = animG[j]; continue; }
        const int c = m_sbChild[j]; float r[9];
        if (c >= 0) {
            const float aD[3]={animG[c][12]-animG[j][12], animG[c][13]-animG[j][13], animG[c][14]-animG[j][14]};
            const float sD[3]={m_sbSimHead[c*3]-m_sbSimHead[j*3], m_sbSimHead[c*3+1]-m_sbSimHead[j*3+1], m_sbSimHead[c*3+2]-m_sbSimHead[j*3+2]};
            rotFromTo(aD, sD, r);
        } else {
            // Leaf bone (single-bone jewelry / chain tip): roll it to follow how it swung relative
            // to its parent, so an earring/pendant rotates naturally instead of staying upright.
            const int p = m_skeleton[j].parent;
            bool rolled = false;
            if (p >= 0 && p < nb) {
                const bool clothP = (p < m_sbIsCloth.size() && m_sbIsCloth[p] && (p + 1) * 3 <= m_sbSimHead.size());
                const float ppx = clothP ? m_sbSimHead[p*3]   : animG[p][12];
                const float ppy = clothP ? m_sbSimHead[p*3+1] : animG[p][13];
                const float ppz = clothP ? m_sbSimHead[p*3+2] : animG[p][14];
                const float aD[3]={animG[j][12]-animG[p][12], animG[j][13]-animG[p][13], animG[j][14]-animG[p][14]};
                const float sD[3]={m_sbSimHead[j*3]-ppx, m_sbSimHead[j*3+1]-ppy, m_sbSimHead[j*3+2]-ppz};
                const float al=aD[0]*aD[0]+aD[1]*aD[1]+aD[2]*aD[2], sl=sD[0]*sD[0]+sD[1]*sD[1]+sD[2]*sD[2];
                if (al > 1e-8f && sl > 1e-8f) { rotFromTo(aD, sD, r); rolled = true; }
            }
            if (!rolled) {
                if (p>=0 && p<nb && m_sbIsCloth[p]) { const auto& pr=swing[p]; for (int k=0;k<9;++k) r[k]=pr[k]; }
                else { r[0]=1;r[1]=0;r[2]=0;r[3]=0;r[4]=1;r[5]=0;r[6]=0;r[7]=0;r[8]=1; }
            }
        }
        for (int k=0;k<9;++k) swing[j][k]=r[k];
        const Mat4& a = animG[j]; Mat4 m;
        auto col = [&](int ci, int outBase) {
            const float v0=a[ci*4+0], v1=a[ci*4+1], v2=a[ci*4+2];
            m[outBase+0]=r[0]*v0+r[3]*v1+r[6]*v2;
            m[outBase+1]=r[1]*v0+r[4]*v1+r[7]*v2;
            m[outBase+2]=r[2]*v0+r[5]*v1+r[8]*v2; m[outBase+3]=0.0f;
        };
        col(0,0); col(1,4); col(2,8);
        m[12]=m_sbSimHead[j*3]; m[13]=m_sbSimHead[j*3+1]; m[14]=m_sbSimHead[j*3+2]; m[15]=1.0f;
        global[j]=m;
    }
}

// True when the cloth sim should run even with NO animation loaded: a rigged model with cloth and
// the sim enabled still needs stepping so user-driven forces ("React to rotation") and gravity can
// move the garment at the REST pose. Everything downstream already falls back to the rest pose when
// a bone has no anim track, so this simply stops the early-out from skipping the whole sim.

// Export capture: one deterministic cloth step per captured frame (see the header). Stops
// the idle settle timer (the capture loop pumps events, so it would slip in extra wall-
// clock steps) and re-seeds the spin tracker so programmatic yaw changes (turntable GIFs)
// are not read as user rotation. Restored symmetrically when capture ends.
void GLModelWidget::setCaptureMode(bool on)
{
    if (m_capturing == on) return;
    m_capturing = on;
    if (on) {
        if (m_clothTimer) m_clothTimer->stop();
        m_spinOmega = 0.0f; m_spinPrevYaw = m_yaw; m_spinSeeded = false;
    } else {
        m_spinPrevYaw = m_yaw; m_spinSeeded = false;
        if (m_clothTimer) m_clothTimer->start();
        else ensureClothTimer();
    }
}

// Run N extra deterministic cloth steps at the CURRENT pose — the settle steps a live
// preview gets from the idle timer but an export capture doesn't (see the header).
void GLModelWidget::settleCloth(int steps)
{
    if (!m_clothEnabled || m_skeleton.isEmpty() || m_bindVerts.isEmpty()) return;
    for (int i = 0; i < qBound(0, steps, 600); ++i)
        applySkinning();   // each call advances the sim exactly one step at this pose
}

bool GLModelWidget::simAtRestActive() const
{
    if (!m_clothEnabled || m_skeleton.isEmpty()) return false;
    // The cloth topology is BUILT INSIDE applySkinning, so before that first pass we can't know
    // whether this model has cloth — answer yes so the builders get their chance (otherwise the
    // guard and the builder would wait on each other and a static model would never simulate).
    if (!m_clothBuilt || !m_sbBuilt) return true;
    return !m_sbOrder.isEmpty() || !m_cages.isEmpty();
}

void GLModelWidget::applySkinning()
{
    if ((!m_hasAnim && !simAtRestActive()) || m_skeleton.isEmpty() || m_bindVerts.isEmpty()) {
        m_verts = m_bindVerts;
        return;
    }
    const int nb = m_skeleton.size();
    const int f = m_frame;

    // Per-joint animated local matrix (anim track if present, else rest pose).
    QVector<Mat4> local(nb);
    for (int j = 0; j < nb; ++j) {
        const ModelJoint& jt = m_skeleton[j];
        const int ai = m_animByHash.value(jt.nameHash, -1);
        // Guard the anim index against the CURRENT clip (a restored frame/clip can leave
        // ai or f out of range for this animation), and clamp the frame per-track since
        // static channels legitimately carry fewer keyframes than the rotation track.
        if (ai >= 0 && ai < m_anim.bones.size()) {
            const auto& ba = m_anim.bones[ai];
            // Attached tracks run on their own clock (see m_trackClock).
            const int fa = animFrameFor(ai);
            if (!ba.rotations.isEmpty() && !ba.translations.isEmpty() && !ba.scales.isEmpty()) {
                const int rf = qBound(0, fa, int(ba.rotations.size())    - 1);
                const int tf = qBound(0, fa, int(ba.translations.size()) - 1);
                const int sf = qBound(0, fa, int(ba.scales.size())       - 1);
                local[j] = composeTRS(ba.rotations[rf].data(), ba.translations[tf].data(), ba.scales[sf].data());
                continue;
            }
        }
        local[j] = composeTRS(jt.restQ.data(), jt.restT.data(), jt.restS.data());
    }
    // Hierarchy → global (parents precede children in D4 skeletons).
    // World transforms. NOTE: this must NOT assume parents precede children. The old
    // `(p >= 0 && p < j)` test treated any bone whose parent sat later in the array as a ROOT,
    // collapsing its world matrix to its local one — the bone jumped to the model origin and every
    // child drew a connection line back to it. Resolve by readiness instead: the common case still
    // completes in the first linear pass, stragglers in one or two more.
    QVector<Mat4> global(nb);
    QVector<quint8> done(nb, 0);
    int remaining = 0;
    for (int j = 0; j < nb; ++j) {
        const int p = m_skeleton[j].parent;
        if (p < 0 || p >= nb)        { global[j] = local[j];                  done[j] = 1; }
        else if (p < j)              { global[j] = mat4mul(global[p], local[j]); done[j] = 1; }
        else                         { ++remaining; }
    }
    while (remaining > 0) {
        int progressed = 0;
        for (int j = 0; j < nb; ++j) {
            if (done[j]) continue;
            const int p = m_skeleton[j].parent;
            if (p >= 0 && p < nb && done[p]) {
                global[j] = mat4mul(global[p], local[j]);
                done[j] = 1; --remaining; ++progressed;
            }
        }
        if (!progressed) {   // parent cycle (corrupt rig): break it rather than spin forever
            for (int j = 0; j < nb; ++j) if (!done[j]) { global[j] = local[j]; done[j] = 1; }
            remaining = 0;
        }
    }
    // ── Cloth physics: spring-bone simulation of the cloth-bone chains. The cape/skirt/
    // chains are skinned to these bones, so simulating the bones (then skinning normally)
    // makes them swing. Runs BEFORE the palette so the skinned mesh follows. ──
    if (m_clothEnabled) {
        if (!m_clothBuilt) buildClothSim();   // fits the body-collision capsules
        if (!m_sbBuilt) buildSpringBones();   // identifies the cloth-bone chains
        // Pose the collision capsules from the animated body bones (cloth bones don't move
        // them) so the spring bones can collide against the posed body.
        auto capXf = [&](int b, const float* in, float* o) {
            if (b < 0 || b >= nb) { o[0]=in[0]; o[1]=in[1]; o[2]=in[2]; return; }
            const Mat4 m = mat4mul(global[b], m_skeleton[b].inverseBind);
            o[0]=m[0]*in[0]+m[4]*in[1]+m[8]*in[2]+m[12];
            o[1]=m[1]*in[0]+m[5]*in[1]+m[9]*in[2]+m[13];
            o[2]=m[2]*in[0]+m[6]*in[1]+m[10]*in[2]+m[14];
        };
        const int nCap = m_colR0.size();
        if (m_colBoneA.size()>=nCap && m_colBoneB.size()>=nCap && m_colP0Bind.size()>=nCap*3
            && m_colP1Bind.size()>=nCap*3 && m_colP0.size()>=nCap*3 && m_colP1.size()>=nCap*3) {
            // Keep LAST frame's capsule pose: springBoneStep interpolates colliders from it
            // across sub-steps (continuous collision — see m_colP0Prev in the header).
            m_colPrevValid = m_colPrevValid || false;   // (validity decided below)
            const bool hadPose = (m_colP0Prev.size() == m_colP0.size());
            m_colP0Prev = m_colP0; m_colP1Prev = m_colP1;
            m_colPrevValid = hadPose;
            for (int i=0;i<nCap;++i){ capXf(m_colBoneA[i], m_colP0Bind.constData()+i*3, m_colP0.data()+i*3);
                                      capXf(m_colBoneB[i], m_colP1Bind.constData()+i*3, m_colP1.data()+i*3); }
        }
        for (int i=0;i<m_planeBone.size();++i){   // pose the authored plane colliders too
            capXf(m_planeBone[i], m_planePtBind.constData()+i*3, m_planePt.data()+i*3);
            capXf(m_planeBone[i], m_planeNmBind.constData()+i*3, m_planeNm.data()+i*3);
        }
        springBoneStep(global);   // modifies global[] for cloth bones
    }
    m_boneGlobalSim = global;   // post-sim world matrices (for the phys-bone axis overlay)
    // Skinning palette = global × inverseBind.
    QVector<Mat4> palette(nb);
    for (int j = 0; j < nb; ++j)
        palette[j] = mat4mul(global[j], m_skeleton[j].inverseBind);

    m_verts = m_bindVerts;
    const int vcount = m_vJoints.size();
    for (int v = 0; v < vcount; ++v) {
        const float* bp = m_bindVerts.constData() + v * 11;
        const auto& J = m_vJoints[v];
        const auto& W = m_vWeights[v];
        const float wsum = W[0] + W[1] + W[2] + W[3];
        if (wsum <= 0.0f) continue;   // unskinned vertex keeps bind pose
        float px = 0, py = 0, pz = 0, nx = 0, ny = 0, nz = 0, tx = 0, ty = 0, tz = 0;
        for (int k = 0; k < 4; ++k) {
            const float w = W[k];
            if (w == 0.0f) continue;
            const int b = J[k];
            if (b < 0 || b >= nb) continue;
            const Mat4& m = palette[b];
            px += w * (m[0]*bp[0] + m[4]*bp[1] + m[8]*bp[2]  + m[12]);
            py += w * (m[1]*bp[0] + m[5]*bp[1] + m[9]*bp[2]  + m[13]);
            pz += w * (m[2]*bp[0] + m[6]*bp[1] + m[10]*bp[2] + m[14]);
            nx += w * (m[0]*bp[3] + m[4]*bp[4] + m[8]*bp[5]);
            ny += w * (m[1]*bp[3] + m[5]*bp[4] + m[9]*bp[5]);
            nz += w * (m[2]*bp[3] + m[6]*bp[4] + m[10]*bp[5]);
            tx += w * (m[0]*bp[8] + m[4]*bp[9] + m[8]*bp[10]);
            ty += w * (m[1]*bp[8] + m[5]*bp[9] + m[9]*bp[10]);
            tz += w * (m[2]*bp[8] + m[6]*bp[9] + m[10]*bp[10]);
        }
        float* out = m_verts.data() + v * 11;
        out[0] = px; out[1] = py; out[2] = pz;
        const float nl = std::sqrt(nx*nx + ny*ny + nz*nz);
        if (nl > 1e-9f) { out[3] = nx/nl; out[4] = ny/nl; out[5] = nz/nl; }
        const float tl = std::sqrt(tx*tx + ty*ty + tz*tz);
        if (tl > 1e-9f) { out[8] = tx/tl; out[9] = ty/tl; out[10] = tz/tl; }
    }
    // Cloth is now simulated at the bone level (spring bones, above) — the mesh follows
    // through the normal skinning palette, so there's no separate mesh-cloth pass here.
}

// Derive the simulated-cloth vertex set + edge constraints from the cloth-flagged
// parts. Rest lengths come from the immutable bind pose. Rebuilt lazily when the
// geometry or the per-part cloth flags change.
void GLModelWidget::buildClothSim()
{
    m_clothBuilt = true;
    m_clothSeeded = false;
    // Reset capsule state up-front so an early return (a model with no cloth) can't leave the
    // previous model's capsules OR the authored-radius flag stale for the next collide()/overlay.
    m_colBoneA.clear(); m_colBoneB.clear();
    m_colP0Bind.clear(); m_colP1Bind.clear(); m_colR0.clear(); m_colR1.clear(); m_colRegion.clear();
    m_colP0.clear(); m_colP1.clear();
    m_colAuthored = false;
    m_vCloth.clear(); m_clothVerts.clear(); m_clothTris.clear();
    m_clothEdgeA.clear(); m_clothEdgeB.clear(); m_clothRest.clear();
    m_clothBendA.clear(); m_clothBendB.clear(); m_clothBendRest.clear();
    m_clothMaxDist.clear();
    m_clothPos.clear(); m_clothPrev.clear();
    const int vcount = m_bindVerts.size() / 11;
    if (vcount <= 0 || m_indices.isEmpty() || m_parts.isEmpty()) return;
    m_vCloth.fill(0, vcount);
    bool any = false;
    // (a) Material-flagged cloth/sim submeshes (cloth, _sim, skirt, cape, …).
    for (int i = 0; i < m_parts.size(); ++i) {
        if (i >= m_partCloth.size() || !m_partCloth[i]) continue;
        const Part& p = m_parts[i];
        for (int k = p.offset; k < p.offset + p.count && k < m_indices.size(); ++k) {
            const quint32 vi = m_indices[k];
            if (int(vi) < vcount) { m_vCloth[vi] = 1; any = true; }
        }
    }
    // (b) In-game cloth bones: any vertex whose dominant bone is a cloth/physics bone
    // (unified index >= the rig's base-bone count) is simulated, regardless of name —
    // except FX submeshes (e.g. flame planes), which are excluded so they don't wobble.
    // Same bad-split guard as buildSpringBones — and the same STANDALONE-PIECE relaxation: a bad
    // ratio only blocks classification when the model carries no authored cloth data. A lone helm/
    // cape in the Models tab legitimately has more cloth bones than base bones; its authored sim
    // cages/capsules are the proof the split is real.
    // (Matches buildSpringBones' rule: a credible body rig underneath — or any authored cloth
    //  data — means the split is real, even when the outfit carries more cloth bones than base.)
    const int skelN = m_skeleton.size();
    const bool ratioOk = (skelN - m_baseBones) <= skelN / 2 || m_baseBones >= 64
                         || !m_clothSims.isEmpty() || !m_authoredCaps.isEmpty();
    if (m_baseBones > 0 && m_baseBones < skelN && ratioOk) {
        QVector<quint8> vFx(vcount, 0);
        for (int i = 0; i < m_parts.size(); ++i) {
            if (i >= m_partFx.size() || !m_partFx[i]) continue;
            const Part& p = m_parts[i];
            for (int k = p.offset; k < p.offset + p.count && k < m_indices.size(); ++k) {
                const quint32 vi = m_indices[k];
                if (int(vi) < vcount) vFx[vi] = 1;
            }
        }
        for (int v = 0; v < vcount && v < m_vJoints.size(); ++v) {
            if (vFx[v]) continue;
            const auto& J = m_vJoints[v];
            const auto& W = m_vWeights[v];
            int best = -1; float bw = 0.0f;
            for (int k = 0; k < 4; ++k) if (W[k] > bw) { bw = W[k]; best = J[k]; }
            if (best >= m_baseBones) { m_vCloth[v] = 1; any = true; }
        }
    }
    if (!any) return;
    for (int v = 0; v < vcount; ++v) if (m_vCloth[v]) m_clothVerts.push_back(v);

    auto restLen = [&](int a, int b) {
        const float* pa = m_bindVerts.constData() + a * 11;
        const float* pb = m_bindVerts.constData() + b * 11;
        const float dx = pb[0]-pa[0], dy = pb[1]-pa[1], dz = pb[2]-pa[2];
        return std::sqrt(dx*dx + dy*dy + dz*dz);
    };
    // Stretch (structural) constraints — one per unique cloth edge.
    QSet<quint64> seen;
    auto addEdge = [&](int a, int b) {
        if (!m_vCloth[a] || !m_vCloth[b]) return;
        const int lo = qMin(a, b), hi = qMax(a, b);
        const quint64 key = (quint64(quint32(lo)) << 32) | quint32(hi);
        if (seen.contains(key)) return;
        seen.insert(key);
        m_clothEdgeA.push_back(lo); m_clothEdgeB.push_back(hi);
        m_clothRest.push_back(restLen(lo, hi));
    };
    // Bending constraints — link the two opposite verts of triangle pairs sharing an
    // edge (NvCloth-style). These resist folding, the main cause of self-clipping.
    QHash<quint64, int> edgeOpp;   // edge → first opposite vertex
    auto addBend = [&](int a, int b, int opp) {
        if (!m_vCloth[a] || !m_vCloth[b] || !m_vCloth[opp]) return;
        const int lo = qMin(a, b), hi = qMax(a, b);
        const quint64 key = (quint64(quint32(lo)) << 32) | quint32(hi);
        const auto it = edgeOpp.constFind(key);
        if (it == edgeOpp.constEnd()) { edgeOpp.insert(key, opp); return; }
        if (*it == opp) return;
        m_clothBendA.push_back(*it); m_clothBendB.push_back(opp);
        m_clothBendRest.push_back(restLen(*it, opp));
    };
    for (int k = 0; k + 2 < m_indices.size(); k += 3) {
        const int a = m_indices[k], b = m_indices[k+1], c = m_indices[k+2];
        if (!(m_vCloth[a] || m_vCloth[b] || m_vCloth[c])) continue;
        m_clothTris.push_back(k);   // precomputed: only triangles touching cloth
        addEdge(a, b); addEdge(b, c); addEdge(c, a);
        addBend(a, b, c); addBend(b, c, a); addBend(c, a, b);
    }
    m_clothPos.fill(0.0f, vcount * 3);
    m_clothPrev.fill(0.0f, vcount * 3);

    // ── Per-vertex MAX-DISTANCE (the game's flLateralMaxDistance anchoring) ───────
    // A particle may move at most this far from its skinned position: ~0 at the seam
    // (so it tracks the body), growing toward the free end. We approximate the game's
    // per-vertex cloth paint from the cloth-bone CHAIN DEPTH (data-driven from the rig)
    // blended with how much each vertex is weighted to cloth vs base bones.
    // Per-vertex max-distance (swing limit from the skinned pose): ~0 at the attachment
    // seam (so it tracks the body) growing toward the free end. Approximated from the
    // cloth-bone chain depth (data-driven from the rig) × how much each vert is weighted
    // to cloth vs base bones. Small overall, so the cloth stays near the authored shape.
    m_clothMaxDist.fill(0.0f, vcount);
    {
        const int nbAll = m_skeleton.size();
        QVector<int> depth(nbAll, 0);   // cloth-bone hops to the first base ancestor
        int maxDepth = 1;
        for (int j = 0; j < nbAll; ++j) {
            if (j < m_baseBones) { depth[j] = 0; continue; }
            const int p = m_skeleton[j].parent;
            depth[j] = (p >= 0 && p < nbAll && p >= m_baseBones) ? depth[p] + 1 : 1;
            maxDepth = qMax(maxDepth, depth[j]);
        }
        for (int v : m_clothVerts) {
            const auto& J = m_vJoints[v]; const auto& W = m_vWeights[v];
            float clothFrac = 0.0f; int domCloth = -1; float domW = 0.0f;
            for (int k = 0; k < 4; ++k) {
                if (W[k] <= 0.0f) continue;
                if (J[k] >= m_baseBones && J[k] < nbAll) { clothFrac += W[k];
                    if (W[k] > domW) { domW = W[k]; domCloth = J[k]; } }
            }
            const float dFac = (domCloth >= 0 && domCloth < nbAll)
                                   ? float(depth[domCloth]) / float(maxDepth) : 0.0f;
            m_clothMaxDist[v] = clothFrac * dFac;   // 0 (seam) .. 1 (free end)
        }
    }

    // ── Body-collision capsules, fitted from the body skin per bone ──────────────
    m_colBoneA.clear(); m_colBoneB.clear();
    m_colP0Bind.clear(); m_colP1Bind.clear(); m_colR0.clear(); m_colR1.clear(); m_colRegion.clear();
    const int nb = m_skeleton.size();
    if (nb <= 0) return;
    // Rest-pose world transform per bone (same compose+hierarchy as the skinning).
    std::vector<Mat4> restG(nb);
    for (int j = 0; j < nb; ++j) {
        const ModelJoint& jt = m_skeleton[j];
        const Mat4 L = composeTRS(jt.restQ.data(), jt.restT.data(), jt.restS.data());
        const int p = jt.parent;
        restG[j] = (p >= 0 && p < j) ? mat4mul(restG[p], L) : L;
    }

    // ── AUTHORED capsules (the game's real ClothData.ptCapsuleDefs) ──────────────
    // Each is a tapered capsule rigidly attached to one bone: spheres r1/r2 separated by
    // `height` along the local axis, placed by a bone-local transform. Positions are taken
    // to the bind pose via the bone's rest matrix; they then animate with the skinning
    // palette exactly like the skin-fit ones. Use them in place of the heuristic fit.
    // Authored radii are the game's EXACT collision sizes — they must not be scaled by the
    // capsuleRadius slider (that slider compensates the heuristic skin fit). m_colAuthored
    // makes collide()/the overlay use them at 1.0×; a 0.55× default was silently shrinking
    // the thigh capsules to half size, letting skirts clip into the legs.
    // ── Per-capsule BODY REGION ───────────────────────────────────────────────────────────────
    // The rig's shared player bones have stable hashes (same table blenderizeSkeletonNames uses),
    // so a capsule's bone identifies the body part it guards. Capsules often sit on an unnamed
    // child (a twist/roll bone), so walk up parents until a known hash is found — that is why this
    // is a lookup and not a name match. Unknown ⇒ CapOther, which defaults to 1.0 and changes
    // nothing. Region is resolved ONCE per geometry, not per frame.
    auto regionOfBone = [this](int b) -> quint8 {
        const int nb2 = m_skeleton.size();
        for (int g = 0, j = b; g < 64 && j >= 0 && j < nb2; ++g, j = m_skeleton[j].parent) {
            switch (m_skeleton[j].nameHash) {
                case 0x1289E8B3u: case 0x121D55ADu:   // thigh L/R
                case 0xB9CFD755u: case 0xB963444Fu:   // shin  L/R
                case 0x9CAC595Du: case 0x9C3FC657u:   // ankle L/R
                case 0x34EFB08Du: case 0x34831D87u:   // foot  L/R
                    return ClothParams::CapLegs;
                case 0x32FF39ADu:                     // pelvis
                    return ClothParams::CapWaist;
                case 0xD2322EEBu: case 0x20365219u:   // chest, center
                    return ClothParams::CapTorso;
                case 0xF51D6140u: case 0xF4B0CE3Au:   // upperArm L/R
                case 0x8E911E73u: case 0x8E248B6Du:   // forearm  L/R
                case 0xF54A1413u: case 0xF4DD810Du:   // hand     L/R
                    return ClothParams::CapArms;
                case 0xB0076FACu: case 0xD12DD5D1u:   // head, mouth
                    return ClothParams::CapHead;
                default: break;
            }
        }
        return ClothParams::CapOther;
    };
    m_colAuthored = !m_authoredCaps.isEmpty();
    m_capsFullSize = qEnvironmentVariableIsSet("D4_CAPS_FULL");
    if (m_colAuthored && qEnvironmentVariableIsSet("D4_DUMP_CLOTH")) {
        // Authored vs APPLIED radius, so the discrepancy is visible instead of inferred.
        const float applied = m_capsFullSize ? 1.0f : m_cloth.capsuleRadius;
        float mn = 1e9f, mx = 0.0f;
        for (const ClothCapsule& c : m_authoredCaps) { mn = qMin(mn, c.radius1); mx = qMax(mx, c.radius1); }
        static const char* kRgn[] = { "legs", "waist", "torso", "arms", "head", "other" };
        int rc[ClothParams::CapRegionCount] = {0};
        for (quint8 r : m_colRegion) if (r < ClothParams::CapRegionCount) ++rc[r];
        QString rs;
        for (int r = 0; r < ClothParams::CapRegionCount; ++r)
            if (rc[r]) rs += QStringLiteral(" %1=%2x%3").arg(QLatin1String(kRgn[r]))
                                 .arg(rc[r]).arg(double(m_cloth.capRegion[r]), 0, 'f', 2);
        qInfo("cloth-caps regions:%s", qPrintable(rs));
        qInfo("cloth-caps: %lld authored | radius1 %.3f..%.3f wu | slider %.2f | APPLIED x%.2f "
              "-> %.3f..%.3f wu%s",
              (long long)m_authoredCaps.size(), double(mn), double(mx),
              double(m_cloth.capsuleRadius), double(applied),
              double(mn*applied), double(mx*applied),
              m_capsFullSize ? "  [D4_CAPS_FULL=1]" : "");
    }
    if (!m_authoredCaps.isEmpty()) {
        QString capDbg;
        for (const ClothCapsule& c : m_authoredCaps) {
            if (c.boneIndex < 0 || c.boneIndex >= nb) continue;
            if (qEnvironmentVariableIsSet("D4_DUMP_CLOTH"))
                capDbg += QStringLiteral("cap bone=%1 '%2' restPos=[%3 %4 %5] r1=%6 h=%7\n")
                    .arg(c.boneIndex).arg(m_skeleton[c.boneIndex].name)
                    .arg(double(restG[c.boneIndex][12]),0,'f',3).arg(double(restG[c.boneIndex][13]),0,'f',3)
                    .arg(double(restG[c.boneIndex][14]),0,'f',3).arg(double(c.radius1),0,'f',3).arg(double(c.height),0,'f',3);
            const Mat4& m = restG[c.boneIndex];
            // A mirrored (left/right-flipped) bone has a left-handed rest frame: det<0.
            // The game authors capsules in that frame, so on the mirrored side the local
            // transform comes out reflected. Negate the bone-local offset+axis there so the
            // capsule is built in a consistent (right-handed) frame across both sides.
            const float det = m[0]*(m[5]*m[10]-m[6]*m[9]) - m[4]*(m[1]*m[10]-m[2]*m[9]) + m[8]*(m[1]*m[6]-m[2]*m[5]);
            const float mir = (det < 0.0f) ? -1.0f : 1.0f;
            const float qx=c.localQ[0], qy=c.localQ[1], qz=c.localQ[2], qw=c.localQ[3];
            // The capsule's long axis = the chosen local axis (X/Y/Z column of the local
            // rotation matrix), live-selectable since the convention isn't in the data.
            float ax, ay, az;
            if (m_capAxis == 1)      { ax=2*(qx*qy-qw*qz);     ay=1-2*(qx*qx+qz*qz); az=2*(qy*qz+qw*qx); }  // Y
            else if (m_capAxis == 2) { ax=2*(qx*qz+qw*qy);     ay=2*(qy*qz-qw*qx);   az=1-2*(qx*qx+qy*qy); } // Z
            else                     { ax=1-2*(qy*qy+qz*qz);   ay=2*(qx*qy+qw*qz);   az=2*(qx*qz-qw*qy); }   // X
            ax *= mir; ay *= mir; az *= mir;
            const float hh = c.height * 0.5f;
            const float lpx = c.localP[0]*mir, lpy = c.localP[1]*mir, lpz = c.localP[2]*mir;
            const float l0[3] = { lpx-ax*hh, lpy-ay*hh, lpz-az*hh };
            const float l1[3] = { lpx+ax*hh, lpy+ay*hh, lpz+az*hh };
            auto xf = [&](const float* in, float* o) {
                o[0] = m[0]*in[0] + m[4]*in[1] + m[8]*in[2] + m[12];
                o[1] = m[1]*in[0] + m[5]*in[1] + m[9]*in[2] + m[13];
                o[2] = m[2]*in[0] + m[6]*in[1] + m[10]*in[2] + m[14];
            };
            float p0[3], p1[3]; xf(l0, p0); xf(l1, p1);
            // The game authors left/right capsules in their own mirrored bone frames, which
            // flips the local axis on one side and inverts the taper. Anchor the radius
            // assignment to a consistent WORLD criterion (height) so both sides match: the
            // lower endpoint always carries radius2.
            float r0 = c.radius1, r1 = c.radius2;
            if (p0[1] < p1[1]) std::swap(r0, r1);
            m_colBoneA.push_back(c.boneIndex); m_colBoneB.push_back(c.boneIndex);
            m_colP0Bind.push_back(p0[0]); m_colP0Bind.push_back(p0[1]); m_colP0Bind.push_back(p0[2]);
            m_colP1Bind.push_back(p1[0]); m_colP1Bind.push_back(p1[1]); m_colP1Bind.push_back(p1[2]);
            m_colR0.push_back(r0); m_colR1.push_back(r1);
            m_colRegion.push_back(regionOfBone(c.boneIndex));   // authored: region from its bone
        }
        m_colP0.fill(0.0f, m_colR0.size() * 3);
        m_colP1.fill(0.0f, m_colR0.size() * 3);
        if (!capDbg.isEmpty()) {
            QFile f(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("cloth_caps_world.txt")));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) { f.write(capDbg.toUtf8()); f.close(); }
        }
        return;   // skip the skin-fit fallback below
    }
    // Cloth bind-pose bounding box (+margin): only body capsules near the cloth can
    // ever collide with it, so we prune the rest — big perf win, no quality loss.
    float clo[3] = {1e30f,1e30f,1e30f}, chi[3] = {-1e30f,-1e30f,-1e30f};
    for (int v : m_clothVerts) {
        const float* q = m_bindVerts.constData() + v * 11;
        for (int a = 0; a < 3; ++a) { clo[a] = qMin(clo[a], q[a]); chi[a] = qMax(chi[a], q[a]); }
    }
    constexpr float kNear = 0.20f;   // capsule must come within this of the cloth box
    for (int a = 0; a < 3; ++a) { clo[a] -= kNear; chi[a] += kNear; }
    // Per-base-bone: gather the body-skin verts dominated by it (not cloth, not FX).
    QVector<QVector<int>> byBone(nb);
    for (int v = 0; v < vcount && v < m_vJoints.size(); ++v) {
        if (m_vCloth[v]) continue;
        const auto& J = m_vJoints[v]; const auto& W = m_vWeights[v];
        int best = -1; float bw = 0.0f;
        for (int k = 0; k < 4; ++k) if (W[k] > bw) { bw = W[k]; best = J[k]; }
        if (best >= 0 && best < nb) byBone[best].push_back(v);
    }
    for (int b = 0; b < nb && b < m_baseBones; ++b) {
        if (byBone[b].size() < 16) continue;          // too little skin → skip
        // Prune: skip bones whose joint is far from the cloth (in bind pose).
        const float jx = restG[b][12], jy = restG[b][13], jz = restG[b][14];
        if (jx < clo[0] || jx > chi[0] || jy < clo[1] || jy > chi[1] || jz < clo[2] || jz > chi[2])
            continue;
        // Capsule segment: this bone's joint → its first base child's joint.
        int child = -1;
        for (int c = 0; c < nb; ++c) if (m_skeleton[c].parent == b && c < m_baseBones) { child = c; break; }
        const float p0x = restG[b][12], p0y = restG[b][13], p0z = restG[b][14];
        float p1x = p0x, p1y = p0y, p1z = p0z;
        if (child >= 0) { p1x = restG[child][12]; p1y = restG[child][13]; p1z = restG[child][14]; }
        // TAPERED radii (like the game's flRadiusA/flRadiusB): fit a separate radius
        // at each end from the skin verts nearest that end, so the capsule narrows down
        // the limb (hip→knee, etc.) instead of using one fat radius for the whole bone.
        const float sx = p1x-p0x, sy = p1y-p0y, sz = p1z-p0z;
        const float slen2 = sx*sx + sy*sy + sz*sz;
        std::vector<float> d0, d1;   // perpendicular distances near the p0 / p1 ends
        d0.reserve(byBone[b].size()); d1.reserve(byBone[b].size());
        for (int v : byBone[b]) {
            const float* q = m_bindVerts.constData() + v * 11;
            float t = slen2 > 1e-8f ? ((q[0]-p0x)*sx + (q[1]-p0y)*sy + (q[2]-p0z)*sz) / slen2 : 0.0f;
            t = qBound(0.0f, t, 1.0f);
            const float cx = p0x + sx*t, cy = p0y + sy*t, cz = p0z + sz*t;
            const float dx = q[0]-cx, dy = q[1]-cy, dz = q[2]-cz;
            const float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (t <= 0.5f) d0.push_back(dist);   // bucket toward the nearer endpoint
            if (t >= 0.5f) d1.push_back(dist);
        }
        // The capsule must ENCLOSE the limb (so cloth pushed onto it stays OUTSIDE the
        // body) — use a high percentile of the skin distances, not the median. (The old
        // ×0.92 median sat inside the skin, which guaranteed the body poked through.)
        auto pct = [](std::vector<float>& a, float p) -> float {
            if (a.empty()) return 0.0f;
            const size_t k = std::min(a.size() - 1, size_t(p * a.size()));
            std::nth_element(a.begin(), a.begin() + k, a.end());
            return a[k];
        };
        // 85th-pct encloses the limb; ×0.55 bakes in the old default multiplier so the unified
        // "Capsule size" slider reads 1.0 = the previous fitted size (was collide × 0.55).
        float r0 = pct(d0, 0.85f) * 0.55f, r1 = pct(d1, 0.85f) * 0.55f;
        if (r0 <= 1e-4f) r0 = r1;
        if (r1 <= 1e-4f) r1 = r0;
        if (r0 <= 1e-4f && r1 <= 1e-4f) continue;
        m_colBoneA.push_back(b); m_colBoneB.push_back(child >= 0 ? child : b);
        m_colP0Bind.push_back(p0x); m_colP0Bind.push_back(p0y); m_colP0Bind.push_back(p0z);
        m_colP1Bind.push_back(p1x); m_colP1Bind.push_back(p1y); m_colP1Bind.push_back(p1z);
        m_colR0.push_back(r0); m_colR1.push_back(r1);
        m_colRegion.push_back(regionOfBone(b));   // skin-fit: same lookup, same index
    }
    m_colP0.fill(0.0f, m_colR0.size() * 3);
    m_colP1.fill(0.0f, m_colR0.size() * 3);

    // DIAGNOSTIC: report what the sim actually detected, so a flat/static cloth can be
    // traced (clothVerts==0 → not detected; maxDist all ~0 → anchored to skinned pose).
    {
        // How many of the merged verts are skinned to a cloth bone (unified idx >= base)?
        int clothBoneVerts = 0;
        for (int v = 0; v < vcount && v < m_vJoints.size(); ++v) {
            const auto& J = m_vJoints[v]; const auto& W = m_vWeights[v];
            for (int k = 0; k < 4; ++k) if (W[k] > 0.0f && J[k] >= m_baseBones) { ++clothBoneVerts; break; }
        }
        float mdMin = 1e9f, mdMax = -1e9f;
        for (int v : m_clothVerts) { mdMin = qMin(mdMin, m_clothMaxDist[v]); mdMax = qMax(mdMax, m_clothMaxDist[v]); }
        qInfo().noquote() << "CLOTHSIM verts=" << vcount << "baseBones=" << m_baseBones
                          << "skel=" << m_skeleton.size() << "clothBoneVerts=" << clothBoneVerts
                          << "clothVerts=" << m_clothVerts.size() << "capsules=" << m_colR0.size()
                          << "edges=" << m_clothEdgeA.size()
                          << "maxDist=[" << (m_clothVerts.isEmpty()?0.f:mdMin) << ".." << (m_clothVerts.isEmpty()?0.f:mdMax) << "]";
    }
    // (The mesh-cage solver path is retired — cloth is now driven by the spring-bone solver,
    // built in buildSpringBones — so buildCageBindings is no longer invoked.)
}

// (The legacy vertex-Verlet cloth solver — clothStep / closestPtTri / buildCageBindings /
//  cageStep and all m_cage* state — is DELETED. Cloth is simulated at the bone level by
//  springBoneStep (buildSpringBones), and nothing called this path any more.)


QString GLModelWidget::s_glInfo;

void GLModelWidget::initializeGL()
{
    if (!m_fxClock.isValid()) m_fxClock.start();   // wall-clock for animated mesh FX
    const bool funcsOk = initializeOpenGLFunctions();
    {
        QOpenGLContext* ctx = context();
        const QSurfaceFormat f = ctx ? ctx->format() : QSurfaceFormat();
        qInfo("GLModelWidget GL: funcs=%d ctxValid=%d version=%d.%d renderer=%s",
              funcsOk ? 1 : 0, (ctx && ctx->isValid()) ? 1 : 0,
              f.majorVersion(), f.minorVersion(),
              reinterpret_cast<const char*>(glGetString(GL_VERSION)));
        // Remember the GPU/driver string for Help ▸ Copy diagnostic info.
        s_glInfo = QStringLiteral("%1 — GL %2")
            .arg(QString::fromLatin1(reinterpret_cast<const char*>(glGetString(GL_RENDERER))),
                 QString::fromLatin1(reinterpret_cast<const char*>(glGetString(GL_VERSION))));
    }
    glClearColor(0.10f, 0.10f, 0.11f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);   // smooth reflection-probe sampling across cube edges
    // Max anisotropy this GPU supports (EXT constant 0x84FF; same value as the GL 4.6 core token).
    // Anisotropic filtering keeps high-frequency albedo detail (marking/tattoo edges) crisp at the
    // grazing angles body surfaces are usually viewed at, instead of aliasing into a rough fringe.
    m_maxAniso = 1.0f;
    glGetFloatv(0x84FF, &m_maxAniso);

    auto compile = [this](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            m_error = QString::fromLatin1(log);
            qWarning("GLModelWidget shader compile failed: %s", log);
        }
        return s;
    };

    m_uniLoc.clear();   // (re)building programs → any cached uniform locations are now stale
    GLuint vs = compile(GL_VERTEX_SHADER, kVert);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
    m_prog = glCreateProgram();
    glAttachShader(m_prog, vs);
    glAttachShader(m_prog, fs);
    glLinkProgram(m_prog);
    GLint linked = 0;
    glGetProgramiv(m_prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(m_prog, sizeof(log), nullptr, log);
        qWarning("GLModelWidget program link failed: %s", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    // Shadow-map program + depth FBO (key-light self-shadowing).
    GLuint svs = compile(GL_VERTEX_SHADER, kShadowVert);
    GLuint sfs = compile(GL_FRAGMENT_SHADER, kShadowFrag);
    m_shadowProg = glCreateProgram();
    glAttachShader(m_shadowProg, svs);
    glAttachShader(m_shadowProg, sfs);
    glLinkProgram(m_shadowProg);
    glDeleteShader(svs);
    glDeleteShader(sfs);
    glGenFramebuffers(1, &m_shadowFbo);
    glGenTextures(1, &m_shadowTex);
    glBindTexture(GL_TEXTURE_2D, m_shadowTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, m_shadowSize, m_shadowSize, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float sbord[4] = {1.0f, 1.0f, 1.0f, 1.0f};   // outside the map = far (fully lit)
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, sbord);
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_shadowTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo());

    // SSAO position-prepass program + world-position G-buffer (storage sized on demand in renderPos).
    GLuint pvs = compile(GL_VERTEX_SHADER, kPosVert);
    GLuint pfs = compile(GL_FRAGMENT_SHADER, kPosFrag);
    m_posProg = glCreateProgram();
    glAttachShader(m_posProg, pvs);
    glAttachShader(m_posProg, pfs);
    glLinkProgram(m_posProg);
    glDeleteShader(pvs);
    glDeleteShader(pfs);
    glGenFramebuffers(1, &m_posFbo);
    glGenTextures(1, &m_posTex);
    glGenRenderbuffers(1, &m_posDepth);

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ibo);
}

// SSAO prepass: render the model's world-space position into the RGBA16F G-buffer (a=1 geometry).
void GLModelWidget::renderPos(const QMatrix4x4& mvp, const QMatrix4x4& model)
{
    if (m_posFbo == 0 || m_posProg == 0 || m_indexCount == 0) return;
    if (m_posW != m_fbW || m_posH != m_fbH) {           // (re)allocate to framebuffer resolution
        m_posW = m_fbW; m_posH = m_fbH;
        glBindTexture(GL_TEXTURE_2D, m_posTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_posW, m_posH, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindRenderbuffer(GL_RENDERBUFFER, m_posDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, m_posW, m_posH);
        glBindFramebuffer(GL_FRAMEBUFFER, m_posFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_posTex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_posDepth);
        glBindFramebuffer(GL_FRAMEBUFFER, targetFbo());
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_posFbo);
    glViewport(0, 0, m_posW, m_posH);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);               // a=0 → background (skipped by the AO loop)
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(m_posProg);
    glUniformMatrix4fv(uni(m_posProg, "uMVP"), 1, GL_FALSE, mvp.constData());
    glUniformMatrix4fv(uni(m_posProg, "uModel"), 1, GL_FALSE, model.constData());
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(m_vao);
    // Same per-part visibility as the shadow/main passes so HIDDEN parts don't occlude in SSAO
    // (otherwise a hidden mesh leaves a dark ambient-occlusion halo where it used to be).
    if (m_parts.isEmpty()) {
        glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
    } else {
        for (int i = 0; i < m_parts.size(); ++i) {
            const Part& p = m_parts[i];
            if (!p.visible || p.count == 0) continue;
            if (i < m_partFx.size() && m_partFx[i]) continue;
            glDrawElements(GL_TRIANGLES, p.count, GL_UNSIGNED_INT,
                           reinterpret_cast<void*>(qintptr(p.offset) * sizeof(quint32)));
        }
    }
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo());
    glViewport(0, 0, m_fbW, m_fbH);
}

// Depth-only pass: render the model from the key light's POV into the shadow map.
void GLModelWidget::renderShadow(const QMatrix4x4& lightMvp)
{
    if (m_shadowFbo == 0 || m_shadowProg == 0 || m_indexCount == 0) return;
    if (m_shadowResDirty) {                              // resolution changed → resize the depth texture
        m_shadowResDirty = false;
        glBindTexture(GL_TEXTURE_2D, m_shadowTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, m_shadowSize, m_shadowSize, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFbo);
    glViewport(0, 0, m_shadowSize, m_shadowSize);
    glClear(GL_DEPTH_BUFFER_BIT);
    glUseProgram(m_shadowProg);
    glUniformMatrix4fv(uni(m_shadowProg, "uLightMVP"), 1, GL_FALSE, lightMvp.constData());
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(m_vao);
    // Cast shadows per-part so HIDDEN parts (part-tree checkbox / FX·SIM·FORM toggles) don't cast,
    // and transparent FX submeshes don't throw solid shadows. Fall back to the whole mesh when the
    // model has no sub-parts.
    if (m_parts.isEmpty()) {
        glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
    } else {
        for (int i = 0; i < m_parts.size(); ++i) {
            const Part& p = m_parts[i];
            if (!p.visible || p.count == 0) continue;                       // hidden → no shadow
            if (i < m_partFx.size() && m_partFx[i]) continue;               // transparent FX → no shadow
            glDrawElements(GL_TRIANGLES, p.count, GL_UNSIGNED_INT,
                           reinterpret_cast<void*>(qintptr(p.offset) * sizeof(quint32)));
        }
    }
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo());
    glViewport(0, 0, m_fbW, m_fbH);
}


void GLModelWidget::destroyBuffers()
{
    if (m_ibo) { glDeleteBuffers(1, &m_ibo); m_ibo = 0; }
    if (m_vbo) { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
    if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    m_thumbFbo.reset(); m_thumbFboSize = 0;   // free the persistent thumbnail FBO (context is current)
    destroyTextures();
    m_indexCount = 0;
}

void GLModelWidget::uploadPending()
{
    m_hasPending = false;
    if (m_vao == 0) { glGenVertexArrays(1, &m_vao); }
    if (m_vbo == 0) { glGenBuffers(1, &m_vbo); }
    if (m_ibo == 0) { glGenBuffers(1, &m_ibo); }

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, m_verts.size() * sizeof(float),
                 m_verts.constData(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indices.size() * sizeof(quint32),
                 m_indices.constData(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float),
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 11 * sizeof(float),
                          reinterpret_cast<void*>(6 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float),
                          reinterpret_cast<void*>(8 * sizeof(float)));
    glBindVertexArray(0);

    m_indexCount = m_indices.size();
}

void GLModelWidget::resizeGL(int w, int h)
{
    m_fbW = w; m_fbH = h;   // device-pixel framebuffer size (restored after the shadow pass)
    glViewport(0, 0, w, h);
    if (m_overlayLabel) m_overlayLabel->setGeometry(rect());   // keep the overlay covering the viewport
    if (m_gizmo) {
        m_gizmo->setGeometry(width() - 96, 8, 88, 88);   // pinned top-right (widget coords)
        m_gizmo->raise();   // above the loading/empty-state overlay label
    }
}

// Centered text over the viewport (empty-state hint or "Loading…"). Implemented as a child QLabel
// composited over the GL surface — driver-independent (no QPainter-over-GL), so it renders the same
// on every machine. Empty string hides it.
void GLModelWidget::setOverlayText(const QString& text)
{
    if (text.isEmpty()) { if (m_overlayLabel) m_overlayLabel->hide(); return; }
    if (!m_overlayLabel) {
        m_overlayLabel = new QLabel(this);
        m_overlayLabel->setAlignment(Qt::AlignCenter);
        m_overlayLabel->setWordWrap(true);
        m_overlayLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_overlayLabel->setStyleSheet(QStringLiteral(
            "QLabel { color: rgba(190,190,200,220); background: transparent; font-size: 13px; }"));
    }
    m_overlayLabel->setText(text);
    m_overlayLabel->setGeometry(rect());
    m_overlayLabel->show();
    m_overlayLabel->raise();
}

void GLModelWidget::paintGL()
{
    if (m_hasPending)
        uploadPending();
    if (m_hasPendingTex)
        uploadTextures();
    // (The pending dye-gradient upload left with setDyeGradient — nothing ever queued one.)

    // a=0 → native-alpha capture. Coverage mode clears to 0 as well but keeps DRAWING the backdrop,
    // so the colour is the normal opaque render while the alpha channel still says where the model
    // is. That is what lets "crop to model" find the subject without a second render.
    glClearColor(m_bg[0], m_bg[1], m_bg[2],
                 (m_transparentClear || m_coverageAlpha) ? 0.0f : 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // ── Studio-gradient backdrop (Graphics ▸ Backdrop ▸ Gradient): a vertical wash derived
    // from the background colour (lighter up top, darker below), drawn as a single vertex-ID
    // fullscreen triangle — no VBO, depth untouched, and skipped for transparent captures. ──
    if (m_bgGradient && !m_transparentClear) {
        if (!m_bgProg) {
            static const char* kBgVert = R"(#version 450 core
out float vT;
void main() {
    vec2 p = vec2(gl_VertexID == 2 ? 3.0 : -1.0, gl_VertexID == 1 ? 3.0 : -1.0);
    vT = p.y * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
})";
            static const char* kBgFrag = R"(#version 450 core
in float vT; out vec4 o;
uniform vec3 uTop; uniform vec3 uBot; uniform float uA;
void main() { o = vec4(mix(uBot, uTop, clamp(vT, 0.0, 1.0)), uA); })";
            auto compile = [this](GLenum type, const char* src) -> GLuint {
                GLuint sh = glCreateShader(type);
                glShaderSource(sh, 1, &src, nullptr);
                glCompileShader(sh);
                GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
                if (!ok) { glDeleteShader(sh); return 0; }
                return sh;
            };
            const GLuint vs = compile(GL_VERTEX_SHADER, kBgVert);
            const GLuint fs = compile(GL_FRAGMENT_SHADER, kBgFrag);
            if (vs && fs) {
                m_bgProg = glCreateProgram();
                glAttachShader(m_bgProg, vs); glAttachShader(m_bgProg, fs);
                glLinkProgram(m_bgProg);
                GLint ok = 0; glGetProgramiv(m_bgProg, GL_LINK_STATUS, &ok);
                if (!ok) { glDeleteProgram(m_bgProg); m_bgProg = 0; }
            }
            if (vs) glDeleteShader(vs);
            if (fs) glDeleteShader(fs);
            if (m_bgProg) glGenVertexArrays(1, &m_bgVao);   // core profile needs A bound VAO
        }
        if (m_bgProg) {
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glUseProgram(m_bgProg);
            glUniform3f(glGetUniformLocation(m_bgProg, "uTop"),
                        qMin(1.0f, m_bg[0] * 1.55f + 0.05f), qMin(1.0f, m_bg[1] * 1.55f + 0.05f),
                        qMin(1.0f, m_bg[2] * 1.55f + 0.05f));
            glUniform3f(glGetUniformLocation(m_bgProg, "uBot"),
                        m_bg[0] * 0.40f, m_bg[1] * 0.40f, m_bg[2] * 0.40f);
            // The backdrop is background, so in coverage mode it stays at alpha 0 — only the model
            // marks the alpha channel. Its RGB is untouched either way.
            glUniform1f(glGetUniformLocation(m_bgProg, "uA"), m_coverageAlpha ? 0.0f : 1.0f);
            glBindVertexArray(m_bgVao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
            glDepthMask(GL_TRUE);
            glEnable(GL_DEPTH_TEST);
        }
    }

    if (m_indexCount == 0 || m_prog == 0)
        return;

    const float aspect = height() > 0 ? float(width()) / float(height()) : 1.0f;

    // Orbit camera around the model centre.
    const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    const float cy = std::cos(m_yaw),   sy = std::sin(m_yaw);
    const QVector3D dir(cp * sy, sp, cp * cy);
    const QVector3D eye = m_center + dir * m_dist;

    QMatrix4x4 proj;
    if (m_ortho) {
        // Match the perspective framing at the orbit centre: half-height = dist·tan(fov/2).
        const float h = m_dist * std::tan(m_fov * 0.5f * 3.14159265f / 180.0f);
        const float w = h * aspect;
        // Depth range scaled by BOTH dist and radius so it never collapses when m_radius is momentarily
        // stale (camera restored on relaunch before the model's bounds are known — the old
        // `m_dist - m_radius*10` became a thin slab around m_dist and sliced the model).
        const float ext = qMax(m_radius, m_dist * 0.5f) * 10.0f;
        proj.ortho(-w, w, -h, h, qMax(0.001f, m_dist - ext), m_dist + ext);
    } else {
        // Far plane follows the camera distance so you can zoom out on very large models without
        // the mesh clipping into the far plane (no fixed 20×-radius wall).
        proj.perspective(m_fov, aspect, qMax(0.001f, m_radius * 0.02f),
                         qMax(m_radius * 20.0f, m_dist + m_radius * 4.0f));
    }
    QMatrix4x4 view;
    view.lookAt(eye, m_center, QVector3D(0, 1, 0));
    QMatrix4x4 model;  // identity
    const QMatrix4x4 mvp = proj * view * model;
    m_lastViewProj = mvp;   // model is identity → this projects bone-head world positions for labels

    glUseProgram(m_prog);
    glUniformMatrix4fv(uni(m_prog, "uMVP"), 1, GL_FALSE, mvp.constData());
    glUniformMatrix4fv(uni(m_prog, "uModel"), 1, GL_FALSE, model.constData());
    // Camera-relative three-point rig (a portrait set-up that tracks the orbit): warm key
    // (offset up/right so Cook-Torrance highlights + rim Fresnel read), cool back rim behind
    // and above for edge separation, cool front fill opposite the key to open the shadows.
    const QVector3D viewDir = (eye - m_center).normalized();
    QVector3D rightV = QVector3D::crossProduct(viewDir, QVector3D(0, 1, 0));
    if (rightV.lengthSquared() < 1e-6f) rightV = QVector3D(1, 0, 0);
    rightV.normalize();
    const QVector3D worldUp(0, 1, 0);
    const float D2R = 0.01745329252f;
    const float kaz = m_rig.keyAzimuth * D2R, kel = m_rig.keyElevation * D2R;
    // Camera-relative three-point basis (always computed, so lock can snapshot the current look).
    const QVector3D camKey  = (viewDir*std::cos(kel) + worldUp*std::sin(kel) + rightV*std::sin(kaz)).normalized();
    const QVector3D camRim  = (-viewDir*0.70f + worldUp*0.55f + rightV*0.30f).normalized();
    const QVector3D camFill = ( viewDir*0.70f + worldUp*0.05f - rightV*0.50f).normalized();
    // Light-lock: freeze the rig in world space at the moment lock is enabled — orbit the camera to
    // light the character how you like, then toggle lock to pin those exact world directions so they
    // no longer follow the camera. Re-enabling re-captures at the current orbit.
    QVector3D keyDir, rimDir, fillDir;
    if (m_lightLock) {
        if (!m_lockValid) { m_lockKey = camKey; m_lockRim = camRim; m_lockFill = camFill; m_lockValid = true; }
        keyDir = m_lockKey; rimDir = m_lockRim; fillDir = m_lockFill;
    } else {
        keyDir = camKey; rimDir = camRim; fillDir = camFill;
    }
    // Self-shadow: render the model's depth from the key light into the shadow map (own FBO),
    // then restore the main framebuffer + program before the main pass continues.
    QMatrix4x4 shadowMvp;
    if (m_shadowOn) {
        const QVector3D lpos = m_center + keyDir * (m_radius * 3.0f);
        const QVector3D lup  = (std::abs(keyDir.y()) > 0.98f) ? QVector3D(0, 0, 1) : QVector3D(0, 1, 0);
        QMatrix4x4 lview; lview.lookAt(lpos, m_center, lup);
        QMatrix4x4 lproj; const float sr = m_radius * m_shadowRange;
        lproj.ortho(-sr, sr, -sr, sr, m_radius * 0.5f, m_radius * 6.0f);
        shadowMvp = lproj * lview;
        renderShadow(shadowMvp);
        glUseProgram(m_prog);
        glUniformMatrix4fv(uni(m_prog, "uMVP"), 1, GL_FALSE, mvp.constData());
    }
    // SSAO: render the world-position G-buffer (own FBO), then restore the main framebuffer.
    if (m_ssaoOn) {
        renderPos(mvp, model);
        glUseProgram(m_prog);
        glUniformMatrix4fv(uni(m_prog, "uMVP"), 1, GL_FALSE, mvp.constData());
    }
    glUniform3f(uni(m_prog, "uLightDir"), keyDir.x(),  keyDir.y(),  keyDir.z());
    glUniform3f(uni(m_prog, "uRimDir"),   rimDir.x(),  rimDir.y(),  rimDir.z());
    glUniform3f(uni(m_prog, "uFillDir"),  fillDir.x(), fillDir.y(), fillDir.z());
    glUniform3f(uni(m_prog, "uViewPos"), eye.x(), eye.y(), eye.z());

    const GLint uBase = uni(m_prog, "uBase");
    const GLint uSolid = uni(m_prog, "uSolid");            // selection silhouette: flat fill
    const GLint uNdcOffset = uni(m_prog, "uNdcOffset");    // selection silhouette: pixel jitter
    const GLint uHasTex = uni(m_prog, "uHasTex");
    const GLint uHasNormal = uni(m_prog, "uHasNormal");
    const GLint uPbr = uni(m_prog, "uPbr");
    const GLint uMetal = uni(m_prog, "uMetal");
    const GLint uRough = uni(m_prog, "uRough");
    const GLint uHasOrm = uni(m_prog, "uHasOrm");
    const GLint uHasEmissive = uni(m_prog, "uHasEmissive");
    const GLint uEmisMul = uni(m_prog, "uEmisMul");
    const GLint uEmisColor = uni(m_prog, "uEmisColor");
    const GLint uHasDetailN = uni(m_prog, "uHasDetailN");
    const GLint uDetailNInt = uni(m_prog, "uDetailNInt");
    const GLint uDetailRInt = uni(m_prog, "uDetailRInt");
    const GLint uDetailROffset = uni(m_prog, "uDetailROffset");
    const GLint uDetailColorAdd = uni(m_prog, "uDetailColorAdd");
    const GLint uDetailScales = uni(m_prog, "uDetailScales");
    const GLint uDetailMetalLayer = uni(m_prog, "uDetailMetalLayer");
    const GLint uZoneMap = uni(m_prog, "uZoneMap");
    const GLint uDyeBands = uni(m_prog, "uDyeBands");
    const GLint uMetalThresh = uni(m_prog, "uMetalThresh");
    const GLint uMetalRoute = uni(m_prog, "uMetalRoute");
    const GLint uHasDetailR = uni(m_prog, "uHasDetailR");
    const GLint uHasDetN0 = uni(m_prog, "uHasDetN0");
    const GLint uHasDetN1 = uni(m_prog, "uHasDetN1");
    const GLint uHasDetN2 = uni(m_prog, "uHasDetN2");
    const GLint uHasDetR0 = uni(m_prog, "uHasDetR0");
    const GLint uHasDetR1 = uni(m_prog, "uHasDetR1");
    const GLint uHasDetR2 = uni(m_prog, "uHasDetR2");
    const GLint uHasTrans = uni(m_prog, "uHasTrans");
    const GLint uHasMask = uni(m_prog, "uHasMask");
    const GLint uHasDyeMask = uni(m_prog, "uHasDyeMask");
    const GLint uHasDyeRamp = uni(m_prog, "uHasDyeRamp");
    const GLint uDyeRegion = uni(m_prog, "uDyeRegion");
    const GLint uFDye = uni(m_prog, "uFDye");        // cached: set per-part in loop
    const GLint uDyeColor = uni(m_prog, "uDyeColor");// cached: set per-part in loop
    const GLint uIsHair = uni(m_prog, "uIsHair");
    const GLint uHairParams = uni(m_prog, "uHairParams");   // set per-hair-part in loop
    const GLint uIsSkin = uni(m_prog, "uIsSkin");
    const GLint uIsHead = uni(m_prog, "uIsHead");
    const GLint uIsEye = uni(m_prog, "uIsEye");
    const GLint uIsCloth = uni(m_prog, "uIsCloth");
    const GLint uFurEnabled = uni(m_prog, "uFurEnabled");
    const GLint uFurShell   = uni(m_prog, "uFurShell");
    const GLint uHasFurNoise = uni(m_prog, "uHasFurNoise");
    const GLint uFxMode = uni(m_prog, "uFxMode");   // FX per-part uniforms set in drawFxParts()
    glUniform1i(uni(m_prog, "uTex"), 0);
    glUniform1i(uni(m_prog, "uNormalTex"), 1);
    glUniform1i(uni(m_prog, "uOrmTex"), 2);
    glUniform1i(uni(m_prog, "uEmissiveTex"), 3);
    // Up-to-3 detail normal maps (units 17-19) + 3 rough (20-22), selected per texel by dye zone.
    glUniform1i(uni(m_prog, "uDetN0"), 17);
    glUniform1i(uni(m_prog, "uDetN1"), 18);
    glUniform1i(uni(m_prog, "uDetN2"), 19);
    glUniform1i(uni(m_prog, "uDetR0"), 20);
    glUniform1i(uni(m_prog, "uDetR1"), 21);
    glUniform1i(uni(m_prog, "uDetR2"), 22);
    glUniform1i(uni(m_prog, "uTransTex"), 6);
    glUniform1i(uni(m_prog, "uMaskTex"), 7);
    glUniform1i(uni(m_prog, "uDyeMaskTex"), 8);
    glUniform1i(uni(m_prog, "uDyeRampTex"), 9);
    glUniform1i(uni(m_prog, "uDyeGradTex"), 10);
    glUniform1i(uni(m_prog, "uDyeMode"), m_dyeMode);
    {   // bind the (global) real-dye gradient once
        glActiveTexture(GL_TEXTURE10);
        glBindTexture(GL_TEXTURE_2D, m_dyeGradTex);
        glUniform1i(uni(m_prog, "uHasDyeGrad"), m_dyeGradTex ? 1 : 0);
        glActiveTexture(GL_TEXTURE0);
    }
    glUniform1f(uni(m_prog, "uDetailScale"), m_detailScale);
    glUniform1f(uni(m_prog, "uDetailNormalMul"), m_detailNormalMul);
    glUniform1f(uni(m_prog, "uDetailRoughMul"), m_detailRoughMul);
    // Fur: noise on unit 11, density/length mask on unit 12; params from the game's fur model.
    glUniform1i(uni(m_prog, "uFurNoise"), 11);
    glUniform1i(uni(m_prog, "uFurMask"), 12);
    glUniform1i(uFurEnabled, 0);
    glUniform1f(uFurShell, 0.0f);
    glUniform1f(uni(m_prog, "uFurExtrusion"), m_furLength * m_radius);
    glUniform1f(uni(m_prog, "uFurGravity"),   m_furGravity * m_radius);
    glUniform1f(uni(m_prog, "uFurCurl"),      m_furCurl * m_radius);
    glUniform1f(uni(m_prog, "uFurCoverage"),  m_furCoverage);
    glUniform1f(uni(m_prog, "uFurTiling"),    m_furTiling);
    glUniform1f(uni(m_prog, "uFurAniso"),     0.25f);   // Fur Aniso Strength
    glUniform1f(uni(m_prog, "uFurRootRough"), 0.62f);   // Fur Root Roughness
    glUniform1f(uni(m_prog, "uFurTipRough"),  0.92f);   // Fur Tip Roughness
    glUniform1f(uni(m_prog, "uFurSecondary"), 0.6f);    // dual-noise blend
    glUniform3f(uni(m_prog, "uFurRootColor"), 0.55f, 0.52f, 0.48f);
    glUniform3f(uni(m_prog, "uFurTipColor"),  1.06f, 1.04f, 1.0f);
    glUniform1i(uHasFurNoise, 0);
    glUniform1f(uEmisMul, 1.0f);
    glUniform3f(uEmisColor, 1.0f, 1.0f, 1.0f);
    glUniform1f(uni(m_prog, "uEmisScale"), m_emisScale);
    // Mesh FX defaults: noise on unit 13, gentle upward drift, emissive boost, soft fresnel.
    glUniform1i(uni(m_prog, "uFxNoise"), 13);
    glUniform1i(uFxMode, 0);
    glUniform1i(uni(m_prog, "uHasFxNoise"), 0);
    glUniform1f(uni(m_prog, "uFxTiling"),    2.0f);
    glUniform2f(uni(m_prog, "uFxScroll"),    0.015f * m_fxScrollSpeed, 0.07f * m_fxScrollSpeed);
    // uFx{Intensity,Wobble,Fresnel,Alpha,Saturation} are set per-part in the FX pass from authored values.
    glUniform1f(uni(m_prog, "uTime"), float(m_fxClock.isValid() ? m_fxClock.elapsed() : 0) * 0.001f);
    glUniform1i(uni(m_prog, "uFDetail"),  m_fDetail ? 1 : 0);
    glUniform1i(uni(m_prog, "uSpecAA"),   m_fSpecAA ? 1 : 0);
    glUniform1i(uni(m_prog, "uFSubsurf"), m_fSubsurf ? 1 : 0);
    glUniform1i(uni(m_prog, "uFHair"),    m_fHair ? 1 : 0);
    glUniform1i(uni(m_prog, "uFIbl"),     m_fIbl ? 1 : 0);
    glUniform1i(uni(m_prog, "uFMask"),    m_fMask ? 1 : 0);
    glUniform1i(uni(m_prog, "uFTonemap"), m_fTonemap ? 1 : 0);
    glUniform1i(uni(m_prog, "uViewChannel"), m_viewChannel);
    glUniform1i(uFDye,     m_fDye ? 1 : 0);              // global default; per-part override in loop
    glUniform3fv(uDyeColor, 4, &m_dyeColor[0][0]);
    glUniform1f(uni(m_prog, "uExposure"), m_exposure);
    glUniform1i(uni(m_prog, "uColorGrade"), m_colorGrade ? 1 : 0);
    glUniform1f(uni(m_prog, "uCgContrast"), m_cgContrast);
    glUniform1f(uni(m_prog, "uCgSat"),      m_cgSat);
    glUniform1f(uni(m_prog, "uCgWarmth"),   m_cgWarmth);
    if (m_lutDirty) {                                    // deferred LUT upload (context is current here)
        m_lutDirty = false;
        if (m_pendingLut.isNull()) { m_hasLut = false; }
        else {
            if (m_lutTex == 0) glGenTextures(1, &m_lutTex);
            glActiveTexture(GL_TEXTURE17);
            glBindTexture(GL_TEXTURE_2D, m_lutTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_pendingLut.constBits());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glActiveTexture(GL_TEXTURE0);
            m_hasLut = true;
            m_pendingLut = QImage();
        }
    }
    glActiveTexture(GL_TEXTURE17);                       // real D4 grade LUT (256×16), when loaded
    glBindTexture(GL_TEXTURE_2D, m_lutTex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(uni(m_prog, "uLut"),    17);
    glUniform1i(uni(m_prog, "uHasLut"), (m_lutTex && m_hasLut) ? 1 : 0);
    glUniform1f(uni(m_prog, "uSkinCurv"), m_radius);   // scale-independent skin curvature
    glUniform3fv(uni(m_prog, "uEnvSky"), 1, m_envSky);
    glUniform3fv(uni(m_prog, "uEnvHor"), 1, m_envHor);
    glUniform3fv(uni(m_prog, "uEnvGnd"), 1, m_envGnd);
    glUniform3fv(uni(m_prog, "uLightCol"), 1, m_lightCol);
    glUniform3fv(uni(m_prog, "uRimCol"),   1, m_rimCol);
    glUniform3fv(uni(m_prog, "uFillCol"),  1, m_fillCol);
    glUniform1f(uni(m_prog, "uAmbScale"), m_ambScale);
    // Reflection-probe cubemap on unit 14 (uploaded lazily from its pending CASC payload).
    if (m_hasPendingRefl) uploadReflectionCubemap();
    const bool reflOn = (m_reflCube != 0) && m_reflEnabled;
    glActiveTexture(GL_TEXTURE14);
    glBindTexture(GL_TEXTURE_CUBE_MAP, reflOn ? m_reflCube : 0);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(uni(m_prog, "uReflCube"), 14);
    glUniform1i(uni(m_prog, "uHasReflCube"), reflOn ? 1 : 0);
    glUniform1f(uni(m_prog, "uReflMaxMip"), float(m_reflMaxMip));
    // Shadow map on unit 15 + its parameters.
    const bool shOn = m_shadowOn && m_shadowTex != 0;
    glActiveTexture(GL_TEXTURE15);
    glBindTexture(GL_TEXTURE_2D, shOn ? m_shadowTex : 0);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(uni(m_prog, "uShadowMap"), 15);
    glUniform1i(uni(m_prog, "uShadowOn"), shOn ? 1 : 0);
    glUniformMatrix4fv(uni(m_prog, "uLightMVP"), 1, GL_FALSE, shadowMvp.constData());
    glUniform1f(uni(m_prog, "uShadowStr"),  m_shadowStr);
    glUniform1f(uni(m_prog, "uShadowSoft"), m_shadowSoft);
    glUniform1f(uni(m_prog, "uShadowBias"), m_shadowBias);
    glUniform1f(uni(m_prog, "uShadowNBias"), m_shadowNBias * m_radius);
    glUniform1f(uni(m_prog, "uShadowSize"), float(m_shadowSize));
    // SSAO world-position G-buffer on unit 16 + parameters (radius scaled to the model size).
    const bool ssOn = m_ssaoOn && m_posTex != 0;
    glActiveTexture(GL_TEXTURE16);
    glBindTexture(GL_TEXTURE_2D, ssOn ? m_posTex : 0);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(uni(m_prog, "uPosTex"), 16);
    // Global detail-map experiment config (Detail-maps panel). metalThresh/metalRoute are always
    // global; zoneMap/bands are set here as a fallback and overridden per-part below when autoMode
    // is on (each part carries its own detected bands + derived zone→map).
    glUniform4i(uZoneMap, m_detailCfg.zoneMap[0], m_detailCfg.zoneMap[1], m_detailCfg.zoneMap[2], m_detailCfg.zoneMap[3]);
    glUniform4f(uDyeBands, m_detailCfg.bands[0], m_detailCfg.bands[1], m_detailCfg.bands[2], m_detailCfg.bands[3]);
    glUniform1f(uMetalThresh, m_detailCfg.metalThresh);
    glUniform1i(uMetalRoute, m_detailCfg.metalRoute);
    glUniform1i(uni(m_prog, "uSsaoOn"), ssOn ? 1 : 0);
    glUniform1f(uni(m_prog, "uSsaoStr"), m_ssaoStr);
    glUniform1f(uni(m_prog, "uSsaoRad"), m_ssaoRad * m_radius * 0.1f);
    glUniform1f(uni(m_prog, "uReflStrength"), m_reflStrength);
    glUniform1f(uni(m_prog, "uSkinWarm"), m_skinWarm);
    glUniform1f(uni(m_prog, "uSssStrength"), m_sssStrength);
    glUniform1f(uni(m_prog, "uWetness"), m_wetness);
    glUniform1f(uni(m_prog, "uSnow"), m_snow);
    glUniform1i(uHasEmissive, 0);
    glUniform1i(uHasDetailN, 0);
    glUniform1f(uDetailNInt, 0.0f);
    glUniform1f(uDetailRInt, 0.0f);
    glUniform1f(uDetailROffset, 0.0f);
    glUniform1f(uDetailColorAdd, 0.0f);
    glUniform3f(uDetailScales, 8.0f, 8.0f, 8.0f);
    glUniform1i(uDetailMetalLayer, -1);
    glUniform1i(uHasDetailR, 0);
    glUniform1i(uHasTrans, 0);
    glUniform1i(uHasMask, 0);
    glUniform1i(uHasDyeMask, 0);
    glUniform1i(uHasDyeRamp, 0);
    glUniform1i(uIsHair, 0);
    glUniform1i(uIsSkin, 0);
    glUniform1i(uIsHead, 0);
    glUniform1i(uIsEye, 0);
    glUniform1f(uni(m_prog, "uEyeRough"), m_eyeRough);
    glUniform1i(uPbr, m_pbr ? 1 : 0);
    glUniform1f(uMetal, 0.0f);
    glUniform1f(uRough, 0.6f);
    auto setTint = [&](bool hot) {
        if (hot) glUniform3f(uBase, 1.0f, 0.55f, 0.12f);   // highlight = orange
        else     glUniform3f(uBase, 0.78f, 0.78f, 0.78f);  // default grey
    };

    // Ground grid (drawn filled regardless of wireframe mode).
    if (m_showGrid) {
        if (m_gridVerts == 0) buildGrid();
        glUniform1i(uHasTex, 0);
        glUniform1i(uHasNormal, 0);
        glUniform1i(uHasDyeMask, 0);
        glUniform1i(uHasDyeRamp, 0);
        glUniform1i(uPbr, 0);   // no specular on the grid lines
        glUniform3f(uBase, 0.34f, 0.34f, 0.38f);
        glBindVertexArray(m_gridVao);
        if (m_gridAxisColors && m_gridVerts - m_gridAxisFirst == 4) {
            // Same VBO, three draws: plain lines, then the X axis red, the Z axis blue.
            glDrawArrays(GL_LINES, 0, m_gridAxisFirst);
            glUniform3f(uBase, 0.72f, 0.27f, 0.25f);
            glDrawArrays(GL_LINES, m_gridAxisFirst, 2);
            glUniform3f(uBase, 0.24f, 0.42f, 0.72f);
            glDrawArrays(GL_LINES, m_gridAxisFirst + 2, 2);
        } else {
            glDrawArrays(GL_LINES, 0, m_gridVerts);
        }
        glBindVertexArray(0);
    }
    glUniform1i(uPbr, m_pbr ? 1 : 0);

    glPolygonMode(GL_FRONT_AND_BACK, m_wireframe ? GL_LINE : GL_FILL);
    // Cull back faces: thin double-walled armor otherwise shows its inner wall
    // poking through the outer wall (shard/patch artifacts). Winding is CCW-front.
    // User-toggleable (Preview Settings); off renders both sides.
    if (m_backfaceCull) { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }
    else glDisable(GL_CULL_FACE);
    glBindVertexArray(m_vao);
    if (m_parts.isEmpty()) {
        glUniform1i(uHasTex, 0);
        glUniform1i(uHasNormal, 0);
        glUniform1i(uHasOrm, 0);
        glUniform1i(uHasEmissive, 0);
        glUniform1i(uHasDyeMask, 0);
        glUniform1i(uHasDyeRamp, 0);
        setTint(false);
        glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
    } else {
        // Alpha-to-coverage (needs the 4x MSAA default format) anti-aliases the per-part alpha
        // cutout — cloth/foliage edges resolve smoothly instead of a hard jagged fringe. Opaque
        // fragments emit coverage 1.0 (cutCov=1), so solid geometry is unaffected.
        glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        for (int i = 0; i < m_parts.size(); ++i) {
            const Part& p = m_parts[i];
            if (!p.visible || p.count == 0) continue;
            // FX submeshes are drawn in the separate blended pass below, never here.
            if (i < m_partFx.size() && m_partFx[i]) continue;
            // NOTE: highlighting no longer forces a flat tint here — it is drawn as an outline
            // after this pass (see the outline block), so the part keeps its real material while
            // selected. `hot` now only suppresses the normal map, which would fight the outline.
            const bool hot = false;
            const GLuint tex = (i < m_partTex.size()) ? m_partTex[i] : 0;
            if (tex == 0 || !m_showTex) {
                glUniform1i(uHasTex, 0);   // untextured / textures-off → flat
                setTint(false);
            } else {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, tex);
                glUniform1i(uHasTex, 1);
            }
            const GLuint ntex = (i < m_partNormTex.size()) ? m_partNormTex[i] : 0;
            if (m_pbr && ntex != 0 && !hot) {
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, ntex);
                glUniform1i(uHasNormal, 1);
            } else {
                glUniform1i(uHasNormal, 0);
            }
            glUniform1f(uMetal, i < m_partMetal.size() ? m_partMetal[i] : 0.0f);
            glUniform1f(uRough, i < m_partRough.size() ? m_partRough[i] : 0.6f);
            const GLuint otex = (i < m_partOrmTex.size()) ? m_partOrmTex[i] : 0;
            if (m_pbr && otex != 0 && !hot) {
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, otex);
                glUniform1i(uHasOrm, 1);
            } else {
                glUniform1i(uHasOrm, 0);
            }
            const GLuint etex = (i < m_partEmisTex.size()) ? m_partEmisTex[i] : 0;
            if (etex != 0 && !hot && m_showTex) {
                glActiveTexture(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_2D, etex);
                glUniform1i(uHasEmissive, 1);
                glUniform1f(uEmisMul, i < m_partEmisMul.size() ? m_partEmisMul[i] : 1.0f);
                if (i*3+2 < m_partEmisColor.size())
                    glUniform3f(uEmisColor, m_partEmisColor[i*3], m_partEmisColor[i*3+1], m_partEmisColor[i*3+2]);
                else
                    glUniform3f(uEmisColor, 1.0f, 1.0f, 1.0f);
            } else {
                glUniform1i(uHasEmissive, 0);
            }
            auto bindExtra = [&](int unit, const QVector<GLuint>& v, GLint has) {
                const GLuint t = (i < v.size()) ? v[i] : 0;
                if (m_pbr && t != 0 && !hot) {
                    glActiveTexture(GL_TEXTURE0 + unit);
                    glBindTexture(GL_TEXTURE_2D, t);
                    glUniform1i(has, 1);
                } else {
                    glUniform1i(has, 0);
                }
            };
            // Detail maps: bind up-to-3 normal (units 17-19) + 3 rough (20-22); the shader picks
            // one per texel by dye-mask region. uHasDetailN/R = any present (for the feature gate).
            const GLint hasN[3] = {uHasDetN0, uHasDetN1, uHasDetN2};
            const GLint hasR[3] = {uHasDetR0, uHasDetR1, uHasDetR2};
            bool anyN = false, anyR = false;
            for (int k = 0; k < 3; ++k) {
                bindExtra(17 + k, m_partDetailNTex[k], hasN[k]);
                bindExtra(20 + k, m_partDetailRTex[k], hasR[k]);
                if (k < 3 && i < m_partDetailNTex[k].size() && m_partDetailNTex[k][i]) anyN = true;
                if (k < 3 && i < m_partDetailRTex[k].size() && m_partDetailRTex[k][i]) anyR = true;
            }
            glUniform1i(uHasDetailN, (m_pbr && anyN && !hot) ? 1 : 0);
            glUniform1i(uHasDetailR, (m_pbr && anyR && !hot) ? 1 : 0);
            glUniform1f(uDetailNInt, (i < m_partDetailNInt.size()) ? m_partDetailNInt[i] : 0.0f);
            glUniform1f(uDetailRInt, (i < m_partDetailRInt.size()) ? m_partDetailRInt[i] : 0.0f);
            glUniform1f(uDetailROffset, (i < m_partDetailROffset.size()) ? m_partDetailROffset[i] : 0.0f);
            glUniform1f(uDetailColorAdd, (i < m_partDetailCAdd.size()) ? m_partDetailCAdd[i] : 0.0f);
            const QVector3D dsc = (i < m_partDetailScales.size()) ? m_partDetailScales[i] : QVector3D(8, 8, 8);
            glUniform3f(uDetailScales, dsc.x(), dsc.y(), dsc.z());
            glUniform1i(uDetailMetalLayer, (i < m_partDetailMetalLayer.size()) ? m_partDetailMetalLayer[i] : -1);
            // Auto (game-data) selection: use this part's detected dye-mask bands + derived zone→map.
            if (m_detailCfg.autoMode) {
                if (i < m_partDyeBandsV.size()) { const QVector4D b = m_partDyeBandsV[i];
                    glUniform4f(uDyeBands, b.x(), b.y(), b.z(), b.w()); }
                if (i < m_partZoneMapV.size()) { const QVector4D z = m_partZoneMapV[i];
                    glUniform4i(uZoneMap, qRound(z.x()), qRound(z.y()), qRound(z.z()), qRound(z.w())); }
            }
            bindExtra(6, m_partTransTex, uHasTrans);
            bindExtra(7, m_partMaskTex, uHasMask);
            auto bindDye = [&](int unit, const QVector<GLuint>& v, GLint has) {
                const GLuint t = (i < v.size()) ? v[i] : 0;   // dye applies in flat mode too
                if (t != 0 && !hot) {
                    glActiveTexture(GL_TEXTURE0 + unit);
                    glBindTexture(GL_TEXTURE_2D, t);
                    glUniform1i(has, 1);
                } else {
                    glUniform1i(has, 0);
                }
            };
            bindDye(8, m_partDyeMaskTex, uHasDyeMask);
            bindDye(9, m_partDyeRampTex, uHasDyeRamp);
            glUniform1i(uDyeRegion, (i < m_partDyeRegion.size()) ? m_partDyeRegion[i] : 0);
            // Per-part dye override (per-slot pigments). When this part carries its own dye,
            // enable dye + push its 4 colours for this draw; otherwise fall back to the global
            // dye state already set before the loop.
            if (i < m_partDyeOn.size()) {
                const bool on = m_partDyeOn[i] != 0;
                glUniform1i(uFDye, on ? 1 : 0);
                if (on && (i * 12 + 11) < m_partDyeColor.size())
                    glUniform3fv(uDyeColor, 4, &m_partDyeColor[i * 12]);
            } else {
                glUniform1i(uFDye, m_fDye ? 1 : 0);
                glUniform3fv(uDyeColor, 4, &m_dyeColor[0][0]);
            }
            const int partIsHair = (!hot && i < m_partHair.size()) ? m_partHair[i] : 0;
            glUniform1i(uIsHair, partIsHair);
            if (partIsHair && (i * 3 + 2) < m_partHairParams.size())
                glUniform3f(uHairParams, m_partHairParams[i*3+0], m_partHairParams[i*3+1], m_partHairParams[i*3+2]);
            else
                glUniform3f(uHairParams, 0.50f, 0.30f, 0.0f);   // data-driven fallback (roughFactor≈0.5, spec 0.3, no shift)
            glUniform1i(uIsSkin, (!hot && i < m_partSkin.size()) ? m_partSkin[i] : 0);
            glUniform1i(uIsHead, (!hot && i < m_partHead.size()) ? m_partHead[i] : 0);
            glUniform1i(uIsCloth, (!hot && i < m_partCloth.size()) ? m_partCloth[i] : 0);
            glUniform1i(uIsEye, (!hot && i < m_partEye.size()) ? m_partEye[i] : 0);
            // Hair cards are two-sided alpha sheets — don't cull them, or half vanishes.
            if (partIsHair && !m_wireframe) glDisable(GL_CULL_FACE);
            glDrawElements(GL_TRIANGLES, p.count, GL_UNSIGNED_INT,
                           reinterpret_cast<void*>(qintptr(p.offset) * sizeof(quint32)));
            if (partIsHair && !m_wireframe && m_backfaceCull) { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }

            // Shell fur: redraw this part as m_furShells concentric, normal-extruded layers.
            // The base color / normal / dye textures stay bound from above, so the fur is lit
            // and dyed consistently; we add the density mask (unit 12) + strand noise (unit 11).
            const bool furPart = m_furEnabled && !hot && m_showTex && m_pbr
                                 && i < m_partFur.size() && m_partFur[i];
            if (furPart && !m_wireframe) {
                const GLuint fmask  = (i < m_partFurMaskTex.size())  ? m_partFurMaskTex[i]  : 0;
                const GLuint fnoise = (i < m_partFurNoiseTex.size()) ? m_partFurNoiseTex[i] : 0;
                if (fmask != 0) {
                    glActiveTexture(GL_TEXTURE11);
                    glBindTexture(GL_TEXTURE_2D, fnoise ? fnoise : fmask);
                    glActiveTexture(GL_TEXTURE12);
                    glBindTexture(GL_TEXTURE_2D, fmask);
                    glUniform1i(uHasFurNoise, fnoise ? 1 : 0);
                    glUniform1i(uFurEnabled, 1);
                    // Alpha-to-coverage (enabled for the whole pass above) softly tapers strand tips.
                    for (int s = 1; s <= m_furShells; ++s) {
                        glUniform1f(uFurShell, float(s) / float(m_furShells));
                        glDrawElements(GL_TRIANGLES, p.count, GL_UNSIGNED_INT,
                                       reinterpret_cast<void*>(qintptr(p.offset) * sizeof(quint32)));
                    }
                    glUniform1i(uFurEnabled, 0);
                    glUniform1f(uFurShell, 0.0f);
                    glActiveTexture(GL_TEXTURE0);
                }
            }
        }
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);   // end alpha-cutout pass

        // ── Selection SILHOUETTE pass ─────────────────────────────────────────────────────
        // Selected parts keep their real material (a flat tint hid the textures you are trying
        // to inspect). Instead the part is stamped into the STENCIL buffer, then redrawn as a
        // flat colour offset by a few screen pixels in eight directions with the stencil test
        // rejecting the interior — what survives is a constant-width ring hugging the outline.
        //
        // This replaces a wireframe overlay, whose apparent thickness scaled with triangle
        // density: on a high-poly cape every edge drew and the "outline" read as a solid mesh.
        // Screen-space offsets also avoid the normal-push hull's failure mode — D4 meshes have
        // split normals at UV seams, which would tear such a hull open.
        //
        // Depth test stays OFF, so a part buried inside the body still shows.
        // RED  = parts-list / programmatic highlight.
        // BLUE = the part you right-clicked in the viewport; drawn last so it wins on overlap.
        {
            QVector<QPair<int, int>> outline;   // (part, 0 = red, 1 = blue)
            for (int i : m_highlight)
                if (i >= 0 && i < m_parts.size() && m_parts[i].visible && m_parts[i].count)
                    outline.push_back({i, 0});
            if (m_pickedPart >= 0 && m_pickedPart < m_parts.size()
                && m_parts[m_pickedPart].visible && m_parts[m_pickedPart].count) {
                for (int k = outline.size() - 1; k >= 0; --k)
                    if (outline[k].first == m_pickedPart) outline.removeAt(k);
                outline.push_back({m_pickedPart, 1});
            }
            // No stencil attachment → the stencil test always passes and the jittered draws would
            // paint eight solid copies of the part. Fall back to the old wireframe in that case.
            // Ask the BOUND framebuffer, not the requested surface format: QOpenGLWidget renders
            // into its own FBO, and a driver may hand back a context whose format differs from
            // what main.cpp asked for. The OBJECT_TYPE query is legal with no attachment present,
            // so it answers without pushing GL_INVALID_OPERATION onto the error queue.
            if (!outline.isEmpty()) {
                // Queried inside the guard: a glGet is a driver round-trip, and nothing is
                // selected on most frames.
                // The attachment enum DIFFERS by target: GL_STENCIL is only legal for the default
                // framebuffer, GL_STENCIL_ATTACHMENT only for a named one. QOpenGLWidget normally
                // renders into its own FBO, so asking for GL_STENCIL there raises
                // GL_INVALID_OPERATION and leaves the result untouched — which reads as "no
                // stencil" and silently disables this whole pass.
                GLint drawFbo = 0;
                glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
                GLint stencilAttach = GL_NONE;
                glGetFramebufferAttachmentParameteriv(
                    GL_DRAW_FRAMEBUFFER, drawFbo == 0 ? GL_STENCIL : GL_STENCIL_ATTACHMENT,
                    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &stencilAttach);
                const bool haveStencil = (stencilAttach != GL_NONE) && m_fbW > 0 && m_fbH > 0;
                glDisable(GL_DEPTH_TEST);
                glDepthMask(GL_FALSE);        // never write depth: later FX depth-test against the mesh
                glDisable(GL_CULL_FACE);      // stamp the whole projected shape, front and back faces
                glDisable(GL_BLEND);
                glUniform1i(uHasNormal, 0); glUniform1i(uHasEmissive, 0);
                glUniform1i(uHasDyeMask, 0); glUniform1i(uHasDyeRamp, 0); glUniform1i(uPbr, 0);
                // uSolid short-circuits the fragment shader but NOT the vertex shader: leftover fur
                // extrusion would puff the stamped footprint away from the real mesh silhouette.
                glUniform1i(uFurEnabled, 0); glUniform1f(uFurShell, 0.0f);

                // The base texture stays bound so the shader can honour the alpha cutout: without
                // it the outline of hair, fur trim and cut-out cloth traces the card RECTANGLES
                // rather than the strands you can actually see.
                auto drawParts = [&](int colour) {
                    for (const auto& o : outline) {
                        if (o.second != colour) continue;
                        const int idx = o.first;
                        const GLuint tex = (idx < m_partTex.size()) ? m_partTex[idx] : 0;
                        if (tex && m_showTex) {
                            glActiveTexture(GL_TEXTURE0);
                            glBindTexture(GL_TEXTURE_2D, tex);
                            glUniform1i(uHasTex, 1);
                            glUniform1i(uIsHair, (idx < m_partHair.size()) ? m_partHair[idx] : 0);
                        } else {
                            glUniform1i(uHasTex, 0);
                        }
                        const Part& p = m_parts[idx];
                        glDrawElements(GL_TRIANGLES, p.count, GL_UNSIGNED_INT,
                                       reinterpret_cast<void*>(qintptr(p.offset) * sizeof(quint32)));
                    }
                };

                if (haveStencil) {
                    glUniform1i(uSolid, 1);
                    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);   // wireframe mode would otherwise
                    glEnable(GL_STENCIL_TEST);                   // stamp only the triangle edges
                    glStencilMask(0xFF);
                    // Outline half-width in DEVICE pixels, so it looks the same on a hi-dpi display.
                    const float r = 2.0f * float(devicePixelRatioF());
                    const float sx = 2.0f * r / float(m_fbW);
                    const float sy = 2.0f * r / float(m_fbH);
                    static const float kDir[8][2] = {
                        { 1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f,  1.0f}, { 0.0f, -1.0f},
                        { 0.707f, 0.707f}, {-0.707f, 0.707f}, {0.707f, -0.707f}, {-0.707f, -0.707f},
                    };
                    // Red first, blue second: each group gets a fresh mask, so blue paints over red
                    // where two selected parts overlap on screen.
                    for (int colour = 0; colour <= 1; ++colour) {
                        bool any = false;
                        for (const auto& o : outline) if (o.second == colour) { any = true; break; }
                        if (!any) continue;
                        glClear(GL_STENCIL_BUFFER_BIT);
                        // Pass A — stamp the part's screen footprint into stencil, colour off.
                        glStencilFunc(GL_ALWAYS, 1, 0xFF);
                        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                        glUniform2f(uNdcOffset, 0.0f, 0.0f);
                        drawParts(colour);
                        // Pass B — ring: only outside the stamped footprint. Overlapping jitters
                        // rewrite the same opaque colour, so no stencil bookkeeping is needed.
                        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
                        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
                        if (colour == 1) glUniform3f(uBase, 0.25f, 0.60f, 1.00f);   // blue = picked
                        else             glUniform3f(uBase, 1.00f, 0.15f, 0.15f);   // red  = highlight
                        for (const auto& d : kDir) {
                            glUniform2f(uNdcOffset, d[0] * sx, d[1] * sy);
                            drawParts(colour);
                        }
                    }
                    glUniform2f(uNdcOffset, 0.0f, 0.0f);
                    glUniform1i(uSolid, 0);
                    glDisable(GL_STENCIL_TEST);
                    glPolygonMode(GL_FRONT_AND_BACK, m_wireframe ? GL_LINE : GL_FILL);
                } else {
                    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                    glLineWidth(1.0f);
                    for (const auto& o : outline) {
                        if (o.second == 1) glUniform3f(uBase, 0.25f, 0.60f, 1.00f);
                        else               glUniform3f(uBase, 1.00f, 0.15f, 0.15f);
                        const Part& p = m_parts[o.first];
                        glDrawElements(GL_TRIANGLES, p.count, GL_UNSIGNED_INT,
                                       reinterpret_cast<void*>(qintptr(p.offset) * sizeof(quint32)));
                    }
                    glPolygonMode(GL_FRONT_AND_BACK, m_wireframe ? GL_LINE : GL_FILL);
                }
                glEnable(GL_CULL_FACE);
                glDepthMask(GL_TRUE);
                glEnable(GL_DEPTH_TEST);
                glUniform1i(uHasTex, 0);            // the cutout bind above left these per-part;
                glUniform1i(uIsHair, 0);            // the FX pass does not set uIsHair itself
                glUniform1i(uPbr, m_pbr ? 1 : 0);   // restore for later passes
            }
        }

        // ── Mesh FX pass: visible FX submeshes, unlit + alpha-blended + scrolling, two-sided. ──
        bool anyFx = false;
        for (int i = 0; i < m_parts.size() && !anyFx; ++i)
            anyFx = m_parts[i].visible && m_parts[i].count && i < m_partFx.size() && m_partFx[i];
        if (anyFx && m_showTex && !m_wireframe) {
            glUniform1i(uFxMode, 1);
            glEnable(GL_BLEND);
            glDepthMask(GL_FALSE);          // transparent: depth-test against opaque, don't write
            glDisable(GL_CULL_FACE);        // two-sided cards
            drawFxParts(false);             // per-part add/alpha blend
            glUniform1i(uFxMode, 0);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            glActiveTexture(GL_TEXTURE0);
        }
        // Keep repainting while FX are on screen so they animate.
        const bool wantAnim = m_showTex && anyFx;
        if (wantAnim) {
            if (!m_fxTimer) {
                m_fxTimer = new QTimer(this);
                connect(m_fxTimer, &QTimer::timeout, this, [this]{ update(); });
            }
            if (!m_fxTimer->isActive()) m_fxTimer->start(33);   // ~30 fps
        } else if (m_fxTimer && m_fxTimer->isActive()) {
            m_fxTimer->stop();
        }
    }
    glBindVertexArray(0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);   // restore
    glDisable(GL_CULL_FACE);                      // restore (only enabled for wireframe)

    // Skeleton overlay: bone lines drawn over the mesh (depth test off).
    if (m_showSkeleton) {
        if (m_skelVerts == 0) buildSkeleton();
        if (m_skelVerts > 0) {
            glDisable(GL_DEPTH_TEST);
            glUniform1i(uHasTex, 0);
            glUniform1i(uHasNormal, 0);
            glUniform1i(uHasEmissive, 0);
            glUniform1i(uHasDyeMask, 0);
            glUniform1i(uHasDyeRamp, 0);
            glUniform1i(uPbr, 0);
            glUniform3f(uBase, 0.20f, 1.0f, 0.55f);   // bright green bones
            glBindVertexArray(m_skelVao);
            glDrawArrays(GL_LINES, 0, m_skelVerts);
            glBindVertexArray(0);
            glEnable(GL_DEPTH_TEST);
        }
    }

    // Hardpoint overlay: an RGB axis gizmo at each rig attach socket (weapon grips, sheaths,
    // trail emitters, look-at…), pose-aware so it follows animation. X red, Y green, Z blue.
    if (m_showHardpoints) {
        if (m_hpAxisVerts == 0) buildHardpoints();
        if (m_hpAxisVerts > 0) {
            glDisable(GL_DEPTH_TEST);
            glUniform1i(uHasTex, 0); glUniform1i(uHasNormal, 0); glUniform1i(uHasEmissive, 0);
            glUniform1i(uHasDyeMask, 0); glUniform1i(uHasDyeRamp, 0); glUniform1i(uPbr, 0);
            glBindVertexArray(m_hpVao);
            const int n = m_hpAxisVerts;
            glUniform3f(uBase, 1.0f, 0.25f, 0.25f); glDrawArrays(GL_LINES, 0,       n);   // X red
            glUniform3f(uBase, 0.30f, 1.0f, 0.35f); glDrawArrays(GL_LINES, n,       n);   // Y green
            glUniform3f(uBase, 0.35f, 0.55f, 1.0f); glDrawArrays(GL_LINES, 2 * n,   n);   // Z blue
            glBindVertexArray(0);
            glEnable(GL_DEPTH_TEST);
        }
    }

    // Physics-bone overlay: simulated cloth bones (orange connections) + per-bone XYZ axis
    // gizmos (X red, Y green, Z blue) oriented by the live simulated rotation.
    if (m_showPhysBones) {
        if (m_physVerts == 0) buildPhysBones();
        if (m_physVerts > 0) {
            glDisable(GL_DEPTH_TEST);
            glUniform1i(uHasTex, 0);
            glUniform1i(uHasNormal, 0);
            glUniform1i(uHasEmissive, 0);
            glUniform1i(uHasDyeMask, 0);
            glUniform1i(uHasDyeRamp, 0);
            glUniform1i(uPbr, 0);
            glBindVertexArray(m_physVao);
            const int pin = m_physPinnedVerts, conn = m_physConnVerts, ax = m_physAxisVerts;
            const int fre = m_physFreeVerts, tch = m_physTouchVerts;
            // Connection lines, coloured by state so collision problems are visible at a glance:
            //   grey   = anchored/kinematic (pins + chain roots)
            //   orange = free, no contact
            //   yellow = resting on the body (resolved contact — normal)
            //   red    = penetrating deeply (wedged inside a capsule — the thing to chase)
            glUniform3f(uBase, 0.55f, 0.58f, 0.62f); glDrawArrays(GL_LINES, 0,   pin);          // pinned
            glUniform3f(uBase, 1.0f,  0.55f, 0.12f); glDrawArrays(GL_LINES, pin, fre - pin);    // free
            glUniform3f(uBase, 1.0f,  0.90f, 0.20f); glDrawArrays(GL_LINES, fre, tch - fre);    // touching
            glUniform3f(uBase, 1.0f,  0.18f, 0.18f); glDrawArrays(GL_LINES, tch, conn - tch);   // deep
            if (ax > 0 && m_showPhysAxes) {
                glUniform3f(uBase, 1.0f, 0.20f, 0.20f); glDrawArrays(GL_LINES, conn,            ax);   // X red
                glUniform3f(uBase, 0.25f, 1.0f, 0.25f); glDrawArrays(GL_LINES, conn + ax,       ax);   // Y green
                glUniform3f(uBase, 0.30f, 0.55f, 1.0f); glDrawArrays(GL_LINES, conn + 2 * ax,   ax);   // Z blue
            }
            glBindVertexArray(0);
            glEnable(GL_DEPTH_TEST);
        }
    }

    // Collision-capsule overlay (cloth-physics debug): rebuilt each frame (they animate).
    if (m_showColliders) {
        buildColliderLines();
        if (m_colLineVerts > 0) {
            glDisable(GL_DEPTH_TEST);
            glUniform1i(uHasTex, 0); glUniform1i(uHasNormal, 0); glUniform1i(uHasEmissive, 0);
            glUniform1i(uHasDyeMask, 0); glUniform1i(uHasDyeRamp, 0); glUniform1i(uPbr, 0);
            glBindVertexArray(m_colVao);
            // One draw per BODY REGION, each in its own colour, so the region sliders are
            // visually attributable and an all-"Other" rig is obvious at a glance.
            // legs, waist, torso, arms, head, other
            static const float kRegionRGB[ClothParams::CapRegionCount][3] = {
                {0.25f, 0.75f, 1.00f},   // legs   — cyan
                {1.00f, 0.80f, 0.20f},   // waist  — amber
                {0.45f, 0.95f, 0.45f},   // torso  — green
                {1.00f, 0.45f, 0.85f},   // arms   — pink
                {0.75f, 0.60f, 1.00f},   // head   — violet
                {1.00f, 0.55f, 0.15f},   // other  — the original orange
            };
            int first = 0;
            for (int r = 0; r < ClothParams::CapRegionCount; ++r) {
                const int end = (r < m_colRegionSpan.size()) ? m_colRegionSpan[r] : m_colLineVerts;
                if (end > first) {
                    glUniform3f(uBase, kRegionRGB[r][0], kRegionRGB[r][1], kRegionRGB[r][2]);
                    glDrawArrays(GL_LINES, first, end - first);
                }
                first = qMax(first, end);
            }
            if (first < m_colLineVerts) {        // safety: draw any ungrouped remainder
                glUniform3f(uBase, 1.0f, 0.55f, 0.15f);
                glDrawArrays(GL_LINES, first, m_colLineVerts - first);
            }
            glBindVertexArray(0);
            glEnable(GL_DEPTH_TEST);
        }
    }

    // Bone-name labels: a transparent QPainter overlay child, projected with m_lastViewProj. Called
    // when on, or once more after turning off (to hide + clear the overlay).
    if (m_showBoneNames || m_boneLabels) updateBoneLabels();
    if (m_showHardpoints || m_hpLabels) updateHardpointLabels();
    // The axis gizmo tracks yaw/pitch — repaint it alongside every frame (88px, trivial).
    if (m_gizmo && m_gizmo->isVisible()) m_gizmo->update();
}

int GLModelWidget::partTriangles(int i) const
{
    return (i >= 0 && i < m_parts.size()) ? m_parts[i].count / 3 : 0;
}

void GLModelWidget::setHighlightPart(int i)
{
    QSet<int> s;
    if (i >= 0 && i < m_parts.size()) s.insert(i);
    if (s != m_highlight) { m_highlight = s; update(); }
}

void GLModelWidget::setPickedPart(int i)
{
    const int v = (i >= 0 && i < m_parts.size()) ? i : -1;
    if (v != m_pickedPart) { m_pickedPart = v; update(); }
}

void GLModelWidget::setHighlightParts(const QList<int>& parts)
{
    QSet<int> s;
    for (int i : parts)
        if (i >= 0 && i < m_parts.size()) s.insert(i);
    if (s != m_highlight) { m_highlight = s; update(); }
}

void GLModelWidget::setPartTextures(const QVector<QImage>& baseColor)
{
    m_pendingTex = baseColor;
    m_hasPendingTex = true;
    update();
}

void GLModelWidget::setPartNormals(const QVector<QImage>& normalMaps)
{
    m_pendingNorm = normalMaps;
    m_hasPendingTex = true;
    update();
}

void GLModelWidget::setPartOrm(const QVector<QImage>& orm)
{
    m_pendingOrm = orm;
    m_hasPendingTex = true;
    update();
}

void GLModelWidget::setPartEmissive(const QVector<QImage>& emissive)
{
    m_pendingEmis = emissive;
    m_hasPendingTex = true;
    update();
}

void GLModelWidget::setPartDetailNormals(const QVector<QImage>& m0, const QVector<QImage>& m1,
                                         const QVector<QImage>& m2)
{ m_pendingDetailN[0] = m0; m_pendingDetailN[1] = m1; m_pendingDetailN[2] = m2;
  m_hasPendingTex = true; update(); }
void GLModelWidget::setPartDetailRoughs(const QVector<QImage>& m0, const QVector<QImage>& m1,
                                        const QVector<QImage>& m2)
{ m_pendingDetailR[0] = m0; m_pendingDetailR[1] = m1; m_pendingDetailR[2] = m2;
  m_hasPendingTex = true; update(); }
void GLModelWidget::setPartTranslucency(const QVector<QImage>& maps)
{ m_pendingTrans = maps; m_hasPendingTex = true; update(); }
void GLModelWidget::setPartMask(const QVector<QImage>& maps)
{ m_pendingMask = maps; m_hasPendingTex = true; update(); }
void GLModelWidget::setPartDyeMask(const QVector<QImage>& maps)
{ m_pendingDyeMask = maps; m_hasPendingTex = true; update(); }
void GLModelWidget::setPartDyeRamp(const QVector<QImage>& maps)
{ m_pendingDyeRamp = maps; m_hasPendingTex = true; update(); }
void GLModelWidget::setPartDyeRegion(const QVector<int>& region)
{ m_partDyeRegion = region; update(); }
void GLModelWidget::setPartDye(const QVector<int>& on, const QVector<float>& colors12)
{ m_partDyeOn = on; m_partDyeColor = colors12; update(); }
// (setDyeGradient / setDyeMode removed — no caller ever fed the real-dye gradient path; dyes
//  recolour through setPartDye. m_dyeMode stays 0, so the shader's uDyeMode branch is inert.)
void GLModelWidget::setPartFlags(const QVector<int>& hair, const QVector<int>& skin,
                                 const QVector<int>& cloth)
{ m_partHair = hair; m_partSkin = skin; m_partCloth = cloth; m_clothBuilt = false; update(); }

void GLModelWidget::setPartHairParams(const QVector<float>& p)
{ m_partHairParams = p; update(); }

void GLModelWidget::setPartFx(const QVector<int>& fx)
{ m_partFx = fx; m_clothBuilt = false; update(); }
void GLModelWidget::setPartFxNoise(const QVector<QImage>& maps)
{ m_pendingFxNoise = maps; m_hasPendingTex = true; update(); }
// Draw every visible FX submesh with the main program in FX mode (uFxMode must already be 1,
// blend enabled, depth-write off, culling off). The on-screen FX pass calls it; forceAdditive forces additive blend for accumulation.
void GLModelWidget::drawFxParts(bool forceAdditive)
{
    const GLint uHasTex     = uni(m_prog, "uHasTex");
    const GLint uHasFxNoise = uni(m_prog, "uHasFxNoise");
    const GLint li = uni(m_prog, "uFxIntensity");
    const GLint lw = uni(m_prog, "uFxWobble");
    const GLint lf = uni(m_prog, "uFxFresnel");
    const GLint la = uni(m_prog, "uFxAlpha");
    const GLint ls = uni(m_prog, "uFxSaturation");
    for (int i = 0; i < m_parts.size(); ++i) {
        const Part& p = m_parts[i];
        if (!p.visible || !p.count || i >= m_partFx.size() || !m_partFx[i]) continue;
        const bool add = forceAdditive || (i < m_partFxAdditive.size() && m_partFxAdditive[i]);
        glBlendFunc(GL_SRC_ALPHA, add ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
        const float aInt = (i < m_partFxIntensity.size())  ? m_partFxIntensity[i]  : 1.5f;
        const float aWob = (i < m_partFxWobble.size())     ? m_partFxWobble[i]     : 0.0f;
        const float aFre = (i < m_partFxFresnel.size())    ? m_partFxFresnel[i]    : 1.6f;
        const float aAlp = (i < m_partFxAlpha.size())      ? m_partFxAlpha[i]      : 1.0f;
        const float aSat = (i < m_partFxSaturation.size()) ? m_partFxSaturation[i] : 1.0f;
        glUniform1f(li, aInt * m_fxIntensity);
        glUniform1f(lw, aWob * 0.025f * m_radius * m_fxWobble);
        glUniform1f(lf, qBound(0.05f, aFre, 4.0f));
        glUniform1f(la, qBound(0.0f, aAlp, 4.0f));
        glUniform1f(ls, qBound(0.0f, aSat, 2.0f));
        const GLuint tex = (i < m_partTex.size()) ? m_partTex[i] : 0;
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex); glUniform1i(uHasTex, tex ? 1 : 0);
        const GLuint fxn = (i < m_partFxNoiseTex.size()) ? m_partFxNoiseTex[i] : 0;
        glActiveTexture(GL_TEXTURE13); glBindTexture(GL_TEXTURE_2D, fxn); glUniform1i(uHasFxNoise, fxn ? 1 : 0);
        glDrawElements(GL_TRIANGLES, p.count, GL_UNSIGNED_INT,
                       reinterpret_cast<void*>(qintptr(p.offset) * sizeof(quint32)));
    }
    glActiveTexture(GL_TEXTURE0);
}

void GLModelWidget::setPartFxAdditive(const QVector<int>& add) { m_partFxAdditive = add; update(); }
void GLModelWidget::setPartFxParams(const QVector<float>& intensity, const QVector<float>& wobble,
                                    const QVector<float>& fresnel, const QVector<float>& alpha,
                                    const QVector<float>& saturation)
{ m_partFxIntensity = intensity; m_partFxWobble = wobble; m_partFxFresnel = fresnel;
  m_partFxAlpha = alpha; m_partFxSaturation = saturation; update(); }
void GLModelWidget::setFxIntensity(float v)   { m_fxIntensity = qBound(0.0f, v, 4.0f);  update(); }
void GLModelWidget::setFxScrollSpeed(float v) { m_fxScrollSpeed = qBound(0.0f, v, 4.0f); update(); }
void GLModelWidget::setFxWobble(float v)      { m_fxWobble = qBound(0.0f, v, 4.0f);     update(); }

void GLModelWidget::setFeatureDetail(bool on)     { if (m_fDetail  != on) { m_fDetail = on;  update(); } }
void GLModelWidget::setFeatureSpecAA(bool on)     { if (m_fSpecAA  != on) { m_fSpecAA = on;  update(); } }
void GLModelWidget::setFeatureSubsurface(bool on) { if (m_fSubsurf != on) { m_fSubsurf = on; update(); } }
void GLModelWidget::setFeatureHair(bool on)       { if (m_fHair    != on) { m_fHair = on;    update(); } }
void GLModelWidget::setFeatureIbl(bool on)        { if (m_fIbl     != on) { m_fIbl = on;     update(); } }
void GLModelWidget::setFeatureMask(bool on)       { if (m_fMask    != on) { m_fMask = on;    update(); } }
void GLModelWidget::setFeatureTonemap(bool on)    { if (m_fTonemap != on) { m_fTonemap = on; update(); } }
void GLModelWidget::setFeatureDye(bool on)        { if (m_fDye     != on) { m_fDye = on;     update(); } }
void GLModelWidget::setDyeColor(int region, const QColor& c)
{
    if (region < 0 || region > 3) return;
    m_dyeColor[region][0] = float(c.redF());
    m_dyeColor[region][1] = float(c.greenF());
    m_dyeColor[region][2] = float(c.blueF());
    update();
}
void GLModelWidget::setColorGrade(bool on, float contrast, float sat, float warmth) {
    m_colorGrade = on;
    m_cgContrast = qBound(0.5f, contrast, 2.0f);
    m_cgSat      = qBound(0.0f, sat, 2.0f);
    m_cgWarmth   = qBound(0.0f, warmth, 0.3f);
    update();
}
// Queue a D4 colour-grade LUT (expects the 16³ → 256×16 unwrapped strip). The actual GL upload is
// deferred to the next paint (the context may not be current when this is called during load). Any
// other size is rejected so a mis-picked texture can't corrupt the image (stylised grade is used).
void GLModelWidget::setColorGradeLut(const QImage& lut) {
    if (lut.isNull() || lut.width() != 256 || lut.height() != 16) {
        m_pendingLut = QImage();      // not a LUT → clear on next paint
        m_hasLut = false;
    } else {
        m_pendingLut = lut.convertToFormat(QImage::Format_RGBA8888);
    }
    m_lutDirty = true;
    update();
}
void GLModelWidget::setEnvironment(int preset)
{
    // Environment now controls ONLY the hemisphere-ambient gradient; the key/rim/fill
    // lights belong to the Lighting rig (applyRig), so changing Environment no longer
    // clobbers the rig's key colour. {sky, horizon, ground}
    static const float P[4][3][3] = {
        {{0.55f,0.55f,0.57f},{0.50f,0.50f,0.51f},{0.42f,0.42f,0.43f}},   // 0 Studio
        {{0.42f,0.48f,0.60f},{0.30f,0.30f,0.33f},{0.12f,0.11f,0.10f}},   // 1 Outdoor
        {{0.20f,0.16f,0.13f},{0.13f,0.11f,0.09f},{0.05f,0.04f,0.03f}},   // 2 Dungeon (warm)
        {{0.10f,0.13f,0.22f},{0.06f,0.08f,0.13f},{0.03f,0.03f,0.05f}},   // 3 Night (cool)
    };
    const int p = qBound(0, preset, 3);
    for (int i = 0; i < 3; ++i) {
        m_envSky[i] = P[p][0][i]; m_envHor[i] = P[p][1][i]; m_envGnd[i] = P[p][2][i];
    }
    update();
}
void GLModelWidget::setExposure(float v)          { m_exposure = v; update(); }

// Recompute the key/rim/fill colours + ambient scale from the chosen rig preset.
// Colours are Diablo IV's real FrontEnd_CharacterCreate authored values (sRGB 0..1):
//   key  = Fire_Spotlight (255,157,87) warm campfire
//   rim  = Rim            (196,250,255) cool cyan
//   fill = Fill_Front     (229,250,255) cool white
// The BASE scales place the key near the previous default brightness; rim/fill sit under it.
void GLModelWidget::applyRig()
{
    static const float K[3][3][3] = {
        // key                       rim                        fill
        {{1.000f,0.616f,0.341f}, {0.769f,0.980f,1.000f}, {0.898f,0.980f,1.000f}},  // 0 D4 Wardrobe (campfire)
        {{1.000f,1.000f,1.000f}, {0.800f,0.850f,1.000f}, {1.000f,1.000f,1.000f}},  // 1 Hero Direct (neutral)
        {{0.950f,0.970f,1.000f}, {0.750f,0.900f,1.000f}, {0.850f,0.920f,1.000f}},  // 2 Studio (cool 3-point)
    };
    // Hemisphere-ambient gradient per preset {sky, horizon, ground}. Folds in what the old
    // Environment dropdown did: campfire = warm fire-bounce from below; the others neutral/cool.
    static const float E[3][3][3] = {
        {{0.10f,0.12f,0.16f}, {0.17f,0.13f,0.11f}, {0.22f,0.12f,0.07f}},  // 0 campfire (warm low bounce)
        {{0.30f,0.32f,0.36f}, {0.26f,0.26f,0.27f}, {0.14f,0.13f,0.12f}},  // 1 neutral grey
        {{0.34f,0.40f,0.52f}, {0.26f,0.28f,0.32f}, {0.12f,0.12f,0.14f}},  // 2 cool studio
    };
    const int p = qBound(0, m_rig.preset, 2);
    const float KEY_BASE = 3.0f, RIM_BASE = 2.2f, FILL_BASE = 1.1f;
    for (int i = 0; i < 3; ++i) {
        m_lightCol[i] = K[p][0][i] * m_rig.keyInt  * KEY_BASE;
        m_rimCol[i]   = K[p][1][i] * m_rig.rimInt  * RIM_BASE;
        m_fillCol[i]  = K[p][2][i] * m_rig.fillInt * FILL_BASE;
        m_envSky[i]   = E[p][0][i];
        m_envHor[i]   = E[p][1][i];
        m_envGnd[i]   = E[p][2][i];
    }
    m_ambScale = m_rig.ambInt;
}
void GLModelWidget::setLightRig(const LightRig& r) { m_rig = r; applyRig(); update(); }

void GLModelWidget::setReflectionEnabled(bool on) { if (m_reflEnabled != on) { m_reflEnabled = on; update(); } }
void GLModelWidget::setReflectionStrength(float v) { v = qMax(0.0f, v); if (m_reflStrength != v) { m_reflStrength = v; update(); } }
void GLModelWidget::setSkinWarmth(float v)         { v = qMax(0.0f, v); if (m_skinWarm != v)     { m_skinWarm = v;     update(); } }
void GLModelWidget::setSssStrength(float v)        { v = qBound(0.0f, v, 2.0f); if (m_sssStrength != v) { m_sssStrength = v; update(); } }
void GLModelWidget::setWetness(float v)            { v = qBound(0.0f, v, 1.0f); if (m_wetness != v) { m_wetness = v; update(); } }
void GLModelWidget::setSnow(float v)               { v = qBound(0.0f, v, 1.0f); if (m_snow != v) { m_snow = v; update(); } }
void GLModelWidget::setShadowEnabled(bool on)      { if (m_shadowOn != on) { m_shadowOn = on; update(); } }
void GLModelWidget::setShadowParams(float strength, float softness, float bias)
{
    m_shadowStr  = qBound(0.0f, strength, 1.0f);
    m_shadowSoft = qMax(0.0f, softness);
    m_shadowBias = qBound(0.0f, bias, 0.02f);
    update();
}
void GLModelWidget::setShadowExtra(float rangeMul, float normalBiasFrac, int resolution)
{
    m_shadowRange = qBound(1.0f, rangeMul, 4.0f);
    m_shadowNBias = qMax(0.0f, normalBiasFrac);
    int r = qBound(512, resolution, 4096);
    r = (r / 512) * 512;                              // snap to a multiple of 512
    if (r != m_shadowSize) { m_shadowSize = r; m_shadowResDirty = true; }
    update();
}
void GLModelWidget::setLightLock(bool worldFixed)  { if (m_lightLock != worldFixed) { m_lightLock = worldFixed; if (worldFixed) m_lockValid = false; update(); } }
void GLModelWidget::setSsaoEnabled(bool on)        { if (m_ssaoOn != on) { m_ssaoOn = on; update(); } }
void GLModelWidget::setSsaoParams(float strength, float radius)
{
    m_ssaoStr = qBound(0.0f, strength, 4.0f);
    m_ssaoRad = qMax(0.001f, radius);
    update();
}

void GLModelWidget::setReflectionCubemap(const QByteArray& payload, int faceSize,
                                         const QVector<quint32>& faceOffsets)
{
    if (payload.isEmpty() || faceSize <= 0 || faceOffsets.size() < 6) {
        m_pendingRefl.clear(); m_reflOffsets.clear(); m_reflSize = 0; m_hasPendingRefl = false;
        return;
    }
    m_pendingRefl    = payload;
    m_reflSize       = faceSize;
    m_reflOffsets    = faceOffsets;
    m_hasPendingRefl = true;
    update();   // uploaded on the next paint (needs a current GL context)
}

// Upload the pending reflection probe as a GL_TEXTURE_CUBE_MAP (RGBA16F HDR). We take each
// face's top mip (128² — unpadded, so no row-stride handling) and let the GPU build the
// roughness mip chain. Any inconsistency leaves m_reflCube at 0 → the shader keeps the
// analytic hemisphere, so a bad/absent probe never breaks rendering.
void GLModelWidget::uploadReflectionCubemap()
{
    m_hasPendingRefl = false;
    const int sz = m_reflSize;
    if (m_pendingRefl.isEmpty() || m_reflOffsets.size() < 6 || sz <= 0) return;
    const qint64 faceBytes = qint64(sz) * sz * 8;   // RGBA16F (8 bytes/texel), top mip
    for (int f = 0; f < 6; ++f)
        if (qint64(m_reflOffsets[f]) + faceBytes > m_pendingRefl.size()) return;   // bad layout → fallback
    if (m_reflCube == 0) glGenTextures(1, &m_reflCube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_reflCube);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    for (int f = 0; f < 6; ++f) {
        const void* p = m_pendingRefl.constData() + m_reflOffsets[f];
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA16F, sz, sz, 0,
                     GL_RGBA, GL_HALF_FLOAT, p);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    m_reflMaxMip = int(std::floor(std::log2(float(qMax(1, sz)))));   // 7 for a 128² probe
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    m_pendingRefl.clear();   // drop the CPU copy now it's on the GPU
    qInfo("GLModelWidget: reflection cubemap uploaded (%dx%d, maxMip=%d)", sz, sz, m_reflMaxMip);
}

void GLModelWidget::setPartFactors(const QVector<float>& metal, const QVector<float>& rough)
{
    m_partMetal = metal;
    m_partRough = rough;
    update();
}

void GLModelWidget::setPartDetailROffset(const QVector<float>& off) { m_partDetailROffset = off; update(); }
void GLModelWidget::setPartDetailColorAdd(const QVector<float>& ca) { m_partDetailCAdd = ca; update(); }
void GLModelWidget::setPartDetailScales(const QVector<QVector3D>& s) { m_partDetailScales = s; update(); }
void GLModelWidget::setPartDetailMetalLayer(const QVector<int>& ml) { m_partDetailMetalLayer = ml; update(); }
void GLModelWidget::setPartDetailBands(const QVector<QVector4D>& b) { m_partDyeBandsV = b; update(); }
void GLModelWidget::setPartDetailZoneMap(const QVector<QVector4D>& z) { m_partZoneMapV = z; update(); }
// (setDetailNormalMul / setDetailRoughMul / setDetailScale removed — unused hooks; the members
//  keep their defaults and per-part authored values arrive via setPartDetail*.)

void GLModelWidget::setPartEmissiveMult(const QVector<float>& mult) { m_partEmisMul = mult; update(); }
void GLModelWidget::setPartEmissiveColor(const QVector<float>& rgb3) { m_partEmisColor = rgb3; update(); }
void GLModelWidget::setPartDetailIntensity(const QVector<float>& normalInt, const QVector<float>& roughInt)
{
    m_partDetailNInt = normalInt;
    m_partDetailRInt = roughInt;
    update();
}
void GLModelWidget::setPartEye(const QVector<int>& eye) { m_partEye = eye; update(); }
void GLModelWidget::setPartHead(const QVector<int>& head) { m_partHead = head; update(); }
void GLModelWidget::setEyeParams(float irisRoughness) { m_eyeRough = qBound(0.02f, irisRoughness, 1.0f); update(); }
void GLModelWidget::setEmissiveScale(float v) { v = qMax(0.0f, v); if (m_emisScale != v) { m_emisScale = v; update(); } }

void GLModelWidget::setPbr(bool on)
{
    if (m_pbr != on) { m_pbr = on; update(); }
}

// Bleed opaque colour outward into transparent texels so mip generation doesn't average the
// (usually black) transparent regions into alpha-cutout edges — the dark-halo / black-outline
// artifact on cut-out cloth/foliage. Alpha is untouched; only hidden (cut) texels change, so
// visible pixels are identical. Fully-opaque textures return after one no-op pass.
static QImage dilateOpaqueRGB(QImage img, int passes)
{
    const int W = img.width(), H = img.height();
    if (W < 2 || H < 2) return img;
    // Validity mask (alpha is NEVER modified — it's the cutout). A texel is a valid colour source
    // once its alpha is above the 0.35 cutout OR it's been filled this run. Filled texels become
    // sources next pass, so colour spreads `passes` texels outward from the visible region.
    QByteArray valid(qsizetype(W) * H, 0);
    for (int y = 0; y < H; ++y) {
        const uchar* s = img.constScanLine(y);
        for (int x = 0; x < W; ++x) valid[qsizetype(y) * W + x] = (s[x * 4 + 3] >= 89) ? 1 : 0;
    }
    for (int pass = 0; pass < passes; ++pass) {
        const QImage prev = img;
        const QByteArray prevValid = valid;
        bool changed = false;
        for (int y = 0; y < H; ++y) {
            uchar* d = img.scanLine(y);
            for (int x = 0; x < W; ++x) {
                if (prevValid[qsizetype(y) * W + x]) continue;   // already has a real colour
                int r = 0, g = 0, b = 0, n = 0;
                auto tap = [&](int xx, int yy) {
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H) return;
                    if (!prevValid[qsizetype(yy) * W + xx]) return;
                    const uchar* s = prev.constScanLine(yy);
                    r += s[xx * 4]; g += s[xx * 4 + 1]; b += s[xx * 4 + 2]; ++n;
                };
                tap(x - 1, y); tap(x + 1, y); tap(x, y - 1); tap(x, y + 1);
                if (n) {
                    d[x * 4] = uchar(r / n); d[x * 4 + 1] = uchar(g / n); d[x * 4 + 2] = uchar(b / n);
                    valid[qsizetype(y) * W + x] = 1;
                    changed = true;
                }
            }
        }
        if (!changed) break;   // nothing left to bleed (e.g. a fully-opaque texture)
    }
    return img;
}

// Soften a hard/1-bit cutout mask into a smooth 0→1 alpha band so the shader's smooth alpha-test
// + MSAA alpha-to-coverage have a gradient to anti-alias (a binary mask has none → jagged edges).
// Separable box blur on the ALPHA channel only; RGB (already edge-dilated) is untouched.
static void blurAlphaChannel(QImage& img, int r)
{
    const int W = img.width(), H = img.height();
    if (W < 3 || H < 3 || r < 1) return;
    QByteArray a(qsizetype(W) * H, 0);
    for (int y = 0; y < H; ++y) { const uchar* s = img.constScanLine(y);
        for (int x = 0; x < W; ++x) a[qsizetype(y) * W + x] = char(s[x * 4 + 3]); }
    QByteArray tmp(a.size(), 0);
    for (int y = 0; y < H; ++y)                         // horizontal
        for (int x = 0; x < W; ++x) {
            int sum = 0, c = 0;
            for (int dx = -r; dx <= r; ++dx) { const int xx = x + dx; if (xx < 0 || xx >= W) continue;
                sum += uchar(a[qsizetype(y) * W + xx]); ++c; }
            tmp[qsizetype(y) * W + x] = char(sum / c);
        }
    for (int y = 0; y < H; ++y)                         // vertical → back into a
        for (int x = 0; x < W; ++x) {
            int sum = 0, c = 0;
            for (int dy = -r; dy <= r; ++dy) { const int yy = y + dy; if (yy < 0 || yy >= H) continue;
                sum += uchar(tmp[qsizetype(yy) * W + x]); ++c; }
            a[qsizetype(y) * W + x] = char(sum / c);
        }
    for (int y = 0; y < H; ++y) { uchar* d = img.scanLine(y);
        for (int x = 0; x < W; ++x) d[x * 4 + 3] = uchar(a[qsizetype(y) * W + x]); }
}

void GLModelWidget::uploadTextures()
{
    m_hasPendingTex = false;
    // If the VRAM-pool toggle changed since we last held textures, drop everything under the OLD
    // mode (destroyTextures() branches on m_poolActive) and switch. This keeps ownership consistent:
    // in legacy mode the per-part arrays own their textures; in pooled mode the pool owns them and
    // the per-part arrays are just references.
    if (m_vramPool != m_poolActive) { destroyTextures(); m_poolActive = m_vramPool; }
    ++m_poolGen;

    // Create one GL texture from a single image (shared by both paths).
    auto uploadOne = [this](const QImage& srcImg, bool nearest, bool dilate) -> GLuint {
        QImage img = srcImg.convertToFormat(QImage::Format_RGBA8888);
        if (dilate) {
            // Alpha-cutout surfaces (cloth/foliage/hair): edge-dilate the RGB (kills the black halo)
            // and soften the alpha band so MSAA can anti-alias the 1-bit-ish mask.
            const int W2 = img.width(), H2 = img.height();
            bool hasCut = false;
            for (int y = 0; y < H2 && !hasCut; ++y) { const uchar* s = img.constScanLine(y);
                for (int x = 0; x < W2; ++x) if (s[x * 4 + 3] == 0) { hasCut = true; break; } }
            img = dilateOpaqueRGB(img, 6);
            if (hasCut) blurAlphaChannel(img, 2);
        }
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        // Value-banded maps (dye mask) point-sample to keep zone edges crisp; everything else trilinear.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width(), img.height(), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
        if (!nearest) {
            glGenerateMipmap(GL_TEXTURE_2D);
            if (m_maxAniso > 1.0f)   // anisotropic filtering (EXT token 0x84FE), capped at 8×
                glTexParameterf(GL_TEXTURE_2D, 0x84FE, qMin(m_maxAniso, 8.0f));
        }
        return tex;
    };

    auto upload = [&](QVector<GLuint>& dst, const QVector<QImage>& src, const char* role,
                      bool nearest = false, bool dilate = false) {
        if (!m_poolActive) {
            // Legacy: per-part arrays own their textures. Free old, upload fresh, dedup by image.
            QSet<GLuint> old(dst.begin(), dst.end());
            for (GLuint t : old) if (t) glDeleteTextures(1, &t);
            dst.fill(0, m_parts.size());
            QHash<qint64, GLuint> byKey;
            for (int i = 0; i < src.size() && i < dst.size(); ++i) {
                if (src[i].isNull()) continue;
                const qint64 key = src[i].cacheKey();
                const auto it = byKey.constFind(key);
                if (it != byKey.constEnd()) { dst[i] = it.value(); continue; }
                GLuint tex = uploadOne(src[i], nearest, dilate);
                dst[i] = tex; byKey.insert(key, tex);
            }
            return;
        }
        // Pooled: reuse GPU textures across builds by role|material key; the pool owns them, so we
        // never free here — dst just references pooled ids. Parts with the same key share a texture.
        dst.fill(0, m_parts.size());
        for (int i = 0; i < m_parts.size(); ++i) {
            // Missing key ⇒ build-unique fallback: still pool-owned (so it's freed), but it can never
            // match a later build, so we never risk reusing the wrong texture across outfits.
            const QString mk = (i < m_partMatKey.size() && !m_partMatKey[i].isEmpty())
                                   ? m_partMatKey[i]
                                   : (QStringLiteral("#") + QString::number(m_poolGen) + QLatin1Char('_') + QString::number(i));
            const QString key = QLatin1String(role) + QLatin1Char('|') + mk;
            const auto it = m_texPool.constFind(key);
            if (it != m_texPool.constEnd()) { dst[i] = it.value(); m_texPoolLastGen[key] = m_poolGen; continue; }
            if (i >= src.size() || src[i].isNull()) continue;   // this part/material has no such map
            GLuint tex = uploadOne(src[i], nearest, dilate);
            const qint64 bytes = qint64(src[i].width()) * src[i].height() * 4;
            m_texPool.insert(key, tex);
            m_texPoolSize.insert(key, bytes);
            m_texPoolLastGen.insert(key, m_poolGen);
            m_texPoolBytes += bytes;
            dst[i] = tex;
        }
    };
    if (!m_pendingTex.isEmpty() || m_partTex.size() != m_parts.size())
        upload(m_partTex, m_pendingTex, "base", /*nearest=*/false, /*dilate=*/true);   // base colour: edge-bleed
    if (!m_pendingNorm.isEmpty()) upload(m_partNormTex, m_pendingNorm, "norm");
    if (!m_pendingOrm.isEmpty())  upload(m_partOrmTex, m_pendingOrm, "orm");
    if (!m_pendingEmis.isEmpty()) upload(m_partEmisTex, m_pendingEmis, "emis");
    for (int k = 0; k < 3; ++k) {
        if (!m_pendingDetailN[k].isEmpty()) upload(m_partDetailNTex[k], m_pendingDetailN[k], k==0?"dN0":k==1?"dN1":"dN2");
        if (!m_pendingDetailR[k].isEmpty()) upload(m_partDetailRTex[k], m_pendingDetailR[k], k==0?"dR0":k==1?"dR1":"dR2");
    }
    if (!m_pendingTrans.isEmpty())   upload(m_partTransTex, m_pendingTrans, "trans");
    if (!m_pendingMask.isEmpty())    upload(m_partMaskTex, m_pendingMask, "mask");
    if (!m_pendingDyeMask.isEmpty()) upload(m_partDyeMaskTex, m_pendingDyeMask, "dyeM", /*nearest=*/true);
    if (!m_pendingDyeRamp.isEmpty()) upload(m_partDyeRampTex, m_pendingDyeRamp, "dyeR");
    if (!m_pendingFurMask.isEmpty())  upload(m_partFurMaskTex, m_pendingFurMask, "furM");
    if (!m_pendingFurNoise.isEmpty()) upload(m_partFurNoiseTex, m_pendingFurNoise, "furN");
    if (!m_pendingFxNoise.isEmpty())  upload(m_partFxNoiseTex, m_pendingFxNoise, "fxN");
    glBindTexture(GL_TEXTURE_2D, 0);
    if (m_poolActive && m_texPoolBytes > m_texPoolBudget) evictTexturePool();
    m_pendingTex.clear();
    m_pendingNorm.clear();
    m_pendingOrm.clear();
    m_pendingEmis.clear();
    for (int k = 0; k < 3; ++k) { m_pendingDetailN[k].clear(); m_pendingDetailR[k].clear(); }
    m_pendingTrans.clear();
    m_pendingMask.clear();
    m_pendingDyeMask.clear();
    m_pendingDyeRamp.clear();
    m_pendingFurMask.clear();
    m_pendingFurNoise.clear();
    m_pendingFxNoise.clear();
}

void GLModelWidget::destroyTextures()
{
    if (m_poolActive) {
        // Pooled mode: the pool owns the textures; the per-part arrays are only references, so free
        // the pool once and clear the arrays without freeing (that would double-free).
        for (auto it = m_texPool.constBegin(); it != m_texPool.constEnd(); ++it)
            if (it.value()) { GLuint t = it.value(); glDeleteTextures(1, &t); }
        m_texPool.clear();
        m_texPoolSize.clear();
        m_texPoolLastGen.clear();
        m_texPoolBytes = 0;
    } else {
        // Legacy mode: per-part arrays own their textures (deduped across arrays).
        QSet<GLuint> uniq;
        for (GLuint t : m_partTex)     if (t) uniq.insert(t);
        for (GLuint t : m_partNormTex) if (t) uniq.insert(t);
        for (GLuint t : m_partOrmTex)  if (t) uniq.insert(t);
        for (GLuint t : m_partEmisTex) if (t) uniq.insert(t);
        for (int k = 0; k < 3; ++k) {
            for (GLuint t : m_partDetailNTex[k]) if (t) uniq.insert(t);
            for (GLuint t : m_partDetailRTex[k]) if (t) uniq.insert(t);
        }
        for (GLuint t : m_partTransTex)   if (t) uniq.insert(t);
        for (GLuint t : m_partMaskTex)    if (t) uniq.insert(t);
        for (GLuint t : m_partDyeMaskTex) if (t) uniq.insert(t);
        for (GLuint t : m_partDyeRampTex) if (t) uniq.insert(t);
        for (GLuint t : m_partFurMaskTex)  if (t) uniq.insert(t);
        for (GLuint t : m_partFurNoiseTex) if (t) uniq.insert(t);
        for (GLuint t : m_partFxNoiseTex)  if (t) uniq.insert(t);
        for (GLuint t : uniq) glDeleteTextures(1, &t);
    }
    if (m_dyeGradTex) { glDeleteTextures(1, &m_dyeGradTex); m_dyeGradTex = 0; }
    m_partTex.clear();
    m_partNormTex.clear();
    m_partOrmTex.clear();
    m_partEmisTex.clear();
    for (int k = 0; k < 3; ++k) { m_partDetailNTex[k].clear(); m_partDetailRTex[k].clear(); }
    m_partTransTex.clear();
    m_partMaskTex.clear();
    m_partDyeMaskTex.clear();
    m_partDyeRampTex.clear();
    m_partFurMaskTex.clear();
    m_partFurNoiseTex.clear();
    m_partFxNoiseTex.clear();
}

// Evict pooled textures that no per-part array currently references, oldest-used first, until we're
// back under the byte budget. Referenced textures are never freed (so no dangling per-part ids).
void GLModelWidget::evictTexturePool()
{
    QSet<GLuint> live;
    auto addLive = [&](const QVector<GLuint>& a) { for (GLuint t : a) if (t) live.insert(t); };
    addLive(m_partTex); addLive(m_partNormTex); addLive(m_partOrmTex); addLive(m_partEmisTex);
    for (int k = 0; k < 3; ++k) { addLive(m_partDetailNTex[k]); addLive(m_partDetailRTex[k]); }
    addLive(m_partTransTex); addLive(m_partMaskTex); addLive(m_partDyeMaskTex); addLive(m_partDyeRampTex);
    addLive(m_partFurMaskTex); addLive(m_partFurNoiseTex); addLive(m_partFxNoiseTex);

    QVector<QString> cand;
    cand.reserve(m_texPool.size());
    for (auto it = m_texPool.constBegin(); it != m_texPool.constEnd(); ++it)
        if (!live.contains(it.value())) cand.push_back(it.key());
    std::sort(cand.begin(), cand.end(), [this](const QString& a, const QString& b) {
        return m_texPoolLastGen.value(a) < m_texPoolLastGen.value(b);   // oldest used first
    });
    for (const QString& key : cand) {
        if (m_texPoolBytes <= m_texPoolBudget) break;
        GLuint t = m_texPool.value(key);
        if (t) glDeleteTextures(1, &t);
        m_texPoolBytes -= m_texPoolSize.value(key);
        m_texPool.remove(key);
        m_texPoolSize.remove(key);
        m_texPoolLastGen.remove(key);
    }
}

void GLModelWidget::setVramPoolEnabled(bool on)
{
    // Applied at the next uploadTextures() (which handles the ownership transition). No immediate GL.
    m_vramPool = on;
}

void GLModelWidget::setPartMatKeys(const QVector<QString>& keys)
{
    m_partMatKey = keys;
}

void GLModelWidget::setFurEnabled(bool on)
{ if (m_furEnabled != on) { m_furEnabled = on; update(); } }
void GLModelWidget::setFurShells(int n)        { n = qBound(2, n, 32); if (m_furShells != n) { m_furShells = n; update(); } }
void GLModelWidget::setFurLength(float frac)   { m_furLength = qBound(0.0f, frac, 0.06f); update(); }
void GLModelWidget::setFurDensity(float t)     { m_furTiling = qBound(4.0f, t, 160.0f);  update(); }
void GLModelWidget::setFurCoverage(float t)    { m_furCoverage = qBound(0.0f, t, 0.6f);  update(); }
void GLModelWidget::setFurGravity(float frac)  { m_furGravity = qBound(0.0f, frac, 0.02f); update(); }
void GLModelWidget::setFurCurl(float frac)     { m_furCurl = qBound(0.0f, frac, 0.02f); update(); }
void GLModelWidget::setPartFur(const QVector<int>& fur)
{ m_partFur = fur; update(); }
void GLModelWidget::setPartFurMask(const QVector<QImage>& maps)
{ m_pendingFurMask = maps; m_hasPendingTex = true; update(); }
void GLModelWidget::setPartFurNoise(const QVector<QImage>& maps)
{ m_pendingFurNoise = maps; m_hasPendingTex = true; update(); }

void GLModelWidget::setPartVisible(int i, bool on)
{
    if (i >= 0 && i < m_parts.size() && m_parts[i].visible != on) {
        m_parts[i].visible = on;
        update();
    }
}

bool GLModelWidget::partVisible(int i) const
{
    return (i >= 0 && i < m_parts.size()) ? m_parts[i].visible : true;
}

// See the header. The whole reason this can be short: every pass already sizes itself from
// m_fbW/m_fbH and every "back to the screen" bind already goes through targetFbo(), so raising the
// former and pointing the latter at our own FBO supersamples the entire pipeline — SSAO G-buffer,
// shadow pass, FX, stencil silhouette — with no per-pass special-casing.
QImage GLModelWidget::grabSupersampled(int factor)
{
    if (factor <= 1 || m_prog == 0) return {};
    const qreal dpr = devicePixelRatioF();
    const int baseW = qMax(1, int(width()  * dpr));
    const int baseH = qMax(1, int(height() * dpr));
    qint64 W = qint64(baseW) * factor, H = qint64(baseH) * factor;

    makeCurrent();
    // Two ceilings, both of which the driver enforces silently by handing back an invalid FBO:
    // the texture-size limit, and what the GPU will actually allocate. Shrink the factor until it
    // fits rather than returning nothing, so a 4x request on a big viewport degrades to 3x or 2x.
    GLint maxTex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    if (maxTex <= 0) maxTex = 8192;
    while (factor > 1 && (W > maxTex || H > maxTex)) {
        --factor; W = qint64(baseW) * factor; H = qint64(baseH) * factor;
    }
    if (factor <= 1) { doneCurrent(); return {}; }

    std::unique_ptr<QOpenGLFramebufferObject> fbo;
    while (factor > 1) {
        // CombinedDepthStencil, not Depth: the part-highlight silhouette is a stencil pass, and a
        // capture target without a stencil buffer would drop it.
        fbo = std::make_unique<QOpenGLFramebufferObject>(int(W), int(H),
                  QOpenGLFramebufferObject::CombinedDepthStencil);
        if (fbo->isValid()) break;
        fbo.reset();
        --factor; W = qint64(baseW) * factor; H = qint64(baseH) * factor;
    }
    if (!fbo) { doneCurrent(); return {}; }

    const int prevW = m_fbW, prevH = m_fbH;
    m_captureFbo = fbo->handle();
    m_fbW = int(W); m_fbH = int(H);
    fbo->bind();
    glViewport(0, 0, m_fbW, m_fbH);
    paintGL();                       // the real thing, at the real size
    glFinish();                      // localise any driver fault to this render (see grabThumbnail)
    glGetError();
    fbo->release();
    QImage img = fbo->toImage();

    m_captureFbo = 0;
    m_fbW = prevW; m_fbH = prevH;
    // The SSAO G-buffer was reallocated to the capture size; renderPos re-checks against m_fbW/m_fbH
    // and will size it back on the next on-screen frame, so nothing is left stale.
    doneCurrent();
    update();
    return img;
}

QImage GLModelWidget::grabThumbnail(int size)
{
    if (m_prog == 0)              // GL never initialized (tab never shown)
        return {};
    makeCurrent();
    if (m_hasPending) uploadPending();
    if (m_indexCount == 0) { doneCurrent(); return {}; }

    // Reuse one persistent FBO across thumbnails (recreating it every call — dozens per scroll
    // — is a known driver-crash source). Rebuild only if the requested size changed.
    if (!m_thumbFbo || m_thumbFboSize != size) {
        m_thumbFbo = std::make_unique<QOpenGLFramebufferObject>(size, size, QOpenGLFramebufferObject::Depth);
        m_thumbFboSize = size;
    }
    QOpenGLFramebufferObject& fbo = *m_thumbFbo;
    if (!fbo.isValid()) { doneCurrent(); return {}; }
    fbo.bind();
    glViewport(0, 0, size, size);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.12f, 0.12f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float cp = std::cos(0.25f), sp = std::sin(0.25f);
    const float cy = std::cos(0.6f),  sy = std::sin(0.6f);
    const QVector3D dir(cp * sy, sp, cp * cy);
    const QVector3D eye = m_center + dir * (m_radius * 2.6f);
    QMatrix4x4 proj; proj.perspective(45.0f, 1.0f, qMax(0.001f, m_radius * 0.02f), m_radius * 20.0f);
    QMatrix4x4 view; view.lookAt(eye, m_center, QVector3D(0, 1, 0));
    QMatrix4x4 model;
    const QMatrix4x4 mvp = proj * view * model;

    glUseProgram(m_prog);
    glUniformMatrix4fv(uni(m_prog, "uMVP"), 1, GL_FALSE, mvp.constData());
    glUniformMatrix4fv(uni(m_prog, "uModel"), 1, GL_FALSE, model.constData());
    const QVector3D ld = (eye - m_center).normalized();
    glUniform3f(uni(m_prog, "uLightDir"), ld.x(), ld.y(), ld.z());
    glUniform3f(uni(m_prog, "uViewPos"), eye.x(), eye.y(), eye.z());
    glUniform3f(uni(m_prog, "uBase"), 0.78f, 0.78f, 0.78f);
    glUniform1i(uni(m_prog, "uHasTex"), 0);
    glUniform1i(uni(m_prog, "uHasNormal"), 0);
    glUniform1i(uni(m_prog, "uPbr"), 0);
    glUniform1i(uni(m_prog, "uViewChannel"), 0);   // thumbnails always shaded
    glUniform1i(uni(m_prog, "uHasOrm"), 0);
    glUniform1i(uni(m_prog, "uHasEmissive"), 0);
    glUniform1i(uni(m_prog, "uHasDyeMask"), 0);
    glUniform1i(uni(m_prog, "uHasDyeRamp"), 0);
    glUniform1i(uni(m_prog, "uFDye"), 0);
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);

    // Force this thumbnail's GPU work to fully complete before we move on. Without it,
    // a burst of thumbnail renders queues a lot of GPU work; if one heavy mesh hangs the
    // GPU, the driver's timeout (TDR) can reset the device and crash the app a moment
    // LATER — during an unrelated model's parse. glFinish() localizes each render so a
    // fault is attributed to (and contained within) the model that caused it.
    glFinish();
    glGetError();   // swallow any accumulated GL error so it doesn't bleed into the next render

    fbo.release();
    QImage img = fbo.toImage();
    doneCurrent();
    update();   // restore on-screen view
    return img;
}

// Ensemble tile: unlike grabThumbnail's grey single-pass, this runs the REAL paintGL (per-part
// textures, dyes, fur) via grabFramebuffer, with the camera parked on the front (+X) axis and
// the channel forced to BASE COLOUR — unlit, so the tile is the outfit's flat palette. Guides
// (grid/skeleton/gradient) are suppressed for a clean card; everything is restored after.
QImage GLModelWidget::grabEnsembleThumb(int size)
{
    if (m_indexCount == 0) return {};
    const float yaw = m_yaw, pitch = m_pitch, dist = m_dist;
    const QVector3D ctr = m_center;
    const QVector3D tgtC = m_tgtCenter;
    const float tgtY = m_tgtYaw, tgtP = m_tgtPitch, tgtD = m_tgtDist;
    const int  chan = m_viewChannel;
    const bool grid = m_showGrid, skel = m_showSkeleton, grad = m_bgGradient;
    if (m_camAnim) m_camAnim->stop();   // a mid-glide paint would drift the parked camera

    QVector3D c; float r;
    liveBounds(c, r);                             // frame the LIVE pose, not the bind pose
    m_center = c;    m_tgtCenter = c;
    m_dist = qMax(0.01f, r * 2.4f);               // whole character in frame
    m_tgtDist = m_dist;
    m_yaw = 3.14159265f / 2.0f;  m_tgtYaw = m_yaw;    // +X = the gizmo's front view
                                                      // (literal — MSVC hides M_PI by default)
    m_pitch = 0.0f;              m_tgtPitch = 0.0f;
    m_viewChannel = 1;                            // Base Color — flat, colourful
    m_showGrid = false; m_showSkeleton = false; m_bgGradient = false;

    QImage img = grabFramebuffer();               // full pipeline at widget resolution

    m_yaw = yaw; m_pitch = pitch; m_dist = dist; m_center = ctr;
    m_tgtCenter = tgtC; m_tgtYaw = tgtY; m_tgtPitch = tgtP; m_tgtDist = tgtD;
    m_viewChannel = chan; m_showGrid = grid; m_showSkeleton = skel; m_bgGradient = grad;
    update();

    if (img.isNull()) return img;
    const int sq = qMin(img.width(), img.height());   // centre-crop square, then scale down
    return img.copy((img.width() - sq) / 2, (img.height() - sq) / 2, sq, sq)
              .scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

void GLModelWidget::mousePressEvent(QMouseEvent* e)
{
    m_lastPos = e->pos();
    if (e->button() == Qt::RightButton) m_rightPressPx = e->pos();   // remember for click-vs-drag test
    // Middle-click = reset/re-frame the view (replaces the old "Reset view" toolbar button).
    // Middle-drag isn't used for anything here, so the press is unambiguous.
    if (e->button() == Qt::MiddleButton) {
        resetView();
        e->accept();
    }
}

// Right-click (a press+release that did NOT pan) → part context menu. A right-DRAG pans the view,
// so we only treat it as a click when the cursor barely moved between press and release.
void GLModelWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::RightButton
        && (e->pos() - m_rightPressPx).manhattanLength() < 4) {
        const int part = pickPart(e->pos());          // front-most triangle under the cursor (-1 = miss)
        emit partRightClicked(part, e->globalPosition().toPoint());
        e->accept();
    }
}

// Double-click a part to focus it: ray-pick the front-most triangle under the cursor, frame
// that part's live bounds (keeping the current angle), and notify listeners for slot sync.
void GLModelWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    const int part = pickPart(e->pos());
    // "Frame part on select": whether the camera snaps to the picked part. Read live from
    // QSettings so ONE setting governs every viewport (Models/Wardrobe/Stable) with no parity
    // plumbing — the Camera-panel checkbox just writes the key. Selection sync (partFocused)
    // fires either way.
    if (part >= 0 && QSettings().value(QStringLiteral("viewer/framePartOnPick"), true).toBool()) {
        QVector3D c; float r;
        if (partsBounds(QVector<int>{part}, c, r)) {
            m_followParts.clear();                        // a deliberate focus cancels slot-follow
            frameRegionKeepRotation(c, r, /*animate=*/true);
        }
    }
    emit partFocused(part);
}

void GLModelWidget::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint d = e->pos() - m_lastPos;
    m_lastPos = e->pos();
    if (e->buttons() & Qt::LeftButton) {
        m_yaw   -= d.x() * 0.01f;
        m_pitch += d.y() * 0.01f;
        const float lim = 1.553f;  // ~89° to avoid gimbal flip at the poles
        m_pitch = qBound(-lim, m_pitch, lim);
        update();
    } else if (e->buttons() & Qt::RightButton) {
        // Right-drag pans the orbit centre in the camera plane.
        const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
        const float cy = std::cos(m_yaw),   sy = std::sin(m_yaw);
        const QVector3D viewDir = -QVector3D(cp * sy, sp, cp * cy);   // eye → centre
        QVector3D right = QVector3D::crossProduct(viewDir, QVector3D(0, 1, 0));
        if (right.lengthSquared() < 1e-8f) right = QVector3D(1, 0, 0);
        right.normalize();
        const QVector3D camUp = QVector3D::crossProduct(right, viewDir).normalized();
        const float s = m_dist * 0.0015f;   // pan speed scales with zoom
        m_center -= right * (d.x() * s);
        m_center += camUp * (d.y() * s);
        update();
    }
}

// Bounds of the pose CURRENTLY on screen (the skinned m_verts), so camera snaps track the
// live character instead of the static bind pose — which drifts as soon as an animation with
// root motion plays. Falls back to the bind bounds when no geometry is skinned yet.
void GLModelWidget::liveBounds(QVector3D& center, float& radius) const
{
    constexpr int stride = 11;
    const int n = m_verts.size();
    const int nv = n / stride;
    if (n < stride) { center = m_homeCenter; radius = m_radius; return; }
    float mnx = 1e30f, mny = 1e30f, mnz = 1e30f, mxx = -1e30f, mxy = -1e30f, mxz = -1e30f;
    bool any = false;
    // Frame only the VISIBLE parts, so hiding submeshes (part-tree, FX/SIM/FORM, isolate-eyeball,
    // armour-covered base regions) actually re-frames the camera on what's shown — not the whole
    // (partly hidden) mesh.
    if (!m_parts.isEmpty() && !m_indices.isEmpty()) {
        for (const Part& p : m_parts) {
            if (!p.visible || p.count == 0) continue;
            const int end = qMin(p.offset + p.count, m_indices.size());
            for (int k = p.offset; k < end; ++k) {
                const int vi = int(m_indices[k]);
                if (vi < 0 || vi >= nv) continue;
                const float x = m_verts[vi*stride], y = m_verts[vi*stride+1], z = m_verts[vi*stride+2];
                mnx = qMin(mnx, x); mny = qMin(mny, y); mnz = qMin(mnz, z);
                mxx = qMax(mxx, x); mxy = qMax(mxy, y); mxz = qMax(mxz, z);
                any = true;
            }
        }
    }
    if (!any) {   // no sub-parts (single-draw model) or everything hidden → fall back to all verts
        for (int i = 0; i + 2 < n; i += stride) {
            const float x = m_verts[i], y = m_verts[i+1], z = m_verts[i+2];
            mnx = qMin(mnx, x); mny = qMin(mny, y); mnz = qMin(mnz, z);
            mxx = qMax(mxx, x); mxy = qMax(mxy, y); mxz = qMax(mxz, z);
        }
    }
    center = QVector3D((mnx + mxx) * 0.5f, (mny + mxy) * 0.5f, (mnz + mxz) * 0.5f);
    radius = qMax(0.001f, QVector3D(mxx - mnx, mxy - mny, mxz - mnz).length() * 0.5f);
}

bool GLModelWidget::partsBounds(const QVector<int>& partIndices, QVector3D& center, float& radius) const
{
    constexpr int stride = 11;   // px,py,pz,nx,ny,nz,u,v,… (matches liveBounds)
    const int nv = m_verts.size() / stride;
    if (nv <= 0 || m_indices.isEmpty())
        return false;
    float mnx = 1e30f, mny = 1e30f, mnz = 1e30f, mxx = -1e30f, mxy = -1e30f, mxz = -1e30f;
    bool any = false;
    for (int pi : partIndices) {
        if (pi < 0 || pi >= m_parts.size()) continue;
        const Part& p = m_parts[pi];
        const int end = qMin(p.offset + p.count, m_indices.size());
        for (int k = p.offset; k < end; ++k) {
            const int vi = int(m_indices[k]);
            if (vi < 0 || vi >= nv) continue;
            const float x = m_verts[vi * stride], y = m_verts[vi * stride + 1], z = m_verts[vi * stride + 2];
            mnx = qMin(mnx, x); mny = qMin(mny, y); mnz = qMin(mnz, z);
            mxx = qMax(mxx, x); mxy = qMax(mxy, y); mxz = qMax(mxz, z);
            any = true;
        }
    }
    if (!any) return false;
    center = QVector3D((mnx + mxx) * 0.5f, (mny + mxy) * 0.5f, (mnz + mxz) * 0.5f);
    radius = qMax(0.05f, QVector3D(mxx - mnx, mxy - mny, mxz - mnz).length() * 0.5f * 1.25f);
    return true;
}

void GLModelWidget::followParts(const QVector<int>& partIndices)
{
    m_followParts = partIndices;
}

GLModelWidget::CamState GLModelWidget::cameraState() const
{
    CamState s;
    s.yaw = m_yaw; s.pitch = m_pitch; s.dist = m_dist; s.fov = m_fov;
    s.cx = m_center.x(); s.cy = m_center.y(); s.cz = m_center.z();
    s.ortho = m_ortho; s.valid = true;
    return s;
}

void GLModelWidget::setCameraState(const CamState& s)
{
    if (!s.valid) return;
    if (m_camAnim) m_camAnim->stop();   // a hard restore cancels any in-flight glide
    m_followParts.clear();
    m_yaw = s.yaw; m_pitch = s.pitch; m_dist = s.dist; m_fov = s.fov;
    m_center = QVector3D(s.cx, s.cy, s.cz);
    m_ortho = s.ortho;
    update();
}

// Ray-pick the nearest visible triangle under a widget-space point. Reconstructs the same
// view/projection paintGL uses, unprojects the pixel into a world ray, and Möller–Trumbore
// tests every visible part's triangles against the live (skinned) vertices. Returns the part.
int GLModelWidget::pickPart(const QPoint& posPx) const
{
    constexpr int stride = 11;
    const int nv = m_verts.size() / stride;
    if (nv <= 0 || m_indices.isEmpty() || width() <= 0 || height() <= 0) return -1;

    const float aspect = float(width()) / float(height());
    const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    const float cy = std::cos(m_yaw),   sy = std::sin(m_yaw);
    const QVector3D dir(cp * sy, sp, cp * cy);
    const QVector3D eye = m_center + dir * m_dist;
    QMatrix4x4 proj;
    if (m_ortho) {
        const float h = m_dist * std::tan(m_fov * 0.5f * 3.14159265f / 180.0f);
        const float w = h * aspect;
        const float ext = qMax(m_radius, m_dist * 0.5f) * 10.0f;   // match paintGL's mismatch-proof range
        proj.ortho(-w, w, -h, h, qMax(0.001f, m_dist - ext), m_dist + ext);
    } else {
        // Far plane follows the camera distance so you can zoom out on very large models without
        // the mesh clipping into the far plane (no fixed 20×-radius wall).
        proj.perspective(m_fov, aspect, qMax(0.001f, m_radius * 0.02f),
                         qMax(m_radius * 20.0f, m_dist + m_radius * 4.0f));
    }
    QMatrix4x4 view; view.lookAt(eye, m_center, QVector3D(0, 1, 0));
    bool ok = false;
    const QMatrix4x4 invVP = (proj * view).inverted(&ok);
    if (!ok) return -1;

    const float nx = 2.0f * float(posPx.x()) / float(width())  - 1.0f;
    const float ny = 1.0f - 2.0f * float(posPx.y()) / float(height());
    QVector4D pn = invVP * QVector4D(nx, ny, -1.0f, 1.0f);
    QVector4D pf = invVP * QVector4D(nx, ny,  1.0f, 1.0f);
    if (qFuzzyIsNull(pn.w()) || qFuzzyIsNull(pf.w())) return -1;
    const QVector3D ro = pn.toVector3D() / pn.w();
    const QVector3D rd = (pf.toVector3D() / pf.w() - ro).normalized();

    float best = 1e30f; int bestPart = -1;
    for (int pi = 0; pi < m_parts.size(); ++pi) {
        const Part& p = m_parts[pi];
        if (!p.visible) continue;
        const int end = qMin(p.offset + p.count, m_indices.size());
        for (int k = p.offset; k + 2 < end; k += 3) {
            const int i0 = int(m_indices[k]), i1 = int(m_indices[k + 1]), i2 = int(m_indices[k + 2]);
            if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= nv || i1 >= nv || i2 >= nv) continue;
            const QVector3D v0(m_verts[i0 * stride], m_verts[i0 * stride + 1], m_verts[i0 * stride + 2]);
            const QVector3D v1(m_verts[i1 * stride], m_verts[i1 * stride + 1], m_verts[i1 * stride + 2]);
            const QVector3D v2(m_verts[i2 * stride], m_verts[i2 * stride + 1], m_verts[i2 * stride + 2]);
            const QVector3D e1 = v1 - v0, e2 = v2 - v0;
            const QVector3D pv = QVector3D::crossProduct(rd, e2);
            const float det = QVector3D::dotProduct(e1, pv);
            if (std::fabs(det) < 1e-8f) continue;      // parallel
            const float inv = 1.0f / det;
            const QVector3D tv = ro - v0;
            const float u = QVector3D::dotProduct(tv, pv) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            const QVector3D qv = QVector3D::crossProduct(tv, e1);
            const float vparam = QVector3D::dotProduct(rd, qv) * inv;
            if (vparam < 0.0f || u + vparam > 1.0f) continue;
            const float t = QVector3D::dotProduct(e2, qv) * inv;
            if (t > 1e-4f && t < best) { best = t; bestPart = pi; }
        }
    }
    return bestPart;
}

void GLModelWidget::resetView()
{
    m_followParts.clear();   // an explicit re-frame cancels slot-follow
    QVector3D c; float r;
    liveBounds(c, r);
    m_center = c;
    m_dist = r * 2.6f;
    m_yaw = 0.6f;
    m_pitch = 0.25f;
    update();
}

void GLModelWidget::frameAll(bool keepRotation, bool animate)
{
    m_followParts.clear();
    if (!keepRotation) { resetView(); return; }
    QVector3D c; float r;
    liveBounds(c, r);
    frameRegionKeepRotation(c, r, animate);   // re-centre + zoom, keep the angle
}

void GLModelWidget::frameThreeQuarter(float yawRad, float pitchRad, float targetUpFrac)
{
    QVector3D c; float r;
    liveBounds(c, r);
    c.setY(c.y() + r * targetUpFrac);   // raise the look target (chest-framing)
    frameRegion(c, r, yawRad, pitchRad, /*animate=*/false);
}

void GLModelWidget::frameRegionKeepRotation(const QVector3D& center, float radius, bool animate)
{
    // Keep whatever angle the user is currently viewing from; only re-target + re-zoom.
    const float yaw = m_camAnim && m_camAnim->isActive() ? m_tgtYaw : m_yaw;
    const float pitch = m_camAnim && m_camAnim->isActive() ? m_tgtPitch : m_pitch;
    frameRegion(center, radius, yaw, pitch, animate);
}

void GLModelWidget::frameRegion(const QVector3D& center, float radius, float yawRad, float pitchRad, bool animate)
{
    const float half = m_fov * 0.5f * 3.14159265f / 180.0f;
    const float dist = qMax(radius, 0.001f) / qMax(0.05f, std::sin(half));   // sphere fits the FOV
    m_tgtCenter = center; m_tgtYaw = yawRad; m_tgtPitch = pitchRad; m_tgtDist = dist;
    if (!animate) {
        if (m_camAnim) m_camAnim->stop();
        m_center = center; m_yaw = yawRad; m_pitch = pitchRad; m_dist = dist;
        update();
        return;
    }
    // Take the shortest angular path for yaw, then glide on a timer.
    const float kPi = 3.14159265f;
    while (m_yaw - m_tgtYaw >  kPi) m_yaw -= 2.0f * kPi;
    while (m_tgtYaw - m_yaw >  kPi) m_yaw += 2.0f * kPi;
    if (!m_camAnim) {
        m_camAnim = new QTimer(this);
        connect(m_camAnim, &QTimer::timeout, this, [this]() {
            const float t = 0.28f;   // ease factor per tick (~60fps → ~0.25s settle)
            m_yaw   += (m_tgtYaw   - m_yaw)   * t;
            m_pitch += (m_tgtPitch - m_pitch) * t;
            m_dist  += (m_tgtDist  - m_dist)  * t;
            m_center += (m_tgtCenter - m_center) * t;
            const bool done = qAbs(m_yaw - m_tgtYaw) < 1e-3f && qAbs(m_pitch - m_tgtPitch) < 1e-3f
                           && qAbs(m_dist - m_tgtDist) < 1e-3f && (m_center - m_tgtCenter).length() < 1e-3f;
            if (done) { m_yaw = m_tgtYaw; m_pitch = m_tgtPitch; m_dist = m_tgtDist; m_center = m_tgtCenter; m_camAnim->stop(); }
            update();
        });
    }
    m_camAnim->start(16);
}

void GLModelWidget::setShowTextures(bool on) { if (m_showTex != on)   { m_showTex = on;   update(); } }
void GLModelWidget::setViewChannel(int c)    { if (m_viewChannel != c) { m_viewChannel = c; update(); } }
void GLModelWidget::setFov(float deg)        { deg = qBound(10.0f, deg, 100.0f); if (qAbs(m_fov - deg) > 0.01f) { m_fov = deg; update(); } }
void GLModelWidget::setWireframe(bool on)    { if (m_wireframe != on) { m_wireframe = on; update(); } }
void GLModelWidget::setShowGrid(bool on)     { if (m_showGrid != on)  { m_showGrid = on; if (on) m_gridVerts = 0; update(); } }

void GLModelWidget::setShowAxisGizmo(bool on)
{
    m_showAxisGizmo = on;
    if (m_gizmo) m_gizmo->setVisible(on);
}

void GLModelWidget::setGridAxisColors(bool on)
{
    if (m_gridAxisColors == on) return;
    m_gridAxisColors = on;   // draw-time tint only — no grid rebuild needed
    update();
}

// Glide to an axis view keeping BOTH the orbit centre and the current zoom. frameRegion derives
// dist from a radius (sphere-fit: dist = r / sin(fov/2)), so feed it the radius that inverts to
// the distance we already have.
void GLModelWidget::orbitToAxis(float yawRad, float pitchRad)
{
    const float half = m_fov * 0.5f * 3.14159265f / 180.0f;
    const float keepR = m_dist * qMax(0.05f, std::sin(half));
    frameRegion(m_center, keepR, yawRad, pitchRad, /*animate=*/true);
}
void GLModelWidget::setShowSkeleton(bool on) { if (m_showSkeleton != on) { m_showSkeleton = on; if (on) m_skelVerts = 0; update(); } }
void GLModelWidget::setHardpoints(const QVector<ModelHardpoint>& hps) { m_hardpoints = hps; m_hpAxisVerts = 0; if (m_showHardpoints) update(); }
void GLModelWidget::setShowHardpoints(bool on) {
    if (m_showHardpoints == on) return;
    m_showHardpoints = on;
    if (on) m_hpAxisVerts = 0;
    if (!on && m_hpLabels) m_hpLabels->hide();
    update();
}
void GLModelWidget::setShowPhysBones(bool on) { if (m_showPhysBones != on) { m_showPhysBones = on; if (on) m_physVerts = 0; update(); } }
void GLModelWidget::setShowPhysAxes(bool on)  { if (m_showPhysAxes  != on) { m_showPhysAxes  = on; update(); } }
void GLModelWidget::setShowBoneNames(bool on) {
    if (m_showBoneNames == on) return;
    m_showBoneNames = on;
    if (!on && m_boneLabels) m_boneLabels->hide();
    update();   // paintGL will (re)project + show the overlay
}
void GLModelWidget::setBoneNamesTranslated(bool on) {
    if (m_boneNamesTranslated == on) return;
    m_boneNamesTranslated = on;
    update();
}
void GLModelWidget::setBoneNamesHideUnknown(bool on) {
    if (m_boneNamesHideUnknown == on) return;
    m_boneNamesHideUnknown = on;
    update();
}

// D4 name hash — VERIFIED (Fable research): DJB2 with seed 0, h = h*33 + tolower(c), 32-bit wrap,
// ASCII. Matches gbidHash in blizzhackers/d4data and 54 real (name, hash) pairs mined from
// Diablo IV.exe + the barM_base00 payload. Kept for hashing hardpoint/name strings on demand.
[[maybe_unused]] static quint32 d4NameHash(const char* s) {
    quint32 h = 0u;
    for (; *s; ++s) { char c = *s; if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a'); h = h * 33u + quint32(quint8(c)); }
    return h;
}

// Bone hash → label, from REAL Diablo IV data (Fable research; see D4_BoneHash_Research_Report.md +
// d4_hash_tables.h). D4 stores NO authored bone-name strings — only hashes — so these labels are
// DERIVED from the game's own hardpoint attachments + IK chains on the SHARED PLAYER RIG (barM/barF
// base00; 190 identical bones). Real & sourced, not guessed — labels are Fable's table (one very
// long sheath-cluster label condensed for the overlay). Bones outside this set (per-rig cloth/hair
// chains etc.) fall back to the raw bone_<hash>.
QString GLModelWidget::translateBoneName(quint32 nameHash) {
    static const QHash<quint32, QString> kMap = [] {
        struct BN { quint32 h; const char* n; };
        static const BN kBones[] = {
            { 0xBF59F7AFu, "UI+health+light+playerLight" },
            { 0x98C6875Eu, "attached+physics1+physics2+sheath+trail1+trail2+uniqueFX" },
            { 0x20365219u, "center" },
            { 0xD2322EEBu, "chest+back sheaths (2hSword/pole/staff/shield/quiver)" },
            { 0xB0076FACu, "head+lookAt" },
            { 0xD12DD5D1u, "mouth" },
            // IK limb ids decoded from barM_base00 tIKData: eIKLimb 0 = LEFT leg (its chain holds
            // the HP_leftKnee bone), 4 = RIGHT leg, 8 = RIGHT arm (rightShoulder/Elbow/Hand chain,
            // bones 106/107/108/113), 9 = LEFT arm. An earlier revision had the legs swapped and
            // called the arms "limb 8/9" — corrected against the hardpoint co-location data.
            { 0xF51D6140u, "rightShoulder | IK right arm #0" },
            { 0x8E911E73u, "rightElbow+rightShield | IK right arm #1" },
            { 0xF54A1413u, "rightHand | IK right arm #2" },
            { 0x3536F3E9u, "explosion+rightExplosion+rightWeapon" },
            { 0x50A78B5Bu, "IK right arm #3 (hand end)" },
            { 0xF4B0CE3Au, "leftShoulder | IK left arm #0" },
            { 0x8E248B6Du, "leftElbow+leftShield | IK left arm #1" },
            { 0xF4DD810Du, "leftHand | IK left arm #2" },
            { 0x34CA60E3u, "leftExplosion+leftWeapon+shield" },
            { 0x503AF855u, "IK left arm #3 (hand end)" },
            { 0x32FF39ADu, "leftHipSheath+pelvis+rightHipSheath" },
            { 0x1289E8B3u, "IK right leg #0" },
            { 0xB9CFD755u, "rightKnee | IK right leg #1" },
            { 0x9CAC595Du, "IK right leg #2" },
            { 0x34EFB08Du, "rightFoot" },
            { 0x121D55ADu, "IK left leg #0" },
            { 0xB963444Fu, "leftKnee | IK left leg #1" },
            { 0x9C3FC657u, "IK left leg #2" },
            { 0x34831D87u, "leftFoot" },
            { 0xF7A2423Bu, "UIAnimated" },
        };
        QHash<quint32, QString> m;
        for (const auto& b : kBones) m.insert(b.h, QString::fromLatin1(b.n));
        return m;
    }();
    return kMap.value(nameHash);
}

// Blender/glTF-safe token: keep [A-Za-z0-9], collapse any run of other chars to a single '_', trim.
static QString sanitizeBoneName(const QString& s) {
    QString out; bool us = false;
    for (QChar c : s) {
        if (c.isLetterOrNumber()) { out += c; us = false; }
        else if (!us && !out.isEmpty()) { out += QLatin1Char('_'); us = true; }
    }
    while (out.endsWith(QLatin1Char('_'))) out.chop(1);
    return out.isEmpty() ? s : out;
}

// Rewrite each bone's export name to its verified translated label where known (else leave the raw
// bone_<hash>). Called by the .glb exporters when "Translated bone names" is on. Operates on a COPY of
// the skeleton at the call site, so it never affects the live preview.
void GLModelWidget::translateSkeletonNames(QVector<ModelJoint>& skeleton) {
    for (ModelJoint& j : skeleton) {
        const QString t = translateBoneName(j.nameHash);
        if (!t.isEmpty()) j.name = sanitizeBoneName(t);
    }
}

// ── Blender-friendly bone names ────────────────────────────────────────────────────────────────
// Goal: names Blender's symmetry tools understand. Blender pairs bones whose names are identical
// except for a ".L"/".R" suffix (X-Mirror in edit/pose mode, Symmetrize, mirrored weight paint).
//
// Two sources, both from real data:
//  1) Curated names for the SHARED PLAYER RIG (hashes identical across all player classes),
//     derived from the game's own hardpoint/IK data ("leftHand" bone → "hand.L").
//  2) For every remaining bone (cloth/hair chains, monsters, mounts): geometric mirror detection.
//     D4 rigs mirror across the D4 Y axis (verified: HP_leftHand bone rests at +Y, HP_rightHand
//     at −Y). Reciprocal nearest-neighbour pairs of rest-pose bone heads across that plane get
//     synthetic paired names "m<idx>.L"/"m<idx>.R" (idx = skeleton index of the left member, so
//     the name stays traceable). Center/unpaired bones keep their current name.
// Suffix side convention matches Blender: with the Blender-orientation export (character facing
// −Y), the character's LEFT lands on +X — and D4 +Y is that left side.
void GLModelWidget::blenderizeSkeletonNames(QVector<ModelJoint>& skeleton) {
    const int nb = skeleton.size();
    if (nb == 0) return;

    // Curated player-rig names: { hash, base, side } — side: 0 center, +1 left, -1 right.
    struct BB { quint32 h; const char* n; int side; };
    static const BB kBlender[] = {
        { 0xBF59F7AFu, "root",      0 }, { 0x98C6875Eu, "attach",    0 },
        { 0x20365219u, "center",    0 }, { 0xD2322EEBu, "chest",     0 },
        { 0xB0076FACu, "head",      0 }, { 0xD12DD5D1u, "mouth",     0 },
        { 0xF7A2423Bu, "uiAnimated",0 }, { 0x32FF39ADu, "pelvis",    0 },
        { 0xF51D6140u, "upperArm", -1 }, { 0xF4B0CE3Au, "upperArm", +1 },
        { 0x8E911E73u, "forearm",  -1 }, { 0x8E248B6Du, "forearm",  +1 },
        { 0xF54A1413u, "hand",     -1 }, { 0xF4DD810Du, "hand",     +1 },
        { 0x3536F3E9u, "weapon",   -1 }, { 0x34CA60E3u, "weapon",   +1 },
        { 0x50A78B5Bu, "handEnd",  -1 }, { 0x503AF855u, "handEnd",  +1 },
        { 0x1289E8B3u, "thigh",    -1 }, { 0x121D55ADu, "thigh",    +1 },
        { 0xB9CFD755u, "shin",     -1 }, { 0xB963444Fu, "shin",     +1 },
        { 0x9CAC595Du, "ankle",    -1 }, { 0x9C3FC657u, "ankle",    +1 },
        { 0x34EFB08Du, "foot",     -1 }, { 0x34831D87u, "foot",     +1 },
    };
    QHash<quint32, QPair<QString, int>> curated;
    for (const auto& b : kBlender)
        curated.insert(b.h, {QString::fromLatin1(b.n), b.side});

    // Pairing + positions come from Retarget so naming and the exporter's X-mirror
    // symmetrization use the SAME partner table (curated pairs first — geometric-only
    // matching gets fooled where weapon/hand/handEnd share a location; see D4_XMirror_Spec.md).
    const QVector<QVector3D> pos = Retarget::restHeadsD4(skeleton);
    const QVector<int> partner = Retarget::mirrorPairs(skeleton);

    // Assign names. Curated first, then geometric pairs, then leave the rest untouched.
    QVector<QString> out(nb);
    for (int i = 0; i < nb; ++i) {
        const auto it = curated.constFind(skeleton[i].nameHash);
        if (it != curated.constEnd()) {
            out[i] = it->second == 0 ? it->first
                   : it->first + (it->second > 0 ? QStringLiteral(".L") : QStringLiteral(".R"));
        }
    }
    for (int i = 0; i < nb; ++i) {
        if (!out[i].isEmpty() || partner[i] < 0) continue;
        const int j = partner[i];
        if (!out[j].isEmpty()) continue;                       // curated already covers that pair
        const int left  = pos[i].y() > 0.0f ? i : j;           // D4 +Y = character's left
        const int right = left == i ? j : i;
        const QString base = QStringLiteral("m%1").arg(left, 3, 10, QLatin1Char('0'));
        out[left]  = base + QStringLiteral(".L");
        out[right] = base + QStringLiteral(".R");
    }
    // Fallback for everything else: keep the current name (bone_<hash> or a translated label).
    QSet<QString> used;
    for (int i = 0; i < nb; ++i) {
        QString n = out[i].isEmpty() ? skeleton[i].name : out[i];
        n = sanitizeBoneName(n.left(n.size()));                // sanitize, but keep the .L/.R dot
        if (!out[i].isEmpty() && (out[i].endsWith(QStringLiteral(".L")) || out[i].endsWith(QStringLiteral(".R"))))
            n = sanitizeBoneName(out[i].left(out[i].size() - 2)) + out[i].right(2);
        QString uniq = n; int k = 2;
        while (used.contains(uniq)) uniq = n + QStringLiteral("_%1").arg(k++);
        used.insert(uniq);
        skeleton[i].name = uniq;
    }
}

// Project each bone head with the last frame's proj*view and push (screenPos, label) to the overlay.
void GLModelWidget::updateBoneLabels() {
    if (!m_boneLabels) m_boneLabels = new BoneLabelOverlay(this);
    auto* ov = static_cast<BoneLabelOverlay*>(m_boneLabels);
    ov->items.clear();
    const int nb = m_skeleton.size();
    if (!m_showBoneNames || nb == 0) { ov->hide(); return; }
    // Recompute bone-head world positions the same way buildSkeleton does (anim/sim aware).
    QVector<Mat4> global(nb);
    for (int j = 0; j < nb; ++j) {
        const ModelJoint& jt = m_skeleton[j];
        Mat4 local; const int ai = m_hasAnim ? m_animByHash.value(jt.nameHash, -1) : -1;
        if (const int af = animFrameFor(ai); ai >= 0 && af < m_anim.bones[ai].rotations.size())
            local = composeTRS(m_anim.bones[ai].rotations[af].data(),
                               m_anim.bones[ai].translations[af].data(),
                               m_anim.bones[ai].scales[af].data());
        else
            local = composeTRS(jt.restQ.data(), jt.restT.data(), jt.restS.data());
        const int p = jt.parent; global[j] = (p >= 0 && p < j) ? mat4mul(global[p], local) : local;
    }
    const int W = width(), H = height();
    // Scope the labels to the bones that are actually drawn: if the ONLY bone overlay showing is
    // the physics-bones one (skeleton off), label just the physics/cloth bones (index >= baseBones)
    // — otherwise every bone gets a name even though only the cloth chain is on screen.
    const bool physScoped = m_showPhysBones && !m_showSkeleton && m_baseBones > 0 && m_baseBones < nb;
    bool anyUnknown = false; QString dbg;
    for (int j = 0; j < nb; ++j) {
        if (physScoped && j < m_baseBones) continue;   // phys overlay only → skip base-rig bones
        float hp[3];
        if (m_clothEnabled && m_sbSeeded && j < m_sbIsCloth.size() && m_sbIsCloth[j] && m_sbSimHead.size() >= (j+1)*3)
        { hp[0]=m_sbSimHead[j*3]; hp[1]=m_sbSimHead[j*3+1]; hp[2]=m_sbSimHead[j*3+2]; }
        else { hp[0]=global[j][12]; hp[1]=global[j][13]; hp[2]=global[j][14]; }
        const QVector4D clip = m_lastViewProj * QVector4D(hp[0], hp[1], hp[2], 1.0f);
        if (clip.w() <= 0.0001f) continue;                    // behind the camera
        const float sx = (clip.x() / clip.w() * 0.5f + 0.5f) * W;
        const float sy = (1.0f - (clip.y() / clip.w() * 0.5f + 0.5f)) * H;
        if (sx < -40 || sy < -20 || sx > W + 40 || sy > H + 20) continue;   // off-screen
        QString label; bool translated = false;
        if (m_boneNamesTranslated) {
            label = translateBoneName(m_skeleton[j].nameHash);
            if (!label.isEmpty()) translated = true;
            else { label = m_skeleton[j].name; anyUnknown = true; }   // fall back to raw bone_<hash>
        } else {
            label = m_skeleton[j].name;                               // raw names; not "translated"
        }
        if (m_boneNamesHideUnknown && !translated) continue;         // "Hide unnamed bones"
        ov->items.append({QPoint(int(sx), int(sy)), label, translated});
    }
    if (m_boneNamesTranslated && anyUnknown && !m_boneHashesDumped) {
        m_boneHashesDumped = true;                            // one-shot: dump hashes to calibrate the dict
        for (int j = 0; j < nb; ++j)
            dbg += QStringLiteral("%1=%2 ").arg(m_skeleton[j].nameHash).arg(translateBoneName(m_skeleton[j].nameHash));
        qInfo().noquote() << "SKELHASHES (nameHash=translated; blank=unknown):" << dbg;
    }
    ov->setGeometry(rect());
    ov->show(); ov->raise(); ov->update();
}

void GLModelWidget::setBackgroundColor(const QColor& c)
{
    if (!c.isValid()) return;
    m_bg[0] = float(c.redF()); m_bg[1] = float(c.greenF()); m_bg[2] = float(c.blueF());
    update();
}

QColor GLModelWidget::backgroundColor() const
{
    return QColor::fromRgbF(m_bg[0], m_bg[1], m_bg[2]);
}

void GLModelWidget::buildSkeleton()
{
    m_skelVerts = 0;
    const int nb = m_skeleton.size();
    if (nb == 0) return;
    // Global transforms (same compose+axis-swap the skinning palette uses, so the
    // bones line up with the mesh). When an animation is playing, use the current
    // frame's bone tracks so the skeleton overlay animates too; else rest pose.
    QVector<Mat4> global(nb);
    for (int j = 0; j < nb; ++j) {
        const ModelJoint& jt = m_skeleton[j];
        Mat4 local;
        const int ai = m_hasAnim ? m_animByHash.value(jt.nameHash, -1) : -1;
        if (const int af = animFrameFor(ai); ai >= 0 && af < m_anim.bones[ai].rotations.size()) {
            const auto& ba = m_anim.bones[ai];
            local = composeTRS(ba.rotations[af].data(),
                               ba.translations[af].data(), ba.scales[af].data());
        } else {
            local = composeTRS(jt.restQ.data(), jt.restT.data(), jt.restS.data());
        }
        const int p = jt.parent;
        global[j] = (p >= 0 && p < j) ? mat4mul(global[p], local) : local;
    }
    QVector<float> v;   // line list, 11-float layout (pos, normal-up, uv0, tangent)
    // Bone head position: for SIMULATED cloth bones use the live spring-bone position so the
    // overlay shows the physics moving (not the static animated pose); else the animated pose.
    auto headOf = [&](int j, float o[3]) {
        if (m_clothEnabled && m_sbSeeded && j >= 0 && j < m_sbIsCloth.size()
            && m_sbIsCloth[j] && m_sbSimHead.size() >= (j+1)*3) {
            o[0]=m_sbSimHead[j*3]; o[1]=m_sbSimHead[j*3+1]; o[2]=m_sbSimHead[j*3+2];
        } else { o[0]=global[j][12]; o[1]=global[j][13]; o[2]=global[j][14]; }
    };
    auto pushPos = [&](const float p[3]) {
        v << p[0] << p[1] << p[2] << 0 << 1 << 0 << 0 << 0 << 1 << 0 << 0;
    };
    for (int j = 0; j < nb; ++j) {
        const int p = m_skeleton[j].parent;
        if (p < 0 || p >= nb) continue;
        float a[3], b[3]; headOf(p, a); headOf(j, b);
        pushPos(a); pushPos(b);
    }
    if (v.isEmpty()) return;
    if (m_skelVao == 0) glGenVertexArrays(1, &m_skelVao);
    if (m_skelVbo == 0) glGenBuffers(1, &m_skelVbo);
    glBindVertexArray(m_skelVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_skelVbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.constData(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<void*>(6 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<void*>(8 * sizeof(float)));
    glBindVertexArray(0);
    m_skelVerts = v.size() / 11;
}

// Rig hardpoints (attach sockets) overlay: an RGB axis gizmo at each socket, drawn in the SAME
// pose-aware space as the skeleton overlay (bone global · hardpoint bone-local), so it lines up
// with the mesh and follows animation. Layout: all X-axis lines, then Y, then Z (so paintGL can
// colour each group). Each socket contributes 2 verts per axis.
void GLModelWidget::buildHardpoints()
{
    m_hpAxisVerts = 0;
    const int nb = m_skeleton.size();
    if (m_hardpoints.isEmpty() || nb == 0) return;
    // Pose-aware bone globals (identical to buildSkeleton).
    QVector<Mat4> global(nb);
    for (int j = 0; j < nb; ++j) {
        const ModelJoint& jt = m_skeleton[j];
        Mat4 local;
        const int ai = m_hasAnim ? m_animByHash.value(jt.nameHash, -1) : -1;
        if (const int af = animFrameFor(ai); ai >= 0 && af < m_anim.bones[ai].rotations.size())
            local = composeTRS(m_anim.bones[ai].rotations[af].data(),
                               m_anim.bones[ai].translations[af].data(),
                               m_anim.bones[ai].scales[af].data());
        else
            local = composeTRS(jt.restQ.data(), jt.restT.data(), jt.restS.data());
        const int p = jt.parent;
        global[j] = (p >= 0 && p < j) ? mat4mul(global[p], local) : local;
    }
    // Axis length ~5% of the model radius, so gizmos read at any scale.
    const float len = qMax(0.02f, m_radius * 0.05f);
    const float kOne[3] = {1.0f, 1.0f, 1.0f};
    QVector<float> vx, vy, vz;   // three axis groups, 11-float layout (pos + padding attrs)
    auto push = [](QVector<float>& out, float x, float y, float z) {
        out << x << y << z << 0 << 1 << 0 << 0 << 0 << 1 << 0 << 0;
    };
    // Hardpoint semantics (verified against d4data mount rigs): an AUTHORED (non-identity)
    // transform is the socket's rest placement in MODEL space — nBoneIndex is only the bone it
    // FOLLOWS in animation — so it's skinned like a vertex: global · inverseBind · hpRest
    // (= hpRest exactly at rest). An IDENTITY transform means "the bone itself" (player rigs).
    auto hpAuthored = [](const ModelHardpoint& hp) {
        return std::fabs(hp.t[0]) > 1e-4f || std::fabs(hp.t[1]) > 1e-4f || std::fabs(hp.t[2]) > 1e-4f
            || std::fabs(hp.q[0]) > 1e-3f || std::fabs(hp.q[1]) > 1e-3f || std::fabs(hp.q[2]) > 1e-3f;
    };
    for (const ModelHardpoint& hp : m_hardpoints) {
        if (hp.boneIndex < 0 || hp.boneIndex >= nb) continue;
        const Mat4 m = hpAuthored(hp)
            ? mat4mul(mat4mul(global[hp.boneIndex], m_skeleton[hp.boneIndex].inverseBind),
                      composeTRS(hp.q.data(), hp.t.data(), kOne))
            : global[hp.boneIndex];
        const float ox = m[12], oy = m[13], oz = m[14];
        push(vx, ox, oy, oz); push(vx, ox + len*m[0], oy + len*m[1], oz + len*m[2]);   // X
        push(vy, ox, oy, oz); push(vy, ox + len*m[4], oy + len*m[5], oz + len*m[6]);   // Y
        push(vz, ox, oy, oz); push(vz, ox + len*m[8], oy + len*m[9], oz + len*m[10]);  // Z
    }
    if (vx.isEmpty()) return;
    QVector<float> v; v.reserve(vx.size() * 3);
    v << vx << vy << vz;
    if (m_hpVao == 0) glGenVertexArrays(1, &m_hpVao);
    if (m_hpVbo == 0) glGenBuffers(1, &m_hpVbo);
    glBindVertexArray(m_hpVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_hpVbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.constData(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<void*>(6 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<void*>(8 * sizeof(float)));
    glBindVertexArray(0);
    m_hpAxisVerts = vx.size() / 11;   // verts per axis group
}

// Socket-name labels, projected like the bone labels (reuses BoneLabelOverlay).
void GLModelWidget::updateHardpointLabels()
{
    if (!m_hpLabels) m_hpLabels = new BoneLabelOverlay(this);
    auto* ov = static_cast<BoneLabelOverlay*>(m_hpLabels);
    ov->items.clear();
    const int nb = m_skeleton.size();
    if (!m_showHardpoints || m_hardpoints.isEmpty() || nb == 0) { ov->hide(); return; }
    QVector<Mat4> global(nb);
    for (int j = 0; j < nb; ++j) {
        const ModelJoint& jt = m_skeleton[j];
        Mat4 local;
        const int ai = m_hasAnim ? m_animByHash.value(jt.nameHash, -1) : -1;
        if (const int af = animFrameFor(ai); ai >= 0 && af < m_anim.bones[ai].rotations.size())
            local = composeTRS(m_anim.bones[ai].rotations[af].data(),
                               m_anim.bones[ai].translations[af].data(),
                               m_anim.bones[ai].scales[af].data());
        else
            local = composeTRS(jt.restQ.data(), jt.restT.data(), jt.restS.data());
        const int p = jt.parent;
        global[j] = (p >= 0 && p < j) ? mat4mul(global[p], local) : local;
    }
    const int W = width(), H = height();
    const float kOne[3] = {1.0f, 1.0f, 1.0f};
    // Same semantics as buildHardpoints: authored transform = model-space rest placement skinned
    // to the follow bone; identity = the bone itself.
    auto hpAuthored = [](const ModelHardpoint& hp) {
        return std::fabs(hp.t[0]) > 1e-4f || std::fabs(hp.t[1]) > 1e-4f || std::fabs(hp.t[2]) > 1e-4f
            || std::fabs(hp.q[0]) > 1e-3f || std::fabs(hp.q[1]) > 1e-3f || std::fabs(hp.q[2]) > 1e-3f;
    };
    for (const ModelHardpoint& hp : m_hardpoints) {
        if (hp.boneIndex < 0 || hp.boneIndex >= nb) continue;
        const Mat4 m = hpAuthored(hp)
            ? mat4mul(mat4mul(global[hp.boneIndex], m_skeleton[hp.boneIndex].inverseBind),
                      composeTRS(hp.q.data(), hp.t.data(), kOne))
            : global[hp.boneIndex];
        const QVector4D clip = m_lastViewProj * QVector4D(m[12], m[13], m[14], 1.0f);
        if (clip.w() <= 0.0001f) continue;
        const float sx = (clip.x() / clip.w() * 0.5f + 0.5f) * W;
        const float sy = (1.0f - (clip.y() / clip.w() * 0.5f + 0.5f)) * H;
        if (sx < -40 || sy < -20 || sx > W + 40 || sy > H + 20) continue;
        ov->items.append({QPoint(int(sx), int(sy)), hp.name, true});
    }
    ov->setGeometry(rect());
    ov->show(); ov->raise(); ov->update();
}

// Only the PHYSICS (cloth) bones, using the live simulated bone matrices: a connection line
// from each cloth bone to its parent (orange), then an RGB axis gizmo at each head oriented by
// the bone's actual simulated rotation (X=red, Y=green, Z=blue) so you can see it roll/twist.
void GLModelWidget::buildPhysBones()
{
    m_physVerts = 0; m_physConnVerts = 0; m_physPinnedVerts = 0; m_physAxisVerts = 0;
    m_physFreeVerts = 0; m_physTouchVerts = 0;
    const int nb = m_skeleton.size();
    // NOTE: no m_baseBones guard here — a standalone piece (Models tab) has baseBones 0 or nb and
    // identifies its cloth bones from the authored cages instead, exactly like buildSpringBones.
    // The old guard made this overlay silently unavailable for every single-piece model.
    if (nb == 0) return;
    const int ovStart = (m_baseBones > 0 && m_baseBones < nb) ? m_baseBones : 0;
    auto isClothBone = [&](int j) {
        if (m_sbBuilt && j < m_sbIsCloth.size()) return m_sbIsCloth[j] != 0;
        return j >= ovStart;
    };
    // Prefer the post-sim matrices (real swing rotation); fall back to the rest/anim pose if
    // skinning hasn't run yet (static view), so the overlay still appears.
    QVector<std::array<float, 16>> fallback;
    if (m_boneGlobalSim.size() < nb) {
        fallback.resize(nb);
        for (int j = 0; j < nb; ++j) {
            const ModelJoint& jt = m_skeleton[j];
            Mat4 local;
            const int ai = m_hasAnim ? m_animByHash.value(jt.nameHash, -1) : -1;
            if (const int af = animFrameFor(ai); ai >= 0 && af < m_anim.bones[ai].rotations.size()) {
                const auto& ba = m_anim.bones[ai];
                local = composeTRS(ba.rotations[af].data(), ba.translations[af].data(), ba.scales[af].data());
            } else {
                local = composeTRS(jt.restQ.data(), jt.restT.data(), jt.restS.data());
            }
            const int p = jt.parent;
            fallback[j] = (p >= 0 && p < j) ? mat4mul(fallback[p], local) : local;
        }
    }
    const auto& G = (m_boneGlobalSim.size() >= nb) ? m_boneGlobalSim : fallback;
    QVector<float> v;
    auto push = [&](float x, float y, float z) {
        v << x << y << z << 0 << 1 << 0 << 0 << 0 << 1 << 0 << 0;
    };
    // Group 0: connection lines, split into ANCHORED (grey) first, then FREE (orange), so the
    // draw can colour them differently. A bone is anchored when the solver flagged it kinematic
    // (invMass 0) OR it is a chain root — a cloth bone whose parent is a body/base bone, i.e. the
    // point where the chain attaches to the skeleton. Jewelry hangs from a base bone and has no
    // invMass-pinned cloth bones, so without the chain-root rule it would read as all-free.
    auto pinned = [&](int j) {
        if (m_sbBuilt && j < m_sbPin.size() && m_sbPin[j]) return true;   // kinematic pin
        const int p = m_skeleton[j].parent;
        return (p >= 0 && !isClothBone(p));                              // chain root → body bone
    };
    // A chain root's parent is a BODY bone — one shared attachment point for the whole garment
    // (measured: 7 cape roots on bone 5, 7 skirt roots on bone 164). Drawing the full parent→child
    // line for each produced a dozen long spokes radiating from the chest and hip, which reads as
    // "bones flying off the model" even though every bone is where it belongs. Draw a short stub
    // toward the attachment instead: the anchor is still legible, the false explosion is gone.
    // Genuine cloth→cloth segments (invMass-pinned bones) keep their full line.
    const float kStubMax = qMax(0.004f, m_radius * 0.02f);
    for (int j = ovStart; j < nb; ++j) {                // anchored points first
        const int p = m_skeleton[j].parent;
        if (p < 0 || p >= nb || !isClothBone(j) || !pinned(j)) continue;
        float sx = G[p][12], sy = G[p][13], sz = G[p][14];
        if (!isClothBone(p)) {                          // attachment spoke → shorten to a stub
            const float dx = sx-G[j][12], dy = sy-G[j][13], dz = sz-G[j][14];
            const float d  = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (d > kStubMax) {
                const float t = kStubMax / d;
                sx = G[j][12] + dx*t; sy = G[j][13] + dy*t; sz = G[j][14] + dz*t;
            }
        }
        push(sx, sy, sz);
        push(G[j][12], G[j][13], G[j][14]);
    }
    m_physPinnedVerts = v.size() / 11;
    // CONTACT groups: free bones split by their last contact state so the overlay SHOWS whether a
    // bone is resting on the body (yellow) or wedged inside it (red) instead of leaving you to
    // infer it from the silhouette.
    auto emitFree = [&](quint8 want) {
        for (int j = ovStart; j < nb; ++j) {
            const int p = m_skeleton[j].parent;
            if (p < 0 || p >= nb || !isClothBone(j) || pinned(j)) continue;
            const quint8 st = (j < m_sbContact.size()) ? m_sbContact[j] : 0;
            if (st != want) continue;
            push(G[p][12], G[p][13], G[p][14]);
            push(G[j][12], G[j][13], G[j][14]);
        }
    };
    emitFree(0);   // free — orange
    m_physFreeVerts = v.size() / 11;
    emitFree(1);   // touching — yellow
    m_physTouchVerts = v.size() / 11;
    emitFree(2);   // penetrating — red
    m_physConnVerts = v.size() / 11;
    // ── DIAGNOSTIC (env D4_DUMP_CLOTH=1). Reports the longest lines this overlay actually draws,
    // with the identity of both endpoints. Screenshots cannot tell us whether a stray line comes
    // from a bad parent link, an unsimulated bone or a diverging solver; this can.
    if (qEnvironmentVariableIsSet("D4_DUMP_CLOTH")) {
        static qint64 lastDump = -100000;
        const qint64 now = m_clothClock.isValid() ? m_clothClock.elapsed() : 0;
        if (now - lastDump > 2000) {
            lastDump = now;
            QVector<QPair<float,int>> len;
            for (int j = ovStart; j < nb; ++j) {
                const int p = m_skeleton[j].parent;
                if (p < 0 || p >= nb || !isClothBone(j)) continue;
                const float dx = G[j][12]-G[p][12], dy = G[j][13]-G[p][13], dz = G[j][14]-G[p][14];
                len.append(qMakePair(std::sqrt(dx*dx+dy*dy+dz*dz), j));
            }
            std::sort(len.begin(), len.end(),
                      [](const QPair<float,int>& a, const QPair<float,int>& b){ return a.first > b.first; });
            qInfo("cloth-overlay: nb=%d ovStart=%d baseBones=%d sbBuilt=%d clothEnabled=%d "
                  "drawn=%d simBones=%d radius=%.3f usingSimMatrices=%d",
                  nb, ovStart, m_baseBones, (int)m_sbBuilt, (int)m_clothEnabled,
                  (int)len.size(), (int)m_sbOrder.size(), m_radius,
                  (int)(m_boneGlobalSim.size() >= nb));
            for (int k = 0; k < qMin(10, (int)len.size()); ++k) {
                const int j = len[k].second, p = m_skeleton[j].parent;
                // cage/vert of the bone AND of its parent: a mismatch means the bone is driven by
                // a different garment's simulation than the rest of its chain.
                const int cg  = (j < m_sbAnchorPiece.size()) ? m_sbAnchorPiece[j] : -1;
                const int cv  = (j < m_sbAnchorVert.size())  ? m_sbAnchorVert[j]  : -1;
                const int pcg = (p < m_sbAnchorPiece.size()) ? m_sbAnchorPiece[p] : -1;
                qInfo("  len=%.3f  bone %d '%s'  parent %d '%s'  pos(%.3f %.3f %.3f) "
                      "parentPos(%.3f %.3f %.3f)  inSim=%d pin=%d driven=%d isCloth(parent)=%d "
                      "cage=%d vert=%d parentCage=%d driveW=%.2f%s",
                      len[k].first, j, qPrintable(m_skeleton[j].name), p, qPrintable(m_skeleton[p].name),
                      G[j][12], G[j][13], G[j][14], G[p][12], G[p][13], G[p][14],
                      (int)m_sbOrder.contains(j),
                      (int)(j < m_sbPin.size()    ? m_sbPin[j]    : 0),
                      (int)(j < m_sbDriven.size() ? m_sbDriven[j] : 0),
                      (int)isClothBone(p), cg, cv, pcg,
                      (j < m_sbAnchorW.size() ? m_sbAnchorW[j] : -1.0f),
                      (cg >= 0 && pcg >= 0 && cg != pcg) ? "  <<< CAGE MISMATCH" : "");
            }
        }
    }
    // Groups 1-3: per-bone local X / Y / Z axes (the simulated rotation basis), drawn from the head.
    const float len = 0.028f;
    auto axisGroup = [&](int col0) {
        for (int j = ovStart; j < nb; ++j) {
            const int p = m_skeleton[j].parent;
            if (p < 0 || p >= nb || !isClothBone(j)) continue;
            const auto& m = G[j];
            float ax = m[col0 + 0], ay = m[col0 + 1], az = m[col0 + 2];
            const float l = std::sqrt(ax*ax + ay*ay + az*az);
            if (l > 1e-6f) { ax /= l; ay /= l; az /= l; }
            push(m[12], m[13], m[14]);
            push(m[12] + ax*len, m[13] + ay*len, m[14] + az*len);
        }
    };
    axisGroup(0); axisGroup(4); axisGroup(8);
    m_physAxisVerts = (v.size() / 11 - m_physConnVerts) / 3;
    if (v.isEmpty()) return;
    if (m_physVao == 0) glGenVertexArrays(1, &m_physVao);
    if (m_physVbo == 0) glGenBuffers(1, &m_physVbo);
    glBindVertexArray(m_physVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_physVbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.constData(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 11*sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 11*sizeof(float), reinterpret_cast<void*>(3*sizeof(float)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 11*sizeof(float), reinterpret_cast<void*>(6*sizeof(float)));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 11*sizeof(float), reinterpret_cast<void*>(8*sizeof(float)));
    glBindVertexArray(0);
    m_physVerts = v.size() / 11;
}

// Wireframe of the cloth body-collision capsules (debug overlay): a ring at each end
// + longitudinal lines (a cylinder; endpoints approximate the hemispheres).
void GLModelWidget::buildColliderLines()
{
    m_colLineVerts = 0;
    if (!m_clothBuilt) buildClothSim();
    const int n = m_colR0.size();
    if (n == 0) return;
    const bool anim = m_hasAnim && m_colP0.size() == n * 3;
    QVector<float> v;
    auto push = [&](float x, float y, float z) {
        v << x << y << z << 0 << 1 << 0 << 0 << 0 << 1 << 0 << 0;
    };
    auto seg = [&](const float* a, const float* b) { push(a[0],a[1],a[2]); push(b[0],b[1],b[2]); };
    constexpr int N = 14;
    // Emit capsules GROUPED BY BODY REGION into contiguous vertex ranges, so paintGL can draw one
    // colour per region (same technique the phys-bone overlay uses for contact state). Without
    // this the six region sliders are unattributable — you cannot see which capsule a slider moved,
    // and a rig whose capsules all fall to "Other" looks identical to one that is correctly
    // classified. m_colRegionSpan[r] = one-past-the-last vertex of region r.
    m_colRegionSpan.fill(0, ClothParams::CapRegionCount);
    QVector<int> order;
    order.reserve(n);
    for (int r = 0; r < ClothParams::CapRegionCount; ++r)
        for (int i = 0; i < n; ++i) {
            const quint8 rg = (i < m_colRegion.size()) ? m_colRegion[i] : quint8(ClothParams::CapOther);
            if (rg == r) order.push_back(i);
        }
    int emitted = 0, curRegion = 0;
    for (int oi = 0; oi < order.size(); ++oi) {
        const int i = order[oi];
        {
            const quint8 rg = (i < m_colRegion.size()) ? m_colRegion[i] : quint8(ClothParams::CapOther);
            while (curRegion < rg) m_colRegionSpan[curRegion++] = emitted;
        }
        const float* p0 = (anim ? m_colP0.constData() : m_colP0Bind.constData()) + i*3;
        const float* p1 = (anim ? m_colP1.constData() : m_colP1Bind.constData()) + i*3;
        // Must mirror the solver's rScale exactly, or the Collision-model overlay draws capsules
        // the solver isn't using — a debug view that lies is worse than none.
        // Includes the per-region trim, so the overlay shows what the region sliders actually do.
        const quint8 rg = (i < m_colRegion.size()) ? m_colRegion[i] : quint8(ClothParams::CapOther);
        const float ovScale = ((m_colAuthored && m_capsFullSize) ? 1.0f : m_cloth.capsuleRadius)
                            * ((rg < ClothParams::CapRegionCount) ? m_cloth.capRegion[rg] : 1.0f);
        const float r0 = m_colR0[i] * ovScale;   // tapered radius (live-scaled if skin-fit)
        const float r1 = m_colR1[i] * ovScale;
        float dx = p1[0]-p0[0], dy = p1[1]-p0[1], dz = p1[2]-p0[2];
        float dl = std::sqrt(dx*dx+dy*dy+dz*dz);
        if (dl > 1e-5f) { dx/=dl; dy/=dl; dz/=dl; } else { dx=0; dy=1; dz=0; }
        // two perpendicular basis vectors
        float ux, uy, uz;
        if (std::fabs(dy) < 0.99f) { ux = dz; uy = 0; uz = -dx; } else { ux = 1; uy = 0; uz = 0; }
        float ul = std::sqrt(ux*ux+uy*uy+uz*uz); ux/=ul; uy/=ul; uz/=ul;
        const float wx = dy*uz - dz*uy, wy = dz*ux - dx*uz, wz = dx*uy - dy*ux;
        float ring0[N+1][3], ring1[N+1][3];
        for (int k = 0; k <= N; ++k) {
            const float a = float(k) / N * 6.2831853f, c = std::cos(a), s = std::sin(a);
            ring0[k][0]=p0[0]+r0*(c*ux+s*wx); ring0[k][1]=p0[1]+r0*(c*uy+s*wy); ring0[k][2]=p0[2]+r0*(c*uz+s*wz);
            ring1[k][0]=p1[0]+r1*(c*ux+s*wx); ring1[k][1]=p1[1]+r1*(c*uy+s*wy); ring1[k][2]=p1[2]+r1*(c*uz+s*wz);
        }
        for (int k = 0; k < N; ++k) { seg(ring0[k], ring0[k+1]); seg(ring1[k], ring1[k+1]); }
        for (int k = 0; k < N; k += N/4) seg(ring0[k], ring1[k]);   // 4 longitudinal lines
        emitted = v.size() / 11;          // running end-of-region marker
    }
    if (v.isEmpty()) return;
    if (m_colVao == 0) glGenVertexArrays(1, &m_colVao);
    if (m_colVbo == 0) glGenBuffers(1, &m_colVbo);
    glBindVertexArray(m_colVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_colVbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.constData(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<void*>(6 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<void*>(8 * sizeof(float)));
    glBindVertexArray(0);
    while (curRegion < ClothParams::CapRegionCount) m_colRegionSpan[curRegion++] = emitted;
    m_colLineVerts = v.size() / 11;
}

void GLModelWidget::buildGrid()
{
    const float r = qMax(0.001f, m_radius);
    const float ext = r * 2.0f;
    const float step = r * 0.25f;
    const float y = m_homeCenter.y() - r * 1.02f;
    const float cx = m_homeCenter.x(), cz = m_homeCenter.z();
    QVector<float> v;
    auto line = [&](float x0, float z0, float x1, float z1) {
        v << x0 << y << z0 << 0 << 1 << 0 << 0 << 0 << 1 << 0 << 0;   // pos,normal,uv,tangent
        v << x1 << y << z1 << 0 << 1 << 0 << 0 << 0 << 1 << 0 << 0;
    };
    for (float t = -ext; t <= ext + 1e-3f; t += step) {
        if (std::abs(t) < step * 0.5f) continue;   // the two centre lines are appended LAST (axes)
        line(cx - ext, cz + t, cx + ext, cz + t);
        line(cx + t, cz - ext, cx + t, cz + ext);
    }
    // World-axis lines at the tail of the buffer so the draw can tint them separately
    // (Blender-style: X red, Z blue). Order: X line first, then Z.
    m_gridAxisFirst = v.size() / 11;
    line(cx - ext, cz, cx + ext, cz);   // along X
    line(cx, cz - ext, cx, cz + ext);   // along Z
    if (m_gridVao == 0) glGenVertexArrays(1, &m_gridVao);
    if (m_gridVbo == 0) glGenBuffers(1, &m_gridVbo);
    glBindVertexArray(m_gridVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_gridVbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.constData(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<void*>(6 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<void*>(8 * sizeof(float)));
    glBindVertexArray(0);
    m_gridVerts = v.size() / 11;
}

void GLModelWidget::wheelEvent(QWheelEvent* e)
{
    const float steps = e->angleDelta().y() / 120.0f;
    m_dist *= std::pow(0.9f, steps);
    // Keep a minimum so you can't zoom through the model, but no maximum — some models are huge and
    // need to be pulled back far (the far plane grows with distance to match).
    m_dist = qMax(m_radius * 0.05f, m_dist);
    update();
}
