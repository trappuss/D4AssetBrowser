#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

class SnoIndex;

class CascReader;

// Free-function material/texture decoders shared by the model preview tabs. Given a
// CASC reader, the d4data dir, and a material name, these read the .mat.json role
// bindings and decode the referenced textures (CASC payload + d4data tex.json) into
// the images the viewport consumes. Self-contained so any tab can assemble + texture
// a model without depending on ModelsTab internals.
namespace MaterialDecode {

// Decode a texture by name + SNO (d4data tex.json for dims/format, CASC for pixels).
QImage texture(CascReader* reader, const QString& d4, const QString& texName, qint64 texSno);

// First texture bound to a shader role (e.g. "NORMAL", "DYE_MASK"), decoded.
QImage byRole(CascReader* reader, const QString& d4, const QString& matName, const char* role);


// Decode the up-to-3 detail NORMAL maps (slots 212/213/214) and detail ROUGHNESS maps
// (218/219/220) as SEPARATE images (out vectors sized 3; null where a slot is absent), so the
// shader can select the right one per texel from the dye-mask region. outNStrength/outRStrength
// are the AVERAGE authored intensities of the present maps (a single bounded-blend strength) and
// outROffset the summed present rough offsets. This is the region-masked replacement for the
// (now unused) single-map composite: D4 does NOT blend the detail maps — it picks one per region.
// outScales[i] = the per-map tiling (ptTexAnim flUScale from slot 212/213/214), the real grain
// size the game uses (e.g. 6-20); default 8 when a map is absent.
// outMetalLayer = the index (0/1/2) of the detail map whose texture name is a Metal library map
// (or -1 if none). The dye mask segments dyeable regions, not material types, so metal and leather
// can land in the same zone; the shader uses metalness (the true metal signal) to route metal
// texels to THIS map rather than letting the zone hand them a leather grain.
void detailMapsSeparate(CascReader* reader, const QString& d4, const QString& matName,
                        const float nInt[3], const float rInt[3], const float rOff[3],
                        QVector<QImage>& outNormals, QVector<QImage>& outRoughs,
                        float& outNStrength, float& outRStrength, float& outROffset,
                        float outScales[3], int& outMetalLayer);

QImage baseColor(CascReader* reader, const QString& d4, const QString& matName);   // BASE_COLOR
QImage normalMap(CascReader* reader, const QString& d4, const QString& matName);   // NORMAL (raw RG)
QImage orm(CascReader* reader, const QString& d4, const QString& matName);         // AO(R)/rough(G)/metal(B)
void   factors(CascReader* reader, const QString& d4, const QString& matName,
               float& metal, float& rough);

// Default-look material roster of an appearance, indexed by primitive materialIndex
// (override > base > cloth, per ptAppearanceMaterials[].ptSOAs[0]).
QStringList appearanceRoster(const QString& d4, const QString& appName);

// The same roster read from the CASC meta BINARY, for appearances that have no .app.json — i.e.
// every encrypted one. Names are resolved sno -> index name, so an encrypted material (itself
// nameless) comes back as "~unnamed_<sno>": positionally correct, which is what materialIndex
// needs, even though the texture lookup that follows still wants a real name.
// Empty when the blob carries no material array. See AppearanceMatBin for the derived layout.
QStringList appearanceRosterFromMeta(const QByteArray& meta, const SnoIndex* idx);

// The texture SNOs a material references, via whichever route works — .mat.json when it exists,
// the meta binary when it does not. Used by the health audit to check a material can resolve its
// textures without paying for a pixel decode.
QVector<qint64> textureSnosFor(CascReader* reader, const QString& d4, const QString& matName);

}  // namespace MaterialDecode
