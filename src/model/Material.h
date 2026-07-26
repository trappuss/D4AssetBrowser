#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

// One texture binding from a material's tUberMaterial.ptMatTexList[].
struct MatTexture {
    int     slot = 0;     // eShaderTex
    QString role;         // semantic role for the slot (BASE_COLOR, NORMAL, …)
    QString texName;      // snoTex.name
    qint64  texSno = 0;   // snoTex.__raw__
    float   uScale = 1;   // tMatTex.ptTexAnim[0].flUScale — per-map tiling (detail maps use e.g. 8-20)
    float   vScale = 1;   // flVScale
};

// Scalar/vector MaterialValues d4analyzer surfaces (and the exporter applies).
struct MaterialValues {
    bool  valid = false;
    bool  hasMetal = false, hasRough = false, hasAO = false;
    bool  hasEmisMult = false, hasEmisColor = false;
    float metal = 0, rough = 0, ao = 0, emisMult = 1;
    float emisR = 0, emisG = 0, emisB = 0;
};

// Parse a d4data Material JSON (<name>.mat.json) → its texture bindings.
QVector<MatTexture> parseMaterialJson(const QByteArray& json);

// Parse the material's MaterialValues from tUberMaterial.ptRunTimeMaterialValues.
MaterialValues parseMaterialValues(const QByteArray& json);

// Shader slot → semantic role (ported from d4extract material_format_spec via the
// Python fork's SLOT_ROLES). Unknown slots return "SLOT_<n>".
QString shaderSlotRole(int slot);
