#pragma once
#include "model/ModelGeometry.h"
#include "model/AnimParser.h"

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

// Serializes a ModelGeometry to a binary glTF (.glb). Self-contained writer (glTF
// JSON + BIN chunk) — no external glTF library needed for writing, so the byte
// format is fully under our control and verifiable.
//
// Exports static OR skinned geometry (POSITION / NORMAL / TEXCOORD_0 / indices,
// plus JOINTS_0 / WEIGHTS_0 + skin + inverseBindMatrices when a skeleton is
// present). Materials can optionally be enriched with real names + PBR factors
// (see ExportMaterial); without that, one default PBR material per name is used.
// Embedded textures remain a documented follow-up (see docs/MODEL_EXPORT.md).
namespace ModelExporter {

// Per-material override, indexed by MeshPrimitive::materialIndex. Supplied by the
// caller (which has the appearance roster + .mat.json MaterialValues).
struct ExportMaterial {
    QString name;
    bool    doubleSided = false;
    bool    alphaCutout = false;   // → alphaMode=MASK + alphaCutoff (hair / cut-out cloth)
    float   alphaCutoff = 0.35f;   // glTF alphaCutoff (matches the shader's cutout threshold)
    bool    hasMetal = false;  float metal = 0.0f;
    bool    hasRough = false;  float rough = 1.0f;
    bool    hasEmissive = false;
    float   emisR = 0.0f, emisG = 0.0f, emisB = 0.0f, emisMult = 1.0f;
    QImage  baseColor;   // → baseColorTexture (PNG) if non-null
    QImage  normal;      // → normalTexture (tangent-space RGB) if non-null
    QImage  orm;         // → metallicRoughness + occlusion (R=AO, G=rough, B=metal) if non-null
    QImage  emissive;    // → emissiveTexture (masks the emissive glow) if non-null
};

// When `anims` is non-empty AND the geometry is skinned, the exported bone nodes use TRS
// (instead of a baked matrix — required for animated nodes) and each clip is written as a
// glTF animation (rotation/translation/scale channels per bone). The anim curves are D4-native
// (pre axis-swap); the exporter applies the same Z-up→Y-up swap the live skinning uses.
// reconstructNormalZ: when true, the exported normal map's blue channel is rebuilt as
// √(1−x²−y²) so Blender lights it correctly (D4's BC5 normals decode with B≈0). Set false to
// export the normal exactly as decoded from the game.
// blenderFriendly: bake an extra yaw (glTF-space rotY −90°) into vertices, root bones, inverse
// bind matrices and root-bone anim channels so the model imports into Blender in the Blender
// character convention: facing −Y with the character's LEFT on +X (verified from hardpoint data:
// D4 rigs mirror across D4 +Y = character's left, and HP_chestFront shows facing = D4 +X).
// Required for Blender's X-Mirror / Symmetrize to line up with the .L/.R names produced by
// GLModelWidget::blenderizeSkeletonNames.
// Full export options. `unitScale` multiplies every position / bone translation /
// inverse-bind translation / anim translation (rotations & normals untouched) — for
// centimeter pipelines (Unreal/Skyrim FBX round-trips) use 100.
//
// flipNormalGreen: inverts the normal map's G channel, DirectX → OpenGL.
//
// The direction was BACKWARDS until it was measured, and it defaulted to "no flip", so the two
// presets that need the flip (Blender, Unity) were the two that did not do it. D4's normal maps
// are DIRECTX convention: G points toward the BOTTOM of the texture. Measured on
// barM_P00_BOD_normal from three features with known anatomy, with no integration involved — on
// a convex bump the upper half carries the HIGHER G under OpenGL and the LOWER G under DirectX,
// and both nipples (−15, −13) and both pectoral mounds (−5, −4) read DirectX, as did the navel
// taken as a pit (+6). The same statistic on a synthetic bump of each convention returns +14 /
// −14, so its sign is established rather than assumed.
//
// glTF mandates OpenGL-convention normal maps and Blender is OpenGL-convention, so glTF output
// FLIPS by default; only a DirectX target (Unreal/Skyrim) wants the channel as decoded.
struct Options {
    bool  reconstructNormalZ = true;
    bool  blenderFriendly    = false;
    float unitScale          = 1.0f;
    bool  flipNormalGreen    = true;
    // Symmetrize the rig for Blender's Pose ▸ X-Axis Mirror (only with blenderFriendly):
    // each .R bone's world rest rotation is rewritten to the exact mirror of its .L
    // partner's, locals/inverse-binds rebuilt, anim curves conjugated into the new local
    // frames. Verified in Blender 4.2.9 — see D4_XMirror_Spec.md. Skinning is preserved
    // exactly; unpaired/center/cloth bones untouched.
    bool  xMirror            = true;
};

// Resolve Options from QSettings: reads export/reconstructNormalZ + export/blenderFriendly,
// then applies the retarget/enginePreset override (0 Custom — uses retarget/unitScale,
// 1 Blender, 2 Unreal/Skyrim, 3 Unity; see SettingsDialog "Retarget & modding").
Options optionsFromSettings();

bool exportGlb(const ModelGeometry& geo, const QString& path,
               const QVector<ExportMaterial>& materials,
               const QVector<AnimParser::DecodedAnim>& anims,
               const QStringList& animNames,
               const Options& opt);

// Legacy convenience overload (kept for callers that predate Options).
bool exportGlb(const ModelGeometry& geo, const QString& path,
               const QVector<ExportMaterial>& materials = {},
               const QVector<AnimParser::DecodedAnim>& anims = {},
               const QStringList& animNames = {},
               bool reconstructNormalZ = true,
               bool blenderFriendly = false);

// Invert a normal map's G channel in place: DirectX <-> OpenGL. Shared by exportGlb and the
// self-test below, deliberately — a test that reimplements the transform it is guarding proves
// nothing about the code that ships.
void flipNormalGreenChannel(QImage& nrm);

// Startup sanity check of the normal-map CONVENTION. Empty string on success.
//
// This exists because the green channel's meaning has been wrong twice in this codebase and
// neither time produced a symptom anything could catch: it compiles, it renders, it exports, and
// the only evidence is that relief looks subtly inside-out in someone else's DCC. The seam fix on
// mirrored UV islands is blind to it too — flipping both sides makes them AGREE, whichever one is
// right. So the invariant is asserted directly: given a bump authored the way D4 authors them
// (DirectX, G toward the image bottom), the default export path must emit OpenGL.
QString normalConventionSelfTest();
}
