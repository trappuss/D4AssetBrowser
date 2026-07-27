#pragma once
#include "model/AnimParser.h"
#include "model/ModelGeometry.h"

#include <QByteArray>
#include <QColor>
#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLWidget>
#include <QString>
#include <QVector3D>
#include <QVector4D>

#include <array>
#include <memory>

class QLabel;
class QOpenGLFramebufferObject;

// Renders a parsed ModelGeometry with an orbit camera and a simple headlight
// Lambert shader. Same GL surface as the rest of the tool (QOpenGLWidget +
// QOpenGLFunctions_4_5_Core, #version 450 core) — this is the in-app 3D preview
// matching d4analyzer's Models viewport. Consumes the exact ModelGeometry the
// .glb exporter does, so geometry shown == geometry exported.
class GLModelWidget : public QOpenGLWidget, protected QOpenGLFunctions_4_5_Core {
    Q_OBJECT
public:
    explicit GLModelWidget(QWidget* parent = nullptr);
    ~GLModelWidget() override;

    // Upload geometry (flattened to one position+normal buffer). Auto-frames the
    // camera on the model's bounding box, unless keepView is set (preserve the current
    // orbit/zoom — used when re-assembling a model the user is already inspecting).
    void setGeometry(const ModelGeometry& geo, bool keepView = false);
    void clearGeometry();

    // GPU/driver string captured at first GL init — Help ▸ Copy diagnostic info reads it.
    static QString glInfo() { return s_glInfo; }

    // Render the current (bind-pose) model to a square thumbnail. Null if empty.
    QImage grabThumbnail(int size = 64);
    // Ensemble tile: the FULL render pipeline (textures/dyes/fur) in the BASE COLOUR channel —
    // unlit flat colour so the tile reads as the outfit's palette — framed from the FRONT (+X)
    // view with grid/skeleton/gradient suppressed. State is restored after the grab.
    QImage grabEnsembleThumb(int size = 128);

    // Per-primitive (sub-object) visibility, for the parts tree.
    int     partCount() const { return m_parts.size(); }
    int     partTriangles(int i) const;   // triangle count of part i
    void    setPartVisible(int i, bool on);
    bool    partVisible(int i) const;             // current viewport visibility of part i
    // Highlighting is drawn as a SEE-THROUGH OUTLINE, not a flat tint: a tint destroys the very
    // textures you are inspecting, and is invisible when the part is behind other geometry.
    void    setHighlightPart(int i);              // -1 = none; RED outline (parts-list selection)
    void    setHighlightParts(const QList<int>& parts);   // RED outline over a set
    void    setPickedPart(int i);                 // -1 = none; BLUE outline (right-clicked part)
    int     pickedPart() const { return m_pickedPart; }
    // Per-part base-colour textures (index = part = source primitive). Null image →
    // that part renders flat grey. Uploaded to GL on the next paint.
    void    setPartTextures(const QVector<QImage>& baseColor);
    // VRAM texture pool: when enabled, uploaded per-part textures are kept resident in a bounded
    // GPU-side pool keyed by role|material-key, so an outfit change that only swaps one item reuses
    // the other materials' textures instead of re-uploading them. setPartMatKeys() supplies one
    // stable key per part (material name + a colour epoch); parts with the same key share a texture.
    void    setVramPoolEnabled(bool on);
    void    setPartMatKeys(const QVector<QString>& keys);
    void    setOverlayText(const QString& text);   // centered viewport hint / "Loading…" (empty hides)
    void    setPartNormals(const QVector<QImage>& normalMaps);   // per-part normal maps
    void    setPartOrm(const QVector<QImage>& orm);   // packed AO(R)/rough(G)/metal(B)
    void    setPartEmissive(const QVector<QImage>& emissive);   // per-part emissive maps
    void    setPartEmissiveMult(const QVector<float>& mult);    // authored "emissive multiplier" per part
    void    setPartEmissiveColor(const QVector<float>& rgb3);   // authored "emissive color" (3 floats/part)
    void    setPartDetailIntensity(const QVector<float>& normalInt, const QVector<float>& roughInt);
    void    setPartDetailROffset(const QVector<float>& roughOffset);   // authored roughness bias per part
    void    setPartDetailColorAdd(const QVector<float>& colorAdd);      // detail albedo-tint intensity per part
    void    setPartDetailScales(const QVector<QVector3D>& scales);      // per-map tiling (x/y/z = map0/1/2)
    void    setPartDetailMetalLayer(const QVector<int>& metalLayer);    // which detail map is metal (-1 none)

    // Global detail-map EXPERIMENT config (the "Detail maps" panel). NOT per-item: these are
    // exploration knobs to discover the correct game-data rule, then bake it. Defaults reproduce
    // the current shipped behaviour exactly (zone1→map0, zone2→map1, zone3→map2; auto metal route).
    struct DetailConfig {
        bool  autoMode   = true;            // true = per-part values derived from game data (below)
        int   zoneMap[4] = {-1, 0, 1, 2};   // dye-zone band index → detail-map layer (-1 = none)
        float bands[4]   = {0.063f, 0.345f, 0.596f, 0.831f};  // dye-mask value band centres
        float metalThresh = 0.5f;           // metalness above which a texel counts as metal
        int   metalRoute  = -2;             // -2 = auto (by name), -1 = off, 0/1/2 = force a map
    };
    void    setDetailConfig(const DetailConfig& c) { m_detailCfg = c; update(); }
    DetailConfig detailConfig() const { return m_detailCfg; }
    // Per-part game-data-derived selection (used when DetailConfig.autoMode is true): the dye-mask
    // value band centres detected from each part's actual DYE_MASK texture, and the zone→map table
    // derived from which detail maps are present (metal excluded, non-metal clamped to the last).
    void    setPartDetailBands(const QVector<QVector4D>& bands);   // per-part 4 band centres
    void    setPartDetailZoneMap(const QVector<QVector4D>& zoneMap); // per-part 4 layers (ints as floats)
    void    setPartDetailNormals(const QVector<QImage>& m0, const QVector<QImage>& m1,
                                 const QVector<QImage>& m2);    // up-to-3 tiled detail normals
    void    setPartDetailRoughs(const QVector<QImage>& m0, const QVector<QImage>& m1,
                                const QVector<QImage>& m2);     // up-to-3 tiled detail roughness
    void    setPartTranslucency(const QVector<QImage>& maps);   // SSS / translucency colour
    void    setPartMask(const QVector<QImage>& maps);           // MASK_PRIMARY
    void    setPartDyeMask(const QVector<QImage>& maps);        // DYE_MASK
    void    setPartDyeRamp(const QVector<QImage>& maps);        // DYE_RAMP
    void    setPartDyeRegion(const QVector<int>& region);       // per-part dye-colour index
    void    setPartFlags(const QVector<int>& hair, const QVector<int>& skin,
                         const QVector<int>& cloth = {});
    void    setPartHairParams(const QVector<float>& p);   // per hair part ×3: rough/spec/highlight-shift
    void    setPartEye(const QVector<int>& eye);   // per-part eyeball flag (wet-cornea shading)
    void    setPartHead(const QVector<int>& head); // per-part face-skin flag (warm Fresnel rim; body has none)
    void    setEyeParams(float irisRoughness);     // EyeColor flIrisRoughness
    void    setEmissiveScale(float v);             // global emissive-intensity multiplier
    void    setPartFx(const QVector<int>& fx);   // per-part FX flag (excluded from cloth sim)
    void    setPartFxNoise(const QVector<QImage>& maps);   // mesh-FX scrolling noise (NOISE_PROCEDURAL)
    void    setPartFxAdditive(const QVector<int>& add);    // per-FX-part additive-blend flag
    void    setPartFxParams(const QVector<float>& intensity, const QVector<float>& wobble,
                            const QVector<float>& fresnel, const QVector<float>& alpha,
                            const QVector<float>& saturation);   // authored per-part real values
    void    setFxIntensity(float v);             // mesh-FX brightness MULTIPLIER (×authored)
    void    setFxScrollSpeed(float v);           // mesh-FX UV scroll multiplier
    void    setFxWobble(float v);                // mesh-FX wobble MULTIPLIER (×authored)
    // Shell fur (Diablo's hero_opaque_fur_dualNoise): per-part flag + density/strand textures.
    void    setPartFur(const QVector<int>& fur);               // per-part fur flag
    void    setPartFurMask(const QVector<QImage>& maps);       // MASK_PRIMARY: R = density/length
    void    setPartFurNoise(const QVector<QImage>& maps);      // NOISE_PROCEDURAL: strand pattern
    void    setFurEnabled(bool on);                            // global force-off toggle
    void    setFurShells(int n);                               // shell layer count
    void    setFurLength(float frac);                          // extrusion (fraction of radius)
    void    setFurDensity(float tiling);                       // strand noise tiling
    void    setFurCoverage(float thresh);                      // FurMask density threshold (lower = fuller)
    void    setFurGravity(float frac);                         // tip droop (fraction of radius)
    void    setFurCurl(float frac);                            // lateral comb (fraction of radius)
    void    setPartFactors(const QVector<float>& metal, const QVector<float>& rough);
    void    setPbr(bool on);   // normal-map + specular shading on/off (View ▸ PBR)
    // Cook-Torrance feature toggles (Model preview viewport settings).
    void    setFeatureDetail(bool on);
    void    setFeatureSpecAA(bool on);   // geometric specular anti-aliasing (Toksvig)
    void    setFeatureSubsurface(bool on);
    void    setFeatureHair(bool on);
    void    setFeatureIbl(bool on);
    void    setFeatureMask(bool on);
    void    setFeatureTonemap(bool on);
    void    setFeatureDye(bool on);
    void    setDyeColor(int region, const QColor& c);   // region 0..3 (DYE_MASK channel)
    // Per-part dye override (used for per-slot pigments). `on[i]`=1 dyes part i with the 4
    // colours packed at colors12[i*12 .. i*12+11] (region 0..3, RGB each); on[i]=0 leaves part
    // i undyed regardless of the global dye. Empty vectors fall back to the global dye state.
    void    setPartDye(const QVector<int>& on, const QVector<float>& colors12);
    void    setEnvironment(int preset);                 // 0 Studio 1 Outdoor 2 Dungeon 3 Night
    void    setExposure(float v);
    void    setColorGrade(bool on, float contrast, float sat, float warmth);   // optional post-tonemap grade
    void    setColorGradeLut(const QImage& lut);        // real D4 grade LUT (256×16); empty/other size clears it
    void    setFov(float deg);                          // vertical FOV in degrees (default 45)

    // ── Lighting rig ─────────────────────────────────────────────────────────────
    // Three-point character-screen rig built from Diablo IV's real authored values
    // (base/meta/Light/FrontEnd_CharacterCreate_* — the campfire character/wardrobe
    // screen): a warm key (the fire), a cool back rim, and a cool front fill, plus a
    // hemisphere-ambient scale. Directions are camera-relative (a portrait rig that
    // tracks the orbit). Colours come from the chosen preset; the panel tunes intensity
    // and key direction.
    struct LightRig {
        int   preset       = 0;      // 0 D4 Wardrobe (campfire) · 1 Hero Direct · 2 Studio
        float keyInt       = 1.0f;   // warm key (the campfire spotlight)
        float rimInt       = 1.0f;   // cool back rim (edge separation)
        float fillInt      = 1.0f;   // cool front fill (opens the shadows)
        float ambInt       = 1.0f;   // hemisphere-ambient (IBL) scale
        float keyAzimuth   = 15.0f;  // deg, + = key swings to camera-right
        float keyElevation = 25.0f;  // deg above the camera horizon
    };
    void     setLightRig(const LightRig& r);
    LightRig lightRig() const { return m_rig; }

    // Reflection probe — a real Diablo IV cubemap (RGBA16F HDR) used for ambient specular
    // reflections instead of the analytic hemisphere. `faceOffsets` gives the byte offset of
    // each of the 6 faces' top mip in `payload` (faceSize = face width = height). The mip
    // chain is generated on the GPU. Empty payload clears it (→ analytic fallback).
    void setReflectionCubemap(const QByteArray& payload, int faceSize, const QVector<quint32>& faceOffsets);
    void setReflectionEnabled(bool on);
    void setReflectionStrength(float v);   // ambient-specular reflection intensity (1 = default)
    void setSkinWarmth(float v);           // skin SSS red-bleed amount (1 = default)
    void setSssStrength(float v);          // skin subsurface strength (0 = off, 1 = full)
    void setWetness(float v);              // rain-slick amount (0 = dry, 1 = fully wet)
    void setSnow(float v);                 // snow dusting on up-facing surfaces (0 = none)
    // Self-shadowing (key-light shadow map).
    void setShadowEnabled(bool on);
    void setShadowParams(float strength, float softnessTexels, float bias);
    void setShadowExtra(float rangeMul, float normalBiasFrac, int resolution);
    void setLightLock(bool worldFixed);
    // Screen-space ambient occlusion.
    void setSsaoEnabled(bool on);
    void setSsaoParams(float strength, float radiusFrac);

    // Animation: CPU-skin the bind-pose mesh by the decoded clip and show frame f.
    void setAnimation(const AnimParser::DecodedAnim& anim);
    void clearAnimation();
    void setFrame(int f);
    // Marks tracks [from, end) of the current animation as an attached model's, played on their own
    // looping timeline rather than the body clip's. Reset by every setAnimation().
    void setAttachAnimRange(int from, int frames, float fps);
    // Which frame index track `ai` should be sampled at. Attached tracks run on their own clock, so
    // every consumer of m_anim has to ask rather than reach for m_frame — the skeleton overlays
    // reached, and drew the attachment a phase behind the mesh once the two timelines diverged.
    int  animFrameFor(int ai) const
    { return (m_animAttachFrom >= 0 && ai >= m_animAttachFrom) ? m_frameAttach : m_frame; }
    int   animFrameCount() const { return m_hasAnim ? m_anim.frameCount : 0; }
    // Copy the CURRENT on-screen pose (the CPU-skinned verts) back into `geo` — positions +
    // normals, primitives walked in setGeometry's flatten order — and zero the skinning data so
    // exporters treat the result as a plain static mesh frozen in this pose. False on mismatch.
    bool snapshotPose(ModelGeometry& geo) const;
    float animFrameRate()  const { return m_hasAnim ? m_anim.frameRate : 0.0f; }  // for anim-loop GIF timing
    int   animFrame()      const { return m_frame; }                              // current frame

    // Cloth (NvCloth-style) sim parameters — live-tunable from the Physics panel.
    struct ClothParams {
        float gravity         = -0.025f;   // per-frame downward accel; slider stores +0.025 default (0 = authored .clt gravity)
        float damping         = 0.93f;     // velocity retention (lower = stiffer/less swing)
        float maxDistance     = 1.0f;      // swing limit from the skinned pose (×per-vertex
                                           // factor) — the NvCloth motion constraint. Per-piece
                                           // bone-tracking scales this down for chains/jewelry.
        float bendStiffness   = 0.47f;     // 0..1 resist folding (higher = stiffer)
        float stretchStiffness= 0.21f;     // 0..1 structural stiffness
        int   iterations      = 10;        // constraint solver iterations (user default)
        int   subSteps        = 2;         // integrate+solve passes per frame (1-4). Small steps
                                           // converge far better under fast motion than one big
                                           // step at any tuning; cost scales linearly.
        float selfCollision   = 0.004f;    // self-collision thickness (0 = off)
        float collisionMargin = 0.02f;     // extra body-collision clearance (closes gaps)
        float friction        = 0.30f;     // body-contact friction (0..1, grips the body)
        float backstop        = 0.020f;    // max sink toward the body behind the skinned surface
        float capsuleRadius   = 0.52f;     // live multiplier on ALL body-collision capsules.
                                           // VERIFIED AGAINST THE RENDER: ~0.52 is what actually
                                           // matches the mesh. It is NOT a workaround — the authored
                                           // capsule radii describe a collision volume noticeably
                                           // larger than the visible body (the game applies them with
                                           // its own margins), so feeding them in at 1.0 inflates the
                                           // body and splays skirts open with the legs showing through
                                           // the gap. Raise only if a piece grazes; lower if a garment
                                           // stands off the body.
        // PER-REGION capsule trim, applied ON TOP of capsuleRadius. The game authors a radius per
        // capsule per bone, so a skirt clipping the thighs is a LEGS problem — the global knob also
        // inflates the chest and arms, which is why tuning it alone never lands. 1.0 = untouched.
        enum CapRegion { CapLegs = 0, CapWaist, CapTorso, CapArms, CapHead, CapOther, CapRegionCount };
        float capRegion[CapRegionCount] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        // ── From the game's Cloth definition (.clt.json / dmClothTuningMirror). The cloth
        // mostly tracks the authored bone-skinned pose; physics is a light correction. ──
        float boneTracking    = 0.45f;     // flBoneTrackingFactor: blend toward skinned pose
        float actorTracking   = 0.6f;      // flActorTrackingFactor: inherit character rigid motion
        float horizStiffness  = 0.5f;      // flHorizontalStiffness (structural, lateral)
        float shearStiffness  = 0.15f;     // flShearStiffness (diagonal)
        float attachStiffness = 0.2f;      // flAttachmentStiffness: seam/anchor hold
        // ── Aerodynamics (dmClothTuningMirror). A constant authored "self-wind" gives the
        // cloth its idle drift; drag resists motion through the air (settles billowing). ──
        float windX = 0.0f, windY = 0.0f, windZ = 0.0f;  // vSelfWind × flWindFactor (model units/frame²)
        float dragFactor = 0.0f;           // flDragFactor: air resistance on particle velocity
        float liftFactor = 0.0f;           // flLiftFactor: lift perpendicular to motion (subtle)
        // Spring-bone solver: return-to-pose stiffness. Higher = the cloth holds its authored
        // (skinned) shape more / droops less; lower = hangs freely under gravity.
        float boneStiffness = 0.02f;
        // ── User-driven inertia ("React to rotation") ──────────────────────────────────────
        // Orbiting the camera reads as SPINNING THE MODEL, so the cloth gets the fictitious
        // forces of that rotation: free particles lag behind the turn (Euler force) and fan
        // outward while it continues (centrifugal). Off = the viewer never disturbs the sim.
        bool  userSpin      = false;
        float userSpinForce = 0.1f;   // 0..5 multiplier on the induced lag/centrifugal terms
    };
    void setClothParams(const ClothParams& p) { m_cloth = p; update(); }
    ClothParams clothParams() const { return m_cloth; }
    void setShowColliders(bool on) { m_showColliders = on; update(); }   // debug overlay
    // Master physics on/off. When off, cloth/sim verts render at their skinned pose
    // (the authored garment shape) with no simulation.
    void setClothEnabled(bool on) { m_clothEnabled = on; m_clothBuilt = false; update(); }
    bool clothEnabled() const { return m_clothEnabled; }
    // Which local axis the authored capsule extends along (0=X,1=Y,2=Z) — live-tunable
    // because the convention can't be read from data; rebuilds the capsules on change.
    void setCapsuleAxis(int a) { m_capAxis = a & 3; m_clothBuilt = false; update(); }
    // 1:1 cage solver: simulate the game's authored low-poly sim cages (authored pins +
    // constraints + capsules) and drive the high-poly render cloth from them. When off,
    // the heuristic render-mesh solver runs (the proven fallback). Live-switchable.
    // Bone-based cloth physics (spring bones on the cloth-bone chains). When off, cloth bones
    // hold their skinned (animated) pose — no sim.
    // (setBoneSim/boneSim removed — nothing ever called them; the cloth master toggle is the
    //  tabs' clothSim settings, applied through setClothParams.)
    // Back-face culling of the solid mesh (Preview Settings). On = cull back faces (hides
    // the inner wall of double-walled armor); off = render both sides.
    void setBackfaceCull(bool on) { m_backfaceCull = on; update(); }

    // Continuous turntable rotation of the orbit camera (the "Spin" toggle).
    void setAutoSpin(bool on);
    void setSpinSpeed(float radPerTick) { m_spinSpeed = radPerTick; }   // turntable rate
    void resetView();   // re-frame the camera (orbit + pan) on the model, resetting the angle
    // Re-fit the whole model. keepRotation=true holds the current orbit angle (re-centre + zoom
    // only, no rotation change); false is equivalent to resetView().
    void frameAll(bool keepRotation, bool animate = true);
    // Snap to a 3/4 "hero" framing (D4 wardrobe/shop look): orbit yaw/pitch in radians,
    // target raised by targetUpFrac × model radius, distance auto-fit to the current FOV.
    void frameThreeQuarter(float yawRad, float pitchRad, float targetUpFrac);
    // Frame an arbitrary region: orbit at yaw/pitch (radians) around `center`, distance
    // auto-fit so a sphere of `radius` fills the current FOV. `animate` glides there.
    void frameRegion(const QVector3D& center, float radius, float yawRad, float pitchRad, bool animate);
    // Same, but keep the CURRENT orbit angle (only move the target + distance).
    void frameRegionKeepRotation(const QVector3D& center, float radius, bool animate);
    // Bounding sphere of the given draw-parts in the CURRENT (live, animated) pose — so the
    // wardrobe's Camera Snap frames where a slot actually is, not its bind-pose position.
    // Returns false if none of the parts have on-screen vertices.
    bool partsBounds(const QVector<int>& partIndices, QVector3D& center, float& radius) const;
    // "Camera Snap and follow": keep re-centring the camera on these parts every animation
    // frame (empty list turns following off). Zoom/angle are preserved; only the target pans.
    void followParts(const QVector<int>& partIndices);
    // Orthographic vs perspective projection (Camera popup toggle).
    void setOrthographic(bool on) { m_ortho = on; update(); }
    bool orthographic() const { return m_ortho; }
    // Full camera state, for "remember camera on relaunch".
    struct CamState { float yaw = 0.6f, pitch = 0.3f, dist = 3.0f, fov = 45.0f;
                      float cx = 0.0f, cy = 0.0f, cz = 0.0f; bool ortho = false; bool valid = false; };
    CamState cameraState() const;
    void     setCameraState(const CamState& s);
    void     setOrbitYaw(float y) { m_yaw = y; update(); }   // drive turntable-GIF frames
    // Export capture: guarantee ONE deterministic cloth step per captured frame. Stops the idle
    // settle timer (the capture loop pumps events, so it would otherwise slip in extra steps at
    // wall-clock intervals) and stops treating programmatic yaw changes as user rotation. Both
    // caused visible twitching/stutter in exported GIFs. Always pair via CaptureScope.
    void     setCaptureMode(bool on);
    bool     capturing() const { return m_capturing; }
    // Run N EXTRA deterministic cloth steps at the current pose (no re-pose, no timer, no spin).
    // Capture advances the sim once per frame via setFrame(); live playback additionally gets idle
    // timer steps whenever a frame takes >60 ms, so an export is strictly less settled than the
    // preview it is supposed to reproduce. These are the missing settle steps.
    void     settleCloth(int steps);
    // Deterministic FX shader time for captures (seconds). Wall-clock uTime shimmers unevenly in
    // an export because frames are grabbed at irregular real intervals.
    void     setCaptureTime(float seconds) { m_captureTime = seconds; }
    // RAII pairing — exception- and early-return-safe.
    struct CaptureScope {
        GLModelWidget* v;
        explicit CaptureScope(GLModelWidget* w) : v(w) { if (v) v->setCaptureMode(true); }
        ~CaptureScope() { if (v) v->setCaptureMode(false); }
        CaptureScope(const CaptureScope&) = delete;
        CaptureScope& operator=(const CaptureScope&) = delete;
    };
    float    orbitYaw() const { return m_yaw; }
    // Ray-pick the front-most visible draw-part under a widget-space point (-1 = miss).
    int      pickPart(const QPoint& posPx) const;
    // View-settings toggles (from the View ▾ menu).
    void setShowTextures(bool on);
    void setViewChannel(int c);   // 0 shaded · 1 base · 2 normal · 3 rough · 4 metal · 5 AO · 6 emissive
    void setWireframe(bool on);
    void setShowGrid(bool on);
    void setShowSkeleton(bool on);
    void setHardpoints(const QVector<ModelHardpoint>& hps);   // rig attach sockets to overlay
    void setShowHardpoints(bool on);  // draw an RGB axis gizmo + label at each rig hardpoint
    void setShowPhysBones(bool on);   // overlay only the simulated cloth/physics bones
    void setShowPhysAxes(bool on);    // draw the per-bone XYZ axis gizmos
    void setShowBoneNames(bool on);        // draw a name label at each bone (viewport overlay)
    void setBoneNamesTranslated(bool on);  // labels use the hash→readable dictionary vs raw bone_<hash>
    void setBoneNamesHideUnknown(bool on); // only label bones whose name is known (hide raw bone_<hash>)
    static QString translateBoneName(quint32 nameHash);   // hash reverse-map ("" = unknown)
    static void    translateSkeletonNames(QVector<ModelJoint>& skeleton);  // bone_<hash> → readable (.glb export)
    static void    blenderizeSkeletonNames(QVector<ModelJoint>& skeleton); // Blender .L/.R names (mirror-paired)
    void setBackgroundColor(const QColor& c);   // viewport clear colour
    void setBackgroundGradient(bool on) { m_bgGradient = on; update(); }   // studio vertical wash
    QColor backgroundColor() const;
    // Clear the background with alpha 0 (for native-alpha screenshot/GIF capture). The captured
    // framebuffer then carries the model's coverage as alpha; restored to opaque after the grab.
    void setTransparentClear(bool on) { m_transparentClear = on; }
    // Bones below this index are authored/base; from it onward they're spring-simulated
    // (cloth/physics). Exposed for the outliner's "N phys" armature badge.
    int  baseBoneCount() const { return m_baseBones; }

    // ── Viewport guides (Blender-style orientation aids) ──
    float camYaw() const   { return m_yaw; }     // the axis-gizmo overlay reads these each repaint
    float camPitch() const { return m_pitch; }
    void  orbitToAxis(float yawRad, float pitchRad);   // glide to an axis view — keeps centre + zoom
    void  setShowAxisGizmo(bool on);              // clickable X/Y/Z ball, top-right corner
    void  setGridAxisColors(bool on);             // grid: X axis red, Z axis blue

signals:
    void partFocused(int part);   // double-click focus → the picked draw-part (or -1 on a miss)
    void partRightClicked(int part, const QPoint& globalPos);   // right-click (no drag) → picked part + menu anchor

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;       // right-click (no drag) → partRightClicked
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;   // focus the clicked part
    void wheelEvent(QWheelEvent*) override;

public:

private:
    void uploadPending();
    void uploadTextures();   // create GL textures from m_pendingTex
    void destroyBuffers();
    void destroyTextures();
    void drawFxParts(bool forceAdditive); // draw visible FX submeshes (additive option for accumulation)

    // pending CPU-side geometry (uploaded on next paintGL with a current context)
    bool                m_hasPending = false;
    QVector<float>      m_verts;       // interleaved px,py,pz,nx,ny,nz
    QVector<quint32>    m_indices;

    // One draw-range per source primitive (sub-object), toggleable for the parts tree.
    struct Part { int offset = 0; int count = 0; bool visible = true; QString name; };
    QVector<Part>       m_parts;
    QSet<int>           m_highlight;      // red outline
    int                 m_pickedPart = -1;  // blue outline — the part the user right-clicked
    QVector<int>        m_followParts;   // parts the camera keeps centred each anim frame (Camera Snap+follow)

    // Skinning data (kept so animation can re-deform the bind pose on the CPU).
    void applySkinning();
    QVector<float>                 m_bindVerts;   // bind-pose interleaved px..nz (immutable)
    QVector<std::array<quint16,4>> m_vJoints;     // per-vertex global bone indices
    QVector<std::array<float,4>>   m_vWeights;    // per-vertex weights
    QVector<ModelJoint>            m_skeleton;
    AnimParser::DecodedAnim        m_anim;
    QHash<quint32,int>             m_animByHash;  // bone hash → anim bone index
    // An ATTACHED model's clip rides in the same DecodedAnim (its tracks appended after the body's)
    // but on its OWN timeline. Projecting it onto the body clip's frame count made it restart every
    // time the body clip looped — a 130-frame trophy idle on a 56-frame body idle only ever played
    // its first 56 frames. Tracks at or past m_animAttachFrom are indexed by m_frameAttach instead,
    // derived from wall-clock at the clip's authored rate and wrapped on ITS own length.
    int                            m_animAttachFrom = -1;   // first attached track (-1 = none)
    int                            m_animAttachFrames = 0;
    float                          m_animAttachFps = 30.0f;
    int                            m_frameAttach = 0;
    bool                           m_hasAnim = false;
    int                            m_frame = 0;

    // ── Lightweight Verlet cloth sim (sim/cloth submeshes during animation) ──────
    void buildClothSim();   // (re)derive cloth vertices + edge constraints from parts
    ClothParams          m_cloth;                  // live cloth-sim tuning
    int                  m_baseBones = 0;          // bones < this are base; >= are cloth/physics
    bool                 m_clothEnabled = true;    // master physics on/off
    // Previous-frame capsule poses: the cage solve interpolates colliders across
    // sub-steps (NvCloth-style continuous colliders) so a fast-swinging limb SWEEPS
    // through the fabric instead of teleporting past it once per frame.
    QVector<float>       m_colP0Prev, m_colP1Prev;
    bool                 m_colPrevValid = false;
    int                  m_capAxis = 0;            // authored-capsule long axis (0=X,1=Y,2=Z,3=bone-dir)
    bool                 m_clothBuilt = false;     // lazily (re)built when geometry/flags change
    bool                 m_clothSeeded = false;    // sim positions initialised to the skinned pose
    QVector<quint8>      m_vCloth;                 // per-vertex: 1 = simulated cloth vertex
    QVector<int>         m_clothVerts;             // indices of the cloth vertices
    QVector<float>       m_clothPos, m_clothPrev;  // Verlet positions (vcount*3, cloth verts only)
    QVector<int>         m_clothEdgeA, m_clothEdgeB;
    QVector<float>       m_clothRest;              // rest length per edge (from bind pose)
    QVector<int>         m_clothTris;              // triangle base indices touching cloth
    QVector<int>         m_clothBendA, m_clothBendB;   // bending constraints (opposite verts)
    QVector<float>       m_clothBendRest;          // bending rest length (from bind pose)
    QVector<float>       m_clothMaxDist;           // per-vertex max distance from skinned pose
                                                   // (0 = pinned seam, large = free end)
    // Authored NvCloth collision capsules (from the game's ClothData). When present, the
    // sim collides against these exact bone-bound capsules instead of the skin-fit ones.
    QVector<ClothCapsule> m_authoredCaps;
    // Authored NvCloth sim cages (one per cloth piece). The 1:1 path simulates these
    // (authored pins + constraints + capsules) and drives the render skirt from them.
    QVector<ClothSim>    m_clothSims;
    QVector<int>         m_pinnedBones;   // bones held rigid by the physics solver (attached-prop base)
    // (The 1:1 cage-solver state block is deleted with the legacy vertex cloth path.)
    // ── Spring-bone cloth physics (RE-Chain / VRM style) ─────────────────────────
    // The cloth bones (skel index ≥ baseBones) form chains; we simulate each as a Verlet
    // particle (inertia + stiffness-to-animated-pose + gravity + rigid length + capsule
    // collision) and write the result back into the bone globals, so the skinned cape /
    // skirt / chains swing. This is the cloth solver (replaces the mesh-cage path).
    bool                 m_sbBuilt = false, m_sbSeeded = false;
    QVector<int>         m_sbOrder;       // cloth bone indices (skeleton), parent before child
    QVector<int>         m_sbChild;       // a representative child cloth bone (skel idx) or -1
    QVector<float>       m_sbLenParent;   // rest length from this bone's head to its parent's
    QVector<quint8>      m_sbIsCloth;     // per skeleton bone: 1 = simulated cloth bone
    QVector<float>       m_sbSimHead, m_sbPrevHead;   // per skeleton bone (nb*3): Verlet state
    // AUTHORED data mapped onto the bones (they coincide with cage particles ~1:1): the cloth
    // cage's distance-constraint network + invMass pins. This is what actually shapes the cloth.
    QVector<int>         m_sbConA, m_sbConB;   // authored constraint bone pairs (skeleton idx)
    QVector<float>       m_sbConRest;          // authored rest length per constraint
    QVector<quint8>      m_sbPin;              // per skeleton bone: 1 = pinned (authored invMass 0)
    QVector<float>       m_sbAttach;           // per skeleton bone: authored motion constraint 0..1
                                               // (ptAttachmentLengths) — 0 locked, 1 free to swing
    QVector<int>         m_sbSim;              // per skeleton bone: index into m_clothSims (its
                                               // cloth piece) for per-piece tuning, or -1
    QVector<quint8>      m_sbHair;             // per skeleton bone: 1 = driven by hair-part verts
                                               // (data-driven from skinning) → hair-class physics
    // Per cloth bone: 1 = the CURRENT clip genuinely animates it (real per-frame motion) so it must
    // follow the animation, not be sprung. 0 = its track is static/rest (the game leaves cloth bones
    // to NvCloth), so we SIMULATE it — fixes skirt chains that were clipping because a static rest
    // track made them "look animated" and rigidly track the leg. Rebuilt when the clip changes.
    QVector<quint8>      m_sbAnimMoves;
    bool                 m_sbAnimMovesBuilt = false;
    void computeAnimMoves();                   // fill m_sbAnimMoves from the current clip's tracks
    // ── Cage-level cloth sim (the game's NvCloth architecture) ──────────────────
    // The game simulates each authored low-poly SIM CAGE (40–136 particles per piece),
    // colliding EVERY cage particle against the authored capsules, with authored invMass
    // pins, the authored distance-constraint network and authored per-particle motion
    // limits (ptAttachmentLengths). Bone-head-only collision cannot do this: the fabric
    // spans BETWEEN bones (where a walking thigh pushes) had no collision at all, and the
    // old ≤3cm cage→bone matcher dropped most of a dense skirt cage's authored data.
    // So: simulate the cages at cage-vertex density, then DRIVE each cloth bone from its
    // nearest cage particle; the mesh follows through normal skinning. Pieces without a
    // cage (hair, loose chains) keep the spring-bone path.
    struct CageRt {
        int simIdx = -1;                        // index into m_clothSims
        QVector<std::array<quint16, 4>> J;      // per cage vert: borrowed skinning joints
        QVector<std::array<float, 4>>   W;      // per cage vert: borrowed skinning weights
        QVector<float> pos, prev, target;       // Verlet state + per-frame skinned target (nv*3)
        bool seeded = false;
    };
    QVector<CageRt>      m_cages;               // one per authored cage piece
    QVector<int>         m_sbAnchorPiece;       // per bone: m_cages index driving it, or -1
    QVector<int>         m_sbAnchorVert;        // per bone: cage vert index within that piece
    QVector<float>       m_sbAnchorW;           // per bone: how much of that particle's motion to
                                                // apply (1 = coincident, tapering with distance)
    QVector<float>       m_cageSpan;            // per ClothSim: world-space reach from the pinned
                                                // edge — the length a normalized attachLen of 1
                                                // actually refers to
    QVector<quint8>      m_sbDriven;            // per bone: 1 = follows the cage (skip bone sim)
    bool                 m_colAuthored = false; // capsules are authored (exact game radii — the
                                                // capsuleRadius slider only scales SKIN-FIT ones)
    void buildSpringBones();
    // global is the per-bone world-transform array (Mat4 == std::array<float,16> in the .cpp);
    // spelled out here because Mat4 is a .cpp-local alias not visible to the header.
    void springBoneStep(QVector<std::array<float, 16>>& global);
    // Authored plane colliders (ptPlaneDefs): bone-bound half-spaces the cloth stays in
    // front of. Built in bind pose, animated per-frame via the skinning palette (like caps).
    QVector<int>         m_planeBone;             // bone per plane
    QVector<float>       m_planePtBind, m_planeNmBind;  // bind point + (point+normal), 3 each
    QVector<float>       m_planePt, m_planeNm;    // animated world point + outward normal
    // Body-collision capsules (authored if available, else fitted from the body skin per
    // bone; cloth is pushed out of them so legs/hips don't clip through during animation).
    QVector<int>         m_colBoneA, m_colBoneB;   // capsule endpoint bones
    QVector<float>       m_colP0Bind, m_colP1Bind; // bind-pose endpoints (3 floats each)
    QVector<float>       m_colR0, m_colR1;         // tapered radii at each endpoint (flRadiusA/B)
    QVector<float>       m_colP0, m_colP1;         // per-frame animated endpoints (3 each)

    class QTimer* m_spinTimer = nullptr;   // drives the Spin turntable
    float         m_spinSpeed = 0.025f;    // radians per tick (turntable rate)
    class QTimer* m_fxTimer = nullptr;     // drives mesh-FX UV-scroll repaints
    QElapsedTimer m_fxClock;               // wall-clock for FX time uniform
    // Idle cloth-settle timer: steps the cloth sim + repaints while the animation is PAUSED, so
    // slider tweaks apply live. Auto-suppressed during playback (setFrame stamps m_lastFrameStep;
    // the timer skips a tick if a frame was skinned very recently) so it never double-steps.
    class QTimer* m_clothTimer = nullptr;
    QElapsedTimer m_clothClock;            // wall-clock; m_lastFrameStep marks the last setFrame skin
    qint64        m_lastFrameStep = -1000;
    void ensureClothTimer();               // start the idle timer once cloth exists

    // GL objects
    GLuint  m_prog = 0, m_vao = 0, m_vbo = 0, m_ibo = 0;
    int     m_indexCount = 0;

    // Uniform-location memo: glGetUniformLocation is a driver round-trip + string hash, and
    // paintGL/renderPos/renderShadow/drawFxParts query ~170 uniforms every frame. Locations are
    // fixed once a program links, so cache them per (program, name). Cleared when programs rebuild.
    QHash<GLuint, QHash<QByteArray, GLint>> m_uniLoc;
    GLint uni(GLuint prog, const char* name);
    // Persistent offscreen FBO reused by grabThumbnail — creating/destroying an FBO on every
    // thumbnail (dozens per scroll) stresses the driver and crashes some GPUs. Reuse one.
    std::unique_ptr<QOpenGLFramebufferObject> m_thumbFbo;
    int     m_thumbFboSize = 0;
    // Reflection-probe cubemap (uploaded on the GL thread from a pending CASC payload).
    QByteArray       m_pendingRefl;
    int              m_reflSize = 0;
    QVector<quint32> m_reflOffsets;
    bool             m_hasPendingRefl = false;
    GLuint           m_reflCube = 0;
    int              m_reflMaxMip = 0;
    bool             m_reflEnabled = true;
    float            m_reflStrength = 1.0f;   // reflection-intensity slider
    float            m_skinWarm     = 1.0f;   // skin SSS red-bleed slider
    float            m_sssStrength  = 0.24f;  // skin subsurface strength (fleshier, closer to in-game)
    float            m_wetness      = 0.0f;   // rain-slick amount (0 = dry)
    float            m_snow         = 0.0f;   // snow-dusting amount on up-facing surfaces
    // Self-shadow (key-light shadow map).
    GLuint           m_shadowProg = 0, m_shadowFbo = 0, m_shadowTex = 0;
    int              m_shadowSize = 2048;
    int              m_fbW = 1, m_fbH = 1;    // device-pixel framebuffer size (viewport restore)
    bool             m_shadowOn = true;
    float            m_shadowStr = 0.6f, m_shadowSoft = 1.5f, m_shadowBias = 0.0018f;
    float            m_shadowRange = 1.3f;     // ortho frustum half-size (× model radius)
    float            m_shadowNBias = 0.01f;    // normal-bias as a fraction of the model radius
    bool             m_shadowResDirty = false; // shadow texture needs reallocation (resolution change)
    bool             m_lightLock = false;      // true = lights fixed in world space (not camera-relative)
    bool             m_lockValid = false;      // frozen directions captured yet (recaptured on each lock)
    QVector3D        m_lockKey, m_lockRim, m_lockFill;   // world-space rig snapshot taken when locking
    void             renderShadow(const QMatrix4x4& lightMvp);
    // SSAO (world-position G-buffer prepass + in-shader hemisphere occlusion).
    GLuint           m_posProg = 0, m_posFbo = 0, m_posTex = 0, m_posDepth = 0;
    int              m_posW = -1, m_posH = -1;   // currently-allocated G-buffer size
    bool             m_ssaoOn = true;
    float            m_ssaoStr = 1.0f, m_ssaoRad = 0.30f;
    void             renderPos(const QMatrix4x4& mvp, const QMatrix4x4& model);
    void             liveBounds(QVector3D& center, float& radius) const;   // current animated-pose bounds
    void             uploadReflectionCubemap();
    QVector<GLuint>  m_partTex;       // per-part base-colour GL texture (0 = none)
    QVector<GLuint>  m_partNormTex;   // per-part normal-map GL texture (0 = none)
    QVector<GLuint>  m_partOrmTex;    // per-part ORM GL texture (0 = none)
    QVector<GLuint>  m_partEmisTex;   // per-part emissive GL texture (0 = none)
    QVector<GLuint>  m_partDetailNTex[3], m_partDetailRTex[3];   // up-to-3 detail normal/rough maps
    QVector<GLuint>  m_partTransTex, m_partMaskTex;
    QVector<GLuint>  m_partDyeMaskTex, m_partDyeRampTex;
    QVector<GLuint>  m_partFurMaskTex, m_partFurNoiseTex;   // shell-fur density + strand textures
    QVector<GLuint>  m_partFxNoiseTex;                      // mesh-FX scrolling noise
    // ── Optional VRAM texture pool (skip re-uploading unchanged materials across rebuilds) ──
    void             evictTexturePool();   // drop non-referenced pooled textures until under budget
    bool             m_vramPool = false;   // desired mode (set by setVramPoolEnabled)
    bool             m_poolActive = false; // mode of the textures currently held
    int              m_poolGen = 0;        // bumped each upload pass → LRU ordering
    qint64           m_texPoolBytes = 0;   // approx GPU bytes currently pooled
    qint64           m_texPoolBudget = 256LL * 1024 * 1024;   // hard cap (auto-evicted)
    QVector<QString> m_partMatKey;         // per-part stable key (material + colour epoch)
    QLabel*          m_overlayLabel = nullptr;   // empty-state / loading hint over the viewport
    QHash<QString, GLuint> m_texPool;      // "role|matKey" → GL texture (pool-owned)
    QHash<QString, qint64> m_texPoolSize;  // "role|matKey" → approx bytes
    QHash<QString, int>    m_texPoolLastGen;   // "role|matKey" → last build that used it
    QVector<int>     m_partFxAdditive;                      // per-FX-part: 1 = additive blend
    // Authored per-FX-part real values: Color Intensity, Vertex Offset, Fresnel Slope,
    // Alpha Brightness Global, Color Saturation.
    QVector<float>   m_partFxIntensity, m_partFxWobble, m_partFxFresnel, m_partFxAlpha, m_partFxSaturation;
    QVector<QImage>  m_pendingTex;    // base-colour images awaiting upload
    QVector<QImage>  m_pendingNorm;   // normal-map images awaiting upload
    QVector<QImage>  m_pendingOrm;    // ORM images awaiting upload
    QVector<QImage>  m_pendingEmis;   // emissive images awaiting upload
    QVector<QImage>  m_pendingDetailN[3], m_pendingDetailR[3];   // up-to-3 detail normal/rough maps
    QVector<QImage>  m_pendingTrans, m_pendingMask;
    QVector<QImage>  m_pendingDyeMask, m_pendingDyeRamp;
    QVector<QImage>  m_pendingFurMask, m_pendingFurNoise;   // shell-fur textures awaiting upload
    QVector<QImage>  m_pendingFxNoise;                      // mesh-FX noise awaiting upload
    QVector<int>     m_partHair, m_partSkin, m_partCloth;   // per-part shading flags (0/1)
    QVector<float>   m_partHairParams;   // per part ×3: hero_hair (rough, spec, highlight-shift)
    QVector<int>     m_partHead;                            // per-part face-skin flag (warm Fresnel rim)
    QVector<int>     m_partEye;                             // per-part eyeball flag (wet cornea)
    float            m_eyeRough = 0.10f;                    // EyeColor flIrisRoughness
    QVector<int>     m_partFx;                              // per-part FX flag (skip cloth sim)
    QVector<int>     m_partFur;                             // per-part fur flag (shell rendering)
    QVector<int>     m_partDyeRegion;          // per-part dye-colour index (0..3)
    QVector<int>     m_partDyeOn;              // per-part dye override enable (per-slot pigments)
    QVector<float>   m_partDyeColor;           // per-part 4×RGB dye colours (12 floats per part)
    GLuint           m_dyeGradTex = 0;         // real dye gradient LUT (0 = none)
    int              m_dyeMode = 0;            // 0 = custom colour, 1 = real dye gradient
    bool             m_hasPendingTex = false;
    bool             m_pbr = true;    // normal-map shading enabled
    bool             m_fDetail = true, m_fSubsurf = true, m_fHair = true;
    bool             m_fSpecAA = true;   // geometric specular anti-aliasing toggle
    bool             m_fIbl = true, m_fMask = false, m_fTonemap = false, m_fDye = false;
    // Shell fur. Length/gravity/curl are fractions of the model bounding radius
    // (scale-independent); tiling sets strand density across the UV.
    bool             m_furEnabled = true;
    int              m_furShells = 20;        // concentric shell layers (quality vs. fill)
    float            m_furLength = 0.022f;    // max extrusion as fraction of m_radius
    float            m_furGravity = 0.0045f;  // tip droop as fraction of m_radius
    float            m_furCurl = 0.0035f;     // lateral comb along the tangent (fraction of m_radius)
    float            m_furTiling = 30.0f;     // strand noise tiling
    float            m_furCoverage = 0.03f;   // FurMask density threshold (fur grows where R >= this)
    // Mesh FX (vfx_actor_*) global multipliers on the authored per-part values.
    float            m_fxIntensity = 1.0f;     // × authored Color Intensity
    float            m_fxScrollSpeed = 1.0f;   // UV scroll multiplier
    float            m_fxWobble = 1.0f;        // × authored Vertex Offset
    float            m_exposure = 1.0f;
    bool             m_colorGrade = false;     // optional post-tonemap colour grade (off by default)
    float            m_cgContrast = 1.05f, m_cgSat = 1.10f, m_cgWarmth = 0.03f;
    GLuint           m_lutTex = 0;             // real D4 grade LUT texture (256×16), 0 = none
    bool             m_hasLut = false;         // a valid LUT is uploaded → shader uses it
    bool             m_lutDirty = false;       // a new LUT is queued for upload on the next paint
    QImage           m_pendingLut;             // queued LUT image (uploaded in paintGL when ctx is current)
    float            m_dyeColor[4][3] = {{1,1,1},{1,1,1},{1,1,1},{1,1,1}};   // per-region tint
    // Environment preset (default = Outdoor, matching the previous hardcoded look).
    float            m_envSky[3] = {0.42f, 0.48f, 0.60f};
    float            m_envHor[3] = {0.30f, 0.30f, 0.33f};
    float            m_envGnd[3] = {0.12f, 0.11f, 0.10f};
    float            m_lightCol[3] = {3.0f, 3.0f, 3.0f};   // KEY light colour × intensity (set by applyRig)
    // Lighting rig: secondary rim/fill colours + ambient scale, derived from m_rig.
    LightRig         m_rig;
    float            m_rimCol[3]  = {0.0f, 0.0f, 0.0f};
    float            m_fillCol[3] = {0.0f, 0.0f, 0.0f};
    float            m_ambScale   = 1.0f;
    void             applyRig();   // recompute key/rim/fill colours + ambient scale from m_rig
    QVector<float>   m_partMetal, m_partRough;   // per-part PBR scalars
    QVector<float>   m_partEmisMul;              // per-part authored emissive multiplier
    QVector<float>   m_partEmisColor;            // per-part authored emissive colour (3 floats/part)
    QVector<float>   m_partDetailNInt, m_partDetailRInt;   // detail normal/rough summed strengths
    QVector<float>   m_partDetailROffset;                  // authored roughness bias per part
    QVector<float>   m_partDetailCAdd;                      // detail albedo-tint intensity per part
    QVector<QVector3D> m_partDetailScales;                  // per-map tiling (x/y/z = map0/1/2)
    QVector<int>     m_partDetailMetalLayer;               // per-part metal detail-map index (-1 = none)
    DetailConfig     m_detailCfg;                          // global detail-map experiment config (panel)
    QVector<QVector4D> m_partDyeBandsV;                     // per-part detected dye-mask band centres
    QVector<QVector4D> m_partZoneMapV;                      // per-part derived zone→layer (ints as floats)
    float            m_detailNormalMul = 1.0f;   // global detail strength multipliers (Preview sliders;
    float            m_detailRoughMul  = 1.0f;   // 1.0 = game-accurate additive, no manual tweak needed)
    float            m_detailScale     = 8.0f;   // detail tiling (baseRes/detailRes ≈ 2048/256)
    float            m_emisScale = 0.5f;         // global emissive-intensity multiplier (slider)
    bool             m_showTex = true, m_wireframe = false, m_showGrid = false;
    int              m_viewChannel = 0;   // material-channel debug viewer (0 = shaded)
    bool             m_showSkeleton = false;
    bool             m_showBoneNames = false;        // draw a name label at each bone head
    bool             m_boneNamesTranslated = false;  // use hash→readable dictionary vs raw bone_<hash>
    bool             m_boneNamesHideUnknown = false; // only label bones with a known/translated name
    QWidget*         m_boneLabels = nullptr;         // transparent QPainter overlay (BoneLabelOverlay)
    QMatrix4x4       m_lastViewProj;                 // last frame's proj*view (to project bone heads)
    bool             m_boneHashesDumped = false;     // one-shot SKELHASHES diagnostic guard
    void             updateBoneLabels();             // recompute projected label positions → overlay
    float            m_bg[3] = {0.10f, 0.10f, 0.11f};   // viewport clear colour
    GLuint           m_gridVao = 0, m_gridVbo = 0;
    static QString   s_glInfo;                   // renderer + GL version (set in initializeGL)
    bool             m_bgGradient = false;       // studio-gradient backdrop (Graphics ▸ Backdrop)
    GLuint           m_bgProg = 0, m_bgVao = 0;  // lazy fullscreen-triangle gradient pass
    int              m_gridVerts = 0;
    int              m_gridAxisFirst = 0;        // vertex where the 2 world-axis lines start (drawn tinted)
    bool             m_gridAxisColors = true;    // X red / Z blue (Blender-style), else plain grey
    bool             m_showAxisGizmo = true;     // clickable orientation ball, top-right
    QWidget*         m_gizmo = nullptr;          // AxisGizmoOverlay (file-local class, child widget)
    GLuint           m_skelVao = 0, m_skelVbo = 0;
    int              m_skelVerts = 0;
    // Rig hardpoints (attach sockets) overlay: an RGB axis gizmo + name label at each.
    bool             m_showHardpoints = false;
    QVector<ModelHardpoint> m_hardpoints;                // sockets to draw (set on load)
    GLuint           m_hpVao = 0, m_hpVbo = 0;
    int              m_hpAxisVerts = 0;                  // verts per X/Y/Z axis group (N sockets × 2)
    QWidget*         m_hpLabels = nullptr;               // BoneLabelOverlay for socket names
    void             buildHardpoints();
    void             updateHardpointLabels();
    bool             m_showPhysBones = false;            // draw only the physics (cloth) bones
    bool             m_showPhysAxes = true;              // draw the per-bone XYZ axis gizmos
    GLuint           m_physVao = 0, m_physVbo = 0;
    int              m_physVerts = 0;
    int              m_physConnVerts = 0;                // total connection-line verts (all groups)
    int              m_physPinnedVerts = 0;              // pinned (kinematic) connection verts (drawn first)
    // Contact-state split of the FREE bones, so the overlay can colour resting vs wedged bones.
    // Buffer order: [0,pinned) grey · [pinned,free) orange · [free,touch) yellow · [touch,conn) red.
    int              m_physFreeVerts = 0;                // end of the no-contact group
    int              m_physTouchVerts = 0;               // end of the resting-contact group
    int              m_physAxisVerts = 0;                // verts per X/Y/Z axis group
    QVector<std::array<float, 16>> m_boneGlobalSim;      // post-cloth-sim bone world matrices
    bool             m_showColliders = false;            // draw cloth collision capsules
    bool             m_capturing = false;                // export capture in progress (see setCaptureMode)
    bool             m_orphanLegacy = false;             // D4_CLOTH_LEGACY_ORPHANS=1: restore the old
                                                         // cage-less = hair treatment (rigid) path
    // Per-capsule body region, so the collision size can be tuned per body part instead of one
    // global multiplier. The game authors a radius PER CAPSULE PER BONE (thigh 0.27, chest 0.16…),
    // so this exposes a distinction the data already makes. Index = m_colR0 index.
    QVector<quint8>  m_colRegion;                        // ClothParams::CapRegion per capsule
    bool             m_capsFullSize = false;             // D4_CAPS_FULL=1: authored capsules at 1.0x
                                                         // (ignore the Capsule-size slider) — see rScale
    int              m_subStepsSaved = 0;                // solver subSteps to restore after capture
    float            m_captureTime = 0.0f;               // deterministic FX time while capturing
    bool             m_backfaceCull = true;              // cull back faces of the solid mesh
    GLuint           m_colVao = 0, m_colVbo = 0;
    int              m_colLineVerts = 0;
    // One-past-last collider-overlay vertex per body region, so paintGL can colour each group.
    QVector<int>     m_colRegionSpan;
    void buildGrid();
    void buildSkeleton();
    void buildPhysBones();
    void buildColliderLines();

    // camera framing
    QVector3D m_center;
    QVector3D m_homeCenter;   // framed centre (pan resets here)
    float     m_radius = 1.0f;
    float     m_yaw = 0.6f, m_pitch = 0.3f, m_dist = 3.0f;
    // "React to rotation": yaw at the previous sim step + the smoothed angular velocity carried
    // between steps, so a flick keeps swinging the cloth after the mouse stops.
    float     m_spinPrevYaw = 0.0f;
    float     m_spinOmega   = 0.0f;
    bool      m_spinSeeded  = false;
    // Cloth should also simulate with NO animation loaded (rest pose), so gravity and the
    // user-rotation forces still move a garment on a static model.
    bool simAtRestActive() const;
    // Per-bone contact state from the last sim step, for the phys-bone overlay:
    // 0 = free · 1 = touching (resolved this step) · 2 = was penetrating deeply.
    // Purely diagnostic — makes "stuck inside a capsule" visible instead of inferred.
    QVector<quint8> m_sbContact;
    float     m_fov = 45.0f;   // vertical field of view (degrees)
    bool      m_ortho = false;  // orthographic projection (Camera popup)
    bool      m_transparentClear = false;  // clear alpha 0 during native-alpha capture
    float     m_maxAniso = 1.0f;           // GPU max anisotropic-filtering level (1 = unsupported)
    // Smooth camera glide (frameRegion animate=true): interpolate toward these targets.
    QTimer*   m_camAnim = nullptr;
    QVector3D m_tgtCenter;
    float     m_tgtYaw = 0.0f, m_tgtPitch = 0.0f, m_tgtDist = 0.0f;
    QPoint    m_lastPos;
    QPoint    m_rightPressPx;   // where a right-button press began (to tell a click from a pan-drag)

    QString m_error;
};
