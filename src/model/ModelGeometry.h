#pragma once
#include <QString>
#include <QVector>
#include <array>

// ── The geometry contract the .glb exporter consumes ─────────────────────────
//
// This is the boundary between the two halves of model export:
//
//   D4 model payload  ──[ModelParser, TODO]──►  ModelGeometry  ──[ModelExporter,
//                                                                  fastgltf]──► .glb
//
// The parser (not yet implemented — needs the D4 binary model format; see
// docs/MODEL_EXPORT.md) fills a ModelGeometry from base/payload/<sno>. The exporter
// serializes it with fastgltf. Material textures + MaterialValues are supplied
// separately by the existing d4data parsers (model/Material.h) and applied during
// export, reusing the same accuracy rules already implemented for textures.
//
// Keeping this contract stable means the parser and exporter can be built and
// tested independently.

struct MeshVertex {
    float px = 0, py = 0, pz = 0;          // POSITION
    float nx = 0, ny = 0, nz = 0;          // NORMAL (unit length)
    float u  = 0, v  = 0;                  // TEXCOORD_0
    // COLOR_0 (0..1). In D4's uber shader the RGB channels are the per-vertex blend
    // weights for tiled Detail Map 1/2/3; A is a secondary weight. Defaults to all-1
    // (meshes without a colour stream behave as before — Detail Map 1 at full weight).
    float cr = 1, cg = 1, cb = 1, ca = 1;
    bool  hasColor = false;                // true only when the stream actually carried COLOR_0
    float c2r = 0, c2g = 0, c2b = 0, c2a = 0;  // COLOR_1 (diagnostic: alt detail-mask candidate)
    bool  hasColor1 = false;
    float u1 = 0, v1 = 0;                  // TEXCOORD_1 (second UV — detail-mask atlas candidate)
    bool  hasUv1 = false;
    // Skinning (optional; all-zero weights ⇒ treated as static).
    quint16 joints[4]  = {0, 0, 0, 0};     // JOINTS_0
    float   weights[4] = {0, 0, 0, 0};     // WEIGHTS_0 (should sum to 1)
};

struct MeshPrimitive {
    QVector<MeshVertex> vertices;
    QVector<quint32>    indices;           // triangle list (3 per triangle)
    QString             materialName;      // → Material.h for textures + MaterialValues
    int                 materialIndex = -1; // SubObject.nMaterialIndex (appearance roster)
    quint32             subObjectHash = 0;  // SubObject.dwSubObjectHash (own name hash)
    quint32             slotHash = 0;       // SubObject slot hash (bdy/trs/leg/hlm/glv/bts)
    bool                doubleSided = false;
};

struct ModelJoint {
    QString               name;
    quint32               nameHash = 0;     // bone-name DJB2 hash (maps anim curves → joints)
    int                   parent = -1;     // index into ModelGeometry::skeleton, -1 = root
    // AUTHORED cloth/physics-bone marker (BoneData.nBaseBoneCount/nClothBoneCount: the
    // piece's bone array is base-first, cloth-last; bones past nBaseBoneCount are the
    // game's simulated set). mergeGeometries ORs this across pieces, so the solver can
    // classify per-bone instead of guessing from a single merged-index boundary — the
    // guess broke every class whose BODY carries cloth (spiritborn/druid).
    bool                  cloth = false;
    // A RIGID-LINK chain bone (a flail's links). Cloth too, so the solver picks it up, but it wants
    // pendulum physics — hold the rest distance to the parent, swing freely — rather than the
    // pose-hugging treatment a cage-less fur bone gets. Set only by attachSubRigAt.
    bool                  chain = false;
    std::array<float, 16> inverseBind {};  // inverse bind matrix, column-major
    std::array<float, 16> localMatrix {};  // node local transform, column-major
    // Raw rest-pose TRS (D4-native, pre axis-swap) — the animation rest fallback
    // and the basis the per-frame skinning palette composes from.
    std::array<float, 4>  restQ {{0, 0, 0, 1}};  // quaternion x,y,z,w
    std::array<float, 3>  restT {{0, 0, 0}};
    std::array<float, 3>  restS {{1, 1, 1}};
};

// Vertex-buffer description (for the d4analyzer-style VertexBuffers panel).
struct VertexAttr { int semantic = 0; int format = 0; int offset = 0; };
struct VertexBufferInfo {
    int index = 0;
    int stride = 0;
    int vertexCount = 0;
    QVector<VertexAttr> attrs;
};

// Authored NvCloth collision capsule (ClothData.ptCapsuleDefs / dmClothCapsuleDefMirror).
// A tapered capsule bound to a skeleton bone: spheres of radius1 / radius2 separated by
// `height` along the local axis, placed by a bone-local transform. boneIndex refers to
// ModelGeometry::skeleton (remapped through mergeGeometries like vertex joints).
struct ClothCapsule {
    int                 boneIndex = -1;
    std::array<float,4> localQ {{0, 0, 0, 1}};   // bone-local rotation (x,y,z,w)
    std::array<float,3> localP {{0, 0, 0}};      // bone-local position
    float               radius1 = 0, radius2 = 0;
    float               height = 0;
    float               friction = 0.1f;
};

// Authored NvCloth sim cage (one per cloth piece, from ClothData). A low-poly mesh the
// game simulates and then uses to drive (skin) a high-poly render submesh. We match the
// cage rest verts to the equipped piece's "sim" render submesh, then run the authored
// pin/constraint model on it. All positions are y-up (swapped to match the mesh/skeleton).
// Authored cloth plane collider (ptPlaneDefs / dmClothPlaneDefMirror, 48B). A bone-local
// half-space the cloth stays in front of (normal = local +Z through localP). Like capsules,
// boneIndex binds it to the rig; the plane is placed by restG[bone] × localTransform.
struct ClothPlane {
    int                 boneIndex = -1;
    std::array<float,4> localQ {{0, 0, 0, 1}};
    std::array<float,3> localP {{0, 0, 0}};
    float               stiffness = 1.0f;
    float               friction = 0.1f;
};

struct ClothSim {
    QVector<float>   bindVerts;      // cage rest positions, 3 floats per vert (ptBindVertices)
    QVector<float>   invMasses;      // per cage vert; 0 ⇒ pinned/kinematic (ptInvMasses)
    QVector<quint16> constraintIdx;  // authored distance-constraint vertex pairs, 2 per (ptConstraintIndices)
    QVector<float>   constraintLen;  // rest length per constraint (ptConstraintLengths)
    QVector<quint16> triangles;      // cage triangle vertex indices, 3 per tri (ptTriangles)
    QVector<ClothPlane> planes;      // authored plane colliders (ptPlaneDefs)
    QVector<float>   attachLen;      // per-vert TETHER length in ABSOLUTE world units
                                     // (ptAttachmentLengths): the allowed distance from the
                                     // particle's kinematic-root particle. Measured r=1.0000
                                     // against the summed parent-chain length on 12/12 blocks
                                     // (tools/d4cloth/FINDINGS.md F1) — NOT a normalized 0..1
                                     // fraction, and NOT relative to the skinned pose.
    // ── Authored driving system (tools/d4cloth/FINDINGS.md F2). vertCount above is the
    // array CAPACITY; nRealVerts is the authored particle count — indices >= nRealVerts
    // are SIMD padding (bind at origin, self-pair constraints) and must not simulate. ──
    int              nRealVerts = 0; // dmClothDataMirror.vertexCount (@252); 0 = unknown (use vertCount)
    QVector<quint16> drvInf;         // ptDriverInfluences: 4 driver indices per vert (capacity-sized)
    QVector<float>   drvW;           // ptWeights: 4 weights per vert, rows sum to 1 (capacity-sized)
    QVector<int>     drvBone;        // per driver: bone the driver frame is bound to (ptDriverMap),
                                     // PIECE skeleton index — remapped by mergeGeometries; -1 unresolved
    QVector<int>     followerBone;   // per REAL vert: the bone this particle drives (ptFollowerIndices),
                                     // remapped by mergeGeometries; -1 = none (pinned / bone-less fabric).
                                     // Rest alignment bone↔particle measured ≤5 µm; never duplicated.
    QVector<quint16> kinRoots;       // ptKinematicRoots: per vert, the tether anchor particle (capacity-sized)
    QVector<float>   blendW;         // ptBlendWeights: per vert sim<->skinned blend (the game's
                                     // anti-clip: sim is a CORRECTION on the animated drape; mean
                                     // ~0.79 on the cape, lower where fabric must hug the body)
    QVector<quint8>  conClass;       // per constraint pair: 0=warp 1=weft 2=shear 3=bend (from the
                                     // pt*Clusters ranges), 255=unclassified. The classes take the
                                     // .clt.json per-class stiffnesses (stretch/horiz/shear/bend).
    // Which bone's REST frame bindVerts are expressed in. -1 (the normal case) means the piece's
    // own model space, which after merging IS the merged model space — so the solver compares cage
    // particles against rest globals directly. An ATTACHMENT is different: its cage is authored
    // around the trophy's own origin, but its bones end up wherever the placement bone puts them,
    // metres away. Naming the placement bone here lets the solver put the two back in one frame
    // instead of the cage silently matching nothing.
    int              spaceBone = -1;
    QString          name;           // ClothData.name (e.g. "barF_dlux100_TRS_cape") — legacy
                                     // key for the matching Cloth/*.clt.json (fallback path only)
    // ── Authoritative tuning link. The game's OWN per-item physics reference: each
    // appearance sub-object carries snoCloth (Appearance/<app>.app.json →
    // ptSubObjects[].nMaterialIndex → ptAppearanceMaterials[].ptSOAs[].snoCloth), paired
    // to this block by its dataOffset. Measured: resolves rogF_stor214_HLM to the MALE
    // rogM_stor214_feather_sim files — unreachable by any name heuristic. ──
    QString          srcApp;         // appearance this block came from ("" = unknown → fallback)
    int              srcOffset = -1; // this ClothData's dataOffset (pairs with the .app.json link)
    // Per-piece authored tuning (filled from this piece's .clt.json by the wardrobe assembler;
    // applied per-bone so each garment behaves with its own params). Defaults = neutral.
    float windX = 0, windY = 0, windZ = 0;   // vSelfWind (y-up) × flWindFactor, model-scaled
    float boneTrack = 0.5f;                   // flBoneTrackingFactor (return-to-pose stiffness)
    float gravScale = 1.0f;                   // this piece's gravity ÷ a reference (≈ vGravity.z/-20)
    float attachStiff = 0.3f;                 // flAttachmentStiffness: strength of the pull back
                                              // toward the skinned pose (the game's motion-constraint
                                              // spring). 0.9 = near-rigid crests; 0.1 = free feathers.
    // Authored per-class constraint stiffness (flStretching/Horizontal/Shear/Bending-
    // Stiffness), indexed by conClass 0..3 (warp/weft/shear/bend). Skirts author
    // stretch ≈0.85 — running them at the global slider default (0.21) let the warp
    // chains elongate under authored gravity until the skirt hung as a line through
    // the body. Applied per piece when tuned; sliders remain the untuned fallback.
    float clsStiff[4] = { 0.8f, 0.8f, 0.5f, 0.5f };
    float dragF = 0.0f;                       // flDragFactor: air drag on velocity (keep x= 1-drag*0.1)
    bool  tuned = false;                      // true once filled from .clt.json
    int              vertCount = 0;  // == bindVerts.size()/3 == invMasses.size()
};

// An attachment point authored on the rig (weapon grip, sheath, trail emitter, look-at…).
// Parented under `boneIndex` in the exported .glb as an empty named `name`, so the exact
// game socket transforms come across for aligning props/weapons in other tools/games.
// q/t are the D4-native (z-up, pre axis-swap) BONE-LOCAL rotation+translation — the exporter
// applies the same swap/yaw/X-mirror the bones get.
struct ModelHardpoint {
    QString              name;              // "HP_rightWeapon" … (or "HP_<hash>" if unknown)
    int                  boneIndex = -1;    // parent bone (index into ModelGeometry::skeleton)
    quint32              boneHash = 0;      // parent bone's nameHash — survives skeleton reorder/reduce
    std::array<float, 4> q {{0, 0, 0, 1}};  // bone-local rotation (x,y,z,w), D4-native
    std::array<float, 3> t {{0, 0, 0}};     // bone-local translation, D4-native
};

struct ModelGeometry {
    bool                   valid = false;
    QVector<MeshPrimitive> primitives;
    QVector<ModelJoint>    skeleton;       // empty ⇒ static (rigid) model
    QVector<VertexBufferInfo> vertexBuffers;   // raw VB layouts (informational)
    // Rig attachment points (weapon grips, sheaths, trails…). Empty unless the export path
    // opted to attach them (Settings ▸ Export ▸ "Export hardpoints as empties").
    QVector<ModelHardpoint> hardpoints;
    // Count of base (non-cloth) bones in the shared rig — bones at index >= this are
    // the game's cloth/physics bones (the merge appends armor cloth bones after the
    // base rig). 0 ⇒ unknown / no cloth bones.
    int                    nBaseBones = 0;
    // Authored cloth collision capsules (from the equipped pieces' ClothData). Bone
    // indices are remapped into the merged skeleton by mergeGeometries.
    QVector<ClothCapsule>  clothCapsules;
    // Authored sim cages (one per cloth piece). Self-contained (cage-local indices), so
    // mergeGeometries just appends them; matching to render submeshes happens at sim build.
    QVector<ClothSim>      clothSims;
    // Bones the physics solver must treat as RIGID (hold at the animated pose, never Verlet-sim),
    // even though they sit past nBaseBones. Used when a separately-rigged prop (a mount trophy with
    // its own base+cloth skeleton) is skeletally attached: its BASE bones are pinned so only its
    // cloth bones swing. Empty for the normal single-rig case.
    QVector<int>           pinnedBones;
};
