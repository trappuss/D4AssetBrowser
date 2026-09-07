#pragma once
#include <QByteArray>
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

// MarkingShape face/body mask texture names + default MarkingColor stem + flEmissiveStrength (glow)
// + the shape's own hIconImage (the swatch the shop and the creator show for it).
// faceSno/bodySno are the mask references themselves, and they are NOT a convenience duplicate of
// the names. SnoIndex::nameForSno deliberately returns EMPTY for a record whose name is encrypted,
// so a marking whose mask texture is encrypted resolved to an empty name, every consumer that
// re-derived the sno from that name got 0, and the marking listed but painted nothing. The sno is
// the identity; the name is only for the .tex.json fast path.
struct MarkingDef {
    QString faceTex, bodyTex, colorStem;
    float   emissive = 0.0f;
    quint32 icon     = 0;
    quint32 faceSno  = 0, bodySno = 0;
};

// MarkingColor: 3-point ramp (shadow/mid/highlight, sRGB-encoded) + authored surface properties.
struct MarkingPaint {
    std::array<QColor,3> ramp{};
    float roughness = -1.0f;   // <0 ⇒ not authored (leave the skin's own value)
    float metalness = -1.0f;
    bool  isTattoo  = true;
    bool  valid     = false;
};

// ── The same record, read from the GAME instead of the snapshot ─────────────────────────────────
// d4data describes 304 MarkingShapes; the game ships 374. The 70 in the gap are simply absent from
// the Wardrobe's list — not greyed, not named — and that gap is where new content lands: every one
// of the Diablo IV x Berserk Brand of Sacrifice markings sits in it, in the exact numbering holes
// of each class's sequence.
//
// The layout below was MEASURED, not guessed — `Dump - MarkingShape Layout.bat` searches each
// record's own binary for the values its json already states and reports where they land. Every
// offset here was unanimous across all the records that carry the field:
//     0x00 u32  0xDEADBEEF magic          0x24 u32  hIconImage
//     0x10 u32  self sno                  0x28 u32  snoMaskFace
//     0x18 i32  eClassRestriction         0x2c u32  snoMaskBody
//                (-1 = unrestricted)      0x30 u32  snoDefaultColor
// A shipped record is 56 bytes. SNO fields use 0xFFFFFFFF for "none" (verified on the Bad Data
// record, whose three refs are all 0xFFFFFFFF); hIconImage uses 0.
//
// Pure function over the blob: no Qt GUI, no index, no threading contract — the sno→name lookups
// its results need are the CALLER's job, because SnoIndex::nameForSno is GUI-thread only.
struct MarkingBin {
    bool    valid            = false;
    int     classRestriction = -1;   // eHeroClass index, or -1 for "any class"
    quint32 icon             = 0;    // hIconImage handle (0 = none)
    quint32 maskFaceSno      = 0;    // 0 = none
    quint32 maskBodySno      = 0;
    quint32 defColorSno      = 0;
};
MarkingBin           markingBinParse(const QByteArray& meta);

MarkingDef           markingDef(const QString& d4, const QString& stem);   // .msh.json
std::array<QColor,3> markingRamp(const QString& d4, const QString& stem);  // .mcl.json 3-point ramp
MarkingPaint         markingPaint(const QString& d4, const QString& stem); // ramp + rough/metal/tattoo
QColor               rampLerp(const std::array<QColor,3>& r, float t);
QImage               applyMarking(QImage base, const QImage& mask0, const std::array<QColor,3>& ramp);
QImage               applyMarkingMaterial(QImage& base, QImage& orm, const QImage& mask0,
                                          const MarkingPaint& paint, float emissiveStrength,
                                          float skinRough, float skinMetal, float& outEmisMul);
QString              markingSelfTest();   // startup sanity check of the R/G model
