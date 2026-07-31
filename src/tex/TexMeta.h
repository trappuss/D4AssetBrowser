#pragma once
#include <QByteArray>
#include <QImage>
#include <QVector>

// One atlas sub-frame from ptFrame[]: an image handle + its UV rectangle in the
// texture (0..1). flTrim* is the tight non-transparent bounds.
struct TexFrame {
    quint64 handle = 0;
    float   u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};

// Texture descriptor read from d4data's <name>.tex.json (the parsed form of the
// CASC meta file). Gives the format + dimensions needed to upload the payload.
//
// Bridge approach (same as the Python fork): metadata from d4data JSON, raw pixel
// payload from CASC. A later milestone can parse the binary meta from CASC directly
// (d4analyzer-style) to drop the d4data dependency.
// One entry of serTex[]: byte offset + size of a sub-resource (face/mip slice) in the
// raw CASC payload. Layout is face-major: [face0: mip0..mipN, pad…][face1: …]….
struct TexSubRes {
    quint32 offset = 0;
    quint32 size   = 0;   // dwSizeAndFlags (size in bytes for the slice; may include face pad)
};

struct TexMeta {
    bool valid     = false;
    int  eTexFormat = 0;
    int  width      = 0;
    int  height     = 0;
    int  depth      = 0;
    int  faceCount  = 0;
    int  mipMin     = 0;
    int  mipMax     = 0;
    QVector<TexFrame>  frames;
    QVector<TexSubRes> subres;   // serTex[]: per face×mip byte offsets/sizes (for cubemaps)
};

TexMeta parseTexMetaJson(const QByteArray& json);

// Corrected atlas dimensions for textures whose d4data .tex.json is stale or missing (the game
// patched the atlas but the JSON snapshot lags — the CASC payload has no descriptor, only pixels).
// Returns true and fills w/h when a real dimension override exists for `texName`. Sources, merged:
//   1. a compiled-in table of known inventory atlases (real dims from d4analyzer PNG exports),
//   2. a user file  <AppData>/D4AssetBrowser/texture_dims_override.txt  (name<TAB>W<TAB>H),
//   3. PNGs dropped in <AppData>/D4AssetBrowser/texture_overrides/  (stem = name, size = dims).
bool textureDimOverride(const QString& texName, int& w, int& h);

// Per-frame icon override: a decoded icon for an atlas frame, loaded from PNGs the user exported
// with d4analyzer (Export ▸ TexFrames) into <AppData>/D4AssetBrowser/icon_overrides/.
// d4analyzer names those files "<atlasName> [<atlasSno>] - <frameIdx> <frameName>.png", so we index
// by (atlasSno, frameIdx). Returns a null QImage when none is present. This lets wardrobe / model /
// texture-tab icons use the exported frames when the atlas ptFrame layout in d4data is stale/missing
// after a patch (the atlas is re-laid-out, so the old UVs miscrop). frameIdx is the frame's position
// in the atlas's ptFrame array.
QImage frameIconOverride(int atlasSno, int frameIdx);

// Number of exported frame-icon overrides present for an atlas sno (highest frame index + 1), so a
// texture with no ptFrame in d4data can still display its exported frames. 0 = none.
int frameOverrideCount(int atlasSno);
