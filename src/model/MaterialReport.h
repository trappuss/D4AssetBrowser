#pragma once
#include <QString>

class CascReader;
class SnoIndex;

// ── "Why does this part look wrong?" in one answer ──────────────────────────────────────────────
//
// Every rendering defect this tool has shipped presented the same way: something is white, grey,
// blank or blown out, and there is no way to ask the tool WHY. The DOOM StoreProducts vanishing
// from the Catalogue, the Wardrobe's weapon material roster coming back empty, every emissive glow
// saturating to paper, encrypted materials silently running stand-in PBR — four separate failures,
// four rounds of reading source to answer a question the program already had the facts for.
//
// This assembles those facts into text a person can read and paste. It answers, in order:
//
//   · is this appearance ENCRYPTED, and do we hold its key
//   · where the material ROSTER came from — .app.json, the CASC meta binary, or nowhere
//   · which material this part actually resolved to
//   · which of its values are AUTHORED and which are stand-ins the tool substituted
//   · every texture role the material references, its sno, and whether the definition resolves
//
// The distinction it exists to make is authored-versus-assumed. A material reporting roughness 0.6
// looks identical whether the game authored 0.6 or the tool gave up and picked it, and that
// ambiguity is what let encrypted content look merely ugly instead of unread for months.
//
// Pure and side-effect free: it decodes no pixels and writes no files, so it is safe to call from
// a context menu on any part, however broken.
namespace MaterialReport {

// `appearanceName` / `appearanceSno` identify the piece; `materialName` is the ROSTER name for the
// clicked part (never MeshPrimitive::materialName, which is "Material_<n>" placeholder text).
// Either may be empty — the report says which fact is missing rather than returning nothing.
QString explain(CascReader* reader, SnoIndex* index, const QString& d4,
                const QString& appearanceName, int appearanceSno,
                const QString& materialName);

}   // namespace MaterialReport
