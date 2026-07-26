#include "tabs/WardrobeTab2.h"
#include "util/ViewportPartMenu.h"
#include "app/AppPaths.h"         // ensemble thumbnails live in the portable data folder
#include "tabs/PanelBox.h"        // the stacking-toggle panel system, shared with the Models tab
#include "tabs/ModelOutliner.h"   // kindIcon — the same strip glyphs the Models tab uses
#include "tabs/ViewGlyphs.h"      // shadeBallGlyph + overlayGlyph — the Models toolbar's glyphs
#include "tabs/HintBar.h"
#include "tabs/IconBadge.h"       // ✓/✗ model-presence overlay on look cards

#include <QStyle>                 // polish/unpolish — re-evaluate the [panelOpen] selector
#include <QWidgetAction>          // the Channel combo rides inside the shading ⌄ popover

#include "app/Config.h"
#include "app/ExportNotifier.h"

#include <QElapsedTimer>
#include "app/SehGuard.h"          // hardware-fault guard for parse/decode/GPU/anim paths
#include "app/SettingsDialog.h"
#include "casc/CascReader.h"
#include "gl/GLModelWidget.h"
#include "app/ExportCapture.h"
#include <QHideEvent>
#include <QProgressDialog>
#include "index/AnimActionIndex.h"
#include "index/AppearanceMeta.h"
#include "index/IconIndex.h"
#include "index/CoreToc.h"
#include "index/SnoIndex.h"
#include "model/AnimParser.h"
#include "model/Material.h"
#include "model/Retarget.h"
#include "model/Hardpoints.h"
#include "model/MaterialDecode.h"
#include "tabs/MarkingCompose.h"
#include "tex/BcDecode.h"
#include "tex/TexMeta.h"
#include "model/ModelParser.h"
#include "index/ItemHoverIndex.h"
#include "util/DyeColorWheel.h"
#include "util/HoverInfo.h"
#include "util/PanelPersist.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QFont>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDrag>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDebug>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QCursor>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDirIterator>
#include <QFrame>
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QLineEdit>
#include <QListWidget>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPair>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QSpinBox>
#include <QPlainTextEdit>
#include <QFontDatabase>
#include <QPushButton>
#include <QRegularExpression>
#include <QGridLayout>
#include <QListView>
#include <QScrollArea>
#include <QShortcut>
#include <QStackedWidget>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QShowEvent>
#include <QSlider>
#include <QTimer>
#include <QThreadPool>
#include <utility>
#include <QWheelEvent>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QToolButton>
#include <QMenu>
#include <QScrollBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVector3D>
#include <QVector4D>

#include <algorithm>
#include <cmath>
#include <functional>
#include <thread>

namespace {
constexpr int kGroupAppearance = 9;
constexpr int kGroupTexture = 44;

// Painter-drawn playback transport icons — 0 play · 1 pause · 2 step-back · 3 step-forward.
// (Copied verbatim from the Models tab so the Wardrobe transport bar matches it exactly.)
QIcon transportGlyph(int kind)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c(210, 205, 190);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    switch (kind) {
    case 0:   p.drawPolygon(QPolygonF({{4.5, 2.5}, {4.5, 13.5}, {13.0, 8.0}})); break;         // play
    case 1:   p.drawRect(4, 3, 3, 10); p.drawRect(9, 3, 3, 10); break;                          // pause
    case 2:   p.drawRect(3, 3, 2, 10); p.drawPolygon(QPolygonF({{13.0, 3.0}, {13.0, 13.0}, {6.0, 8.0}})); break;  // step back
    case 3:   p.drawPolygon(QPolygonF({{3.0, 3.0}, {3.0, 13.0}, {10.0, 8.0}})); p.drawRect(11, 3, 2, 10); break;  // step fwd
    }
    return QIcon(pm);
}

// Material-name classifiers for the FX / SIM part toggles. D4 names cosmetic effect and
// cloth-sim submeshes with recognizable tokens, but the set is broad — beyond plain "fx"
// you get *_eyewisps, *_trailskull, *_shoulder_smoke (FX), and beyond "cloth"/"_sim" you
// get *_skirt3, *_skirtlong, *_cape, *_sash (SIM). Match generously so these stragglers are
// caught by the toggles; substring + case-insensitive.
inline bool nameHasAny(const QString& s, std::initializer_list<const char*> toks) {
    for (const char* t : toks)
        if (s.contains(QLatin1String(t), Qt::CaseInsensitive)) return true;
    return false;
}
inline bool isFxName(const QString& m) {
    return nameHasAny(m, { "fx", "vfx", "effect", "glow", "wisp", "smoke", "trail",
                           "particle", "mist", "swirl", "spark", "flare", "steam",
                           "vapor", "vapour", "tendril", "_wisps", "_smoke" });
}
inline bool isSimName(const QString& m) {
    return nameHasAny(m, { "cloth", "_sim", "skirt", "cape", "cloak", "loin", "tabard",
                           "sash", "drape", "scarf", "ribbon", "tassel", "fringe",
                           "banner", "dangle", "frill", "loincloth" });
}

// ── Game-data classifiers (preferred over names) ─────────────────────────────
// FX submeshes render with a "vfx_*" shader map (vfx_actor_blend_uber_unlit…,
// vfx_particle_blend_uber_unlit) while armour uses "hero_opaque…". The shader is the
// authoritative signal — it's what makes the mesh an unlit, blended effect — so a piece
// named *_shell / *_ember / *_eyewisps is caught regardless of token. Returns the shader
// map name, or empty if the material file can't be read (e.g. cloth submeshes reuse the
// body material and have no own .mat, so this stays empty and the SIM path classifies them).
inline QString shaderMapOf(const QString& d4, const QString& matName) {
    if (d4.isEmpty() || matName.isEmpty()) return QString();
    QFile f(d4 + QStringLiteral("/json/base/meta/Material/") + matName + QStringLiteral(".mat.json"));
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QJsonDocument::fromJson(f.readAll()).object()
               .value(QStringLiteral("tUberMaterial")).toObject()
               .value(QStringLiteral("snoShaderMap")).toObject()
               .value(QStringLiteral("name")).toString();
}
inline bool shaderIsFx(const QString& shader) {
    return shader.startsWith(QLatin1String("vfx"), Qt::CaseInsensitive);
}
// SIM submeshes have an authored NvCloth definition file keyed by the submesh name
// (base/meta/Cloth/<name>.clt.json, or <name>_sim.clt.json — both conventions exist).
// Its presence means the game simulates that submesh, so this catches skirts/capes/sashes
// regardless of token, and avoids false positives on look-alike armour names.
inline bool hasClothDef(const QString& d4, const QString& name) {
    if (d4.isEmpty() || name.isEmpty()) return false;
    const QString base = d4 + QStringLiteral("/json/base/meta/Cloth/") + name;
    return QFile::exists(base + QStringLiteral(".clt.json"))
        || QFile::exists(base + QStringLiteral("_sim.clt.json"));
}
// FUR submeshes use the engine's shell-fur path (shaders hero_opaque_fur_dualNoise, etc.).
// The clean game-data signal is dwFlags bit 5 (0x20): set on every fur material we sampled
// (33, 2081) and clear on non-fur variants (1) and normal armour (2049). We also accept a
// "*_fur_*" shader name as a fallback. The mesh then carries a furMask (MASK_PRIMARY) +
// strand noise (NOISE_PROCEDURAL) for the shells.
// Cached composited detail maps + their summed strengths (per material).
struct DetailCacheEntry { QVector<QImage> n, r; float ns = 0, rs = 0, ro = 0, ca = 0;
                          float sc[3] = {8, 8, 8}; int ml = -1; };   // n/r sized 3; sc = per-map tiling; ml = metal-map layer
// Read an authored FX scalar from a material's ptRunTimeMaterialValues (real game data, e.g.
// "Color Intensity" = emissive brightness, "Vertex Normal Offset Intensity" = undulation).
// Matches by name prefix; returns the fallback when absent.
inline float fxScalar(const QString& d4, const QString& matName, const char* valueName, float fallback) {
    if (d4.isEmpty() || matName.isEmpty()) return fallback;
    QFile f(d4 + QStringLiteral("/json/base/meta/Material/") + matName + QStringLiteral(".mat.json"));
    if (!f.open(QIODevice::ReadOnly)) return fallback;
    const QJsonObject um = QJsonDocument::fromJson(f.readAll()).object()
                               .value(QStringLiteral("tUberMaterial")).toObject();
    const QLatin1String want(valueName);
    for (const QJsonValue& rv : um.value(QStringLiteral("ptRunTimeMaterialValues")).toArray())
        for (const QJsonValue& sv : rv.toObject().value(QStringLiteral("arMaterialScalarValues")).toArray()) {
            const QJsonObject tv = sv.toObject().value(QStringLiteral("tValue")).toObject();
            if (tv.value(QStringLiteral("snoMaterialValue")).toObject()
                  .value(QStringLiteral("name")).toString().startsWith(want, Qt::CaseInsensitive))
                return float(tv.value(QStringLiteral("value")).toDouble());
        }
    return fallback;
}
// Authored "emissive color" (a vector MaterialValue). The emissive texture is often just a
// grayscale mask, so this colour is what the glow should be (gold, blue, …). White when absent
// → the texture's own colour is used unchanged.
inline QColor emissiveColorOf(const QString& d4, const QString& matName) {
    if (d4.isEmpty() || matName.isEmpty()) return QColor(255, 255, 255);
    QFile f(d4 + QStringLiteral("/json/base/meta/Material/") + matName + QStringLiteral(".mat.json"));
    if (!f.open(QIODevice::ReadOnly)) return QColor(255, 255, 255);
    const QJsonObject um = QJsonDocument::fromJson(f.readAll()).object()
                               .value(QStringLiteral("tUberMaterial")).toObject();
    for (const QJsonValue& rv : um.value(QStringLiteral("ptRunTimeMaterialValues")).toArray())
        for (const QJsonValue& sv : rv.toObject().value(QStringLiteral("arMaterialVectorValues")).toArray()) {
            const QJsonObject tv = sv.toObject().value(QStringLiteral("tValue")).toObject();
            if (tv.value(QStringLiteral("snoMaterialValue")).toObject()
                  .value(QStringLiteral("name")).toString().startsWith(QLatin1String("emissive color"), Qt::CaseInsensitive)) {
                const QJsonObject c = tv.value(QStringLiteral("value")).toObject();
                return QColor::fromRgbF(qBound(0.0, c.value(QStringLiteral("x")).toDouble(), 1.0),
                                        qBound(0.0, c.value(QStringLiteral("y")).toDouble(), 1.0),
                                        qBound(0.0, c.value(QStringLiteral("z")).toDouble(), 1.0));
            }
        }
    return QColor(255, 255, 255);
}
inline bool isFurMaterial(const QString& d4, const QString& matName) {
    if (matName.isEmpty()) return false;
    if (d4.isEmpty()) return matName.contains(QLatin1String("_fur"), Qt::CaseInsensitive);
    QFile f(d4 + QStringLiteral("/json/base/meta/Material/") + matName + QStringLiteral(".mat.json"));
    if (!f.open(QIODevice::ReadOnly)) return matName.contains(QLatin1String("_fur"), Qt::CaseInsensitive);
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if ((root.value(QStringLiteral("dwFlags")).toInt() & 0x20) != 0) return true;
    if (root.value(QStringLiteral("tUberMaterial")).toObject()
            .value(QStringLiteral("snoShaderMap")).toObject()
            .value(QStringLiteral("name")).toString().contains(QLatin1String("fur"), Qt::CaseInsensitive))
        return true;
    return matName.contains(QLatin1String("_fur"), Qt::CaseInsensitive);
}

// Card outline styles. The equipped (checked) card gets a white border; a theme-matching
// card gets a blue border. Both styles keep the :checked rule so the equipped item stays
// clearly marked even when it is ALSO a theme match (the :checked selector wins over the
// plain QToolButton rule). Cleared cards still use the base sheet, not an empty string, so
// the native checked look is never the only indicator.
const QString kCardBaseQss  = QStringLiteral(
    "QToolButton:checked{border:2px solid #ffffff;border-radius:4px;}");
const QString kCardMatchQss = QStringLiteral(
    "QToolButton{border:2px solid #46c2ff;border-radius:4px;}"
    "QToolButton:checked{border:2px solid #ffffff;border-radius:4px;}");
// Keyboard cursor outline (look grid): a dashed gold border marks the focused card before
// it's equipped (press Enter/Space to equip), so arrowing around doesn't reload geometry.
const QString kCardCursorQss = QStringLiteral(
    "QToolButton{border:2px dashed #ffd24a;border-radius:4px;}");
// Saved custom pigments get a gold border so they stand out from the built-in dyes.
const QString kCardCustomQss = QStringLiteral(
    "QToolButton{border:2px solid #c9a24b;border-radius:4px;}"
    "QToolButton:hover{border-color:#e6c266;}"
    "QToolButton:checked{border:2px solid #ffffff;border-radius:4px;}");

// Weapon types, keyed by the real appearance-name prefix in the game data. Each maps
// to its ItemType (carries fUsableByClass + the in-hand grip offset) plus flags for
// which hand(s) can hold it. (Driven entirely by real game data — see the d4data
// Appearance/ and ItemType/ folders.)
struct WeapTypeDef {
    const char* prefix;     // appearance-name prefix (case-insensitive)
    const char* label;      // UI label
    const char* itemType;   // ItemType/<name>.itt.json
    bool twoHand;           // can't go in the off hand
    bool offHandOnly;       // off-hand item only (shield / focus / totem)
};
const WeapTypeDef kWeapTypes[] = {
    {"sword",               "Sword (1H)",        "Sword",        false, false},
    {"twoHandSword",        "Sword (2H)",        "Sword2H",      true,  false},
    {"axe",                 "Axe (1H)",          "Axe",          false, false},
    {"twoHandAxe",          "Axe (2H)",          "Axe2H",        true,  false},
    {"mace",                "Mace (1H)",         "Mace",         false, false},
    {"twoHandMace",         "Mace (2H)",         "Mace2H",       true,  false},
    {"dagger",              "Dagger",            "Dagger",       false, false},
    {"scythe",              "Scythe (1H)",       "Scythe",       false, false},
    {"twoHandScythe",       "Scythe (2H)",       "Scythe2H",     true,  false},
    {"wand",                "Wand",              "Wand",         false, false},
    {"twoHandPolearm",      "Polearm",           "Polearm",      true,  false},
    {"twoHandSorcStaff",    "Staff (Sorcerer)",  "StaffSorcerer",true,  false},
    {"twoHandDruidStaff",   "Staff (Druid)",     "StaffDruid",   true,  false},
    {"twoHandStaff",        "Staff",             "Staff",        true,  false},
    {"twoHandBow",          "Bow",               "Bow",          true,  false},
    {"twoHandCrossbow",     "Crossbow",          "Crossbow",     true,  false},
    {"twoHandGlaive",       "Glaive",            "Glaive",       true,  false},
    {"twoHandQuarterstaff", "Quarterstaff",      "Quarterstaff", true,  false},
    {"Flail",               "Flail",             "Flail",        false, false},
    {"shield",              "Shield",            "Shield",       false, true},
    {"offHandFocus",        "Focus",             "Focus",        false, true},
    {"OffHandTotem",        "Totem",             "OffHandTotem", false, true},
};
const WeapTypeDef* weapTypeByLabel(const QString& label)
{
    for (const WeapTypeDef& w : kWeapTypes)
        if (label == QLatin1String(w.label)) return &w;
    return nullptr;
}

struct SlotDef { const char* label; const char* code; };
const SlotDef kSlots[5] = {
    {"Helm", "HLM"}, {"Torso", "TRS"}, {"Gloves", "GLV"}, {"Legs", "LEG"}, {"Boots", "BTS"}};

// fubc = index into entries' fUsableByClass array (game-fixed class order).
struct ClassDef { const char* name; const char* code; int fubc; };
const ClassDef kClasses[8] = {
    {"Barbarian", "bar", 2}, {"Druid", "dru", 1}, {"Necromancer", "nec", 4},
    {"Paladin", "pal", 6}, {"Rogue", "rog", 3}, {"Sorcerer", "sor", 0},
    {"Spiritborn", "spi", 5}, {"Warlock", "war", 7}};
constexpr int kNumClasses = 8;


// The nine character-creator categories: UI label, SNO folder, file extension.
struct CreatorCat { const char* label; const char* folder; const char* ext; };
const CreatorCat kCreator[9] = {
    {"Face",          "Face",         ".fac.json"},
    {"Hair style",    "HairStyle",    ".har.json"},
    {"Hair colour",   "HairColor",    ".hcl.json"},
    {"Eye colour",    "EyeColor",     ".eye.json"},
    {"Facial hair",   "FacialHair",   ".fhr.json"},
    {"Makeup",        "Makeup",       ".mak.json"},
    {"Marking",       "MarkingShape", ".msh.json"},
    {"Marking colour","MarkingColor", ".mcl.json"},
    {"Jewelry",       "Jewelry",      ".jwl.json"}};
constexpr int kHairColorCat = 2;   // index into kCreator

// Catalogue one creator category: file stems usable by the given class (fubc index)
// and gender. Entries without fUsableByClass (e.g. MarkingShape/Color) are always included.
QStringList creatorEntries(const QString& d4, const CreatorCat& cat, int fubc, bool male)
{
    QStringList out;
    if (d4.isEmpty()) return out;
    QDir dir(d4 + QStringLiteral("/json/base/meta/") + QString::fromLatin1(cat.folder));
    const int extLen = int(QString::fromLatin1(cat.ext).size());
    for (const QString& fn : dir.entryList(QStringList{QStringLiteral("*") + QString::fromLatin1(cat.ext)}, QDir::Files)) {
        const QString stem = fn.left(fn.size() - extLen);
        if (stem.contains(QLatin1String("Bad Data"))) continue;
        // Gender lock: facial hair (and any gender-named creator asset) is authored per gender
        // as Global_Male_* / Global_Female_*. Hide the wrong gender's entries so a female
        // character doesn't get male beards (note "female" contains "male", so test it first).
        {
            const QString sl = stem.toLower();
            const bool nameFemale = sl.contains(QLatin1String("female"));
            const bool nameMale   = !nameFemale && sl.contains(QLatin1String("male"));
            if (nameMale && !male) continue;
            if (nameFemale && male) continue;
        }
        QFile f(dir.filePath(fn));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        // Class lock #1: fUsableByClass (Face/Hair/Eye/Makeup/Jewelry, …).
        const QJsonArray uc = o.value(QStringLiteral("fUsableByClass")).toArray();
        if (!uc.isEmpty() && fubc >= 0 && fubc < uc.size() && uc[fubc].toInt() == 0)
            continue;   // not usable by this class
        // Class lock #2: eClassRestriction (MarkingShape/Jewelry) — a class index;
        // out-of-range (-1 / ≥8) means global/unrestricted, so keep those.
        if (o.contains(QStringLiteral("eClassRestriction"))) {
            const int r = o.value(QStringLiteral("eClassRestriction")).toInt(-1);
            if (r >= 0 && r < 8 && r != fubc) continue;
        }
        out << stem;
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

// dwSubObjectStyle of a creator def (HairStyle/Jewelry) → the style index NN that
// names its mesh appearance. Returns -1 if absent.
int styleOf(const QString& d4, const char* folder, const char* ext, const QString& stem)
{
    if (stem.isEmpty()) return -1;
    QFile f(d4 + QStringLiteral("/json/base/meta/") + QLatin1String(folder)
            + QStringLiteral("/") + stem + QLatin1String(ext));
    if (!f.open(QIODevice::ReadOnly)) return -1;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    return o.contains(QStringLiteral("dwSubObjectStyle"))
               ? o.value(QStringLiteral("dwSubObjectStyle")).toInt(-1) : -1;
}

// Facial-hair style index NN → its beard mesh appearance <pref>_B<NN>. FacialHair defs carry the
// style in "unk_2ab2122" (there's no dwSubObjectStyle). 0/1 = Clean/Stubble (no mesh — texture only),
// 2..9 = real beards (male-only meshes barM_B02…barM_B09, per class). Returns -1 if absent.
int facialHairStyle(const QString& d4, const QString& stem)
{
    if (stem.isEmpty()) return -1;
    QFile f(d4 + QStringLiteral("/json/base/meta/FacialHair/") + stem + QStringLiteral(".fhr.json"));
    if (!f.open(QIODevice::ReadOnly)) return -1;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    return o.contains(QStringLiteral("unk_2ab2122"))
               ? o.value(QStringLiteral("unk_2ab2122")).toInt(-1) : -1;
}

// Player skin-tone palette from a class Actor's ptPlayerData.arSkinColorChoices
// (identical across classes). Each entry: display label + swatch colour. Test/dev
// entries (dead/debug) are skipped.
QVector<QPair<QString, QColor>> loadSkinTones(const QString& d4)
{
    QVector<QPair<QString, QColor>> out;
    if (d4.isEmpty()) return out;
    QFile f(d4 + QStringLiteral("/json/base/meta/Actor/barbarianF.acr.json"));
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonArray pd = root.value(QStringLiteral("ptPlayerData")).toArray();
    if (pd.isEmpty()) return out;
    const QJsonArray choices = pd[0].toObject().value(QStringLiteral("arSkinColorChoices")).toArray();
    for (const QJsonValue& v : choices) {
        const QJsonObject c = v.toObject();
        const QString label = c.value(QStringLiteral("szLabel")).toString();
        const QString ll = label.toLower();
        if (ll.contains(QLatin1String("dead")) || ll.contains(QLatin1String("debug"))
            || ll.contains(QLatin1String("test"))) continue;
        const QJsonObject rc = c.value(QStringLiteral("rgbaUIDisplayColor")).toObject();
        out.append({label, QColor(rc.value(QStringLiteral("r")).toInt(), rc.value(QStringLiteral("g")).toInt(),
                                  rc.value(QStringLiteral("b")).toInt())});
    }
    return out;
}

// Find a material's bound texture whose name contains `substr` → (texName, SNO).
// Used to pull skin-detail overlays (Freckle/Vitiligo) straight from the skin material,
// which carries the authoritative texture SNO (role 235/236).
QPair<QString, qint64> matTexContaining(const QString& d4, const QString& matName, const QString& substr)
{
    if (matName.isEmpty()) return {};
    QFile f(d4 + QStringLiteral("/json/base/meta/Material/") + matName + QStringLiteral(".mat.json"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonArray tl = o.value(QStringLiteral("tUberMaterial")).toObject()
                              .value(QStringLiteral("ptMatTexList")).toArray();
    for (const QJsonValue& v : tl) {
        const QJsonObject sno = v.toObject().value(QStringLiteral("tMatTex")).toObject()
                                    .value(QStringLiteral("snoTex")).toObject();
        const QString nm = sno.value(QStringLiteral("name")).toString();
        if (!nm.isEmpty() && nm.contains(substr, Qt::CaseInsensitive))
            return {nm.section('/', -1), qint64(sno.value(QStringLiteral("__raw__")).toDouble())};
    }
    return {};
}

// Split a CamelCase / underscore-joined token into spaced words: "FullGoatee" → "Full Goatee",
// "Scar_ChinGoatee" → "Scar Chin Goatee". Used to humanise raw creator def names.
QString prettyToken(QString s)
{
    s.replace('_', ' ');
    QString out;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (i > 0 && c.isUpper()
            && (s[i-1].isLower() || (s[i-1].isUpper() && i+1 < s.size() && s[i+1].isLower())))
            out += ' ';
        out += c;
    }
    return out.simplified();
}

// Humanise a raw creator def stem for the dropdown by cleaning up the REAL identifier only — no
// invented names. Strips sort/code prefixes ("H03_", a leading "001…"), collapses marking noise
// ("Barbarian_bodyMarking_01" → "Barbarian 1"), splits CamelCase, drops leading zeros. Defs with
// no descriptive name (Makeup "05", Jewelry "Global_00") keep their real id, just spaced.
// Already-clean names ("African", "Blue Dark", "Agent of Anguish") pass through unchanged.
QString prettyCreatorName(const QString& stem)
{
    QString s = stem;
    s.remove(QRegularExpression(QStringLiteral("^H\\d+_")));                       // hair-style code
    s.replace(QRegularExpression(QStringLiteral("_?bodyMarking_?"),
              QRegularExpression::CaseInsensitiveOption), QStringLiteral(" "));     // marking noise
    s.remove(QRegularExpression(QStringLiteral("^\\d+(?=[A-Za-z])")));             // "001DeepBrown"
    QString out = prettyToken(s);
    out.replace(QRegularExpression(QStringLiteral("\\b0+(\\d)")), QStringLiteral("\\1"));  // "01"→"1"
    out = out.simplified();
    bool numeric = !out.isEmpty();
    for (const QChar c : out) if (!c.isDigit() && !c.isSpace()) { numeric = false; break; }
    if (out.isEmpty() || numeric) return prettyToken(stem);   // keep the real id, never fabricate
    return out;
}

// The REAL localized display name of a creator def, from the game's string table at
// enUS_Text/meta/StringList/<folder>_<stem>.stl.json (the "Name" entry). This is exactly what the
// in-game creator shows ("Makeup/05" → "Caldean Rouge", "Jewelry/Global_21" → "Twin Crescents").
// Facial-hair names are gendered ("|5 Evening Dust : Dry Brush"): we strip the leading "|N" code and
// pick the female (before " : ") or male (after) variant. Empty if the game ships no string for it.
QString creatorRealName(const QString& d4, const char* folder, const QString& stem, bool male)
{
    QFile f(d4 + QStringLiteral("/json/enUS_Text/meta/StringList/")
            + QLatin1String(folder) + QStringLiteral("_") + stem + QStringLiteral(".stl.json"));
    if (!f.open(QIODevice::ReadOnly)) return QString();
    const QJsonArray a = QJsonDocument::fromJson(f.readAll()).object()
                             .value(QStringLiteral("arStrings")).toArray();
    QString name;
    for (const QJsonValue& v : a) {
        const QJsonObject e = v.toObject();
        if (e.value(QStringLiteral("szLabel")).toString() == QLatin1String("Name")) {
            name = e.value(QStringLiteral("szText")).toString(); break;
        }
    }
    if (name.isEmpty() && !a.isEmpty()) name = a.first().toObject().value(QStringLiteral("szText")).toString();
    name.remove(QRegularExpression(QStringLiteral("^\\|\\d+\\s*")));   // gender-form code, e.g. "|5 "
    if (name.contains(QStringLiteral(" : "))) {                        // "<female> : <male>"
        const QStringList g = name.split(QStringLiteral(" : "));
        name = (male && g.size() > 1) ? g[1] : g[0];
    }
    return name.trimmed();
}

// The gender-correct shell material of a FacialHair def (e.g. Global_Female_Facialhair_05_Arched),
// which carries the beard _Alpha/_Normal/_Mask textures. Empty = keep the face's own.
QString facialHairMat(const QString& d4, const QString& stem, bool male)
{
    if (stem.isEmpty()) return QString();
    QFile f(d4 + QStringLiteral("/json/base/meta/FacialHair/") + stem + QStringLiteral(".fhr.json"));
    if (!f.open(QIODevice::ReadOnly)) return QString();
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const QString key = male ? QStringLiteral("snoShellMaterialM") : QStringLiteral("snoShellMaterialF");
    return o.value(key).toObject().value(QStringLiteral("name")).toString().section('/', -1);
}

// The "clean" (beardless) FEMALE facial-hair shell material. Used as the female default so a
// female character isn't rendered with the face piece's baked male-stubble placeholder. Scans
// the FacialHair folder for a female "clean" (…_00_Clean) def, falling back to any female def.
QString femaleCleanFacialHairMat(const QString& d4)
{
    if (d4.isEmpty()) return QString();
    QDir dir(d4 + QStringLiteral("/json/base/meta/FacialHair"));
    QString best;
    for (const QString& fn : dir.entryList(QStringList{QStringLiteral("*.fhr.json")}, QDir::Files)) {
        const QString sl = fn.toLower();
        if (!sl.contains(QLatin1String("female"))) continue;                 // female-authored only
        const QString stem = fn.left(fn.size() - 9);                          // strip ".fhr.json"
        if (sl.contains(QLatin1String("clean")) || sl.contains(QLatin1String("_00"))) {
            best = stem; break;                                              // prefer the clean/00 entry
        }
        if (best.isEmpty()) best = stem;                                     // else remember the first female one
    }
    return best.isEmpty() ? QString() : facialHairMat(d4, best, false);
}

// Mid colour of a HairColor definition's 3-colour ramp (for tinting the hair material).
QColor hairColorMid(const QString& d4, const QString& stem)
{
    QFile f(d4 + QStringLiteral("/json/base/meta/HairColor/") + stem + QStringLiteral(".hcl.json"));
    if (!f.open(QIODevice::ReadOnly)) return QColor();
    const QJsonArray a = QJsonDocument::fromJson(f.readAll()).object()
                             .value(QStringLiteral("rgbaColors")).toArray();
    if (a.isEmpty()) return QColor();
    const QJsonObject c = a[a.size() / 2].toObject();
    return QColor(c.value(QStringLiteral("r")).toInt(), c.value(QStringLiteral("g")).toInt(),
                  c.value(QStringLiteral("b")).toInt());
}

// Full HairColor ramp stops (dark root/shadow → bright length → tip). D4 colours hair by
// GRADIENT-MAPPING the grayscale strand texture through this ramp — that tonal spread across the
// strands is the hair's depth. We previously collapsed the ramp to its middle stop → flat hair.
QVector<QColor> hairColorRamp(const QString& d4, const QString& stem)
{
    QVector<QColor> out;
    QFile f(d4 + QStringLiteral("/json/base/meta/HairColor/") + stem + QStringLiteral(".hcl.json"));
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonArray a = QJsonDocument::fromJson(f.readAll()).object()
                             .value(QStringLiteral("rgbaColors")).toArray();
    for (const QJsonValue& v : a) { const QJsonObject c = v.toObject();
        out << QColor(c.value(QStringLiteral("r")).toInt(), c.value(QStringLiteral("g")).toInt(),
                      c.value(QStringLiteral("b")).toInt()); }
    return out;
}

// Gradient-map a grayscale strand base through the hair ramp: each texel's luminance walks the ramp
// (dark gaps/roots → first stop, bright lit strands → last stop). This is D4's own hair-colour
// method and restores the tonal depth a flat tint destroys. Alpha (the strand cutout) is preserved.
QImage hairGradientMap(QImage img, const QVector<QColor>& ramp)
{
    if (img.isNull() || ramp.size() < 2) return img;
    img = img.convertToFormat(QImage::Format_RGBA8888);
    QRgb lut[256];
    const int N = ramp.size();
    for (int i = 0; i < 256; ++i) {
        const float t = i / 255.0f * float(N - 1);
        const int i0 = qMin(N - 1, int(t)); const int i1 = qMin(N - 1, i0 + 1); const float f = t - float(i0);
        const QColor& a = ramp[i0]; const QColor& b = ramp[i1];
        lut[i] = qRgb(int(a.red()   + (b.red()   - a.red())   * f),
                      int(a.green() + (b.green() - a.green()) * f),
                      int(a.blue()  + (b.blue()  - a.blue())  * f));
    }
    for (int y = 0; y < img.height(); ++y) {
        uchar* s = img.scanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            const int lum = (s[x*4]*30 + s[x*4+1]*59 + s[x*4+2]*11) / 100;
            const QRgb c = lut[lum & 0xFF];
            // Retain the strand texture's own brightness as a mild AO so gaps/roots stay DARKER (not
            // just differently-coloured) — restores the occlusion depth the flat colour-map loses.
            // D4 hair has NO AO texture — depth comes from the grayscale base's own baked strand
            // shading (dark gaps/roots) + dithered self-shadowing. Preserve that shading strongly as
            // brightness so gaps read as deep occlusion, not lifted flat colour. Gamma the luminance
            // so mid-strands stay bright while gaps crush dark.
            const float ln = lum / 255.0f;
            const float shade = 0.12f + 0.88f * ln * ln;   // gaps → ~0.12 (deep AO), lit strands → full
            s[x*4]   = uchar(qBound(0, int(qRed(c)   * shade), 255));
            s[x*4+1] = uchar(qBound(0, int(qGreen(c) * shade), 255));
            s[x*4+2] = uchar(qBound(0, int(qBlue(c)  * shade), 255));   // alpha kept
        }
    }
    return img;
}

// D4's ACTUAL hair colouring. The base _Color is a flat white strand-SHAPE map (no shading); the
// MASK_PRIMARY carries the root→tip gradient (dark roots at the scalp → light tips). D4 walks the
// HairColor ramp by that mask coordinate — dark-maroon roots → bright-copper mid-length → medium
// tips — AND uses the mask as strand AO (roots occluded). That mask is the hair's real depth; the
// base only supplies the alpha cutout. Falls back to the base-luminance map when no mask exists.
QImage hairColorFromMask(QImage base, const QImage& maskIn, const QVector<QColor>& ramp)
{
    if (base.isNull() || ramp.size() < 2) return base;
    base = base.convertToFormat(QImage::Format_RGBA8888);
    if (maskIn.isNull()) return hairGradientMap(base, ramp);
    QImage m = maskIn.convertToFormat(QImage::Format_RGBA8888);
    if (m.size() != base.size())          // the mask shares the base's UVs; align its grid
        m = m.scaled(base.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QRgb lut[256];
    const int N = ramp.size();
    for (int i = 0; i < 256; ++i) {
        const float t = i / 255.0f * float(N - 1);
        const int i0 = qMin(N - 1, int(t)); const int i1 = qMin(N - 1, i0 + 1); const float f = t - float(i0);
        const QColor& a = ramp[i0]; const QColor& b = ramp[i1];
        lut[i] = qRgb(int(a.red()   + (b.red()   - a.red())   * f),
                      int(a.green() + (b.green() - a.green()) * f),
                      int(a.blue()  + (b.blue()  - a.blue())  * f));
    }
    for (int y = 0; y < base.height(); ++y) {
        uchar* d = base.scanLine(y); const uchar* s = m.constScanLine(y);
        for (int x = 0; x < base.width(); ++x) {
            const int mv = (s[x*4]*30 + s[x*4+1]*59 + s[x*4+2]*11) / 100;   // root(0) → tip(255)
            const QRgb c = lut[mv & 0xFF];
            const float ao = 0.42f + 0.58f * (mv / 255.0f);                // roots deep but keep colour
            d[x*4]   = uchar(qBound(0, int(qRed(c)   * ao), 255));
            d[x*4+1] = uchar(qBound(0, int(qGreen(c) * ao), 255));
            d[x*4+2] = uchar(qBound(0, int(qBlue(c)  * ao), 255));         // base alpha (cutout) kept
        }
    }
    return base;
}

// Read an r/g/b colour object that may be 0–1 floats or 0–255 ints.
QColor readColorObj(const QJsonObject& c)
{
    if (c.isEmpty()) return QColor();
    const double r = c.value(QStringLiteral("r")).toDouble(), g = c.value(QStringLiteral("g")).toDouble(),
                 b = c.value(QStringLiteral("b")).toDouble();
    const bool unit = (r <= 1.0 && g <= 1.0 && b <= 1.0);
    return QColor(int(qBound(0.0, unit ? r*255 : r, 255.0)),
                  int(qBound(0.0, unit ? g*255 : g, 255.0)),
                  int(qBound(0.0, unit ? b*255 : b, 255.0)));
}

// Iris colour of an EyeColor def (the procedural Hero_Eye shader can't be run, so the
// preview tints the eye toward this).
QColor eyeIrisColor(const QString& d4, const QString& stem)
{
    if (stem.isEmpty()) return QColor();
    QFile f(d4 + QStringLiteral("/json/base/meta/EyeColor/") + stem + QStringLiteral(".eye.json"));
    if (!f.open(QIODevice::ReadOnly)) return QColor();
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    QColor c = readColorObj(o.value(QStringLiteral("rgbaIrisInner")).toObject());
    if (!c.isValid()) c = readColorObj(o.value(QStringLiteral("rgbaIrisOuter")).toObject());
    if (!c.isValid()) c = readColorObj(o.value(QStringLiteral("rgbaUIDisplayColor")).toObject());
    return c;
}

// EyeColor flIrisRoughness — the cornea wetness (low = glossy/wet eye with a live catchlight).
float eyeIrisRoughness(const QString& d4, const QString& stem)
{
    if (stem.isEmpty()) return 0.10f;
    QFile f(d4 + QStringLiteral("/json/base/meta/EyeColor/") + stem + QStringLiteral(".eye.json"));
    if (!f.open(QIODevice::ReadOnly)) return 0.10f;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonValue v = o.value(QStringLiteral("flIrisRoughness"));
    return v.isDouble() ? float(v.toDouble()) : 0.10f;
}

// EyeColor rgbaIrisOuter — the limbus-edge iris colour (paired with rgbaIrisInner near the pupil).
QColor eyeIrisOuterColor(const QString& d4, const QString& stem)
{
    if (stem.isEmpty()) return QColor();
    QFile f(d4 + QStringLiteral("/json/base/meta/EyeColor/") + stem + QStringLiteral(".eye.json"));
    if (!f.open(QIODevice::ReadOnly)) return QColor();
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    QColor c = readColorObj(o.value(QStringLiteral("rgbaIrisOuter")).toObject());
    if (!c.isValid()) c = readColorObj(o.value(QStringLiteral("rgbaIrisInner")).toObject());
    return c;
}

// Generic EyeColorDefinition scalar reader (all the fl* fields the Hero_Eye shader consumes).
float eyeScalar(const QString& d4, const QString& stem, const char* key, float def)
{
    if (stem.isEmpty()) return def;
    QFile f(d4 + QStringLiteral("/json/base/meta/EyeColor/") + stem + QStringLiteral(".eye.json"));
    if (!f.open(QIODevice::ReadOnly)) return def;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? float(v.toDouble()) : def;
}
// EyeColor flScleraBrightness — the sclera (eye-white) value. 0.6 for normal eyes, but the
// Incandescent/Vampire eyes drop it to 0.05–0.10 → a near-BLACK sclera behind a glowing iris.
float eyeScleraBrightness(const QString& d4, const QString& stem) { return qBound(0.0f, eyeScalar(d4, stem, "flScleraBrightness", 0.6f), 1.5f); }
// EyeColor flScleraRednessAmount — bloodshot blend toward the red-sclera texture (0.1 normal → 0.5).
float eyeScleraRedness(const QString& d4, const QString& stem)   { return qBound(0.0f, eyeScalar(d4, stem, "flScleraRednessAmount", 0.1f), 1.0f); }
// EyeColor flIrisEmissiveStrength — iris self-glow. 1 = normal, 3 = the glowing "Incandescent"/
// Vampire eyes. Drives the emissive channel (amount ≈ strength − 1) masked to the iris.
float eyeIrisEmissive(const QString& d4, const QString& stem)    { return qBound(0.0f, eyeScalar(d4, stem, "flIrisEmissiveStrength", 1.0f), 6.0f); }

// Compose the eye base-colour the way D4's Hero_Eye shader does, from the REAL shared eye
// textures (base_eyes_sclera + base_eyes_sclera_red + base_eyes_irisColor + base_eyes_irisMask)
// rather than one flat tint. The iris mask places the coloured iris precisely over the white
// sclera, so the eye keeps its white (or, per flScleraBrightness, near-black) sclera instead of
// being a solid disc of iris colour. inner/outer = EyeColor iris gradient; scleraBright darkens
// the sclera (0.6 normal, 0.05 vampire); redness blends toward the bloodshot variant.
// Normalise base_eyes_irisMask so the IRIS reads as high (white) and the sclera as low. The
// authored mask's polarity isn't guaranteed, and the eyeball's UV devotes most of its area to the
// sclera, so the iris is always the minority region: if the mask flags the majority as "iris", it's
// inverted → flip it. Returns an R-channel coverage mask (iris = 255).
QImage normalizeIrisMask(const QImage& maskIn)
{
    if (maskIn.isNull()) return QImage();
    const QImage m = maskIn.convertToFormat(QImage::Format_RGBA8888);
    quint64 sum = 0, n = 0;
    for (int y = 0; y < m.height(); y += 2) {
        const uchar* s = m.constScanLine(y);
        for (int x = 0; x < m.width(); x += 2) { sum += s[x*4]; ++n; }
    }
    const bool invert = n && (sum / n) > 128;             // majority is high → that's the sclera → flip
    QImage out(m.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < m.height(); ++y) {
        const uchar* s = m.constScanLine(y);
        uchar* d = out.scanLine(y);
        for (int x = 0; x < m.width(); ++x) {
            const uchar v = invert ? uchar(255 - s[x*4]) : s[x*4];
            d[x*4] = v; d[x*4+1] = v; d[x*4+2] = v; d[x*4+3] = 255;
        }
    }
    return out;
}

// All eye-composite controls (EyeColor def values, plus the debug sizing/scaling overrides).
struct EyeComposeParams {
    QColor inner, outer;
    // NOTE: the fl* defaults below are RENDERER-CALIBRATED (the game's authored constants — 2, 0.86,
    // 0.047, 0.05 — assume D4's tonemap/lighting and over/under-shoot in our viewport, so these are
    // the tuned equivalents). Per-COLOUR fields (scleraBright, iris inner/outer, glow) stay data-driven.
    float irisBright   = 0.176f;
    float scleraBright = 0.6f;
    float scleraRedness= 0.162f;
    float limbusThick  = 0.629f;
    float limbusBright = 0.515f;
    float scleraDesat  = 0.25f;
    float irisShadow   = 0.247f;  // flIrisShadowIntensity — soft socket shadow toward the pupil
    float irisScale    = 0.65f;   // iris radius ×; <1 shrinks, >1 enlarges (tuned to the eyeball UV)
    float pupilSize    = 0.633f;  // pupil radius as a fraction of the iris radius
    float offX = 0.0f, offY = 0.0f;   // iris centre offset (fraction of iris radius)
};

QImage recolorEyeComposite(const QImage& scleraIn, const QImage& scleraRedIn,
                           const QImage& irisIn, const QImage& maskIn, const EyeComposeParams& P)
{
    if (scleraIn.isNull() && irisIn.isNull()) return QImage();
    const QSize sz = !maskIn.isNull() ? maskIn.size()
                   : !scleraIn.isNull() ? scleraIn.size() : irisIn.size();
    auto fit = [&](const QImage& im) {
        return im.isNull() ? QImage()
                           : im.convertToFormat(QImage::Format_RGBA8888)
                                 .scaled(sz, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    };
    const QImage sclera = fit(scleraIn), sred = fit(scleraRedIn), iris = fit(irisIn), mask = fit(maskIn);
    const float ir = float(P.inner.redF()), ig = float(P.inner.greenF()), ib = float(P.inner.blueF());
    const QColor oc = P.outer.isValid() ? P.outer : P.inner;
    const float orr = float(oc.redF()), og = float(oc.greenF()), ob = float(oc.blueF());
    auto smooth = [](float e0, float e1, float x) { float t = qBound(0.0f, (x-e0)/(e1-e0), 1.0f); return t*t*(3.0f-2.0f*t); };
    // Iris geometry from the mask: centroid + area-equivalent radius. The sizing/offset controls then
    // scale + shift a clean radial iris disc about that centre (sampling the iris texture with an
    // inverse-mapped UV so the fibres follow the resize).
    double cxs = 0, cys = 0; qint64 cnt = 0;
    if (!mask.isNull())
        for (int y = 0; y < sz.height(); ++y) { const uchar* m = mask.constScanLine(y);
            for (int x = 0; x < sz.width(); ++x) if (m[x*4] > 128) { cxs += x; cys += y; ++cnt; } }
    const bool haveGeom = cnt > 16 && !iris.isNull();
    const float cx = haveGeom ? float(cxs/double(cnt)) : sz.width()*0.5f;
    const float cy = haveGeom ? float(cys/double(cnt)) : sz.height()*0.5f;
    const float baseR = haveGeom ? float(std::sqrt(double(cnt)/3.14159265)) : 0.0f;
    const float R = qMax(4.0f, baseR * P.irisScale);       // drawn iris radius
    const float ocx = cx + P.offX * baseR, ocy = cy + P.offY * baseR;
    const int iw = iris.isNull() ? 0 : iris.width(), ih = iris.isNull() ? 0 : iris.height();
    const float sMul = qBound(0.0f, P.scleraBright / 0.6f, 1.15f);   // 0.6 = normal white reference
    QImage out(sz, QImage::Format_RGBA8888);
    for (int y = 0; y < sz.height(); ++y) {
        uchar* d = out.scanLine(y);
        const uchar* ps = sclera.isNull() ? nullptr : sclera.constScanLine(y);
        const uchar* pr = sred.isNull()   ? nullptr : sred.constScanLine(y);
        for (int x = 0; x < sz.width(); ++x) {
            // Sclera (eye-white): neutralise the warm texture, then scale by the sclera brightness.
            float br = ps ? ps[x*4]/255.0f   : 0.95f;
            float bg = ps ? ps[x*4+1]/255.0f : 0.93f;
            float bb0= ps ? ps[x*4+2]/255.0f : 0.92f;
            const float sl = 0.299f*br + 0.587f*bg + 0.114f*bb0;
            br += (sl-br)*P.scleraDesat; bg += (sl-bg)*P.scleraDesat; bb0 += (sl-bb0)*P.scleraDesat;
            float sr = br*sMul, sg = bg*sMul, sb = bb0*sMul;
            if (pr) {                                    // bloodshot: only where the red map shows a VEIN
                const float rv = pr[x*4]/255.0f, gv = pr[x*4+1]/255.0f, bv = pr[x*4+2]/255.0f;
                const float vein = qBound(0.0f, (rv - 0.5f*(gv+bv)) * 2.5f, 1.0f);
                const float a = P.scleraRedness * vein;
                sr += (rv*sMul - sr) * a;
                sg += (gv*sMul*0.6f - sg) * a;
                sb += (bv*sMul*0.6f - sb) * a;
            }
            float rr = sr, gg = sg, bb = sb;
            if (haveGeom) {
                const float dx = x - ocx, dy = y - ocy;
                const float r = std::sqrt(dx*dx + dy*dy) / R;     // 0 = centre, 1 = iris edge
                if (r < 1.03f) {
                    // Sample the iris texture at the inverse-mapped position → fibres follow scale/offset.
                    const int sx = qBound(0, int(cx + dx / P.irisScale), iw-1);
                    const int sy = qBound(0, int(cy + dy / P.irisScale), ih-1);
                    const uchar* srow = iris.constScanLine(sy);
                    const float lum = (0.299f*srow[sx*4] + 0.587f*srow[sx*4+1] + 0.114f*srow[sx*4+2]) / 255.0f;
                    const float grad  = smooth(P.pupilSize, 1.0f, r);                 // pupil→limbus colour blend
                    // Keep the authored iris colour SATURATED: the texture only provides fibre
                    // structure (detail) — it must not wash the hue out. Brightness = the fibre detail
                    // × flIrisBrightness lift; a subtle flIrisShadowIntensity darkens toward the pupil.
                    const float detail = 0.55f + 0.65f*lum;                           // fibre structure
                    const float lift   = 0.70f + 0.30f*P.irisBright;                  // flIrisBrightness
                    const float pupil  = smooth(P.pupilSize*0.82f, P.pupilSize, r);   // black pupil disc
                    const float lring  = smooth(P.limbusThick, 1.0f, r);
                    const float limbal = 1.0f - (1.0f - P.limbusBright) * lring;      // dark limbal ring
                    const float shadow = 1.0f - P.irisShadow * (1.0f - grad);         // soft inner shadow
                    const float cov    = 1.0f - smooth(0.97f, 1.03f, r);             // soft outer edge
                    const float k = detail * lift * pupil * limbal * shadow;
                    const float nr = (ir + (orr-ir)*grad) * k;
                    const float ng = (ig + (og-ig)*grad) * k;
                    const float nb = (ib + (ob-ib)*grad) * k;
                    rr += (nr-rr)*cov; gg += (ng-gg)*cov; bb += (nb-bb)*cov;
                }
            }
            d[x*4]   = uchar(qBound(0.0f, rr*255.0f, 255.0f));
            d[x*4+1] = uchar(qBound(0.0f, gg*255.0f, 255.0f));
            d[x*4+2] = uchar(qBound(0.0f, bb*255.0f, 255.0f));
            d[x*4+3] = 255;
        }
    }
    return out;
}

// Build the eye's ORM (roughness) from the two authored EyeColor roughness scalars: glossy iris
// (flIrisRoughness) inside the iris mask, matte sclera (flScleraRoughness) outside.
QImage buildEyeOrm(const QImage& maskIn, float irisRough, float scleraRough)
{
    if (maskIn.isNull()) return QImage();
    const QImage mask = maskIn.convertToFormat(QImage::Format_RGBA8888);
    QImage orm(mask.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < mask.height(); ++y) {
        const uchar* m = mask.constScanLine(y);
        uchar* o = orm.scanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            const float t = m[x*4]/255.0f;                       // 1 = iris, 0 = sclera
            const float rough = scleraRough + (irisRough - scleraRough) * t;
            o[x*4] = 255; o[x*4+1] = uchar(qBound(0.0f, rough*255.0f, 255.0f)); o[x*4+2] = 0; o[x*4+3] = 255;
        }
    }
    return orm;
}

// Iris-only emissive mask for the glowing "Incandescent"/Vampire eyes: the iris mask tinted by
// the iris colour, so the emissive channel lights only the iris (never the sclera).
QImage buildEyeEmissive(const QImage& maskIn, const QColor& irisCol)
{
    if (maskIn.isNull() || !irisCol.isValid()) return QImage();
    const QImage mask = maskIn.convertToFormat(QImage::Format_RGBA8888);
    QImage emis(mask.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < mask.height(); ++y) {
        const uchar* m = mask.constScanLine(y);
        uchar* e = emis.scanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            const float t = m[x*4]/255.0f;
            e[x*4]   = uchar(qBound(0.0, irisCol.redF()  *t*255.0, 255.0));
            e[x*4+1] = uchar(qBound(0.0, irisCol.greenF()*t*255.0, 255.0));
            e[x*4+2] = uchar(qBound(0.0, irisCol.blueF() *t*255.0, 255.0));
            e[x*4+3] = 255;
        }
    }
    return emis;
}

// Makeup overlay: the .mak.json's snoMakeup texture name + intensity.
QString makeupTexName(const QString& d4, const QString& stem, float& intensity)
{
    intensity = 1.0f;
    if (stem.isEmpty()) return QString();
    QFile f(d4 + QStringLiteral("/json/base/meta/Makeup/") + stem + QStringLiteral(".mak.json"));
    if (!f.open(QIODevice::ReadOnly)) return QString();
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    intensity = float(o.value(QStringLiteral("flIntensity")).toDouble(1.0));
    return o.value(QStringLiteral("snoMakeup")).toObject().value(QStringLiteral("name")).toString().section('/', -1);
}

//  applyMarkingMaterial, markingSelfTest — moved to tabs/MarkingCompose.{h,cpp})

// Alpha-composite an RGBA overlay (e.g. makeup) over a base-colour image.
QImage overlayRGBA(QImage base, const QImage& overlay0, float intensity)
{
    if (overlay0.isNull() || base.isNull() || intensity <= 0.0f) return base;
    base = base.convertToFormat(QImage::Format_RGBA8888);
    const QImage ov = overlay0.convertToFormat(QImage::Format_RGBA8888)
                          .scaled(base.width(), base.height(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for (int y = 0; y < base.height(); ++y) {
        uchar* d = base.scanLine(y); const uchar* s = ov.scanLine(y);
        for (int x = 0; x < base.width(); ++x) {
            const float a = (s[x*4+3] / 255.0f) * intensity;
            for (int k = 0; k < 3; ++k)
                d[x*4+k] = uchar(qBound(0, int(d[x*4+k]*(1-a) + s[x*4+k]*a), 255));
        }
    }
    return base;
}

// Bake the D4 dye into a base-colour image — a CPU port of the shader's dyeZoneColor. For each texel:
// the DyeMask R selects a zone (nearest of 4 hardcoded bands), the DyeRamp R gives a value multiplier
// (sh = 0.35..1.40), and the zone's dye colour × sh REPLACES the base (matching the shader — the
// dyed hue comes from the dye, the value/detail from the ramp). Undyed texels (mask ≤ 0.02) keep the
// base. `colors12` = 4 RGB triples in 0..1. Used only for the .glb export so exported armour is dyed.
QImage bakeDye(QImage base, const QImage& maskIn, const QImage& rampIn, const float* colors12)
{
    if (base.isNull() || maskIn.isNull() || !colors12) return base;
    base = base.convertToFormat(QImage::Format_RGBA8888);
    const int W = base.width(), H = base.height();
    QImage mask = maskIn.convertToFormat(QImage::Format_RGBA8888);
    if (mask.size() != base.size()) mask = mask.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const bool hasRamp = !rampIn.isNull();
    QImage ramp;
    if (hasRamp) { ramp = rampIn.convertToFormat(QImage::Format_RGBA8888);
                   if (ramp.size() != base.size()) ramp = ramp.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation); }
    static const float bands[4] = { 0.063f, 0.345f, 0.596f, 0.831f };
    for (int y = 0; y < H; ++y) {
        uchar* d = base.scanLine(y);
        const uchar* m = mask.constScanLine(y);
        const uchar* r = hasRamp ? ramp.constScanLine(y) : nullptr;
        for (int x = 0; x < W; ++x) {
            const float mv = m[x*4] / 255.0f;
            if (mv <= 0.02f) continue;                       // undyed → keep base
            int zone = 0; float best = 2.0f;
            for (int k = 0; k < 4; ++k) { const float e = qAbs(mv - bands[k]); if (e < best) { best = e; zone = k; } }
            const float sh = 0.35f + 1.05f * (r ? r[x*4] / 255.0f : 0.5f);   // mix(0.35,1.40,ramp)
            for (int c = 0; c < 3; ++c)
                d[x*4+c] = uchar(qBound(0.0f, colors12[zone*3 + c] * sh * 255.0f, 255.0f));
        }
    }
    return base;
}

// Bake the tiled, zone-routed detail maps into a part's exported normal + ORM(roughness) — a CPU port
// of the shader's detail block. Per texel: classify the DyeMask into a zone → detail layer, honour the
// metal routing (metal texels use the metal layer or fade non-metal grain out), sample the TILED detail
// normal/rough, combine the detail normal into the base normal's xy, and add the detail roughness. So
// exported armour carries the leather/fabric/brushed-metal surface grain instead of a smooth base map.
void bakeDetail(QImage& normal, QImage& orm, const QImage& dyeMask,
                const QImage detN[3], const QImage detR[3], const QVector3D& scale,
                const QVector4D& zoneMap, const QVector4D& bands, int metalLayer,
                float nInt, float rInt, float rOff)
{
    if (normal.isNull()) return;
    normal = normal.convertToFormat(QImage::Format_RGBA8888);
    const int W = normal.width(), H = normal.height();
    const bool hasOrm = !orm.isNull();
    if (hasOrm) {   // ORM is indexed with the NORMAL's x/y below, so it MUST match the normal's size
        orm = orm.convertToFormat(QImage::Format_RGBA8888);
        if (orm.size() != normal.size()) orm = orm.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    QImage mask = dyeMask.isNull() ? QImage() : dyeMask.convertToFormat(QImage::Format_RGBA8888);
    if (!mask.isNull() && mask.size() != normal.size())
        mask = mask.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage dN[3], dR[3];
    for (int k = 0; k < 3; ++k) {
        if (!detN[k].isNull()) dN[k] = detN[k].convertToFormat(QImage::Format_RGBA8888);
        if (!detR[k].isNull()) dR[k] = detR[k].convertToFormat(QImage::Format_RGBA8888);
    }
    const float bnd[4] = { float(bands.x()), float(bands.y()), float(bands.z()), float(bands.w()) };
    const int   zmap[4] = { int(zoneMap.x()), int(zoneMap.y()), int(zoneMap.z()), int(zoneMap.w()) };
    const float sc[3] = { float(scale.x()), float(scale.y()), float(scale.z()) };
    auto wrap = [](int a, int n) { if (n <= 0) return 0; a %= n; return a < 0 ? a + n : a; };
    for (int y = 0; y < H; ++y) {
        uchar* np = normal.scanLine(y);
        uchar* op = hasOrm ? orm.scanLine(y) : nullptr;
        const uchar* mp = mask.isNull() ? nullptr : mask.constScanLine(y);
        const float vv = (y + 0.5f) / H;
        for (int x = 0; x < W; ++x) {
            const float uu = (x + 0.5f) / W;
            int layer = zmap[1];
            if (mp) { const float mv = mp[x*4] / 255.0f; int zone = 0; float best = 2.0f;
                      for (int k = 0; k < 4; ++k) { const float e = qAbs(mv - bnd[k]); if (e < best) { best = e; zone = k; } }
                      layer = zmap[zone]; if (mv <= 0.02f) layer = -1; }
            const float metalv = op ? op[x*4+2] / 255.0f : 0.0f;
            float metalMask = 1.0f;
            if (metalv > 0.5f) { if (metalLayer >= 0) layer = metalLayer; else metalMask = 0.0f; }
            else if (metalLayer >= 0 && layer == metalLayer) layer = -1;
            if (metalLayer < 0 || layer != metalLayer) metalMask *= 1.0f - qBound(0.0f, (metalv - 0.30f) / 0.30f, 1.0f);
            if (layer < 0 || layer > 2 || metalMask <= 0.001f) continue;
            const float s = sc[layer];
            if (!dN[layer].isNull()) {
                const int dw = dN[layer].width(), dh = dN[layer].height();
                const uchar* dp = dN[layer].constScanLine(wrap(int(vv*s*dh), dh)) + wrap(int(uu*s*dw), dw) * 4;
                const float dnx = (dp[0]/255.0f)*2.0f-1.0f, dny = (dp[1]/255.0f)*2.0f-1.0f;
                const float amt = qBound(0.0f, nInt, 1.0f) * metalMask;
                const float nx = (np[x*4]/255.0f)*2.0f-1.0f + dnx*amt, ny = (np[x*4+1]/255.0f)*2.0f-1.0f + dny*amt;
                np[x*4]   = uchar(qBound(0.0f, (nx*0.5f+0.5f)*255.0f, 255.0f));
                np[x*4+1] = uchar(qBound(0.0f, (ny*0.5f+0.5f)*255.0f, 255.0f));
            }
            if (hasOrm && !dR[layer].isNull()) {
                const int dw = dR[layer].width(), dh = dR[layer].height();
                const float drg = dR[layer].constScanLine(wrap(int(vv*s*dh), dh))[wrap(int(uu*s*dw), dw)*4 + 1] / 255.0f;
                float dr = ((drg - 0.5f) * qBound(0.0f, rInt, 4.0f) + rOff) * metalMask;
                if (dr > 0.0f) dr *= 1.0f - 0.85f * qBound(0.0f, (metalv - 0.35f) / 0.35f, 1.0f);
                op[x*4+1] = uchar(qBound(0.04f, op[x*4+1]/255.0f + dr, 1.0f) * 255.0f);
            }
        }
    }
}

// (marking helpers — MarkingDef/MarkingPaint, markingDef/Ramp/Paint, rampLerp, applyMarking/
//  applyMarkingMaterial, markingSelfTest — moved to tabs/MarkingCompose.{h,cpp})

// A real D4 dye/pigment: name + its 4 colours (rgbaWardrobeColorSwatch → DyeMask zones).
struct DyeDef { QString name; QColor colors[4]; };

QVector<DyeDef> loadPlayerDyes(const QString& d4)
{
    QVector<DyeDef> out;
    if (d4.isEmpty()) return out;
    QDir dir(d4 + QStringLiteral("/json/base/meta/Dye"));
    const QStringList files = dir.entryList(QStringList{QStringLiteral("*.dye.json")}, QDir::Files);
    for (const QString& fn : files) {
        const QString stem = fn.left(fn.size() - 9);   // strip ".dye.json"
        if (stem.startsWith(QLatin1String("NPC_")) || stem == QLatin1String("Debug")
            || stem.contains(QLatin1String("Bad Data")))
            continue;
        QFile f(dir.filePath(fn));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        if (o.value(QStringLiteral("fHiddenFromUI")).toBool()) continue;
        const QJsonArray sw = o.value(QStringLiteral("rgbaWardrobeColorSwatch")).toArray();
        if (sw.isEmpty()) continue;
        DyeDef d; d.name = stem;
        for (int i = 0; i < 4; ++i) {
            const QJsonObject c = sw[qMin(i, sw.size() - 1)].toObject();
            d.colors[i] = QColor(c.value(QStringLiteral("r")).toInt(), c.value(QStringLiteral("g")).toInt(),
                                 c.value(QStringLiteral("b")).toInt());
        }
        out.append(d);
    }
    std::sort(out.begin(), out.end(), [](const DyeDef& a, const DyeDef& b) { return a.name < b.name; });
    return out;
}

// User-saved custom pigments (QSettings "wardrobe2/customPigments": "name\th0\th1\th2\th3").
QVector<DyeDef> loadCustomPigments()
{
    QVector<DyeDef> out;
    const QStringList saved = QSettings().value(QStringLiteral("wardrobe2/customPigments")).toStringList();
    for (const QString& e : saved) {
        const QStringList p = e.split(QLatin1Char('\t'));
        if (p.size() < 5) continue;
        DyeDef d; d.name = p[0];
        for (int k = 0; k < 4; ++k) d.colors[k] = QColor(p[k + 1]);
        out.append(d);
    }
    return out;
}

// ── Minimal column-major 4×4 math for resolving a bone's rest-pose world position ──
using Mat4 = std::array<float, 16>;
Mat4 composeTRS(const std::array<float,3>& t, const std::array<float,4>& q, const std::array<float,3>& s)
{
    const float x=q[0],y=q[1],z=q[2],w=q[3];
    const float r00=1-2*(y*y+z*z), r01=2*(x*y-z*w), r02=2*(x*z+y*w);
    const float r10=2*(x*y+z*w),   r11=1-2*(x*x+z*z), r12=2*(y*z-x*w);
    const float r20=2*(x*z-y*w),   r21=2*(y*z+x*w),   r22=1-2*(x*x+y*y);
    return {{ r00*s[0], r10*s[0], r20*s[0], 0,
              r01*s[1], r11*s[1], r21*s[1], 0,
              r02*s[2], r12*s[2], r22*s[2], 0,
              t[0],     t[1],     t[2],     1 }};
}
Mat4 mat4mul(const Mat4& a, const Mat4& b)   // column-major a*b
{
    Mat4 o{};
    for (int c=0;c<4;++c) for (int r=0;r<4;++r) {
        float v=0; for (int k=0;k<4;++k) v += a[k*4+r]*b[c*4+k];
        o[c*4+r]=v;
    }
    return o;
}
// Full rest-pose world matrix of a joint, in D4-native (z-up) space — composed from
// the native rest TRS up the parent chain. (The mesh-space y-up swap is applied later
// when seating the weapon, so the whole attach chain stays in one consistent space.)
Mat4 jointWorldMat(const QVector<ModelJoint>& skel, int idx)
{
    QVector<int> chain;
    for (int i = idx; i >= 0 && i < skel.size(); i = skel[i].parent) {
        chain.push_front(i);
        if (skel[i].parent == i) break;
    }
    Mat4 world{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    for (int i : chain)
        world = mat4mul(world, composeTRS(skel[i].restT, skel[i].restQ, skel[i].restS));
    return world;
}

// quaternion (x,y,z,w) + position → column-major matrix (no axis swap; native space).
Mat4 quatPosMat(const std::array<float,4>& q, const std::array<float,3>& p)
{
    return composeTRS(p, q, {{1.0f, 1.0f, 1.0f}});
}

// z-up → y-up basis change S (and its inverse), as 4×4 column-major matrices, so a
// native attach matrix M can be re-expressed in mesh space as S · M · S⁻¹.
const Mat4 kSwapZtoY{{1,0,0,0, 0,0,-1,0, 0,1,0,0, 0,0,0,1}};
const Mat4 kSwapYtoZ{{1,0,0,0, 0,0,1,0, 0,-1,0,0, 0,0,0,1}};

QIcon pigmentIcon(const QColor c[4])
{
    QPixmap pm(16, 16); pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.fillRect(0, 0, 8, 8, c[0]); p.fillRect(8, 0, 8, 8, c[1]);
    p.fillRect(0, 8, 8, 8, c[2]); p.fillRect(8, 8, 8, 8, c[3]);
    p.end();
    return QIcon(pm);
}

// Size of a pigment swatch card in the library grid (small, dense, palette-style).
constexpr int kPigSwatch = 46;

// A 4-quadrant pigment swatch that FILLS a card of the given size (no tiny floating icon).
QPixmap pigmentSwatchPixmap(const QColor c[4], int size)
{
    QPixmap pm(size, size); pm.fill(Qt::transparent);
    QPainter p(&pm);
    const int h = size / 2;
    p.fillRect(0, 0, h, h, c[0]); p.fillRect(h, 0, size - h, h, c[1]);
    p.fillRect(0, h, h, size - h, c[2]); p.fillRect(h, h, size - h, size - h, c[3]);
    p.end();
    return pm;
}

// The "no pigment" swatch: a dark tile with a thin diagonal slash.
QPixmap nonePigmentPixmap(int size)
{
    QPixmap pm(size, size); pm.fill(QColor(0x24, 0x20, 0x1c));
    QPainter p(&pm);
    p.setPen(QPen(QColor(0x9a, 0x6a, 0x5a), 2));
    p.drawLine(5, 5, size - 5, size - 5);
    p.end();
    return pm;
}
}  // namespace

WardrobeTab2::WardrobeTab2(QWidget* parent) : BrowserTab(parent)
{
    // Decoder sanity checks (run once): catch silent regressions in the BC7 tables and the
    // marking R/G channel model. Failures are logged to the console, not fatal.
    static bool s_selfTested = false;
    if (!s_selfTested) {
        s_selfTested = true;
        const QString bt = BcDecode::selfTest();
        if (!bt.isEmpty()) qWarning().noquote() << "[self-test] BcDecode FAILED:" << bt;
        const QString mt = markingSelfTest();
        if (!mt.isEmpty()) qWarning().noquote() << "[self-test] marking FAILED:" << mt;
        if (bt.isEmpty() && mt.isEmpty()) qInfo().noquote() << "[self-test] decoders OK (BC7 + marking)";
    }
    // Hard cap the temporary decode pool at 256 MB (cost measured in KiB below). LRU eviction
    // keeps it bounded — it can never grow without limit, so it won't bloat.
    m_texCache.setMaxCost(256 * 1024);
    // Remembered "Camera Snap" preferences (read before the Camera panel is built, since
    // selectSlot() may snap the camera before the user ever opens it).
    {
        QSettings s;
        m_d4View     = s.value(QStringLiteral("wardrobe2/cameraSnap"), false).toBool();
        m_camFollow  = s.value(QStringLiteral("wardrobe2/cameraFollow"), true).toBool();
        m_snapMargin = float(s.value(QStringLiteral("wardrobe2/snapMargin"), 0.06).toDouble());
        m_hoverSnap  = s.value(QStringLiteral("wardrobe2/hoverSnap"), false).toBool();
    }

    auto* root = new QVBoxLayout(this);   // vertical so the first-run hint can sit above the columns
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    if (QWidget* hint = makeHintBar(this,
            QStringLiteral("Tip: double-click the character to select a part · F fullscreen · "
                           "Ctrl+Z undoes outfit changes · F1 lists every shortcut"),
            "hints/wardrobe"))
        root->addWidget(hint);
    auto* split = new QSplitter(Qt::Horizontal, this);
    m_mainSplit = split;   // fullscreen (maximize-in-place) hides panes 0 and 2 of this splitter
    root->addWidget(split, 1);

    // ── Left: class/gender + equipment slots + view options ──
    auto* left = new QWidget;
    auto* ll = new QVBoxLayout(left);
    ll->setContentsMargins(8, 8, 8, 8);
    ll->setSpacing(5);
    ll->addWidget(new QLabel(QStringLiteral("CHARACTER")));

    auto* cgRow = new QHBoxLayout();
    m_class = new QComboBox;
    for (const ClassDef& c : kClasses) m_class->addItem(QString::fromLatin1(c.name), QString::fromLatin1(c.code));
    m_gender = new QComboBox;
    m_gender->addItem(QStringLiteral("Female"), QStringLiteral("f"));
    m_gender->addItem(QStringLiteral("Male"), QStringLiteral("m"));
    cgRow->addWidget(m_class, 1); cgRow->addWidget(m_gender, 1);
    ll->addLayout(cgRow);

    // ── Equipment ⟷ Appearance mode switch (the two menus you toggle between) ──
    auto* modeRow = new QHBoxLayout();
    auto* btnEquip  = new QPushButton(QStringLiteral("Equipment"));
    auto* btnAppear = new QPushButton(QStringLiteral("Appearance"));
    btnEquip->setCheckable(true); btnAppear->setCheckable(true); btnEquip->setChecked(true);
    auto* modeGrp = new QButtonGroup(this); modeGrp->setExclusive(true);
    modeGrp->addButton(btnEquip, 0); modeGrp->addButton(btnAppear, 1);
    modeRow->addWidget(btnEquip, 1); modeRow->addWidget(btnAppear, 1);
    ll->addLayout(modeRow);
    auto* modeStack = new QStackedWidget;
    ll->addWidget(modeStack, 2);   // stretch: the icon grid fills most of the panel height
    auto* equipPage  = new QWidget; auto* eqLay = new QVBoxLayout(equipPage);  eqLay->setContentsMargins(0,0,0,0); eqLay->setSpacing(5);
    auto* appearPage = new QWidget; auto* apLay = new QVBoxLayout(appearPage); apLay->setContentsMargins(0,0,0,0); apLay->setSpacing(5);
    modeStack->addWidget(equipPage);    // index 0 — Equipment (gear slots)
    modeStack->addWidget(appearPage);   // index 1 — Appearance (character creator)
    connect(modeGrp, &QButtonGroup::idClicked, modeStack, &QStackedWidget::setCurrentIndex);

    // ── Animation transport — Models-tab style, CENTERED UNDER THE VIEWPORT (added to the centre
    //    column below). Step / play-pause / step drawn icons + scrub + frame field + time/speed/loop.
    m_timeline = new QWidget;
    auto* tlay = new QHBoxLayout(m_timeline);
    tlay->setContentsMargins(0, 0, 0, 0);
    tlay->setSpacing(3);
    auto mkTransport = [&](int glyph, const QString& tip) {
        auto* b = new QToolButton(m_timeline);
        b->setIcon(transportGlyph(glyph));
        b->setAutoRaise(true);
        b->setToolTip(tip);
        tlay->addWidget(b);
        return b;
    };
    auto* stepB = mkTransport(2, QStringLiteral("Step back one frame"));
    m_playBtn = new QPushButton(m_timeline);
    m_playBtn->setIcon(transportGlyph(0));
    m_playBtn->setMaximumWidth(34);
    m_playBtn->setToolTip(QStringLiteral("Play / pause (Play at the end restarts)"));
    tlay->addWidget(m_playBtn);
    auto* stepF = mkTransport(3, QStringLiteral("Step forward one frame"));
    connect(stepB, &QToolButton::clicked, this, [this]() { m_animSlider->setValue(m_animSlider->value() - 1); });
    connect(stepF, &QToolButton::clicked, this, [this]() { m_animSlider->setValue(m_animSlider->value() + 1); });
    m_animSlider = new QSlider(Qt::Horizontal, m_timeline);
    m_animSlider->setTickPosition(QSlider::TicksBelow);   // frame ticks (interval set per clip)
    m_animSlider->setSingleStep(1);
    m_animSlider->setToolTip(QStringLiteral("Scrub the clip"));
    m_frameSpin = new QSpinBox(m_timeline);
    m_frameSpin->setToolTip(QStringLiteral("Current frame — type to jump"));
    m_frameSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_frameSpin->setFixedWidth(52);
    m_frameSpin->setAlignment(Qt::AlignRight);
    connect(m_frameSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
        if (m_animSlider->value() != v) m_animSlider->setValue(v);
    });
    m_frameMax = new QLabel(QStringLiteral("/ 0"), m_timeline);
    m_timeLabel = new QLabel(QStringLiteral("0.00 / 0.00s"), m_timeline);
    m_speedCombo = new QComboBox(m_timeline);
    m_speedCombo->addItems({QStringLiteral("0.25x"), QStringLiteral("0.5x"), QStringLiteral("1x"),
                            QStringLiteral("1.5x"), QStringLiteral("2x")});
    m_speedCombo->setCurrentIndex(2);
    m_loopCheck = new QCheckBox(QStringLiteral("Loop"), m_timeline);
    m_loopCheck->setChecked(true);
    tlay->addWidget(m_animSlider, 1);
    tlay->addWidget(m_frameSpin);
    tlay->addWidget(m_frameMax);
    tlay->addWidget(m_timeLabel);
    tlay->addWidget(m_speedCombo);
    tlay->addWidget(m_loopCheck);
    m_timeline->setVisible(false);

    // ── Animations clip list — registered as a RIGHT-side panel below (like the Models tab). The
    //    PanelBox header supplies the "ANIMATIONS" title, so no label here.
    m_animPanel = new QWidget;
    auto* bl = new QVBoxLayout(m_animPanel);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(3);
    // Category filter + sort order.
    auto* afRow = new QHBoxLayout(); afRow->setSpacing(4);
    m_animFilter = new QComboBox(m_animPanel);
    m_animFilter->setToolTip(QStringLiteral("Filter clips by category"));
    static const struct { const char* label; const char* token; } kAnimCats[] = {
        {"All categories", ""}, {"Idle", "idle"}, {"Walk", "walk"}, {"Run", "run"},
        {"Attack", "attk"}, {"Navigation", "nav"}, {"Turn", "turn"}, {"Sheathe", "sheath"},
        {"Reaction", "reac"}, {"Emote", "emote,emotes"}, {"UI", "_ui_"}, {"Wardrobe", "wardrobe"},
        {"Mount", "mount,horse"}, {"Two-Hand", "2h"}, {"Dual Wield", "dw_"}, {"One-Hand", "1h"},
    };
    for (const auto& c : kAnimCats)
        m_animFilter->addItem(QString::fromLatin1(c.label), QString::fromLatin1(c.token));
    m_animSort = new QComboBox(m_animPanel);
    m_animSort->setToolTip(QStringLiteral("Sort order"));
    m_animSort->addItems({QStringLiteral("Name A–Z"), QStringLiteral("Name Z–A"),
                          QStringLiteral("Frames ↑"), QStringLiteral("Frames ↓")});
    afRow->addWidget(m_animFilter, 1);
    afRow->addWidget(m_animSort, 1);
    bl->addLayout(afRow);
    m_animSearch = new QLineEdit(m_animPanel);
    m_animSearch->setPlaceholderText(QStringLiteral("Search animations…"));
    m_animSearch->setClearButtonEnabled(true);
    bl->addWidget(m_animSearch);
    m_anims = new QListWidget(m_animPanel);
    m_anims->setMinimumHeight(120);
    // Multi-select: ctrl/shift-click to pick exactly the clips to embed / put in a library.
    m_anims->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // Right-click → export the selected clips as a rig-only animation library .glb. Setting an
    // explicit menu here makes the generic "Copy" auto-menu loop skip m_anims (it keeps non-Default
    // policies). If the right-clicked clip isn't selected, export just it.
    m_anims->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_anims, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        QListWidgetItem* hit = m_anims->itemAt(p);
        auto pickHit = [this, hit]() { if (hit && !hit->isSelected()) { m_anims->clearSelection(); hit->setSelected(true); } };
        const int nSel = (hit && hit->isSelected()) ? qMax(1, int(m_anims->selectedItems().size())) : 1;
        QMenu menu(this);
        menu.addAction(nSel > 1 ? QStringLiteral("Export animation library — %1 clip(s) (.glb)…").arg(nSel)
                                : QStringLiteral("Export animation library (.glb)…"),
                       this, [this, pickHit]() { pickHit(); exportAnimLibrary(false); });
        menu.addAction(QStringLiteral("Export animation library to last dir"),
                       this, [this, pickHit]() { pickHit(); exportAnimLibrary(true); });
        if (hit) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("Copy name"), this,
                           [hit] { QGuiApplication::clipboard()->setText(hit->text()); });
        }
        menu.exec(m_anims->viewport()->mapToGlobal(p));
    });
    bl->addWidget(m_anims, 1);
    connect(m_animFilter, &QComboBox::currentIndexChanged, this, [this](int) { fillAnimList(); });
    connect(m_animSort,   &QComboBox::currentIndexChanged, this, [this](int) { fillAnimList(); });
    buildEnsemblePanel();                     // Ensembles: under the item browser
    if (m_ensemblePanel) ll->addWidget(m_ensemblePanel);   // compact (list is height-capped) — no
                                                           // stretch, so the item grid fills instead
    // (m_animPanel is NOT added here — it's registered as a RIGHT-side panel; the transport bar
    //  m_timeline is placed under the viewport in the centre column. Both happen below.)

    // ── Equipment page: slot cells (icons) + per-slot appearance icon grid ──
    // The five m_slot combos stay HIDDEN as backing state; the grid drives them so all the
    // existing populate/restore/rebuild logic keeps working unchanged (no rebuild-path edits).
    for (int i = 0; i < 5; ++i) { m_slot[i] = new QComboBox(equipPage); m_slot[i]->hide(); }
    {
        // Mirror the game: a row of the 5 armour slots, then a separate weapons row below.
        auto* slotGrp = new QButtonGroup(this); slotGrp->setExclusive(true);
        auto makeCell = [&](int i) {
            auto* cell = new QToolButton;
            cell->setText(slotLabel(i));
            cell->setCheckable(true);
            cell->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            cell->setIconSize(QSize(44, 44));
            cell->setFixedSize(60, 74);
            slotGrp->addButton(cell, i);
            m_slotCells[i] = cell;
            cell->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(cell, &QWidget::customContextMenuRequested, this, [this, i, cell](const QPoint& p) {
                QMenu menu;
                if (i < 5) {   // armour only — weapons can't be dyed
                    menu.addAction(QStringLiteral("Copy pigment"), this,
                                   [this, i] { m_dyeClip = m_slotDye[i]; m_dyeClipSet = true; });
                    QAction* paste = menu.addAction(QStringLiteral("Paste pigment"), this,
                                   [this, i] { applyPresetDye(m_dyeClip.name, m_dyeClip.hex, false, i); });
                    paste->setEnabled(m_dyeClipSet);
                    menu.addAction(QStringLiteral("Clear pigment"), this,
                                   [this, i] { applyPresetDye(QString(), QStringList(), false, i); });
                    menu.addSeparator();
                }
                menu.addAction(QStringLiteral("Clear"), this, [this, i] {   // unequip this slot
                    if (QComboBox* c = slotCombo(i)) c->setCurrentIndex(0);   // (none) → rebuilds
                    refreshSlotCells();
                    if (i == m_activeSlot) { refreshLookSelection(); updateLookHeader(); }
                });
                menu.exec(cell->mapToGlobal(p));
            });
            return cell;
        };
        // One condensed row: the 5 armour slots, a gap, the 4 weapon slots (held + sheathed),
        // a gap, then the back-trophy slot.
        auto* slotRow = new QHBoxLayout();
        slotRow->setSpacing(3);
        for (int i = 0; i < 5; ++i) slotRow->addWidget(makeCell(i));   // HLM..BTS
        slotRow->addSpacing(10);
        for (int i = 5; i <= 8; ++i) slotRow->addWidget(makeCell(i));  // Main, Off, Sheath, Sheath 2
        slotRow->addSpacing(10);
        slotRow->addWidget(makeCell(9));                               // Back trophy
        slotRow->addStretch(1);
        eqLay->addLayout(slotRow);

        m_slotCells[0]->setChecked(true);
        connect(slotGrp, &QButtonGroup::idClicked, this, [this](int i) { selectSlot(i); });
        for (auto* c : m_slotCells) if (c) c->installEventFilter(this);   // hover → Snap-to-slot-on-hover
    }
    {   // Set Look / Set Pigment toggle — acts on the selected slot, like the game.
        auto* lpRow = new QHBoxLayout(); lpRow->setSpacing(4);
        m_lookModeBtn    = new QPushButton(QStringLiteral("Set Look"));
        m_pigmentModeBtn = new QPushButton(QStringLiteral("Set Pigment"));
        m_lookModeBtn->setCheckable(true); m_pigmentModeBtn->setCheckable(true);
        m_lookModeBtn->setChecked(true);
        auto* lpGrp = new QButtonGroup(this); lpGrp->setExclusive(true);
        lpGrp->addButton(m_lookModeBtn, 0); lpGrp->addButton(m_pigmentModeBtn, 1);
        lpRow->addWidget(m_lookModeBtn, 1); lpRow->addWidget(m_pigmentModeBtn, 1);
        eqLay->addLayout(lpRow);
        connect(lpGrp, &QButtonGroup::idClicked, this, [this](int id) { setPigmentMode(id == 1); });
    }
    m_lookHeader = new QLabel(QStringLiteral("Helmets"));
    m_lookHeader->setStyleSheet(QStringLiteral("color:#bbb; font-weight:bold;"));
    m_lookHeader->setWordWrap(true);
    eqLay->addWidget(m_lookHeader);
    buildPigmentPanel();                 // custom pigment picker (hidden until Set Pigment)
    eqLay->addWidget(m_pigmentPanel);
    m_pigmentPanel->hide();
    {   // Search box + collection filter for the look grid.
        auto* fr = new QHBoxLayout();
        m_lookSearch = new QLineEdit;
        m_lookSearch->setPlaceholderText(QStringLiteral("Search this slot..."));
        m_lookSearch->setClearButtonEnabled(true);
        m_lookCollFilter = new QComboBox;
        m_lookCollFilter->addItem(QStringLiteral("All collections"), QString());
        m_lookCollFilter->setToolTip(QStringLiteral("Filter this slot by transmog collection"));
        fr->addWidget(m_lookSearch, 2);
        fr->addWidget(m_lookCollFilter, 1);
        // (Weapon settings — class restriction, auto-upright, per-hand flips — live in
        //  File ▸ Settings ▸ Wardrobe ▸ Weapons; they persist live and reseat on toggle.)
        eqLay->addLayout(fr);
        connect(m_lookSearch, &QLineEdit::textChanged, this,
                [this](const QString& s) { m_lookFilter = s.trimmed().toLower(); fillLookGrid(); });
        connect(m_lookCollFilter, &QComboBox::currentIndexChanged, this, [this](int) { fillLookGrid(); });
    }
    m_iconProgress = new QLabel;
    m_iconProgress->setStyleSheet(QStringLiteral("color:#d9a441;"));
    m_iconProgress->hide();
    eqLay->addWidget(m_iconProgress);
    m_lookScroll = new QScrollArea;
    m_lookScroll->setWidgetResizable(true);
    m_lookScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lookScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);   // stable viewport width → no reflow jitter
    m_lookScroll->setMinimumHeight(320);
    m_lookContent = new QWidget;
    m_lookLayout = new QGridLayout(m_lookContent);
    m_lookLayout->setContentsMargins(2, 2, 2, 2);
    m_lookLayout->setSpacing(3);
    m_lookLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_lookScroll->setWidget(m_lookContent);
    m_lookScroll->setFocusPolicy(Qt::StrongFocus);         // arrow-key pigment navigation
    m_lookScroll->installEventFilter(this);                // arrow keys (pigment grid)
    m_lookScroll->viewport()->installEventFilter(this);   // reflow cards on width change
    connect(m_lookScroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
        auto* sb = m_lookScroll ? m_lookScroll->verticalScrollBar() : nullptr;   // lazy-build more on scroll
        if (sb && v >= sb->maximum() - 400 && m_lookBuildPos < m_lookItems.size()) appendLookCards(48);
    });
    eqLay->addWidget(m_lookScroll, 1);
    {   // Weapons (main + off).
        // The type/model combos are kept as hidden backing state — selection now happens
        // entirely in the icon browser (all weapons, grouped by category dividers).
        m_weaponType  = new QComboBox; m_weaponType->hide();
        m_weapon      = new QComboBox; m_weapon->hide();
        m_weaponType2 = new QComboBox; m_weaponType2->hide();
        m_weapon2     = new QComboBox; m_weapon2->hide();
        m_weapon3     = new QComboBox; m_weapon3->hide();   // sheathed main
        m_weapon4     = new QComboBox; m_weapon4->hide();   // sheathed off
        m_backTrophy  = new QComboBox; m_backTrophy->hide();// back trophy (player back cosmetic)
    }
    // (The "equip a set" selector was removed — Set Pigment + per-slot looks cover this.)

    // ── Appearance page: 9 creator-category cells + a per-category card browser ──
    // The m_creator combos stay HIDDEN as backing state; the grid drives them (mirrors Equipment).
    for (int i = 0; i < 9; ++i) { m_creator[i] = new QComboBox(appearPage); m_creator[i]->hide(); }
    {   // Compact category cells (slot-cell style), 5 per row to reclaim space.
        auto* crow = new QGridLayout();
        crow->setSpacing(3);
        auto* cgrp = new QButtonGroup(this); cgrp->setExclusive(true);
        for (int i = 0; i < 9; ++i) {
            auto* cell = new QToolButton;
            cell->setText(QString::fromLatin1(kCreator[i].label));
            cell->setCheckable(true);
            cell->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            cell->setIconSize(QSize(40, 40));
            cell->setFixedSize(66, 70);
            cell->setStyleSheet(QStringLiteral("QToolButton{font-size:9px;}"));
            cgrp->addButton(cell, i);
            m_creatorCells[i] = cell;
            crow->addWidget(cell, i / 5, i % 5);   // 5 columns × 2 rows
        }
        crow->setColumnStretch(5, 1);   // keep the cells left-aligned
        m_creatorCells[0]->setChecked(true);
        apLay->addLayout(crow);
        connect(cgrp, &QButtonGroup::idClicked, this, [this](int i) { selectCreator(i); });
    }
    m_creatorHeader = new QLabel(QStringLiteral("Face"));
    m_creatorHeader->setStyleSheet(QStringLiteral("color:#bbb; font-weight:bold;"));
    m_creatorHeader->setWordWrap(true);
    apLay->addWidget(m_creatorHeader);
    {   // Search + sort (parity with the Equipment browser).
        auto* csRow = new QHBoxLayout(); csRow->setSpacing(4);
        m_creatorSearch = new QLineEdit;
        m_creatorSearch->setPlaceholderText(QStringLiteral("Search this category..."));
        m_creatorSearch->setClearButtonEnabled(true);
        m_creatorSort = new QComboBox;
        m_creatorSort->addItems({QStringLiteral("Name A–Z"), QStringLiteral("Name Z–A")});
        m_creatorSort->setToolTip(QStringLiteral("Sort order"));
        csRow->addWidget(m_creatorSearch, 2);
        csRow->addWidget(m_creatorSort, 1);
        apLay->addLayout(csRow);
        connect(m_creatorSearch, &QLineEdit::textChanged, this,
                [this](const QString& s) { m_creatorFilter = s.trimmed().toLower(); fillCreatorGrid(); });
        connect(m_creatorSort, &QComboBox::currentIndexChanged, this, [this](int) { fillCreatorGrid(); });
    }
    m_creatorScroll = new QScrollArea;
    m_creatorScroll->setWidgetResizable(true);
    m_creatorScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_creatorScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);   // stable viewport width
    m_creatorScroll->setMinimumHeight(260);
    m_creatorContent = new QWidget;
    m_creatorLayout = new QGridLayout(m_creatorContent);
    m_creatorLayout->setContentsMargins(2, 2, 2, 2);
    m_creatorLayout->setSpacing(3);
    m_creatorLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_creatorScroll->setWidget(m_creatorContent);
    m_creatorScroll->setFocusPolicy(Qt::StrongFocus);        // arrow-key navigation
    m_creatorScroll->installEventFilter(this);               // arrow keys
    m_creatorScroll->viewport()->installEventFilter(this);   // reflow cards on width change
    apLay->addWidget(m_creatorScroll, 1);
    {   // Skin tone (parametric arSkinColorChoices; swatch-tint approximation)
        auto* row = new QHBoxLayout();
        auto* lbl = new QLabel(QStringLiteral("Skin tone")); lbl->setFixedWidth(86);
        m_skinTone = new QComboBox;
        m_skinTone->addItem(QStringLiteral("(default)"), QString());
        for (const auto& st : loadSkinTones(Config::d4dataDir())) {
            QPixmap pm(16, 16); pm.fill(st.second);
            m_skinTone->addItem(QIcon(pm), st.first, st.second.name());
        }
        row->addWidget(lbl); row->addWidget(m_skinTone, 1);
        apLay->addLayout(row);
    }
    {   // Skin detail overlay (freckles / vitiligo)
        auto* row = new QHBoxLayout();
        auto* lbl = new QLabel(QStringLiteral("Skin detail")); lbl->setFixedWidth(86);
        m_skinDetail = new QComboBox;
        m_skinDetail->addItem(QStringLiteral("(none)"), QString());
        m_skinDetail->addItem(QStringLiteral("Freckles"), QStringLiteral("Freckle"));
        m_skinDetail->addItem(QStringLiteral("Vitiligo"), QStringLiteral("Vitiligo"));
        row->addWidget(lbl); row->addWidget(m_skinDetail, 1);
        apLay->addLayout(row);
    }

    ll->addSpacing(6);
    // (Dye selector removed — use Set Pigment. Environment lighting moved to Preview Settings.)

    // (Wireframe / Grid / Skeleton / FX / SIM / Spin live in the centre preview
    // toolbar now — Models-tab style — so they're not duplicated here.)
    auto* btnRow = new QHBoxLayout();
    auto* btnSettings = new QPushButton(QStringLiteral("Settings"));
    btnSettings->setToolTip(QStringLiteral("Open Settings (Wardrobe category)"));
    auto* btnReset = new QPushButton(QStringLiteral("Reset to default"));
    m_copyDebugBtn = new QPushButton(QStringLiteral("Copy debug"));
    m_copyDebugBtn->setToolTip(QStringLiteral("Open the full per-piece debug log (also copies it to the clipboard)"));
    btnRow->addWidget(btnSettings); btnRow->addWidget(btnReset);
    btnRow->addWidget(m_copyDebugBtn);
    ll->addLayout(btnRow);
    connect(btnSettings, &QPushButton::clicked, this, [this] {
        SettingsDialog dlg(this);
        // Real-time: apply Wardrobe option changes the instant they're toggled.
        connect(&dlg, &SettingsDialog::wardrobeLiveChanged, this, [this](bool rebuild) {
            applySidebars();
            if (rebuild && m_loaded) rebuildOutfit();
        });
        if (dlg.exec() == QDialog::Accepted) onSettingsChanged();
    });
    // Character + animation-library export now live in the top menu-bar Export menu — it shows
    // "Export selected look…" and "Export animation library (.glb)…" when this tab is active. The
    // "Include animation" option lives in Export settings (Settings ▸ Export).
    // (FOV slider moved to Preview Settings.)
    connect(btnReset, &QPushButton::clicked, this, [this] { resetDefaults(); });
    connect(m_copyDebugBtn, &QPushButton::clicked, this, [this] {
        showDebugConsole();   // proper scrollable log + copies the full debug text to the clipboard
    });
    m_status = new QLabel(QStringLiteral("Pick a class and equipment."));
    m_status->setStyleSheet(QStringLiteral("color:#888;"));
    m_status->setWordWrap(true);
    ll->addWidget(m_status);
    // The debug log + Copy debug are hidden by default (toggle in Settings → Wardrobe 2).
    // (modeStack + the animations panel carry the stretch; the bottom controls stay fixed)

    // ── Centre: one preview toolbar (shading toggles left, view buttons right) + viewport ──
    auto* center = new QWidget;
    auto* cl = new QVBoxLayout(center);
    cl->setContentsMargins(4, 4, 4, 4);
    m_centerLayout = cl;

    // Settings-popover buttons — created here, re-homed onto the viewport's N-strip below
    // (only Fullscreen stays in the toolbar). Same skin + kBarH as the Models tab, plus the
    // red [panelOpen] state the eventFilter drives while a button's panel is showing.
    const QString rsStyle = QLatin1String(kToolBtnQss) + QStringLiteral(
        "QToolButton[panelOpen=\"true\"]{background:#8a1414;color:#fff;border-color:#a01818;}");
    auto mkViewBtn = [&](const QString& text, const QString& tip, std::function<void()> slot) {
        auto* b = new QToolButton(center);
        b->setText(text);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(rsStyle);
        b->setFixedHeight(kBarH);
        connect(b, &QToolButton::clicked, this, [slot]() { slot(); });
        return b;
    };
    // ("Reset view" is gone — MIDDLE-CLICK the viewport to re-frame, same as the Models tab.
    //  The Camera panel also has explicit framing buttons.)
    // (The text "Fullscreen" toolbar button was removed — the ⛶ icon on the viewport N-strip plus
    //  the F key and the floating Exit button cover it; m_fsBtn stays null, its uses are guarded.)
    m_vpBtn = mkViewBtn(QStringLiteral("Graphics"),
                        QStringLiteral("Rendering quality: shadows/AO, detail & surface shading, tonemap, background"),
                        [this] { togglePreviewPanel(); });
    m_camBtn = mkViewBtn(QStringLiteral("Camera"),
                         QStringLiteral("Camera Snap + follow · FOV · view angles · turntable"),
                         [this] { toggleCameraPanel(); });
    m_lightBtn = mkViewBtn(QStringLiteral("Lighting"),
                           QStringLiteral("Character-screen light rig (key / rim / fill) from D4's real values"),
                           [this] { toggleLightingPanel(); });
    m_shaderBtn = mkViewBtn(QStringLiteral("Shaders"),
                            QStringLiteral("Shell-fur + mesh-FX shading settings"),
                            [this] { toggleShaderPanel(); });
    m_detailBtn = mkViewBtn(QStringLiteral("Detail maps"),
                            QStringLiteral("Experiment with the detail-map selection rule "
                                           "(zone→map, metalness routing). Global, for discovery — Copy config to share."),
                            [this] { toggleDetailPanel(); });
    // (Rig button retired — its toggles live in the Overlays ▾ panel now, same as Models.)
    m_physBtn = mkViewBtn(QStringLiteral("Physics"),
                          QStringLiteral("Live cloth-physics tuning (debug)"),
                          [this] { togglePhysicsPanel(); });

    // View toggle toolbar: Shading · Overlays · Submeshes · Camera.
    auto* viewBar = new QHBoxLayout();
    viewBar->setSpacing(3);
    viewBar->setContentsMargins(0, 2, 0, 2);
    auto mkToggle = [&](const QString& key, const QString& text, const QString& tip,
                        bool checked, std::function<void(bool)> slot) {
        auto* b = new QToolButton(center);
        b->setObjectName(key);
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setChecked(checked);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QLatin1String(kToolBtnQss));   // shared skin (BrowserTab.h)
        b->setFixedHeight(kBarH);                       // = the icon buttons beside them
        connect(b, &QToolButton::toggled, this, [this, slot, key](bool on) {
            slot(on);
            QSettings().setValue(QStringLiteral("wardrobe2/view/") + key, on);
        });
        viewBar->addWidget(b);
        m_viewToggleBtns.append(b);
    };
    auto sep = [&] {
        auto* f = new QFrame(center); f->setFrameShape(QFrame::VLine);
        f->setStyleSheet(QStringLiteral("color:#444;")); viewBar->addWidget(f);
    };
    QSettings vs;
    auto vchk = [&](const QString& k, bool def) { return vs.value(QStringLiteral("wardrobe2/view/") + k, def).toBool(); };
    // Channel viewer (replaces the old "Color" toggle): lit result, or one raw material
    // channel. A combo so it scrolls with ↑/↓, applies instantly, and highlights the current.
    m_channelCombo = new QComboBox(center);
    m_channelCombo->addItems({QStringLiteral("Shaded"), QStringLiteral("Base Color"),
                              QStringLiteral("Normal"), QStringLiteral("Roughness"),
                              QStringLiteral("Metallic"), QStringLiteral("AO"),
                              QStringLiteral("Emissive"), QStringLiteral("Detail maps"),
                              QStringLiteral("Dye zones")});
    m_channelCombo->setToolTip(QStringLiteral("View the lit result or one raw material channel (↑/↓ to scroll)"));
    m_channelCombo->setCursor(Qt::PointingHandCursor);
    m_channelCombo->setStyleSheet(QStringLiteral(
        "QComboBox{padding:2px 8px;border:1px solid #555;border-radius:3px;background:#2b2b2b;color:#bbb;}"
        "QComboBox:hover{border-color:#b0453c;}"
        "QComboBox QAbstractItemView{background:#2b2b2b;color:#dddddd;"
        "selection-background-color:#8a1414;selection-color:#ffffff;}"));
    m_channelCombo->setCurrentIndex(vs.value(QStringLiteral("wardrobe2/view/channel"), 0).toInt());
    connect(m_channelCombo, &QComboBox::currentIndexChanged, this, [this](int i) {
        QSettings().setValue(QStringLiteral("wardrobe2/view/channel"), i);
        if (m_view) m_view->setViewChannel(i);
    });
    // (m_channelCombo joins the shading ⌄ popover below as "Channel" — not added to the bar.)
    // ── Skeleton toggle — HIDDEN state-holder, same trick as Models: applyRigToggle mirrors it
    // by objectName; the visible control lives in Overlays ▾ below.
    mkToggle(QStringLiteral("skeleton"), QStringLiteral("Skeleton"), QStringLiteral("Show bones"),
             vchk(QStringLiteral("skeleton"), false),
             [this](bool on) { applyRigToggle(QStringLiteral("wardrobe2/view/skeleton"), on); });
    m_skelToggleBtn = center->findChild<QToolButton*>(QStringLiteral("skeleton"));
    if (m_skelToggleBtn) m_skelToggleBtn->hide();

    // ── Shading mode: Blender's four spheres — wire · flat · shaded · rendered (ViewGlyphs.h).
    // "Rendered" turns the post pipeline ON (IBL/shadows/SSAO/tonemap) and "Shaded" turns it OFF —
    // written through the Graphics panel's own settings keys, so its checkboxes (built from those
    // keys) and the mode can never disagree. Seeded from the legacy flat/wireframe toggles.
    {
        auto* shadeGroup = new QButtonGroup(this);
        shadeGroup->setExclusive(true);
        auto mkShade = [&](int mode, const QString& tip) {
            auto* b = new QToolButton(center);
            b->setIcon(QIcon(shadeBallGlyph(mode)));
            b->setIconSize(QSize(20, 20));   // = the pixmap's own size: no up/down-scaling
            b->setToolTip(tip);
            b->setCheckable(true);
            b->setCursor(Qt::PointingHandCursor);
            b->setFixedSize(28, kBarH);
            b->setStyleSheet(QLatin1String(kIconBtnQss));
            shadeGroup->addButton(b, mode);
            viewBar->addWidget(b);
        };
        mkShade(0, QStringLiteral("Wireframe"));
        mkShade(1, QStringLiteral("Flat: base colour only"));
        mkShade(2, QStringLiteral("Shaded: PBR, post effects off"));
        mkShade(3, QStringLiteral("Rendered: PBR + IBL, shadows, SSAO, tonemap"));
        int savedShade = QSettings().value(QStringLiteral("wardrobe2/view/shadeMode"), -1).toInt();
        if (savedShade < 0) {   // first run on this scheme: honour the old independent toggles
            savedShade = vchk(QStringLiteral("wireframe"), false) ? 0
                       : vchk(QStringLiteral("flat"), false)      ? 1 : 3;
        }
        savedShade = qBound(0, savedShade, 3);
        if (QAbstractButton* b = shadeGroup->button(savedShade)) b->setChecked(true);
        auto applyShade = [this](int id) {
            QSettings s2;
            s2.setValue(QStringLiteral("wardrobe2/view/shadeMode"), id);
            if (!m_view) return;
            m_view->setWireframe(id == 0);
            m_view->setPbr(id >= 2);
            if (id >= 2) {   // Shaded/Rendered own the post pipeline (keys = Graphics checkboxes')
                const bool post = (id == 3);
                s2.setValue(QStringLiteral("wardrobe2/viewport/ibl"), post);
                s2.setValue(QStringLiteral("wardrobe2/viewport/shadow"), post);
                s2.setValue(QStringLiteral("wardrobe2/viewport/ssao"), post);
                s2.setValue(QStringLiteral("wardrobe2/viewport/tonemap"), post);
                m_view->setFeatureIbl(post);
                m_view->setShadowEnabled(post);
                m_view->setSsaoEnabled(post);
                m_view->setFeatureTonemap(post);
            }
        };
        connect(shadeGroup, &QButtonGroup::idClicked, this, applyShade);
        // ⌄ — shading popover holding the Channel combo. ALSO scrollable in place: wheel over
        // this button cycles channels live without opening anything (see eventFilter).
        auto* shadeMore = new QToolButton(center);
        m_shadeMoreBtn = shadeMore;
        shadeMore->setText(QStringLiteral("⌄"));
        shadeMore->setPopupMode(QToolButton::InstantPopup);
        shadeMore->setFixedSize(18, kBarH);   // same slim arrow as the Overlays one
        shadeMore->setCursor(Qt::PointingHandCursor);
        shadeMore->setStyleSheet(QLatin1String(kArrowBtnQss));
        shadeMore->installEventFilter(this);   // wheel → cycle channel
        auto* sm2 = new QMenu(shadeMore);
        {
            auto* row = new QWidget(sm2);
            auto* rl2 = new QHBoxLayout(row);
            rl2->setContentsMargins(10, 4, 10, 4);
            rl2->setSpacing(6);
            rl2->addWidget(new QLabel(QStringLiteral("Channel"), row));
            rl2->addWidget(m_channelCombo, 1);   // reparents; its connect lives on
            auto* wa = new QWidgetAction(sm2);
            wa->setDefaultWidget(row);
            sm2->addAction(wa);
        }
        shadeMore->setMenu(sm2);
        viewBar->addWidget(shadeMore);
        // The button reports the live channel, so a non-default view is never a mystery.
        auto syncChannelBtn = [this]() {
            if (!m_shadeMoreBtn || !m_channelCombo) return;
            const int i = m_channelCombo->currentIndex();
            const QString ch = m_channelCombo->currentText();
            m_shadeMoreBtn->setText(i == 0 ? QStringLiteral("⌄") : QStringLiteral("◆"));
            m_shadeMoreBtn->setToolTip(
                QStringLiteral("Channel: %1\nScroll here to flip channels · click for the list").arg(ch));
        };
        syncChannelBtn();
        connect(m_channelCombo, &QComboBox::currentIndexChanged, this,
                [syncChannelBtn](int) { syncChannelBtn(); });
    }
    sep();
    // ── Overlays — Blender's split control, exactly like Models: a SPHERE TOGGLE (master on/off
    // for every guide) plus an ARROW opening a persistent settings panel (Qt::Popup QFrame, so
    // ticking boxes doesn't close it). One home for grid/axes and the whole rig/bone set — this
    // replaces both the Grid/Skeleton bar buttons and the dev-only Rig popup.
    {
        auto* ovBtn = new QToolButton(center);
        m_overlayBtn = ovBtn;
        ovBtn->setIcon(QIcon(overlayGlyph()));
        ovBtn->setIconSize(QSize(20, 20));
        ovBtn->setToolTip(QStringLiteral("Show overlays (grid, axes, skeleton…) — master toggle"));
        ovBtn->setCursor(Qt::PointingHandCursor);
        ovBtn->setCheckable(true);
        ovBtn->setChecked(QSettings().value(QStringLiteral("wardrobe2/view/overlays"), true).toBool());
        ovBtn->setFixedSize(28, kBarH);
        ovBtn->setStyleSheet(QLatin1String(kIconBtnQss));
        viewBar->addWidget(ovBtn);

        m_overlayPanel = new QFrame(this, Qt::Popup);
        m_overlayPanel->setObjectName(QStringLiteral("ovPanel"));
        m_overlayPanel->setStyleSheet(QStringLiteral(
            "QFrame#ovPanel{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
            "QLabel{color:#cccccc;} QCheckBox{color:#cccccc;}"));
        auto* opl = new QVBoxLayout(m_overlayPanel);
        opl->setContentsMargins(12, 10, 12, 10);
        opl->setSpacing(4);
        auto* ovHdr = new QLabel(QStringLiteral("Viewport Overlays"), m_overlayPanel);
        ovHdr->setStyleSheet(QLatin1String(kHdrQss));
        opl->addWidget(ovHdr);
        auto ovSection = [&](const QString& t) {
            auto* l = new QLabel(t, m_overlayPanel);
            l->setStyleSheet(QStringLiteral("%1margin-top:6px;").arg(QLatin1String(kSubHdrQss)));
            opl->addWidget(l);
        };
        // Plain guides: persist the key, push to GL, gate on the master.
        auto addOverlay = [&](const QString& label, const QString& key, bool def, bool indent,
                              const QString& tip, std::function<void(bool)> apply) {
            auto* cb = new QCheckBox(label, m_overlayPanel);
            if (indent) cb->setStyleSheet(QStringLiteral("QCheckBox{color:#cccccc;margin-left:16px;}"));
            if (!tip.isEmpty()) cb->setToolTip(tip);
            cb->setChecked(QSettings().value(key, def).toBool());
            connect(cb, &QCheckBox::toggled, this, [this, key, apply](bool on) {
                QSettings().setValue(key, on);
                if (m_overlaysOn) apply(on);   // master off → GL stays dark; re-applied on master on
            });
            opl->addWidget(cb);
            m_overlayChks.append({cb, apply});
            return cb;
        };
        // Rig-mirrored guides: route through applyRigToggle so the Physics-panel duplicates and
        // the hidden skeleton state-holder stay in sync (applyRigToggle mirrors m_rigChk*, which
        // ARE these checkboxes — the retired Rig popup's members, re-homed here).
        auto addRigOverlay = [&](QCheckBox*& out, const QString& label, const QString& key,
                                 bool def, bool indent, const QString& tip,
                                 std::function<void(bool)> apply) {
            out = new QCheckBox(label, m_overlayPanel);
            if (indent) out->setStyleSheet(QStringLiteral("QCheckBox{color:#cccccc;margin-left:16px;}"));
            out->setToolTip(tip);
            out->setChecked(QSettings().value(key, def).toBool());
            connect(out, &QCheckBox::toggled, this, [this, key](bool on) {
                if (m_overlaysOn) applyRigToggle(key, on);
                else              QSettings().setValue(key, on);   // remember; GL stays dark
            });
            opl->addWidget(out);
            m_overlayChks.append({out, apply});
        };

        ovSection(QStringLiteral("Guides"));
        addOverlay(QStringLiteral("Ground grid"), QStringLiteral("wardrobe2/view/grid"), false, false,
                   QStringLiteral("Ground plane grid."),
                   [this](bool on) { if (m_view) m_view->setShowGrid(on); });
        addOverlay(QStringLiteral("Axis gizmo"), QStringLiteral("viewer/axisGizmo"), true, false,
                   QStringLiteral("Clickable X/Y/Z orientation ball in the viewport corner."),
                   [this](bool on) { if (m_view) m_view->setShowAxisGizmo(on); });
        addOverlay(QStringLiteral("Colored grid axes"), QStringLiteral("viewer/gridAxisColors"), true, true,
                   QStringLiteral("Tint the grid's world axes: X red, Z blue."),
                   [this](bool on) { if (m_view) m_view->setGridAxisColors(on); });

        ovSection(QStringLiteral("Skeleton"));
        addRigOverlay(m_rigChkSkel, QStringLiteral("Skeleton"),
                      QStringLiteral("wardrobe2/view/skeleton"), false, false,
                      QStringLiteral("Draw the bone hierarchy."),
                      [this](bool on) { if (m_view) m_view->setShowSkeleton(on); });
        addOverlay(QStringLiteral("Collision model"), QStringLiteral("wardrobe2/cloth/showColliders"), false, false,
                   QStringLiteral("Draw the cloth collision model — the authored capsules and plane "
                                  "colliders the cloth is solved against. Use it to see whether a "
                                  "garment is clipping because the capsules don't match the body."),
                   [this](bool on) { if (m_view) m_view->setShowColliders(on); });
        addRigOverlay(m_rigChkPhys, QStringLiteral("Physics bones"),
                      QStringLiteral("wardrobe2/cloth/showPhysBones"), false, false,
                      QStringLiteral("Overlay the cloth/physics bones (anchored grey, simulated orange)."),
                      [this](bool on) { if (m_view) m_view->setShowPhysBones(on); });
        addRigOverlay(m_rigChkAxis, QStringLiteral("Axis gizmos (per-bone)"),
                      QStringLiteral("wardrobe2/cloth/showPhysAxes"), true, true,
                      QStringLiteral("Per-bone XYZ rotation gizmo (R/G/B)."),
                      [this](bool on) { if (m_view) m_view->setShowPhysAxes(on); });
        addOverlay(QStringLiteral("Hardpoints"), QStringLiteral("wardrobe2/rig/hardpoints"), false, false,
                   QStringLiteral("Draw the rig's attach sockets (weapon grips, sheaths, trail emitters…) "
                                  "as labeled XYZ gizmos — where held/attached models snap on."),
                   [this](bool on) { if (m_view) m_view->setShowHardpoints(on); });
        addRigOverlay(m_rigChkNames, QStringLiteral("Bone names"),
                      QStringLiteral("wardrobe2/rig/boneNames"), false, false,
                      QStringLiteral("Label each bone at its position in the viewport."),
                      [this](bool on) { if (m_view) m_view->setShowBoneNames(on); });
        addRigOverlay(m_rigChkTrans, QStringLiteral("Translated names"),
                      QStringLiteral("wardrobe2/rig/boneNamesTranslated"), false, true,
                      QStringLiteral("Readable labels from verified D4 hardpoint/IK data; others keep bone_<hash>."),
                      [this](bool on) { if (m_view) m_view->setBoneNamesTranslated(on); });
        addRigOverlay(m_rigChkHideUnk, QStringLiteral("Hide unnamed bones"),
                      QStringLiteral("wardrobe2/rig/boneNamesHideUnknown"), false, true,
                      QStringLiteral("Only label bones with a known/translated name."),
                      [this](bool on) { if (m_view) m_view->setBoneNamesHideUnknown(on); });
        auto* ovNote = new QLabel(QStringLiteral(
            "<span style='color:#888'>Identified bones draw green; others show bone_&lt;hash&gt;.</span>"),
            m_overlayPanel);
        ovNote->setWordWrap(true);
        ovNote->setMinimumWidth(230);
        opl->addWidget(ovNote);

        // Master toggle: all guides off at once, remembering each box's own state.
        m_overlaysOn = ovBtn->isChecked();
        connect(ovBtn, &QToolButton::toggled, this, [this](bool on) {
            m_overlaysOn = on;
            QSettings().setValue(QStringLiteral("wardrobe2/view/overlays"), on);
            reapplyOverlays();   // off = force-off; on = restore each box (and the cloth flags)
            if (m_overlayPanel) m_overlayPanel->setEnabled(on);
        });
        m_overlayPanel->setEnabled(m_overlaysOn);

        // ▾ — opens/closes the panel (Blender's arrow beside the overlay toggle).
        auto* ovArrow = new QToolButton(center);
        ovArrow->setText(QStringLiteral("⌄"));
        ovArrow->setToolTip(QStringLiteral("Overlay settings"));
        ovArrow->setCursor(Qt::PointingHandCursor);
        ovArrow->setFixedSize(18, kBarH);
        ovArrow->setStyleSheet(QLatin1String(kArrowBtnQss));
        connect(ovArrow, &QToolButton::clicked, this, [this, ovArrow]() {
            if (!m_overlayPanel) return;
            if (m_overlayPanel->isVisible()) { m_overlayPanel->hide(); return; }
            m_overlayPanel->adjustSize();
            m_overlayPanel->move(ovArrow->mapToGlobal(QPoint(0, ovArrow->height() + 2)));
            m_overlayPanel->show();
            m_overlayPanel->raise();
        });
        viewBar->addWidget(ovArrow);
    }
    sep();
    mkToggle(QStringLiteral("fx"), QStringLiteral("FX"), QStringLiteral("Show FX submeshes"),
             QSettings().value(QStringLiteral("wardrobe2/showFx"), true).toBool(),
             [this](bool on) { QSettings().setValue(QStringLiteral("wardrobe2/showFx"), on); recomputePartVisibility(); });
    mkToggle(QStringLiteral("sim"), QStringLiteral("SIM"), QStringLiteral("Show cloth-sim submeshes"),
             QSettings().value(QStringLiteral("wardrobe2/showSim"), true).toBool(),
             [this](bool on) { QSettings().setValue(QStringLiteral("wardrobe2/showSim"), on); recomputePartVisibility(); });
    mkToggle(QStringLiteral("form"), QStringLiteral("FORM"),
             QStringLiteral("Show transformation-form submeshes (e.g. Warlock's demon form) — hidden by default"),
             QSettings().value(QStringLiteral("wardrobe2/showForm"), false).toBool(),
             [this](bool on) { QSettings().setValue(QStringLiteral("wardrobe2/showForm"), on); recomputePartVisibility(); });
    // (Turntable / Spin moved to the Camera popup, where it has a speed control.)
    viewBar->addStretch(1);
    // (Fullscreen text button removed — the ⛶ strip icon covers it. Graphics/Camera/Lighting/
    //  Shaders/Detail/Physics move onto the viewport's N-strip below —
    //  they're reparented there after m_view exists, exactly like the Models tab.)
    m_viewBarW = new QWidget(center);   // container → hidden as one unit when maximized
    m_viewBarW->setLayout(viewBar);
    cl->addWidget(m_viewBarW);

    m_view = new GLModelWidget(center);
    m_view->setMinimumSize(360, 360);
    m_view->setFocusPolicy(Qt::StrongFocus);   // F fullscreen / Esc unselect / H-family hotkeys
    // (No viewport tooltip — a hint card following the cursor over the model is just in the way.
    // Same call the Models tab dropped. "Reset view" is middle-click, in GLModelWidget.)
    // ── Blender N-strip: the settings popovers live as icon buttons on the viewport's right
    // edge (below the axis gizmo) instead of eating half the toolbar. The BUTTONS move; the
    // panels, their toggle functions and dev-gating are untouched — popups open LEFTward beside
    // the strip (panelPosLeftOf). Identical to the Models tab, same shared glyphs.
    {
        m_vpStrip = new QWidget(m_view);
        auto* sv = new QVBoxLayout(m_vpStrip);
        sv->setContentsMargins(2, 2, 2, 2);
        sv->setSpacing(3);
        // »/« — hide/show the whole right panel column, Blender's sidebar arrow: the column
        // vanishes COMPLETELY (viewport takes the width) and this floating arrow brings it back.
        m_sideArrow = new QToolButton(m_vpStrip);
        m_sideArrow->setText(QStringLiteral("»"));
        m_sideArrow->setToolTip(QStringLiteral("Hide the side panels (this arrow brings them back)"));
        m_sideArrow->setCheckable(true);
        m_sideArrow->setCursor(Qt::PointingHandCursor);
        m_sideArrow->setFixedSize(26, 18);
        m_sideArrow->setStyleSheet(QLatin1String(kArrowBtnQss));
        connect(m_sideArrow, &QToolButton::toggled, this, [this](bool on) { setSideCollapsed(on); });
        sv->addWidget(m_sideArrow);
        const struct { QAbstractButton* b; int g; } stripBtns[] = {
            {m_vpBtn, 0},     {m_camBtn, 2},    {m_lightBtn, 3},
            {m_shaderBtn, 4}, {m_detailBtn, 5}, {m_physBtn, 7}};
        for (const auto& e : stripBtns) {
            if (!e.b) continue;
            e.b->setToolTip(e.b->text() + QStringLiteral(" — ") + e.b->toolTip());
            e.b->setText(QString());
            e.b->setIcon(QIcon(stripGlyph(e.g)));
            e.b->setIconSize(QSize(16, 16));
            e.b->setFixedSize(26, 26);   // 24 left almost nothing around a 16px glyph
            sv->addWidget(e.b);   // reparents the button out of the toolbar
        }
        // Fullscreen ⛶ on the strip too (parity with the Stable tab) — same toggle as the toolbar button.
        auto* fsStrip = new QToolButton(m_vpStrip);
        fsStrip->setText(QStringLiteral("⛶"));
        fsStrip->setToolTip(QStringLiteral("Fullscreen — viewport fills the tab (F / Esc restores)"));
        fsStrip->setCursor(Qt::PointingHandCursor);
        fsStrip->setFixedSize(26, 26);
        fsStrip->setStyleSheet(QLatin1String(kArrowBtnQss));
        connect(fsStrip, &QToolButton::clicked, this, [this] { toggleFullscreen(); });
        sv->addWidget(fsStrip);
        m_vpStrip->adjustSize();
        m_view->installEventFilter(this);   // Resize → keep the strip pinned to the right edge
        // (The hidden-column state restores at the END of the sidebar build — m_sidebar doesn't
        // exist yet here, so checking the arrow now would set the flag but hide nothing, leaving
        // the width clamp to squeeze the column into a broken sliver.)
    }
    // Double-click: select the picked part in the PARTS tree (Blender's click-in-viewport-
    // highlights-in-outliner). The SAME part again, or empty space (part = -1), deselects.
    // Whether the camera also zooms to it is the Camera panel's "Frame part on select"
    // (viewer/framePartOnPick — read live inside GLModelWidget, shared with the Models tab).
    connect(m_view, &GLModelWidget::partFocused, this, [this](int part) {
        if (m_partTree) {
            QTreeWidgetItem* hit = nullptr;
            for (int r = 0; r < m_partTree->topLevelItemCount() && !hit; ++r) {
                QTreeWidgetItem* root = m_partTree->topLevelItem(r);
                for (int c2 = 0; c2 < root->childCount(); ++c2)
                    if (root->child(c2)->data(0, Qt::UserRole).toInt() == part) {
                        hit = root->child(c2);
                        break;
                    }
            }
            const bool samePart = hit && hit->isSelected() && m_partTree->selectedItems().size() == 1;
            m_partTree->clearSelection();   // selectionChanged → highlight sync
            if (hit && !samePart) {
                if (hit->parent()) hit->parent()->setExpanded(true);
                hit->setSelected(true);
                m_partTree->scrollToItem(hit);
            }
        }
        // Slot sync (unchanged): the equipment grid follows the picked part's slot.
        if (part >= 0 && part < m_partSlot.size()) {
            const int slot = m_partSlot[part];
            if (slot >= 0 && slot < kSlotCount && slot != m_activeSlot && m_slotCells[slot]) {
                m_slotCells[slot]->setChecked(true);   // updates the look grid via the button group
                selectSlot(slot);
            }
        }
    });
    // Right-click a part in the viewport → hide/show it + copy its material name.
    connect(m_view, &GLModelWidget::partRightClicked, this,
            [this](int part, const QPoint& gp) { showPartContextMenu(part, gp); });
    // F = fullscreen (maximize-in-place), matching the Models tab. Scoped to the viewport
    // (click it first) so it never hijacks the letter "f" typed into the tab's search boxes.
    // Re-framing is middle-click; the old F-to-fit is retired with it.
    auto* fsSc = new QShortcut(QKeySequence(Qt::Key_F), m_view);
    fsSc->setContext(Qt::WidgetShortcut);
    connect(fsSc, &QShortcut::activated, this, [this] { toggleFullscreen(); });
    // Ctrl+Z = undo the last outfit change (slot/dye/weapon/creator pick), anywhere in the tab.
    auto* undoSc = new QShortcut(QKeySequence::Undo, this);
    undoSc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(undoSc, &QShortcut::activated, this, [this] { undoLook(); });
    {   // Apply saved viewport shading features + exposure + background.
        QSettings s;
        auto vp = [&](const QString& k, bool def) { return s.value(QStringLiteral("wardrobe2/viewport/") + k, def).toBool(); };
        m_view->setFeatureDetail(vp(QStringLiteral("detail"), true));
        m_view->setFeatureSpecAA(vp(QStringLiteral("specaa"), true));
        m_view->setShadowEnabled(vp(QStringLiteral("shadow"), true));
        m_view->setSsaoEnabled(vp(QStringLiteral("ssao"), true));
        m_view->setFeatureSubsurface(vp(QStringLiteral("subsurface"), true));
        m_view->setFeatureHair(vp(QStringLiteral("hair"), true));
        m_view->setFeatureIbl(vp(QStringLiteral("ibl"), true));
        m_view->setFeatureMask(vp(QStringLiteral("mask"), false));
        m_view->setFeatureTonemap(vp(QStringLiteral("tonemap"), true));
        m_view->setBackfaceCull(false);   // double-sided always (back-face culling removed)
        applyLightRig();   // restore the saved lighting rig (exposure/shadows/SSAO + campfire rig)
        const QString bg = s.value(QStringLiteral("wardrobe2/viewport/bg")).toString();
        if (!bg.isEmpty()) m_view->setBackgroundColor(QColor(bg));
        m_view->setBackgroundGradient(
            s.value(QStringLiteral("wardrobe2/viewport/bgGradient"), false).toBool());
        // Apply the view-toolbar toggle states (the buttons were built before m_view existed).
        auto v2 = [&](const QString& k, bool def) { return s.value(QStringLiteral("wardrobe2/view/") + k, def).toBool(); };
        m_view->setShowTextures(true);   // textures always sampled; the channel combo picks the view
        m_view->setViewChannel(s.value(QStringLiteral("wardrobe2/view/channel"), 0).toInt());
        // Shading mode (the four spheres): wire/PBR follow the saved mode, not the legacy toggles.
        {
            int sm = s.value(QStringLiteral("wardrobe2/view/shadeMode"), -1).toInt();
            if (sm < 0) sm = v2(QStringLiteral("wireframe"), false) ? 0
                           : v2(QStringLiteral("flat"), false)      ? 1 : 3;
            m_view->setWireframe(sm == 0);
            m_view->setPbr(sm >= 2);
        }
        // Overlays: each guide = master gate AND its own box (the boxes' states are the keys).
        const bool ovOn = v2(QStringLiteral("overlays"), true);
        m_view->setShowGrid(ovOn && v2(QStringLiteral("grid"), false));
        m_view->setShowSkeleton(ovOn && v2(QStringLiteral("skeleton"), false));
        m_view->setShowAxisGizmo(ovOn && s.value(QStringLiteral("viewer/axisGizmo"), true).toBool());
        m_view->setGridAxisColors(ovOn && s.value(QStringLiteral("viewer/gridAxisColors"), true).toBool());
        // Turntable is (re)started in the deferred step at the end of refresh(), AFTER the model
        // has framed itself and the camera has been restored — otherwise the startup framing glide
        // fights the spin and it stutters/sticks until toggled off/on.
        m_view->setOrthographic(s.value(QStringLiteral("wardrobe2/ortho"), false).toBool());   // projection
    }
    applyClothParams();   // restore saved cloth-physics tuning
    applyDetailConfig();  // restore saved detail-map experiment config (defaults = shipped behaviour)

    cl->addWidget(m_view, 1);       // viewport fills the centre
    cl->addWidget(m_timeline);      // transport bar CENTERED UNDER THE VIEWPORT (Models-tab parity)

    // Animation timer + wiring.
    m_animTimer = new QTimer(this);
    connect(m_animTimer, &QTimer::timeout, this, &WardrobeTab2::tickAnimation);
    connect(m_speedCombo, &QComboBox::currentIndexChanged, this, [this](int) { applyAnimSpeed(); });
    connect(m_animSlider, &QSlider::valueChanged, this, [this](int v) {
        if (!m_view) return;
        m_view->setFrame(v);
        const int fc = m_view->animFrameCount();
        const float fps = m_animFps > 0 ? m_animFps : 30.0f;
        if (m_frameSpin && m_frameSpin->value() != v) { QSignalBlocker b(m_frameSpin); m_frameSpin->setValue(v); }
        if (m_timeLabel && fc > 0)
            m_timeLabel->setText(QStringLiteral("%1 / %2s").arg(v / fps, 0, 'f', 2).arg((fc - 1) / fps, 0, 'f', 2));
    });
    connect(m_playBtn, &QPushButton::clicked, this, [this] {
        if (!m_view) return;
        if (m_animTimer->isActive()) { m_animTimer->stop(); m_playBtn->setIcon(transportGlyph(0)); }
        else if (m_view->animFrameCount() > 0) {
            if (m_animSlider->value() >= m_view->animFrameCount() - 1) m_animSlider->setValue(0);
            m_animTimer->start(); m_playBtn->setIcon(transportGlyph(1));
        }
    });
    // Selection change (incl. arrow-key navigation) loads + plays the clip instantly.
    connect(m_anims, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* it, QListWidgetItem*) {
        if (!it) return;
        const QString name = it->data(Qt::UserRole).toString();
        if (name != m_playingAnim) { playAnimByName(name); m_animJustSelected = true; }
    });
    // Re-clicking the ALREADY-selected clip unselects it (stops + clears). But a first click
    // fires currentItemChanged (which plays) on press, then itemClicked on release — without
    // the guard that release would immediately toggle the just-selected clip back off.
    connect(m_anims, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        if (m_animJustSelected) { m_animJustSelected = false; return; }   // this click just selected it
        if (it && it->data(Qt::UserRole).toString() == m_playingAnim)
            clearAnimationSelection();
    });
    connect(m_animSearch, &QLineEdit::textChanged, this, [this](const QString&) { fillAnimList(); });

    // ── Right: panel column — the SAME stacking-toggle system as the Models tab (PanelBox.h).
    // A vertical icon strip toggles panels in and out of a vertical splitter: several stack at
    // once, drag the handles to size them, ▲▼ to reorder, ✕ to hide. With nothing up the whole
    // column collapses to just the strip. Layout persists (wardrobe2/rememberPanels).
    m_sidebar = new QWidget;
    auto* sl = new QHBoxLayout(m_sidebar);
    sl->setContentsMargins(4, 4, 4, 4);
    sl->setSpacing(2);
    m_rstripW = new QWidget(m_sidebar);
    m_rstripLay = new QVBoxLayout(m_rstripW);
    m_rstripLay->setContentsMargins(0, 2, 0, 0);
    m_rstripLay->setSpacing(2);
    sl->addWidget(m_rstripW);           // strip on the LEFT of the panels, like Models
    // (The sidebar hide/show arrow lives on the VIEWPORT's N-strip, not in this column — when
    // the column hides it hides completely, Blender-style, and the floating arrow brings it back.)
    m_rsplit = new QSplitter(Qt::Vertical, m_sidebar);
    m_rsplit->setChildrenCollapsible(false);   // a drag can't erase a panel — ✕ / the strip does
    m_rsplit->setHandleWidth(4);
    sl->addWidget(m_rsplit, 1);
    connect(m_rsplit, &QSplitter::splitterMoved, this, [this](int, int) { saveSidePanelLayout(); });

    const QString kListCss = QStringLiteral(
        "QTreeWidget{background:#161616;border:1px solid #2a2a2a;font-size:11px;}"
        "QHeaderView::section{background:#222;color:#999;border:0;padding:2px;font-size:10px;}");
    auto styleList = [&](QTreeWidget* t) {
        t->setRootIsDecorated(false);
        t->setStyleSheet(kListCss);
        t->setSelectionMode(QAbstractItemView::SingleSelection);
        return t;
    };
    // Register a panel: content into a hidden PanelBox in the splitter, a checkable icon toggle
    // onto the strip. `key` is the STABLE settings token (titles may grow live counts later).
    auto section = [&](const QString& title, const QString& key, QWidget* content,
                       const QPixmap& icon, const QString& tip) -> QWidget* {
        const int page = m_rsections.size();
        auto* box = new PanelBox(title, content, m_rsplit);
        box->hide();               // up only when its strip toggle says so
        m_rsplit->addWidget(box);
        m_rsections.append(box);
        m_sectKeys.append(QStringLiteral("wardrobe2/panel/") + key);
        auto* b = new QToolButton(m_rstripW);
        b->setIcon(QIcon(icon));
        b->setIconSize(QSize(16, 16));
        b->setCheckable(true);     // checked = panel is up
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QStringLiteral(
            "QToolButton{border:1px solid transparent;border-radius:3px;padding:3px;background:transparent;}"
            "QToolButton:hover{border-color:#b0453c;}"
            "QToolButton:checked{background:#8a1414;border-color:#a01818;}"));
        connect(b, &QToolButton::toggled, this, [this, page](bool on) { showSidePanel(page, on); });
        // Header buttons: ▲▼ reorder; ✕ hides by unchecking the strip toggle (one path in/out).
        connect(box->up,    &QToolButton::clicked, this, [this, page]() { moveSidePanel(page, -1); });
        connect(box->down,  &QToolButton::clicked, this, [this, page]() { moveSidePanel(page, +1); });
        connect(box->close, &QToolButton::clicked, this, [this, page]() {
            if (page < m_rpageBtns.size()) m_rpageBtns[page]->setChecked(false);
        });
        m_rstripLay->addWidget(b);
        m_rpageBtns.append(b);
        return box;
    };

    // PARTS — the per-part visibility hierarchy.
    m_partTree = new QTreeWidget;
    m_partTree->setStyleSheet(kListCss);
    m_partTree->setColumnCount(2);
    m_partTree->setHeaderLabels({QStringLiteral("Part"), QStringLiteral("Tris")});
    m_partTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_partTree->setRootIsDecorated(true);
    m_partTree->setMouseTracking(true);
    m_partTree->viewport()->setMouseTracking(true);
    m_partTree->setToolTip(QStringLiteral("Uncheck to hide a submesh · hover/select to highlight · Esc clears"));
    section(QStringLiteral("PARTS"), QStringLiteral("PARTS"), m_partTree,
            ModelOutlinerModel::kindIcon(ModelOutlinerModel::Part),
            QStringLiteral("Parts — per-piece visibility and triangle counts"));

    // MATERIALS — App Materials / SubObject Apps / Materials / Vertex Buffers.
    m_appMatList = styleList(new QTreeWidget);
    m_appMatList->setColumnCount(4);
    m_appMatList->setHeaderLabels({QStringLiteral("#"), QStringLiteral("Material"),
                                   QStringLiteral("Source"), QStringLiteral("Tris")});
    m_subObjList = styleList(new QTreeWidget);
    m_subObjList->setColumnCount(5);
    m_subObjList->setHeaderLabels({QStringLiteral("#"), QStringLiteral("Material"),
                                   QStringLiteral("Source"), QStringLiteral("Tris"), QStringLiteral("2-Sided")});
    m_matList = styleList(new QTreeWidget);
    m_matList->setColumnCount(4);
    m_matList->setHeaderLabels({QStringLiteral("Material"), QStringLiteral("SNO"),
                                QStringLiteral("Flags"), QStringLiteral("Cloth")});
    m_vbList = styleList(new QTreeWidget);
    m_vbList->setColumnCount(4);
    m_vbList->setHeaderLabels({QStringLiteral("#"), QStringLiteral("Verts"),
                               QStringLiteral("Tris"), QStringLiteral("Source")});
    m_matTabs = new QTabWidget;
    m_matTabs->setDocumentMode(true);
    m_matTabs->addTab(m_appMatList, QStringLiteral("App Materials"));
    m_matTabs->addTab(m_subObjList, QStringLiteral("SubObject Apps"));
    m_matTabs->addTab(m_matList,    QStringLiteral("Materials"));
    m_matTabs->addTab(m_vbList,     QStringLiteral("Vertex Buffers"));
    m_matTabs->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);   // greedy: fills its panel
    section(QStringLiteral("MATERIALS"), QStringLiteral("MATERIALS"), m_matTabs,
            ModelOutlinerModel::kindIcon(ModelOutlinerModel::Material),
            QStringLiteral("Materials — app materials, sub-objects, vertex buffers"));

    // MATERIAL TEXTURES — Textures / Values / Shaders (for the selected material).
    m_matTexList = styleList(new QTreeWidget);
    m_matTexList->setColumnCount(3);
    m_matTexList->setHeaderLabels({QStringLiteral("Role"), QStringLiteral("SNO"), QStringLiteral("Name")});
    m_matValList = styleList(new QTreeWidget);
    m_matValList->setColumnCount(2);
    m_matValList->setHeaderLabels({QStringLiteral("Value"), QStringLiteral("Amount")});
    m_shaderList = styleList(new QTreeWidget);
    m_shaderList->setColumnCount(2);
    m_shaderList->setHeaderLabels({QStringLiteral("Field"), QStringLiteral("Value")});
    // Detail-maps tab: the up-to-3 tiled detail maps for the selected material, with the
    // authored per-map intensities/offset and the effective (game-model) strengths applied.
    m_detailList = styleList(new QTreeWidget);
    m_detailList->setColumnCount(5);
    m_detailList->setHeaderLabels({QStringLiteral("Detail map"), QStringLiteral("Texture"),
                                   QStringLiteral("N.Int"), QStringLiteral("R.Int"),
                                   QStringLiteral("R.Off")});
    m_texTabs = new QTabWidget;
    m_texTabs->setDocumentMode(true);
    m_texTabs->addTab(m_matTexList, QStringLiteral("Textures"));
    m_texTabs->addTab(m_matValList, QStringLiteral("Values"));
    m_texTabs->addTab(m_shaderList, QStringLiteral("Shaders"));
    m_texTabs->addTab(m_detailList, QStringLiteral("Detail"));
    m_texTabs->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);   // greedy: fills its panel
    section(QStringLiteral("MATERIAL TEXTURES"), QStringLiteral("SHADING"), m_texTabs,
            ModelOutlinerModel::kindIcon(ModelOutlinerModel::TexGroup),
            QStringLiteral("Material textures — bindings, values, shaders, detail maps"));

    // TEXTURE PREVIEW — six square channel tiles, captions overlaid bottom-left
    // and hidden on hover (identical styling/behaviour to the Models tab). The
    // row scrolls horizontally so all six tiles stay reachable in a narrow sidebar.
    auto* prev = new QWidget;
    auto* pv = new QVBoxLayout(prev);
    pv->setContentsMargins(0, 0, 0, 0);
    pv->setSpacing(2);
    auto* tileHost = new QWidget;
    auto* tpRow = new QHBoxLayout(tileHost);
    tpRow->setSpacing(1);
    tpRow->setContentsMargins(0, 0, 0, 0);
    static const char* kChanCap[6] = {"COLOR", "ROUGHNESS", "METAL", "NORMAL", "ALPHA", "EMISSIVE"};
    constexpr int kTile = 92;
    for (int c = 0; c < 6; ++c) {
        m_chanLbl[c] = new QLabel;
        m_chanLbl[c]->setFixedSize(kTile, kTile);
        m_chanLbl[c]->setAlignment(Qt::AlignCenter);
        m_chanLbl[c]->setScaledContents(false);
        m_chanLbl[c]->setStyleSheet(QStringLiteral(
            "QLabel{border:1px solid #444;border-radius:0px;background:#1b1b1b;}"));
        m_chanLbl[c]->installEventFilter(this);   // hover → caption + zoom preview
        m_chanCap[c] = new QLabel(QString::fromLatin1(kChanCap[c]), m_chanLbl[c]);
        m_chanCap[c]->setStyleSheet(QStringLiteral(
            "QLabel{color:#fff;background:rgba(0,0,0,150);border:0;padding:0px 2px;font-size:8px;}"));
        m_chanCap[c]->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_chanCap[c]->adjustSize();
        m_chanCap[c]->move(2, kTile - m_chanCap[c]->height() - 2);
        tpRow->addWidget(m_chanLbl[c]);
    }
    tpRow->addStretch(1);
    auto* tpScroll = new QScrollArea;
    tpScroll->setWidget(tileHost);
    tpScroll->setWidgetResizable(true);
    tpScroll->setFrameShape(QFrame::NoFrame);
    tpScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tpScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    tpScroll->setFixedHeight(kTile + 16);
    pv->addWidget(tpScroll);
    pv->addStretch(1);
    section(QStringLiteral("TEXTURE PREVIEW"), QStringLiteral("PREVIEW"), prev,
            ModelOutlinerModel::kindIcon(ModelOutlinerModel::TexTiles),
            QStringLiteral("Texture preview — the six channel tiles for the selected material"));
    // ANIMATIONS — the clip list, in the right column like the Models tab (transport stays under
    // the viewport). The play glyph doubles as its strip icon.
    section(QStringLiteral("ANIMATIONS"), QStringLiteral("ANIMATIONS"), m_animPanel,
            transportGlyph(0).pixmap(QSize(16, 16)),
            QStringLiteral("Animations — the model's clip list (filter, search, select to play)"));
    m_rstripLay->addStretch(1);   // registrations are done — pin the strip buttons to the top

    // ── Restore the panel layout: which panels are up, their order, their heights. Same scheme
    // as the Models tab (Settings ▸ Wardrobe ▸ "Remember the right-hand panel layout").
    {
        QSettings st;
        const bool remember = st.value(QStringLiteral("wardrobe2/rememberPanels"), true).toBool();
        QStringList shown;   // default: none up — the column collapses to just the strip
        if (remember && st.contains(QStringLiteral("wardrobe2/panels/shown"))) {
            shown = st.value(QStringLiteral("wardrobe2/panels/shown")).toStringList();
        } else {
            // Seed from the legacy Settings checkboxes so upgraders keep their sections.
            if (st.value(QStringLiteral("wardrobe2/dbg/parts"), false).toBool())     shown << QStringLiteral("PARTS");
            if (st.value(QStringLiteral("wardrobe2/dbg/materials"), false).toBool()) shown << QStringLiteral("MATERIALS");
            if (st.value(QStringLiteral("wardrobe2/dbg/textures"), false).toBool())  shown << QStringLiteral("SHADING");
            if (st.value(QStringLiteral("wardrobe2/dbg/preview"), false).toBool())   shown << QStringLiteral("PREVIEW");
            shown << QStringLiteral("ANIMATIONS");   // the clip list was always visible before the move
        }
        const QStringList heights = remember
            ? st.value(QStringLiteral("wardrobe2/panels/sizes")).toStringList() : QStringList();
        m_panelRestore = true;   // don't let these toggles write a half-applied layout back out
        for (const QString& name : shown) {
            const int page = m_sectKeys.indexOf(QStringLiteral("wardrobe2/panel/") + name);
            if (page < 0 || page >= m_rpageBtns.size()) continue;
            m_rsplit->addWidget(m_rsections[page]);   // re-append → the up panels land in the
                                                      // saved order, ahead of the hidden ones
            m_rpageBtns[page]->setChecked(true);      // → showSidePanel
        }
        m_panelRestore = false;
        // Heights, by name: walk the splitter and hand each up panel the height saved against it.
        if (heights.size() == shown.size() && !heights.isEmpty()) {
            QList<int> sizes = m_rsplit->sizes();
            int k = 0;
            for (int i = 0; i < m_rsplit->count() && k < heights.size(); ++i) {
                if (m_rsplit->widget(i)->isHidden()) continue;
                sizes[i] = heights[k++].toInt();
            }
            m_rsplit->setSizes(sizes);
        }
        updateSidebarCollapse();
        // Hidden-column state (» arrow on the N-strip): restored HERE, with m_sidebar built, so
        // the whole column actually hides instead of just arming the flag.
        if (m_sideArrow && st.value(QStringLiteral("wardrobe2/panels/collapsed"), false).toBool())
            m_sideArrow->setChecked(true);   // → setSideCollapsed
    }

    // Zoomed hover popup for a channel thumbnail (0.5s delay, wheel-resizable, clamped).
    m_chanPreview = new QLabel(nullptr, Qt::ToolTip | Qt::FramelessWindowHint);
    m_chanPreview->setStyleSheet(QStringLiteral("background:#000;border:1px solid #777;"));
    m_chanPreview->hide();
    m_hoverTimer = new QTimer(this);
    m_hoverTimer->setSingleShot(true);
    m_hoverTimer->setInterval(500);
    connect(m_hoverTimer, &QTimer::timeout, this, [this] { if (m_hoverChan >= 0) showChanPreview(m_hoverChan); });

    connect(m_matList, &QTreeWidget::itemSelectionChanged, this, [this]() {
        auto* it = m_matList->currentItem();
        showMaterial(it ? it->text(0) : QString());
    });
    // Selecting a single texture in the Textures tab switches the preview to that
    // texture's RGBA channel split (RGBA · R · G · B · A) — exactly like Models tab.
    connect(m_matTexList, &QTreeWidget::itemSelectionChanged, this, [this]() {
        auto* it = m_matTexList->currentItem();
        if (!it) return;
        previewTexture(it->text(2), it->text(1).toLongLong());   // NAME, SNO
    });

    connect(m_partTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem*, int) { recomputePartVisibility(); });
    connect(m_partTree, &QTreeWidget::itemSelectionChanged, this, [this]() {
        if (m_view) m_view->setHighlightParts(selectedParts());
        // Also select the picked part's MATERIAL in the materials list, so the material/texture panels
        // populate for that part — parity with the Models tab's part→material auto-select.
        const QList<QTreeWidgetItem*> sel = m_partTree->selectedItems();
        if (sel.size() == 1 && m_matList) {
            const int idx = sel.first()->data(0, Qt::UserRole).toInt();
            if (idx >= 0 && idx < m_lastMerged.primitives.size()) {
                const QString mat = m_lastMerged.primitives[idx].materialName;
                for (int r = 0; r < m_matList->topLevelItemCount(); ++r) {
                    QTreeWidgetItem* mi = m_matList->topLevelItem(r);
                    if (mi->text(0) == mat) {
                        m_matList->setCurrentItem(mi);   // → itemSelectionChanged → showMaterial(mat)
                        m_matList->scrollToItem(mi);
                        break;
                    }
                }
            }
        }
    });
    connect(m_partTree, &QTreeWidget::itemEntered, this, [this](QTreeWidgetItem* it, int) {
        if (!m_view) return;
        QList<int> hot = selectedParts(); hot += primitivesOf(it);
        m_view->setHighlightParts(hot);
    });
    m_partTree->viewport()->installEventFilter(this);
    m_partTree->installEventFilter(this);
    // Right-click → Copy name (the raw material/submesh name, minus the [FX]/[SIM] tags).
    m_partTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_partTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* it = m_partTree->itemAt(pos);
        if (!it) return;
        const int idx = it->data(0, Qt::UserRole).toInt();
        QString name = (idx >= 0 && idx < m_lastMerged.primitives.size())
                           ? m_lastMerged.primitives[idx].materialName   // child: clean material name
                           : it->text(0);                                // root: source-piece name
        Q_UNUSED(name);
        // Same menu as the viewport, so parts can be copied/exported straight from the panel.
        showPartContextMenu(idx, m_partTree->viewport()->mapToGlobal(pos));
    });
    applySidebars();

    // The control column has grown tall — make it scrollable.
    auto* leftScroll = new QScrollArea;
    leftScroll->setWidget(left);
    leftScroll->setWidgetResizable(true);
    leftScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    leftScroll->setMinimumWidth(470);

    split->addWidget(leftScroll);
    split->addWidget(center);
    split->addWidget(m_sidebar);
    split->setSizes({490, 500, 290});
    // ── Layout stability ─────────────────────────────────────────────────────────────────────
    // Without stretch factors a QSplitter re-apportions EVERY child on every resize, and on any
    // child's size-hint change — so toggling a panel or a long item name shifted all three columns.
    // Pin the sides, let only the centre viewport absorb slack, and forbid collapsing so no column
    // (and none of its content) can ever be squeezed out of existence.
    split->setStretchFactor(0, 0);   // left (creator/slots): keeps its width
    split->setStretchFactor(1, 1);   // centre (viewport): absorbs all resizing
    split->setStretchFactor(2, 0);   // right (sidebar): keeps its width
    split->setChildrenCollapsible(false);
    updateSidebarCollapse();   // min/max width tracks whether any panel is up (owns the widths)

    // Remember the column widths across sessions — especially the right (panel) column, which used
    // to snap back to 290 every launch. Restore the saved split, then persist any user drag.
    if (PanelPersist::enabled()) {
        const QVariantList sv = QSettings().value(QStringLiteral("wardrobe2/splitSizes")).toList();
        if (sv.size() == 3) {
            QList<int> sizes;
            for (const QVariant& v : sv) sizes << qMax(0, v.toInt());
            if (sizes[0] > 0 && sizes[1] > 0) split->setSizes(sizes);
        }
    }
    connect(split, &QSplitter::splitterMoved, this, [this, split](int, int) {
        if (m_restoring || !PanelPersist::enabled()) return;
        QVariantList sv;
        for (int s : split->sizes()) sv << s;
        QSettings().setValue(QStringLiteral("wardrobe2/splitSizes"), sv);
    });

    connect(m_class, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_restoring) return;
        QSettings().setValue(QStringLiteral("wardrobe2/class"), m_class->currentData());
        // Batch the repopulate so the outfit rebuilds ONCE at the end, not once per populate*.
        m_restoring = true;
        populateCreator(); populateSlots(); populateWeapons();   // weapon types are class-filtered
        m_restoring = false;
        rebuildOutfit();
        remapAnimationForRig();   // swap the clip to the new rig's equivalent (or clear it)
    });
    connect(m_gender, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_restoring) return;
        QSettings().setValue(QStringLiteral("wardrobe2/gender"), m_gender->currentData());
        // ── Outfit retention across the gender swap: capture what's equipped BEFORE the
        // repopulate, then re-equip each slot's opposite-gender twin. D4 names encode gender as
        // the 4th character of the class prefix ("barF_stor23_TRS" ↔ "barM_stor23_TRS"), so the
        // twin is a single-character swap; anything without a twin in the data falls back to
        // whatever populateSlots restored (usually "(none)").
        QString kept[5];
        for (int i = 0; i < 5; ++i)
            kept[i] = (m_slot[i] && m_slot[i]->currentIndex() > 0) ? m_slot[i]->currentText() : QString();
        m_restoring = true;
        populateCreator(); populateSlots(); populateWeapons();
        for (int i = 0; i < 5; ++i) {
            if (kept[i].size() < 4 || !m_slot[i]) continue;
            QString twin = kept[i];
            const QChar g = twin.at(3).toLower();
            if (g == QLatin1Char('m'))      twin[3] = twin.at(3).isUpper() ? QLatin1Char('F') : QLatin1Char('f');
            else if (g == QLatin1Char('f')) twin[3] = twin.at(3).isUpper() ? QLatin1Char('M') : QLatin1Char('m');
            else continue;   // not a gendered name (e.g. shared piece) — leave the restore alone
            const int idx = m_slot[i]->findText(twin, Qt::MatchFixedString);   // case-insensitive exact
            if (idx > 0) {
                m_slot[i]->setCurrentIndex(idx);
                QSettings().setValue(QStringLiteral("wardrobe2/slot/%1").arg(i),
                                     m_slot[i]->currentText());   // handler is suppressed while restoring
            }
        }
        m_restoring = false;
        rebuildOutfit();
        remapAnimationForRig();
    });
    for (int i = 0; i < 9; ++i)
        connect(m_creator[i], &QComboBox::currentIndexChanged, this, [this, i](int) {
            if (m_restoring) return;
            QSettings().setValue(QStringLiteral("wardrobe2/creator/%1").arg(i), m_creator[i]->currentData().toString());
            scheduleRebuild();
        });
    connect(m_skinTone, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_restoring) return;
        QSettings().setValue(QStringLiteral("wardrobe2/skinTone"), m_skinTone->currentText());
        scheduleRebuild();
    });
    connect(m_skinDetail, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_restoring) return;
        QSettings().setValue(QStringLiteral("wardrobe2/skinDetail"), m_skinDetail->currentText());
        scheduleRebuild();
    });
    for (int i = 0; i < 5; ++i)
        connect(m_slot[i], &QComboBox::currentIndexChanged, this, [this, i](int) {
            if (m_restoring) return;
            QSettings().setValue(QStringLiteral("wardrobe2/slot/%1").arg(i), m_slot[i]->currentText());
            scheduleRebuild();
        });
    connect(m_weaponType, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_restoring) return;
        QSettings().setValue(QStringLiteral("wardrobe2/weaponType"), m_weaponType->currentText());
        populateWeapons();
    });
    connect(m_weapon, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_restoring) return;
        QSettings().setValue(QStringLiteral("wardrobe2/weapon"), m_weapon->currentText());
        scheduleRebuild();
    });
    connect(m_weaponType2, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_restoring) return;
        QSettings().setValue(QStringLiteral("wardrobe2/weaponType2"), m_weaponType2->currentText());
        populateWeapons();
    });
    connect(m_weapon2, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_restoring) return;
        QSettings().setValue(QStringLiteral("wardrobe2/weapon2"), m_weapon2->currentText());
        scheduleRebuild();
    });
    connect(m_weapon3, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_restoring) return;
        QSettings().setValue(QStringLiteral("wardrobe2/weaponSheath"), m_weapon3->currentText());
        scheduleRebuild();
    });
    connect(m_weapon4, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_restoring) return;
        QSettings().setValue(QStringLiteral("wardrobe2/weaponSheath2"), m_weapon4->currentText());
        scheduleRebuild();
    });
    connect(m_backTrophy, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_restoring) return;
        QSettings().setValue(QStringLiteral("wardrobe2/backTrophy"), m_backTrophy->currentText());
        scheduleRebuild();
    });
    // Build both popups eagerly so m_env (Preview) and m_fovSlider (Camera) exist for
    // restore/rebuild before the user ever opens either panel.
    buildPreviewPanel();
    buildCameraPanel();

    // Make static text selectable/copyable (status, headers, names, paths, values) — drag to
    // select, Ctrl+A to select all, right-click for a Copy menu (all provided by QLabel).
    for (QLabel* lbl : findChildren<QLabel*>())
        lbl->setTextInteractionFlags(lbl->textInteractionFlags()
                                     | Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    // Add a right-click "Copy" to list/tree widgets that don't already have their own context
    // menu — copies the selected row(s) as newline-separated text (works with multi-select).
    for (QListWidget* lw : findChildren<QListWidget*>()) {
        if (lw->contextMenuPolicy() != Qt::DefaultContextMenu) continue;   // keep existing menus
        lw->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(lw, &QWidget::customContextMenuRequested, this, [lw](const QPoint& p) {
            QStringList lines;
            for (QListWidgetItem* it : lw->selectedItems()) lines << it->text();
            if (lines.isEmpty() && lw->currentItem()) lines << lw->currentItem()->text();
            QMenu menu;
            QAction* copy = menu.addAction(QStringLiteral("Copy"));
            copy->setEnabled(!lines.isEmpty());
            connect(copy, &QAction::triggered, lw, [lines] { QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n'))); });
            menu.exec(lw->viewport()->mapToGlobal(p));
        });
    }
    for (QTreeWidget* tw : findChildren<QTreeWidget*>()) {
        if (tw->contextMenuPolicy() != Qt::DefaultContextMenu) continue;   // part tree keeps its own
        tw->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tw, &QWidget::customContextMenuRequested, this, [tw](const QPoint& p) {
            QStringList lines;
            for (QTreeWidgetItem* it : tw->selectedItems()) lines << it->text(0);
            if (lines.isEmpty() && tw->currentItem()) lines << tw->currentItem()->text(0);
            QMenu menu;
            QAction* copy = menu.addAction(QStringLiteral("Copy"));
            copy->setEnabled(!lines.isEmpty());
            connect(copy, &QAction::triggered, tw, [lines] { QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n'))); });
            menu.exec(tw->viewport()->mapToGlobal(p));
        });
    }
}

// Defined with the other cloth-tuning code near fillClothSimTuning (bottom of file).
static QString runClothAudit(const QString& d4, SnoIndex* idx, CascReader* rd, QWidget* parent);

void WardrobeTab2::refresh()
{
    if (m_loaded || !m_index || !m_index->isLoaded()) return;
    m_loaded = true;
    QElapsedTimer refT; refT.start();
    // ONE-TIME: wipe the remembered equipped models that were crash-looping the restore on every
    // launch (a set saved just before a crash). The remember feature itself stays fully active —
    // anything equipped from now on is saved/restored normally; this only clears the stuck state
    // once (gated by a persistent flag).
    {
        QSettings s;
        if (!s.value(QStringLiteral("wardrobe2/clearRemembered_v1"), false).toBool()) {
            for (int i = 0; i < 5; ++i) s.remove(QStringLiteral("wardrobe2/slot/%1").arg(i));
            s.remove(QStringLiteral("wardrobe2/weaponType"));  s.remove(QStringLiteral("wardrobe2/weapon"));
            s.remove(QStringLiteral("wardrobe2/weaponType2")); s.remove(QStringLiteral("wardrobe2/weapon2"));
            s.remove(QStringLiteral("wardrobe2/anim"));
            s.setValue(QStringLiteral("wardrobe2/clearRemembered_v1"), true);
        }
    }
    AppearanceMeta::instance().ensureBuilt(Config::d4dataDir(), m_index, m_reader);   // collections for Equip set
    // D4_CLOTH_AUDIT=1 → corpus-wide cloth audit ("Cloth Audit.bat"): sweep every
    // Appearance SNO and report tuning-resolution failures / fallback-path pieces as a
    // list (cloth_audit.csv + cloth_audit_summary.txt next to the exe). Deferred so the
    // window paints first; the sweep shows its own progress dialog.
    if (qEnvironmentVariableIsSet("D4_CLOTH_AUDIT"))
        QTimer::singleShot(1500, this, [this] {
            const QString msg = runClothAudit(Config::d4dataDir(), m_index, m_reader, this);
            QMessageBox::information(this, QStringLiteral("Cloth audit"), msg);
        });
    // Item-level hover metadata (rarity / season / description / introduced-in) — background build;
    // re-stamp the card tooltips when it lands.
    ItemHoverIndex::instance().ensureBuilt(Config::d4dataDir());
    connect(&ItemHoverIndex::instance(), &ItemHoverIndex::readyChanged, this,
            [this] { fillLookGrid(); refreshSlotCells(); });
    // Appearance-icon atlas index (same trigger point as the Models tab). Refill the grid/cells
    // when it finishes; show the live percentage while it scans.
    IconIndex::instance().ensureBuilt(Config::d4dataDir());
    connect(&IconIndex::instance(), &IconIndex::readyChanged, this, [this] { fillLookGrid(); refreshSlotCells(); fillCreatorGrid(); refreshCreatorCells(); refreshEnsembles(); });
    connect(&AppearanceMeta::instance(), &AppearanceMeta::readyChanged, this, [this] {
        populateSets();   // collection-based theme sets resolve once metadata is ready
        fillLookGrid(); refreshSlotCells();
    });
    // Icon-index progress is shown by the global status-bar indicator now; nothing per-tab.
    restoreSelection();        // class/gender (combos), env
    rebuildDyeCombo();         // restores saved dye via wardrobe/dyeSel
    restoreSlotDyes();         // restores each slot's per-slot pigment
    ensureWeaponIndex();       // restores saved weapon type/model when ready
    populateCreator();         // restores saved creator picks
    populateSlots();           // restores saved slot picks, then rebuilds
    if (m_view) {
        if (m_env) m_view->setEnvironment(m_env->currentIndex());
        if (m_fovSlider) m_view->setFov(float(m_fovSlider->value()));
    }
    // Restore the remembered animation and start it playing. This MUST be deferred
    // onto the event loop: refresh() runs synchronously during MainWindow construction,
    // before window.show() and before the GL context/first paint exist. Driving the
    // animation (and the cloth sim it triggers on first paint) at that point crashed;
    // running it after the window is up — exactly like a manual click — is safe.
    const QString savedAnim = QSettings().value(QStringLiteral("wardrobe2/anim")).toString();
    if (!savedAnim.isEmpty())
        QTimer::singleShot(0, this, [this, savedAnim] { playAnimByName(savedAnim); });
    // "Remember camera on relaunch": restore the saved orbit/zoom AFTER the model has framed
    // itself (deferred, so it wins over rebuildOutfit()'s initial reset/snap). Then start the
    // turntable last, so it owns the camera cleanly (no glide left to fight the spin).
    QTimer::singleShot(0, this, [this] {
        restoreCameraState();
        if (m_view && QSettings().value(QStringLiteral("wardrobe2/turntable"), false).toBool()) {
            m_view->setSpinSpeed(float(qBound(1, QSettings().value(QStringLiteral("wardrobe2/turntableSpeed"), 25).toInt(), 100)) / 1000.0f);
            m_view->setAutoSpin(true);
        }
    });
    qInfo("startup: wardrobe refresh — %lld ms total (restore + weapon index + initial rebuild)", refT.elapsed());
}

QString WardrobeTab2::classPrefix() const
{
    return (m_class->currentData().toString() + m_gender->currentData().toString()).toLower();
}

// Re-apply the debug-sidebar setting each time the tab is shown (so toggling it in
// Settings takes effect on return, without a restart).
void WardrobeTab2::showEvent(QShowEvent* ev)
{
    applySidebars();
    // Re-lay-out the appearance grid now that the tab is actually visible. IconMode won't
    // position items that were added while the widget was hidden (e.g. the readyChanged refill
    // fired while another tab was up), so the grid looks empty until we repopulate on show.
    if (m_lookLayout) { fillLookGrid(); refreshSlotCells(); }
    if (m_creatorLayout) { fillCreatorGrid(); refreshCreatorCells(); }
    BrowserTab::showEvent(ev);
}

// Show/hide the two debug panels per settings; the sidebar shows if either is on.
void WardrobeTab2::applySidebars()
{
    QSettings s;
    // (The per-section debug toggles that lived here are GONE — the sidebar's sections are now
    // stacking panels toggled directly from the icon strip, exactly like the Models tab. The old
    // wardrobe2/dbg/* keys still seed the first-run layout in the ctor's restore block.)
    // (Animations moved to a right-side stacking panel — it's toggled from the icon strip like the
    //  other panels, so it must NOT be force-shown/hidden here or it fights the PanelBox.)
    // Ensembles panel (toggle in Preview Settings ▸ Geometry & debug; default on).
    if (m_ensemblePanel) m_ensemblePanel->setVisible(s.value(QStringLiteral("wardrobe2/viewport/ensembles"), true).toBool());
    // Debug log + Copy debug button (off by default; toggle in Settings → Wardrobe 2).
    const bool showLog = s.value(QStringLiteral("wardrobe2/dbg/log"), false).toBool();
    if (m_status)        m_status->setVisible(showLog);
    if (m_copyDebugBtn)  m_copyDebugBtn->setVisible(showLog);
    // (Developer mode is retired — the Shaders / Detail maps / Physics panels are always
    //  available from the N-strip. The Rig button is gone; its toggles live in Overlays ▾.)
}

// ── Stacking panels — the same four functions the Models tab has, against m_rsplit ───────────

// Strip toggle → panel in/out of the splitter. QSplitter honours child visibility: a hidden
// panel takes no space and its handle goes with it.
void WardrobeTab2::showSidePanel(int page, bool on)
{
    if (page < 0 || page >= m_rsections.size()) return;
    PanelBox* box = m_rsections[page];
    const bool was = !box->isHidden();
    box->setVisible(on);
    if (on && !was && !m_panelRestore) panelBoxArrive(m_rsplit, box);   // arrive at a fitting height
    updateSidebarCollapse();
    saveSidePanelLayout();
}

// ▲▼ — move a panel one slot among the VISIBLE panels, so the arrows do what they look like
// they do even while others are hidden.
void WardrobeTab2::moveSidePanel(int page, int delta)
{
    if (!m_rsplit || page < 0 || page >= m_rsections.size()) return;
    PanelBox* box = m_rsections[page];
    QVector<int> vis;   // splitter indices of the up panels, in order (isHidden, not isVisible:
                        // the latter is false for every child while the tab itself is unshown)
    for (int i = 0; i < m_rsplit->count(); ++i)
        if (!m_rsplit->widget(i)->isHidden()) vis << i;
    const int cur = vis.indexOf(m_rsplit->indexOf(box));
    const int tgt = cur + delta;
    if (cur < 0 || tgt < 0 || tgt >= vis.size()) return;   // already at an end
    const QList<int> sizes = m_rsplit->sizes();
    m_rsplit->insertWidget(vis[tgt], box);                 // moves the existing child
    m_rsplit->setSizes(sizes);                             // insertWidget resets sizes — restore
    saveSidePanelLayout();
}

// Which panels are up, in what order, at what heights. Deliberately names + heights, not
// QSplitter::saveState() — that blob is positional, and the hidden panels still occupy splitter
// slots, so index N would mean a different panel between runs.
void WardrobeTab2::saveSidePanelLayout()
{
    if (!m_rsplit || m_panelRestore) return;   // don't write while the ctor is replaying a layout
    QSettings s;
    const QList<int> sizes = m_rsplit->sizes();
    QStringList shown, heights;
    for (int i = 0; i < m_rsplit->count(); ++i) {
        QWidget* w = m_rsplit->widget(i);
        if (w->isHidden()) continue;
        const int page = m_rsections.indexOf(static_cast<PanelBox*>(w));
        if (page < 0) continue;
        shown   << m_sectKeys.value(page).section(QLatin1Char('/'), -1);
        heights << QString::number(sizes.value(i));
    }
    s.setValue(QStringLiteral("wardrobe2/panels/shown"), shown);
    s.setValue(QStringLiteral("wardrobe2/panels/sizes"), heights);
}

// With no panels up the column shrinks to just the icon strip (the strip must stay reachable —
// it's the only way to bring a panel back); with any panel up it gets a workable width again.
// (Fully HIDING the column is separate — setSideCollapsed hides the whole pane, so the widths
// here only ever apply while it's visible. The flag must NOT clamp widths: that combination is
// exactly the broken 26px sliver.)
void WardrobeTab2::updateSidebarCollapse()
{
    if (!m_sidebar || !m_rstripW) return;
    bool any = false;
    for (PanelBox* b : m_rsections)
        if (b && !b->isHidden()) { any = true; break; }
    if (any) {
        m_sidebar->setMinimumWidth(260);
        m_sidebar->setMaximumWidth(QWIDGETSIZE_MAX);
    } else {
        m_sidebar->setMinimumWidth(0);
        m_sidebar->setMaximumWidth(m_rstripW->sizeHint().width() + 10);
    }
}

// » — hide the whole right column, Blender-style: no reserved sliver, the viewport takes the
// width, and the floating « on the N-strip brings it back. The panels keep their shown/hidden
// states — nothing inside the column is touched, the column itself just leaves.
void WardrobeTab2::setSideCollapsed(bool on)
{
    m_sideCollapsed = on;
    QSettings().setValue(QStringLiteral("wardrobe2/panels/collapsed"), on);
    if (m_sideArrow) {
        m_sideArrow->setText(on ? QStringLiteral("«") : QStringLiteral("»"));
        m_sideArrow->setToolTip(on ? QStringLiteral("Show the side panels")
                                   : QStringLiteral("Hide the side panels (this arrow brings them back)"));
    }
    if (m_sidebar) m_sidebar->setVisible(!on && !m_viewMaxed);
}

// Re-apply settings live when the Settings dialog closes (nude base + debug panels).
void WardrobeTab2::onSettingsChanged()
{
    applySidebars();
    if (m_loaded) rebuildOutfit();   // nude-base / fallback order may have changed
    fillLookGrid();                  // re-stamp look-card icons (presence-badge toggles)
}

// Live (per-toggle) apply from the Settings dialog: update the toggleable panels instantly,
// and only rebuild the outfit when the change actually affects geometry/textures (nude base).
void WardrobeTab2::onSettingsLiveChanged(bool rebuild)
{
    applySidebars();
    // Weapon settings live in Settings ▸ Wardrobe ▸ Weapons now. A class-restriction flip changes
    // the weapon LISTS (not just the seating), so refill them — populateWeapons ends with its own
    // rebuild + grid refresh, covering the reseat too.
    const bool cr = QSettings().value(QStringLiteral("wardrobe2/weap/classRestrict"), true).toBool();
    if (m_loaded && cr != m_lastClassRestrict) {
        populateWeapons();   // also re-syncs m_lastClassRestrict + slot availability
        return;
    }
    if (rebuild && m_loaded) rebuildOutfit();
}

// Resolve a texture name → SNO (case-insensitive) via a lazily-built map of group 44.
qint64 WardrobeTab2::texSnoFor(const QString& texName)
{
    if (texName.isEmpty() || !m_index) return 0;
    if (m_texSno.isEmpty())
        for (const SnoEntry& e : m_index->entries(kGroupTexture))
            m_texSno.insert(e.name.toLower(), e.snoId);
    return m_texSno.value(texName.toLower(), 0);
}

// Load Diablo IV's real character-screen reflection probe (a prefiltered RGBA16F HDR cubemap)
// and hand it to the viewport for ambient specular reflections. Best-effort: any failure just
// leaves the analytic hemisphere in place. The payload is face-major; serTex gives each face's
// top-mip byte offset (entriesPerFace = serTex.size()/faceCount, face f → subres[f*epf]).
void WardrobeTab2::loadReflectionProbe()
{
    if (!m_view || !m_reader || !m_reader->isReady()) return;
    const QString name = QStringLiteral("Character_Select_Sorcerer_World_N01_W01 (Lighting)_probe");
    const qint64 sno = texSnoFor(name);
    if (sno <= 0) return;
    QFile mf(Config::d4dataDir() + QStringLiteral("/json/base/meta/Texture/") + name
             + QStringLiteral(".tex.json"));
    if (!mf.open(QIODevice::ReadOnly)) return;
    const TexMeta meta = parseTexMetaJson(mf.readAll());
    // Only the uncompressed RGBA16F probe layout (eTexFormat 25) is handled by the GPU upload.
    if (!meta.valid || meta.eTexFormat != 25 || meta.faceCount != 6 || meta.subres.size() < 6)
        return;
    const int epf = meta.subres.size() / meta.faceCount;   // serTex entries per face (= 11 here)
    if (epf < 1) return;
    const QByteArray payload = m_reader->readPayloadBySno(quint64(sno));
    if (payload.isEmpty()) return;
    QVector<quint32> faceOffsets;
    faceOffsets.reserve(6);
    for (int f = 0; f < 6; ++f) faceOffsets.append(meta.subres[f * epf].offset);
    m_view->setReflectionCubemap(payload, meta.width, faceOffsets);
    m_view->setReflectionEnabled(
        QSettings().value(QStringLiteral("wardrobe2/light/reflections"), true).toBool());
}

// Restore saved class/gender (and env) without triggering rebuilds mid-restore.
void WardrobeTab2::restoreSelection()
{
    QSettings s;
    m_restoring = true;
    const QString cls = s.value(QStringLiteral("wardrobe2/class")).toString();
    const QString gen = s.value(QStringLiteral("wardrobe2/gender")).toString();
    if (!cls.isEmpty()) { int i = m_class->findData(cls); if (i >= 0) m_class->setCurrentIndex(i); }
    if (!gen.isEmpty()) { int i = m_gender->findData(gen); if (i >= 0) m_gender->setCurrentIndex(i); }
    if (m_env) m_env->setCurrentIndex(s.value(QStringLiteral("models/viewport/env"), 1).toInt());
    if (m_skinTone) {
        const QString st = s.value(QStringLiteral("wardrobe2/skinTone")).toString();
        const int i = st.isEmpty() ? 0 : m_skinTone->findText(st);
        if (i > 0) m_skinTone->setCurrentIndex(i);
    }
    if (m_skinDetail) {
        const QString sd = s.value(QStringLiteral("wardrobe2/skinDetail")).toString();
        const int i = sd.isEmpty() ? 0 : m_skinDetail->findText(sd);
        if (i > 0) m_skinDetail->setCurrentIndex(i);
    }
    m_restoring = false;
}

// Compatibility shim (called from rebuildOutfit historically) → recompute visibility.
// (applyFxSim removed — the FX/SIM/FORM toggles call recomputePartVisibility directly.)

// Combine the tree checkboxes with the FX/SIM toggles → per-part viewport visibility.
void WardrobeTab2::recomputePartVisibility()
{
    if (!m_view) return;
    const bool showFx  = QSettings().value(QStringLiteral("wardrobe2/showFx"), true).toBool();
    const bool showSim = QSettings().value(QStringLiteral("wardrobe2/showSim"), true).toBool();
    const bool showForm = QSettings().value(QStringLiteral("wardrobe2/showForm"), false).toBool();   // demon/transform form off by default
    QHash<int, bool> checked;
    if (m_partTree)
        for (int r = 0; r < m_partTree->topLevelItemCount(); ++r) {
            QTreeWidgetItem* root = m_partTree->topLevelItem(r);
            for (int c = 0; c < root->childCount(); ++c) {
                QTreeWidgetItem* it = root->child(c);
                checked[it->data(0, Qt::UserRole).toInt()] = (it->checkState(0) == Qt::Checked);
            }
        }
    int n = qMax(m_partFx.size(), m_partSim.size());
    n = qMax(n, m_partForm.size());
    for (int i = 0; i < n; ++i) {
        const bool isFx   = i < m_partFx.size()   && m_partFx[i];
        const bool isSim  = i < m_partSim.size()  && m_partSim[i];
        const bool isForm = i < m_partForm.size() && m_partForm[i];
        m_view->setPartVisible(i, checked.value(i, true) && !(isFx && !showFx)
                                  && !(isSim && !showSim) && !(isForm && !showForm));
    }
    m_view->update();
}

// (Re)build the per-part tree: one parent per source piece, one child per submesh.
void WardrobeTab2::rebuildPartList()
{
    if (!m_partTree) return;
    QSignalBlocker block(m_partTree);
    m_partTree->clear();
    QHash<QString, QTreeWidgetItem*> roots;
    for (int i = 0; i < m_lastMerged.primitives.size(); ++i) {
        const QString src = (i < m_partSource.size() && !m_partSource[i].isEmpty())
                                ? m_partSource[i] : QStringLiteral("(model)");
        QTreeWidgetItem*& root = roots[src];
        if (!root) {
            root = new QTreeWidgetItem(m_partTree, QStringList{src});
            root->setData(0, Qt::UserRole, -1);
            root->setFlags(root->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            root->setCheckState(0, Qt::Checked);
        }
        QString name = m_lastMerged.primitives[i].materialName;
        if (name.isEmpty()) name = QStringLiteral("part %1").arg(i);
        if (i < m_partFx.size()   && m_partFx[i])   name += QStringLiteral("  [FX]");
        if (i < m_partSim.size()  && m_partSim[i])  name += QStringLiteral("  [SIM]");
        if (i < m_partForm.size() && m_partForm[i]) name += QStringLiteral("  [FORM]");
        const bool covered = i < m_partCovered.size() && m_partCovered[i];
        if (covered) name += QStringLiteral("  [COVERED]");
        const int tris = (i < m_partTris.size()) ? m_partTris[i] : 0;
        auto* child = new QTreeWidgetItem(root, QStringList{name, QString::number(tris)});
        child->setData(0, Qt::UserRole, i);
        child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
        // Covered base-body regions start hidden (armour occupies them) but stay re-checkable.
        child->setCheckState(0, covered ? Qt::Unchecked : Qt::Checked);
    }
    // Per-root triangle totals.
    for (int r = 0; r < m_partTree->topLevelItemCount(); ++r) {
        QTreeWidgetItem* root = m_partTree->topLevelItem(r);
        int sum = 0;
        for (int c = 0; c < root->childCount(); ++c) sum += root->child(c)->text(1).toInt();
        root->setText(1, QString::number(sum));
    }
    m_partTree->expandAll();
    m_partTree->resizeColumnToContents(0);
}

// Fill every debug table from the assembled model: LOOKS (equipped pieces),
// App Materials / SubObject Apps / Vertex Buffers (per-primitive), Materials
// (unique, drives the texture preview).
void WardrobeTab2::populateMaterials()
{
    if (!m_matList) return;
    m_matList->clear();
    if (m_appMatList) m_appMatList->clear();
    if (m_subObjList) m_subObjList->clear();
    if (m_vbList)     m_vbList->clear();
    m_matNames.clear();
    const QString d4 = Config::d4dataDir();

    // App Materials / SubObject Apps / Vertex Buffers — per primitive (draw call).
    for (int i = 0; i < m_lastMerged.primitives.size(); ++i) {
        const MeshPrimitive& p = m_lastMerged.primitives[i];
        const QString src  = i < m_partSource.size() ? m_partSource[i] : QString();
        const int     tris = int(p.indices.size() / 3);
        const QString idx  = QString::number(i);
        if (m_appMatList)
            new QTreeWidgetItem(m_appMatList, QStringList{idx, p.materialName, src, QString::number(tris)});
        if (m_subObjList)
            new QTreeWidgetItem(m_subObjList, QStringList{idx, p.materialName, src,
                QString::number(tris), p.doubleSided ? QStringLiteral("✓") : QString()});
        if (m_vbList)
            new QTreeWidgetItem(m_vbList, QStringList{idx, QString::number(p.vertices.size()),
                QString::number(tris), src});
    }

    // Materials — unique materials (drives the texture/values/shaders preview).
    QSet<QString> seen;
    for (const MeshPrimitive& p : m_lastMerged.primitives) {
        const QString& m = p.materialName;
        if (m.isEmpty() || seen.contains(m)) continue;
        seen.insert(m);
        m_matNames << m;
        QString sno, flags, cloth;
        QFile f(d4 + QStringLiteral("/json/base/meta/Material/") + m + QStringLiteral(".mat.json"));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            sno = QString::number(o.value(QStringLiteral("__snoID__")).toInt());
            const int fl = o.value(QStringLiteral("dwFlags")).toInt();
            if (fl) flags = QStringLiteral("0x%1").arg(quint32(fl), 0, 16);
            if (m.contains(QLatin1String("cloth"), Qt::CaseInsensitive)
                || m.contains(QLatin1String("_sim"), Qt::CaseInsensitive)) cloth = QStringLiteral("✓");
        }
        new QTreeWidgetItem(m_matList, QStringList{m, sno, flags, cloth});
    }
    m_matList->resizeColumnToContents(0);
    if (m_appMatList) m_appMatList->resizeColumnToContents(1);
    if (m_subObjList) m_subObjList->resizeColumnToContents(1);
    if (m_matTabs)    m_matTabs->setTabText(2, QStringLiteral("Materials (%1)").arg(m_matNames.size()));
    showMaterial(QString());   // clear textures/preview until a row is picked
}

// Aspect-preserving channel-tile fill (square tile, image centred — Models-tab style).
void WardrobeTab2::setChanTile(int c, const QImage& img)
{
    m_chanImg[c] = img;
    if (!m_chanLbl[c]) return;
    if (img.isNull()) {
        m_chanLbl[c]->setPixmap(QPixmap());
        m_chanLbl[c]->setText(QStringLiteral("—"));
    } else {
        m_chanLbl[c]->setText(QString());
        const int side = qMax(8, m_chanLbl[c]->width() - 6);
        m_chanLbl[c]->setPixmap(QPixmap::fromImage(
            img.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    }
    if (m_chanCap[c]) {   // keep the caption pinned bottom-left after a relayout
        m_chanCap[c]->adjustSize();
        m_chanCap[c]->move(2, m_chanLbl[c]->height() - m_chanCap[c]->height() - 2);
        m_chanCap[c]->raise();
    }
}

// Relabel the six tile captions (e.g. material channels vs a single texture's RGBA split).
void WardrobeTab2::setChanCaptions(const char* const labels[6])
{
    for (int c = 0; c < 6; ++c) {
        if (!m_chanCap[c]) continue;
        const QString s = QString::fromLatin1(labels[c]);
        m_chanCap[c]->setText(s);
        m_chanCap[c]->setVisible(!s.isEmpty());
        m_chanCap[c]->adjustSize();
        if (m_chanLbl[c])
            m_chanCap[c]->move(2, m_chanLbl[c]->height() - m_chanCap[c]->height() - 2);
        m_chanCap[c]->raise();
    }
}

namespace {
// Greyscale view of one channel (0=R,1=G,2=B,3=A) of an RGBA image.
QImage chanGrey(const QImage& srcAny, int ch)
{
    if (srcAny.isNull()) return {};
    const QImage src = srcAny.convertToFormat(QImage::Format_RGBA8888);
    QImage out(src.width(), src.height(), QImage::Format_RGBA8888);
    for (int y = 0; y < src.height(); ++y) {
        const uchar* s = src.constScanLine(y);
        uchar* d = out.scanLine(y);
        for (int x = 0; x < src.width(); ++x) {
            const uchar v = s[x * 4 + ch];
            d[x * 4 + 0] = d[x * 4 + 1] = d[x * 4 + 2] = v;
            d[x * 4 + 3] = 255;
        }
    }
    return out;
}
}  // namespace

// A single texture was picked in MATERIAL TEXTURES → show its channel split:
// RGBA · R · G · B · A · (blank), like the Models tab.
void WardrobeTab2::previewTexture(const QString& texName, qint64 texSno)
{
    static const char* const kTexCaps[6] = {"RGBA", "R", "G", "B", "A", ""};
    setChanCaptions(kTexCaps);
    for (int c = 0; c < 6; ++c) setChanTile(c, {});
    if (texName.isEmpty()) return;
    const QImage img = MaterialDecode::texture(m_reader, Config::d4dataDir(), texName, texSno);
    if (img.isNull()) return;
    setChanTile(0, img);
    setChanTile(1, chanGrey(img, 0));
    setChanTile(2, chanGrey(img, 1));
    setChanTile(3, chanGrey(img, 2));
    setChanTile(4, chanGrey(img, 3));
    // tile 5 stays blank
}

// Fill MATERIAL TEXTURES (Textures / Values / Shaders) + channel preview for one material.
void WardrobeTab2::showMaterial(const QString& matName)
{
    if (!m_matTexList) return;
    static const char* const kMatCaps[6] = {"COLOR", "ROUGHNESS", "METAL", "NORMAL", "ALPHA", "EMISSIVE"};
    setChanCaptions(kMatCaps);
    m_matTexList->clear();
    if (m_matValList) m_matValList->clear();
    if (m_shaderList) m_shaderList->clear();
    if (m_detailList) m_detailList->clear();
    for (int c = 0; c < 6; ++c) setChanTile(c, {});
    if (m_texTabs) {
        m_texTabs->setTabText(0, QStringLiteral("Textures"));
        m_texTabs->setTabText(1, QStringLiteral("Values"));
        m_texTabs->setTabText(2, QStringLiteral("Shaders"));
        m_texTabs->setTabText(3, QStringLiteral("Detail"));
    }
    if (matName.isEmpty()) return;
    const QString d4 = Config::d4dataDir();
    QFile f(d4 + QStringLiteral("/json/base/meta/Material/") + matName + QStringLiteral(".mat.json"));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray json = f.readAll();

    // Textures tab.
    const QVector<MatTexture> texs = parseMaterialJson(json);
    QHash<QString, QPair<QString, qint64>> byRole;
    for (const MatTexture& t : texs) {
        if (t.texName.isEmpty()) continue;
        new QTreeWidgetItem(m_matTexList, QStringList{t.role, QString::number(t.texSno), t.texName});
        if (!byRole.contains(t.role)) byRole.insert(t.role, {t.texName, t.texSno});
    }
    m_matTexList->resizeColumnToContents(0);
    if (m_texTabs) m_texTabs->setTabText(0, QStringLiteral("Textures (%1)").arg(m_matTexList->topLevelItemCount()));

    // Values tab — EVERY authored MaterialValue by its real D4 name (Fresnel Strength, Skin
    // Roughness, Hair Scattering, Wetness Bias, Translucency Color Hero, …), scalars and vectors,
    // so the actual per-material parameters the game ships are all visible for browsing.
    if (m_matValList) {
        auto addVal = [&](const QString& n, const QString& v) {
            new QTreeWidgetItem(m_matValList, QStringList{n, v}); };
        const QJsonArray rtv = QJsonDocument::fromJson(json).object()
            .value(QStringLiteral("tUberMaterial")).toObject()
            .value(QStringLiteral("ptRunTimeMaterialValues")).toArray();
        for (const QJsonValue& rv : rtv) {
            const QJsonObject rvo = rv.toObject();
            for (const QJsonValue& sv : rvo.value(QStringLiteral("arMaterialScalarValues")).toArray()) {
                const QJsonObject tv = sv.toObject().value(QStringLiteral("tValue")).toObject();
                const QString nm = tv.value(QStringLiteral("snoMaterialValue")).toObject()
                                     .value(QStringLiteral("name")).toString();
                if (!nm.isEmpty()) addVal(nm, QString::number(tv.value(QStringLiteral("value")).toDouble(), 'g', 4));
            }
            for (const QJsonValue& vv : rvo.value(QStringLiteral("arMaterialVectorValues")).toArray()) {
                const QJsonObject tv = vv.toObject().value(QStringLiteral("tValue")).toObject();
                const QString nm = tv.value(QStringLiteral("snoMaterialValue")).toObject()
                                     .value(QStringLiteral("name")).toString();
                const QJsonObject val = tv.value(QStringLiteral("value")).toObject();
                if (!nm.isEmpty()) addVal(nm, QStringLiteral("%1, %2, %3")
                    .arg(val.value(QStringLiteral("x")).toDouble(), 0, 'g', 3)
                    .arg(val.value(QStringLiteral("y")).toDouble(), 0, 'g', 3)
                    .arg(val.value(QStringLiteral("z")).toDouble(), 0, 'g', 3));
            }
        }
        if (m_texTabs) m_texTabs->setTabText(1, QStringLiteral("Values (%1)").arg(m_matValList->topLevelItemCount()));
    }

    // Shaders tab — surface the material's shader/flag fields from the JSON.
    if (m_shaderList) {
        const QJsonObject o = QJsonDocument::fromJson(json).object();
        auto addField = [&](const QString& key) {
            if (!o.contains(key)) return;
            const QJsonValue v = o.value(key);
            new QTreeWidgetItem(m_shaderList,
                QStringList{key, v.isDouble() ? QString::number(v.toVariant().toLongLong())
                                              : v.toVariant().toString()});
        };
        for (const QString& k : o.keys())
            if (k.contains(QLatin1String("shader"), Qt::CaseInsensitive)
                || k.contains(QLatin1String("Shader"))) addField(k);
        addField(QStringLiteral("dwFlags"));
        addField(QStringLiteral("__snoID__"));
        m_shaderList->resizeColumnToContents(0);
        if (m_texTabs) m_texTabs->setTabText(2, QStringLiteral("Shaders (%1)").arg(m_shaderList->topLevelItemCount()));
    }

    // Detail tab — the up-to-3 tiled detail maps (library textures) + the authored per-map
    // intensities/offset, then the effective game-model strengths actually applied (avg intensity
    // for the bounded normal/roughness blend, summed roughness offset, fixed 8× tiling).
    if (m_detailList) {
        QString nName[3], rName[3];
        for (const MatTexture& t : texs) {
            switch (t.slot) {
                case 212: nName[0] = t.texName; break; case 213: nName[1] = t.texName; break;
                case 214: nName[2] = t.texName; break;
                case 218: rName[0] = t.texName; break; case 219: rName[1] = t.texName; break;
                case 220: rName[2] = t.texName; break;
                default: break;
            }
        }
        float sumN = 0, sumR = 0, sumOff = 0; int cN = 0, cR = 0;
        for (int i = 0; i < 3; ++i) {
            if (nName[i].isEmpty() && rName[i].isEmpty()) continue;
            const QByteArray kN = QStringLiteral("Normal Intensity - Detail Map %1").arg(i + 1).toUtf8();
            const QByteArray kR = QStringLiteral("Roughness Intensity - Detail Map %1").arg(i + 1).toUtf8();
            const QByteArray kO = QStringLiteral("Roughness Offset - Detail Map %1").arg(i + 1).toUtf8();
            const float nI = fxScalar(d4, matName, kN.constData(), 1.0f);
            const float rI = fxScalar(d4, matName, kR.constData(), 1.0f);
            const float rO = fxScalar(d4, matName, kO.constData(), 0.0f);
            const QString tex = !nName[i].isEmpty() ? nName[i] : rName[i];
            new QTreeWidgetItem(m_detailList, QStringList{
                QStringLiteral("Detail Map %1").arg(i + 1), tex,
                QString::number(nI, 'g', 3), QString::number(rI, 'g', 3), QString::number(rO, 'g', 3)});
            if (!nName[i].isEmpty()) { sumN += qMax(0.0f, nI); ++cN; }
            if (!rName[i].isEmpty()) { sumR += qMax(0.0f, rI); ++cR; sumOff += rO; }
        }
        if (m_detailList->topLevelItemCount() > 0) {
            auto* applied = new QTreeWidgetItem(m_detailList, QStringList{
                QStringLiteral("→ Applied"), QStringLiteral("avg intensity · 8× tiling"),
                QString::number(cN ? sumN / cN : 0.0f, 'g', 3),
                QString::number(cR ? sumR / cR : 0.0f, 'g', 3),
                QString::number(sumOff, 'g', 3)});
            QFont bold = applied->font(0); bold.setBold(true);
            for (int c = 0; c < 5; ++c) applied->setFont(c, bold);
        } else {
            new QTreeWidgetItem(m_detailList, QStringList{QStringLiteral("(none)"),
                QStringLiteral("this material has no detail maps")});
        }
        m_detailList->resizeColumnToContents(0);
        m_detailList->resizeColumnToContents(1);
        if (m_texTabs) m_texTabs->setTabText(3, QStringLiteral("Detail (%1)").arg(qMax(0, m_detailList->topLevelItemCount() - 1)));
    }

    // Channel preview tiles.
    auto decode = [&](const char* role) -> QImage {
        const auto it = byRole.constFind(QLatin1String(role));
        return it == byRole.constEnd() ? QImage() : MaterialDecode::texture(m_reader, d4, it->first, it->second);
    };
    const QImage color = decode("BASE_COLOR");
    setChanTile(0, color);
    setChanTile(1, decode("ROUGHNESS"));
    setChanTile(2, decode("METALLIC"));
    setChanTile(3, decode("NORMAL"));
    setChanTile(4, color.isNull() ? QImage()
                              : color.convertToFormat(QImage::Format_Alpha8).convertToFormat(QImage::Format_Grayscale8));
    setChanTile(5, decode("EMISSIVE"));
}

// Popup the zoomed channel image at the cursor, clamped inside the window/screen
// (flips to the other side of the cursor near an edge) — mirrors the Models tab.
void WardrobeTab2::showChanPreview(int chan)
{
    if (chan < 0 || chan >= 6 || !m_chanPreview || m_chanImg[chan].isNull()) return;
    const QPixmap pm = QPixmap::fromImage(m_chanImg[chan])
                           .scaled(m_previewPx, m_previewPx, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_chanPreview->setPixmap(pm);
    m_chanPreview->resize(pm.size());
    const QPoint cur = QCursor::pos();
    const QSize sz = pm.size();
    QRect bound = window() ? window()->frameGeometry() : QRect();
    QScreen* scr = QGuiApplication::screenAt(cur);
    if (!scr) scr = QGuiApplication::primaryScreen();
    if (scr) bound = bound.isNull() ? scr->availableGeometry() : bound.intersected(scr->availableGeometry());
    QPoint pos = cur + QPoint(18, 18);
    if (!bound.isNull()) {
        if (pos.x() + sz.width()  > bound.right())  pos.setX(cur.x() - 18 - sz.width());
        if (pos.y() + sz.height() > bound.bottom()) pos.setY(cur.y() - 18 - sz.height());
        pos.setX(qBound(bound.left(), pos.x(), qMax(bound.left(), bound.right()  - sz.width())));
        pos.setY(qBound(bound.top(),  pos.y(), qMax(bound.top(),  bound.bottom() - sz.height())));
    }
    m_chanPreview->move(pos);
    m_chanPreview->show();
    m_chanPreview->raise();
}

QList<int> WardrobeTab2::primitivesOf(QTreeWidgetItem* it) const
{
    QList<int> out;
    if (!it) return out;
    const int prim = it->data(0, Qt::UserRole).toInt();
    if (prim >= 0) out << prim;
    else for (int c = 0; c < it->childCount(); ++c) out += primitivesOf(it->child(c));
    return out;
}

QList<int> WardrobeTab2::selectedParts() const
{
    QList<int> out;
    if (m_partTree)
        for (QTreeWidgetItem* it : m_partTree->selectedItems()) out += primitivesOf(it);
    return out;
}

// Parts tree: Esc clears selection; empty click deselects; re-clicking a selected
// item deselects it; hover-leave reverts the highlight to the current selection.
static void cardMetrics(int availW, int& cols, int& cardW, int& cardH, int& iconW);   // defined below

bool WardrobeTab2::eventFilter(QObject* obj, QEvent* ev)
{
    const QEvent::Type t = ev->type();

    // ── Shading ⌄: wheel cycles the view channel in place (no popup, live preview) ──
    if (m_shadeMoreBtn && obj == m_shadeMoreBtn && t == QEvent::Wheel && m_channelCombo) {
        const auto* we = static_cast<QWheelEvent*>(ev);
        const int n = m_channelCombo->count();
        if (n > 0) {
            const int dir = we->angleDelta().y() > 0 ? -1 : 1;   // wheel-up = previous
            m_channelCombo->setCurrentIndex((m_channelCombo->currentIndex() + dir + n) % n);
        }
        return true;   // consume: never scroll the toolbar under us
    }

    // A settings popup opened/closed: mark its opener button active while open (the red
    // [panelOpen] state), and on close clear the stuck :hover highlight — Qt::Popup panels grab
    // the mouse, so the button never gets a Leave event otherwise. Button = "hoverBtn" property.
    if (t == QEvent::Show || t == QEvent::Hide) {
        const QVariant hb = obj->property("hoverBtn");
        if (hb.isValid()) {
            if (auto* b = qobject_cast<QWidget*>(hb.value<QObject*>())) {
                b->setProperty("panelOpen", t == QEvent::Show);
                b->style()->unpolish(b);
                b->style()->polish(b);   // re-evaluate the [panelOpen] style selector
                b->update();
                if (t == QEvent::Hide) {   // also drop the stuck hover highlight
                    b->setAttribute(Qt::WA_UnderMouse, false);
                    QEvent leave(QEvent::Leave);
                    QApplication::sendEvent(b, &leave);
                }
            }
        }
    }

    // ── Viewport Esc: clear the part selection (fullscreen-exit Esc is the m_fsEsc shortcut,
    // which is only enabled while maximized, so the two never fight). ──
    if (m_view && obj == m_view && t == QEvent::KeyPress
        && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_Escape && !m_viewMaxed) {
        if (m_partTree) m_partTree->clearSelection();   // selectionChanged → highlight sync
        if (m_view) m_view->setHighlightParts({});
        return true;
    }

    // Alt+H is also the menubar's &Help mnemonic, and mnemonics run through the shortcut system
    // BEFORE widget key events — accepting the ShortcutOverride claims the key back for us.
    if ((obj == m_view || obj == m_partTree) && t == QEvent::ShortcutOverride
        && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_H) {
        ev->accept();
        return true;
    }

    // ── Part-visibility hotkeys — H hide · Shift+H hide others (solo) · Alt+H show all.
    // Accepted from the PARTS tree *or* the VIEWPORT, same as the Models tab: hiding parts is
    // something you do while looking at the model. Check states change with signals blocked,
    // then ONE recompute — the per-item itemChanged path would recompute per part.
    if ((obj == m_view || obj == m_partTree) && t == QEvent::KeyPress && m_partTree) {
        const auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_H) {
            const QList<int> sel = selectedParts();
            const bool alt   = ke->modifiers() & Qt::AltModifier;
            const bool shift = ke->modifiers() & Qt::ShiftModifier;
            if (!alt && sel.isEmpty())
                return BrowserTab::eventFilter(obj, ev);   // H with nothing selected: ignore
            const bool was = m_partTree->blockSignals(true);
            for (int r = 0; r < m_partTree->topLevelItemCount(); ++r) {
                QTreeWidgetItem* root = m_partTree->topLevelItem(r);
                for (int c2 = 0; c2 < root->childCount(); ++c2) {
                    QTreeWidgetItem* it = root->child(c2);
                    const int prim = it->data(0, Qt::UserRole).toInt();
                    if (alt)                          it->setCheckState(0, Qt::Checked);
                    else if (shift)                   it->setCheckState(0, sel.contains(prim)
                                                          ? Qt::Checked : Qt::Unchecked);
                    else if (sel.contains(prim))      it->setCheckState(0, Qt::Unchecked);
                }
            }
            m_partTree->blockSignals(was);
            recomputePartVisibility();
            return true;
        }
    }

    // Keep the viewport's floating children pinned to its right edge on resize.
    if (t == QEvent::Resize && m_view && obj == m_view) {
        if (m_vpStrip) {
            m_vpStrip->adjustSize();
            m_vpStrip->move(m_view->width() - m_vpStrip->width() - 6, 104);
            m_vpStrip->raise();
        }
        if (m_fsExitBtn && m_fsExitBtn->isVisible()) {   // maximized: keep Exit reachable
            m_fsExitBtn->move(m_view->width() - m_fsExitBtn->width() - 8, 8);
            m_fsExitBtn->raise();
        }
    }

    // ── Snap-to-slot-on-hover: frame a slot while hovering its cell, and STAY there (the
    // camera holds the last-hovered slot rather than snapping back). Keeps the current angle. ──
    if (m_hoverSnap && m_view && t == QEvent::Enter) {
        for (int i = 0; i < kSlotCount; ++i) {
            if (obj != m_slotCells[i]) continue;
            QVector<int> parts;
            for (int p = 0; p < m_partSlot.size(); ++p) if (m_partSlot[p] == i) parts << p;
            QVector3D ctr; float rad;
            if (m_view->partsBounds(parts, ctr, rad)) {
                m_view->frameRegionKeepRotation(ctr, rad * (1.0f + m_snapMargin), /*animate=*/true);
                // If "Follow animation" is on, retarget the follow to the hovered slot — otherwise
                // the active-slot follow keeps yanking the camera back and the hover won't stick.
                m_view->followParts(m_camFollow ? parts : QVector<int>{});
            }
            break;
        }
    }

    // ── Arrow keys navigate + apply within card grids (pigments + creator) ──
    if (t == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(ev)->key();
        if (obj == m_lookScroll && m_pigmentMode && navGrid(m_lookScroll, m_lookLayout, m_lookCols, key))
            return true;
        if (obj == m_lookScroll && !m_pigmentMode && navLookGrid(key))   // looks: move cursor, Enter equips
            return true;
        if (obj == m_creatorScroll && navGrid(m_creatorScroll, m_creatorLayout, m_creatorCols, key))
            return true;
    }

    // ── Pigment zone slots: drag a colour onto another zone, right-click to reset ──
    for (int r = 0; r < 4; ++r) {
        if (obj != m_dyeRegionBtn[r]) continue;
        if (t == QEvent::MouseButtonPress) {
            m_dyeDragStart = static_cast<QMouseEvent*>(ev)->pos();
        } else if (t == QEvent::MouseMove) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            if ((me->buttons() & Qt::LeftButton)
                && (me->pos() - m_dyeDragStart).manhattanLength() > QApplication::startDragDistance()) {
                const QColor c(slotHex(m_activeSlot).value(r, QStringLiteral("#ffffff")));
                auto* mime = new QMimeData; mime->setColorData(c);
                auto* drag = new QDrag(m_dyeRegionBtn[r]); drag->setMimeData(mime);
                QPixmap pm(18, 18); pm.fill(c); drag->setPixmap(pm);
                drag->exec(Qt::CopyAction);
                return true;
            }
        } else if (t == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(ev);
            if (de->mimeData()->hasColor()) { de->acceptProposedAction(); return true; }
        } else if (t == QEvent::Drop) {
            auto* de = static_cast<QDropEvent*>(ev);
            const QColor c = qvariant_cast<QColor>(de->mimeData()->colorData());
            if (c.isValid()) { setDyeSlotColor(r, c); de->acceptProposedAction(); }
            return true;
        } else if (t == QEvent::ContextMenu) {
            QMenu menu(this);
            menu.addAction(QStringLiteral("Reset to white"), this,
                           [this, r]() { setDyeSlotColor(r, QColor(Qt::white)); });
            menu.exec(static_cast<QContextMenuEvent*>(ev)->globalPos());
            return true;
        }
        return BrowserTab::eventFilter(obj, ev);
    }

    // ── Memory swatches: store dropped colours; drag them out; right-click clears ──
    for (int i = 0; i < 8; ++i) {
        if (obj != m_dyeMem[i]) continue;
        const QString key = QStringLiteral("wardrobe2/dyeMem%1").arg(i);
        auto setMem = [this, i, key](const QColor& c) {
            if (c.isValid()) {
                QSettings().setValue(key, c.name());
                m_dyeMem[i]->setStyleSheet(
                    QStringLiteral("QToolButton{background:%1;border:1px solid #555;}").arg(c.name()));
            } else {
                QSettings().remove(key);
                m_dyeMem[i]->setStyleSheet(
                    QStringLiteral("QToolButton{background:#2b2b2b;border:1px dashed #555;}"));
            }
        };
        if (t == QEvent::MouseButtonPress) {
            m_dyeDragStart = static_cast<QMouseEvent*>(ev)->pos();
        } else if (t == QEvent::MouseMove) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            const QString mc = QSettings().value(key).toString();
            if (!mc.isEmpty() && (me->buttons() & Qt::LeftButton)
                && (me->pos() - m_dyeDragStart).manhattanLength() > QApplication::startDragDistance()) {
                const QColor c(mc);
                auto* mime = new QMimeData; mime->setColorData(c);
                auto* drag = new QDrag(m_dyeMem[i]); drag->setMimeData(mime);
                QPixmap pm(18, 18); pm.fill(c); drag->setPixmap(pm);
                drag->exec(Qt::CopyAction);
                return true;
            }
        } else if (t == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(ev);
            if (de->mimeData()->hasColor()) { de->acceptProposedAction(); return true; }
        } else if (t == QEvent::Drop) {
            auto* de = static_cast<QDropEvent*>(ev);
            setMem(qvariant_cast<QColor>(de->mimeData()->colorData()));
            de->acceptProposedAction();
            return true;
        } else if (t == QEvent::ContextMenu) {
            QMenu menu(this);
            menu.addAction(QStringLiteral("Clear"), this, [setMem]() { setMem(QColor()); });
            menu.exec(static_cast<QContextMenuEvent*>(ev)->globalPos());
            return true;
        }
        return BrowserTab::eventFilter(obj, ev);
    }

    // Reflow the icon browsers when the splitter resizes their width: recompute the column
    // count and refill only when it changes (cheap — no per-pixel rebuild of hundreds of cards).
    if (m_lookScroll && obj == m_lookScroll->viewport() && t == QEvent::Resize) {
        if (m_pigmentMode) {   // dense pigment swatches use their own column metric
            const int pc = qMax(1, (m_lookScroll->viewport()->width() + 3) / (kPigSwatch + 3));
            if (pc != m_lookCols && m_lookLayout) fillPigmentGrid();
            return BrowserTab::eventFilter(obj, ev);
        }
        int cols, cw, ch, iw; cardMetrics(m_lookScroll->viewport()->width(), cols, cw, ch, iw);
        if (cols != m_lookCols && m_lookLayout) {
            // Re-flow at the new column count WITHOUT losing the user's place: remember how many
            // cards were already lazily built and the relative scroll position, rebuild, then
            // re-append up to the same count and restore the scroll fraction once layout settles.
            const int builtBefore = m_lookBuildPos;
            QScrollBar* sb = m_lookScroll->verticalScrollBar();
            const double frac = (sb && sb->maximum() > 0) ? double(sb->value()) / sb->maximum() : 0.0;
            fillLookGrid();
            if (builtBefore > m_lookBuildPos) appendLookCards(builtBefore - m_lookBuildPos);
            QTimer::singleShot(0, this, [this, frac] {
                if (!m_lookScroll) return;
                QScrollBar* s = m_lookScroll->verticalScrollBar();
                if (s) s->setValue(int(frac * s->maximum()));
            });
        }
        return BrowserTab::eventFilter(obj, ev);
    }
    if (m_creatorScroll && obj == m_creatorScroll->viewport() && t == QEvent::Resize) {
        const bool colorCat = (m_activeCreator == 2 || m_activeCreator == 3 || m_activeCreator == 7);
        int cols;
        if (colorCat) cols = qMax(1, (m_creatorScroll->viewport()->width() + 3) / (kPigSwatch + 3));
        else { int cw, ch, iw; cardMetrics(m_creatorScroll->viewport()->width(), cols, cw, ch, iw); }
        if (cols != m_creatorCols && m_creatorLayout) fillCreatorGrid();
        return BrowserTab::eventFilter(obj, ev);
    }
    // Channel-thumbnail hover → 0.5s-delayed zoomed popup; wheel resizes it.
    for (int c = 0; c < 6; ++c) {
        if (obj != m_chanLbl[c]) continue;
        if (t == QEvent::Enter) {
            m_hoverChan = c;
            if (m_chanCap[c]) m_chanCap[c]->hide();   // reveal the full tile on hover
            if (!m_chanImg[c].isNull() && m_hoverTimer) m_hoverTimer->start();
        } else if (t == QEvent::Leave) {
            m_hoverChan = -1;
            if (m_chanCap[c]) m_chanCap[c]->show();
            if (m_hoverTimer) m_hoverTimer->stop();
            if (m_chanPreview) m_chanPreview->hide();
        } else if (t == QEvent::Wheel && m_chanPreview && m_chanPreview->isVisible()) {
            const auto* we = static_cast<QWheelEvent*>(ev);
            m_previewPx = qBound(64, m_previewPx + (we->angleDelta().y() > 0 ? 24 : -24), 512);
            showChanPreview(c);
            return true;   // consume: scroll resizes the popup
        }
        return BrowserTab::eventFilter(obj, ev);
    }
    if (m_partTree && obj == m_partTree && t == QEvent::KeyPress
        && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_Escape) {
        m_partTree->clearSelection();
        return true;
    }
    if (m_partTree && obj == m_partTree->viewport()) {
        if (t == QEvent::Leave) {
            if (m_view) m_view->setHighlightParts(selectedParts());
        } else if (t == QEvent::MouseButtonPress) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            const QPoint p = me->position().toPoint();
            QTreeWidgetItem* it = m_partTree->itemAt(p);
            if (!it) {
                m_partTree->clearSelection();
            } else if (me->button() == Qt::LeftButton && me->modifiers() == Qt::NoModifier
                       && it->isSelected()) {
                const QRect r = m_partTree->visualItemRect(it);
                if (p.x() > r.left() + 24) { it->setSelected(false); return true; }  // re-click → deselect
            }
        }
    }
    return BrowserTab::eventFilter(obj, ev);
}

// Single source of truth for a view-debug flag (skeleton / phys bones / axis / bone names /
// translated). Writes the setting, applies it to the viewport, and mirrors EVERY duplicate control
// (the centre Skeleton button, the Physics-panel checkboxes, the Rig-panel checkboxes) with signals
// blocked — so toggling the same flag from any location stays in sync and never re-enters.
void WardrobeTab2::applyRigToggle(const QString& key, bool on)
{
    QSettings().setValue(key, on);
    if (m_view) {
        if      (key == QLatin1String("wardrobe2/view/skeleton"))            m_view->setShowSkeleton(on);
        else if (key == QLatin1String("wardrobe2/cloth/showPhysBones"))      m_view->setShowPhysBones(on);
        else if (key == QLatin1String("wardrobe2/cloth/showPhysAxes"))       m_view->setShowPhysAxes(on);
        else if (key == QLatin1String("wardrobe2/rig/boneNames"))            m_view->setShowBoneNames(on);
        else if (key == QLatin1String("wardrobe2/rig/boneNamesTranslated"))  m_view->setBoneNamesTranslated(on);
        else if (key == QLatin1String("wardrobe2/rig/boneNamesHideUnknown")) m_view->setBoneNamesHideUnknown(on);
    }
    auto mirror = [on](QAbstractButton* b) {
        if (!b || b->isChecked() == on) return;                 // early-out also prevents re-entry
        const bool was = b->blockSignals(true); b->setChecked(on); b->blockSignals(was);
    };
    if      (key == QLatin1String("wardrobe2/view/skeleton"))            { mirror(m_skelToggleBtn); mirror(m_rigChkSkel); }
    else if (key == QLatin1String("wardrobe2/cloth/showPhysBones"))      { mirror(m_physChkBones); mirror(m_rigChkPhys); }
    else if (key == QLatin1String("wardrobe2/cloth/showPhysAxes"))       { mirror(m_physChkAxis);  mirror(m_rigChkAxis); }
    else if (key == QLatin1String("wardrobe2/rig/boneNames"))            { mirror(m_rigChkNames); }
    else if (key == QLatin1String("wardrobe2/rig/boneNamesTranslated"))  { mirror(m_rigChkTrans); }
    else if (key == QLatin1String("wardrobe2/rig/boneNamesHideUnknown")) { mirror(m_rigChkHideUnk); }
}

// Export-menu hook: the anim-library export is offered as the menu's contextual "anim export"
// action (labelled "Export animation library (.glb)…"), enabled once a character is assembled.
bool WardrobeTab2::hasAnimExport() const { return hasExportSelection(); }
void WardrobeTab2::exportAnimations()    { exportAnimLibrary(); }

// Export the RIG + selected animations only (no mesh) — a "clip library" you append onto
// an already-imported character in Blender. Temporarily forces the includeAnim scope so
// collectExportAnims() gathers clips even if "Include animation" is unticked.
void WardrobeTab2::exportAnimLibrary(bool toLast)
{
    if (m_lastMerged.skeleton.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Animation library"),
            QStringLiteral("Assemble a character first (the rig comes from the equipped body)."));
        return;
    }
    QVector<AnimParser::DecodedAnim> anims; QStringList names;
    {   // gather clips regardless of the "Include animation" checkbox
        QSettings s;
        const QVariant saved = s.value(QStringLiteral("export/includeAnim"));
        s.setValue(QStringLiteral("export/includeAnim"), true);
        collectExportAnims(anims, names);
        if (saved.isValid()) s.setValue(QStringLiteral("export/includeAnim"), saved);
        else                 s.remove(QStringLiteral("export/includeAnim"));
    }
    if (anims.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Animation library"),
            QStringLiteral("No animations selected. Ctrl/Shift-select one or more clips in the "
                           "ANIMATIONS list (or play one), then export."));
        return;
    }
    const QString dir = QSettings().value(QStringLiteral("wardrobe2/exportDir"), QDir::homePath()).toString();
    const QString suggested = dir + QStringLiteral("/") + classPrefix() + QStringLiteral("_anims.glb");
    QString path;
    if (toLast) {
        const QString last = QSettings().value(QStringLiteral("wardrobe2/exportDir")).toString();
        if (last.isEmpty()) { exportAnimLibrary(false); return; }   // nothing remembered → prompt
        path = QDir(last).filePath(classPrefix() + QStringLiteral("_anims.glb"));
    } else {
        path = QFileDialog::getSaveFileName(this, QStringLiteral("Export animation library"),
                                            suggested, QStringLiteral("glTF Binary (*.glb)"));
    }
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".glb"), Qt::CaseInsensitive)) path += QStringLiteral(".glb");

    ModelGeometry geo;                       // rig only — no primitives
    geo.valid = true;
    geo.skeleton = m_lastMerged.skeleton;
    geo.nBaseBones = m_lastMerged.nBaseBones;
    const ModelExporter::Options opt = ModelExporter::optionsFromSettings();
    if (QSettings().value(QStringLiteral("export/hardpointEmpties"), false).toBool())
        Hardpoints::readInto(geo, baseAppJsonPath());
    // No retarget on a clip library: remap/collapse would drop the very bones the clips drive.
    if (opt.blenderFriendly)
        GLModelWidget::blenderizeSkeletonNames(geo.skeleton);
    else if (QSettings().value(QStringLiteral("export/boneNamesTranslated"), false).toBool())
        GLModelWidget::translateSkeletonNames(geo.skeleton);
    Hardpoints::resolveBoneIndices(geo);
    const bool ok = ModelExporter::exportGlb(geo, path, {}, anims, names, opt);
    const QString folder = QFileInfo(path).absolutePath();
    QSettings().setValue(QStringLiteral("wardrobe2/exportDir"), folder);
    if (ok)
        ExportNotifier::instance().notify(
            QStringLiteral("Exported %1 animation clip(s), rig only").arg(anims.size()), folder);
    else
        QMessageBox::warning(this, QStringLiteral("Animation library"), QStringLiteral("Export failed."));
}

// Shared model-export helpers (defined in ModelsTab.cpp, external linkage) — reused so a single
// wardrobe item exports with the same material/texture pipeline the Models tab uses.
QStringList appearancePalette(const QString& d4, const QString& name);
QVector<ModelExporter::ExportMaterial> buildExportMats(
    const QStringList& palette, const ModelGeometry& geo, const QString& modelName,
    const QString& d4, CascReader* reader, bool wantTex);

// Right-click "Export model" on a look-grid item card → export just that one appearance's model
// (mesh + textured materials) to a .glb, independent of the assembled outfit. toLast → remembered dir.
void WardrobeTab2::exportItemModel(int sno, const QString& name, bool toLast)
{
    if (!m_reader || !m_reader->isReady() || sno <= 0) return;
    const QByteArray meta    = m_reader->readMetaBySno(quint64(sno));
    const QByteArray payload = m_reader->readPayloadBySno(quint64(sno));
    if (meta.isEmpty() || payload.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Export model"),
                             QStringLiteral("That item has no readable model payload."));
        return;
    }
    ModelGeometry geo = ModelParser::parseApp(meta, payload, name);
    if (!geo.valid || geo.primitives.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Export model"),
                             QStringLiteral("That item has no exportable geometry."));
        return;
    }
    const QString d4   = Config::d4dataDir();
    const QString stem = (name.isEmpty() ? QStringLiteral("model") : name) + QStringLiteral(".glb");
    QString path;
    if (toLast) {
        const QString last = QSettings().value(QStringLiteral("wardrobe2/exportDir")).toString();
        if (last.isEmpty()) { exportItemModel(sno, name, false); return; }   // nothing remembered → prompt
        path = QDir(last).filePath(stem);
    } else {
        const QString dir = QSettings().value(QStringLiteral("wardrobe2/exportDir"), QDir::homePath()).toString();
        path = QFileDialog::getSaveFileName(this, QStringLiteral("Export model"),
                                            QDir(dir).filePath(stem), QStringLiteral("glTF Binary (*.glb)"));
    }
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".glb"), Qt::CaseInsensitive)) path += QStringLiteral(".glb");

    const bool wantTex = QSettings().value(QStringLiteral("export/includeTex"), true).toBool();
    const QStringList palette = appearancePalette(d4, name);
    QVector<ModelExporter::ExportMaterial> mats = buildExportMats(palette, geo, name, d4, m_reader, wantTex);
    ModelExporter::Options opt = ModelExporter::optionsFromSettings();
    if (QSettings().value(QStringLiteral("export/hardpointEmpties"), false).toBool())
        Hardpoints::readInto(geo, QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4, name));
    if (opt.blenderFriendly)
        GLModelWidget::blenderizeSkeletonNames(geo.skeleton);
    else if (QSettings().value(QStringLiteral("export/boneNamesTranslated"), false).toBool())
        GLModelWidget::translateSkeletonNames(geo.skeleton);
    Hardpoints::resolveBoneIndices(geo);
    const bool ok = ModelExporter::exportGlb(geo, path, mats, {}, {}, opt);
    const QString folder = QFileInfo(path).absolutePath();
    QSettings().setValue(QStringLiteral("wardrobe2/exportDir"), folder);
    if (ok) ExportNotifier::instance().notify(QStringLiteral("Exported %1").arg(QFileInfo(path).fileName()), folder);
    else    QMessageBox::warning(this, QStringLiteral("Export model"), QStringLiteral("Export failed."));
}

// Fill the nine creator pickers from real game data, filtered to this class/gender,
// restoring the saved selection per category.
void WardrobeTab2::populateCreator()
{
    const QString d4 = Config::d4dataDir();
    const int fubc = kClasses[qBound(0, m_class ? m_class->currentIndex() : 0, kNumClasses - 1)].fubc;
    const bool male = m_gender && m_gender->currentData().toString() == QLatin1String("m");
    for (int i = 0; i < 9; ++i) {
        if (!m_creator[i]) continue;
        QSignalBlocker block(m_creator[i]);
        const QString saved = QSettings().value(QStringLiteral("wardrobe2/creator/%1").arg(i)).toString();
        m_creator[i]->clear();
        m_creator[i]->addItem(QStringLiteral("(default)"), QString());
        for (const QString& stem : creatorEntries(d4, kCreator[i], fubc, male)) {
            // Show the game's REAL localized name + the file stem, e.g. "Caldean Rouge  (05)".
            // (Facial-hair names are gender-correct.) Fall back to the humanised stem if the game
            // ships no string for this def — never a fabricated name.
            const QString real = creatorRealName(d4, kCreator[i].folder, stem, male);
            const QString disp = real.isEmpty() ? prettyCreatorName(stem)
                                                : QStringLiteral("%1  (%2)").arg(real, stem);
            m_creator[i]->addItem(disp, stem);
        }
        // Restore by stem (item data), robust to the display text differing from the stem.
        const int idx = saved.isEmpty() ? 0 : m_creator[i]->findData(saved);
        m_creator[i]->setCurrentIndex(idx > 0 ? idx : 0);
    }
    if (m_creatorLayout) { fillCreatorGrid(); refreshCreatorCells(); }   // refill the Appearance cards
}

namespace {
// True if a weapon ItemType is usable by the class at fUsableByClass index `fubc`.
// (Cached: the ~20 ItemType files are read at most once each.)
bool weapTypeUsable(const QString& d4, const QString& itemType, int fubc)
{
    static QHash<QString, std::array<int, 8>> cache;
    auto it = cache.constFind(itemType);
    if (it == cache.constEnd()) {
        std::array<int, 8> arr{{1,1,1,1,1,1,1,1}};
        QFile f(d4 + QStringLiteral("/json/base/meta/ItemType/") + itemType + QStringLiteral(".itt.json"));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonArray uc = QJsonDocument::fromJson(f.readAll()).object()
                                      .value(QStringLiteral("fUsableByClass")).toArray();
            if (uc.size() == 8) for (int i = 0; i < 8; ++i) arr[i] = uc[i].toInt();
        }
        it = cache.insert(itemType, arr);
    }
    return fubc < 0 || fubc >= 8 || it.value()[fubc] != 0;
}
}  // namespace

// Enumerate weapon model appearances straight from the SNO index, grouped by the
// real weapon-type name prefix (data-driven — no Item scan). Fast and in-memory.
void WardrobeTab2::ensureWeaponIndex()
{
    if (m_weapReady || !m_index) return;
    m_weapByType.clear();
    for (const SnoEntry& e : m_index->entries(kGroupAppearance)) {
        const QString lower = e.name.toLower();
        for (const WeapTypeDef& w : kWeapTypes) {
            const QString pfx = QString::fromLatin1(w.prefix).toLower() + QLatin1Char('_');
            if (lower.startsWith(pfx)) { m_weapByType[QString::fromLatin1(w.prefix)].insert(e.name, e.snoId); break; }
        }
    }
    m_weapReady = true;
    m_weapBuilding = false;
    populateWeapons();
}

void WardrobeTab2::populateWeapons()
{
    if (!m_weaponType || !m_weapon || !m_weaponType2 || !m_weapon2) return;
    const QString d4 = Config::d4dataDir();
    // "Class restricted" (Weapon Settings dropdown, default ON): filter weapon TYPES by the class's
    // authored fUsableByClass (the game's own class↔weapon table). OFF ⇒ fubc -1 ⇒ every type lists,
    // so any class can preview any weapon (seating still uses the class's own hardpoints).
    const bool classRestrict = QSettings().value(QStringLiteral("wardrobe2/weap/classRestrict"), true).toBool();
    m_lastClassRestrict = classRestrict;   // onSettingsLiveChanged diffs against this
    const int fubc = classRestrict
        ? kClasses[qBound(0, m_class ? m_class->currentIndex() : 0, kNumClasses - 1)].fubc : -1;

    // Flatten ALL class-usable, hand-eligible weapons into the model combo, grouped by
    // type (so the icon browser can draw category dividers). `offHand` picks the
    // off-hand-eligible set (1H + shields/focus/totems). Each item stores:
    //   data            = appearance SNO
    //   UserRole+1      = full appearance name
    //   UserRole+2      = weapon-type label (for dividers + hardpoint seating)
    auto fillHand = [&](QComboBox* typeCb, QComboBox* modelCb, bool offHand,
                        const QString& typeKey, const QString& modelKey) {
        Q_UNUSED(typeKey);
        if (!modelCb) return;
        const QString savedModel = QSettings().value(modelKey).toString();
        if (typeCb) { QSignalBlocker bt(typeCb); typeCb->clear(); }   // type combo retired (hidden)
        QSignalBlocker b(modelCb);
        modelCb->clear();
        modelCb->addItem(QStringLiteral("(none)"), 0);
        for (const WeapTypeDef& w : kWeapTypes) {
            if (offHand ? w.twoHand : w.offHandOnly) continue;      // hand eligibility
            const QString pfx = QString::fromLatin1(w.prefix);
            const QMap<QString, int>& models = m_weapByType.value(pfx);
            if (models.isEmpty()) continue;                         // no models in data
            if (!weapTypeUsable(d4, QString::fromLatin1(w.itemType), fubc)) continue;
            const QString strip = pfx + QLatin1Char('_');
            const QString label = QString::fromLatin1(w.label);
            for (auto it = models.constBegin(); it != models.constEnd(); ++it) {
                QString disp = it.key();
                if (disp.startsWith(strip, Qt::CaseInsensitive)) disp = disp.mid(strip.size());
                modelCb->addItem(disp, it.value());                            // data = SNO
                const int idx = modelCb->count() - 1;
                modelCb->setItemData(idx, it.key(), Qt::UserRole + 1);         // full appearance name
                modelCb->setItemData(idx, label,    Qt::UserRole + 2);         // weapon-type label
            }
        }
        const int wi = savedModel.isEmpty() ? 0 : modelCb->findText(savedModel);
        if (wi > 0) modelCb->setCurrentIndex(wi);
    };

    fillHand(m_weaponType, m_weapon, /*offHand=*/false,
             QStringLiteral("wardrobe2/weaponType"), QStringLiteral("wardrobe2/weapon"));
    fillHand(m_weaponType2, m_weapon2, /*offHand=*/true,
             QStringLiteral("wardrobe2/weaponType2"), QStringLiteral("wardrobe2/weapon2"));
    // Sheathed slots draw from the same class-usable weapon pool as the held hands
    // (main-hand eligibility for the sheathed main, off-hand for the sheathed off).
    fillHand(nullptr, m_weapon3, /*offHand=*/false,
             QString(), QStringLiteral("wardrobe2/weaponSheath"));
    fillHand(nullptr, m_weapon4, /*offHand=*/true,
             QString(), QStringLiteral("wardrobe2/weaponSheath2"));
    updateWeaponSlotAvailability();
    if (!m_restoring) rebuildOutfit();
    if (m_lookLayout) { fillLookGrid(); refreshSlotCells(); }   // refresh the weapon slot cell + grid
}

// Grey out weapon slots this class can't fill. Purely DATA-DRIVEN: a hand's combo is populated
// from the class's authored fUsableByClass table (see fillHand), so an empty list means the game
// gives this class nothing for that hand — e.g. classes that can't dual-wield end up with no
// off-hand-eligible weapon and the Off / Sheath 2 cells grey out. With "Class-restricted weapons"
// switched off every list fills, so every slot re-enables.
void WardrobeTab2::updateWeaponSlotAvailability()
{
    auto gate = [this](int slot, QComboBox* cb, const QString& why) {
        if (slot < 0 || slot >= kSlotCount || !m_slotCells[slot] || !cb) return;
        const bool usable = cb->count() > 1;   // >1 ⇒ something beyond "(none)"
        m_slotCells[slot]->setEnabled(usable);
        m_slotCells[slot]->setToolTip(usable ? QString() : why);
        if (!usable && cb->currentIndex() != 0) {   // clear a stale pick from a previous class
            QSignalBlocker b(cb);
            cb->setCurrentIndex(0);
        }
        if (!usable && m_activeSlot == slot) selectSlot(0);   // don't sit on a disabled slot
    };
    const QString why = QStringLiteral("This class has no weapon for that hand.\nTurn off "
                                       "\"Class-restricted weapons\" (Settings ▸ Wardrobe) to browse anyway.");
    gate(6, m_weapon2, why);   // Off hand
    gate(8, m_weapon4, why);   // Sheath 2 (off-hand sheath)
    gate(5, m_weapon,  why);   // Main hand (defensive — normally always populated)
    gate(7, m_weapon3, why);   // Sheath (main)
}

void WardrobeTab2::rebuildDyeCombo()
{
    if (!m_dyeCombo) return;
    QSignalBlocker block(m_dyeCombo);
    const QString cur = QSettings().value(QStringLiteral("wardrobe2/dyeSel"),
                                          QStringLiteral("None")).toString();
    m_dyeCombo->clear();
    m_dyeCombo->addItem(QStringLiteral("None (undyed)"));
    for (const DyeDef& dd : loadPlayerDyes(Config::d4dataDir())) {
        QStringList hex; for (int k = 0; k < 4; ++k) hex << dd.colors[k].name();
        m_dyeCombo->addItem(pigmentIcon(dd.colors), dd.name, hex);
    }
    const int idx = m_dyeCombo->findText(cur);
    m_dyeCombo->setCurrentIndex(idx > 0 ? idx : 0);
}

// The bottom dye combo changed — recompute pigments. (Kept as the slot for the existing
// signal connection; the real work is per-slot in applyAllDyes.)
void WardrobeTab2::applyDye() { applyAllDyes(); }

// Compute the effective pigment for every drawn primitive and push it to the view. Each slot
// can carry its own pigment (Set Pigment); slots without one fall back to the global dye combo.
// No geometry rebuild — this only updates the per-part dye uniforms.
void WardrobeTab2::applyAllDyes()
{
    if (!m_view) return;
    QStringList globalHex;
    if (m_dyeCombo && m_dyeCombo->currentIndex() > 0)
        globalHex = m_dyeCombo->currentData().toStringList();

    const int n = m_partSlot.size();
    if (n == 0) {
        // No slot map yet (e.g. before the first rebuild) — fall back to the simple global path.
        if (globalHex.size() == 4) {
            for (int k = 0; k < 4; ++k) m_view->setDyeColor(k, QColor(globalHex[k]));
            m_view->setFeatureDye(true);
        } else {
            m_view->setFeatureDye(false);
        }
        m_view->setPartDye({}, {});
        m_view->update();
        return;
    }
    QVector<int> on(n, 0);
    QVector<float> col(n * 12, 1.0f);
    for (int i = 0; i < n; ++i) {
        const int slot = m_partSlot[i];
        if (slot >= 5) continue;   // weapons (Main/Off) can't be dyed
        const QStringList hex = (slot >= 0 && slot < kSlotCount && m_slotDye[slot].hex.size() == 4)
                                    ? m_slotDye[slot].hex : globalHex;
        if (hex.size() != 4) continue;
        on[i] = 1;
        for (int k = 0; k < 4; ++k) {
            const QColor c(hex[k]);
            col[i * 12 + k * 3 + 0] = float(c.redF());
            col[i * 12 + k * 3 + 1] = float(c.greenF());
            col[i * 12 + k * 3 + 2] = float(c.blueF());
        }
    }
    m_view->setPartDye(on, col);
    m_view->update();
}

// Real game attach data: the base body appearance (<pref>_base00) carries the
// skeleton's hardpoints — each maps a hardpoint hash → (bone index, bone-local TRS).
// Returned in native (z-up) space; the y-up swap is applied at seating time.
// Path to the current class/gender base body's .app.json (case-resolved via the index).
QString WardrobeTab2::baseAppJsonPath() const
{
    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty()) return QString();
    const QString want = classPrefix() + QStringLiteral("_base00");
    QString stem = want;
    if (m_index)
        for (const SnoEntry& e : m_index->entries(kGroupAppearance))
            if (e.name.compare(want, Qt::CaseInsensitive) == 0) { stem = e.name; break; }
    return d4 + QStringLiteral("/json/base/meta/Appearance/") + stem + QStringLiteral(".app.json");
}

QHash<quint32, QPair<int, std::array<float, 16>>> WardrobeTab2::loadBodyHardpoints(const QString& d4)
{
    QHash<quint32, QPair<int, std::array<float, 16>>> out;
    if (d4.isEmpty()) return out;
    QFile f(baseAppJsonPath());
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonArray bd = o.value(QStringLiteral("tStructure")).toObject()
                              .value(QStringLiteral("ptBoneData")).toArray();
    if (bd.isEmpty()) return out;
    const QJsonArray hps = bd.at(0).toObject().value(QStringLiteral("ptHardpoints")).toArray();
    for (const QJsonValue& hv : hps) {
        const QJsonObject h = hv.toObject();
        const int bone = h.value(QStringLiteral("nBoneIndex")).toInt(-1);
        if (bone < 0) continue;
        const quint32 hash = quint32(h.value(QStringLiteral("tInfo")).toObject()
                                         .value(QStringLiteral("dwHash")).toVariant().toULongLong());
        if (!hash) continue;
        const QJsonObject t  = h.value(QStringLiteral("transform")).toObject();
        const QJsonObject q  = t.value(QStringLiteral("q")).toObject();
        const QJsonObject wp = t.value(QStringLiteral("wp")).toObject();
        const std::array<float,4> qa{{ float(q.value(QStringLiteral("x")).toDouble()),
                                       float(q.value(QStringLiteral("y")).toDouble()),
                                       float(q.value(QStringLiteral("z")).toDouble()),
                                       float(q.value(QStringLiteral("w")).toDouble(1.0)) }};
        const std::array<float,3> pa{{ float(wp.value(QStringLiteral("x")).toDouble()),
                                       float(wp.value(QStringLiteral("y")).toDouble()),
                                       float(wp.value(QStringLiteral("z")).toDouble()) }};
        out.insert(hash, qMakePair(bone, quatPosMat(qa, pa)));
    }
    return out;
}

// Body sheath socket for a STOWED weapon, chosen by its ItemType. Returns the first candidate
// hardpoint the current body rig actually exposes (classes differ — e.g. only Barbarian carries
// HP_2hSwordSheath / HP_poleSheath), falling back to a generic back socket. `offHand` picks the
// mirror hip for 1H weapons. Returns 0 when the rig has no suitable socket. Hashes: see Hardpoints.cpp.
static quint32 sheathHardpointFor(const QString& itemType, bool offHand,
                                  const QHash<quint32, QPair<int, std::array<float,16>>>& hp)
{
    constexpr quint32 kLeftHip = 543141696u, kRightHip = 924169363u, kSheath = 1410062708u,
                      kLeftBack = 585752080u, kRightBack = 274763203u, k2hSword = 1803764157u,
                      kPole = 2338619876u, kStaff = 3092546280u, kShield = 1327944173u,
                      kQuiver = 1347375027u, kChestBack = 899481535u, kScythe2h = 390646174u;
    auto is = [&](const char* s) { return itemType.compare(QLatin1String(s), Qt::CaseInsensitive) == 0; };
    QVector<quint32> order;
    if (is("Shield"))                                          order << kShield << kLeftBack << kChestBack;
    else if (is("Sword2H"))                                    order << k2hSword << kRightBack << kChestBack << kLeftBack;
    else if (is("Axe2H") || is("Mace2H"))                      order << kRightBack << kLeftBack << kChestBack;
    else if (is("Scythe2H"))                                   order << kScythe2h << kRightBack << kChestBack;
    else if (is("Polearm") || is("Quarterstaff") || is("Glaive"))
                                                               order << kPole << kRightBack << kChestBack;
    else if (is("Staff") || is("StaffSorcerer") || is("StaffDruid"))
                                                               order << kStaff << kRightBack << kChestBack;
    else if (is("Bow") || is("Crossbow"))                      order << kQuiver << kRightBack << kChestBack;
    else if (is("Focus") || is("OffHandTotem"))                order << (offHand ? kRightHip : kLeftHip) << kSheath << kLeftHip;
    else /* 1H sword/axe/mace/dagger/scythe/wand/flail */      order << (offHand ? kRightHip : kLeftHip) << kSheath
                                                                     << (offHand ? kLeftHip : kRightHip) << kLeftBack;
    for (quint32 h : order) if (hp.contains(h)) return h;
    return 0;
}

// Seat one parsed weapon into a hand using real attach data: the weapon's ItemType
// names the hardpoint (dwHash) + a grip offset; the body hardpoint maps that hash to
// a bone + bone-local transform. Final world = boneWorld · T_hp · T_off, re-expressed
// in mesh (y-up) space, then applied to the weapon's vertices and normals.
// forceHash != 0 seats rigidly at that exact body socket (sheathed slots): no grip offset,
// no mirror, no held-roll.
void WardrobeTab2::seatWeapon(ModelGeometry& wgeo, int hand, const QString& itemType,
                             const QString& gender,
                             const QHash<quint32, QPair<int, std::array<float,16>>>& hp,
                             QString& dbg, quint32 forceHash)
{
    constexpr quint32 kMain = 3636304447u, kOff = 4036545548u;
    auto mirror = [](quint32 h) -> quint32 {
        if (h == 3636304447u) return 4036545548u;   // weapon_R → weapon_L
        if (h == 924169363u)  return 543141696u;    // flail pair
        return h;
    };
    const QString d4 = Config::d4dataDir();
    const char* lbl = hand == 0 ? "main" : "off";

    // ItemType grip offsets: rows of (entry index, hardpoint hash, offset matrix).
    struct Off { int entry; quint32 hash; std::array<float,16> m; };
    QVector<Off> offs;
    {
        QFile f(d4 + QStringLiteral("/json/base/meta/ItemType/") + itemType + QStringLiteral(".itt.json"));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            const QJsonArray hpo = o.value(QStringLiteral("tHardpointOffsets")).toArray();
            const QString wantKey  = gender == QStringLiteral("Female")
                                         ? QStringLiteral("arFemaleOffsets") : QStringLiteral("arMaleOffsets");
            const QString otherKey = gender == QStringLiteral("Female")
                                         ? QStringLiteral("arMaleOffsets") : QStringLiteral("arFemaleOffsets");
            for (int i = 0; i < hpo.size(); ++i) {
                const QJsonObject e = hpo.at(i).toObject();
                QJsonArray arr = e.value(wantKey).toArray();
                if (arr.isEmpty()) arr = e.value(otherKey).toArray();
                for (const QJsonValue& ov : arr) {
                    const QJsonObject oo = ov.toObject();
                    const quint32 h = quint32(oo.value(QStringLiteral("tHardpointLink")).toObject()
                                                  .value(QStringLiteral("tInfo")).toObject()
                                                  .value(QStringLiteral("dwHash")).toVariant().toULongLong());
                    if (!h) continue;
                    const QJsonObject q = oo.value(QStringLiteral("qRotationOffset")).toObject();
                    const QJsonObject v = oo.value(QStringLiteral("vecOffset")).toObject();
                    const std::array<float,4> qa{{ float(q.value(QStringLiteral("x")).toDouble()),
                                                   float(q.value(QStringLiteral("y")).toDouble()),
                                                   float(q.value(QStringLiteral("z")).toDouble()),
                                                   float(q.value(QStringLiteral("w")).toDouble(1.0)) }};
                    const std::array<float,3> pa{{ float(v.value(QStringLiteral("x")).toDouble()),
                                                   float(v.value(QStringLiteral("y")).toDouble()),
                                                   float(v.value(QStringLiteral("z")).toDouble()) }};
                    offs.push_back({i, h, quatPosMat(qa, pa)});
                }
            }
        }
    }

    // Pick the hardpoint hash + grip offset.
    quint32 hash = hand == 0 ? kMain : kOff;
    std::array<float,16> Toff{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    if (forceHash) hash = forceHash;   // sheathed: seat rigidly at the given socket, identity offset
    else {
        // In-hand: prefer the standard mount; off hand uses a dual-state row or mirror.
        QVector<Off> usable;
        for (const Off& o : offs) if (hp.contains(o.hash)) usable.push_back(o);
        if (!usable.isEmpty()) {
            const Off* main = nullptr;
            for (const Off& o : usable) if (o.hash == kMain) { main = &o; break; }
            if (!main) main = &usable.first();
            hash = main->hash; Toff = main->m;
            if (hand == 1) {
                const Off* dual = nullptr;
                for (const Off& o : usable) if (o.entry == main->entry && o.hash != main->hash) { dual = &o; break; }
                if (dual) { hash = dual->hash; Toff = dual->m; }
                else      { hash = mirror(main->hash); }
            }
        } else if (hand == 1) {
            hash = mirror(kMain);
        }
    }

    if (!hp.contains(hash)) {
        dbg += QStringLiteral("\n%1: no body hardpoint (hash %2) — left at origin").arg(QLatin1String(lbl)).arg(hash);
        return;
    }
    const QPair<int, std::array<float,16>> bh = hp.value(hash);
    const int bone = bh.first;
    if (bone < 0 || bone >= m_bodySkeleton.size()) {
        dbg += QStringLiteral("\n%1: bone %2 out of range (rig has %3)").arg(QLatin1String(lbl)).arg(bone).arg(m_bodySkeleton.size());
        return;
    }
    // Main-hand grip roll — DATA-CONDITIONAL (the non-Barbarian weapon-position fix).
    // Verified against d4data: Barbarian authors HP_rightWeapon IDENTITY-local (barM_base00:
    // q≈(0,0,0,-1)) and relies on a runtime 180° grip roll; other classes BAKE the roll into the
    // authored socket rotation (rogM_base00: q≈(-1,0,0,0) = 180° about X, different bone too).
    // The old code applied the Barbarian roll to every class ON TOP of the authored rotation —
    // double-correcting and mis-orienting every non-Barbarian main-hand weapon. Now the roll is
    // applied ONLY when the class's own socket rotation is (near-)identity, so each body's
    // authored transform is trusted exactly as the game data specifies it.
    if (!forceHash && hand == 0) {   // in-hand main-weapon mesh orientation fix
        const std::array<float,16>& s = bh.second;   // authored socket, column-major
        const bool socketIdentity = std::fabs(s[0]  - 1.0f) < 0.01f
                                 && std::fabs(s[5]  - 1.0f) < 0.01f
                                 && std::fabs(s[10] - 1.0f) < 0.01f;
        if (socketIdentity) {
            static const Mat4 kHeld{{ -1,0,0,0,  0,1,0,0,  0,0,-1,0,  0,0,0,1 }};   // 180 about Y
            Toff = mat4mul(Toff, kHeld);
        }
    }
    // ── User orientation overrides (Weapon Settings dropdown): per-hand flip (half turn about
    // the grip) and invert (upside down), applied on top of the data seating. In-hand only —
    // sheathed weapons keep their authored sheath orientation.
    if (!forceHash && (hand == 0 || hand == 1)) {
        QSettings us;
        const bool flip = us.value(hand == 0 ? QStringLiteral("wardrobe2/weap/flipMain")
                                             : QStringLiteral("wardrobe2/weap/flipOff"), false).toBool();
        const bool inv  = us.value(hand == 0 ? QStringLiteral("wardrobe2/weap/invMain")
                                             : QStringLiteral("wardrobe2/weap/invOff"),  false).toBool();
        if (flip) {
            static const Mat4 kFlipY{{ -1,0,0,0,  0,1,0,0,  0,0,-1,0,  0,0,0,1 }};   // 180 about Y
            Toff = mat4mul(Toff, kFlipY);
        }
        if (inv) {
            static const Mat4 kFlipX{{ 1,0,0,0,  0,-1,0,0,  0,0,-1,0,  0,0,0,1 }};   // 180 about X
            Toff = mat4mul(Toff, kFlipX);
        }
    }
    const Mat4 world = jointWorldMat(m_bodySkeleton, bone);            // z-up bone world
    const std::array<float,16> hpMat = bh.second;
    Mat4 Mz = mat4mul(mat4mul(world, hpMat), Toff);                    // z-up attach
    Mat4 M  = mat4mul(mat4mul(kSwapZtoY, Mz), kSwapYtoZ);              // → y-up mesh space

    // ── Auto-correct upside-down weapons (in-hand only) ─────────────────────────────────────────
    // WHY this is measured instead of tabled: each class authors its own weapon socket, and the
    // families genuinely differ (verified in d4data: bar/dru/nec/spi/pal author an IDENTITY
    // HP_rightWeapon; rog/sor/war author a 180°-about-X one, on different hand bones). Whether a
    // given class+weapon ends up inverted also depends on the hand bone's REST orientation, which
    // lives in the binary rig payload — so no static table can decide it. Instead we measure the
    // seated result geometrically and fix only what is actually wrong:
    //   1. the weapon's long axis (bbox) is the blade; weapons are authored grip-at-origin, so the
    //      blade points from the origin toward the far extreme of that axis;
    //   2. seat it, then look at where the blade points in world space;
    //   3. a held weapon whose blade points markedly DOWNWARD is upside down — rotate it a half
    //      turn about an axis perpendicular to the blade (which flips tip-up without spinning the
    //      grip), and re-seat.
    // Conservative threshold (blade more than ~30° below horizontal) so intentionally angled or
    // reversed-grip weapons are left alone. Toggle: Weapon settings ⚙ ▸ Auto-correct upside-down.
    if (!forceHash && (hand == 0 || hand == 1)
        && QSettings().value(QStringLiteral("wardrobe2/weap/autoUpright"), true).toBool()) {
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        for (const MeshPrimitive& p : wgeo.primitives)
            for (const MeshVertex& v : p.vertices) {
                const float c[3] = { v.px, v.py, v.pz };
                for (int k = 0; k < 3; ++k) { lo[k] = qMin(lo[k], c[k]); hi[k] = qMax(hi[k], c[k]); }
            }
        if (lo[0] < 1e29f) {
            int ax = 0;   // longest extent = the blade axis
            for (int k = 1; k < 3; ++k) if ((hi[k] - lo[k]) > (hi[ax] - lo[ax])) ax = k;
            float bladeLocal[3] = { 0, 0, 0 };
            bladeLocal[ax] = (std::fabs(hi[ax]) >= std::fabs(lo[ax])) ? 1.0f : -1.0f;   // grip at origin
            // Blade direction in world space (rotation part of M only).
            const float bx = M[0]*bladeLocal[0] + M[4]*bladeLocal[1] + M[8]*bladeLocal[2];
            const float by = M[1]*bladeLocal[0] + M[5]*bladeLocal[1] + M[9]*bladeLocal[2];
            const float bz = M[2]*bladeLocal[0] + M[6]*bladeLocal[1] + M[10]*bladeLocal[2];
            const float bl = std::sqrt(bx*bx + by*by + bz*bz);
            const float upDot = bl > 1e-6f ? (by / bl) : 0.0f;   // +Y is up in mesh space
            if (upDot < -0.5f) {
                // 180° about a unit axis perpendicular to the blade: R = 2·outer(v,v) − I.
                float pv[3] = { 0, 0, 0 };
                pv[(ax + 1) % 3] = 1.0f;
                Mat4 R{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
                for (int c = 0; c < 3; ++c)
                    for (int r = 0; r < 3; ++r)
                        R[c * 4 + r] = 2.0f * pv[c] * pv[r] - (c == r ? 1.0f : 0.0f);
                Toff = mat4mul(Toff, R);
                Mz = mat4mul(mat4mul(world, hpMat), Toff);
                M  = mat4mul(mat4mul(kSwapZtoY, Mz), kSwapYtoZ);
                dbg += QStringLiteral("\n%1: auto-uprighted (blade pointed down, up·blade=%2)")
                           .arg(QLatin1String(lbl)).arg(upDot, 0, 'f', 2);
                qInfo("wardrobe: auto-uprighted %s weapon (type %s, blade axis %d, up-dot %.2f)",
                      lbl, qPrintable(itemType), ax, double(upDot));
            }
        }
    }
    // Rigidly BIND the weapon to its attach bone: place verts at the rest-pose world
    // position (so static render is unchanged) AND weight them 100% to that bone, so
    // the shared skinning palette carries the weapon with the bone during animation
    // (the bone's inverseBind in the palette cancels the rest world). This is what ties
    // weapons to the hand bone when animating.
    for (MeshPrimitive& p : wgeo.primitives)
        for (MeshVertex& v : p.vertices) {
            const float x = v.px, y = v.py, z = v.pz;
            v.px = M[0]*x + M[4]*y + M[8]*z  + M[12];
            v.py = M[1]*x + M[5]*y + M[9]*z  + M[13];
            v.pz = M[2]*x + M[6]*y + M[10]*z + M[14];
            const float nx = v.nx, ny = v.ny, nz = v.nz;
            float rx = M[0]*nx + M[4]*ny + M[8]*nz;
            float ry = M[1]*nx + M[5]*ny + M[9]*nz;
            float rz = M[2]*nx + M[6]*ny + M[10]*nz;
            const float len = std::sqrt(rx*rx + ry*ry + rz*rz);
            if (len > 1e-8f) { rx /= len; ry /= len; rz /= len; }
            v.nx = rx; v.ny = ry; v.nz = rz;
            v.joints[0] = quint16(bone); v.joints[1] = v.joints[2] = v.joints[3] = 0;
            v.weights[0] = 1.0f; v.weights[1] = v.weights[2] = v.weights[3] = 0.0f;
        }
    // Drop the weapon's own rig: its verts now reference the BODY bone index directly,
    // so the merge must use those indices as-is (not remap them via the weapon skeleton).
    wgeo.skeleton.clear();
    dbg += QStringLiteral("\n%1: bone %2 hash %3 boneW=(%4,%5,%6) hpLocal=(%7,%8,%9) attachW=(%10,%11,%12)")
               .arg(QLatin1String(lbl)).arg(bone).arg(hash)
               .arg(world[12], 0, 'f', 2).arg(world[13], 0, 'f', 2).arg(world[14], 0, 'f', 2)
               .arg(bh.second[12], 0, 'f', 2).arg(bh.second[13], 0, 'f', 2).arg(bh.second[14], 0, 'f', 2)
               .arg(Mz[12], 0, 'f', 2).arg(Mz[13], 0, 'f', 2).arg(Mz[14], 0, 'f', 2);
}

void WardrobeTab2::populateSlots()
{
    if (!m_index || !m_index->isLoaded()) return;
    const QString prefix = classPrefix();
    const QVector<SnoEntry>& all = m_index->entries(kGroupAppearance);
    for (int i = 0; i < 5; ++i) {
        const QString suffix = QStringLiteral("_") + QString::fromLatin1(kSlots[i].code).toLower();
        QSignalBlocker block(m_slot[i]);
        m_slot[i]->clear();
        m_slot[i]->addItem(QStringLiteral("(none)"), 0);
        for (const SnoEntry& e : all) {
            const QString l = e.name.toLower();
            if (l.startsWith(prefix) && l.size() > prefix.size() && l[prefix.size()] == QLatin1Char('_')
                && l.endsWith(suffix))
                m_slot[i]->addItem(e.name, e.snoId);
        }
        // The bare/base bodies (nude test999 + base01/02/03 underwear) aren't regular cosmetic
        // armor and are often authored as whole-body appearances without the _trs slot suffix,
        // so the loop above misses them. Surface them in the Torso slot so they're pickable.
        if (i == 1) {
            for (const SnoEntry& e : all) {
                const QString l = e.name.toLower();
                if (!l.startsWith(prefix)) continue;
                if (!(l.contains(QLatin1String("_base0")) || l.contains(QLatin1String("_test999"))
                      || l.contains(QLatin1String("_nude")))) continue;
                if (l.endsWith(QLatin1String("_hlm")) || l.endsWith(QLatin1String("_glv"))
                    || l.endsWith(QLatin1String("_leg")) || l.endsWith(QLatin1String("_bts"))) continue;  // other slots' bases
                if (m_slot[i]->findData(e.snoId) < 0) m_slot[i]->addItem(e.name, e.snoId);
            }
        }
        // Restore the saved selection for this slot.
        const QString saved = QSettings().value(QStringLiteral("wardrobe2/slot/%1").arg(i)).toString();
        const int si = saved.isEmpty() ? 0 : m_slot[i]->findText(saved);
        if (si > 0) m_slot[i]->setCurrentIndex(si);
    }
    // Back trophy (player back cosmetic, ItemType CosmeticBack): the appearances are named
    // "back_*" (e.g. back_dru00, back_bar_proxy001) and carry their own spine/cloth rig, so they
    // skin onto the body via the normal armour merge. Class-agnostic — list them all.
    if (m_backTrophy) {
        QSignalBlocker blockBt(m_backTrophy);
        m_backTrophy->clear();
        m_backTrophy->addItem(QStringLiteral("(none)"), 0);
        for (const SnoEntry& e : all) {
            const QString l = e.name.toLower();
            if (l.startsWith(QLatin1String("back_")) && !l.contains(QLatin1String("_wall"))
                && !l.contains(QLatin1String("_stage")) && !l.contains(QLatin1String("_stair")))
                m_backTrophy->addItem(e.name, e.snoId);
        }
        const QString savedBt = QSettings().value(QStringLiteral("wardrobe2/backTrophy")).toString();
        const int bi = savedBt.isEmpty() ? 0 : m_backTrophy->findText(savedBt);
        if (bi > 0) m_backTrophy->setCurrentIndex(bi);
    }
    populateSets();
    if (!m_restoring) rebuildOutfit();   // callers batching populate* skip this and rebuild once
    fillLookGrid();        // refill the appearance icon grid for the active slot
    refreshSlotCells();    // update each slot cell's equipped icon
}

// ── Equipment icon-grid view (pure view over the hidden m_slot[] combos) ─────
// An appearance's REAL 2D inventory icon (AppearanceMeta handle → atlas crop). 2D only.
// Null until both indexes are ready; the grid refills on readyChanged (lazy build on show).
static QImage wardrobeLookImage(int sno, CascReader* reader)
{
    if (sno <= 0 || !reader) return QImage();
    if (!AppearanceMeta::instance().ready() || !IconIndex::instance().ready()) return QImage();
    static QHash<int, QImage> cache;   // sno → icon is stable once the indexes are ready
    const auto it = cache.constFind(sno);
    if (it != cache.constEnd()) return *it;
    const quint32 h = AppearanceMeta::instance().iconFor(sno);
    QImage img = h ? IconIndex::instance().iconImage(h, reader) : QImage();
    if (!img.isNull()) cache.insert(sno, img);   // don't cache misses — retry on the next refill
    return img;
}

QComboBox* WardrobeTab2::slotCombo(int i) const
{
    if (i < 5)  return m_slot[i];
    switch (i) {
        case 5:  return m_weapon;      // held main
        case 6:  return m_weapon2;     // held off
        case 7:  return m_weapon3;     // sheathed main
        case 8:  return m_weapon4;     // sheathed off
        case 9:  return m_backTrophy;  // back trophy
        default: return nullptr;
    }
}
QString WardrobeTab2::slotLabel(int i) const
{
    if (i < 5) return QString::fromLatin1(kSlots[i].label);
    switch (i) {
        case 5:  return QStringLiteral("Main");
        case 6:  return QStringLiteral("Off");
        case 7:  return QStringLiteral("Sheath");
        case 8:  return QStringLiteral("Sheath 2");
        case 9:  return QStringLiteral("Back");
        default: return QString();
    }
}

void WardrobeTab2::selectSlot(int i)
{
    if (i < 0 || i >= kSlotCount) return;
    // A slot the class can't use (e.g. no off-hand for a two-hand-only class) is greyed out and
    // not selectable — clicking it is a no-op rather than opening an empty grid.
    if (m_slotCells[i] && !m_slotCells[i]->isEnabled()) return;
    m_activeSlot = i;
    if (m_slotCells[i]) m_slotCells[i]->setChecked(true);
    // Weapons (Main/Off, i >= 5) can't be dyed: grey the Set Pigment tab and force Set Look.
    const bool weapon = (i >= 5);
    if (m_pigmentModeBtn) {
        m_pigmentModeBtn->setEnabled(!weapon);
        m_pigmentModeBtn->setToolTip(weapon ? QStringLiteral("Weapons can't be dyed") : QString());
    }
    if (m_d4View) frameSlot(i, /*animate=*/true, /*keepRotation=*/true);   // glide to slot, keep angle
    // If no theme is active yet (e.g. just opened the tab with a set already worn), seed the
    // "matching" highlight from the item equipped in this slot so its set lights up as you
    // browse. Once a theme is set (here or by clicking a card) it persists, so the
    // "click a helm → see the matching torso in the next slot" flow keeps working.
    if (m_activeTheme.armor.isEmpty() && m_activeTheme.weapons.isEmpty() && m_activeTheme.creator.isEmpty()) {
        if (QComboBox* sc = slotCombo(i)) {
            const int k = sc->currentIndex();
            if (k > 0) {
                const QString d = sc->itemData(k, Qt::UserRole + 1).toString();
                m_activeTheme = resolveTheme(sc->itemData(k).toInt(), d.isEmpty() ? sc->itemText(k) : d);
            }
        }
    }
    if (weapon && m_pigmentMode) { setPigmentMode(false); return; }   // setPigmentMode fills the grid
    if (m_pigmentMode) { fillPigmentGrid(); refreshPigmentPanel(); }
    else               fillLookGrid();   // ends with refreshLookHighlights()
    if (m_lookScroll) m_lookScroll->setFocus();   // ready for arrow-key navigation
}

// Camera Snap: zoom the camera onto a slot. Prefer the equipped piece's actual geometry
// (helm→head, gloves→hands, boots→feet); fall back to a height band when the slot has no
// dedicated geometry (e.g. a bare head). Vertical axis is +Y in this (y-up) render space.
void WardrobeTab2::frameSlot(int slot, bool animate, bool keepRotation)
{
    if (!m_view) return;

    // Draw-parts that belong to this slot (part index i == merged-primitive i == m_partSlot[i]).
    QVector<int> slotParts;
    for (int i = 0; i < m_partSlot.size(); ++i)
        if (m_partSlot[i] == slot) slotParts << i;

    QVector3D center; float radius;
    // Prefer the slot's LIVE (animated) bounds so the snap frames where the slot actually is
    // right now — not its bind-pose position (which is wrong mid-animation, e.g. sitting).
    bool haveSlot = m_view->partsBounds(slotParts, center, radius);

    if (!haveSlot) {
        // Fallback: a height band derived from the full (bind) bounds, for slots with no
        // dedicated geometry (e.g. a bare head).
        QVector3D fmin( 1e9f,  1e9f,  1e9f), fmax(-1e9f, -1e9f, -1e9f);
        bool haveFull = false;
        for (const MeshPrimitive& p : m_lastMerged.primitives)
            for (const MeshVertex& v : p.vertices) {
                fmin.setX(qMin(fmin.x(), v.px)); fmin.setY(qMin(fmin.y(), v.py)); fmin.setZ(qMin(fmin.z(), v.pz));
                fmax.setX(qMax(fmax.x(), v.px)); fmax.setY(qMax(fmax.y(), v.py)); fmax.setZ(qMax(fmax.z(), v.pz));
                haveFull = true;
            }
        if (!haveFull) { if (!keepRotation) m_view->frameThreeQuarter(0.5f, 0.10f, 0.12f); return; }
        //                          helm  trs   glv   leg   bts   main  off   shM   shO   back
        static const float kc[kSlotCount] = {0.92f,0.66f,0.50f,0.34f,0.10f,0.55f,0.55f,0.55f,0.55f,0.62f};
        static const float kr[kSlotCount] = {0.16f,0.28f,0.32f,0.28f,0.18f,0.55f,0.55f,0.55f,0.55f,0.30f};
        const int s = qBound(0, slot, kSlotCount - 1);
        const float h = qMax(0.001f, fmax.y() - fmin.y());
        center = QVector3D((fmin.x() + fmax.x()) * 0.5f, fmin.y() + kc[s] * h, (fmin.z() + fmax.z()) * 0.5f);
        radius = qMax(kr[s] * h, 0.05f);
    }

    // Pad the framing by the user's "snap margin" (looser vs. tighter crop).
    radius *= (1.0f + m_snapMargin);
    if (keepRotation) m_view->frameRegionKeepRotation(center, radius, animate);  // hold the user's angle
    else              m_view->frameRegion(center, radius, 0.5f, 0.06f, animate); // ¾ hero angle
    // "Follow animation" ON → keep re-centring on this slot as an animation plays it around.
    // OFF → snap once to the live position (above) but don't track it; clear any prior follow.
    m_view->followParts(m_camFollow ? slotParts : QVector<int>{});
}

// Equipping any theme scope frames the whole outfit (full body), keeping the current angle.
void WardrobeTab2::frameThemeScope(ThemeScope scope)
{
    Q_UNUSED(scope);
    if (m_view) m_view->frameAll(/*keepRotation=*/true);
}

// ── "Remember camera on relaunch" persistence ────────────────────────────────
void WardrobeTab2::saveCameraState()
{
    if (!m_view || !QSettings().value(QStringLiteral("wardrobe2/rememberCam"), true).toBool()) return;
    const GLModelWidget::CamState c = m_view->cameraState();
    QSettings s;
    s.setValue(QStringLiteral("wardrobe2/cam/yaw"), c.yaw);     s.setValue(QStringLiteral("wardrobe2/cam/pitch"), c.pitch);
    s.setValue(QStringLiteral("wardrobe2/cam/dist"), c.dist);   s.setValue(QStringLiteral("wardrobe2/cam/fov"), c.fov);
    s.setValue(QStringLiteral("wardrobe2/cam/cx"), c.cx);       s.setValue(QStringLiteral("wardrobe2/cam/cy"), c.cy);
    s.setValue(QStringLiteral("wardrobe2/cam/cz"), c.cz);       s.setValue(QStringLiteral("wardrobe2/cam/ortho"), c.ortho);
    s.setValue(QStringLiteral("wardrobe2/cam/saved"), true);
}

void WardrobeTab2::restoreCameraState()
{
    QSettings s;
    if (!m_view || !s.value(QStringLiteral("wardrobe2/rememberCam"), true).toBool()
                || !s.value(QStringLiteral("wardrobe2/cam/saved"), false).toBool())
        return;
    GLModelWidget::CamState c;
    c.yaw   = s.value(QStringLiteral("wardrobe2/cam/yaw"),   c.yaw).toFloat();
    c.pitch = s.value(QStringLiteral("wardrobe2/cam/pitch"), c.pitch).toFloat();
    c.dist  = s.value(QStringLiteral("wardrobe2/cam/dist"),  c.dist).toFloat();
    c.fov   = s.value(QStringLiteral("wardrobe2/cam/fov"),   c.fov).toFloat();
    c.cx    = s.value(QStringLiteral("wardrobe2/cam/cx"), 0.0).toFloat();
    c.cy    = s.value(QStringLiteral("wardrobe2/cam/cy"), 0.0).toFloat();
    c.cz    = s.value(QStringLiteral("wardrobe2/cam/cz"), 0.0).toFloat();
    c.ortho = s.value(QStringLiteral("wardrobe2/cam/ortho"), false).toBool();
    c.valid = true;
    m_view->setCameraState(c);
    // Keep the FOV slider in sync with the restored view so the widget and the camera agree
    // (block signals: the view already has this FOV, no need to re-apply/re-save).
    if (m_fovSlider) { QSignalBlocker b(m_fovSlider); m_fovSlider->setValue(int(c.fov)); }
}

// ── Full-character "Look" presets ─────────────────────────────────────────────

// Where an ensemble's viewport-snapshot thumbnail lives. Name → file needs sanitizing (users can
// type anything); a hash suffix keeps two names that sanitize identically from colliding.
static QString ensembleThumbPath(const QString& name)
{
    QString san = name;
    for (QChar& c : san)
        if (!c.isLetterOrNumber() && c != QLatin1Char('_') && c != QLatin1Char('-'))
            c = QLatin1Char('_');
    return AppPaths::dataDir() + QStringLiteral("/ensembles/%1_%2.png")
        .arg(san).arg(qHash(name), 0, 16);
}

// The QSettings keys that together define the whole character look.
static QStringList wardrobeLookKeys()
{
    QStringList k = { QStringLiteral("class"), QStringLiteral("gender"),
                      QStringLiteral("skinTone"), QStringLiteral("skinDetail"),
                      QStringLiteral("weaponType"), QStringLiteral("weapon"),
                      QStringLiteral("weaponType2"), QStringLiteral("weapon2"),
                      QStringLiteral("weaponSheath"), QStringLiteral("weaponSheath2"),
                      QStringLiteral("backTrophy"), QStringLiteral("anim") };
    // NB: free function — can't see WardrobeTab2::kSlotCount (private). Keep this literal in
    // sync with kSlotCount (currently 10: 5 armour + main/off + 2 sheaths + back trophy).
    constexpr int kNumSlots = 10;
    for (int i = 0; i < 9; ++i) k << QStringLiteral("creator/%1").arg(i);
    for (int i = 0; i < kNumSlots; ++i) k << QStringLiteral("slot/%1").arg(i);
    for (int i = 0; i < kNumSlots; ++i) { k << QStringLiteral("dye/%1").arg(i) << QStringLiteral("dyehex/%1").arg(i); }
    return k;
}

void WardrobeTab2::saveLook(const QString& name)
{
    if (name.trimmed().isEmpty()) return;
    QSettings s;
    const QString pfx = QStringLiteral("wardrobe2/looks/%1/").arg(name);
    for (const QString& key : wardrobeLookKeys())
        s.setValue(pfx + key, s.value(QStringLiteral("wardrobe2/") + key));
    // Also snapshot each slot's appearance SNO so the panel can draw the equipped-item icon stack.
    for (int i = 0; i < kSlotCount; ++i) {
        QComboBox* c = slotCombo(i);
        s.setValue(pfx + QStringLiteral("slotSno/%1").arg(i), c ? c->currentData().toInt() : 0);
    }
    QStringList names = s.value(QStringLiteral("wardrobe2/lookNames")).toStringList();
    if (!names.contains(name)) { names << name; names.sort(); s.setValue(QStringLiteral("wardrobe2/lookNames"), names); }
    // Snapshot the character as the ensemble's thumbnail — front (+X) view, BASE COLOUR channel
    // (unlit flat colour), so the tile reads as the outfit's palette regardless of lighting.
    if (m_view) {
        const QImage thumb = m_view->grabEnsembleThumb(128);
        if (!thumb.isNull()) {
            const QString tp = ensembleThumbPath(name);
            QDir().mkpath(QFileInfo(tp).absolutePath());
            thumb.save(tp, "PNG");
        }
    }
    refreshEnsembles();
    if (m_status) m_status->setText(QStringLiteral("Saved ensemble '%1'").arg(name));
}

void WardrobeTab2::loadLook(const QString& name)
{
    if (name.isEmpty()) return;
    m_activeLook = name;   // remember which ensemble is loaded → highlighted in the grid
    QSettings s;
    const QString pfx = QStringLiteral("wardrobe2/looks/%1/").arg(name);
    for (const QString& key : wardrobeLookKeys())
        if (s.contains(pfx + key)) s.setValue(QStringLiteral("wardrobe2/") + key, s.value(pfx + key));
    // refresh() is one-time gated (m_loaded) and restoreSelection is cascade-guarded, so drive the
    // same restore chain refresh() uses directly: class/gender/skin → appearance → dyes → weapons →
    // slots (populateSlots rebuilds the outfit last).
    restoreSelection();
    populateCreator();
    restoreSlotDyes();
    populateWeapons();
    populateSlots();
    if (m_status) m_status->setText(QStringLiteral("Loaded ensemble '%1'").arg(name));
}

// ── Outfit undo ───────────────────────────────────────────────────────────────
QHash<QString, QVariant> WardrobeTab2::snapshotLookState() const
{
    QSettings s;
    QHash<QString, QVariant> out;
    for (const QString& key : wardrobeLookKeys())
        out.insert(key, s.value(QStringLiteral("wardrobe2/") + key));
    return out;
}

// Ctrl+Z: re-apply the previous look-state through the SAME restore chain loadLook drives, so
// combos/dyes/weapons/slots all follow and the outfit rebuilds once at the end (populateSlots).
void WardrobeTab2::undoLook()
{
    if (m_undoStack.isEmpty()) {
        if (m_status) m_status->setText(QStringLiteral("Nothing to undo."));
        return;
    }
    const QHash<QString, QVariant> target = m_undoStack.takeLast();
    m_undoApplying = true;
    QSettings s;
    for (auto it = target.constBegin(); it != target.constEnd(); ++it)
        s.setValue(QStringLiteral("wardrobe2/") + it.key(), it.value());
    restoreSelection();
    populateCreator();
    restoreSlotDyes();
    populateWeapons();
    populateSlots();
    m_lastLookState = target;
    m_undoApplying = false;
    if (m_status)
        m_status->setText(QStringLiteral("Undid outfit change (%1 more)").arg(m_undoStack.size()));
}

// Composite icon strip for one ensemble: the appearance icons that have a picture (hair, facial
// hair, makeup, marking shape, jewelry) followed by the equipped-gear icons (7 slots). Empty picks
// are skipped, so the strip is as full as the look allows.
QPixmap WardrobeTab2::ensembleIconStrip(const QString& pfx) const
{
    QSettings s;
    QVector<QImage> icons;
    // Appearance (creator) icons: cats 1 HairStyle, 4 FacialHair, 5 Makeup, 6 MarkingShape, 8 Jewelry.
    for (int cat : { 1, 4, 5, 6, 8 }) {
        const QString stem = s.value(pfx + QStringLiteral("creator/%1").arg(cat)).toString();
        if (stem.isEmpty()) continue;
        const QImage ic = creatorIconImage(cat, stem);
        if (!ic.isNull()) icons << ic;
    }
    // Equipped gear icons: all slots (helm/torso/gloves/legs/boots/main/off/sheaths/back).
    for (int i = 0; i < kSlotCount; ++i) {
        const int sno = s.value(pfx + QStringLiteral("slotSno/%1").arg(i)).toInt();
        if (sno <= 0) continue;
        const quint32 h = AppearanceMeta::instance().iconFor(quint32(sno));
        const QImage ic = h ? IconIndex::instance().iconImage(h, m_reader) : QImage();
        if (!ic.isNull()) icons << ic;
    }
    if (icons.isEmpty()) return QPixmap();
    const int isz = 30, pad = 2;
    QImage strip(icons.size() * (isz + pad), isz, QImage::Format_RGBA8888);
    strip.fill(Qt::transparent);
    QPainter p(&strip);
    for (int i = 0; i < icons.size(); ++i)
        p.drawImage(QRect(i * (isz + pad), 0, isz, isz), icons[i]);
    p.end();
    return QPixmap::fromImage(strip);
}

void WardrobeTab2::refreshEnsembles()
{
    if (!m_ensembleList) return;
    m_ensembleList->blockSignals(true);
    m_ensembleList->clear();
    for (const QString& name : QSettings().value(QStringLiteral("wardrobe2/lookNames")).toStringList()) {
        const QString pfx = QStringLiteral("wardrobe2/looks/%1/").arg(name);
        // Tile art: the saved viewport SNAPSHOT when one exists (what you saw is what you saved);
        // ensembles from before thumbnails fall back to the old gear-icon strip.
        QPixmap tile(ensembleThumbPath(name));
        if (tile.isNull()) tile = ensembleIconStrip(pfx);
        auto* it = new QListWidgetItem(QIcon(tile), name, m_ensembleList);
        it->setData(Qt::UserRole, name);
        it->setTextAlignment(Qt::AlignHCenter);
        const bool active = (name == m_activeLook);
        it->setToolTip(active ? QStringLiteral("'%1' (currently loaded)").arg(name)
                              : QStringLiteral("Double-click to load '%1'").arg(name));
        if (active) {   // mark the loaded look: selected + bold
            QFont f = it->font(); f.setBold(true); it->setFont(f);
            it->setForeground(QColor(0xd8, 0xa2, 0x3a));
            m_ensembleList->setCurrentItem(it);
        }
    }
    m_ensembleList->blockSignals(false);
}

void WardrobeTab2::deleteEnsemble(const QString& name)
{
    if (name.isEmpty()) return;
    QSettings s;
    s.remove(QStringLiteral("wardrobe2/looks/%1").arg(name));   // drops the whole group
    QStringList names = s.value(QStringLiteral("wardrobe2/lookNames")).toStringList();
    names.removeAll(name);
    s.setValue(QStringLiteral("wardrobe2/lookNames"), names);
    QFile::remove(ensembleThumbPath(name));   // its snapshot goes with it
    refreshEnsembles();
    if (m_status) m_status->setText(QStringLiteral("Deleted ensemble '%1'").arg(name));
}

void WardrobeTab2::renameEnsemble(const QString& oldName, const QString& newName)
{
    const QString nn = newName.trimmed();
    if (oldName.isEmpty() || nn.isEmpty() || oldName == nn) return;
    QSettings s;
    s.beginGroup(QStringLiteral("wardrobe2/looks/%1").arg(oldName));
    const QStringList keys = s.allKeys();
    s.endGroup();
    const QString op = QStringLiteral("wardrobe2/looks/%1/").arg(oldName);
    const QString np = QStringLiteral("wardrobe2/looks/%1/").arg(nn);
    for (const QString& k : keys) s.setValue(np + k, s.value(op + k));
    s.remove(QStringLiteral("wardrobe2/looks/%1").arg(oldName));
    QStringList names = s.value(QStringLiteral("wardrobe2/lookNames")).toStringList();
    names.removeAll(oldName);
    if (!names.contains(nn)) names << nn;
    names.sort();
    s.setValue(QStringLiteral("wardrobe2/lookNames"), names);
    QFile::rename(ensembleThumbPath(oldName), ensembleThumbPath(nn));   // carry the snapshot over
    refreshEnsembles();
}

// Build the Ensembles panel — an icon-stack list of saved looks + Save/Overwrite/Delete/Rename.
void WardrobeTab2::buildEnsemblePanel()
{
    m_ensemblePanel = new QWidget;
    auto* v = new QVBoxLayout(m_ensemblePanel);
    v->setContentsMargins(0, 0, 0, 0); v->setSpacing(4);
    v->addWidget(new QLabel(QStringLiteral("ENSEMBLES")));
    m_ensembleList = new QListWidget;
    // Wrapping tile grid (was a single-column list) — saved looks are far more scannable as tiles,
    // and the active one is highlighted (see refreshEnsembles). Each tile: gear strip + name below.
    m_ensembleList->setViewMode(QListView::IconMode);
    m_ensembleList->setMovement(QListView::Static);
    m_ensembleList->setResizeMode(QListView::Adjust);
    m_ensembleList->setUniformItemSizes(true);
    m_ensembleList->setWordWrap(true);
    m_ensembleList->setSpacing(4);
    m_ensembleList->setIconSize(QSize(84, 84));    // viewport snapshot (old saves: icon strip)
    m_ensembleList->setGridSize(QSize(96, 114));   // tile = snapshot + one-line name
    m_ensembleList->setMaximumHeight(126);         // one row of tiles, scroll for more
    m_ensembleList->setToolTip(QStringLiteral("Saved full-character looks (equipment, appearance, dyes, weapons). "
                                              "Double-click to load."));
    v->addWidget(m_ensembleList);   // fixed height (no stretch) so it stays compact
    auto* row = new QHBoxLayout();
    auto* saveB = new QPushButton(QStringLiteral("Save"));
    auto* overB = new QPushButton(QStringLiteral("Overwrite"));
    auto* delB  = new QPushButton(QStringLiteral("Delete"));
    auto* renB  = new QPushButton(QStringLiteral("Rename"));
    saveB->setToolTip(QStringLiteral("Save the current character as a new ensemble."));
    overB->setToolTip(QStringLiteral("Overwrite the selected ensemble with the current character."));
    for (QPushButton* b : { saveB, overB, delB, renB }) row->addWidget(b);
    v->addLayout(row);

    connect(m_ensembleList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        if (it) loadLook(it->data(Qt::UserRole).toString());
    });
    connect(saveB, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString n = QInputDialog::getText(this, QStringLiteral("Save ensemble"),
                              QStringLiteral("Name:"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (ok && !n.isEmpty()) saveLook(n);
    });
    connect(overB, &QPushButton::clicked, this, [this] {
        if (auto* it = m_ensembleList->currentItem()) saveLook(it->data(Qt::UserRole).toString());
        else if (m_status) m_status->setText(QStringLiteral("Select an ensemble to overwrite."));
    });
    connect(delB, &QPushButton::clicked, this, [this] {
        if (auto* it = m_ensembleList->currentItem()) deleteEnsemble(it->data(Qt::UserRole).toString());
    });
    connect(renB, &QPushButton::clicked, this, [this] {
        auto* it = m_ensembleList->currentItem();
        if (!it) return;
        const QString old = it->data(Qt::UserRole).toString();
        bool ok = false;
        const QString n = QInputDialog::getText(this, QStringLiteral("Rename ensemble"),
                              QStringLiteral("New name:"), QLineEdit::Normal, old, &ok).trimmed();
        if (ok && !n.isEmpty()) renameEnsemble(old, n);
    });
    refreshEnsembles();
    // Honour the "Ensembles panel" toggle (Preview Settings ▸ Geometry & debug); default on.
    m_ensemblePanel->setVisible(
        QSettings().value(QStringLiteral("wardrobe2/viewport/ensembles"), true).toBool());
}

void WardrobeTab2::hideEvent(QHideEvent* ev)
{
    saveCameraState();   // tab switch / app close → remember the current view for next launch
    BrowserTab::hideEvent(ev);
}

// Responsive card metrics: pick a column count + card size that fills the available
// browser width, so cards grow/shrink as the splitter (the horizontal divider) is dragged.
static void cardMetrics(int availW, int& cols, int& cardW, int& cardH, int& iconW)
{
    const int spacing = 3, prefW = 132;
    if (availW < 80) availW = 408;                              // fallback before first show
    cols  = qMax(1, (availW + spacing) / (prefW + spacing));    // how many ~132px cards fit
    cardW = qBound(104, (availW - spacing * (cols + 1)) / cols, 240);  // stretch to fill the row
    cardH = cardW * 150 / 132;                                  // keep the card aspect ratio
    iconW = cardW - 16;
}

// Informational hover tooltip: bold name over series/collection + file name.
// Look-card hover tooltip, line-by-line per Settings ▸ General ▸ On-hover ▸ Wardrobe tab.
// Collection/item DESCRIPTIONS come from the game's text StringLists, which the d4data snapshot
// doesn't carry per-item — those lines are omitted when no text exists (never shown blank).
static QString infoTip(int sno, const QString& title, const QString& coll, const QString& file,
                       bool dyeableKnown = false, bool dyeable = false)
{
    // Optional colour coding (Settings ▸ General ▸ On-hover ▸ "Colour-code hover info"). Name stays
    // WHITE and the filename greyish, per request; the rest get a consistent semantic palette that
    // matches the app's accents: gold = series/collection, parchment italic = flavour text, and
    // rarity in D4's own item-quality colours.
    const bool colour = HoverInfo::colourCode();
    auto tint = [colour](const QString& s, const char* hex) {
        return colour ? QStringLiteral("<span style='color:%1'>%2</span>").arg(QLatin1String(hex), s) : s;
    };
    QStringList out;
    if (HoverInfo::on("w2/name") && !title.isEmpty())
        out << QStringLiteral("<b>%1</b>").arg(tint(title.toHtmlEscaped(), "#ffffff"));
    if (HoverInfo::on("w2/sno") && !file.isEmpty())
        out << tint(sno > 0 ? QStringLiteral("%1 · %2").arg(sno).arg(file.toHtmlEscaped())
                            : file.toHtmlEscaped(), "#9a9a9a");
    if (HoverInfo::on("w2/collName") && !coll.isEmpty())
        out << QStringLiteral("Series: %1").arg(tint(coll.toHtmlEscaped(), "#e8c46a"));
    // Item-level lines from the ItemHoverIndex (name-joined, tolerant — absent data = no line).
    {
        const ItemHoverIndex::Info inf = ItemHoverIndex::instance().infoFor(file);
        // Item description: prefer the mechanical "Description", else the lore "Flavor" line
        // (cosmetics almost always carry Flavor rather than Description).
        if (HoverInfo::on("w2/itemDesc")) {
            const QString d = !inf.desc.isEmpty() ? inf.desc : inf.flavor;
            if (!d.isEmpty()) out << QStringLiteral("<i>%1</i>").arg(tint(d.toHtmlEscaped(), "#c9b48a"));
        }
        // Collection description: the StoreProduct blurb (what the bundle contains) + its quote.
        if (HoverInfo::on("w2/collDesc")) {
            if (!inf.collDesc.isEmpty())
                out << tint(inf.collDesc.toHtmlEscaped(), "#c9b48a");
            if (!inf.collQuote.isEmpty())
                out << QStringLiteral("<i>%1</i>").arg(tint(inf.collQuote.toHtmlEscaped(), "#c9b48a"));
        }
        if (HoverInfo::on("w2/rarity") && inf.rarity > 0)   // Normal (0) is noise — only show above it
            out << QStringLiteral("Rarity: %1")
                       .arg(tint(ItemHoverIndex::rarityLabel(inf.rarity),
                                 ItemHoverIndex::rarityColor(inf.rarity)));
        if (HoverInfo::on("w2/introduced") && !inf.introducedIn.isEmpty())
            out << QStringLiteral("Introduced: %1").arg(tint(inf.introducedIn.toHtmlEscaped(), "#7fb2e5"));
        if (HoverInfo::on("w2/season") && inf.seasonItem)
            out << tint(QStringLiteral("Season item"), "#7fb2e5");
    }
    // (w2/collDesc: collections/series carry no description StringList in the game data — omitted.)
    if (HoverInfo::on("w2/dyeable") && dyeableKnown)
        out << tint(dyeable ? QStringLiteral("Dyeable") : QStringLiteral("Not dyeable"), "#8fbf8f");
    if (out.isEmpty() && !title.isEmpty())
        out << QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped());   // never an empty tooltip
    return out.join(QStringLiteral("<br>"));
}

void WardrobeTab2::fillLookGrid()
{
    if (!m_lookLayout || !m_lookContent) return;
    QComboBox* c = slotCombo(m_activeSlot);
    if (!c) return;
    if (m_lookCollSlot != m_activeSlot) { m_lookCollSlot = m_activeSlot; rebuildLookCollections(); }
    AppearanceMeta& am = AppearanceMeta::instance();
    // Clear the previous buttons + group.
    while (QLayoutItem* it = m_lookLayout->takeAt(0)) { if (it->widget()) it->widget()->deleteLater(); delete it; }
    delete m_lookGroup;
    m_lookGroup = new QButtonGroup(m_lookContent);
    m_lookGroup->setExclusive(true);
    connect(m_lookGroup, &QButtonGroup::idClicked, this, [this](int k) {
        QComboBox* cc = slotCombo(m_activeSlot);
        if (cc) cc->setCurrentIndex(k);   // existing equip/rebuild
        if (cc && k > 0) {                 // selecting an item sets the theme to highlight against
            const QString d = cc->itemData(k, Qt::UserRole + 1).toString();
            setActiveTheme(cc->itemData(k).toInt(), d.isEmpty() ? cc->itemText(k) : d);
        } else {
            refreshLookHighlights();       // (none) → restore styling (clears any cursor outline)
        }
        updateLookHeader();
        refreshSlotCells();
        m_lookCursor = nullptr;                          // next arrow starts from the equipped card
        if (m_lookScroll) m_lookScroll->setFocus();      // keep focus for arrow-key navigation
    });

    // Build the filtered list of combo indices to display (search text + collection filter).
    m_lookItems.clear();
    const QString collSel = m_lookCollFilter ? m_lookCollFilter->currentData().toString() : QString();
    for (int k = 0; k < c->count(); ++k) {
        if (k == 0) { m_lookItems.append(0); continue; }   // always keep "(none)"
        const int sno = c->itemData(k).toInt();
        if (!collSel.isEmpty() && am.collectionFor(sno) != collSel) continue;
        if (!m_lookFilter.isEmpty()) {
            QString hay = c->itemText(k);
            hay += QLatin1Char(' ') + c->itemData(k, Qt::UserRole + 1).toString();
            hay += QLatin1Char(' ') + am.titleFor(sno) + QLatin1Char(' ') + am.collectionFor(sno);
            if (!hay.toLower().contains(m_lookFilter)) continue;
        }
        m_lookItems.append(k);
    }

    // Reset the lazy-build cursor and lay out the first chunk; more load on scroll.
    m_lookCursor = nullptr;   // the previous grid's buttons are gone
    m_lookBuildPos = 0; m_lookBuildRow = 0; m_lookBuildCol = 0; m_lookBuildGroup.clear();
    appendLookCards(60);

    updateLookHeader();
    if (m_iconProgress) m_iconProgress->hide();   // indexing shown on the global status-bar indicator
}

// Build the next chunk of look cards from m_lookItems, continuing the row/col/divider cursor.
void WardrobeTab2::appendLookCards(int maxCards)
{
    if (m_pigmentMode) return;   // the grid holds dye swatches, not lazy look cards
    if (!m_lookLayout || !m_lookGroup || !m_lookContent) return;
    QComboBox* c = slotCombo(m_activeSlot);
    if (!c) return;
    AppearanceMeta& am = AppearanceMeta::instance();
    const bool weaponSlot = (m_activeSlot >= 5 && m_activeSlot <= 8);   // weapons (held+sheathed): group by type
    int cols, cardW, cardH, iconW;
    cardMetrics(m_lookScroll ? m_lookScroll->viewport()->width() : 0, cols, cardW, cardH, iconW);
    m_lookCols = cols;
    auto wrapRow = [&] { if (m_lookBuildCol != 0) { ++m_lookBuildRow; m_lookBuildCol = 0; } };
    int built = 0;
    while (m_lookBuildPos < m_lookItems.size() && built < maxCards) {
        const int k = m_lookItems[m_lookBuildPos++];
        // Full-width category divider whenever the weapon type changes.
        if (weaponSlot && k > 0) {
            const QString grp = c->itemData(k, Qt::UserRole + 2).toString();
            if (!grp.isEmpty() && grp != m_lookBuildGroup) {
                m_lookBuildGroup = grp;
                wrapRow();
                auto* hdr = new QLabel(grp, m_lookContent);
                hdr->setStyleSheet(QStringLiteral(
                    "QLabel{color:#cda85a; font-weight:bold; padding:8px 2px 2px 4px; "
                    "border-bottom:1px solid #4a4133;}"));
                m_lookLayout->addWidget(hdr, m_lookBuildRow, 0, 1, cols);
                ++m_lookBuildRow; m_lookBuildCol = 0;
            }
        }
        const int sno = c->itemData(k).toInt();
        const QString fname = c->itemText(k);
        const QString fullName = c->itemData(k, Qt::UserRole + 1).toString().isEmpty()
                                     ? fname : c->itemData(k, Qt::UserRole + 1).toString();
        auto* b = new QToolButton(m_lookContent);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);   // don't let clicks auto-scroll the list to the card
        b->setFixedSize(cardW, cardH);
        b->setIconSize(QSize(iconW, iconW));
        const QString title = am.titleFor(sno), coll = am.collectionFor(sno);
        const QImage img = wardrobeLookImage(sno, m_reader);
        if (!img.isNull()) {
            QPixmap pm = QPixmap::fromImage(img);
            if (IconBadge::anyEnabled(QStringLiteral("wardrobe"))) {
                int st = 0;
                if (m_reader && m_reader->isReady()) st = m_reader->payloadSize(quint64(sno)) > 0 ? 1 : -1;
                pm = IconBadge::withBadge(pm, st, IconBadge::showPresent(QStringLiteral("wardrobe")),
                                          IconBadge::showMissing(QStringLiteral("wardrobe")));
            }
            b->setIcon(QIcon(pm));
        } else {
            // No 2D icon in the data (common for base weapons) -> label the card with the name.
            b->setToolButtonStyle(Qt::ToolButtonTextOnly);
            QString lbl = (k == 0) ? QStringLiteral("(none)") : fname;
            if (lbl.size() > 24) lbl = lbl.left(22) + QStringLiteral("...");
            b->setText(lbl);
        }
        // Informational hover tooltip — lines per Settings ▸ General ▸ On-hover ▸ Wardrobe tab.
        {
            const bool equippedHere = (k == c->currentIndex()) && m_activeSlot < kSlotCount;
            b->setToolTip(k == 0 ? QStringLiteral("(none)")
                                 : infoTip(sno, title.isEmpty() ? fname : title, coll, fullName,
                                           /*dyeableKnown=*/equippedHere && m_activeSlot < 5,
                                           /*dyeable=*/equippedHere && m_activeSlot < 5
                                                       && m_slotDyeable[m_activeSlot]));
        }
        if (k == c->currentIndex()) b->setChecked(true);
        if (sno > 0) {   // right-click -> equip / equip theme
            b->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(b, &QWidget::customContextMenuRequested, this,
                    [this, b, sno, fullName, fname, title, coll, k](const QPoint& p) {
                        QMenu menu;
                        menu.addAction(QStringLiteral("Equip"), this, [this, k, sno, fullName] {
                            if (QComboBox* cc = slotCombo(m_activeSlot)) cc->setCurrentIndex(k);
                            setActiveTheme(sno, fullName);
                            updateLookHeader(); refreshSlotCells();
                        });
                        menu.addSeparator();
                        const ThemeResolved r = resolveTheme(sno, fullName);
                        menu.addAction(QStringLiteral("Equip Theme  (%1)").arg(themeNames(r, ThemeAll)),
                                       this, [this, sno, fullName] { equipTheme(sno, fullName, ThemeAll); });
                        menu.addAction(QStringLiteral("Equip Theme Armor  (%1)").arg(themeNames(r, ThemeArmor)),
                                       this, [this, sno, fullName] { equipTheme(sno, fullName, ThemeArmor); });
                        menu.addAction(QStringLiteral("Equip Theme Markings  (%1)").arg(themeNames(r, ThemeMarkings)),
                                       this, [this, sno, fullName] { equipTheme(sno, fullName, ThemeMarkings); });
                        menu.addAction(QStringLiteral("Equip Theme Weapons  (%1)").arg(themeNames(r, ThemeWeapons)),
                                       this, [this, sno, fullName] { equipTheme(sno, fullName, ThemeWeapons); });
                        menu.addSeparator();
                        const QString exSuffix = exportMenuSuffix(sno, fullName);
                        // "1 model" was noise — you can only pick one item here. Show the DESTINATION
                        // and the size instead, matching the viewport part menu's wording.
                        const QString exDir = ViewportPartMenu::condensePath(
                            QSettings().value(QStringLiteral("wardrobe2/lastExportDir")).toString());
                        const QString exExtra = exportMenuExtras(exSuffix);
                        if (!exDir.isEmpty())
                            menu.addAction(ViewportPartMenu::withValue(
                                               QStringLiteral("Export Model Last dir"), exDir) + exExtra, this,
                                           [this, sno, fullName] { exportItemModel(sno, fullName, true); });
                        menu.addAction(QStringLiteral("Export Model") + exExtra, this,
                                       [this, sno, fullName] { exportItemModel(sno, fullName, false); });
                        menu.addSeparator();
                        auto clip = [](const QString& s) { QGuiApplication::clipboard()->setText(s); };
                        auto prev = [](const QString& s) { return s.size() > 30 ? s.left(29) + QChar(0x2026) : s; };
                        const QString dispName = title.isEmpty() ? fname : title;
                        menu.addAction(QStringLiteral("Copy SNO id  (%1)").arg(sno), this,
                                       [sno, clip] { clip(QString::number(sno)); });
                        menu.addAction(QStringLiteral("Copy file name  (%1)").arg(prev(fullName)), this,
                                       [fullName, clip] { clip(fullName); });
                        menu.addAction(QStringLiteral("Copy name  (%1)").arg(prev(dispName)), this,
                                       [dispName, clip] { clip(dispName); });
                        QAction* aColl = menu.addAction(QStringLiteral("Copy collection name  (%1)").arg(prev(coll.isEmpty() ? QStringLiteral("—") : coll)), this,
                                       [coll, clip] { clip(coll); });
                        aColl->setEnabled(!coll.isEmpty());
                        menu.exec(b->mapToGlobal(p));
                    });
        }
        m_lookGroup->addButton(b, k);
        m_lookLayout->addWidget(b, m_lookBuildRow, m_lookBuildCol);
        if (++m_lookBuildCol >= cols) { m_lookBuildCol = 0; ++m_lookBuildRow; }
        ++built;
    }
    refreshLookHighlights();   // outline cards matching the active theme
}

// Repopulate the collection dropdown with the distinct collections in the active slot.
void WardrobeTab2::rebuildLookCollections()
{
    if (!m_lookCollFilter) return;
    QComboBox* c = slotCombo(m_activeSlot);
    QSignalBlocker block(m_lookCollFilter);
    const QString prev = m_lookCollFilter->currentData().toString();
    m_lookCollFilter->clear();
    m_lookCollFilter->addItem(QStringLiteral("All collections"), QString());
    if (c) {
        AppearanceMeta& am = AppearanceMeta::instance();
        QSet<QString> seen;
        QStringList colls;
        for (int k = 1; k < c->count(); ++k) {
            const QString cl = am.collectionFor(c->itemData(k).toInt());
            if (!cl.isEmpty() && !seen.contains(cl)) { seen.insert(cl); colls << cl; }
        }
        colls.sort(Qt::CaseInsensitive);
        for (const QString& cl : colls) m_lookCollFilter->addItem(cl, cl);
    }
    const int pi = prev.isEmpty() ? 0 : m_lookCollFilter->findData(prev);
    m_lookCollFilter->setCurrentIndex(pi > 0 ? pi : 0);
}

// Switch the grid between Set Look (appearances) and Set Pigment (per-slot dyes).
void WardrobeTab2::setPigmentMode(bool on)
{
    m_pigmentMode = on;
    if (m_lookModeBtn)    m_lookModeBtn->setChecked(!on);
    if (m_pigmentModeBtn) m_pigmentModeBtn->setChecked(on);
    if (m_lookSearch)     m_lookSearch->setVisible(!on);       // search/collection are look-only
    if (m_lookCollFilter) m_lookCollFilter->setVisible(!on);
    if (m_pigmentPanel)   m_pigmentPanel->setVisible(on);      // custom picker only in pigment mode
    if (on) { fillPigmentGrid(); refreshPigmentPanel(); }
    else      fillLookGrid();
    if (m_lookScroll) m_lookScroll->setFocus();   // ready for arrow-key navigation
}

// Restore each slot's saved pigment. New format stores the 4 hex colours directly; older
// saves stored only a dye name, which we resolve against the dye definitions.
void WardrobeTab2::restoreSlotDyes()
{
    const QVector<DyeDef> dyes = loadPlayerDyes(Config::d4dataDir());
    QSettings s;
    for (int i = 0; i < kSlotCount; ++i) {
        m_slotDye[i] = {};
        const QString nm = s.value(QStringLiteral("wardrobe2/dye/%1").arg(i)).toString();
        const QString hx = s.value(QStringLiteral("wardrobe2/dyehex/%1").arg(i)).toString();
        if (!hx.isEmpty()) {
            const QStringList parts = hx.split(QLatin1Char(','), Qt::SkipEmptyParts);
            if (parts.size() == 4) { m_slotDye[i] = { nm, parts }; continue; }
        }
        if (nm.isEmpty()) continue;
        for (const DyeDef& dd : dyes)
            if (dd.name == nm) {
                QStringList hex; for (int k = 0; k < 4; ++k) hex << dd.colors[k].name();
                m_slotDye[i] = { nm, hex };
                break;
            }
    }
}

// Is the "Apply to all slots" toggle on?
bool WardrobeTab2::applyAllSlots() const { return m_applyAllDye && m_applyAllDye->isChecked(); }

// The 4 hex colours for a slot (white default when the slot has no pigment).
QStringList WardrobeTab2::slotHex(int slot) const
{
    if (slot >= 0 && slot < kSlotCount && m_slotDye[slot].hex.size() == 4) return m_slotDye[slot].hex;
    return QStringList{ QStringLiteral("#ffffff"), QStringLiteral("#ffffff"),
                        QStringLiteral("#ffffff"), QStringLiteral("#ffffff") };
}

// Fill the grid with preset dye swatch cards for the active slot. Left-click applies the
// preset (to all slots when the toggle is on); right-click offers per-slot / all-slots apply.
void WardrobeTab2::fillPigmentGrid()
{
    if (!m_lookLayout || !m_lookContent) return;
    while (QLayoutItem* it = m_lookLayout->takeAt(0)) { if (it->widget()) it->widget()->deleteLater(); delete it; }
    delete m_lookGroup;
    m_lookGroup = new QButtonGroup(m_lookContent);
    m_lookGroup->setExclusive(true);

    // Built-in dyes first, then the user's saved custom pigments (id - 1 indexes `all`).
    const QVector<DyeDef> dyes = loadPlayerDyes(Config::d4dataDir());
    const QVector<DyeDef> customs = loadCustomPigments();
    QVector<DyeDef> all = dyes; all += customs;
    const int presetCount = dyes.size();
    // Dense palette grid: small fixed swatches, as many per row as fit.
    const int spacing = 3;
    const int avail = m_lookScroll ? m_lookScroll->viewport()->width() : 360;
    const int cols = qMax(1, (avail + spacing) / (kPigSwatch + spacing));
    m_lookCols = cols;
    const int iconSz = kPigSwatch - 6;
    const QString curName = (m_activeSlot >= 0 && m_activeSlot < kSlotCount) ? m_slotDye[m_activeSlot].name : QString();

    int row = 0, col = 0;
    auto place = [&](int id, const QString& name, const QPixmap& swatch, bool isCustom) {
        auto* b = new QToolButton(m_lookContent);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedSize(kPigSwatch, kPigSwatch);
        b->setIconSize(QSize(iconSz, iconSz));
        b->setIcon(QIcon(swatch));   // the swatch fills the card
        // Tooltip = the pigment's name (custom pigments noted as such).
        b->setToolTip(name.isEmpty() ? QStringLiteral("No pigment (undyed)")
                                     : (isCustom ? QStringLiteral("%1  (custom)").arg(name) : name));
        if (name == curName) b->setChecked(true);
        b->setStyleSheet(isCustom ? kCardCustomQss : kCardBaseQss);   // gold border = custom
        // Pre-resolve this card's name + 4 hex so the context-menu lambda stays light.
        QString cn = name; QStringList chx;
        if (id > 0 && id - 1 < all.size())
            for (int k = 0; k < 4; ++k) chx << all[id - 1].colors[k].name();
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(b, &QWidget::customContextMenuRequested, this, [this, b, cn, chx, isCustom](const QPoint& p) {
            QMenu menu;
            menu.addAction(QStringLiteral("Apply to this slot"), this, [this, cn, chx] { applyPresetDye(cn, chx, false); });
            menu.addAction(QStringLiteral("Apply to all slots"),  this, [this, cn, chx] { applyPresetDye(cn, chx, true);  });
            if (isCustom) {
                menu.addSeparator();
                menu.addAction(QStringLiteral("Delete custom pigment"), this, [this, cn] {
                    QStringList saved = QSettings().value(QStringLiteral("wardrobe2/customPigments")).toStringList();
                    QStringList kept;
                    for (const QString& e : saved) if (e.section(QLatin1Char('\t'), 0, 0) != cn) kept << e;
                    QSettings().setValue(QStringLiteral("wardrobe2/customPigments"), kept);
                    fillPigmentGrid();
                });
            }
            menu.exec(b->mapToGlobal(p));
        });
        m_lookGroup->addButton(b, id);
        m_lookLayout->addWidget(b, row, col);
        if (++col >= cols) { col = 0; ++row; }
    };
    place(0, QString(), nonePigmentPixmap(iconSz), false);   // "None" = clear this slot's pigment
    for (int d = 0; d < all.size(); ++d)
        place(d + 1, all[d].name, pigmentSwatchPixmap(all[d].colors, iconSz), d >= presetCount);

    connect(m_lookGroup, &QButtonGroup::idClicked, this, [this, all](int id) {
        QString nm; QStringList hx;
        if (id > 0 && id - 1 < all.size()) {
            nm = all[id - 1].name;
            for (int k = 0; k < 4; ++k) hx << all[id - 1].colors[k].name();
        }
        applyPresetDye(nm, hx, applyAllSlots());
        if (m_lookScroll) m_lookScroll->setFocus();   // keep focus for arrow-key navigation
    });
    updateLookHeader();
}

// Apply a full preset (name + 4 colours; empty = clear) to one slot, the active slot, or all.
// `slot` >= 0 targets that specific slot (copy/paste, clear); otherwise `allSlots` decides.
void WardrobeTab2::applyPresetDye(const QString& name, const QStringList& hex, bool allSlots, int slot)
{
    auto one = [&](int sl) {
        m_slotDye[sl] = (hex.size() == 4) ? SlotDye{ name, hex } : SlotDye{};
        QSettings s;
        s.setValue(QStringLiteral("wardrobe2/dye/%1").arg(sl), m_slotDye[sl].name);
        s.setValue(QStringLiteral("wardrobe2/dyehex/%1").arg(sl),
                   m_slotDye[sl].hex.size() == 4 ? m_slotDye[sl].hex.join(QLatin1Char(',')) : QString());
    };
    if (slot >= 0 && slot < kSlotCount) one(slot);
    else if (allSlots) for (int sl = 0; sl < 5; ++sl) one(sl);   // armour only — weapons aren't dyeable
    else if (m_activeSlot >= 0 && m_activeSlot < kSlotCount) one(m_activeSlot);
    applyAllDyes();
    refreshPigmentPanel();
    refreshSlotCells();   // update the per-slot dye badges
    updateLookHeader();
}

// Build the custom pigment picker (4 zone slots + colour wheel + hex + memory). Created once;
// shown only in Set Pigment mode.
void WardrobeTab2::buildPigmentPanel()
{
    m_pigmentPanel = new QWidget;
    auto* pl = new QVBoxLayout(m_pigmentPanel);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(5);

    m_applyAllDye = new QCheckBox(QStringLiteral("Apply to all slots"), m_pigmentPanel);
    m_applyAllDye->setChecked(QSettings().value(QStringLiteral("wardrobe2/dyeApplyAll"), false).toBool());
    m_applyAllDye->setToolTip(QStringLiteral("When on, pigment edits affect every slot at once"));
    connect(m_applyAllDye, &QCheckBox::toggled, this,
            [](bool on) { QSettings().setValue(QStringLiteral("wardrobe2/dyeApplyAll"), on); });
    pl->addWidget(m_applyAllDye);

    // Wheel on the left; zone / hex / save / memory controls on the right (compact).
    auto* mainRow = new QHBoxLayout();
    mainRow->setSpacing(8);

    auto* wheel = new DyeColorWheel(m_pigmentPanel);
    wheel->setMinimumSize(128, 146);
    wheel->setMaximumSize(150, 168);
    wheel->onChanged = [this](const QColor&) { applyDyePicker(); };
    m_dyeWheel = wheel;
    mainRow->addWidget(wheel, 0, Qt::AlignTop);

    auto* ctl = new QVBoxLayout();
    ctl->setSpacing(5);

    ctl->addWidget(new QLabel(QStringLiteral("Pigment zones"), m_pigmentPanel));
    auto* regRow = new QHBoxLayout(); regRow->setSpacing(3);
    auto* grp = new QButtonGroup(this); grp->setExclusive(true);
    for (int r = 0; r < 4; ++r) {
        m_dyeRegionBtn[r] = new QToolButton(m_pigmentPanel);
        m_dyeRegionBtn[r]->setCheckable(true);
        m_dyeRegionBtn[r]->setFixedSize(34, 24);
        m_dyeRegionBtn[r]->setText(QString::number(r + 1));
        m_dyeRegionBtn[r]->setAcceptDrops(true);
        m_dyeRegionBtn[r]->installEventFilter(this);   // drag / drop / right-click reset
        grp->addButton(m_dyeRegionBtn[r], r);
        regRow->addWidget(m_dyeRegionBtn[r]);
    }
    regRow->addStretch(1);
    ctl->addLayout(regRow);
    connect(grp, &QButtonGroup::idClicked, this, [this](int id) { loadDyeRegionToPicker(id); });

    auto* hexRow = new QHBoxLayout();
    m_dyeSwatch = new QLabel(m_pigmentPanel);
    m_dyeSwatch->setFixedSize(24, 22);
    m_dyeSwatch->setStyleSheet(QStringLiteral("border:1px solid #888;"));
    hexRow->addWidget(m_dyeSwatch);
    m_dyeHex = new QLineEdit(m_pigmentPanel);
    m_dyeHex->setMaxLength(7);
    m_dyeHex->setPlaceholderText(QStringLiteral("#rrggbb"));
    connect(m_dyeHex, &QLineEdit::editingFinished, this, [this]() {
        const QColor c(m_dyeHex->text().trimmed());
        if (c.isValid()) setDyeSlotColor(m_dyeRegion, c);
    });
    hexRow->addWidget(m_dyeHex, 1);
    ctl->addLayout(hexRow);

    auto* savePigBtn = new QPushButton(QStringLiteral("Save Pigment"), m_pigmentPanel);
    savePigBtn->setToolTip(QStringLiteral("Save the active slot's 4 colours as a named custom pigment"));
    connect(savePigBtn, &QPushButton::clicked, this, [this]() {
        const QString name = QInputDialog::getText(this, QStringLiteral("Save pigment"),
                                                   QStringLiteral("Pigment name:")).trimmed();
        if (name.isEmpty()) return;
        const QStringList hex = slotHex(m_activeSlot);
        QStringList parts; parts << name << hex;
        QStringList saved = QSettings().value(QStringLiteral("wardrobe2/customPigments")).toStringList();
        QStringList kept;
        for (const QString& e : saved) if (e.section(QLatin1Char('\t'), 0, 0) != name) kept << e;
        kept << parts.join(QLatin1Char('\t'));
        QSettings().setValue(QStringLiteral("wardrobe2/customPigments"), kept);
        if (m_pigmentMode) fillPigmentGrid();   // show the new pigment in the browser
    });
    ctl->addWidget(savePigBtn);

    ctl->addWidget(new QLabel(QStringLiteral("Memory (drag colours here):"), m_pigmentPanel));
    auto* memGrid = new QGridLayout(); memGrid->setSpacing(3);
    QSettings s;
    for (int i = 0; i < 8; ++i) {
        m_dyeMem[i] = new QToolButton(m_pigmentPanel);
        m_dyeMem[i]->setFixedSize(24, 24);
        m_dyeMem[i]->setAcceptDrops(true);
        m_dyeMem[i]->installEventFilter(this);
        m_dyeMem[i]->setToolTip(QStringLiteral(
            "Memory %1 — drag a colour here to store · click to apply · right-click to clear").arg(i + 1));
        const QString mc = s.value(QStringLiteral("wardrobe2/dyeMem%1").arg(i)).toString();
        const QColor c = mc.isEmpty() ? QColor() : QColor(mc);
        m_dyeMem[i]->setStyleSheet(c.isValid()
            ? QStringLiteral("QToolButton{background:%1;border:1px solid #555;}").arg(c.name())
            : QStringLiteral("QToolButton{background:#2b2b2b;border:1px dashed #555;}"));
        connect(m_dyeMem[i], &QToolButton::clicked, this, [this, i]() {
            const QString mc2 = QSettings().value(QStringLiteral("wardrobe2/dyeMem%1").arg(i)).toString();
            if (!mc2.isEmpty()) setDyeSlotColor(m_dyeRegion, QColor(mc2));
        });
        memGrid->addWidget(m_dyeMem[i], i / 4, i % 4);   // 4 columns × 2 rows
    }
    auto* memWrap = new QHBoxLayout(); memWrap->addLayout(memGrid); memWrap->addStretch(1);
    ctl->addLayout(memWrap);
    ctl->addStretch(1);

    mainRow->addLayout(ctl, 1);
    pl->addLayout(mainRow);

    auto* libLbl = new QLabel(QStringLiteral("Pigment library  ·  gold border = custom"), m_pigmentPanel);
    libLbl->setStyleSheet(QStringLiteral("color:#9a8f7d;"));
    pl->addWidget(libLbl);

    m_dyeRegionBtn[0]->setChecked(true);
}

// Load the active slot's 4 zone colours into the region buttons + the wheel.
void WardrobeTab2::refreshPigmentPanel()
{
    for (int r = 0; r < 4; ++r) styleDyeRegionBtn(r);
    loadDyeRegionToPicker(m_dyeRegion);
}

// Restyle one zone button from the active slot's colour for that zone.
void WardrobeTab2::styleDyeRegionBtn(int r)
{
    if (r < 0 || r > 3 || !m_dyeRegionBtn[r]) return;
    const bool dyeable = (m_activeSlot >= 0 && m_activeSlot < kSlotCount) ? m_slotDyeable[m_activeSlot] : true;
    const QColor c(slotHex(m_activeSlot).value(r, QStringLiteral("#ffffff")));
    if (dyeable) {
        m_dyeRegionBtn[r]->setStyleSheet(QStringLiteral(
            "QToolButton{background:%1;color:#000;font-weight:bold;border:1px solid #888;}"
            "QToolButton:checked{border:2px solid #d8a23a;}").arg(c.name()));
        m_dyeRegionBtn[r]->setToolTip(QStringLiteral(
            "Zone %1 — click to edit · drag onto another zone to copy · right-click to reset").arg(r + 1));
    } else {   // slot has no dyeable material → dim the zone
        m_dyeRegionBtn[r]->setStyleSheet(QStringLiteral(
            "QToolButton{background:#262220;color:#6a6258;border:1px dashed #555;}"
            "QToolButton:checked{border:2px solid #6a6258;}"));
        m_dyeRegionBtn[r]->setToolTip(QStringLiteral("Zone %1 — this slot has no dyeable material").arg(r + 1));
    }
}

// Active zone colour → wheel / hex / swatch (does not echo back to the model).
void WardrobeTab2::loadDyeRegionToPicker(int r)
{
    if (r < 0 || r > 3 || !m_dyeWheel) return;
    m_dyeRegion = r;
    const QColor c(slotHex(m_activeSlot).value(r, QStringLiteral("#ffffff")));
    static_cast<DyeColorWheel*>(m_dyeWheel)->setColor(c);
    if (m_dyeSwatch)
        m_dyeSwatch->setStyleSheet(QStringLiteral("background:%1;border:1px solid #888;").arg(c.name()));
    if (m_dyeHex) { QSignalBlocker bh(m_dyeHex); m_dyeHex->setText(c.name()); }
}

// Wheel changed → write the active zone colour live (no echo back to the wheel).
void WardrobeTab2::applyDyePicker()
{
    if (!m_dyeWheel) return;
    const QColor c = static_cast<DyeColorWheel*>(m_dyeWheel)->color();
    if (m_dyeSwatch)
        m_dyeSwatch->setStyleSheet(QStringLiteral("background:%1;border:1px solid #888;").arg(c.name()));
    if (m_dyeHex) { QSignalBlocker bh(m_dyeHex); m_dyeHex->setText(c.name()); }
    writeRegionColor(m_dyeRegion, c);
    styleDyeRegionBtn(m_dyeRegion);
}

// Set a zone colour from outside the wheel flow (drop / memory / hex / reset).
void WardrobeTab2::setDyeSlotColor(int r, const QColor& c)
{
    if (r < 0 || r > 3 || !c.isValid()) return;
    writeRegionColor(r, c);
    styleDyeRegionBtn(r);
    if (r == m_dyeRegion) loadDyeRegionToPicker(r);   // sync wheel/hex/swatch
}

// Store one zone colour onto the active slot (or every slot), persist, and re-apply pigments.
void WardrobeTab2::writeRegionColor(int r, const QColor& c)
{
    if (r < 0 || r > 3 || !c.isValid()) return;
    auto one = [&](int slot) {
        SlotDye& d = m_slotDye[slot];
        if (d.hex.size() != 4) d.hex = slotHex(slot);
        d.hex[r] = c.name();
        if (d.name.isEmpty()) d.name = QStringLiteral("Custom");
        QSettings s;
        s.setValue(QStringLiteral("wardrobe2/dye/%1").arg(slot), d.name);
        s.setValue(QStringLiteral("wardrobe2/dyehex/%1").arg(slot), d.hex.join(QLatin1Char(',')));
    };
    if (applyAllSlots()) for (int sl = 0; sl < 5; ++sl) one(sl);   // armour only — weapons aren't dyeable
    else if (m_activeSlot >= 0 && m_activeSlot < kSlotCount) one(m_activeSlot);
    applyAllDyes();
    refreshSlotCells();   // keep the per-slot dye badges in sync while editing
}

// After an equip, re-check the matching card in place (no rebuild) so the scroll position
// is preserved. If the new selection isn't in the built chunk, just clear the old check.
void WardrobeTab2::refreshLookSelection()
{
    if (m_lookGroup) {
        if (QComboBox* c = slotCombo(m_activeSlot)) {
            if (QAbstractButton* b = m_lookGroup->button(c->currentIndex())) {
                b->setChecked(true);
            } else if (QAbstractButton* ck = m_lookGroup->checkedButton()) {
                m_lookGroup->setExclusive(false); ck->setChecked(false); m_lookGroup->setExclusive(true);
            }
        }
    }
    updateLookHeader();
}

// Resolve the theme of a just-selected item and re-highlight the matching items everywhere.
void WardrobeTab2::setActiveTheme(int sno, const QString& appearanceName)
{
    m_activeTheme = resolveTheme(sno, appearanceName);
    refreshLookHighlights();
    refreshCreatorHighlights();
}

// Outline the look-grid cards that belong to the active theme (matching armour piece for this
// slot, or any of the theme's weapons) so the user can see what completes the set.
void WardrobeTab2::refreshLookHighlights()
{
    if (!m_lookGroup) return;
    QComboBox* c = slotCombo(m_activeSlot);
    if (!c) return;
    const bool weaponSlot = (m_activeSlot >= 5 && m_activeSlot <= 8);
    const QString armorMatch = weaponSlot ? QString() : m_activeTheme.armor.value(m_activeSlot).toLower();
    for (QAbstractButton* ab : m_lookGroup->buttons()) {
        const int k = m_lookGroup->id(ab);
        bool match = false;
        if (k > 0) {
            const QString d = c->itemData(k, Qt::UserRole + 1).toString();
            const QString full = (d.isEmpty() ? c->itemText(k) : d).toLower();
            if (weaponSlot) {
                for (const QString& wn : m_activeTheme.weapons) if (wn.toLower() == full) { match = true; break; }
            } else {
                match = (!armorMatch.isEmpty() && full == armorMatch);
            }
        }
        ab->setStyleSheet(match ? kCardMatchQss : kCardBaseQss);
    }
}

// Outline the creator card that belongs to the active theme (its marking / cosmetic).
void WardrobeTab2::refreshCreatorHighlights()
{
    if (!m_creatorGroup) return;
    QComboBox* cc = m_creator[m_activeCreator];
    const QString match = m_activeTheme.creator.value(m_activeCreator).toLower();
    for (QAbstractButton* ab : m_creatorGroup->buttons()) {
        const int k = m_creatorGroup->id(ab);
        const bool m = (k > 0 && cc && !match.isEmpty() && cc->itemData(k).toString().toLower() == match);
        ab->setStyleSheet(m ? kCardMatchQss : kCardBaseQss);
    }
}

// Header from the blockout: "<SLOT> (count) — <title> · <collection> · <filename>".
void WardrobeTab2::updateLookHeader()
{
    if (!m_lookHeader) return;
    if (m_pigmentMode) {   // Set Pigment: show the slot's chosen dye
        const QString nm = (m_activeSlot >= 0 && m_activeSlot < kSlotCount) ? m_slotDye[m_activeSlot].name : QString();
        QString hdr = QStringLiteral("%1 Pigment - %2")
                          .arg(slotLabel(m_activeSlot), nm.isEmpty() ? QStringLiteral("(none)") : nm);
        if (m_activeSlot >= 0 && m_activeSlot < kSlotCount && !m_slotDyeable[m_activeSlot])
            hdr += QStringLiteral("   (slot not dyeable)");
        m_lookHeader->setText(hdr);
        return;
    }
    QComboBox* c = slotCombo(m_activeSlot);
    if (!c) return;
    AppearanceMeta& am = AppearanceMeta::instance();
    const int sno = c->currentData().toInt();
    const QString fname = c->currentText();
    const QString title = am.titleFor(sno), coll = am.collectionFor(sno);
    QString eq = (c->currentIndex() <= 0) ? QStringLiteral("(none)") : (title.isEmpty() ? fname : title);
    // Show the filtered count ("showing X of Y") when a search/collection filter is active.
    const int total = c->count() - 1;
    const int shown = qMax(0, int(m_lookItems.size()) - 1);
    const QString count = (shown == total) ? QString::number(total)
                                           : QStringLiteral("%1 of %2").arg(shown).arg(total);
    QString hdr = QStringLiteral("%1  (%2) - %3").arg(slotLabel(m_activeSlot), count, eq);
    if (c->currentIndex() > 0) {
        if (!coll.isEmpty()) hdr += QStringLiteral(" - ") + coll;
        hdr += QStringLiteral(" - ") + fname;
    }
    m_lookHeader->setText(hdr);
}

void WardrobeTab2::refreshSlotCells()
{
    for (int i = 0; i < kSlotCount; ++i) {
        if (!m_slotCells[i] || !slotCombo(i)) continue;
        QComboBox* cb = slotCombo(i);
        const int sno = cb->currentData().toInt();
        const QImage img = wardrobeLookImage(sno, m_reader);
        // Composite a small 4-colour dye badge into the bottom-right when this slot is pigmented.
        // Weapons (Main/Off, i >= 5) can't be dyed, so they never get a badge.
        QPixmap pm = img.isNull() ? QPixmap() : QPixmap::fromImage(img);
        const bool dyed = (i < 5 && m_slotDye[i].hex.size() == 4);
        if (dyed) {
            if (pm.isNull()) { pm = QPixmap(48, 48); pm.fill(Qt::transparent); }
            QColor c[4]; for (int k = 0; k < 4; ++k) c[k] = QColor(m_slotDye[i].hex[k]);
            QPainter p(&pm);
            const int bw = qMax(14, pm.width() / 3), bx = pm.width() - bw - 1, by = pm.height() - bw - 1;
            const int h = bw / 2;
            p.fillRect(bx - 1, by - 1, bw + 2, bw + 2, QColor(0, 0, 0));   // dark backing
            p.fillRect(bx, by, h, h, c[0]);          p.fillRect(bx + h, by, bw - h, h, c[1]);
            p.fillRect(bx, by + h, h, bw - h, c[2]);  p.fillRect(bx + h, by + h, bw - h, bw - h, c[3]);
            p.end();
        }
        m_slotCells[i]->setIcon(pm.isNull() ? QIcon() : QIcon(pm));
        // Tooltip: the slot label + the equipped item's name/series/file (if any) + pigment.
        AppearanceMeta& am = AppearanceMeta::instance();
        if (cb->currentIndex() > 0) {
            QString tip = infoTip(sno, slotLabel(i) + QStringLiteral(": ")
                                  + (am.titleFor(sno).isEmpty() ? cb->currentText() : am.titleFor(sno)),
                                  am.collectionFor(sno), cb->currentText(),
                                  /*dyeableKnown=*/i < 5, /*dyeable=*/i < 5 && m_slotDyeable[i]);
            if (dyed) tip += QStringLiteral("<br>Pigment: %1").arg(
                m_slotDye[i].name.isEmpty() ? QStringLiteral("custom") : m_slotDye[i].name.toHtmlEscaped());
            m_slotCells[i]->setToolTip(tip);
        } else {
            m_slotCells[i]->setToolTip(slotLabel(i) + QStringLiteral(": (none)"));
        }
    }
}

// ── Appearance page: creator-category card browser (mirrors the Equipment grid) ──
static QImage creatorSwatch(const QColor& c)
{
    QImage img(72, 72, QImage::Format_RGB888);
    img.fill(c);
    return img;
}

// A gradient swatch that previews a 3-point paint ramp (shadow → mid → highlight) diagonally, so the
// marking-colour picker shows what each colour actually looks like — the metallic gold highlight, the
// two-tone range — instead of a flat mid-tone that hid the defining part of the colour.
static QImage creatorRampSwatch(std::array<QColor, 3> r)
{
    QImage img(72, 72, QImage::Format_RGB888);
    if (!r[0].isValid()) r[0] = r[1].isValid() ? r[1] : QColor(60, 60, 60);
    for (int y = 0; y < 72; ++y)
        for (int x = 0; x < 72; ++x) {
            const QColor c = rampLerp(r, (x + y) / 142.0f);   // 0..1 along the diagonal
            img.setPixel(x, y, qRgb(c.red(), c.green(), c.blue()));
        }
    return img;
}

// A creator item's icon: hIconImage (Makeup/Marking/Jewelry), gender tIcons (Hair/FacialHair),
// or a colour swatch (Hair/Eye/Marking colour). Face has none. Null until IconIndex is ready.
QImage WardrobeTab2::creatorIconImage(int cat, const QString& stem) const
{
    if (stem.isEmpty() || !m_reader) return QImage();
    const bool male = m_gender && m_gender->currentData().toString() == QLatin1String("m");
    const int ci = m_class ? m_class->currentIndex() : 0;
    static QHash<QString, QImage> cache;   // (cat,stem,class,gender) → icon; avoids re-reading defs
    const QString key = QStringLiteral("%1/%2/%3/%4").arg(cat).arg(stem).arg(ci).arg(int(male));
    const auto cit = cache.constFind(key);
    if (cit != cache.constEnd()) return *cit;
    const QString d4 = Config::d4dataDir();
    QImage img;
    if (cat == 2)      { const QVector<QColor> hr = hairColorRamp(d4, stem);   // hair: full ramp gradient
                         if (hr.size() >= 2) img = creatorRampSwatch({ hr[0], hr[1], hr.size() > 2 ? hr[2] : hr[1] });
                         else { const QColor c = hairColorMid(d4, stem); if (c.isValid()) img = creatorSwatch(c); } }
    else if (cat == 3) { const QColor ci = eyeIrisColor(d4, stem), co = eyeIrisOuterColor(d4, stem);   // iris inner→outer
                         if (ci.isValid()) img = creatorRampSwatch({ ci, co.isValid() ? co : ci, co.isValid() ? co : ci });
                         }
    else if (cat == 7) { const auto r = markingRamp(d4, stem);    if (r[0].isValid() || r[1].isValid()) img = creatorRampSwatch(r); }
    else if (cat == 0) { /* Face: no icon */ }
    else if (!IconIndex::instance().ready()) {
        return QImage();   // not ready yet — don't cache; retry on readyChanged
    } else {
        QFile f(d4 + QStringLiteral("/json/base/meta/") + QLatin1String(kCreator[cat].folder)
                + QStringLiteral("/") + stem + QLatin1String(kCreator[cat].ext));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            quint32 h = 0;
            if (o.contains(QStringLiteral("hIconImage")))
                h = quint32(o.value(QStringLiteral("hIconImage")).toDouble());
            else {   // HairStyle / FacialHair: tIcons[] is gender-specific, indexed by class.
                const QJsonArray ic = o.value(QStringLiteral("tIcons")).toArray();
                auto pick = [&](const QJsonObject& g) {
                    return quint32((male ? g.value(QStringLiteral("hDefaultImage")) : g.value(QStringLiteral("hFemaleImage"))).toDouble());
                };
                if (ci >= 0 && ci < ic.size()) h = pick(ic[ci].toObject());
                if (!h) for (const QJsonValue& v : ic) { h = pick(v.toObject()); if (h) break; }
            }
            if (h) img = IconIndex::instance().iconImage(h, m_reader);
        }
    }
    // Only cache resolved icons. A null here means the data/index wasn't ready yet (e.g. during
    // a post-update reload); caching it would blank the cell permanently, so leave it uncached to
    // retry on the next refill (readyChanged / grid rebuild). Face (cat 0) is intentionally null.
    if (!img.isNull() || cat == 0)
        cache.insert(key, img);
    return img;
}

void WardrobeTab2::selectCreator(int i)
{
    if (i < 0 || i >= 9) return;
    m_activeCreator = i;
    if (m_creatorCells[i]) m_creatorCells[i]->setChecked(true);
    fillCreatorGrid();
    if (m_creatorScroll) m_creatorScroll->setFocus();   // ready for arrow-key navigation
}

// Look grid: arrows move a dashed-gold cursor over the cards WITHOUT equipping (equipping
// reloads geometry, so we defer it); Enter/Space/Return equips the highlighted card.
bool WardrobeTab2::navLookGrid(int key)
{
    if (!m_lookGroup || !m_lookLayout || !m_lookScroll) return false;
    if (key == Qt::Key_Return || key == Qt::Key_Enter || (key == Qt::Key_Space && m_lookCursor)) {
        if (m_lookCursor) m_lookCursor->click();   // equip the highlighted look
        return true;
    }
    const int cols = qMax(1, m_lookCols);
    int delta = 0;
    if      (key == Qt::Key_Left)  delta = -1;
    else if (key == Qt::Key_Right) delta = +1;
    else if (key == Qt::Key_Up)    delta = -cols;
    else if (key == Qt::Key_Down)  delta = +cols;
    else return false;
    auto collect = [this](QVector<QAbstractButton*>& order) {
        order.clear();
        for (int i = 0; i < m_lookLayout->count(); ++i) {
            QLayoutItem* it = m_lookLayout->itemAt(i);
            if (auto* b = it ? qobject_cast<QAbstractButton*>(it->widget()) : nullptr) order << b;
        }
    };
    QVector<QAbstractButton*> order;
    collect(order);
    if (order.isEmpty()) return true;
    int cur = m_lookCursor ? order.indexOf(m_lookCursor) : -1;
    if (cur < 0) {                                  // start from the equipped card (or first)
        QAbstractButton* ck = m_lookGroup->checkedButton();
        cur = ck ? qMax(0, order.indexOf(ck)) : 0;
    }
    int next = cur + delta;
    if (next >= order.size() && m_lookBuildPos < m_lookItems.size()) {   // lazily build more first
        appendLookCards(qMax(48, next - order.size() + cols));
        collect(order);
    }
    next = qBound(0, next, order.size() - 1);
    m_lookCursor = order[next];
    refreshLookHighlights();                        // restore base/match/checked styling on all cards
    m_lookCursor->setStyleSheet(kCardCursorQss);    // overlay the cursor outline
    m_lookScroll->ensureWidgetVisible(m_lookCursor);
    return true;
}

// Move the checked card by one (←/→) or one row (↑/↓) in display order and apply it.
bool WardrobeTab2::navGrid(QScrollArea* scroll, QGridLayout* layout, int cols, int key)
{
    if (!scroll || !layout) return false;
    int delta = 0;
    if      (key == Qt::Key_Left)  delta = -1;
    else if (key == Qt::Key_Right) delta = +1;
    else if (key == Qt::Key_Up)    delta = -qMax(1, cols);
    else if (key == Qt::Key_Down)  delta = +qMax(1, cols);
    else return false;
    QVector<QAbstractButton*> order;   // buttons in layout (display) order; skip dividers/labels
    int cur = -1;
    for (int i = 0; i < layout->count(); ++i) {
        QLayoutItem* it = layout->itemAt(i);
        auto* b = it ? qobject_cast<QAbstractButton*>(it->widget()) : nullptr;
        if (!b) continue;
        if (b->isChecked()) cur = order.size();
        order << b;
    }
    if (order.isEmpty()) return true;
    if (cur < 0) cur = 0;
    const int next = qBound(0, cur + delta, order.size() - 1);
    order[next]->click();                       // applies via the group's idClicked
    scroll->ensureWidgetVisible(order[next]);
    return true;
}

void WardrobeTab2::fillCreatorGrid()
{
    if (!m_creatorLayout || !m_creatorContent) return;
    QComboBox* c = m_creator[m_activeCreator];
    if (!c) return;
    while (QLayoutItem* it = m_creatorLayout->takeAt(0)) { if (it->widget()) it->widget()->deleteLater(); delete it; }
    delete m_creatorGroup;
    m_creatorGroup = new QButtonGroup(m_creatorContent);
    m_creatorGroup->setExclusive(true);
    // Sizing: colour categories use dense fill-swatches; the rest keep the responsive card grid.
    const bool colorCat = (m_activeCreator == 2 || m_activeCreator == 3 || m_activeCreator == 7);
    int cols, cardW, cardH, iconW;
    if (colorCat) {
        const int avail = m_creatorScroll ? m_creatorScroll->viewport()->width() : 360;
        cols = qMax(1, (avail + 3) / (kPigSwatch + 3));
        cardW = cardH = kPigSwatch; iconW = kPigSwatch - 6;
    } else {
        cardMetrics(m_creatorScroll ? m_creatorScroll->viewport()->width() : 0, cols, cardW, cardH, iconW);
    }
    m_creatorCols = cols;

    // Display order: "(default)" first, then the rest filtered by search and sorted by name.
    QVector<int> order;
    for (int k = 1; k < c->count(); ++k) {
        const QString st = c->itemData(k).toString();
        if (!m_creatorFilter.isEmpty()
            && !(c->itemText(k).toLower().contains(m_creatorFilter) || st.toLower().contains(m_creatorFilter)))
            continue;
        order << k;
    }
    const int sortMode = m_creatorSort ? m_creatorSort->currentIndex() : 0;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const int cmp = c->itemText(a).compare(c->itemText(b), Qt::CaseInsensitive);
        return sortMode == 1 ? cmp > 0 : cmp < 0;
    });
    order.prepend(0);   // "(default)" always first

    int pos = 0;
    for (int k : order) {
        const QString stem = c->itemData(k).toString();
        auto* b = new QToolButton(m_creatorContent);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);   // don't let clicks auto-scroll the list to the card
        b->setFixedSize(cardW, cardH);
        b->setIconSize(QSize(iconW, iconW));
        const QImage img = creatorIconImage(m_activeCreator, stem);
        if (!img.isNull()) { b->setIcon(QIcon(QPixmap::fromImage(img))); }
        else { b->setToolButtonStyle(Qt::ToolButtonTextOnly); b->setText(k == 0 ? QStringLiteral("(default)") : c->itemText(k)); }
        // Informational hover tooltip: display name + file (stem).
        b->setToolTip(k == 0 ? QStringLiteral("(default)") : infoTip(0, c->itemText(k), QString(), stem));
        if (k == c->currentIndex()) b->setChecked(true);
        if (k > 0) {   // right-click -> equip / equip theme (same menu as the look grid)
            b->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(b, &QWidget::customContextMenuRequested, this,
                    [this, b, stem, k](const QPoint& p) {
                        QMenu menu;
                        menu.addAction(QStringLiteral("Equip"), this, [this, k] {
                            if (m_creator[m_activeCreator]) m_creator[m_activeCreator]->setCurrentIndex(k);
                            refreshCreatorCells();
                        });
                        menu.addSeparator();
                        const ThemeResolved r = resolveTheme(0, stem);
                        menu.addAction(QStringLiteral("Equip Theme  (%1)").arg(themeNames(r, ThemeAll)),
                                       this, [this, stem] { equipTheme(0, stem, ThemeAll); });
                        menu.addAction(QStringLiteral("Equip Theme Armor  (%1)").arg(themeNames(r, ThemeArmor)),
                                       this, [this, stem] { equipTheme(0, stem, ThemeArmor); });
                        menu.addAction(QStringLiteral("Equip Theme Markings  (%1)").arg(themeNames(r, ThemeMarkings)),
                                       this, [this, stem] { equipTheme(0, stem, ThemeMarkings); });
                        menu.addAction(QStringLiteral("Equip Theme Weapons  (%1)").arg(themeNames(r, ThemeWeapons)),
                                       this, [this, stem] { equipTheme(0, stem, ThemeWeapons); });
                        menu.addSeparator();
                        menu.addAction(QStringLiteral("Copy name"), this,
                                       [stem] { QGuiApplication::clipboard()->setText(stem); });
                        menu.exec(b->mapToGlobal(p));
                    });
        }
        m_creatorGroup->addButton(b, k);
        m_creatorLayout->addWidget(b, pos / cols, pos % cols);
        ++pos;
    }
    connect(m_creatorGroup, &QButtonGroup::idClicked, this, [this](int k) {
        if (m_creator[m_activeCreator]) m_creator[m_activeCreator]->setCurrentIndex(k);   // → existing rebuild
        updateCreatorHeader();
        refreshCreatorCells();
        if (m_creatorScroll) m_creatorScroll->setFocus();   // keep focus for arrow-key navigation
    });
    updateCreatorHeader();
    refreshCreatorHighlights();   // outline the card matching the active theme
}

// Header: "<Category>  (count) - <selected option>" (parity with the Equipment LOOKS header).
void WardrobeTab2::updateCreatorHeader()
{
    if (!m_creatorHeader) return;
    QComboBox* c = m_creator[m_activeCreator];
    if (!c) return;
    const QString sel = c->currentIndex() <= 0 ? QStringLiteral("(default)") : c->currentText();
    m_creatorHeader->setText(QStringLiteral("%1  (%2) - %3")
        .arg(QString::fromLatin1(kCreator[m_activeCreator].label)).arg(c->count() - 1).arg(sel));
}

void WardrobeTab2::refreshCreatorCells()
{
    for (int i = 0; i < 9; ++i) {
        if (!m_creatorCells[i] || !m_creator[i]) continue;
        const QString stem = m_creator[i]->currentData().toString();
        const QImage img = creatorIconImage(i, stem);
        m_creatorCells[i]->setIcon(img.isNull() ? QIcon() : QIcon(QPixmap::fromImage(img)));
        // Tooltip: category + the selected item's name/file (if any).
        if (m_creator[i]->currentIndex() > 0)
            m_creatorCells[i]->setToolTip(infoTip(0, QString::fromLatin1(kCreator[i].label)
                                          + QStringLiteral(": ") + m_creator[i]->currentText(), QString(), stem));
        else
            m_creatorCells[i]->setToolTip(QString::fromLatin1(kCreator[i].label) + QStringLiteral(": (default)"));
    }
}

// Reset every wardrobe selection (and its saved state) to defaults.
void WardrobeTab2::resetDefaults()
{
    QSettings s;
    for (const QString& k : {QStringLiteral("wardrobe2/class"), QStringLiteral("wardrobe2/gender"),
                             QStringLiteral("wardrobe2/weaponType"), QStringLiteral("wardrobe2/weapon"),
                             QStringLiteral("wardrobe2/weaponType2"), QStringLiteral("wardrobe2/weapon2"),
                             QStringLiteral("wardrobe2/weaponSheath"), QStringLiteral("wardrobe2/weaponSheath2"),
                             QStringLiteral("wardrobe2/backTrophy"), QStringLiteral("wardrobe2/dyeSel")})
        s.remove(k);
    for (int i = 0; i < 5; ++i) s.remove(QStringLiteral("wardrobe2/slot/%1").arg(i));
    for (int i = 0; i < 9; ++i) s.remove(QStringLiteral("wardrobe2/creator/%1").arg(i));
    for (int i = 0; i < kSlotCount; ++i) {
        s.remove(QStringLiteral("wardrobe2/dye/%1").arg(i));
        s.remove(QStringLiteral("wardrobe2/dyehex/%1").arg(i));
        m_slotDye[i] = {};
    }

    m_restoring = true;
    m_class->setCurrentIndex(0);
    m_gender->setCurrentIndex(0);
    for (int i = 0; i < 5; ++i) m_slot[i]->setCurrentIndex(0);
    for (int i = 0; i < 9; ++i) m_creator[i]->setCurrentIndex(0);
    if (m_weaponType->count() > 0) m_weaponType->setCurrentIndex(0);
    if (m_weapon->count() > 0) m_weapon->setCurrentIndex(0);
    if (m_weaponType2 && m_weaponType2->count() > 0) m_weaponType2->setCurrentIndex(0);
    if (m_weapon2 && m_weapon2->count() > 0) m_weapon2->setCurrentIndex(0);
    if (m_weapon3 && m_weapon3->count() > 0) m_weapon3->setCurrentIndex(0);
    if (m_weapon4 && m_weapon4->count() > 0) m_weapon4->setCurrentIndex(0);
    if (m_backTrophy && m_backTrophy->count() > 0) m_backTrophy->setCurrentIndex(0);
    if (m_dyeCombo && m_dyeCombo->count() > 0) m_dyeCombo->setCurrentIndex(0);
    if (m_env) m_env->setCurrentIndex(1);
    m_restoring = false;

    m_activeTheme = ThemeResolved{};   // nothing is "themed" after a reset
    populateCreator();
    populateSlots();   // ends with rebuildOutfit()
    refreshLookHighlights();
    refreshCreatorHighlights();
}

// Pick a random appearance for each armour slot + both weapon hands, then rebuild once.
// (randomizeOutfit removed — the "Randomize" affordance left the UI long ago; dead code.)

// Group the per-slot appearances into sets that span 2+ slots, by collection (AppearanceMeta)
// and by name token (e.g. base01).
void WardrobeTab2::populateSets()
{
    // NOTE: m_sets is the source of truth for the right-click "Equip Theme" resolver — that's
    // why this survives even though the old "equip a set" dropdown UI is gone.
    m_sets.clear();
    // Collections whose label is shared by more than one real set in the SAME slot (e.g. a whole
    // battlepass season's worth of distinct armours all carry the Series "Season 13"). Such a
    // label isn't a single transmog set — keeping it would overwrite slot-by-slot until only the
    // last-inserted set survives, so clicking barF_stor269 would equip barF_stor270. We drop these
    // below and let the unique per-name token (stor269 vs stor270) be the source of truth.
    QSet<QString> ambiguousColl;
    const QString prefix = classPrefix();
    for (int i = 0; i < 5; ++i) {
        const QString slotSuffix = QStringLiteral("_") + QString::fromLatin1(kSlots[i].code).toLower();
        for (int r = 1; r < m_slot[i]->count(); ++r) {
            const QString name = m_slot[i]->itemText(r);
            const int sno = m_slot[i]->itemData(r).toInt();
            // name token: <prefix>_<token>_<slot>
            QString l = name.toLower();
            if (l.startsWith(prefix + QLatin1Char('_'))) l = l.mid(prefix.size() + 1);
            if (l.endsWith(slotSuffix)) l.chop(slotSuffix.size());
            if (!l.isEmpty()) m_sets[QStringLiteral("name: ") + l][i] = name;
            // real collection (transmog set name), when known
            const QString coll = AppearanceMeta::instance().collectionFor(sno);
            if (!coll.isEmpty()) {
                const QString key = QStringLiteral("set: ") + coll;
                auto& slotMap = m_sets[key];
                if (slotMap.contains(i) && slotMap.value(i) != name)
                    ambiguousColl.insert(key);   // two different appearances claim one slot ⇒ not a set
                slotMap[i] = name;
            }
        }
    }
    for (const QString& k : ambiguousColl) m_sets.remove(k);
    // (The old "equip a set" dropdown fill and applySet() are gone with their UI — equipping a
    //  whole set is the right-click "Equip Theme" action, which resolves through m_sets.)
}

// The shared theme id in an appearance name: strip the class/weapon-type prefix and the
// trailing slot suffix, leaving e.g. "stor007" (barf_stor007_TRS, sword_stor007 -> "stor007").
QString WardrobeTab2::themeToken(const QString& appearanceName) const
{
    QString l = appearanceName.toLower();
    const QString cp = classPrefix();
    if (l.startsWith(cp + QLatin1Char('_'))) {
        l = l.mid(cp.size() + 1);
    } else {
        for (const WeapTypeDef& w : kWeapTypes) {
            const QString p = QString::fromLatin1(w.prefix).toLower() + QLatin1Char('_');
            if (l.startsWith(p)) { l = l.mid(p.size()); break; }
        }
    }
    for (const SlotDef& s : kSlots) {
        const QString suf = QLatin1Char('_') + QString::fromLatin1(s.code).toLower();
        if (l.endsWith(suf)) { l.chop(suf.size()); break; }
    }
    return l;
}

// Read-only resolution of what a theme would equip (shared by the menu preview + equipTheme):
// the matching armour set + the store bundle's cosmetics + weapons.
WardrobeTab2::ThemeResolved WardrobeTab2::resolveTheme(int sno, const QString& appearanceName)
{
    ThemeResolved r;
    const QString coll  = AppearanceMeta::instance().collectionFor(sno);
    const QString token = themeToken(appearanceName);
    QString setKey;
    if (!coll.isEmpty() && m_sets.contains(QStringLiteral("set: ") + coll))
        setKey = QStringLiteral("set: ") + coll;
    else if (!token.isEmpty() && m_sets.contains(QStringLiteral("name: ") + token))
        setKey = QStringLiteral("name: ") + token;
    if (!setKey.isEmpty())
        for (auto it = m_sets[setKey].constBegin(); it != m_sets[setKey].constEnd(); ++it)
            r.armor[it.key()] = it.value();

    const QString classCode = m_class ? m_class->currentData().toString().toLower() : QString();
    if (!classCode.isEmpty()) {
        // The clicked item's token finds its store bundle: an armour appearance BarF_storN maps
        // to Bundle_*_bar_storN (its chest product is Barbarian_N). Equipped weapons' tokens are
        // a fallback for items whose own token has no bundle. The bundle lists the set's marking
        // and weapons as sub-products (arBundledProducts), plus some cosmetics in tCustomizedPreview.
        QStringList tokens;
        if (!token.isEmpty()) tokens << token;
        auto addTok = [&](QComboBox* mc) {
            if (mc && mc->currentData().toInt() > 0) {
                const QString wt = themeToken(mc->currentData(Qt::UserRole + 1).toString());
                if (!wt.isEmpty() && !tokens.contains(wt)) tokens << wt;
            }
        };
        addTok(m_weapon); addTok(m_weapon2);
        QDir sp(Config::d4dataDir() + QStringLiteral("/json/base/meta/StoreProduct"));
        QStringList bundles;
        for (const QString& tk : tokens) {
            bundles = sp.entryList({ QStringLiteral("*_%1_%2.prd.json").arg(classCode, tk) }, QDir::Files);
            if (!bundles.isEmpty()) break;
        }
        static const struct { const char* field; int cat; } kCP[] = {
            {"snoFace", 0}, {"snoHairStyle", 1}, {"snoHairColor", 2}, {"snoEyeColor", 3},
            {"snoFacialHair", 4}, {"snoMakeup", 5}, {"snoMarkingShape", 6},
            {"snoMarkingColor", 7}, {"snoJewelry", 8}
        };
        auto readName = [](const QJsonObject& o, const char* k) {
            return o.value(QLatin1String(k)).toObject().value(QStringLiteral("name")).toString();
        };
        auto isWeap = [](const QString& low) {
            for (const WeapTypeDef& w : kWeapTypes)
                if (low.startsWith(QString::fromLatin1(w.prefix).toLower() + QLatin1Char('_'))) return true;
            return false;
        };
        for (const QString& bn : bundles) {
            QFile f(sp.filePath(bn));
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            const QJsonObject cp = obj.value(QStringLiteral("tCustomizedPreview")).toObject();
            for (const auto& m : kCP) {
                const QString nm = readName(cp, m.field);
                if (!nm.isEmpty() && !r.creator.contains(m.cat)) r.creator[m.cat] = nm;
            }
            for (const QJsonValue& pv : obj.value(QStringLiteral("arBundledProducts")).toArray()) {
                const QString pn = pv.toObject().value(QStringLiteral("name")).toString();
                if (pn.isEmpty()) continue;
                const QString low = pn.toLower();
                if (isWeap(low)) { if (!r.weapons.contains(pn)) r.weapons << pn; continue; }
                if (low.contains(QStringLiteral("marking"))) {
                    // A marking sub-product resolves to the real MarkingShape (+ colour).
                    QFile mf(sp.filePath(pn + QStringLiteral(".prd.json")));
                    if (mf.open(QIODevice::ReadOnly)) {
                        const QJsonObject mo = QJsonDocument::fromJson(mf.readAll()).object();
                        mf.close();
                        const QString sh = readName(mo, "snoMarkingShape");
                        const QString cl = readName(mo, "snoMarkingColor");
                        if (!sh.isEmpty() && !r.creator.contains(6)) r.creator[6] = sh;
                        if (!cl.isEmpty() && !r.creator.contains(7)) r.creator[7] = cl;
                    }
                }
            }
        }
    }
    return r;
}

// Readable, comma-joined preview of a theme scope for the context-menu labels.
QString WardrobeTab2::themeNames(const ThemeResolved& r, ThemeScope scope)
{
    AppearanceMeta& am = AppearanceMeta::instance();
    const QString d4 = Config::d4dataDir();
    const bool male = m_gender && m_gender->currentData().toString() == QLatin1String("m");
    auto join = [](QStringList l) -> QString {
        l.removeDuplicates();
        if (l.isEmpty()) return QStringLiteral("None");
        if (l.size() > 3) { const int more = l.size() - 3; l = l.mid(0, 3); l << QStringLiteral("+%1 more").arg(more); }
        return l.join(QStringLiteral(", "));
    };
    if (scope == ThemeArmor) {
        QStringList names;
        for (auto it = r.armor.constBegin(); it != r.armor.constEnd(); ++it) {
            int s = 0;
            if (m_slot[it.key()]) { const int i = m_slot[it.key()]->findText(it.value()); if (i >= 0) s = m_slot[it.key()]->itemData(i).toInt(); }
            const QString t = s ? am.titleFor(s) : QString();
            names << (t.isEmpty() ? it.value() : t);
        }
        return join(names);
    }
    if (scope == ThemeMarkings) {
        QStringList names;
        if (r.creator.contains(6)) {
            const QString stem = r.creator.value(6);
            const QString rn = creatorRealName(d4, kCreator[6].folder, stem, male);
            names << (rn.isEmpty() ? stem : rn);
        }
        return join(names);
    }
    if (scope == ThemeWeapons) {
        QStringList names;
        for (const QString& wn : r.weapons) {
            int s = 0;
            for (QComboBox* mc : { m_weapon, m_weapon2 }) {
                if (!mc) continue;
                const int i = mc->findData(wn, Qt::UserRole + 1, Qt::MatchFixedString);
                if (i > 0) { s = mc->itemData(i).toInt(); break; }
            }
            const QString t = s ? am.titleFor(s) : QString();
            names << (t.isEmpty() ? wn : t);
        }
        return join(names);
    }
    QStringList s;   // ThemeAll: short count summary
    if (!r.armor.isEmpty())   s << QStringLiteral("%1 armor").arg(r.armor.size());
    if (!r.weapons.isEmpty()) s << QStringLiteral("%1 weapons").arg(r.weapons.size());
    if (r.creator.contains(6) || r.creator.contains(7)) s << QStringLiteral("markings");
    return s.isEmpty() ? QStringLiteral("None") : s.join(QStringLiteral(", "));
}

// Right-click -> equip a theme. `scope` selects which parts: armour set (by collection or
// name token), and/or the store bundle's markings/cosmetics + weapons (found via the clicked
// token, else the equipped weapons' tokens). Everything is already class-filtered.
void WardrobeTab2::equipTheme(int sno, const QString& appearanceName, ThemeScope scope)
{
    const QString token = themeToken(appearanceName);
    const ThemeResolved r = resolveTheme(sno, appearanceName);   // single source of truth (== menu preview)
    m_activeTheme = r;   // remember the theme so matching items stay highlighted
    const bool doArmor = (scope == ThemeAll || scope == ThemeArmor);
    const bool doMark  = (scope == ThemeAll || scope == ThemeMarkings);
    const bool doWeap  = (scope == ThemeAll || scope == ThemeWeapons);
    m_restoring = true;
    QStringList matched;   // categories equipped, for the status line
    int count = 0;

    // ── Armour: the matching set's pieces. ──
    if (doArmor) {
        int n = 0;
        for (auto it = r.armor.constBegin(); it != r.armor.constEnd(); ++it) {
            QComboBox* mc = m_slot[it.key()];
            const int idx = mc ? mc->findText(it.value()) : -1;
            if (idx >= 0) {
                mc->setCurrentIndex(idx);
                QSettings().setValue(QStringLiteral("wardrobe2/slot/%1").arg(it.key()), it.value());
                ++n;
            }
        }
        if (n) { matched << QStringLiteral("armor"); count += n; }
    }

    // ── Markings / cosmetics from the resolved store bundle. ──
    if (doMark) {
        int marks = 0;
        for (auto it = r.creator.constBegin(); it != r.creator.constEnd(); ++it) {
            const int cat = it.key();
            if (scope == ThemeMarkings && cat != 6 && cat != 7) continue;   // markings only = shape + colour
            QComboBox* cc = m_creator[cat];
            if (!cc) continue;
            const QString nm = it.value();
            // Find an existing item by data (either role) or visible text before adding a synthetic
            // one, so repeated theme equips don't pile up duplicate store-marking entries.
            int idx = cc->findData(nm, Qt::UserRole, Qt::MatchFixedString);
            if (idx < 0) idx = cc->findData(nm, Qt::UserRole + 1, Qt::MatchFixedString);
            if (idx < 0) idx = cc->findText(nm);
            if (idx <= 0) { cc->addItem(nm, nm); idx = cc->count() - 1; }   // add store-specific items
            cc->setCurrentIndex(idx);
            QSettings().setValue(QStringLiteral("wardrobe2/creator/%1").arg(cat), cc->itemText(idx));
            ++marks;
        }
        if (marks) { matched << QStringLiteral("markings"); count += marks; }
    }

    // ── Weapons from the resolved store bundle (exact appearance names). ──
    if (doWeap && !r.weapons.isEmpty()) {
        auto equipW = [&](QComboBox* mc, const QString& key) -> bool {
            if (!mc) return false;
            for (const QString& wn : r.weapons) {
                const int i = mc->findData(wn, Qt::UserRole + 1, Qt::MatchFixedString);
                if (i > 0) { mc->setCurrentIndex(i); QSettings().setValue(key, mc->itemText(i)); return true; }
            }
            return false;
        };
        int w = 0;
        if (equipW(m_weapon,  QStringLiteral("wardrobe2/weapon")))  ++w;
        if (equipW(m_weapon2, QStringLiteral("wardrobe2/weapon2"))) ++w;
        if (w) { matched << QStringLiteral("weapons"); count += w; }
    }

    m_restoring = false;
    rebuildOutfit();
    refreshSlotCells();
    if (m_creatorLayout) refreshCreatorCells();
    refreshLookSelection();   // update checkmarks in place (preserves scroll position)
    refreshLookHighlights();
    refreshCreatorHighlights();
    // Camera Snap: frame the parts this theme scope touched (full body for a full theme).
    // Overrides rebuildOutfit()'s per-slot snap so the framing matches what was equipped.
    if (m_d4View) frameThemeScope(scope);
    if (m_status) {
        if (count == 0)
            m_status->setText(QStringLiteral("No matching theme found for '%1'.").arg(token));
        else
            m_status->setText(QStringLiteral("Equipped %1 piece(s): %2").arg(count).arg(matched.join(QStringLiteral(", "))));
    }
}

// Detect the discrete dye-mask value bands the artist painted, straight from the DYE_MASK texture.
// D4's dye mask stores each material zone as a specific grey level (single-channel BC4). These
// levels vary per armor (barF_sets54 clusters near 0.06/0.10/0.29/0.58; others sit elsewhere), so
// hardcoding band centres is wrong — we read the actual histogram, merge nearby bins into clusters,
// and return the (up to) four most-populated centres sorted ascending. Fewer than four → the last
// centre is repeated so the extra zones collapse onto it. This is the game-data source of truth.
static QVector4D detectDyeBands(const QImage& dyeMaskIn)
{
    QVector4D def(0.063f, 0.345f, 0.596f, 0.831f);   // shipped fallback when there's no usable mask
    if (dyeMaskIn.isNull()) return def;
    const QImage m = dyeMaskIn.convertToFormat(QImage::Format_RGBA8888);
    const int W = m.width(), H = m.height();
    if (W < 2 || H < 2) return def;
    long hist[256] = {0}; long total = 0;
    const int sx = qMax(1, W / 256), sy = qMax(1, H / 256);   // sparse sample: fast, plenty accurate
    for (int y = 0; y < H; y += sy) {
        const uchar* s = m.constScanLine(y);
        for (int x = 0; x < W; x += sx) { ++hist[s[x * 4]]; ++total; }
    }
    if (total < 16) return def;
    // Merge adjacent populated bins (gap < 6) into clusters; a bin counts if it holds >0.4% of samples.
    const long minPop = qMax(2L, long(total / 250));
    struct Cl { double sum = 0; long pop = 0; };
    QVector<Cl> clusters;
    int lastBin = -100;
    for (int b = 0; b < 256; ++b) {
        if (hist[b] < minPop) continue;
        if (b - lastBin > 6 || clusters.isEmpty()) clusters.append(Cl{});
        Cl& c = clusters.last(); c.sum += double(b) * hist[b]; c.pop += hist[b];
        lastBin = b;
    }
    if (clusters.isEmpty()) return def;
    std::sort(clusters.begin(), clusters.end(), [](const Cl& a, const Cl& b) { return a.pop > b.pop; });
    if (clusters.size() > 4) clusters.resize(4);              // keep the four most-populated zones
    QVector<float> centres;
    for (const Cl& c : clusters) centres.append(float(c.sum / double(c.pop) / 255.0));
    std::sort(centres.begin(), centres.end());
    while (centres.size() < 4) centres.append(centres.last());
    return QVector4D(centres[0], centres[1], centres[2], centres[3]);
}

// Derive the dye-zone → detail-map table from which maps are actually present. Non-metal maps are
// assigned to zones 1..3 in slot order and CLAMPED to the last one (so a 4th zone with only two
// leather/fabric maps reuses the second); the metal map is excluded here (metalness routes metal
// texels to it in the shader). zone0 is the bare/lowest band → no detail. Returns 4 layers as floats.
static QVector4D deriveZoneMap(const QVector<QImage>& detailN, int metalLayer)
{
    QVector<int> nm;                                          // present NON-metal detail-map slot indices
    for (int i = 0; i < detailN.size() && i < 3; ++i)
        if (!detailN[i].isNull() && i != metalLayer) nm.append(i);
    auto pick = [&](int zoneIdx) -> int {                    // zoneIdx 1..3 → non-metal map (clamped)
        if (nm.isEmpty()) return -1;
        return nm[qMin(zoneIdx - 1, nm.size() - 1)];
    };
    return QVector4D(float(-1), float(pick(1)), float(pick(2)), float(pick(3)));
}

// Coalesce rapid interactive selections (clicking through slots/creator/weapons) into a single
// rebuild after a short quiet period, so browsing a list doesn't stack one full rebuild per click.
void WardrobeTab2::scheduleRebuild()
{
    if (!m_loaded) return;
    QSettings s;
    const bool coalesce = s.value(QStringLiteral("wardrobe2/perf/coalesce"), true).toBool();
    if (!coalesce) {   // rebuild immediately (Settings → Performance)
        rebuildOutfitImpl(s.value(QStringLiteral("wardrobe2/perf/asyncLoad"), true).toBool());
        return;
    }
    if (!m_rebuildTimer) {
        m_rebuildTimer = new QTimer(this);
        m_rebuildTimer->setSingleShot(true);
        m_rebuildTimer->setInterval(35);
        connect(m_rebuildTimer, &QTimer::timeout, this, [this] {
            // Async by default: interactive rebuilds decode on a worker so clicking through slots
            // never blocks the UI (the init/restore path stays synchronous by design).
            rebuildOutfitImpl(QSettings().value(QStringLiteral("wardrobe2/perf/asyncLoad"), true).toBool());
        });
    }
    m_rebuildTimer->start();   // restart on each change → only the settled selection builds
}

void WardrobeTab2::rebuildOutfitImpl(bool async)
{
    // Re-entrancy guard. rebuildOutfit repopulates combos/lists/trees (populateMaterials,
    // rebuildPartList, populateAnims, applyDye…); those emit currentIndexChanged/itemChanged
    // signals which are wired back to rebuildOutfit. Without this guard a rebuild re-enters
    // itself mid-flight, recursing and corrupting the shared parts/merge/export state —
    // the intermittent startup crash. Coalesce: ignore nested calls.
    if (m_rebuilding) return;
    m_rebuilding = true;
    struct RebuildGuard { bool& f; ~RebuildGuard() { f = false; } } rebuildGuard{m_rebuilding};

    if (!m_view) return;
    if (!m_reader || !m_reader->isReady()) { if (m_status) m_status->setText(QStringLiteral("CASC not ready.")); return; }
    QElapsedTimer geomT; geomT.start();   // stage timing → "wardrobe: rebuild …" log line

    // ── Outfit history (Ctrl+Z): the state that led to the PREVIOUS build is what undo returns
    // to, so push it when the current state differs. Undo replays suppress pushes (they'd
    // immediately re-record the state being undone).
    if (!m_undoApplying) {
        const QHash<QString, QVariant> cur = snapshotLookState();
        if (!m_lastLookState.isEmpty() && cur != m_lastLookState) {
            m_undoStack.append(m_lastLookState);
            if (m_undoStack.size() > 30) m_undoStack.removeFirst();
        }
        m_lastLookState = cur;
    }
    if (!m_reflLoaded) { m_reflLoaded = true; loadReflectionProbe(); }   // once, now CASC is ready
    // Preserve the running animation across the rebuild (don't reset it on equip/creator
    // changes) — captured here, re-applied on the new geometry at the end.
    const QString keepAnim = m_playingAnim;
    const int keepFrame = m_animSlider ? m_animSlider->value() : 0;
    const bool wasPlaying = m_animTimer && m_animTimer->isActive();
    const QString d4 = Config::d4dataDir();

    QVector<ModelGeometry> parts;
    QVector<int> primSlot;   // per-primitive slot tag (0..4 armour, 5/6 weapons, -1 = not dyeable)
    QVector<QPair<QString, int>> pieceList;   // (display name, primitive count) for the parts tree
    QVector<int> pieceSno;                    // parallel: the appearance SNO each piece came from
    int pieceCount = 0;
    qint64 totalV = 0, totalT = 0;
    QString skelDbg;
    QString loadLog;       // per-piece result, shown in the status label (reliable channel)
    QString bodySkinMat;   // the face's real body-skin material (for test999's placeholder skin)
    int baseOutfitCount = 0;   // warlock: number of base00 whole-body primitives (they lead `parts`)
    const QString prefix = classPrefix();

    // ── Crash breadcrumb ──────────────────────────────────────────────────────
    // Record the exact models about to load, flushed to disk. The scope guard clears it on every
    // return (success or early-out). If the app CRASHES mid-load the breadcrumb survives, so the
    // next startup (MainWindow) can identify the culprit and auto-recover to the Files tab,
    // clearing only the offending selection. This is what makes a load crash self-healing.
    {
        QStringList loading;
        for (int i = 0; i < 5; ++i)
            if (m_slot[i] && m_slot[i]->currentData().toInt() > 0)
                loading << (QString::fromLatin1(kSlots[i].label) + QLatin1Char(':') + m_slot[i]->currentText());
        if (m_weapon && m_weapon->currentData().toInt() > 0)  loading << (QStringLiteral("MAIN:") + m_weapon->currentText());
        if (m_weapon2 && m_weapon2->currentData().toInt() > 0) loading << (QStringLiteral("OFF:") + m_weapon2->currentText());
        QSettings s;
        s.setValue(QStringLiteral("wardrobe2/_loading"), prefix + QStringLiteral(" | ") + loading.join(QStringLiteral(", ")));
        s.sync();
    }
    struct LoadCrumb { ~LoadCrumb() { QSettings s; s.remove(QStringLiteral("wardrobe2/_loading")); s.sync(); } } loadCrumb;

    // Resolve an appearance SNO by exact lowercase name via the SNO index.
    auto resolveApp = [&](const QString& wantLower, int& outSno, QString& outName) {
        outSno = 0;
        for (const SnoEntry& e : m_index->entries(kGroupAppearance))
            if (e.name.toLower() == wantLower) { outSno = e.snoId; outName = e.name; return; }
    };

    // Parse + register a piece into `parts`. Returns true only if it produced geometry,
    // so callers can fall through to the next candidate when a parse fails. Logs each step.
    auto addPiece = [&](int sno, const QString& nameIn, bool isBody, int slot = -1) -> bool {
        if (sno <= 0) return false;
        // Resolve a missing name from the SNO index (an empty name crashed appearanceRoster).
        QString name = nameIn;
        if (name.isEmpty() && m_index)
            for (const SnoEntry& e : m_index->entries(kGroupAppearance))
                if (e.snoId == sno) { name = e.name; break; }
        if (name.isEmpty()) { loadLog += QStringLiteral("\nsno %1: unnamed, skipped").arg(sno); return false; }
        const QByteArray meta = m_reader->readMetaBySno(quint64(sno));
        const QByteArray payload = m_reader->readPayloadBySno(quint64(sno));
        if (meta.isEmpty() || payload.isEmpty()) {
            loadLog += QStringLiteral("\n%1: NO DATA (pay=%2)").arg(name).arg(payload.size());
            return false;
        }
        ModelGeometry geo;   // guarded: a malformed equipped piece must not crash the process
        if (!seh::runGuarded("w2Parse", [&]() { geo = ModelParser::parseApp(meta, payload, name); })) {
            loadLog += QStringLiteral("\n%1: parse FAULT (pay=%2)").arg(name).arg(payload.size());
            return false;
        }
        if (!geo.valid) {
            loadLog += QStringLiteral("\n%1: parse INVALID (pay=%2)").arg(name).arg(payload.size());
            return false;
        }
        const QStringList roster = MaterialDecode::appearanceRoster(d4, name);
        for (MeshPrimitive& p : geo.primitives) p.materialName = roster.value(p.materialIndex);
        // Remember the face's real body-skin material (…_BOD) — test999 body pieces use a
        // black placeholder (armor_skin_mat), so we re-skin them with this.
        if (isBody && bodySkinMat.isEmpty())
            for (const MeshPrimitive& p : geo.primitives)
                if (p.materialName.contains(QLatin1String("_BOD"), Qt::CaseInsensitive)
                    && !p.materialName.contains(QLatin1String("_body"), Qt::CaseInsensitive)      // NOT demonform_body
                    && !p.materialName.contains(QLatin1String("demonform"), Qt::CaseInsensitive)) { bodySkinMat = p.materialName; break; }
        int nv = 0, nt = 0;
        for (const MeshPrimitive& p : geo.primitives) { nv += int(p.vertices.size()); nt += int(p.indices.size()) / 3; }
        totalV += nv; totalT += nt;
        loadLog += QStringLiteral("\n%1: OK prim=%2 v=%3 t=%4 bones=%5")
                       .arg(name).arg(geo.primitives.size()).arg(nv).arg(nt).arg(geo.skeleton.size());
        if (isBody && !geo.skeleton.isEmpty() && m_bodySkeleton.isEmpty()) {   // base00 rig wins; face is only the fallback
            m_bodySkeleton = geo.skeleton;
            QStringList cand;
            for (const ModelJoint& j : geo.skeleton) {
                const QString n = j.name.toLower();
                if (n.contains(QLatin1String("weap")) || n.contains(QLatin1String("hand"))
                    || n.contains(QLatin1String("hardpoint")) || n.contains(QLatin1String("attach"))
                    || n.contains(QLatin1String("wrist")) || n.contains(QLatin1String("grip"))
                    || n.contains(QLatin1String("hold")) || n.contains(QLatin1String("prop")))
                    cand << j.name;
            }
            skelDbg = QStringLiteral("\nbody skel: %1 bones · hardpoint candidates: %2")
                          .arg(geo.skeleton.size())
                          .arg(cand.isEmpty() ? QStringLiteral("(none)") : cand.join(QStringLiteral(", ")));
        }
        pieceList.append({name, int(geo.primitives.size())});
        pieceSno.append(sno);
        for (int pi = 0; pi < geo.primitives.size(); ++pi) primSlot.append(slot);   // dye-slot tag
        parts.append(geo);
        ++pieceCount;
        return true;
    };

    if (m_index && m_index->isLoaded()) {
        // BASE RIG: load the canonical base body <pref>_base00 FIRST as the SKELETON carrier. It
        // owns the full animation rig (the clips are authored for it) and the weapon hardpoints.
        // The face piece is head-only, so using it as the rig carrier leaves nBaseBones = just the
        // head bones → every body bone (added later) is misclassified as cloth (broken physics on
        // some rigs, e.g. Warlock male) and full-body clips misalign. We keep only base00's
        // skeleton (drop its mesh, so no duplicate body is drawn); the head/body/armor meshes skin
        // onto this rig by bone-name hash. Fall back to the face's own skeleton if base00 is absent.
        m_bodySkeleton.clear();
        bool baseIsOutfit = false;   // warlock: base00's whole-body mesh IS the default outfit
        {
            int b0 = 0; QString b0Name;
            resolveApp(prefix + QStringLiteral("_base00"), b0, b0Name);
            if (b0 > 0) {
                const QByteArray bm = m_reader->readMetaBySno(quint64(b0));
                const QByteArray bp = m_reader->readPayloadBySno(quint64(b0));
                if (!bm.isEmpty() && !bp.isEmpty()) {
                    ModelGeometry bg;   // guarded parse of the base-body appearance
                    seh::runGuarded("w2ParseBase", [&]() { bg = ModelParser::parseApp(bm, bp, b0Name); });
                    if (bg.valid && !bg.skeleton.isEmpty()) {
                        m_bodySkeleton = bg.skeleton;       // hardpoints/anim rig come from base00
                        // Warlock ships base00 as its actual default look, so render its mesh as the
                        // default body. Other classes use base00 for the RIG ONLY (skeleton) and keep
                        // the per-slot base body, so the whole-body base00 mesh isn't drawn for them.
                        const bool warlock = prefix.startsWith(QLatin1String("war"));
                        if (warlock) {
                            const QStringList roster = MaterialDecode::appearanceRoster(d4, b0Name);
                            for (MeshPrimitive& p : bg.primitives) p.materialName = roster.value(p.materialIndex);
                            if (bodySkinMat.isEmpty())
                                for (const MeshPrimitive& p : bg.primitives)
                                    if (p.materialName.contains(QLatin1String("_BOD"), Qt::CaseInsensitive)
                    && !p.materialName.contains(QLatin1String("_body"), Qt::CaseInsensitive)      // NOT demonform_body
                    && !p.materialName.contains(QLatin1String("demonform"), Qt::CaseInsensitive)) { bodySkinMat = p.materialName; break; }
                            for (int pi = 0; pi < bg.primitives.size(); ++pi) primSlot.append(-1);
                            pieceList.append({ b0Name, int(bg.primitives.size()) });
                            pieceSno.append(b0);
                            baseIsOutfit = true;
                            baseOutfitCount = int(bg.primitives.size());   // these lead `parts`/merged
                        } else {
                            bg.primitives.clear();          // skeleton only — don't draw base00's mesh
                        }
                        parts.append(bg);                   // FIRST piece → nBaseBones = full base rig
                        loadLog += QStringLiteral("\nbase rig=%1 sno=%2 bones=%3 outfit=%4")
                                       .arg(b0Name).arg(b0).arg(bg.skeleton.size()).arg(baseIsOutfit ? 1 : 0);
                    }
                }
            }
        }
        // FACE = head + skeleton/anim carrier (head only — NOT a full body). The face
        // ethnicity → base appearance suffix via the Face def's dwSubObjectStyle
        // (Caucasian=0→P00, Asian=1→P01, African=2→P02, Persian=3→P03).
        int faceStyle = styleOf(d4, "Face", ".fac.json",
                                m_creator[0] ? m_creator[0]->currentData().toString() : QString());
        if (faceStyle < 0) faceStyle = 0;
        const QString suffix = QStringLiteral("_p") + QString::number(faceStyle).rightJustified(2, QLatin1Char('0'));
        int baseSno = 0; QString baseName;
        resolveApp(prefix + suffix, baseSno, baseName);
        if (!baseSno)   // irregular face naming, e.g. druM_newModel_P00
            for (const SnoEntry& e : m_index->entries(kGroupAppearance)) {
                const QString l = e.name.toLower();
                if (l.startsWith(prefix) && l.endsWith(suffix)) { baseSno = e.snoId; baseName = e.name; break; }
            }
        loadLog += QStringLiteral("\nface want=%1%2 -> sno=%3").arg(prefix).arg(suffix).arg(baseSno);
        addPiece(baseSno, baseName, true);

        // Body slots: equipped armor; else the first nude/underwear token that PARSES.
        // Armor carries its own skin_mat, so it replaces the nude piece (never both).
        // test999 = true nude; base01/02/03 = underwear fallback the game ships.
        // Nude base setting: test999 = true nude (first); off → default base (base01 underwear).
        const bool nudeBase = QSettings().value(QStringLiteral("wardrobe2/nudeBase"), false).toBool();
        static const char* kNude[]   = {"test999", "base01", "base02", "base03"};
        static const char* kClothed[] = {"base01", "base02", "base03", "test999"};
        const char* const* kNudeTokens = nudeBase ? kNude : kClothed;
        const int kNudeCount = 4;
        for (int i = 0; i < 5; ++i) {
            const int sno = m_slot[i]->currentData().toInt();
            const QString slotCode = QString::fromLatin1(kSlots[i].code).toLower();
            if (sno > 0) { addPiece(sno, m_slot[i]->currentText(), false, i); continue; }   // dyeable armour
            if (i == 0) continue;   // HLM: bare head comes from the FACE piece
            if (baseIsOutfit) continue;   // warlock: base00's whole-body mesh already supplies this slot
            for (int ti = 0; ti < kNudeCount; ++ti) {
                const char* tok = kNudeTokens[ti];
                int nudeSno = 0; QString nudeName;
                resolveApp(prefix + QStringLiteral("_") + QLatin1String(tok) + QStringLiteral("_") + slotCode, nudeSno, nudeName);
                if (addPiece(nudeSno, nudeName, false)) break;   // first that parses wins
            }
        }
        // Back trophy (player back cosmetic): a "back_*" appearance rigged to the spine — merges
        // onto the body like an armour piece. Tagged slot 9 so framing/selection find it.
        if (m_backTrophy) {
            const int btSno = m_backTrophy->currentData().toInt();
            if (btSno > 0) addPiece(btSno, m_backTrophy->currentText(), false, 9);
        }
        // Creator mesh pieces (additive appearances, like equipment):
        //   Hair style → <pref>_H<NN>   ·   Jewelry → jwl<NN>_<pref>
        // where NN is the def's dwSubObjectStyle. (Face = the base; facial hair deferred.)
        auto loadCreatorMesh = [&](const QString& wantLower, const QString& label) {
            int s = 0; QString nm;
            resolveApp(wantLower, s, nm);
            loadLog += QStringLiteral("\n%1 want=%2 -> sno=%3").arg(label).arg(wantLower).arg(s);
            if (s) addPiece(s, nm, false);
        };
        const int hairStyle = styleOf(d4, "HairStyle", ".har.json",
                                      m_creator[1] ? m_creator[1]->currentData().toString() : QString());
        if (hairStyle >= 0)
            loadCreatorMesh(prefix + QStringLiteral("_h") + QString::number(hairStyle).rightJustified(2, QLatin1Char('0')),
                            QStringLiteral("hair"));
        // Facial hair (male beards) → the <pref>_B<NN> mesh, just like hair — previously only the
        // face's beard TEXTURE was swapped, so the actual beard geometry never showed. NN = the
        // FacialHair def's style; styles 0/1 (Clean/Stubble) have no mesh, and female has none, so
        // loadCreatorMesh no-ops there (the appearance simply doesn't exist).
        const bool maleFH = m_gender && m_gender->currentData().toString() == QLatin1String("m");
        if (maleFH) {
            const int fhStyle = facialHairStyle(d4, m_creator[4] ? m_creator[4]->currentData().toString() : QString());
            if (fhStyle >= 0)
                loadCreatorMesh(prefix + QStringLiteral("_b") + QString::number(fhStyle).rightJustified(2, QLatin1Char('0')),
                                QStringLiteral("facialhair"));
        }
        const int jwlStyle = styleOf(d4, "Jewelry", ".jwl.json",
                                     m_creator[8] ? m_creator[8]->currentData().toString() : QString());
        if (jwlStyle >= 0)
            loadCreatorMesh(QStringLiteral("jwl") + QString::number(jwlStyle).rightJustified(2, QLatin1Char('0'))
                                + QStringLiteral("_") + prefix,
                            QStringLiteral("jewelry"));
    }

    // Weapons: parse the main-hand + off-hand models and seat each on its real body
    // hardpoint (bone + grip transform) from game data — full rotation, not just position.
    QString weapDbg;
    const auto hpMap = loadBodyHardpoints(d4);
    const QString wgender = (m_gender && m_gender->currentData().toString() == QLatin1String("m"))
                                ? QStringLiteral("Male") : QStringLiteral("Female");
    // Held main/off (slots 5/6) seat in-hand; sheathed main/off (slots 7/8) seat rigidly on the
    // body sheath socket for their weapon class. `slot` is the dye/part tag so framing + selection work.
    struct HandSel { QComboBox* model; int hand; bool sheathed; int slot; };
    const HandSel hands[4] = {
        {m_weapon,  0, false, 5}, {m_weapon2, 1, false, 6},
        {m_weapon3, 0, true,  7}, {m_weapon4, 1, true,  8},
    };
    for (const HandSel& hs : hands) {
        const int sno = hs.model ? hs.model->currentData().toInt() : 0;
        if (sno <= 0) continue;
        const QString weapName = hs.model->currentData(Qt::UserRole + 1).toString();
        const QByteArray wm = m_reader->readMetaBySno(quint64(sno));
        const QByteArray wp = m_reader->readPayloadBySno(quint64(sno));
        ModelGeometry wgeo;   // guarded parse of the equipped weapon appearance
        if (!wm.isEmpty() && !wp.isEmpty())
            seh::runGuarded("w2ParseWeap", [&]() { wgeo = ModelParser::parseApp(wm, wp, weapName); });
        if (!wgeo.valid) continue;
        const QStringList wr = MaterialDecode::appearanceRoster(d4, weapName);
        for (MeshPrimitive& p : wgeo.primitives) p.materialName = wr.value(p.materialIndex);
        const QString wlabel = hs.model ? hs.model->currentData(Qt::UserRole + 2).toString() : QString();
        const WeapTypeDef* wt = weapTypeByLabel(wlabel);
        const QString itemType = wt ? QString::fromLatin1(wt->itemType) : QStringLiteral("Sword");
        const quint32 forceHash = hs.sheathed ? sheathHardpointFor(itemType, hs.hand == 1, hpMap) : 0u;
        seatWeapon(wgeo, hs.hand, itemType, wgender, hpMap, weapDbg, forceHash);
        for (const MeshPrimitive& p : wgeo.primitives) { totalV += p.vertices.size(); totalT += p.indices.size() / 3; }
        pieceList.append({weapName, int(wgeo.primitives.size())});
        pieceSno.append(sno);
        for (int pi = 0; pi < wgeo.primitives.size(); ++pi) primSlot.append(hs.slot);
        parts.append(wgeo);
    }

    if (parts.isEmpty()) {
        m_view->clearGeometry();
        m_view->setOverlayText(QStringLiteral("Pick a class and gender to start,\nor equip an item / load an ensemble."));
        m_status->setText(QStringLiteral("Nothing equipped yet — pick a class or equip an item."));
        return;
    }
    m_view->setOverlayText(QString());   // model present → clear any empty-state hint

    ModelGeometry merged;   // guarded: unifying skeletons/joints across the equipped pieces
    if (!seh::runGuarded("w2Merge", [&]() { merged = ModelParser::mergeGeometries(parts); }) || !merged.valid) {
        m_view->clearGeometry();
        m_view->setOverlayText(QStringLiteral("Couldn't assemble this outfit — skipped."));
        if (m_status) m_status->setText(QStringLiteral("Assembly failed."));
        return;
    }
    parts.clear(); parts.squeeze();   // per-piece geometry is now baked into `merged` — free the
                                      // duplicate vertex/index/skeleton copies before the texture phase
    fillClothSimTuning(merged);   // per-piece authored params from each sim's Cloth/<name>_sim.clt.json
    // NOTE: geometry is pushed to the viewport in applyOutfit(), NOT here — so on the async path the
    // current model keeps rendering (fully textured) until its replacement is decoded, then geometry
    // and textures swap together in one paint. No untextured-T-pose flash.

    // ── Creator colour selections (data-driven) ──
    auto sel = [&](int cat) { return m_creator[cat] ? m_creator[cat]->currentData().toString() : QString(); };
    const QColor hairTint = hairColorMid(d4, sel(2));                     // Hair colour (flat fallback)
    const QVector<QColor> hairRamp = hairColorRamp(d4, sel(2));           // full ramp → gradient-map
    const QColor irisCol  = eyeIrisColor(d4, sel(3));                     // Eye colour (iris inner)
    const QColor irisOuter = eyeIrisOuterColor(d4, sel(3));              // iris outer (limbus)
    // D4 eyes are rendered by the Hero_Eye shader off a SHARED set of global eye textures (the
    // eyeball material itself carries none). Real slots (from base/meta/Shader/Hero_Eye.shd):
    //   base_eyes_sclera (white)  · base_eyes_sclera_red (bloodshot)  · base_eyes_irisColor (iris)
    //   base_eyes_irisMask (iris footprint)  · BaseEye_Normal (surface normal).
    // We composite them exactly like the shader — iris masked over the sclera — instead of the old
    // flat iris fill, so the eye keeps its sclera. The EyeColor def drives the look: flScleraBrightness
    // (0.6 normal, 0.05 vampire → near-black sclera), flScleraRednessAmount (bloodshot), the iris
    // inner/outer gradient, the iris/sclera roughness, and flIrisEmissiveStrength (glowing eyes).
    QImage eyeBaseImg, eyeNormImg, eyeOrmImg, eyeEmisImg;
    float  eyeEmisMul = 0.0f, eyeIrisRough = 0.1f;
    composeEyeMaps(d4, sel(3), eyeBaseImg, eyeNormImg, eyeOrmImg, eyeEmisImg, eyeEmisMul, eyeIrisRough);
    if (m_view) m_view->setEyeParams(eyeIrisRough);                          // cornea wetness
    loadLog += QStringLiteral("\neye '%1': base=%2x%3 emisMul=%4")
                   .arg(sel(3)).arg(eyeBaseImg.width()).arg(eyeBaseImg.height()).arg(eyeEmisMul, 0, 'f', 2);
    // DIAGNOSTIC: which submeshes are treated as the eye (materialName contains "eyeball"), and any
    // face materials that look eye-related but WON'T be caught. Helps explain classes whose eyes
    // don't composite (rogue/sorcerer): if "eyeball parts=0" then their eyeball material is named
    // differently and the detection misses it.
    {
        int eyeParts = 0; QStringList eyeMats, otherEyeish;
        for (const MeshPrimitive& p : merged.primitives) {
            if (p.materialName.contains(QLatin1String("eyeball"), Qt::CaseInsensitive)) { ++eyeParts; if (!eyeMats.contains(p.materialName)) eyeMats << p.materialName; }
            else if (p.materialName.contains(QLatin1String("eye"), Qt::CaseInsensitive) && !otherEyeish.contains(p.materialName)) otherEyeish << p.materialName;
        }
        loadLog += QStringLiteral("\neyeball parts=%1 mats=[%2] other-eye-ish=[%3]")
                       .arg(eyeParts).arg(eyeMats.join(QStringLiteral(", "))).arg(otherEyeish.join(QStringLiteral(", ")));
    }
    const bool   maleGender = m_gender && m_gender->currentData().toString() == QLatin1String("m");
    QString fhMat = facialHairMat(d4, sel(4), maleGender);                // Facial hair shell material
    // Female default: the face piece can bake in a male-stubble placeholder, so with no facial
    // hair selected force the clean female material → a clean-shaven female by default. Try the
    // data-driven female-clean shell material, then the literal clean material name.
    const bool femaleDefaultFH = !maleGender && sel(4).isEmpty();
    if (fhMat.isEmpty() && femaleDefaultFH) {
        fhMat = femaleCleanFacialHairMat(d4);
        if (fhMat.isEmpty()) fhMat = QStringLiteral("Global_Female_Facialhair_00_Clean");
    }
    const QColor skinCol = (m_skinTone && m_skinTone->currentIndex() > 0)  // Skin tone swatch
                               ? QColor(m_skinTone->currentData().toString()) : QColor();
    float makeupInt = 1.0f;
    const QString makeupTex = makeupTexName(d4, sel(5), makeupInt);       // Makeup
    const MarkingDef mark = markingDef(d4, sel(6));                       // Marking shape
    // Marking colour: explicit pick, else the shape's default colour.
    const QString markColStem = !sel(7).isEmpty() ? sel(7) : mark.colorStem;
    MarkingPaint markPaint = markingPaint(d4, markColStem);   // ramp + roughness/metalness/tattoo
    // Some shapes author no default colour (snoDefaultColor null) and the user hasn't picked one →
    // the marking would render nothing. Fall back to a neutral charcoal ink so the design still
    // shows; a picked Marking colour overrides it.
    if (!sel(6).isEmpty() && !markPaint.valid) {
        markPaint.ramp = { QColor(20, 20, 22), QColor(40, 40, 44), QColor(72, 72, 78) };
        markPaint.valid = true;
        markPaint.isTattoo = true;
        markPaint.roughness = -1.0f;
        markPaint.metalness = -1.0f;
    }
    // VRAM-pool colour epoch: everything besides the material name that changes a baked per-part
    // texture — skin tone plus every creator pick (hair / eyes / makeup / markings / …) plus the
    // dye. Two parts with the same material AND the same epoch decode to identical textures, so the
    // GPU pool reuses them across rebuilds; a pure item-slot swap leaves this string unchanged.
    // Built on the main thread (reads widgets) and captured by the decode lambda.
    QString colorEpoch;
    {
        QStringList e;
        e << (skinCol.isValid() ? skinCol.name() : QStringLiteral("-"));
        for (int c = 0; c < 9; ++c) e << (m_creator[c] ? m_creator[c]->currentText() : QString());
        e << (m_dyeCombo ? m_dyeCombo->currentText() : QString());
        colorEpoch = e.join(QLatin1Char('\x1f'));
    }
    // Decode the makeup/marking overlay + mask textures once (resolve name → SNO).
    const QImage makeupImg = makeupTex.isEmpty() ? QImage() : MaterialDecode::texture(m_reader, d4, makeupTex, texSnoFor(makeupTex));
    const QImage markFaceImg = mark.faceTex.isEmpty() ? QImage() : MaterialDecode::texture(m_reader, d4, mark.faceTex, texSnoFor(mark.faceTex));
    const QImage markBodyImg = mark.bodyTex.isEmpty() ? QImage() : MaterialDecode::texture(m_reader, d4, mark.bodyTex, texSnoFor(mark.bodyTex));
    if (!sel(6).isEmpty()) {   // marking material diagnostic (surfaces the real authored values on-screen)
        const QImage& dmask = !markBodyImg.isNull() ? markBodyImg : markFaceImg;
        // Report the R (coverage) and G (ink→gold) channels separately — that's the real encoding.
        double sR = 0, sG = 0; long n = 0, covN = 0, goldN = 0;
        if (!dmask.isNull()) {
            const QImage m = dmask.convertToFormat(QImage::Format_RGBA8888);
            const int sxx = qMax(1, m.width()/128), syy = qMax(1, m.height()/128);
            for (int y = 0; y < m.height(); y += syy) { const uchar* s = m.constScanLine(y);
                for (int x = 0; x < m.width(); x += sxx) { const uchar* p = s + x*4;
                    sR += p[0]; sG += p[1]; ++n; if (p[0] >= 128) ++covN; if (p[1] >= 128) ++goldN; } }
        }
        const double mR = n ? sR/n : 0, mG = n ? sG/n : 0;
        const double covPct = n ? 100.0*covN/n : 0, goldPct = n ? 100.0*goldN/n : 0;
        auto hex = [](const QColor& c) { return c.isValid()
            ? QStringLiteral("%1%2%3").arg(c.red(),2,16,QLatin1Char('0')).arg(c.green(),2,16,QLatin1Char('0')).arg(c.blue(),2,16,QLatin1Char('0'))
            : QStringLiteral("----"); };
        // R=coverage channel (mean, %≥0.5), G=gold channel (mean, %≥0.5). ramp=shadow/mid/highlight.
        loadLog += QStringLiteral("\nMARK %1 col=%2 metal=%3 rough=%4 tat=%5 emis=%6 mask=%7x%8 R=%9(%10%%cov) G=%11(%12%%gold)\n"
                                  "     ramp=#%13 / #%14 / #%15")
                       .arg(sel(6)).arg(markColStem).arg(markPaint.metalness, 0, 'f', 2).arg(markPaint.roughness, 0, 'f', 2)
                       .arg(markPaint.isTattoo ? 1 : 0).arg(mark.emissive, 0, 'f', 2)
                       .arg(dmask.width()).arg(dmask.height())
                       .arg(mR, 0, 'f', 0).arg(covPct, 0, 'f', 1).arg(mG, 0, 'f', 0).arg(goldPct, 0, 'f', 1)
                       .arg(hex(markPaint.ramp[0])).arg(hex(markPaint.ramp[1])).arg(hex(markPaint.ramp[2]));
    }

    // Skin detail overlay (Freckle / Vitiligo): <pref>_<PXX>_BOD/HED_<Style>_color.
    const QString detailStyle = (m_skinDetail && m_skinDetail->currentIndex() > 0)
                                    ? m_skinDetail->currentData().toString() : QString();
    QImage detailBodyImg, detailHeadImg;
    if (!detailStyle.isEmpty()) {
        // Pull the overlay textures from the body/head skin materials (authoritative SNO).
        const QString bodyMat = bodySkinMat;                                   // e.g. barF_P00_BOD
        QString headMat = bodySkinMat; headMat.replace(QLatin1String("_BOD"), QLatin1String("_HED"), Qt::CaseInsensitive);
        const auto bt = matTexContaining(d4, bodyMat, detailStyle);
        const auto ht = matTexContaining(d4, headMat, detailStyle);
        detailBodyImg = MaterialDecode::texture(m_reader, d4, bt.first, bt.second);
        detailHeadImg = MaterialDecode::texture(m_reader, d4, ht.first, ht.second);
        loadLog += QStringLiteral("\nskinDetail %1: BOD(%2) %3 sno=%4 img=%5x%6 · HED(%7) %8 sno=%9")
                       .arg(detailStyle).arg(bodyMat).arg(bt.first).arg(bt.second)
                       .arg(detailBodyImg.width()).arg(detailBodyImg.height())
                       .arg(headMat).arg(ht.first).arg(ht.second);
    }

    auto tintImage = [](QImage img, const QColor& c) {
        if (!c.isValid() || img.isNull()) return img;
        img = img.convertToFormat(QImage::Format_RGBA8888);
        const float rf = c.redF(), gf = c.greenF(), bf = c.blueF();
        for (int y = 0; y < img.height(); ++y) {
            uchar* s = img.scanLine(y);
            for (int x = 0; x < img.width(); ++x) {
                s[x*4+0] = uchar(qBound(0, int(s[x*4+0] * rf), 255));
                s[x*4+1] = uchar(qBound(0, int(s[x*4+1] * gf), 255));
                s[x*4+2] = uchar(qBound(0, int(s[x*4+2] * bf), 255));
            }
        }
        return img;
    };
    // Recolour skin to a target tone while preserving the texture's luminance detail
    // (lum × hue-normalized target) — a faithful approximation of the parametric HSV system.
    auto skinRecolor = [](QImage img, const QColor& c) {
        if (!c.isValid() || img.isNull()) return img;
        const float cl = c.red()*0.30f + c.green()*0.59f + c.blue()*0.11f;
        if (cl < 1.0f) return img;
        const float nr = c.red()/cl, ng = c.green()/cl, nb = c.blue()/cl;
        img = img.convertToFormat(QImage::Format_RGBA8888);
        for (int y = 0; y < img.height(); ++y) {
            uchar* s = img.scanLine(y);
            for (int x = 0; x < img.width(); ++x) {
                const float lum = s[x*4+0]*0.30f + s[x*4+1]*0.59f + s[x*4+2]*0.11f;
                s[x*4+0] = uchar(qBound(0, int(lum*nr), 255));
                s[x*4+1] = uchar(qBound(0, int(lum*ng), 255));
                s[x*4+2] = uchar(qBound(0, int(lum*nb), 255));
            }
        }
        return img;
    };
    // Hair opacity lives in role-19 (a separate _Alpha mask for short styles, or the
    // _Color's alpha). When the decoded alpha is fully opaque, derive it from the strand
    // mask's luminance so the shader's alpha-cutout shows the hair gaps.
    auto deriveHairAlpha = [](QImage img) {
        if (img.isNull()) return img;
        img = img.convertToFormat(QImage::Format_RGBA8888);
        const int W = img.width(), H = img.height();
        bool opaque = true;
        for (int y = 0; y < H && opaque; ++y) {
            const uchar* s = img.constScanLine(y);
            for (int x = 0; x < W; ++x) if (s[x*4+3] < 250) { opaque = false; break; }
        }
        if (opaque)
            for (int y = 0; y < H; ++y) {
                uchar* s = img.scanLine(y);
                for (int x = 0; x < W; ++x)
                    s[x*4+3] = uchar((s[x*4+0]*30 + s[x*4+1]*59 + s[x*4+2]*11) / 100);
            }
        return img;
    };
    static const QRegularExpression kHairRx(QStringLiteral("_h\\d\\d"), QRegularExpression::CaseInsensitiveOption);

    // Snapshot the geometry-phase context, then decode the per-part textures. The decode loop below
    // is thread-safe (free functions + the QMutex-guarded reader; no widgets / GL / mutable members),
    // so when async it runs on a worker thread and the apply (GL/UI) runs back on the main thread.
    WardrobeBuildCtx ctx;
    ctx.merged = merged; ctx.primSlot = primSlot; ctx.baseOutfitCount = baseOutfitCount;
    ctx.loadLog = loadLog; ctx.skelDbg = skelDbg; ctx.weapDbg = weapDbg; ctx.pieceList = pieceList; ctx.pieceSno = pieceSno;
    ctx.keepAnim = keepAnim; ctx.keepFrame = keepFrame; ctx.wasPlaying = wasPlaying;
    ctx.pieceCount = pieceCount; ctx.totalV = totalV; ctx.totalT = totalT;
    const qint64 tGeom = geomT.elapsed();   // geometry parse/merge phase done
    const int gen = ++m_buildGen;
    // Temporary decode-pool toggle (Settings > Performance). When on, RAW (pre-recolour) decodes
    // are reused across rebuilds from the bounded LRU so a single-slot change skips re-decoding
    // unchanged materials. Read once here and captured by the decode lambda.
    const bool useLru = QSettings().value(QStringLiteral("wardrobe2/perf/texCache"), true).toBool();
    if (!useLru) { QMutexLocker lock(&m_texCacheMutex); m_texCache.clear(); }   // toggle off → release pooled VRAM-alike memory
    auto decode = [=]() -> WardrobeOutfitMaps {
        QString loadLog;   // loop-local: fur diagnostics only (merged with ctx.loadLog in apply)

    // Decode materials per merged primitive (by name) and push to the viewport.
    QVector<QImage> tex, norm, orm, dm, rp, furMask, furNoise, fxNoise, emis, trans;
    QVector<QImage> detailN[3], detailR[3];   // up-to-3 detail normal/rough maps, per part
    QVector<float> metal, rough, emisMul, emisCol;   // emisCol = 3 floats/part (authored glow colour)
    QVector<float> dNInt, dRInt, dROffV, dCAddV;      // detail normal/rough strength + rough offset + colour-add
    QVector<QVector3D> dScaleV;                        // per-part per-map tiling (x/y/z = map0/1/2)
    QVector<int> dMetalLayerV;                          // per-part metal detail-map index (-1 = none)
    QVector<QVector4D> dBandsV, dZoneMapV;              // per-part detected dye bands + derived zone→map
    QVector<int> hair, skin, cloth, region, fur, fxAdd, eye, head;
    QVector<float> hairParams;   // per part ×3: hero_hair (Hair Roughness, Hair Specular, Highlight Shift)
    QVector<QString> matKey;   // per-part VRAM-pool key: material + colour epoch (parallel to parts)
    QVector<float> fxIntensity, fxWobble, fxFresnel, fxAlpha, fxSat;   // authored per-FX-part real values
    // Per-rebuild decode caches: dedup within THIS build only, freed when it returns — no data is
    // held between rebuilds (temporary/working memory, per the no-persistent-cache constraint).
    QHash<QString, QImage> cBase, cNorm, cOrm, cDm, cRp, cTrans, cMask, cNoise, cEmis;
    QHash<QString, DetailCacheEntry> cDetail;   // material → composited detail maps + summed strengths
    QHash<QString, QString> cShader;            // material → shader-map name (one JSON read, reused)
    QString furProbe;   // per-fur-part mask coverage → fur_probe.txt (log is mount-stale while app runs)
    for (const MeshPrimitive& p : merged.primitives) {
        const QString& m = p.materialName;
        auto cached = [&](QHash<QString, QImage>& c, const char* role, const QString& key, auto fn) -> QImage {
            auto it = c.constFind(key);
            if (it != c.constEnd()) return it.value();          // within-build dedup (always)
            QImage img;
            if (useLru) {
                // Cross-build reuse of the RAW decode. Key is role-qualified so base/norm/orm/etc.
                // of the same material never collide. Colour-independent: recolour is applied to a
                // copy after cached() returns, so the pooled original is never mutated.
                const QString lk = QLatin1String(role) + QLatin1Char('|') + key;
                bool hit = false;
                { QMutexLocker lock(&m_texCacheMutex);
                  if (const QImage* p = m_texCache.object(lk)) { img = *p; hit = true; } }
                if (!hit) {
                    img = fn();
                    const int cost = qMax(1, int(img.sizeInBytes() / 1024) + 1);
                    QMutexLocker lock(&m_texCacheMutex);
                    m_texCache.insert(lk, new QImage(img), cost);
                }
            } else {
                img = fn();
            }
            c.insert(key, img); return img;
        };
        // Material substitutions:
        //  • test999 body uses a black placeholder (armor_skin_mat) → real face body-skin.
        //  • the face's facial-hair material → the selected FacialHair style's shell material.
        const bool isSkinPlaceholder = m.contains(QLatin1String("skin_mat"), Qt::CaseInsensitive);
        const bool isFacialHairPrim  = m.contains(QLatin1String("facialhair"), Qt::CaseInsensitive);
        QString effMat = m;
        if (isSkinPlaceholder && !bodySkinMat.isEmpty()) effMat = bodySkinMat;
        else if (isFacialHairPrim && !fhMat.isEmpty())   effMat = fhMat;
        // Resolve the material's shader-map ONCE (a JSON read), cached per material, and reuse it for
        // both the eye test and the FX classification below instead of reading the file twice.
        const QString effShader = [&] {
            auto it = cShader.constFind(effMat);
            if (it != cShader.constEnd()) return it.value();
            const QString sh = shaderMapOf(d4, effMat); cShader.insert(effMat, sh); return sh;
        }();

        const bool isHair = isFacialHairPrim   // facial hair (incl. class beards like barF_B09_mat)
                            || effMat.contains(QLatin1String("hair"), Qt::CaseInsensitive)
                            || kHairRx.match(effMat).hasMatch();   // RogF_H00_mat, barF_H09_…
        const bool isHead = effMat.contains(QLatin1String("_HED"), Qt::CaseInsensitive)
                            || effMat.contains(QLatin1String("head"), Qt::CaseInsensitive)
                            || effMat.contains(QLatin1String("face"), Qt::CaseInsensitive);
        const bool isBody = isSkinPlaceholder || effMat.contains(QLatin1String("_BOD"), Qt::CaseInsensitive)
                            || effMat.contains(QLatin1String("body"), Qt::CaseInsensitive);
        // Eye = the eyeball material by name, OR (robust, class-agnostic) any material that uses D4's
        // Hero_Eye shader. Rogue/Sorcerer name their eyeball material differently, so the name test
        // alone misses them; the shader test catches every class's eyes.
        const bool isEye  = effMat.contains(QLatin1String("eyeball"), Qt::CaseInsensitive)
                            || effShader.contains(QLatin1String("Hero_Eye"), Qt::CaseInsensitive);
        const bool isSkin = isBody || isHead || effMat.contains(QLatin1String("skin"), Qt::CaseInsensitive);
        QImage base = cached(cBase, "base", effMat, [&] { return MaterialDecode::baseColor(m_reader, d4, effMat); });
        if (isHair) base = deriveHairAlpha(base);   // alpha from strand mask FIRST (untinted)
        if (isHair) {                               // colour the strands (alpha kept either way)
            if (hairRamp.size() >= 2) {
                // Walk the ramp by the MASK_PRIMARY root→tip gradient (D4's real method) — this is
                // where the hair's depth/AO lives, not in the flat white base.
                const QImage hairMask = MaterialDecode::byRole(m_reader, d4, effMat, "MASK_PRIMARY");
                base = hairColorFromMask(base, hairMask, hairRamp);
            } else if (hairTint.isValid()) base = tintImage(base, hairTint);   // fallback: flat tint
        }
        if (isSkin && !isHair && skinCol.isValid()) base = skinRecolor(base, skinCol);   // skin tone
        if (isEye) {   // real shared eye base (iris recoloured to the EyeColor); fallback = flat tint
            if (!eyeBaseImg.isNull()) base = eyeBaseImg;
            else if (irisCol.isValid()) {
                if (base.isNull()) { base = QImage(8, 8, QImage::Format_RGBA8888); base.fill(irisCol); }
                else base = tintImage(base, irisCol);
            }
        }
        // Skin-detail overlay (freckles / vitiligo), on toned skin, before makeup/markings.
        // BC1 has no alpha → blend at partial strength so the skin tone shows through.
        if (isBody && !isHair && !detailBodyImg.isNull()) base = overlayRGBA(base, detailBodyImg, 0.6f);
        if (isHead && !detailHeadImg.isNull())            base = overlayRGBA(base, detailHeadImg, 0.6f);
        // Surface factors up front — a marking may synthesise an ORM from the skin's own values.
        float mt, rg; MaterialDecode::factors(m_reader, d4, effMat, mt, rg);
        // Hair/beard uses D4's STRAND-HIGHLIGHT model (Scheuermann), not a GGX shell: the material's
        // "Hair Roughness" (0.5) drives the strand-sheen WIDTH (uHairParams.x, below), NOT a plain
        // specular. Feeding 0.5 into the ordinary GGX/IBL specular made hair read as wet plastic
        // ("fat and shiny"). Force the base ROUGHNESS matte so the standard specular is negligible —
        // the authored strand sheen (hair path) is then the only reflection, as in-game.
        if (isHair) { mt = 0.0f; rg = 0.92f; }
        if (isHair) {   // hair diagnostic → surfaces the real hero_hair values (repo omits hair assets)
            const QString hs = shaderMapOf(d4, effMat);
            const bool hasRoughTex = !MaterialDecode::byRole(m_reader, d4, effMat, "ROUGHNESS").isNull();
            const bool hasNormTex  = !MaterialDecode::byRole(m_reader, d4, effMat, "NORMAL").isNull();
            loadLog += QStringLiteral("\nHAIR %1 shader=%2 roughFactor=%3 HairRough=%4 HairSpec=%5 HairShift=%6 roughTex=%7 normTex=%8")
                           .arg(effMat).arg(hs.isEmpty() ? QStringLiteral("?") : hs)
                           .arg(rg, 0, 'f', 2)
                           .arg(fxScalar(d4, effMat, "Hair Roughness", -1.0f), 0, 'f', 3)
                           .arg(fxScalar(d4, effMat, "Hair Specular", -1.0f), 0, 'f', 3)
                           .arg(fxScalar(d4, effMat, "Hair Highlight Shift", -1.0f), 0, 'f', 3)
                           .arg(hasRoughTex ? QStringLiteral("YES") : QStringLiteral("no"))
                           .arg(hasNormTex ? QStringLiteral("YES") : QStringLiteral("no"));
        }
        // Hair: skip the ORM so the matte base roughness (uRough=0.92, set above) actually applies —
        // otherwise the shader would override it with the ORM's G channel (~0.6, still glossy). Hair's
        // depth comes from the mask/ramp + strand sheen, not an ORM.
        QImage partOrm = isHair ? QImage()
                       : (isEye && !eyeOrmImg.isNull()) ? eyeOrmImg
                                                        : cached(cOrm, "orm", effMat, [&] { return MaterialDecode::orm(m_reader, d4, effMat); });
        QImage markEmis; float markEmisMul = 0.0f;     // marking glow (skin only)
        // Markings carry a FULL D4 paint material: albedo tint through the ramp (mask R=coverage,
        // G=ink→gold), the authored roughness + gold-only metalness driven into the ORM, plus (for
        // emissive shapes) a glow. Face applies makeup first, then its marking.
        if (isHead) {
            if (!makeupImg.isNull()) base = overlayRGBA(base, makeupImg, makeupInt);
            if (!markFaceImg.isNull())
                markEmis = applyMarkingMaterial(base, partOrm, markFaceImg, markPaint, mark.emissive, rg, mt, markEmisMul);
        }
        if (isBody && !markBodyImg.isNull())
            markEmis = applyMarkingMaterial(base, partOrm, markBodyImg, markPaint, mark.emissive, rg, mt, markEmisMul);
        tex  << base;
        // VRAM-pool key for this part. effMat fully determines the base/norm/orm decode and the
        // per-part classification (hair/skin/eye/head/body are deterministic from it); colorEpoch
        // captures every recolour input. So identical (effMat, epoch) ⇒ identical final textures.
        matKey << (effMat + QLatin1Char('\x1f') + colorEpoch);
        norm << ((isEye && !eyeNormImg.isNull()) ? eyeNormImg
                                                  : cached(cNorm, "norm", effMat, [&] { return MaterialDecode::normalMap(m_reader, d4, effMat); }));
        orm  << partOrm;
        dm   << cached(cDm, "dm", effMat,   [&] { return MaterialDecode::byRole(m_reader, d4, effMat, "DYE_MASK"); });
        rp   << cached(cRp, "rp", effMat,   [&] { return MaterialDecode::byRole(m_reader, d4, effMat, "DYE_RAMP"); });
        metal << mt; rough << rg;
        // hero_hair look params, straight from the material (no hardcoded shader constants). Hair
        // Roughness falls back to the part's own roughFactor; Specular→0.3, Highlight Shift→0 if unauthored.
        if (isHair) {
            float hr = fxScalar(d4, effMat, "Hair Roughness", -1.0f);   if (hr    < 0.0f) hr    = rg;
            float hs = fxScalar(d4, effMat, "Hair Specular", -1.0f);    if (hs    < 0.0f) hs    = 0.3f;
            float sh = fxScalar(d4, effMat, "Hair Highlight Shift", 0.0f);
            hairParams << qBound(0.04f, hr, 1.0f) << qMax(0.0f, hs) << sh;
        } else {
            hairParams << rg << 0.3f << 0.0f;   // unused for non-hair (shader reads it only when uIsHair==1)
        }
        hair << (isHair ? 1 : 0); skin << (isSkin ? 1 : 0); eye << (isEye ? 1 : 0);
        head << ((isHead && isSkin && !isHair) ? 1 : 0);   // face skin only → warm Fresnel rim
        // Cloth/physics submeshes: D4 names them with cloth/sim/skirt/cape/loincloth
        // tokens — these are the parts the Verlet sim drapes during animation.
        cloth << ((isSimName(m) && !isFxName(m)) ? 1 : 0);
        region << 0;
        // Skin translucency (_Trans, slot 104): D4's hero_opaque_skin uses this thin-skin subsurface
        // map to place the red back-scatter (ears, nostrils, thin flesh). Feeding it makes our SSS
        // follow the real zones instead of a flat approximation. One entry per part (null off-skin).
        trans << (isSkin ? cached(cTrans, "trans", effMat, [&] { return MaterialDecode::byRole(m_reader, d4, effMat, "TRANSLUCENCY"); }) : QImage());
        // Shell fur: decode the density mask + strand noise only for genuine fur materials.
        const bool isFur = isFurMaterial(d4, effMat);
        fur << (isFur ? 1 : 0);
        const QImage furMaskImg = isFur ? cached(cMask, "mask", effMat,  [&] { return MaterialDecode::byRole(m_reader, d4, effMat, "MASK_PRIMARY"); }) : QImage();
        furMask  << furMaskImg;
        furNoise << (isFur ? cached(cNoise, "noise", effMat, [&] { return MaterialDecode::byRole(m_reader, d4, effMat, "NOISE_PROCEDURAL"); }) : QImage());
        // Fur-mask coverage diagnostic: what fraction of the mask passes the shell shader's
        // "dens >= 0.03 → grow" gate. ~0% ⇒ mask failed/gated everything off (fur vanishes);
        // ~100% ⇒ mask isn't restricting growth (fur covers the whole submesh = "wrong place");
        // a middling value ⇒ mask is properly limiting fur to the trim.
        if (isFur) {
            long grow = 0, tot = 0;
            if (!furMaskImg.isNull()) {
                const QImage g = furMaskImg.convertToFormat(QImage::Format_RGBA8888);
                for (int y = 0; y < g.height(); y += 2) {   // 2px stride: coverage estimate
                    const uchar* sl = g.constScanLine(y);
                    for (int x = 0; x < g.width(); x += 2) { ++tot; if (sl[x * 4] >= 8) ++grow; }
                }
            }
            const int cov = tot ? int(grow * 100 / tot) : -1;
            loadLog += QStringLiteral("\nfur %1: mask=%2x%3 coverage=%4%% (grow if R>=0.03)")
                           .arg(effMat).arg(furMaskImg.width()).arg(furMaskImg.height()).arg(cov);
            // Also collect into a dedicated probe file (the log is unreadable through the sandbox
            // mount while the app runs; an atomic file next to the exe reads cleanly).
            furProbe += QStringLiteral("%1  part#%2  mask=%3x%4  coverage=%5%%\n")
                            .arg(effMat).arg(fur.size() - 1).arg(furMaskImg.width())
                            .arg(furMaskImg.height()).arg(cov);
        }
        // Mesh FX: decode the scrolling noise for vfx_* (unlit/blended) submeshes.
        const QString& fxShader = effShader;   // reuse the shader-map resolved once above
        const bool isFx = fxShader.isEmpty() ? isFxName(effMat) : shaderIsFx(fxShader);
        fxNoise << (isFx ? cached(cNoise, "noise", effMat, [&] { return MaterialDecode::byRole(m_reader, d4, effMat, "NOISE_PROCEDURAL"); }) : QImage());
        // Blend mode from the shader variant: vfx_*_add_* → additive glow, else alpha blend.
        fxAdd << ((isFx && (fxShader.contains(QLatin1String("_add_"), Qt::CaseInsensitive)
                            || fxShader.contains(QLatin1String("additive"), Qt::CaseInsensitive))) ? 1 : 0);
        // Authored per-effect real values: emissive brightness, vertex-animation amount,
        // fresnel-edge exponent, opacity, colour saturation.
        fxIntensity << (isFx ? fxScalar(d4, effMat, "Color Intensity", 1.5f) : 1.5f);
        fxWobble    << (isFx ? fxScalar(d4, effMat, "Vertex Normal Offset", 0.0f) : 0.0f);
        fxFresnel   << (isFx ? fxScalar(d4, effMat, "Fresnel Slope", 1.6f) : 1.6f);
        fxAlpha     << (isFx ? fxScalar(d4, effMat, "Alpha Brightness Global", 1.0f) : 1.0f);
        fxSat       << (isFx ? fxScalar(d4, effMat, "Color Saturation", 1.0f) : 1.0f);
        // Emissive (glowing runes/gems): the authored EMISSIVE map × the material's authored
        // "emissive multiplier". Only gear glows — faces/eyes/hair carry a moist/AO emissive map
        // that would otherwise wash them white, and FX parts emit via their own unlit path.
        const bool emitsOk = !isFx && !isSkin && !isHair && !isEye;
        if (!markEmis.isNull()) {                      // emissive marking glow (skin: overrides the off skin-emissive)
            emis    << markEmis;
            emisMul << markEmisMul;
            emisCol << 1.0f << 1.0f << 1.0f;           // glow colour is already baked into the image
        } else if (isEye && !eyeEmisImg.isNull()) {    // glowing Incandescent/Vampire iris (flIrisEmissiveStrength)
            emis    << eyeEmisImg;
            emisMul << eyeEmisMul;
            emisCol << float(irisCol.redF()) << float(irisCol.greenF()) << float(irisCol.blueF());
        } else {
            emis    << (emitsOk ? cached(cEmis, "emis", effMat, [&] { return MaterialDecode::byRole(m_reader, d4, effMat, "EMISSIVE"); }) : QImage());
            emisMul << (emitsOk ? fxScalar(d4, effMat, "emissive multiplier", 1.0f) : 1.0f);
            const QColor ec = emitsOk ? emissiveColorOf(d4, effMat) : QColor(255, 255, 255);
            emisCol << float(ec.redF()) << float(ec.greenF()) << float(ec.blueF());
        }
        // Tiled detail maps: D4 layers up to 3 (leather/fabric/metal) by authored intensity.
        // Composite all 3 into one normal + one roughness (cached per material), so the shader
        // keeps a single detail sampler. Overall blend strength = the dominant map's intensity
        // (backward-compatible: a lone Map 1 at 0.6 behaves as before).
        // D4 layers the tiled detail maps automatically from the authored per-map scalars.
        // Default intensity = 1.0 (the shader default — materials override to 0 to DISABLE a
        // map; that's why female base01 authors nothing yet must still show detail like the male
        // variant that authors 0.6/1.0). Offsets default 0. Only PRESENT maps count, so the
        // composite returns the true summed strengths (dOvN/dOvR) + roughness bias (dROff).
        const bool detOk = !isFx;
        DetailCacheEntry de;
        if (detOk) {
            const auto it = cDetail.constFind(effMat);
            if (it != cDetail.constEnd()) {
                de = it.value();
            } else {
                const float nI[3] = { fxScalar(d4, effMat, "Normal Intensity - Detail Map 1", 1.0f),
                                      fxScalar(d4, effMat, "Normal Intensity - Detail Map 2", 1.0f),
                                      fxScalar(d4, effMat, "Normal Intensity - Detail Map 3", 1.0f) };
                const float rI[3] = { fxScalar(d4, effMat, "Roughness Intensity - Detail Map 1", 1.0f),
                                      fxScalar(d4, effMat, "Roughness Intensity - Detail Map 2", 1.0f),
                                      fxScalar(d4, effMat, "Roughness Intensity - Detail Map 3", 1.0f) };
                const float rO[3] = { fxScalar(d4, effMat, "Roughness Offset - Detail Map 1", 0.0f),
                                      fxScalar(d4, effMat, "Roughness Offset - Detail Map 2", 0.0f),
                                      fxScalar(d4, effMat, "Roughness Offset - Detail Map 3", 0.0f) };
                MaterialDecode::detailMapsSeparate(m_reader, d4, effMat, nI, rI, rO,
                                                   de.n, de.r, de.ns, de.rs, de.ro, de.sc, de.ml);
                de.ns = qBound(0.0f, de.ns, 4.0f);
                de.rs = qBound(0.0f, de.rs, 4.0f);
                // Color Add Intensity (detail tints albedo) — dominant of the up-to-3 maps.
                de.ca = qBound(0.0f, qMax(fxScalar(d4, effMat, "Color Add Intensity - Detail Map 1", 0.0f),
                                     qMax(fxScalar(d4, effMat, "Color Add Intensity - Detail Map 2", 0.0f),
                                          fxScalar(d4, effMat, "Color Add Intensity - Detail Map 3", 0.0f))), 1.0f);
                cDetail.insert(effMat, de);
            }
        }
        for (int k = 0; k < 3; ++k) {
            detailN[k] << (k < de.n.size() ? de.n[k] : QImage());
            detailR[k] << (k < de.r.size() ? de.r[k] : QImage());
        }
        dNInt   << de.ns;
        dRInt   << de.rs;
        dROffV  << de.ro;
        dCAddV  << de.ca;
        dScaleV << QVector3D(de.sc[0], de.sc[1], de.sc[2]);
        dMetalLayerV << (detOk ? de.ml : -1);
        // Game-data-derived selection for this part: bands read from its own dye mask, zone→map
        // derived from the present non-metal maps (metal handled by metalness in the shader).
        dBandsV   << (detOk ? detectDyeBands(dm.last()) : QVector4D(0.063f, 0.345f, 0.596f, 0.831f));
        dZoneMapV << (detOk ? deriveZoneMap(de.n, de.ml) : QVector4D(-1, 0, 1, 2));
    }
        // Pack the decoded maps and hand them to the apply phase.
        WardrobeOutfitMaps M;
        M.tex=std::move(tex); M.norm=std::move(norm); M.orm=std::move(orm); M.dm=std::move(dm);
        M.rp=std::move(rp); M.furMask=std::move(furMask); M.furNoise=std::move(furNoise);
        M.fxNoise=std::move(fxNoise); M.emis=std::move(emis); M.trans=std::move(trans);
        for (int k=0;k<3;++k){ M.detailN[k]=std::move(detailN[k]); M.detailR[k]=std::move(detailR[k]); }
        M.metal=std::move(metal); M.rough=std::move(rough); M.emisMul=std::move(emisMul); M.emisCol=std::move(emisCol);
        M.dNInt=std::move(dNInt); M.dRInt=std::move(dRInt); M.dROffV=std::move(dROffV); M.dCAddV=std::move(dCAddV);
        M.dScaleV=std::move(dScaleV); M.dMetalLayerV=std::move(dMetalLayerV);
        M.dBandsV=std::move(dBandsV); M.dZoneMapV=std::move(dZoneMapV);
        M.matKey=std::move(matKey);
        M.hair=std::move(hair); M.skin=std::move(skin); M.cloth=std::move(cloth); M.region=std::move(region);
        M.hairParams=std::move(hairParams);
        M.fur=std::move(fur); M.fxAdd=std::move(fxAdd); M.eye=std::move(eye); M.head=std::move(head);
        M.fxIntensity=std::move(fxIntensity); M.fxWobble=std::move(fxWobble); M.fxFresnel=std::move(fxFresnel);
        M.fxAlpha=std::move(fxAlpha); M.fxSat=std::move(fxSat);
        M.furProbe=std::move(furProbe); M.loadLogAdd=std::move(loadLog);
        return M;
    };   // end decode lambda
    if (!async) {   // synchronous path (init/restore/look-apply)
        QElapsedTimer dt; dt.start();
        const WardrobeOutfitMaps M = decode();
        const qint64 tDec = dt.elapsed();
        applyOutfit(ctx, M);
        qInfo("wardrobe: rebuild(sync) — geometry %lld ms · decode %lld ms · apply %lld ms (%d pieces)",
              tGeom, tDec, dt.elapsed() - tDec, ctx.pieceCount);
        return;
    }
    // Async: decode on a worker thread, then apply on the main thread; a generation token drops the
    // result if the user has already picked something newer. Show a "Loading…" hint meanwhile (the
    // previous model stays on screen, dimmed by the label) — cleared when applyOutfit lands.
    if (m_view) m_view->setOverlayText(QStringLiteral("Loading…"));
    QThreadPool::globalInstance()->start([this, gen, ctx, decode, tGeom]() {
        QElapsedTimer dt; dt.start();
        WardrobeOutfitMaps M = decode();
        const qint64 tDec = dt.elapsed();
        QMetaObject::invokeMethod(this, [this, gen, ctx, M, tDec, tGeom]() {
            if (gen != m_buildGen) return;
            QElapsedTimer at; at.start();
            applyOutfit(ctx, M);
            qInfo("wardrobe: rebuild(async) — geometry %lld ms · decode %lld ms (worker) · apply %lld ms (%d pieces)",
                  tGeom, tDec, at.elapsed(), ctx.pieceCount);
        }, Qt::QueuedConnection);
    });
}

// Apply the decoded maps + geometry context to the viewport and UI. Main thread only.
void WardrobeTab2::applyOutfit(const WardrobeBuildCtx& ctx, const WardrobeOutfitMaps& M)
{
    // Block re-entrant rebuilds while we repopulate combos/lists/trees (their signals are wired back
    // to rebuild). Set-only (no early return): the sync path calls this with the guard already held.
    m_rebuilding = true;
    struct ApplyGuard { bool& f; ~ApplyGuard() { f = false; } } applyGuard{m_rebuilding};
    const ModelGeometry& merged = ctx.merged;
    const QVector<int>& primSlot = ctx.primSlot;
    const int baseOutfitCount = ctx.baseOutfitCount;
    QString loadLog = ctx.loadLog + M.loadLogAdd;
    const QString& skelDbg = ctx.skelDbg; const QString& weapDbg = ctx.weapDbg;
    const QVector<QPair<QString,int>>& pieceList = ctx.pieceList;
    const QString keepAnim = ctx.keepAnim; const int keepFrame = ctx.keepFrame; const bool wasPlaying = ctx.wasPlaying;
    const int pieceCount = ctx.pieceCount; const qint64 totalV = ctx.totalV, totalT = ctx.totalT;
    const QVector<QImage>& tex=M.tex; const QVector<QImage>& norm=M.norm; const QVector<QImage>& orm=M.orm;
    const QVector<QImage>& dm=M.dm; const QVector<QImage>& rp=M.rp; const QVector<QImage>& trans=M.trans;
    const QVector<QImage>& furMask=M.furMask; const QVector<QImage>& furNoise=M.furNoise; const QVector<QImage>& fxNoise=M.fxNoise;
    const QVector<QImage>& emis=M.emis; const QVector<QImage>* detailN=M.detailN; const QVector<QImage>* detailR=M.detailR;
    const QVector<float>& metal=M.metal; const QVector<float>& rough=M.rough; const QVector<float>& emisMul=M.emisMul; const QVector<float>& emisCol=M.emisCol;
    const QVector<float>& dNInt=M.dNInt; const QVector<float>& dRInt=M.dRInt; const QVector<float>& dROffV=M.dROffV; const QVector<float>& dCAddV=M.dCAddV;
    const QVector<QVector3D>& dScaleV=M.dScaleV; const QVector<int>& dMetalLayerV=M.dMetalLayerV;
    const QVector<QVector4D>& dBandsV=M.dBandsV; const QVector<QVector4D>& dZoneMapV=M.dZoneMapV;
    const QVector<int>& hair=M.hair; const QVector<int>& skin=M.skin; const QVector<int>& cloth=M.cloth; const QVector<int>& region=M.region;
    const QVector<int>& fur=M.fur; const QVector<int>& fxAdd=M.fxAdd; const QVector<int>& eye=M.eye; const QVector<int>& head=M.head;
    const QVector<float>& fxIntensity=M.fxIntensity; const QVector<float>& fxWobble=M.fxWobble; const QVector<float>& fxFresnel=M.fxFresnel;
    const QVector<float>& fxAlpha=M.fxAlpha; const QVector<float>& fxSat=M.fxSat;
    const QString& furProbe=M.furProbe;
    // VRAM texture pool: apply the toggle + this build's per-part keys BEFORE geometry/textures so the
    // next paint's uploadTextures() can reuse unchanged materials' GPU textures. Off by default.
    m_view->setVramPoolEnabled(QSettings().value(QStringLiteral("wardrobe2/perf/vramPool"), false).toBool());
    m_view->setPartMatKeys(M.matKey);
    // Push geometry now (deferred from rebuildOutfitImpl) so it lands together with the textures below
    // in a single paint — the previous model stayed on screen until this moment. keepFramed after the
    // first build so the user's orbit/zoom survives item swaps.
    // Guard the GPU upload (flatten + VBO/IBO + cloth build) — the stage most likely to fault on a
    // malformed merged outfit.
    if (!seh::runGuarded("w2Gpu", [&]() { m_view->setGeometry(merged, m_framed); })) {
        m_view->clearGeometry();
        m_view->setOverlayText(QStringLiteral("Couldn't display this outfit — skipped."));
        return;
    }
    m_framed = true;
    m_view->setOverlayText(QString());       // model is here → clear any "Loading…"/empty hint
    m_view->setPartTextures(tex);
    m_view->setPartNormals(norm);
    m_view->setPartOrm(orm);
    m_view->setPartTranslucency(trans);   // skin _Trans map → real subsurface zones
    m_expBase = tex; m_expNorm = norm; m_expOrm = orm;   // retained per-part maps for .glb export
    m_expDyeMask = dm; m_expDyeRamp = rp;                // + dye mask/ramp so the export can bake dyes
    for (int k = 0; k < 3; ++k) { m_expDetN[k] = detailN[k]; m_expDetR[k] = detailR[k]; }   // + detail bake
    m_expDScale = dScaleV; m_expDZoneMap = dZoneMapV; m_expDBands = dBandsV;
    m_expDMetalLayer = dMetalLayerV; m_expDNInt = dNInt; m_expDRInt = dRInt; m_expDROff = dROffV;
    m_expEmis = emis; m_expEmisMul = emisMul; m_expEmisCol = emisCol;   // emissive for export
    m_view->setPartDyeMask(dm);
    m_view->setPartDyeRamp(rp);
    m_view->setPartFactors(metal, rough);
    m_view->setPartEmissive(emis);
    m_view->setPartEmissiveMult(emisMul);
    m_view->setPartEmissiveColor(emisCol);
    m_view->setPartDetailNormals(detailN[0], detailN[1], detailN[2]);
    m_view->setPartDetailRoughs(detailR[0], detailR[1], detailR[2]);
    m_view->setPartDetailIntensity(dNInt, dRInt);
    m_view->setPartDetailROffset(dROffV);
    m_view->setPartDetailColorAdd(dCAddV);
    m_view->setPartDetailScales(dScaleV);
    m_view->setPartDetailMetalLayer(dMetalLayerV);
    m_view->setPartDetailBands(dBandsV);
    m_view->setPartDetailZoneMap(dZoneMapV);
    m_view->setPartFlags(hair, skin, cloth);
    m_view->setPartHairParams(M.hairParams);
    m_view->setPartEye(eye);
    m_partEye = eye;   // remember which merged parts are eyes (for the eye-only recompose + isolate)
    m_view->setPartHead(head);
    m_view->setPartFur(fur);
    m_view->setPartFurMask(furMask);
    m_view->setPartFurNoise(furNoise);
    // Fur-mask coverage probe → atomic file next to the exe (reads cleanly through the sandbox
    // mount, unlike the truncating log). ~100% ⇒ mask not gating (fur everywhere); ~0% ⇒ gated
    // off; middling ⇒ correctly limited to trim.
    {
        QFile fp(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("fur_probe.txt")));
        if (fp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            fp.write((QStringLiteral("fur-mask coverage probe\n\n") + furProbe).toUtf8());
            fp.close();
        }
    }
    m_view->setPartFxNoise(fxNoise);
    m_view->setPartFxAdditive(fxAdd);
    m_view->setPartFxParams(fxIntensity, fxWobble, fxFresnel, fxAlpha, fxSat);
    m_view->setFurEnabled(QSettings().value(QStringLiteral("wardrobe2/viewport/fur"), true).toBool());
    m_view->setPartDyeRegion(region);
    m_partSlot = primSlot;   // per-merged-primitive slot tag (applyDye() at the end pushes pigments)
    // A slot is "dyeable" only if one of its primitives carries a DyeMask; otherwise pigments
    // do nothing there, so the picker dims the zone buttons for it.
    for (int s = 0; s < kSlotCount; ++s) m_slotDyeable[s] = false;
    for (int i = 0; i < dm.size() && i < primSlot.size(); ++i) {
        const int s = primSlot[i];
        if (s >= 0 && s < 5 && !dm[i].isNull()) m_slotDyeable[s] = true;   // armour only
    }

    // FX/SIM part flags + per-part source piece (for the tree) + apply visibility.
    m_lastMerged = merged;   // needed by rebuildPartList() below
    // Rig hardpoints (attach sockets) for the viewport overlay — read from the base body appearance
    // (its bones lead the merged skeleton, so the socket bone indices line up).
    m_lastMerged.hardpoints.clear();
    Hardpoints::readInto(m_lastMerged, baseAppJsonPath());
    m_view->setHardpoints(m_lastMerged.hardpoints);
    m_partFx.clear(); m_partSim.clear(); m_partForm.clear(); m_partSource.clear(); m_partTris.clear();
    m_partCovered.clear();
    // Base-body region suppression: the warlock base00 whole-body mesh is drawn as the default
    // outfit, but equipping armour must hide the region it covers (else the bare torso/legs/etc.
    // poke THROUGH the armour). D4 tags every submesh with a body-region slot hash, and an armour
    // piece shares that hash with the base-body region it occupies. So: for each equipped body
    // slot, find the region hash its armour predominantly occupies, then hide the base00 primitives
    // carrying that hash. Helm (slot 0) is excluded, so a helm never removes the head.
    QSet<quint32> coveredRegion;
    QString coverDbg;
    for (int si = 1; si <= 4; ++si) {                     // torso, gloves, legs, boots — never helm
        if (!m_slot[si] || m_slot[si]->currentData().toInt() <= 0) continue;   // slot empty
        QHash<quint32, int> weight;                       // region hash → triangle weight in this armour
        for (int j = 0; j < merged.primitives.size() && j < primSlot.size(); ++j)
            if (primSlot[j] == si && merged.primitives[j].slotHash)
                weight[merged.primitives[j].slotHash] += int(merged.primitives[j].indices.size());
        quint32 dom = 0; int best = 0;                    // dominant region the armour occupies
        for (auto it = weight.constBegin(); it != weight.constEnd(); ++it)
            if (it.value() > best) { best = it.value(); dom = it.key(); }
        if (dom) { coveredRegion.insert(dom); coverDbg += QStringLiteral(" slot%1=0x%2").arg(si).arg(dom, 0, 16); }
    }
    // Classify each submesh from GAME DATA first, names only as a fallback:
    //   FX  → its material uses a "vfx_*" shader map (unlit/blended effect)
    //   SIM → an authored NvCloth definition file exists for it
    // Both lookups are cached per unique name (a character has only a handful of submeshes).
    const QString d4dir = Config::d4dataDir();
    QHash<QString, bool> fxCache, simCache;
    int pcIdx = 0;
    for (const MeshPrimitive& p : merged.primitives) {
        const QString& mn = p.materialName;
        m_partTris << int(p.indices.size()) / 3;
        // A base00 region primitive is hidden when its region hash is claimed by equipped armour.
        const bool covered = pcIdx < baseOutfitCount && p.slotHash && coveredRegion.contains(p.slotHash);
        m_partCovered << (covered ? 1 : 0);
        ++pcIdx;
        bool fx = fxCache.value(mn, false), sim = simCache.value(mn, false);
        if (!fxCache.contains(mn)) {
            const QString shader = shaderMapOf(d4dir, mn);
            fx = shader.isEmpty() ? isFxName(mn) : shaderIsFx(shader);   // game data, else name
            fxCache.insert(mn, fx);
        }
        if (!simCache.contains(mn)) {
            sim = hasClothDef(d4dir, mn) || (d4dir.isEmpty() && isSimName(mn));
            simCache.insert(mn, sim);
        }
        m_partFx  << (fx ? 1 : 0);
        m_partSim << (sim ? 1 : 0);
        // FORM → transformation-model submeshes (e.g. Warlock's demon form) baked into base00.
        // Hidden by default; toggled via the FORM button like FX/SIM.
        m_partForm << (mn.contains(QLatin1String("demonform"), Qt::CaseInsensitive) ? 1 : 0);
    }
    m_partSourceSno.clear();
    for (int pi = 0; pi < pieceList.size(); ++pi) {   // expand (name,count) → per-primitive source
        const int sno = ctx.pieceSno.value(pi, -1);
        for (int k = 0; k < pieceList[pi].second; ++k) {
            m_partSource << pieceList[pi].first;
            m_partSourceSno << sno;                  // lets the context menu export the SOURCE item
        }
    }
    m_view->setPartFx(m_partFx);   // exclude FX submeshes from the cloth sim
    loadClothTuning();             // read the equipped pieces' real cloth params
    // The outfit just changed: new rig, new cloth build. Re-push EVERY overlay (this also calls
    // applyClothParams), so toggles survive an outfit swap instead of needing a manual re-tick.
    reapplyOverlays();
    if (m_clothTuningFound)
        loadLog += QStringLiteral("\ncloth(game): boneTrack=%1 actorTrack=%2 stretch=%3 horiz=%4 shear=%5 bend=%6 damp=%7 fric=%8")
                       .arg(m_gct.boneTrack, 0, 'f', 2).arg(m_gct.actorTrack, 0, 'f', 2)
                       .arg(m_gct.stretch, 0, 'f', 2).arg(m_gct.horiz, 0, 'f', 2)
                       .arg(m_gct.shear, 0, 'f', 2).arg(m_gct.bend, 0, 'f', 2)
                       .arg(m_gct.damping, 0, 'f', 2).arg(m_gct.friction, 0, 'f', 2);
    if (baseOutfitCount > 0) {
        int nCov = m_partCovered.count(1);
        QString baseHashes;
        for (int j = 0; j < baseOutfitCount && j < merged.primitives.size(); ++j)
            baseHashes += QStringLiteral(" 0x%1").arg(merged.primitives[j].slotHash, 0, 16);
        loadLog += QStringLiteral("\nbase00 regions:%1 | armour covers:%2 | hidden=%3")
                       .arg(baseHashes).arg(coverDbg.isEmpty() ? QStringLiteral(" (none)") : coverDbg).arg(nCov);
    }
    rebuildPartList();
    recomputePartVisibility();
    populateMaterials();

    // Cache export materials (indexed by materialIndex) for the Export button — reuses
    // the exact base/normal/orm images already decoded for the preview.
    m_lastMerged = merged;
    int maxMi = 0;
    for (const MeshPrimitive& p : merged.primitives) maxMi = qMax(maxMi, p.materialIndex);
    m_exportMats = QVector<ModelExporter::ExportMaterial>(maxMi + 1);
    for (int pi = 0; pi < merged.primitives.size(); ++pi) {
        const int mi = merged.primitives[pi].materialIndex;
        if (mi < 0 || mi >= m_exportMats.size()) continue;
        ModelExporter::ExportMaterial em;
        em.name = merged.primitives[pi].materialName;
        em.baseColor = tex.value(pi);
        em.normal = norm.value(pi);
        em.orm = orm.value(pi);
        em.hasMetal = true; em.metal = metal.value(pi, 0.0f);
        em.hasRough = true; em.rough = rough.value(pi, 1.0f);
        em.doubleSided = hair.value(pi, 0) != 0;
        m_exportMats[mi] = em;
    }

    // Re-apply the user's preview-settings shading features (defaults on for the
    // creator-friendly look: hair sheen, subsurface, IBL, tonemap, detail maps).
    {
        QSettings s;
        auto vp = [&](const QString& k, bool def) { return s.value(QStringLiteral("wardrobe2/viewport/") + k, def).toBool(); };
        m_view->setFeatureHair(vp(QStringLiteral("hair"), true));
        m_view->setFeatureSubsurface(vp(QStringLiteral("subsurface"), true));
        m_view->setFeatureIbl(vp(QStringLiteral("ibl"), true));
        m_view->setFeatureTonemap(vp(QStringLiteral("tonemap"), true));
        m_view->setBackfaceCull(false);   // double-sided always (back-face culling removed)
        m_view->setFeatureDetail(vp(QStringLiteral("detail"), true));
        m_view->setFeatureSpecAA(vp(QStringLiteral("specaa"), true));
        m_view->setShadowEnabled(vp(QStringLiteral("shadow"), true));
        m_view->setSsaoEnabled(vp(QStringLiteral("ssao"), true));
        m_view->setFeatureMask(vp(QStringLiteral("mask"), false));
        m_view->setFurEnabled(vp(QStringLiteral("fur"), true));
        auto vpi = [&](const QString& k, int def) { return s.value(QStringLiteral("wardrobe2/viewport/") + k, def).toInt(); };
        m_view->setFurLength (vpi(QStringLiteral("furLength"),  44) * 0.0005f);
        m_view->setFurDensity(float(vpi(QStringLiteral("furDensity"), 30)));
        m_view->setFurShells (vpi(QStringLiteral("furShells"),  20));
        m_view->setFurGravity(vpi(QStringLiteral("furGravity"), 18) * 0.00025f);
        m_view->setFurCurl   (vpi(QStringLiteral("furCurl"),    14) * 0.00025f);
        m_view->setFurCoverage(0.60f - vpi(QStringLiteral("furCoverage"), 57) * 0.01f);
        m_view->setFxIntensity  (vpi(QStringLiteral("fxIntensity"), 20) * 0.05f);
        m_view->setFxScrollSpeed(vpi(QStringLiteral("fxScroll"),    20) * 0.05f);
        m_view->setFxWobble     (vpi(QStringLiteral("fxWobble"),    20) * 0.05f);
    }

    applyDye();   // apply per-slot pigments (and the global dye fallback, if any)
    if (m_env) m_view->setEnvironment(m_env->currentIndex());
    populateAnims();   // refresh the animation list for the current body rig

    // Re-apply the animation that was running before this rebuild (setGeometry cleared
    // it), restoring the frame + play/pause state so equipping/creator changes don't
    // interrupt playback.
    if (!keepAnim.isEmpty()) {
        playAnimByName(keepAnim);
        if (m_animSlider && m_view->animFrameCount() > 0) {
            m_animSlider->setValue(qBound(0, keepFrame, m_view->animFrameCount() - 1));
        }
        if (!wasPlaying && m_animTimer) {
            m_animTimer->stop();
            if (m_playBtn) m_playBtn->setIcon(transportGlyph(0));
        }
    }

    // Clean human summary on the visible line ("Warlock · 6 items"); the technical counts + full
    // per-piece debug go to the tooltip (hover) so the default view isn't engineer-facing.
    const QString cls = m_class ? m_class->currentText() : QString();
    const QString human = (cls.isEmpty() ? QString() : cls + QStringLiteral(" · "))
                          + QStringLiteral("%1 item%2").arg(pieceCount).arg(pieceCount == 1 ? QString() : QStringLiteral("s"));
    const QString tech = QStringLiteral("%1 piece(s) · %2 parts · %3 verts · %4 tris")
                             .arg(pieceCount).arg(merged.primitives.size()).arg(totalV).arg(totalT);
    m_status->setText(human + QStringLiteral("   ⓘ ‘Copy debug’ opens the full log"));
    m_status->setToolTip((tech + skelDbg + weapDbg + loadLog).trimmed());

    // Camera Snap: re-fit on the active slot after an equip — keeps the angle, adjusts for
    // size changes (e.g. a helmet with big horns vs. one without).
    if (m_d4View) frameSlot(m_activeSlot, /*animate=*/true, /*keepRotation=*/true);
}

// ── Preview-settings popup (shading features + exposure + background) ──────────
// Clamp a popup so it stays inside the app window (and on its screen), flipping above the
// anchor button if it would spill off the bottom. Keeps panels off a second monitor.

// Fullscreen = MAXIMIZE-IN-PLACE, same as the Models tab: hide the side columns and the toolbar
// chrome so the viewport fills the tab. No top-level window (true fullscreen was too intrusive),
// no GL context re-parenting. Esc or the floating ✕ restores.
void WardrobeTab2::toggleFullscreen()
{
    if (!m_view || !m_mainSplit) return;
    const bool max = !m_viewMaxed;
    m_viewMaxed = max;

    // Side columns: panes 0 (character/slots) and 2 (panel sidebar) of the 3-column splitter.
    // The sidebar only returns if the » arrow hasn't hidden it independently.
    if (QWidget* w = m_mainSplit->widget(0)) w->setVisible(!max);
    if (m_mainSplit->count() > 2)
        if (QWidget* w = m_mainSplit->widget(2)) w->setVisible(!max && !m_sideCollapsed);
    // Centre column chrome (the view toolbar + the floating N-strip). Axis gizmo + overlays
    // stay: they're viewport content the user governs from the Overlays toggle, not chrome.
    if (m_viewBarW) m_viewBarW->setVisible(!max);
    if (m_vpStrip)  m_vpStrip->setVisible(!max);
    // Transport bar under the viewport: hide while maximized; on restore show only if a clip is loaded.
    if (m_timeline) m_timeline->setVisible(!max && m_view && m_view->animFrameCount() > 0);

    if (m_fsBtn) {
        m_fsBtn->setText(max ? QStringLiteral("Exit") : QStringLiteral("Fullscreen"));
        m_fsBtn->setToolTip(max
            ? QStringLiteral("Restore the panels (Esc)")
            : QStringLiteral("Maximize the preview — hides the panels and toolbars (Esc to exit)"));
    }
    // Esc exits. Created once, lives on the tab.
    if (max && !m_fsEsc) {
        m_fsEsc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        m_fsEsc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(m_fsEsc, &QShortcut::activated, this, [this] {
            if (m_viewMaxed) toggleFullscreen();
        });
    }
    if (m_fsEsc) m_fsEsc->setEnabled(max);

    // With the toolbar hidden there'd be no visible way back — Esc alone is a trap for anyone
    // who didn't read a tooltip. A small floating button sits where the toolbar used to be.
    if (max && !m_fsExitBtn) {
        m_fsExitBtn = new QToolButton(m_view);
        m_fsExitBtn->setText(QStringLiteral("✕  Exit fullscreen"));
        m_fsExitBtn->setToolTip(QStringLiteral("Restore the panels (Esc)"));
        m_fsExitBtn->setCursor(Qt::PointingHandCursor);
        m_fsExitBtn->setStyleSheet(QStringLiteral(
            "QToolButton{padding:3px 10px;border:1px solid #5a5a5a;border-radius:3px;"
            "background:rgba(35,35,35,200);color:#ccc;}"
            "QToolButton:hover{border-color:#b0453c;}"));
        connect(m_fsExitBtn, &QToolButton::clicked, this, [this] {
            if (m_viewMaxed) toggleFullscreen();
        });
    }
    if (m_fsExitBtn) {
        m_fsExitBtn->setVisible(max);
        if (max) {
            m_fsExitBtn->adjustSize();
            m_fsExitBtn->move(m_view->width() - m_fsExitBtn->width() - 8, 8);
            m_fsExitBtn->raise();
        }
    }
    m_view->setFocus(Qt::OtherFocusReason);   // so Esc reaches us, not a search box
}

// ── Camera popup (snap · follow · FOV · view angles · turntable) ──────────────


GLModelWidget* WardrobeTab2::previewWidget() { return m_view; }

// ── Export the assembled outfit as a skinned .glb ─────────────────────────────
bool WardrobeTab2::hasExportSelection() const
{
    return m_lastMerged.valid && !m_lastMerged.primitives.isEmpty();
}

// Build a per-primitive material roster from the retained maps (each part keeps its own
// textures) and write the skinned geometry — with the playing clip when enabled — to `path`.
// `keep` selects which merged primitives to write; empty = the whole assembled outfit.
// The subset path is what the viewport/parts context menu uses for "Export Part" and
// "Export Model" — previously those isolated parts in the tree and called this function, but
// it reads m_lastMerged wholesale and never consulted the tree, so BOTH exported the full
// outfit. Filtering here keeps one export path (dye/detail baking, anims, retarget) for all
// three scopes instead of a second one to keep in sync.
bool WardrobeTab2::exportOutfitGlb(const QString& path, const QVector<int>& keep)
{
    if (!hasExportSelection() || path.isEmpty()) return false;
    const bool wantTex  = QSettings().value(QStringLiteral("export/includeTex"), true).toBool();
    const bool bakeDye_    = QSettings().value(QStringLiteral("export/bakeDye"), false).toBool();   // bake applied dye into base colour (default matches Export settings)
    const bool bakeDetail_ = QSettings().value(QStringLiteral("export/bakeDetail"), false).toBool(); // bake tiled detail into normal/ORM
    ModelGeometry geo = m_lastMerged;   // copy so we can give each primitive a unique material index
    // src[i] maps exported primitive i back to its ORIGINAL merged index, which is what the
    // per-part texture/dye/detail arrays (m_exp*, m_partSlot) are indexed by.
    QVector<int> src;
    for (int k = 0; k < (keep.isEmpty() ? geo.primitives.size() : keep.size()); ++k) {
        const int si = keep.isEmpty() ? k : keep[k];
        if (si >= 0 && si < geo.primitives.size()) src << si;
    }
    if (src.isEmpty()) return false;
    if (!keep.isEmpty()) {
        QVector<MeshPrimitive> sub;
        sub.reserve(src.size());
        for (int si : src) sub << m_lastMerged.primitives[si];
        geo.primitives = sub;
    }
    QVector<ModelExporter::ExportMaterial> mats;
    mats.reserve(geo.primitives.size());
    for (int i = 0; i < geo.primitives.size(); ++i) {
        const int si = src[i];              // index into the per-part source arrays
        ModelExporter::ExportMaterial em;
        em.name = geo.primitives[i].materialName;
        em.doubleSided = geo.primitives[i].doubleSided;
        if (wantTex) {
            if (si < m_expBase.size() && !m_expBase[si].isNull()) {
                em.baseColor = m_expBase[si];
                // Bake the applied dye into the exported base colour (opt-in), so a dyed outfit exports
                // dyed instead of in its undyed base. Per-part colours resolved exactly as applyAllDyes().
                if (bakeDye_ && si < m_expDyeMask.size() && !m_expDyeMask[si].isNull() && si < m_partSlot.size()) {
                    const int slot = m_partSlot[si];
                    QStringList hex;
                    if (slot >= 0 && slot < kSlotCount && m_slotDye[slot].hex.size() == 4) hex = m_slotDye[slot].hex;
                    else if (m_dyeCombo && m_dyeCombo->currentIndex() > 0) hex = m_dyeCombo->currentData().toStringList();
                    if (slot < 5 && hex.size() == 4) {
                        float cols[12];
                        for (int k = 0; k < 4; ++k) { const QColor c(hex[k]);
                            cols[k*3+0] = float(c.redF()); cols[k*3+1] = float(c.greenF()); cols[k*3+2] = float(c.blueF()); }
                        const QImage rmp = (si < m_expDyeRamp.size()) ? m_expDyeRamp[si] : QImage();
                        em.baseColor = bakeDye(em.baseColor, m_expDyeMask[si], rmp, cols);
                    }
                }
            }
            if (si < m_expNorm.size() && !m_expNorm[si].isNull()) em.normal    = m_expNorm[si];
            if (si < m_expOrm.size()  && !m_expOrm[si].isNull())  em.orm        = m_expOrm[si];  // R=AO G=rough B=metal
            // Bake the tiled/zone-routed detail grain into the exported normal + ORM (opt-in), so a
            // Blender model carries the leather/fabric/brushed-metal surface texture, not a smooth base.
            if (bakeDetail_ && !em.normal.isNull() && si < m_expDetN[0].size()) {
                const QImage dN[3] = { m_expDetN[0].value(si), m_expDetN[1].value(si), m_expDetN[2].value(si) };
                const QImage dR[3] = { m_expDetR[0].value(si), m_expDetR[1].value(si), m_expDetR[2].value(si) };
                if (!dN[0].isNull() || !dN[1].isNull() || !dN[2].isNull())
                    bakeDetail(em.normal, em.orm, m_expDyeMask.value(si), dN, dR,
                               m_expDScale.value(si, QVector3D(8, 8, 8)),
                               m_expDZoneMap.value(si, QVector4D(-1, 0, 1, 2)),
                               m_expDBands.value(si, QVector4D(0.063f, 0.345f, 0.596f, 0.831f)),
                               m_expDMetalLayer.value(si, -1), m_expDNInt.value(si, 1.0f),
                               m_expDRInt.value(si, 1.0f), m_expDROff.value(si, 0.0f));
            }
            // Emissive only where the part actually glows (mask + strength), matching the preview:
            // emission = emissive map × emisCol × emisMult. No glow → nothing written (no blow-out).
            if (si < m_expEmis.size() && !m_expEmis[si].isNull()
                && si < m_expEmisMul.size() && m_expEmisMul[si] > 0.0f) {
                em.emissive = m_expEmis[si];
                em.hasEmissive = true;
                em.emisMult = m_expEmisMul[si];
                if (3 * si + 2 < m_expEmisCol.size()) {
                    em.emisR = m_expEmisCol[3 * si]; em.emisG = m_expEmisCol[3 * si + 1]; em.emisB = m_expEmisCol[3 * si + 2];
                }
            }
            // Alpha-cutout detection: a base colour with transparent texels (hair strands, cut-out
            // cloth/fur trim) → alphaMode=MASK in the glTF so Blender shows the cutout, not a solid quad.
            if (!em.baseColor.isNull()) {
                const QImage a = em.baseColor.convertToFormat(QImage::Format_RGBA8888);
                const int sy = qMax(1, a.height() / 64), sx = qMax(1, a.width() / 64);
                bool cut = false;
                for (int yy = 0; yy < a.height() && !cut; yy += sy) {
                    const uchar* s = a.constScanLine(yy);
                    for (int xx = 0; xx < a.width(); xx += sx) if (s[xx*4+3] < 200) { cut = true; break; }
                }
                em.alphaCutout = cut;
            }
        }
        geo.primitives[i].materialIndex = i;
        mats.push_back(em);
    }
    QVector<AnimParser::DecodedAnim> anims; QStringList names;
    collectExportAnims(anims, names);      // honors the "Animations to embed" scope
    const bool wantAnim = !anims.isEmpty();
    const ModelExporter::Options opt = ModelExporter::optionsFromSettings();
    if (QSettings().value(QStringLiteral("export/hardpointEmpties"), false).toBool())
        Hardpoints::readInto(geo, baseAppJsonPath());          // body-rig sockets (weapon grips, sheaths…)
    Retarget::applyFromSettings(geo);                          // collapse cloth / remap to anchors
    if (opt.blenderFriendly)
        GLModelWidget::blenderizeSkeletonNames(geo.skeleton);  // .L/.R mirror-paired names
    else if (QSettings().value(QStringLiteral("export/boneNamesTranslated"), false).toBool())
        GLModelWidget::translateSkeletonNames(geo.skeleton);   // bone_<hash> → readable labels
    Hardpoints::resolveBoneIndices(geo);                       // re-index after any skeleton edit

    const bool ok = ModelExporter::exportGlb(geo, path, mats, anims, names, opt);
    if (ok)
        ExportNotifier::instance().notify(
            QStringLiteral("Exported %1%2").arg(QFileInfo(path).fileName(),
                wantAnim ? QStringLiteral(" (with animation)") : QString()),
            QFileInfo(path).absolutePath());
    else if (m_status)
        m_status->setText(QStringLiteral("Export failed: %1").arg(QFileInfo(path).fileName()));
    return ok;
}

// Parts currently drawn in the viewport. The outfit export used to ignore the parts tree
// entirely, so unchecking a piece changed the preview but not the .glb — Models tab has always
// filtered on partVisible(), and this brings Wardrobe in line: what you see is what you get.
QVector<int> WardrobeTab2::visibleParts() const
{
    QVector<int> out;
    if (!m_view) return out;                       // no viewport → export everything
    const int n = m_lastMerged.primitives.size();
    for (int i = 0; i < n; ++i)
        if (m_view->partVisible(i)) out << i;
    if (out.size() == n) out.clear();              // all visible → whole-outfit fast path
    return out;
}

void WardrobeTab2::exportSelection()
{
    if (!hasExportSelection()) return;
    const QString dir = QSettings().value(QStringLiteral("wardrobe2/lastExportDir"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();
    const QString suggest = QDir(dir).filePath(classPrefix() + QStringLiteral("_outfit.glb"));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export outfit .glb"),
                                                      suggest, QStringLiteral("glTF binary (*.glb)"));
    if (path.isEmpty()) return;
    if (exportOutfitGlb(path, visibleParts()))
        QSettings().setValue(QStringLiteral("wardrobe2/lastExportDir"), QFileInfo(path).absolutePath());
}

void WardrobeTab2::exportSelectionToLast()
{
    if (!hasExportSelection()) return;
    const QString dir = QSettings().value(QStringLiteral("wardrobe2/lastExportDir")).toString();
    if (dir.isEmpty()) { exportSelection(); return; }   // no remembered folder → prompt
    exportOutfitGlb(QDir(dir).filePath(classPrefix() + QStringLiteral("_outfit.glb")), visibleParts());
}

// (exportScreenshot / exportTurntableGif removed — image and turntable capture run through the
//  top-level Export menu, which drives ExportCapture against previewWidget() directly.)



// ── Lighting popup (three-point character-screen rig) ─────────────────────────


// Read wardrobe2/light/* → a LightRig and push it to the viewport (also called on load).
void WardrobeTab2::applyLightRig()
{
    if (!m_view) return;
    QSettings s;
    GLModelWidget::LightRig r;
    r.preset       = s.value(QStringLiteral("wardrobe2/light/preset"), 1).toInt();   // default: Hero Direct
    r.keyInt       = s.value(QStringLiteral("wardrobe2/light/key"),  100).toInt() / 100.0f;
    r.rimInt       = s.value(QStringLiteral("wardrobe2/light/rim"),  100).toInt() / 100.0f;
    r.fillInt      = s.value(QStringLiteral("wardrobe2/light/fill"), 100).toInt() / 100.0f;
    r.ambInt       = s.value(QStringLiteral("wardrobe2/light/amb"),  100).toInt() / 100.0f;
    r.keyAzimuth   = float(s.value(QStringLiteral("wardrobe2/light/az"), 15).toInt());
    r.keyElevation = float(s.value(QStringLiteral("wardrobe2/light/el"), 25).toInt());
    m_view->setLightRig(r);
    m_view->setReflectionStrength(s.value(QStringLiteral("wardrobe2/light/refl"),     100).toInt() / 100.0f);
    m_view->setSkinWarmth(        s.value(QStringLiteral("wardrobe2/light/skinwarm"), 100).toInt() / 100.0f);
    m_view->setSssStrength(       s.value(QStringLiteral("wardrobe2/light/sss"),       24).toInt() / 100.0f);
    m_view->setWetness(           s.value(QStringLiteral("wardrobe2/light/wetness"),    0).toInt() / 100.0f);
    m_view->setSnow(              s.value(QStringLiteral("wardrobe2/light/snow"),       0).toInt() / 100.0f);
    m_view->setEmissiveScale(     s.value(QStringLiteral("wardrobe2/light/emis"),      50).toInt() / 100.0f);
    m_view->setShadowParams(      s.value(QStringLiteral("wardrobe2/light/shadowStr"),  60).toInt() / 100.0f,
                                  s.value(QStringLiteral("wardrobe2/light/shadowSoft"), 15).toInt() / 10.0f,
                                  s.value(QStringLiteral("wardrobe2/light/shadowBias"), 18).toInt() / 10000.0f);
    m_view->setShadowExtra(       s.value(QStringLiteral("wardrobe2/light/shadowRange"), 130).toInt() / 100.0f,
                                  s.value(QStringLiteral("wardrobe2/light/shadowNBias"),  10).toInt() / 1000.0f,
                                  s.value(QStringLiteral("wardrobe2/light/shadowRes"),  2048).toInt());
    m_view->setLightLock(         s.value(QStringLiteral("wardrobe2/light/lock"), false).toBool());
    m_view->setExposure(          s.value(QStringLiteral("wardrobe2/light/exp"),        100).toInt() / 100.0f);
    m_view->setColorGrade(        s.value(QStringLiteral("wardrobe2/light/grade"),     false).toBool(),
                                  s.value(QStringLiteral("wardrobe2/light/gradeContrast"), 105).toInt() / 100.0f,
                                  s.value(QStringLiteral("wardrobe2/light/gradeSat"),      110).toInt() / 100.0f,
                                  s.value(QStringLiteral("wardrobe2/light/gradeWarmth"),    30).toInt() / 1000.0f);
    // Real D4 grade LUT: load the CASC texture named in wardrobe2/light/lutName (a 256×16 strip) and
    // hand it to the shader; empty name or wrong size → the stylised grade is used instead. This is
    // how the AUTHENTIC grade gets applied once we know the LUT's texture name.
    {
        const QString lutName = s.value(QStringLiteral("wardrobe2/light/lutName")).toString().trimmed();
        QImage lut;
        if (!lutName.isEmpty() && m_reader && m_reader->isReady()) {
            const qint64 ls = texSnoFor(lutName);
            if (ls > 0) lut = MaterialDecode::texture(m_reader, Config::d4dataDir(), lutName, ls);
        }
        m_view->setColorGradeLut(lut);
    }
    m_view->setSsaoParams(        s.value(QStringLiteral("wardrobe2/light/ssaoStr"),    100).toInt() / 100.0f,
                                  s.value(QStringLiteral("wardrobe2/light/ssaoRad"),     30).toInt() / 100.0f);
}

// ── Shaders popup (shell-fur settings) ────────────────────────────────────────


// ── Detail-maps discovery tool + render config ────────────────────────────────
// A DEV / DISCOVERY tool (its toolbar button shows only in Developer mode), not per-item config.
// All controls are GLOBAL and feed the shader's detail-map selection so the correct game-data rule
// can be found by eye, then baked in. Defaults reproduce the shipped behaviour (Auto = per-item game
// data); "Copy config" dumps the current settings. Persisted in QSettings across restarts.





// ── Live cloth-physics tuning popup (debug) ───────────────────────────────────
void WardrobeTab2::applyClothParams()
{
    if (!m_view) return;
    QSettings s;
    GLModelWidget::ClothParams d;   // built-in defaults
    // Undo the v2 migration: it raised "Capsule size" to 1.0 on the theory that authored radii are
    // body-accurate. They are not — verified against the render, 1.0 inflates the body and splays
    // skirts open with the legs visible through the gap, while ~0.52 drapes correctly. Restore the
    // default once for anyone v2 already changed (an intentional non-1.0 value is left alone).
    if (!s.value(QStringLiteral("cloth/capsuleFix_v3"), false).toBool()) {
        s.setValue(QStringLiteral("cloth/capsuleFix_v3"), true);
        if (s.value(QStringLiteral("cloth/capsuleFix_v2"), false).toBool())
            for (const char* k : {"wardrobe2/cloth/capScale", "models/cloth/capScale", "stable2/cloth/capScale"})
                if (qFuzzyCompare(s.value(QLatin1String(k), 0.52).toDouble(), 1.0))
                    s.setValue(QLatin1String(k), double(d.capsuleRadius));
    }
    auto f = [&](const QString& k, double def) {
        return float(s.value(QStringLiteral("wardrobe2/cloth/") + k, def).toDouble()); };
    GLModelWidget::ClothParams p;
    p.gravity          = -f(QStringLiteral("gravity"), -d.gravity);   // stored as positive magnitude
    p.damping          = f(QStringLiteral("damping"),  d.damping);
    p.maxDistance      = f(QStringLiteral("maxdist"),  d.maxDistance);
    p.bendStiffness    = f(QStringLiteral("bend"),     d.bendStiffness);
    p.stretchStiffness = f(QStringLiteral("stretch"),  d.stretchStiffness);
    p.iterations       = s.value(QStringLiteral("wardrobe2/cloth/iters"), d.iterations).toInt();
    p.subSteps         = s.value(QStringLiteral("wardrobe2/cloth/substeps"), d.subSteps).toInt();
    p.selfCollision    = f(QStringLiteral("self"),     d.selfCollision);
    p.collisionMargin  = f(QStringLiteral("margin"),   d.collisionMargin);
    p.friction         = f(QStringLiteral("friction"), d.friction);
    p.backstop         = f(QStringLiteral("backstop"), d.backstop);
    p.capsuleRadius    = f(QStringLiteral("capScale"), d.capsuleRadius);   // one knob for all capsules
    p.capRegion[0]     = f(QStringLiteral("capLegs"),  d.capRegion[0]);
    p.capRegion[1]     = f(QStringLiteral("capWaist"), d.capRegion[1]);
    p.capRegion[2]     = f(QStringLiteral("capTorso"), d.capRegion[2]);
    p.capRegion[3]     = f(QStringLiteral("capArms"),  d.capRegion[3]);
    p.capRegion[4]     = f(QStringLiteral("capHead"),  d.capRegion[4]);
    p.capRegion[5]     = f(QStringLiteral("capOther"), d.capRegion[5]);
    p.boneTracking     = f(QStringLiteral("tracking"), d.boneTracking);
    p.actorTracking    = d.actorTracking;
    p.horizStiffness   = d.horizStiffness;
    p.shearStiffness   = d.shearStiffness;
    p.attachStiffness  = d.attachStiffness;
    // Aerodynamics: the Wind slider scales the authored self-wind DIRECTION (opt-in, default 0
    // — the game's wind magnitude is in different units than our per-frame model). Drag is a
    // plain 0..1 air-resistance factor (authored value applied when game data is on, below).
    const float windStr = f(QStringLiteral("wind"), 0.0);
    p.windX = m_gct.windDirX * windStr * 0.002f;
    p.windY = m_gct.windDirY * windStr * 0.002f;
    p.windZ = m_gct.windDirZ * windStr * 0.002f;
    p.dragFactor = f(QStringLiteral("drag"), 0.0);
    p.boneStiffness = f(QStringLiteral("bonestiff"), d.boneStiffness);   // spring-bone return-to-pose
    // "React to rotation": orbiting the camera drives inertia into the cloth (see ClothParams).
    p.userSpin      = s.value(QStringLiteral("wardrobe2/cloth/userSpin"), false).toBool();
    p.userSpinForce = f(QStringLiteral("spinForce"), d.userSpinForce);
    // When enabled, the equipped pieces' real Cloth/*.clt.json values drive the full
    // behavioural (dimensionless) param set, so each armor set matches the game 1:1.
    if (m_clothTuningFound
        && s.value(QStringLiteral("wardrobe2/cloth/useGameData"), true).toBool()) {
        p.boneTracking     = m_gct.boneTrack;
        p.actorTracking    = m_gct.actorTrack;
        p.stretchStiffness = m_gct.stretch;
        p.horizStiffness   = m_gct.horiz;
        p.shearStiffness   = m_gct.shear;
        p.bendStiffness    = m_gct.bend;
        p.friction         = m_gct.friction;
        p.attachStiffness  = m_gct.attach;
        if (p.dragFactor <= 0.0f) p.dragFactor = m_gct.drag;   // authored air resistance
        // gravity / maxDistance / backstop / capsuleRadius stay as the live slider values (above)
        // — they're our solver units, not game-authored, and are the knobs you tune.
        p.iterations       = qMax(p.iterations, 4);   // game uses 1 (NvCloth); we need a few for stability
    }
    // Master switch, with the same convergence the Models tab needs: asking for rotation-driven
    // cloth IS asking for the sim, so a saved "userSpin on + physics off" pair (impossible to fix
    // from the UI, since the checkbox already reads on and fires no toggle) resolves to physics on.
    bool clothOn = s.value(QStringLiteral("wardrobe2/cloth/enabled"), true).toBool();
    // NOTE: "React to rotation" no longer force-enables cloth HERE. This runs on every apply — and
    // on every model load — so with userSpin on, an explicit decision to switch physics OFF was
    // silently reverted (and persisted) each time, e.g. loading a new hair model brought physics
    // back until the master toggle was cycled. The convergence now happens once, in the userSpin
    // toggle handler, which is the moment the user actually asks for rotation-driven cloth.
    m_view->setClothEnabled(clothOn);
    m_view->setCapsuleAxis(s.value(QStringLiteral("wardrobe2/cloth/capAxis"), 3).toInt());   // default: bone
    m_view->setClothParams(p);
    // Overlay flags obey the MASTER gate. applyClothParams() runs on every physics-panel edit, so
    // without this a slider nudge switched the phys-bone/collider overlays back on even though the
    // master toggle was off — the setting had been saved, and this path replayed it ungated.
    // Same rule as the overlay checkboxes themselves and as StableTab2: master AND own key.
    const bool ovOn = m_overlaysOn;
    m_view->setShowColliders(ovOn && s.value(QStringLiteral("wardrobe2/cloth/showColliders"), false).toBool());
    m_view->setShowPhysBones(ovOn && s.value(QStringLiteral("wardrobe2/cloth/showPhysBones"), false).toBool());
    m_view->setShowPhysAxes(ovOn && s.value(QStringLiteral("wardrobe2/cloth/showPhysAxes"), true).toBool());
    if (m_physPanel) refreshGameDrivenSliders();   // reflect this model's real .clt values on the sliders
}

// Single place that pushes overlay state to the viewport: master gate AND each box's own state.
// Anything needing overlays refreshed calls THIS — never setShow*() directly, or the master gate
// gets bypassed (that is exactly how physics-panel edits used to switch overlays back on).
// Export ONE part: isolate it in the parts tree, run the tab's normal export (which honours the
// tree's check states), then restore. Reusing the real export path means no second exporter to
// keep in sync — and restoration happens however the export ends.
// All merged parts that came from the SAME source appearance as `part` — i.e. the "model" the
// part belongs to (one equipped item), not the whole assembled outfit. Matched on the source
// SNO where known so two pieces sharing a display name don't merge.
QVector<int> WardrobeTab2::partsOfSource(int part) const
{
    QVector<int> out;
    if (part < 0 || part >= m_partSource.size()) return out;
    const int  sno  = m_partSourceSno.value(part, -1);
    const QString nm = m_partSource.value(part);
    for (int i = 0; i < m_partSource.size(); ++i) {
        const bool same = (sno > 0 && m_partSourceSno.value(i, -1) == sno)
                       || (sno <= 0 && m_partSource.value(i) == nm);
        if (same) out << i;
    }
    return out;
}

// Export an arbitrary subset of merged parts. `label` seeds the suggested filename.
void WardrobeTab2::exportPartsSubset(const QVector<int>& parts, const QString& label, bool toLast)
{
    if (parts.isEmpty() || !hasExportSelection()) return;
    QString base = label.isEmpty() ? classPrefix() : label;
    base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]+")), QStringLiteral("_"));
    if (base.isEmpty()) base = QStringLiteral("part");
    const QString last = QSettings().value(QStringLiteral("wardrobe2/lastExportDir")).toString();
    if (toLast && !last.isEmpty()) {
        exportOutfitGlb(QDir(last).filePath(base + QStringLiteral(".glb")), parts);
        return;
    }
    const QString dir = last.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) : last;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export .glb"),
        QDir(dir).filePath(base + QStringLiteral(".glb")), QStringLiteral("glTF binary (*.glb)"));
    if (path.isEmpty()) return;
    if (exportOutfitGlb(path, parts))
        QSettings().setValue(QStringLiteral("wardrobe2/lastExportDir"), QFileInfo(path).absolutePath());
}

// ONE part menu, shown from BOTH the 3D viewport right-click and the PARTS PANEL, so the panel
// can copy/export exactly like the viewport. part < 0 = clicked empty space (all-parts actions only).
void WardrobeTab2::showPartContextMenu(int part, const QPoint& gp)
{
        if (!m_partTree) return;
        auto itemForPart = [this](int p) -> QTreeWidgetItem* {
            for (int r = 0; r < m_partTree->topLevelItemCount(); ++r) {
                QTreeWidgetItem* root = m_partTree->topLevelItem(r);
                for (int c2 = 0; c2 < root->childCount(); ++c2)
                    if (root->child(c2)->data(0, Qt::UserRole).toInt() == p) return root->child(c2);
            }
            return nullptr;
        };
        auto setAll = [this](Qt::CheckState st) {
            for (int r = 0; r < m_partTree->topLevelItemCount(); ++r) {
                QTreeWidgetItem* root = m_partTree->topLevelItem(r);
                for (int c2 = 0; c2 < root->childCount(); ++c2) root->child(c2)->setCheckState(0, st);
            }
        };
        // Right-click SELECTS (blue outline); the camera only moves via "Frame Part".
        if (m_view) m_view->setPickedPart(part);
        ViewportPartMenu::Info in;
        ViewportPartMenu::Actions act;
        QTreeWidgetItem* item = nullptr;
        // "Model" scope = the source item the picked part belongs to (empty space → whole outfit).
        const QVector<int> modelParts = partsOfSource(part);
        int modelTris = 0;
        if (modelParts.isEmpty()) { for (int i = 0; i < m_partTris.size(); ++i) modelTris += m_partTris[i]; }
        else                      { for (int i : modelParts) modelTris += m_partTris.value(i); }
        // The title names the SOURCE piece under the cursor, and "Export Model" is scoped to
        // that same piece — right-clicking a cape exports the cape, not the whole outfit.
        in.sourceModel   = (part >= 0 && part < m_partSource.size() && !m_partSource[part].isEmpty())
                             ? m_partSource[part] : QStringLiteral("Outfit");
        in.modelTris     = modelTris;
        in.lastExportDir = QSettings().value(QStringLiteral("wardrobe2/lastExportDir")).toString();
        if (part >= 0 && part < m_lastMerged.primitives.size()) {
            item = itemForPart(part);
            in.part           = part;
            in.partName       = m_lastMerged.primitives[part].materialName;
            in.partFileName   = in.partName;
            in.sourceFileName = m_partSource.value(part);     // the outfit piece this part came from
            in.sourceName     = m_partSource.value(part);
            in.partTris       = m_partTris.value(part);
            in.visible        = !item || item->checkState(0) == Qt::Checked;
            in.isSim          = part < m_partSim.size() && m_partSim[part];
            in.isFx           = part < m_partFx.size()  && m_partFx[part];
            act.setVisible    = [item](bool on) { if (item) item->setCheckState(0, on ? Qt::Checked : Qt::Unchecked); };
            act.isolate       = [setAll, item] { setAll(Qt::Unchecked); if (item) item->setCheckState(0, Qt::Checked); };
            act.selectPart    = [this, item] {
                if (!item || !m_partTree) return;
                m_partTree->setCurrentItem(item); m_partTree->scrollToItem(item);
            };
            act.frame         = [this, part] {
                if (!m_view) return;
                QVector3D c; float r;
                if (m_view->partsBounds(QVector<int>{part}, c, r))
                    m_view->frameRegionKeepRotation(c, r, /*animate=*/true);
            };
            const QString pn = in.partName.isEmpty() ? QStringLiteral("part") : in.partName;
            act.exportPart        = [this, part, pn] { exportPartsSubset(QVector<int>{part}, pn, false); };
            act.exportPartLastDir = [this, part, pn] { exportPartsSubset(QVector<int>{part}, pn, true); };
        }
        if (modelParts.isEmpty()) {                       // empty space → the assembled outfit
            act.exportModel        = [this] { exportSelection(); };
            act.exportModelLastDir = [this] { exportSelectionToLast(); };
        } else {                                          // a part was picked → just its source item
            const QString sn = in.sourceModel;
            act.exportModel        = [this, modelParts, sn] { exportPartsSubset(modelParts, sn, false); };
            act.exportModelLastDir = [this, modelParts, sn] { exportPartsSubset(modelParts, sn, true); };
        }
        act.showAll = [setAll] { setAll(Qt::Checked); };
        act.hideAll = [setAll] { setAll(Qt::Unchecked); };
        act.invert  = [this] {
            if (!m_partTree) return;
            for (int r = 0; r < m_partTree->topLevelItemCount(); ++r) {
                QTreeWidgetItem* root = m_partTree->topLevelItem(r);
                for (int c2 = 0; c2 < root->childCount(); ++c2) {
                    QTreeWidgetItem* it = root->child(c2);
                    it->setCheckState(0, it->checkState(0) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
                }
            }
        };
        // The right-click outline is transient: it marks what the menu acts on and must go
        // when the menu does. Persistent selection is the tree/highlight, not this.
        act.closed = [this] { if (m_view) m_view->setPickedPart(-1); };
        ViewportPartMenu::exec(this, gp, in, act);
}

void WardrobeTab2::reapplyOverlays()
{
    if (!m_view) return;
    for (const auto& e : m_overlayChks)
        if (e.first) e.second(m_overlaysOn && e.first->isChecked());
    applyClothParams();   // cloth/collider/phys-bone flags share the same gate
}

// Read the equipped pieces' real cloth-sim params from the game data: each piece's
// appearance references a snoCloth (group 11) → Cloth/<name>.clt.json holds the
// authored NvCloth tuning (flBoneTrackingFactor, stiffnesses, …). We average the
// dimensionless behavioural params across the equipped cloth pieces.
// Fill each parsed ClothSim's PER-PIECE tuning from its own Cloth/<name>_sim.clt.json (the
// ClothData embeds the cloth name). Applied per-bone in the solver, so the cape uses the cape's
// params and the skirt uses the skirt's — instead of one average across the whole outfit.
void WardrobeTab2::fillClothSimTuning(ModelGeometry& geo)
{
    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty()) return;
    for (ClothSim& s : geo.clothSims) {
        if (s.name.isEmpty()) continue;
        const QJsonObject t = ModelParser::resolveClothTuning(d4, s);
        if (t.isEmpty()) continue;
        auto g = [&](const char* k, double def) { return t.value(QLatin1String(k)).toDouble(def); };
        s.boneTrack = qBound(0.0f, float(g("flBoneTrackingFactor", 0.5)), 1.0f);
        const QJsonObject gv = t.value(QStringLiteral("vGravity")).toObject();
        const double gmag = -gv.value(QStringLiteral("z")).toDouble(-20.0);   // game z-down, magnitude
        // NO floor: authored zero gravity is a real statement (upright feathers/plumes
        // author vGravity=(0,0,0) + tight tracking — they must not sag under ANY gravity,
        // including the manual slider, which multiplies this scale).
        // SIGNED: authored positive vGravity.z (e.g. sorF_stor211 feathers, z=+10) is an
        // UPWARD force — the magnitude-only clamp read it as zero. gmag is negated above,
        // so upward authoring yields a negative scale and the solver's gAuth flips sign.
        s.gravScale = qBound(-3.0f, float(gmag / 20.0), 3.0f);
        s.attachStiff = qBound(0.0f, float(g("flAttachmentStiffness", 0.3)), 1.0f);
        // Authored per-class constraint stiffness (see ClothSim::clsStiff).
        s.clsStiff[0] = qBound(0.05f, float(g("flStretchingStiffness", 0.8)), 1.0f);
        s.clsStiff[1] = qBound(0.05f, float(g("flHorizontalStiffness", 0.8)), 1.0f);
        s.clsStiff[2] = qBound(0.05f, float(g("flShearStiffness",      0.5)), 1.0f);
        s.clsStiff[3] = qBound(0.05f, float(g("flBendingStiffness",    0.5)), 1.0f);
        s.dragF = qBound(0.0f, float(g("flDragFactor", 0.0)), 1.0f);
        // vSelfWind (game z-up) × flWindFactor → y-up, scaled to our per-frame units (≈ gravity scale).
        const QJsonObject sw = t.value(QStringLiteral("vSelfWind")).toObject();
        const double wf = g("flWindFactor", 1.0), sc = 0.0006;
        const double sx=sw.value(QStringLiteral("x")).toDouble(), sy=sw.value(QStringLiteral("y")).toDouble(), sz=sw.value(QStringLiteral("z")).toDouble();
        s.windX = float(sx*wf*sc); s.windY = float(sz*wf*sc); s.windZ = float(-sy*wf*sc);
        s.tuned = true;
    }
}

// ── D4_CLOTH_AUDIT=1 — corpus-wide cloth audit (launch via "Cloth Audit.bat") ────────
// The cloth fixes in this codebase are GENERIC mechanisms driven by authored data
// (followers/driver skinning, tethers, constraint clusters, per-piece .clt.json tuning),
// so a model can now only misbehave for one of two detectable reasons: (a) its tuning
// name fails to resolve (it silently runs untuned defaults — the feathers bug class), or
// (b) an authored array is missing, so a legacy fallback path runs (the cape-jut class).
// This sweep visits EVERY Appearance SNO, extracts each embedded ClothData block
// (cloth-only parse, no mesh work), runs the REAL runtime resolution (resolveClothTuning
// above — one implementation, so the audit cannot drift from the viewer), and writes
// next to the exe:
//   cloth_audit.csv          one row per cloth piece, every field
//   cloth_audit_summary.txt  ranked problem lists (TUNING-FAILED first, then
//                            fallback-path pieces, then behaviour outliers)
// Broken pieces surface as a reviewable LIST instead of being found one model at a time.
static QString runClothAudit(const QString& d4, SnoIndex* idx, CascReader* rd, QWidget* parent)
{
    const QString outDir = QCoreApplication::applicationDirPath();
    QFile csv(outDir + QStringLiteral("/cloth_audit.csv"));
    if (!csv.open(QIODevice::WriteOnly | QIODevice::Text))
        return QStringLiteral("Cloth audit: cannot write cloth_audit.csv");
    csv.write("appearance,sno,sim,resolve,how,nRealVerts,capacity,followers,driverSkin,"
              "tether,clusteredPct,blendW,gravityZ,attachStiff,boneTrack,flags\n");

    const auto& apps = idx->entries(kGroupAppearance);
    QProgressDialog prog(QStringLiteral("Cloth audit: scanning %1 appearances…").arg(apps.size()),
                         QStringLiteral("Cancel"), 0, int(apps.size()), parent);
    prog.setWindowModality(Qt::ApplicationModal);
    prog.setMinimumDuration(0);

    int scanned = 0, clothApps = 0, simCount = 0;
    QMap<QString, QStringList> flagLists;   // flag -> "sim (appearance)" entries
    QSet<QString> flagSeen;                 // dedupe key: flag|sim
    int nSno = 0, nSim = 0, nBare = 0, nHq = 0, nPrefix = 0, nFailed = 0;

    for (const SnoEntry& e : apps) {
        prog.setValue(scanned++);           // modal setValue also pumps events
        if (prog.wasCanceled()) break;
        const QByteArray meta = rd->readMetaBySno(quint64(e.snoId));
        if (meta.size() < 32) continue;
        // Cheap prefilter on the small meta buffer: a ClothData is referenced by a 16B
        // DT_VARIABLEARRAY {0,0,offset,size==720}. No hit → skip the payload read (the
        // vast majority of appearances). False positives are rejected by the parser's
        // header validation and cost one payload read.
        bool cand = false;
        const uchar* p = reinterpret_cast<const uchar*>(meta.constData());
        for (int off = 0; off + 16 <= meta.size(); off += 4) {
            quint32 a, b; qint32 sz;
            memcpy(&a, p + off, 4); memcpy(&b, p + off + 4, 4); memcpy(&sz, p + off + 12, 4);
            if (a == 0 && b == 0 && sz == 720) { cand = true; break; }
        }
        if (!cand) continue;
        const QByteArray payload = rd->readPayloadBySno(quint64(e.snoId));
        QVector<ClothCapsule> caps; QVector<ClothSim> sims;
        if (!seh::runGuarded("clothAudit", [&]() { ModelParser::parseClothOnly(meta, payload, caps, sims, e.name); }))
            continue;                       // malformed data must not kill the sweep
        if (sims.isEmpty()) continue;       // false-positive 720B ref
        ++clothApps;
        for (const ClothSim& s : sims) {
            ++simCount;
            QString how;
            const QJsonObject t = ModelParser::resolveClothTuning(d4, s, &how);
            if      (how.startsWith(QLatin1String("snoCloth:"))) ++nSno;
            else if (how == QLatin1String("_sim"))    ++nSim;
            else if (how == QLatin1String("bare"))    ++nBare;
            else if (how == QLatin1String("_HQ_sim")) ++nHq;
            else if (how.startsWith(QLatin1String("prefix:"))) ++nPrefix;
            else ++nFailed;
            // Mechanism coverage — mirrors the solver's own gating in GLModelWidget:
            // a missing array there selects the legacy fallback path for that piece.
            int followed = 0; for (int f : s.followerBone) if (f >= 0) ++followed;
            const bool drvSkin = !s.drvInf.isEmpty() && !s.drvW.isEmpty() && !s.drvBone.isEmpty();
            const bool tether  = !s.kinRoots.isEmpty() && !s.attachLen.isEmpty();
            const int  nPair   = s.constraintIdx.size() / 2;
            int classified = 0; for (quint8 c : s.conClass) if (c != 255) ++classified;
            const int clsPct = nPair > 0 ? (classified * 100) / nPair : 100;
            double gravZ = 0.0, attach = -1.0, track = -1.0;
            if (!t.isEmpty()) {
                gravZ  = t.value(QStringLiteral("vGravity")).toObject().value(QStringLiteral("z")).toDouble(-20.0);
                attach = t.value(QStringLiteral("flAttachmentStiffness")).toDouble(0.3);
                track  = t.value(QStringLiteral("flBoneTrackingFactor")).toDouble(0.5);
            }
            QStringList flags;
            if (t.isEmpty())            flags << QStringLiteral("TUNING-FAILED");     // untuned defaults (feathers bug class)
            if (followed == 0)          flags << QStringLiteral("NO-FOLLOWERS");      // 10cm anchor-search fallback
            if (!drvSkin)               flags << QStringLiteral("NO-DRIVERSKIN");     // 20cm render-vert borrow (cape-jut class)
            if (!tether)                flags << QStringLiteral("NO-TETHER");         // target-relative clamp fallback
            if (clsPct < 100)           flags << QStringLiteral("UNCLASSIFIED-%1PCT").arg(100 - clsPct);
            if (!t.isEmpty() && gravZ == 0.0)  flags << QStringLiteral("ZERO-GRAVITY");   // authored upright (info)
            if (attach >= 0.8)          flags << QStringLiteral("RIGID-ATTACH");          // crest-like (info)
            if (attach >= 0.0 && attach <= 0.1) flags << QStringLiteral("FREE-ATTACH");   // feather-like (info)
            for (const QString& fl : flags) {
                const QString key = fl.section(QLatin1Char('-'), 0, 0) + QLatin1Char('|') + s.name;
                if (flagSeen.contains(key)) continue;
                flagSeen.insert(key);
                flagLists[fl] << QStringLiteral("%1 (%2)").arg(s.name, e.name);
            }
            csv.write(QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13,%14,%15,%16\n")
                .arg(e.name).arg(e.snoId).arg(s.name)
                .arg(t.isEmpty() ? QStringLiteral("FAILED") : QStringLiteral("OK")).arg(how)
                .arg(s.nRealVerts).arg(s.vertCount).arg(followed)
                .arg(drvSkin ? QStringLiteral("yes") : QStringLiteral("no"))
                .arg(tether  ? QStringLiteral("yes") : QStringLiteral("no"))
                .arg(clsPct)
                .arg(s.blendW.isEmpty() ? QStringLiteral("no") : QStringLiteral("yes"))
                .arg(t.isEmpty() ? QStringLiteral("") : QString::number(gravZ, 'f', 2))
                .arg(attach < 0 ? QStringLiteral("") : QString::number(attach, 'f', 2))
                .arg(track  < 0 ? QStringLiteral("") : QString::number(track, 'f', 2))
                .arg(flags.join(QLatin1Char(';'))).toUtf8());
        }
    }
    const bool cancelled = prog.wasCanceled();
    prog.setValue(int(apps.size()));
    csv.close();

    QFile sf(outDir + QStringLiteral("/cloth_audit_summary.txt"));
    if (sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QString out;
        out += QStringLiteral("D4 cloth audit — %1 appearances scanned%2, %3 cloth-bearing, %4 cloth pieces\n")
                   .arg(scanned).arg(cancelled ? QStringLiteral(" (CANCELLED — partial)") : QString())
                   .arg(clothApps).arg(simCount);
        out += QStringLiteral("tuning resolution: snoCloth=%1 _sim=%2 bare=%3 _HQ_sim=%4 prefix-fallback=%5 FAILED=%6\n\n")
                   .arg(nSno).arg(nSim).arg(nBare).arg(nHq).arg(nPrefix).arg(nFailed);
        out += QStringLiteral("Flag meanings:\n"
               "  TUNING-FAILED   piece runs UNTUNED defaults — the exact bug class the feathers/crest had. FIX FIRST.\n"
               "  NO-DRIVERSKIN   cage targets fall back to the 20cm render-vert borrow — the cape-jut bug class.\n"
               "  NO-FOLLOWERS    bones fall back to the 10cm nearest-particle anchor search.\n"
               "  NO-TETHER       motion limit falls back to the target-relative clamp.\n"
               "  UNCLASSIFIED-*  constraints without a warp/weft/shear/bend class run base stiffness.\n"
               "  ZERO-GRAVITY / RIGID-ATTACH / FREE-ATTACH  informational behaviour classes (authored, correct).\n\n");
        for (auto it = flagLists.constBegin(); it != flagLists.constEnd(); ++it) {
            out += QStringLiteral("── %1 (%2) ──\n").arg(it.key()).arg(it.value().size());
            int shown = 0;
            for (const QString& ln : it.value()) { out += QStringLiteral("  %1\n").arg(ln); if (++shown >= 300) { out += QStringLiteral("  … +%1 more (see csv)\n").arg(it.value().size() - shown); break; } }
            out += QLatin1Char('\n');
        }
        sf.write(out.toUtf8());
        sf.close();
    }
    return QStringLiteral("Cloth audit%1: %2 appearances, %3 cloth pieces.\n"
                          "Tuning FAILED: %4   no driver-skin: %5   no followers: %6\n\n"
                          "Reports: cloth_audit.csv + cloth_audit_summary.txt (next to the exe).")
               .arg(cancelled ? QStringLiteral(" (partial)") : QString())
               .arg(scanned).arg(simCount).arg(nFailed)
               .arg(flagLists.value(QStringLiteral("NO-DRIVERSKIN")).size())
               .arg(flagLists.value(QStringLiteral("NO-FOLLOWERS")).size());
}

void WardrobeTab2::loadClothTuning()
{
    m_clothTuningFound = false;
    m_gct = GameClothTuning{};   // defaults
    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty()) return;
    static const QRegularExpression rxCloth(
        QStringLiteral("\"snoCloth\"\\s*:\\s*\\{[^}]*?\"name\"\\s*:\\s*\"([^\"]+)\""));
    if (!rxCloth.isValid()) return;
    // Unique equipped piece appearances (m_partSource is per merged-primitive).
    QSet<QString> pieces;
    for (const QString& s : m_partSource) if (!s.isEmpty()) pieces.insert(s);
    QSet<QString> clothNames;
    for (const QString& pc : pieces) {
        if (pc.isEmpty()) continue;
        QFile f(d4 + QStringLiteral("/json/base/meta/Appearance/") + pc + QStringLiteral(".app.json"));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QString raw = QString::fromUtf8(f.readAll());
        auto it = rxCloth.globalMatch(raw);
        while (it.hasNext()) clothNames.insert(it.next().captured(1));
    }
    // Accumulate the authored dmClothTuningMirror across equipped cloth pieces.
    double boneT=0, actorT=0, stretch=0, horiz=0, shear=0, bend=0, damp=0, attach=0, fric=0, dens=0;
    double drag=0, lift=0, wdx=0, wdy=0, wdz=0;
    int n = 0;
    for (const QString& cn : clothNames) {
        QFile cf(d4 + QStringLiteral("/json/base/meta/Cloth/") + cn + QStringLiteral(".clt.json"));
        if (!cf.open(QIODevice::ReadOnly)) continue;
        const QJsonObject t = QJsonDocument::fromJson(cf.readAll()).object()
                                  .value(QStringLiteral("tClothTuning")).toObject();
        if (t.isEmpty()) continue;
        auto g = [&](const char* k, double def) { return t.value(QLatin1String(k)).toDouble(def); };
        boneT  += g("flBoneTrackingFactor", 0.45);
        actorT += g("flActorTrackingFactor", 0.6);
        stretch+= g("flStretchingStiffness", 0.7);
        horiz  += g("flHorizontalStiffness", 0.5);
        shear  += g("flShearStiffness", 0.15);
        bend   += g("flBendingStiffness", 0.3);
        damp   += g("flDampingFactor", 0.25);
        attach += g("flAttachmentStiffness", 0.2);
        fric   += g("flFrictionScale", 1.0);
        dens   += g("flDensity", 2.0);
        // Aerodynamics: vSelfWind (game z-up) × flWindFactor → swap to y-up; drag/lift factors.
        const double wf = g("flWindFactor", 1.0);
        const QJsonObject sw = t.value(QStringLiteral("vSelfWind")).toObject();
        const double sx=sw.value(QStringLiteral("x")).toDouble(), sy=sw.value(QStringLiteral("y")).toDouble(), sz=sw.value(QStringLiteral("z")).toDouble();
        wdx += sx*wf; wdy += sz*wf; wdz += -sy*wf;          // zUp→yUp: (x,y,z)→(x,z,-y)
        drag += g("flDragFactor", 0.0);
        lift += g("flLiftFactor", 0.0);
        ++n;
    }
    if (n > 0) {
        const double inv = 1.0 / n;
        m_gct.boneTrack  = qBound(0.0f, float(boneT  * inv), 1.0f);
        m_gct.actorTrack = qBound(0.0f, float(actorT * inv), 1.0f);
        m_gct.stretch    = qBound(0.0f, float(stretch* inv), 1.0f);
        m_gct.horiz      = qBound(0.0f, float(horiz  * inv), 1.0f);
        m_gct.shear      = qBound(0.0f, float(shear  * inv), 1.0f);
        m_gct.bend       = qBound(0.0f, float(bend   * inv), 1.0f);
        m_gct.damping    = qBound(0.0f, float(damp   * inv), 1.0f);
        m_gct.attach     = qBound(0.0f, float(attach * inv), 1.0f);
        m_gct.friction   = qBound(0.0f, float(fric   * inv / 3.0), 1.0f);   // data 0..3 → 0..1
        m_gct.density    = qMax(0.1f, float(dens * inv));
        m_gct.drag       = qBound(0.0f, float(drag * inv), 1.0f);
        m_gct.lift       = qBound(0.0f, float(lift * inv), 1.0f);
        // Normalise the averaged self-wind into a unit direction (the Wind slider scales it).
        const double wl = std::sqrt(wdx*wdx + wdy*wdy + wdz*wdz);
        if (wl > 1e-6) { m_gct.windDirX = float(wdx/wl); m_gct.windDirY = float(wdy/wl); m_gct.windDirZ = float(wdz/wl); }
        m_clothTuningFound = true;
    }
}


// Compose the eye maps (base colour / normal / ORM / emissive) from the real Hero_Eye textures and
// the per-colour EyeColor def. The fl* constants below are renderer-calibrated (the game's authored
// values assume D4's tonemap and don't translate 1:1 to our viewport); per-colour fields (iris
// inner/outer, sclera brightness, bloodshot, glow) come straight from the .eye.json.
void WardrobeTab2::composeEyeMaps(const QString& d4, const QString& stem, QImage& base, QImage& norm,
                                  QImage& orm, QImage& emis, float& emisMul, float& irisRough)
{
    emisMul = 0.0f;
    auto loadTex = [&](const QString& n) {
        const qint64 s = texSnoFor(n);
        return s > 0 ? MaterialDecode::texture(m_reader, d4, n, s) : QImage();
    };
    const QImage scleraTex = loadTex(QStringLiteral("base_eyes_sclera"));
    const QImage scleraRed = loadTex(QStringLiteral("base_eyes_sclera_red"));
    const QImage irisTex   = loadTex(QStringLiteral("base_eyes_irisColor"));
    const QImage irisMask  = normalizeIrisMask(loadTex(QStringLiteral("base_eyes_irisMask")));
    EyeComposeParams P;
    P.inner        = eyeIrisColor(d4, stem);                       // per-colour (rgbaIrisInner)
    P.outer        = eyeIrisOuterColor(d4, stem);                  // per-colour (rgbaIrisOuter)
    P.scleraBright = eyeScleraBrightness(d4, stem);                // per-colour (0.6 → 0.05 vampire)
    P.scleraRedness= eyeScleraRedness(d4, stem) * 1.62f;           // per-colour, scaled to the tuned normal
    P.scleraDesat  = 0.25f;
    P.irisBright   = 0.176f;   // renderer-calibrated (game flIrisBrightness=2)
    P.limbusThick  = 0.629f;   // renderer-calibrated (game flLimbusThickness=0.86)
    P.limbusBright = 0.515f;   // renderer-calibrated (game flLimbusBrightness=0.047)
    P.irisShadow   = 0.247f;   // renderer-calibrated (game flIrisShadowIntensity=0.05)
    P.irisScale    = 0.65f;    // tuned to the shared eyeball UV
    P.pupilSize    = 0.633f;
    irisRough      = eyeIrisRoughness(d4, stem);                   // per data (flIrisRoughness 0.1)
    const float irisEmis = eyeIrisEmissive(d4, stem);
    base = recolorEyeComposite(scleraTex, scleraRed, irisTex, irisMask, P);
    norm = loadTex(QStringLiteral("BaseEye_Normal"));
    if (norm.isNull()) norm = loadTex(QStringLiteral("SphericalNormal"));
    orm  = buildEyeOrm(irisMask, irisRough, 0.236f /* renderer-calibrated sclera roughness */);
    emis = QImage();
    if (irisEmis > 1.05f && P.inner.isValid()) {                   // glowing Incandescent/Vampire eyes
        emis = buildEyeEmissive(irisMask.isNull() ? irisTex : irisMask, P.inner);
        emisMul = qBound(0.0f, (irisEmis - 1.0f) * 0.6f, 2.0f);
    }
}


// Note: the Gravity slider is stored negated (sign handled in applyClothParams).

// ── Animations (idle / emote clips for the assembled body rig) ─────────────────
void WardrobeTab2::populateAnims()
{
    if (!m_anims) return;
    m_anims->clear();
    const QString d4 = Config::d4dataDir();
    if (d4.isEmpty() || !m_index) return;
    // The body rig carrier is <pref>_base00; its appearance SNO owns the clips.
    const QString pref = classPrefix();
    // Scan the Anim folder for all clips owned by a given class/gender prefix (matched by name
    // prefix AND, when the body appearance exists, by the clip's snoAppearance).
    auto scanFor = [&](const QString& p) -> QStringList {
        int bodySno = 0;
        const QString want = p + QStringLiteral("_base00");
        for (const SnoEntry& e : m_index->entries(kGroupAppearance))
            if (e.name.compare(want, Qt::CaseInsensitive) == 0) { bodySno = e.snoId; break; }
        static const QRegularExpression rxApp(
            QStringLiteral("\"snoAppearance\":\\s*\\{[^{}]*?\"__raw__\":\\s*(\\d+)"));
        static const QRegularExpression rxFrames(QStringLiteral("\"nKeyframeCount\":\\s*(\\d+)"));
        QStringList r;
        QDirIterator it(d4 + QStringLiteral("/json/base/meta/Anim"),
                        QStringList{QStringLiteral("*.ani.json")}, QDir::Files);
        while (it.hasNext()) {
            const QString fp = it.next();
            const QString base = it.fileName();
            if (!base.toLower().startsWith(p)) continue;   // this class/gender's clips
            QFile jf(fp);
            if (!jf.open(QIODevice::ReadOnly)) continue;
            const QString raw = QString::fromUtf8(jf.readAll());
            const auto m = rxApp.match(raw);
            if (bodySno > 0 && (!m.hasMatch() || m.captured(1).toInt() != bodySno)) continue;
            const QString nm = base.left(base.size() - 9);   // strip ".ani.json"
            const auto fm = rxFrames.match(raw);
            r << (fm.hasMatch() ? QStringLiteral("%1  ·  %2 frames").arg(nm, fm.captured(1)) : nm);
        }
        r.sort();
        return r;
    };
    QStringList rows = m_animCache.value(pref);
    if (!m_animCache.contains(pref)) {
        rows = scanFor(pref);
        // Fallback: a gender's rig may ship no dedicated clips (e.g. Warlock female). Borrow the
        // same class's other-gender clips — the skeleton bones match by hash, so they play fine.
        if (rows.isEmpty() && pref.size() == 4 && (pref.endsWith(QLatin1Char('f')) || pref.endsWith(QLatin1Char('m')))) {
            const QChar other = pref.endsWith(QLatin1Char('f')) ? QLatin1Char('m') : QLatin1Char('f');
            rows = scanFor(pref.left(3) + other);
        }
        m_animCache.insert(pref, rows);
    }
    fillAnimList();   // build the list applying the current filter + sort + search
}

// Build the animation list from the cached rows, applying the category filter, sort order,
// and search text. Restores the currently-playing clip's selection without firing playback.
void WardrobeTab2::fillAnimList()
{
    if (!m_anims) return;
    const QStringList rows = m_animCache.value(classPrefix());
    struct A { QString display, name; int frames; };
    QVector<A> items;
    items.reserve(rows.size());
    for (const QString& r : rows) {
        A a; a.display = r;
        a.name = r.section(QStringLiteral("  ·  "), 0, 0);
        const QString tail = r.section(QStringLiteral("  ·  "), 1, 1);   // "N frames"
        a.frames = tail.isEmpty() ? 0 : tail.section(QLatin1Char(' '), 0, 0).toInt();
        items << a;
    }
    const int sort = m_animSort ? m_animSort->currentIndex() : 0;
    std::sort(items.begin(), items.end(), [sort](const A& x, const A& y) {
        switch (sort) {
            case 1:  return x.name.compare(y.name, Qt::CaseInsensitive) > 0;   // Z–A
            case 2:  return x.frames < y.frames;                              // frames ↑
            case 3:  return x.frames > y.frames;                              // frames ↓
            default: return x.name.compare(y.name, Qt::CaseInsensitive) < 0;  // A–Z
        }
    });
    // Category filter token(s) (comma-separated alternatives) + free-text search.
    const QStringList tokens = m_animFilter
        ? m_animFilter->currentData().toString().toLower().split(QLatin1Char(','), Qt::SkipEmptyParts)
        : QStringList();
    const QString search = m_animSearch ? m_animSearch->text().trimmed().toLower() : QString();
    const QString sel = m_playingAnim.isEmpty()
                            ? QSettings().value(QStringLiteral("wardrobe2/anim")).toString()
                            : m_playingAnim;
    // Prefix each clip with its readable action (Idle / Walk / Get Hit…), from the shared AnimSet
    // Power index — so the Wardrobe list reads the same as the Models tab, not raw filenames.
    AnimActionIndex& aai = AnimActionIndex::instance();
    aai.ensure(Config::d4dataDir());
    m_anims->blockSignals(true);
    m_anims->clear();
    for (const A& a : items) {
        const QString low = a.name.toLower();
        if (!tokens.isEmpty()) {
            bool hit = false;
            for (const QString& t : tokens) if (low.contains(t)) { hit = true; break; }
            if (!hit) continue;
        }
        const QString action = aai.action(low);
        const QString label = action.isEmpty() ? a.display
                                               : QStringLiteral("%1   —   %2").arg(action, a.display);
        if (!search.isEmpty() && !label.toLower().contains(search)) continue;   // match action too
        auto* it = new QListWidgetItem(label, m_anims);
        it->setData(Qt::UserRole, a.name);
        const QString set = aai.animSet(low);
        if (!action.isEmpty() || !set.isEmpty())
            it->setToolTip(QStringLiteral("Action: %1\nAnimSet: %2")
                               .arg(action.isEmpty() ? QStringLiteral("—") : action,
                                    set.isEmpty() ? QStringLiteral("—") : set));
        if (a.name == sel) m_anims->setCurrentItem(it);
    }
    m_anims->blockSignals(false);
}

// Decode one clip for the CURRENT merged rig — extracted from playAnimByName so the
// export path can decode clips that aren't playing (scope "All of the model's animations").
AnimParser::DecodedAnim WardrobeTab2::decodeAnimByName(const QString& animName)
{
    AnimParser::DecodedAnim invalid;
    if (animName.isEmpty() || m_lastMerged.skeleton.isEmpty()
        || !m_reader || !m_reader->isReady())
        return invalid;
    const QString d4 = Config::d4dataDir();
    QFile jf(QStringLiteral("%1/json/base/meta/Anim/%2.ani.json").arg(d4, animName));
    if (!jf.open(QIODevice::ReadOnly)) return invalid;
    const QJsonObject root = QJsonDocument::fromJson(jf.readAll()).object();
    const int animSno = root.value(QStringLiteral("__snoID__")).toInt();
    const QJsonArray perms = root.value(QStringLiteral("ptPermutations")).toArray();
    if (animSno <= 0 || perms.isEmpty()) return invalid;
    const QJsonObject perm = perms.first().toObject();
    const QJsonObject pv = perm.value(QStringLiteral("ptPayloadData")).toObject()
                               .value(QStringLiteral("value")).toObject();
    const int offset = pv.value(QStringLiteral("dataOffset")).toInt();
    const int frames = perm.value(QStringLiteral("nKeyframeCount")).toInt();
    const int comp = perm.value(QStringLiteral("flCompression")).toInt();
    const float fps = float(perm.value(QStringLiteral("flFrameRate")).toDouble(30.0));
    if (frames <= 0) return invalid;
    const QByteArray payload = m_reader->readPayloadBySno(quint64(animSno));
    if (payload.isEmpty()) return invalid;

    QHash<quint32, AnimParser::RestTRS> rest;
    for (const ModelJoint& j : m_lastMerged.skeleton) {
        AnimParser::RestTRS t; t.q = j.restQ; t.t = j.restT; t.s = j.restS;
        rest.insert(j.nameHash, t);
    }
    // Guard the decode — a malformed .ani payload would otherwise crash the process.
    AnimParser::DecodedAnim out;
    if (!seh::runGuarded("w2AnimDecode", [&]() { out = AnimParser::decode(payload, offset, frames, comp, fps, rest); }))
        return AnimParser::DecodedAnim{};
    return out;
}

// Human summary of what a card export will include, per Settings ▸ Export: always "1 model", plus
// the animation count (or the playing/selected clip name, if scope = playing) when animations are
// on, plus the raw-source file count (.app + distinct .tex) when raw export is on. All cheap — the
// clip count is the ANIMATIONS list size; the raw count parses the item's materials.
// The export suffix leads with "1 model", which carries no information when only one item can be
// selected. Strip it and keep the rest (animation scope/clip), so the new labels stay informative.
QString WardrobeTab2::exportMenuExtras(const QString& suffix)
{
    QStringList parts = suffix.split(QStringLiteral(" + "), Qt::SkipEmptyParts);
    for (int i = parts.size() - 1; i >= 0; --i)
        if (parts[i].trimmed() == QLatin1String("1 model")) parts.removeAt(i);
    return parts.isEmpty() ? QString() : QStringLiteral("  —  %1").arg(parts.join(QStringLiteral(" + ")));
}

QString WardrobeTab2::exportMenuSuffix(int sno, const QString& appr)
{
    Q_UNUSED(sno);
    QSettings s;
    QStringList parts; parts << QStringLiteral("1 model");
    if (s.value(QStringLiteral("export/includeAnim"), false).toBool()) {
        if (s.value(QStringLiteral("export/animScope"), 0).toInt() == 1) {
            const int n = m_anims ? m_anims->count() : 0;
            parts << QStringLiteral("%1 animation%2").arg(n).arg(n == 1 ? QString() : QStringLiteral("s"));
        } else {
            QString clip;
            if (m_anims && !m_anims->selectedItems().isEmpty())
                clip = m_anims->selectedItems().first()->data(Qt::UserRole).toString();
            parts << (clip.isEmpty() ? QStringLiteral("playing clip") : QStringLiteral("clip: %1").arg(clip));
        }
    }
    if (s.value(QStringLiteral("export/withDeps"), false).toBool() && !appr.isEmpty()) {
        const QString d4 = Config::d4dataDir();
        QSet<qint64> tex;
        for (const QString& mn : MaterialDecode::appearanceRoster(d4, appr)) {
            if (mn.isEmpty()) continue;
            QFile mf(d4 + QStringLiteral("/json/base/meta/Material/") + mn + QStringLiteral(".mat.json"));
            if (!mf.open(QIODevice::ReadOnly)) continue;
            for (const MatTexture& mt : parseMaterialJson(mf.readAll())) if (mt.texSno) tex.insert(mt.texSno);
        }
        const int raw = 1 + tex.size();
        parts << QStringLiteral("%1 raw file%2").arg(raw).arg(raw == 1 ? QString() : QStringLiteral("s"));
    }
    return parts.join(QStringLiteral(" + "));
}

// Gather the clips to embed, honoring Settings ▸ Export ▸ "Animations to embed".
// The old behavior only embedded a PLAYING clip — with the preview idle, exports
// silently carried no animation at all ("missing animations in Blender").
void WardrobeTab2::collectExportAnims(QVector<AnimParser::DecodedAnim>& anims, QStringList& names)
{
    if (!QSettings().value(QStringLiteral("export/includeAnim"), false).toBool())
        return;
    // Explicit multi-selection wins over the scope setting: embed exactly the ctrl/shift-
    // selected clips (a precise middle ground between "playing clip" and "all listed").
    if (m_anims && m_anims->selectedItems().size() > 1) {
        const auto sel = m_anims->selectedItems();
        QProgressDialog prog(QStringLiteral("Decoding selected animations…"), QStringLiteral("Cancel"),
                             0, sel.size(), this);
        prog.setWindowModality(Qt::WindowModal);
        for (int i = 0; i < sel.size(); ++i) {
            if (prog.wasCanceled()) break;
            const QString nm = sel[i]->data(Qt::UserRole).toString();
            prog.setValue(i); prog.setLabelText(nm);
            QCoreApplication::processEvents();
            const AnimParser::DecodedAnim a = decodeAnimByName(nm);
            if (a.valid && !a.bones.isEmpty()) { anims << a; names << nm; }
        }
        prog.setValue(sel.size());
        if (!anims.isEmpty()) return;
    }
    bool wantAll = QSettings().value(QStringLiteral("export/animScope"), 0).toInt() == 1
                   && m_anims && m_anims->count() > 0;
    if (wantAll && m_anims->count() > 25) {
        const auto r = QMessageBox::question(this, QStringLiteral("Embed animations"),
            QStringLiteral("Embed all %1 clips currently listed in the ANIMATIONS panel?\n"
                           "Every clip is decoded — this takes a while and makes a large "
                           ".glb.\n\nYes = all listed clips (narrow the list with the "
                           "category/search filters first to embed fewer).\n"
                           "No = just the playing/selected clip.")
                .arg(m_anims->count()),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (r == QMessageBox::Cancel) return;                 // no animation at all
        if (r == QMessageBox::No) wantAll = false;
    }
    if (wantAll) {
        QProgressDialog prog(QStringLiteral("Decoding animations…"), QStringLiteral("Cancel"),
                             0, m_anims->count(), this);
        prog.setWindowModality(Qt::WindowModal);
        for (int i = 0; i < m_anims->count(); ++i) {
            if (prog.wasCanceled()) break;
            const QString nm = m_anims->item(i)->data(Qt::UserRole).toString();
            prog.setValue(i);
            prog.setLabelText(nm);
            QCoreApplication::processEvents();
            const AnimParser::DecodedAnim a = decodeAnimByName(nm);
            if (a.valid && !a.bones.isEmpty()) { anims << a; names << nm; }
        }
        prog.setValue(m_anims->count());
        if (!anims.isEmpty())
            return;
    }
    if (m_curAnim.valid && !m_playingAnim.isEmpty()) {        // the playing clip
        anims << m_curAnim; names << m_playingAnim;
        return;
    }
    if (m_anims && m_anims->currentItem()) {                  // nothing playing → selected entry
        const QString nm = m_anims->currentItem()->data(Qt::UserRole).toString();
        const AnimParser::DecodedAnim a = decodeAnimByName(nm);
        if (a.valid && !a.bones.isEmpty()) { anims << a; names << nm; }
    }
}

void WardrobeTab2::playAnimByName(const QString& animName)
{
    if (!m_view)
        return;
    const AnimParser::DecodedAnim anim = decodeAnimByName(animName);
    if (!anim.valid) return;

    m_playingAnim = animName;
    m_curAnim = anim;   // retained for "include animation" .glb export
    QSettings().setValue(QStringLiteral("wardrobe2/anim"), animName);   // remember selection
    if (!seh::runGuarded("w2SetAnim", [&]() { m_view->setAnimation(anim); })) return;
    m_timeline->setVisible(true);   // (restored) timeline shows once a clip is playing
    m_animSlider->blockSignals(true);
    m_animSlider->setRange(0, anim.frameCount - 1);
    m_animSlider->setValue(0);
    // Frame ticks: aim for ~40 marks, snapped to a friendly step (matches the Models tab).
    {
        int step = qMax(1, (anim.frameCount + 39) / 40);
        if (step > 2 && step <= 7)       step = 5;
        else if (step > 7 && step <= 15) step = 10;
        else if (step > 15)              step = ((step + 24) / 25) * 25;
        m_animSlider->setTickInterval(step);
        m_animSlider->setPageStep(qMax(1, anim.frameCount / 10));
    }
    m_animSlider->blockSignals(false);
    if (m_frameSpin) { QSignalBlocker b(m_frameSpin); m_frameSpin->setRange(0, qMax(0, anim.frameCount - 1)); m_frameSpin->setValue(0); }
    if (m_frameMax)  m_frameMax->setText(QStringLiteral("/ %1").arg(qMax(0, anim.frameCount - 1)));
    m_animFps = anim.frameRate > 0 ? anim.frameRate : 30.0f;
    applyAnimSpeed();
    m_animTimer->start();
    m_playBtn->setIcon(transportGlyph(1));
}

// Proper debug log/console for the currently-loaded character. The full per-piece / MARK / HAIR
// diagnostic is assembled during rebuildOutfit() and stashed on m_status's tooltip; here we surface
// it in a real scrollable, selectable window, route it into the app-wide live log (File ▸ console),
// and copy it to the clipboard — replacing the old "hover for details" tooltip-only mechanism.
void WardrobeTab2::showDebugConsole()
{
    QString full = m_status ? m_status->toolTip() : QString();
    if (full.trimmed().isEmpty())
        full = QStringLiteral("(no debug output yet — pick a class and equipment, then load the character first)");

    QGuiApplication::clipboard()->setText(full);        // fix: copy the REAL debug, not the summary line
    qInfo().noquote() << "\n===== Wardrobe 2 debug =====\n" << full;   // mirror into LogBuffer + log file

    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QStringLiteral("Wardrobe 2 — debug log"));
    dlg->resize(760, 560);
    auto* v = new QVBoxLayout(dlg);
    v->setContentsMargins(8, 8, 8, 8);
    auto* te = new QPlainTextEdit(dlg);
    te->setReadOnly(true);
    te->setLineWrapMode(QPlainTextEdit::NoWrap);
    te->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    te->setPlainText(full);
    v->addWidget(te);
    auto* row = new QHBoxLayout();
    auto* copyBtn  = new QPushButton(QStringLiteral("Copy"),  dlg);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), dlg);
    row->addStretch(); row->addWidget(copyBtn); row->addWidget(closeBtn);
    v->addLayout(row);
    connect(copyBtn,  &QPushButton::clicked, dlg, [te] { QGuiApplication::clipboard()->setText(te->toPlainText()); });
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::close);
    dlg->show();
}

void WardrobeTab2::applyAnimSpeed()
{
    float mult = 1.0f;
    if (m_speedCombo) {
        const QString s = m_speedCombo->currentText();
        bool ok = false;
        const float v = s.left(s.size() - 1).toFloat(&ok);
        if (ok && v > 0.0f) mult = v;
    }
    const float eff = m_animFps * mult;
    if (m_animTimer) m_animTimer->setInterval(eff > 0.0f ? int(1000.0f / eff) : 33);
}

void WardrobeTab2::tickAnimation()
{
    const int fc = m_view ? m_view->animFrameCount() : 0;
    if (fc <= 0) { m_animTimer->stop(); return; }
    int next = m_animSlider->value() + 1;
    if (next >= fc) {
        if (m_loopCheck && m_loopCheck->isChecked()) next = 0;
        else { m_animTimer->stop(); m_playBtn->setIcon(transportGlyph(0)); return; }
    }
    m_animSlider->setValue(next);
}

void WardrobeTab2::clearAnimationSelection()
{
    m_playingAnim.clear();
    m_curAnim = {};   // drop the retained clip so exports don't embed a stale animation
    QSettings().remove(QStringLiteral("wardrobe2/anim"));   // forget the remembered clip
    if (m_animTimer) m_animTimer->stop();
    if (m_playBtn) m_playBtn->setIcon(transportGlyph(0));
    if (m_timeline) m_timeline->setVisible(false);
    if (m_view) m_view->clearAnimation();
    if (m_anims) { m_anims->blockSignals(true); m_anims->clearSelection();
                   m_anims->setCurrentItem(nullptr); m_anims->blockSignals(false); }
}

void WardrobeTab2::remapAnimationForRig()
{
    const QString oldName = m_playingAnim;
    populateAnims();                       // refresh the clip list/cache for the new prefix
    if (oldName.size() < 4) return;        // nothing was playing
    // Swap the 4-char class+gender prefix (e.g. barf → barm) and play the same clip on the new
    // rig if that .ani.json actually exists on disk (bones match by hash). This is checked against
    // the file directly rather than the filtered clip cache, which can reject valid clips.
    const QString newName = classPrefix() + oldName.mid(4);
    const QString d4 = Config::d4dataDir();
    if (!d4.isEmpty()
        && QFile::exists(d4 + QStringLiteral("/json/base/meta/Anim/") + newName + QStringLiteral(".ani.json")))
        playAnimByName(newName);
    else
        clearAnimationSelection();
}
  