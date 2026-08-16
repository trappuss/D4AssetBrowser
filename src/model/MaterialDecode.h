#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QVector3D>   // detail tiling scale (bakeDetail)
#include <QVector4D>   // dye bands + zone→detail-map table (bakeDetail)

#include <functional>

#include "model/Material.h"

class SnoIndex;

class CascReader;

// Free-function material/texture decoders shared by the model preview tabs. Given a
// CASC reader, the d4data dir, and a material name, these read the .mat.json role
// bindings and decode the referenced textures (CASC payload + d4data tex.json) into
// the images the viewport consumes. Self-contained so any tab can assemble + texture
// a model without depending on ModelsTab internals.
namespace MaterialDecode {

// ── Material NAME → SNO, from the LIVE game index ────────────────────────────────────────────
// Without this, a material is only resolvable two ways: its .mat.json in the d4data snapshot, or
// — for encrypted, nameless materials — the "~unnamed_<sno>" placeholder carrying its SNO. That
// left a whole class unreachable: materials that HAVE a real name but are absent from the
// snapshot. d4data is a community export and lags the game, so e.g. it ships every
// RogF/RogM_stor273 material while carrying no BarM_stor273 at all — those parts then rendered
// untextured even though the installed game had them all along.
//
// The index comes from the game's own CoreTOC, so it always matches the installed patch. Wiring
// it in here makes d4data a convenience rather than a requirement, and is why an asset can now
// resolve when the snapshot has never heard of it.
//
// Injected rather than #included so this stays free of tab/index dependencies; MainWindow sets it
// once the index is loaded. Unset (or unresolvable) simply falls back to the old behaviour.
void setNameResolver(std::function<qint64(const QString&)> fn);
// Placeholder SNO if the name is "~unnamed_<sno>", else the resolver's answer, else 0.
qint64 snoForMaterial(const QString& matName);

// Decode a texture by name + SNO (d4data tex.json for dims/format, CASC for pixels).
QImage texture(CascReader* reader, const QString& d4, const QString& texName, qint64 texSno);

// ── Run-scoped decode cache ──────────────────────────────────────────────────────────────────
// texture() is a CASC read plus a BC decode every single call, with no memory between calls, and
// the export path leans on that hard. Exporting a class's appearances re-decodes the SAME shared
// detail/leather/metal map once per model, and buildExportMats() re-decodes a material once per
// palette SLOT that names it — which for the empty-slot fallback is once per empty slot.
//
// Construct one of these around a batch and every repeat inside it is served from memory. Outside
// a scope nothing is cached and behaviour is exactly as before, so the viewport and the panels are
// untouched: this only pays off where the same textures come round again, and only there does it
// hold memory.
//
// SHARED ACROSS THREADS and mutex-guarded — deliberately, and this is the whole point. The first
// version made it thread_local, on the mistaken belief that textured model runs are serial. They are
// not: exportModels() hands its items to a std::thread pool (up to 16 workers) whenever a BatchSink
// is present, which is exactly the Bulk Extract case. A per-thread cache there would have been a
// pure no-op — each worker would open its own, share nothing with the others, and re-decode the same
// detail maps N times over — while the counters, read on the parent thread, reported zero decodes
// and 0% reuse. Sharing is also where the win is: those workers are decoding the SAME maps at the
// same moment.
//
// Shared, but NOT ambient: only threads that have opened a scope of their own take part. A Bulk
// Extract runs off the GUI thread, so without that rule every panel fill and thumbnail decoded
// while the run is in flight would evict the export's working set and inflate its hit counters.
// Open one on each worker you want included — exportModels() opens one per pool thread.
//
// The lock is held only around the hash lookup and the insert, never across a decode, so it cannot
// serialise the expensive part. Nesting is reference-counted; only the outermost scope frees.
// A scope may be constructed on one thread and destroyed on another, provided the threads that use
// it have been joined first — which exportModels() does before its scope ends.
//
// Bounded by an LRU cost budget in MB — a CEILING, not a reservation: the cache holds only what was
// actually decoded and evicts once past it, so a ten-model export never approaches the limit. Needed
// because a large run's working set is otherwise unbounded (one 2048x2048 RGBA map is 16 MB alone).
// Eviction only costs a re-decode; it is never wrong. D4_TEXCACHE_MB overrides the budget, and
// D4_NO_TEXCACHE removes it entirely, both without a rebuild.
//
// Nothing is held between exports: the heap allocation happens when the outermost scope opens and is
// freed when it closes. Idle, this costs one null pointer, one int, three counters and a QMutex.
class TextureCacheScope {
public:
    explicit TextureCacheScope(int budgetMB = 256);
    ~TextureCacheScope();
    TextureCacheScope(const TextureCacheScope&) = delete;
    TextureCacheScope& operator=(const TextureCacheScope&) = delete;

    // Counters for THIS scope, measured as a delta from when it was constructed, so a nested scope
    // reports its own work rather than inheriting the enclosing run's totals. hits+misses is the
    // number of texture() calls made while it was open. For the D4_DUMP_EXPORTPERF readout.
    qint64 hits() const;
    qint64 misses() const;
    qint64 bytesSaved() const;   // decoded bytes that did NOT have to be produced again

    // False when D4_NO_TEXCACHE is set — the scope was constructed but does nothing. That env var
    // is the baseline half of "Test - Export Cache.bat": it lets the same build time an export with
    // the cache off, so the comparison is one binary and one asset set, not two builds.
    bool disabled() const { return !m_active; }

private:
    qint64 m_hits0 = 0, m_misses0 = 0, m_saved0 = 0;   // counter values when this scope opened
    bool   m_active = false;                           // false = D4_NO_TEXCACHE, this scope is inert
};

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

// ── Detail compositing for EXPORT ────────────────────────────────────────────────────────────
// The three below were file-local to WardrobeTab2.cpp, which is the only reason `export/bakeDetail`
// worked for Wardrobe exports and was silently ignored by the Models tab, Stable and Bulk Extract.
// They are material compositing, so they live here with detailMapsSeparate — the decode that feeds
// them — rather than in a tab. Moved verbatim; every Wardrobe call site is unchanged.

// Detect the discrete dye-mask value bands the artist painted, straight from the DYE_MASK texture.
// D4's dye mask stores each material zone as a specific grey level (single-channel BC4). These
// levels vary per armor (barF_sets54 clusters near 0.06/0.10/0.29/0.58; others sit elsewhere), so
// hardcoding band centres is wrong — we read the actual histogram, merge nearby bins into clusters,
// and return the (up to) four most-populated centres sorted ascending. Fewer than four → the last
// centre is repeated so the extra zones collapse onto it. This is the game-data source of truth.
// A null/unusable mask returns the shipped fallback QVector4D(0.063, 0.345, 0.596, 0.831).
QVector4D detectDyeBands(const QImage& dyeMaskIn);

// Derive the dye-zone → detail-map table from which maps are actually present. Non-metal maps are
// assigned to zones 1..3 in slot order and CLAMPED to the last one (so a 4th zone with only two
// leather/fabric maps reuses the second); the metal map is excluded here (metalness routes metal
// texels to it in the shader). zone0 is the bare/lowest band → no detail. Returns 4 layers as floats.
QVector4D deriveZoneMap(const QVector<QImage>& detailN, int metalLayer);

// Bake the tiled, zone-routed detail maps into a part's exported normal + ORM(roughness) — a CPU port
// of the shader's detail block. Per texel: classify the DyeMask into a zone → detail layer, honour the
// metal routing (metal texels use the metal layer or fade non-metal grain out), sample the TILED detail
// normal/rough, combine the detail normal into the base normal's xy, and add the detail roughness. So
// exported armour carries the leather/fabric/brushed-metal surface grain instead of a smooth base map.
// `normal` and `orm` are edited IN PLACE (and converted to RGBA8888); a null `normal` is a no-op.
void bakeDetail(QImage& normal, QImage& orm, const QImage& dyeMask,
                const QImage detN[3], const QImage detR[3], const QVector3D& scale,
                const QVector4D& zoneMap, const QVector4D& bands, int metalLayer,
                float nInt, float rInt, float rOff);

// The whole export-time detail path for ONE material, in one call: read the authored per-map
// intensities, decode the detail maps and the dye mask, derive this material's bands + zone→map
// table, and bake into `normal`/`orm` IN PLACE. No-op when `normal` is null or the material has no
// detail normals. The caller gates on `export/bakeDetail`; nothing here reads QSettings.
//
// This exists so the Models/Bulk and Stable export paths cannot drift from each other — they have no
// per-part decode pass of their own to source these from, unlike Wardrobe, which assembles them
// during its own material pass and calls bakeDetail() directly.
//
// COST: one .mat.json read + a detail-texture decode + a DYE_MASK decode per material. Call it once
// per distinct material, not once per primitive — several primitives usually share a material.
void bakeDetailForMaterial(CascReader* reader, const QString& d4, const QString& matName,
                           QImage& normal, QImage& orm);

QImage baseColor(CascReader* reader, const QString& d4, const QString& matName);   // BASE_COLOR
QImage normalMap(CascReader* reader, const QString& d4, const QString& matName);   // NORMAL (raw RG)
QImage orm(CascReader* reader, const QString& d4, const QString& matName);         // AO(R)/rough(G)/metal(B)
void   factors(CascReader* reader, const QString& d4, const QString& matName,
               float& metal, float& rough);

// ── Cloth flags, positionally aligned with the roster ────────────────────────────────────────
// Both roster functions collapse snoOverrideMaterial > snoMaterial > snoCloth >
// snoHighQualityClothOverride into ONE name and, until now, threw away which field answered.
// That is the only authoritative "this entry is cloth" signal either format carries, and losing
// it forced the Wardrobe's SIM toggle to guess from material-name tokens ("_sim", "cape") plus a
// d4data file-existence probe — neither of which can work for encrypted armour. Pass `clothOut`
// to get it back; it is filled index-for-index with the returned roster.
//
// Default-look material roster of an appearance, indexed by primitive materialIndex
// (override > base > cloth, per ptAppearanceMaterials[].ptSOAs[0]).
QStringList appearanceRoster(const QString& d4, const QString& appName,
                             QVector<bool>* clothOut = nullptr);

// The same roster read from the CASC meta BINARY, for appearances that have no .app.json — i.e.
// every encrypted one. Names are resolved sno -> index name, so an encrypted material (itself
// nameless) comes back as "~unnamed_<sno>": positionally correct, which is what materialIndex
// needs, even though the texture lookup that follows still wants a real name.
// Empty when the blob carries no material array. See AppearanceMatBin for the derived layout.
// `why` (optional) receives the binary header numbers the read saw, and the reason it gave up when
// it returns empty. See AppearanceMatBin::read.
QStringList appearanceRosterFromMeta(const QByteArray& meta, const SnoIndex* idx,
                                     QString* why = nullptr, QVector<bool>* clothOut = nullptr);

// The texture SNOs a material references, via whichever route works — .mat.json when it exists,
// the meta binary when it does not. Used by the health audit to check a material can resolve its
// textures without paying for a pixel decode.
QVector<qint64> textureSnosFor(CascReader* reader, const QString& d4, const QString& matName);

// The material's full texture list, via whichever route works — .mat.json when it exists, the meta
// binary when it does not. THE ONLY correct way to read a material's textures: the Models tab used
// to open the JSON directly in five separate functions, so encrypted materials (1184 of them) fell
// out of every one and the model rendered grey with no explanation. Use this, never QFile.
QVector<MatTexture> texturesFor(CascReader* reader, const QString& d4, const QString& matName);

}  // namespace MaterialDecode
