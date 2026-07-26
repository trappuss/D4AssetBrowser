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
// centimeter pipelines (Unreal/Skyrim FBX round-trips) use 100. `flipNormalGreen`
// inverts the normal map's G channel (OpenGL → DirectX convention).
struct Options {
    bool  reconstructNormalZ = true;
    bool  blenderFriendly    = false;
    float unitScale          = 1.0f;
    bool  flipNormalGreen    = false;
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
}
