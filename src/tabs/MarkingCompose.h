#pragma once
#include <QColor>
#include <QImage>
#include <QString>
#include <array>

// D4 body-marking compositing (extracted from WardrobeTab2 for maintainability). Reads the
// MarkingShape / MarkingColor game data and paints a marking onto skin. See STATUS.md "Marking
// model" for the definitive rule:
//   mask RED  = coverage / opacity (where the design sits vs bare skin)
//   mask GREEN= material / ramp position (0 = ink → ramp shadow, 1 = gold → ramp highlight)
//   albedo    = lerp(skin, rampLerp(ramp, G), R);  metalness = flPaintMetalness * G at coverage R;
//   emissive gated by G; no normal emboss. Grayscale (BC4) masks have R==G so they still work.

// MarkingShape face/body mask texture names + default MarkingColor stem + flEmissiveStrength (glow).
struct MarkingDef { QString faceTex, bodyTex, colorStem; float emissive = 0.0f; };

// MarkingColor: 3-point ramp (shadow/mid/highlight, sRGB-encoded) + authored surface properties.
struct MarkingPaint {
    std::array<QColor,3> ramp{};
    float roughness = -1.0f;   // <0 ⇒ not authored (leave the skin's own value)
    float metalness = -1.0f;
    bool  isTattoo  = true;
    bool  valid     = false;
};

MarkingDef           markingDef(const QString& d4, const QString& stem);   // .msh.json
std::array<QColor,3> markingRamp(const QString& d4, const QString& stem);  // .mcl.json 3-point ramp
MarkingPaint         markingPaint(const QString& d4, const QString& stem); // ramp + rough/metal/tattoo
QColor               rampLerp(const std::array<QColor,3>& r, float t);
QImage               applyMarking(QImage base, const QImage& mask0, const std::array<QColor,3>& ramp);
QImage               applyMarkingMaterial(QImage& base, QImage& orm, const QImage& mask0,
                                          const MarkingPaint& paint, float emissiveStrength,
                                          float skinRough, float skinMetal, float& outEmisMul);
QString              markingSelfTest();   // startup sanity check of the R/G model
