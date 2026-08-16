#include "app/SettingsDialog.h"

#include "app/AppLog.h"
#include "app/AppPaths.h"

#include "app/Config.h"
#include "util/AnimExportScope.h"   // the five animation sources + pre-2.2.8 migration
#include "app/Hotkeys.h"
#include "deps/D4DataDownloader.h"
#include "deps/DependencyDialog.h"
#include "deps/UpdateCheck.h"

#include <QApplication>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QComboBox>
#include <QImageWriter>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTextBrowser>
#include <QKeySequenceEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QPair>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QFrame>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
// ExportLayout::kFlat() etc — the shared folder-layout ids.
#include "util/ExportLayout.h"
#include <QTabBar>      // elide/scroll policy for the nine-page tab strip
#include <QTabWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <functional>

namespace {
// Candidate locations, newest-known first (the upstream repo moved this file once already:
// root/d4_tact_keys_clean.txt → keys/d4.keys). curl is invoked with each in turn, so a future
// move degrades to a fallback hit instead of a hard download failure. Keep in sync with
// UpdateCheck.cpp's kTactKeysUrls.
constexpr const char* kTactKeysUrls[] = {
    "https://raw.githubusercontent.com/HoldMyBeer-gg/rustydemon/main/keys/d4.keys",
    "https://raw.githubusercontent.com/HoldMyBeer-gg/rustydemon/main/d4_tact_keys_clean.txt",
};

// SECOND source, fetched alongside the first rather than instead of it. The two barely overlap:
// measured against a live build, CascLib's "Diablo IV Retail" block holds 361 keys of which ZERO
// are in our rustydemon-derived set, and exactly one still has an EncryptedNameDict in the current
// patch (it names Bundle_HMount_stor023). So it is a small but real gain, and costs one HTTP GET.
//
// Saved with a .keys extension so applyTactKeys' folder mode picks it up; loadKeysFromFile detects
// the C# format by content and takes only the Diablo IV section.
constexpr const char* kCascLibKeysUrl =
    "https://raw.githubusercontent.com/WoW-Tools/CascLib/master/CascLib/KeyService.cs";
constexpr const char* kCascLibKeysFile = "casclib_d4_retail.keys";

// "… · last checked 3 h ago" — so a green "up to date" can't be mistaken for fresh when it's stale.
QString lastCheckedSuffix()
{
    const QDateTime t = QSettings().value(QStringLiteral("updates/lastChecked")).toDateTime();
    if (!t.isValid()) return QString();
    const qint64 secs = t.secsTo(QDateTime::currentDateTime());
    QString ago;
    if      (secs < 60)    ago = QStringLiteral("just now");
    else if (secs < 3600)  ago = QStringLiteral("%1 min ago").arg(secs / 60);
    else if (secs < 86400) ago = QStringLiteral("%1 h ago").arg(secs / 3600);
    else                   ago = QStringLiteral("%1 d ago").arg(secs / 86400);
    return QStringLiteral("   ·   last checked %1").arg(ago);
}

QString checkMark(bool ok)
{
    return ok ? QStringLiteral("<span style='color:#4caf50;font-weight:bold'>&#10003;</span>")
              : QStringLiteral("<span style='color:#888'>&mdash;</span>");
}
}

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Settings"));
    resize(680, 0);
    // Must be able to shrink. A user on a short monitor could not reach OK/Cancel because the
    // dialog sized itself to the tallest page and ran off the bottom of the screen — with the
    // buttons below the tabs, that made the whole dialog unusable, not just hard to read.
    setMinimumSize(520, 320);
    setSizeGripEnabled(true);

    auto* root = new QVBoxLayout(this);

    // Grouped into tabs so the dialog stays compact as categories grow. The version line +
    // OK/Cancel live below the tabs so they're always visible.
    //
    // Every page is wrapped in a QScrollArea. The comment here used to claim the pages were "a
    // plain scrolling column" — they were not; nothing scrolled, so a page's full height became
    // the dialog's minimum height and the window grew past the screen. Now the page scrolls and
    // the dialog is free to be any size, which is what showEvent's clamp relies on.
    auto* tabs = new QTabWidget(this);
    // Nine pages overflow the tab bar, and Qt's default response is to ELIDE each label to fit —
    // which is how the bar came to read "eneral" and "Experim". Elision on a tab bar is the wrong
    // trade: a half-word label is unreadable, whereas a tab you have to scroll to is merely one
    // click further away. So: never elide, don't stretch tabs to fill (which shrinks every label to
    // make room for the widest), and scroll when the bar genuinely cannot fit.
    //
    // This is the SAFETY NET, not the fix. showEvent() folds the bar's full width into the size it
    // asks for, so on any normal screen every tab is visible and the scroll buttons never appear.
    tabs->setUsesScrollButtons(true);
    tabs->tabBar()->setElideMode(Qt::ElideNone);
    tabs->tabBar()->setExpanding(false);
    auto makeTab = [tabs](const QString& title) -> QVBoxLayout* {
        auto* page = new QWidget;
        auto* lay  = new QVBoxLayout(page);
        auto* scroll = new QScrollArea(tabs);
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);      // page tracks the viewport width; scrolls vertically
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        tabs->addTab(scroll, title);
        return lay;
    };
    // Page order is "how often you touch it, and what it is about": setup → presentation → the
    // three per-area pages → what lands on disk → keys → upkeep → reference → experimental.
    QVBoxLayout* generalTab  = makeTab(QStringLiteral("General"));
    // Split out of General, which had grown to SEVEN boxes spanning four unrelated concerns while
    // Models and Hotkeys held one each. Everything here answers "what does the app show me";
    // General keeps only "where does the data come from" plus the profile.
    QVBoxLayout* interfaceTab = makeTab(QStringLiteral("Interface"));
    QVBoxLayout* modelsTab   = makeTab(QStringLiteral("Models"));    // its own tab, like Wardrobe
    QVBoxLayout* wardrobeTab = makeTab(QStringLiteral("Wardrobe"));
    QVBoxLayout* exportTab   = makeTab(QStringLiteral("Export"));
    m_exportTabIndex = tabs->count() - 1;   // remember for showExportTab()
    QVBoxLayout* hotkeysTab  = makeTab(QStringLiteral("Hotkeys"));
    QVBoxLayout* maintTab    = makeTab(QStringLiteral("Maintenance"));
    QVBoxLayout* infoTab     = makeTab(QStringLiteral("Information"));
    // Right of Information on purpose: it is the least-used page and the one most likely to be
    // opened by accident, so it sits at the far end rather than between everyday settings.
    QVBoxLayout* experimentalTab = makeTab(QStringLiteral("Experimental"));
    m_tabs = tabs;   // kept so callers can open the dialog on a specific tab

    // Bold sub-heading inside a group box. A long box was a single flat run of controls under one
    // heading, which made every option below that heading read as if it belonged to it — the four
    // animation checkboxes and "Include base body" both sat under "Animations to embed". Sections
    // are labels, not nested QGroupBoxes: a frame inside a frame inside a tab is three borders to
    // say one thing. Matching the existing "<b>Panels</b>" / "<b>Rebuild caches</b>" idiom.
    auto sectionLabel = [](QWidget* parent, const QString& text) {
        auto* l = new QLabel(QStringLiteral("<b>") + text + QStringLiteral("</b>"), parent);
        l->setStyleSheet(QStringLiteral("color:#aaa;margin-top:8px;"));
        return l;
    };

    // ── Tab: Information ───────────────────────────────────────────────
    // A read-only reference explaining how the tool reads the game and what each file type is, so
    // it is not a black box.
    //
    // Sub-tabbed like the Export tab, for the same reason it was: this was ONE scroll of rich text
    // running from CASC archives through icon atlases to file extensions, so reaching the paragraph
    // you wanted meant scrolling past four you did not, with no way to tell how much was left.
    // Split by SUBJECT — which is how the question arrives — and the loose-textures comparison sits
    // on the page it belongs to instead of underneath everything.
    {
        auto* infoSub = new QTabWidget(this);
        infoSub->setDocumentMode(true);
        // Same policy as the outer bar and the Export sub-bar: never elide a page label to fit.
        infoSub->tabBar()->setElideMode(Qt::ElideNone);
        infoSub->tabBar()->setExpanding(false);
        infoSub->setUsesScrollButtons(true);

        // One stylesheet, prepended to every page, so headings and <code> spans match across them.
        const QString kInfoCss = QStringLiteral(R"CSS(<style>
  h2 { color:#8ab4f8; margin:14px 0 4px 0; }
  h3 { color:#d8a23a; margin:12px 0 2px 0; }
  code { background:#2c2c2c; color:#e0e0e0; padding:1px 4px; border-radius:2px; }
  p, li { line-height:1.4; }
  .k { color:#9ccc65; font-weight:bold; }
</style>)CSS");

        // Each page is its own QTextBrowser: they scroll independently, so switching pages does not
        // land you halfway down someone else's section.
        auto makeInfoPage = [&, kInfoCss](const QString& title, const QString& body) -> QVBoxLayout* {
            auto* w = new QWidget(infoSub);
            auto* v = new QVBoxLayout(w);
            v->setContentsMargins(0, 8, 0, 0);
            auto* doc = new QTextBrowser(w);
            doc->setOpenExternalLinks(false);
            doc->setStyleSheet(QStringLiteral(
                "QTextBrowser{background:#1e1e1e;border:1px solid #3a3a3a;border-radius:4px;"
                "color:#cccccc;padding:8px;}"));
            doc->setHtml(kInfoCss + body);
            v->addWidget(doc, 1);
            infoSub->addTab(w, title);
            return v;
        };

        makeInfoPage(QStringLiteral("Reading the game"), QStringLiteral(R"HTML(
<h2>How this tool reads Diablo IV</h2>
<p>Nothing here is modified in the game. The browser reads the game's own asset
storage on disk, decodes it in memory, and (on export) writes standard files like
<code>.glb</code>, <code>.png</code> and <code>.gif</code>. There are two data sources:</p>
<ul>
<li><span class="k">CASC</span> &mdash; the game's packed storage. D4 keeps every asset inside a
nested TVFS whose internal paths are <b>numeric</b> (<code>base/meta/&lt;sno&gt;</code>,
<code>base/payload/&lt;sno&gt;</code>) &mdash; no folders or extensions, just an <b>SNO</b> id number
per asset. The tool parses the build config, the <code>.idx</code> index and the TVFS root itself,
then <b>BLTE-decodes</b> each blob (raw / zlib / lz4 / Salsa20-encrypted) when it's read.</li>
<li><span class="k">d4data</span> &mdash; a JSON snapshot of the game's metadata tables (the
<code>.json</code> files). Used for names, relationships and tuning &mdash; <i>not</i> geometry.</li>
</ul>

<h2>SNO &mdash; the id behind everything</h2>
<p>Every asset has an <b>SNO</b> (Sequence Number Object) id. The tool's indexes map friendly
names &harr; SNO &harr; the numeric CASC path, which is how a click on "Fur-Lined Pants" ends up
reading the right blob out of packed storage.</p>
)HTML"));

        makeInfoPage(QStringLiteral("Models && cloth"), QStringLiteral(R"HTML(
<h2>Models &mdash; <code>.app</code> &rarr; <code>.glb</code></h2>
<p>An <span class="k">.app</span> is a D4 <b>appearance / model</b>: it holds the mesh geometry,
the skeleton, and the list of materials a model uses. It's stored as a paired <i>meta</i> +
<i>payload</i> blob in CASC. The parser reads the highest-detail mesh (LOD0): the vertex and
index buffers (36-byte stride for static meshes, 44-byte for skinned), the bone hierarchy
(BoneData / BoneStructure), and each vertex's bone indices + weights. Coordinates are converted
from the game's z-up space to the y-up space used by glTF and most 3D tools
(<code>(x, y, z) &rarr; (x, z, -y)</code>).</p>
<p>On <b>Export .glb</b>, this becomes a self-contained glTF binary: mesh, a proper skin with the
bone-node hierarchy and inverse-bind matrices, plus embedded textures &mdash; so it opens correctly
in Blender, etc. Verified vertex-for-vertex and matrix-for-matrix against the reference exporter.</p>

<h2>Cloth &amp; physics</h2>
<p>Skirts, capes and chains use the game's authored <b>NvCloth</b> data, parsed straight from each
piece's ClothData: collision capsules (bone-bound tubes around the limbs), low-poly simulation
cages, per-vertex pin/motion constraints, and per-piece tuning from the matching
<code>.clt.json</code>. The viewport simulates those cages against the capsules and drives the
skinned cloth from them, matching how the game does it.</p>
)HTML"));

        QVBoxLayout* pgInfoMat = makeInfoPage(QStringLiteral("Materials && textures"), QStringLiteral(R"HTML(
<h2>Materials &amp; textures &mdash; <code>.tex</code> &rarr; images</h2>
<p>Each material (from its <code>.mat.json</code> &rarr; tUberMaterial) lists <b>texture bindings</b>
&mdash; BASE_COLOR, NORMAL, ROUGHNESS/METAL/AO, EMISSIVE &mdash; plus scalar MaterialValues
(metallic, roughness, AO, emissive strength). A <span class="k">.tex</span> is the actual texture
payload, block-compressed (BC1/BC3/BC4/BC5/BC7). The tool CPU-decodes those blocks (bit-exact vs
reference) and can embed a model's base-colour map in the <code>.glb</code> or export loose
<code>.png</code>s. Detail maps carry their own tiling scale (e.g. 8&ndash;20&times;).</p>
)HTML"));

        makeInfoPage(QStringLiteral("Icons"), QStringLiteral(R"HTML(
<h2>Original icons &mdash; how each model gets the right one</h2>
<p>Inventory icons aren't separate files. They're packed into <b>sprite-sheet atlases</b>
(<code>2DInventory_*</code> / <code>2DUI_*</code> textures), and the game addresses each icon by a
numeric <b>image handle</b>. Binding a model to its correct icon is a metadata crawl (done once, on a
background thread, then cached). It runs in two halves &mdash; find the handle, then turn the handle
into a picture.</p>

<h3>1 &middot; Which item owns the model</h3>
<p>Every <code>Item/&lt;name&gt;.itm.json</code> is walked and routed to the model
(<b>appearance SNO</b>) it depicts, in priority order:</p>
<ul>
<li><b>Mounts / pets</b> &mdash; the visible mesh is the ridden/companion actor, so the item's
<code>snoMount</code> / <code>snoCompanion</code> &rarr; <code>Actor/*.acr.json</code> &rarr;
<code>snoAppearance</code> is used (not the reins item's own actor).</li>
<li><b>Cosmetic name rule</b> &mdash; <code>Helm_Cosmetic_Barb_150_stor</code> expands to appearance
names like <code>barf_stor150_hlm</code> / <code>barm_stor150_hlm</code>.</li>
<li><b>Actor SLOT_style</b> &mdash; the item's actor is often a style name
(<code>HLM_sets53</code>); it's expanded over the item's usable classes &times; both genders
(<code>&lt;cls&gt;&lt;g&gt;_sets53_hlm</code>&hellip;).</li>
<li><b>Fallbacks</b> &mdash; the actor's own <code>snoAppearance</code>, then a direct name match
(barding / mount trophies name their appearance the same as the item).</li>
</ul>

<h3>2 &middot; Which handle, for which class &amp; gender</h3>
<p>The item's <code>tInvImages</code> array is indexed by the <b>eHeroClass</b> enum (all 8 classes);
each entry holds <code>hDefaultImage</code> (male) and <code>hFemaleImage</code> (female). The
appearance's own class code (e.g. <code>barF_</code> &rarr; Barbarian/Female, read straight from the
name) selects <i>that class's</i> icon &mdash; so Spiritborn/Paladin/Warlock get their own art, not a
generic set's Sorcerer icon. Store cosmetics/pets that carry a single handle use it as a fallback.</p>
<p><b>Specificity:</b> a class-specific item (usable by one class, from the <code>fUsableByClass</code>
mask) outranks a generic all-class item for the same appearance &mdash; except base-armour looks
(<code>barF_base02_HLM</code>&hellip;), which <i>are</i> the generic loot art.</p>
<p><b>Staying current:</b> the local d4data snapshot lags live patches, so two backstops fill gaps:
the <code>diablo4.dad</code> database (authoritative per-class handles, keyed by item name) supplies
classes the snapshot is missing, and a delta pass over <b>CoreTOC</b> (always patch-current) recovers
items added since the snapshot &mdash; the fix for "icons vanished after an update."</p>

<h3>3 &middot; Handle &rarr; cropped image</h3>
<p>A separate index scans every <code>Texture/2D*.tex.json</code>. Each atlas exposes a
<code>ptFrame</code> list keyed by <code>hImageHandle</code>, giving that icon's UV rectangle
(<code>flU0/flV0/flU1/flV1</code>). To draw an icon the atlas <code>.tex</code> payload is BC-decoded
once (cached) and the UV rect is cropped out. If a patch left the atlas dimensions stale, an exported
frame-index override crops it correctly instead.</p>
)HTML"));

        makeInfoPage(QStringLiteral("Names, tags && files"), QStringLiteral(R"HTML(
<h2>Translated names &amp; collections</h2>
<p>The friendly title and the collection come from the localized string tables in
<code>enUS_Text/meta/StringList/*.stl.json</code> (each a list of label &rarr; text rows):</p>
<ul>
<li><b>Title</b> &mdash; the row labelled <code>Name</code> in <code>Item_&lt;name&gt;.stl.json</code>
(falling back to the item's own string table). So "Savage Galea" is the game's real item name, not
the internal <code>barF_stor150_HLM</code>.</li>
<li><b>Collection</b> &mdash; store bundles/series link through
<code>StoreProduct/&lt;name&gt;.prd.json</code>: its <code>snoItemTransmog</code> points at the item
(hence its appearances), and the product's <code>Series</code> string label becomes the collection
(e.g. a promo set name). The product's <code>Name</code> is also a title fallback for transmogs.</li>
</ul>

<h2>Tags &amp; filters</h2>
<p>The Models tab's Type / Class / Gender filters are the same crawl's by-product. <b>Class</b> and
<b>gender</b> are read from the appearance name (<code>&lt;cls&gt;&lt;g&gt;_</code>);
<b>weapon vs armor</b> from the item's <code>ItemType</code> (<code>eWeaponClass</code> &rarr; weapon,
<code>arBodySlots</code> &rarr; armor); and a <b>transmog</b> tag from the item's
<code>bIsTransmog</code> flag. Every tag is attached to the appearance SNO, so filtering is instant.</p>

<h2>File types you'll see</h2>
<ul>
<li><code>.app</code> &mdash; D4 appearance/model (geometry + skeleton + material roster). Read from CASC.</li>
<li><code>.tex</code> &mdash; D4 texture payload (block-compressed). Decoded to images.</li>
<li>d4data <b>metadata</b> JSON (names &amp; relationships, not geometry):
<code>.app.json</code> appearance, <code>.mat.json</code> material, <code>.itm.json</code> item,
<code>.acr.json</code> actor, <code>.itt.json</code> item type, <code>.prd.json</code> store product,
<code>.clt.json</code> cloth tuning, and <code>StringList/*.stl.json</code> localized text
(titles &amp; collections). Icon atlases are described by <code>Texture/2D*.tex.json</code>.</li>
<li><code>.glb</code> &mdash; the exported model: a glTF binary bundling mesh + skeleton + textures.</li>
<li><code>.png</code> &mdash; exported texture image. &nbsp; <code>.gif</code> &mdash; exported turntable / animation loop.</li>
<li><code>.txt</code> &mdash; diagnostic logs the tool writes next to the program (e.g.
<code>icon_audit.txt</code> = icon coverage, <code>cloth_parse.txt</code> = parsed cloth data,
<code>casc_coverage.txt</code> = storage read stats). Purely informational &mdash; safe to delete.</li>
<li><code>_bulk_report.csv</code> &mdash; optional per-run Bulk Extract report (name &middot; SNO
&middot; status &middot; reason &middot; size).</li>
</ul>
)HTML"));

        // ── Group: which loose-texture option writes what ──────────────────────────────────────
        // On the Materials page, because that is the subject — and here rather than beside the
        // checkboxes themselves because it needs the width: four columns do not fit the Export
        // tab's form field column, which is ~350px once the label column and margins are taken.
        //
        // Three settings write into a folder called "textures", two on Export ▸ Models and one on
        // Export ▸ Wardrobe, Catalogue & Bulk, so nothing in the UI ever showed them together —
        // and the folder name, the only thing they share, is what all three labels used to lead
        // with. What separates them is HOW THEY PICK what to write.
        auto* texGuide = new QGroupBox(QStringLiteral("Loose textures — which option writes what"), this);
        auto* tgv = new QVBoxLayout(texGuide);
        auto* texGuideBody = new QLabel(QStringLiteral(
            "<p style='color:#ccc;'>Three settings can put texture files next to an exported model. "
            "They are additive, not alternatives — the file names never collide, so any combination "
            "is fine. What differs is <b>how each one decides what to write</b>.</p>"
            "<table cellspacing='0' cellpadding='5'>"
            "<tr style='color:#8ab4f8;'>"
              "<td width='20%'><b>Setting</b></td><td width='27%'><b>Picks</b></td>"
              "<td width='27%'><b>What you get</b></td><td width='26%'><b>Written to</b></td></tr>"

            "<tr><td style='color:#d8a23a;'>Write the .glb's own four maps as PNGs<br>"
              "<span style='color:#777;'>Export &rarr; Models</span></td>"
              "<td>the maps the model's <b>materials</b> bind — base colour, normal, ORM, emissive</td>"
              "<td>the glTF versions, not the originals: ORM is one image packing AO into red, "
                  "roughness into green, metalness into blue, and the normal's blue channel has "
                  "been rebuilt. PNG</td>"
              "<td><code>textures\\</code><br>"
                  "<span style='color:#777;'>&lt;model&gt;_&lt;material&gt;_basecolor.png</span></td></tr>"

            "<tr><td style='color:#d8a23a;'>Write every map the materials bind, as the game stores "
              "them<br><span style='color:#777;'>Export &rarr; Models</span></td>"
              "<td>the same materials, but <b>all</b> of what they bind — the three tiled detail "
                  "normals and their roughness maps, translucency, cutout mask, dye mask, dye ramp. "
                  "A .glb has no slot for any of those, so files are the only way out</td>"
              "<td>exactly as the game stores them — nothing packed, nothing reconstructed — plus "
                  "<code>textures.txt</code> listing each map's slot, role, SNO and UV tiling. "
                  "You need that: detail maps repeat 8–20× across the surface, so the PNG alone "
                  "will not rebuild the material. PNG</td>"
              "<td><code>textures\\material_maps\\</code><br>"
                  "<span style='color:#777;'>&lt;model&gt;_&lt;material&gt;_&lt;role&gt;.png</span></td></tr>"

            "<tr><td style='color:#d8a23a;'>Write every texture whose name matches an exported "
              "model<br><span style='color:#777;'>Export &rarr; Wardrobe, Catalogue &amp; Bulk</span></td>"
              "<td>ignores materials entirely — any texture whose <b>name starts with</b> an "
                  "exported model's name. So it also finds recolour variants, atlases and sheets "
                  "no material references</td>"
              "<td>as the game stores them, keeping the texture's own game name. The only one that "
                  "honours <b>Texture export format</b>, so PNG <i>or</i> JPEG</td>"
              "<td><code>textures\\</code><br>"
                  "<span style='color:#777;'>the texture's own name</span></td></tr>"
            "</table>"

            "<p style='color:#ccc;'><b>Two things that catch people out.</b></p>"
            "<ul style='color:#ccc;'>"
            "<li>Only the <b>first</b> one needs <b>Include textures</b> switched on — it copies "
                "what the .glb embedded, so with that off there is nothing to copy. The other two "
                "decode the textures themselves and work either way.</li>"
            "<li>The first two only cover models <i>this run actually exported</i>. On a Bulk "
                "Extract re-run set to <b>Only new</b>, every model is skipped and neither writes "
                "anything. The third runs over everything that <i>matched</i>, so it still fills "
                "the folder.</li>"
            "</ul>"
            "<p style='color:#888;'>Weapon textures carry no class or category marker and are named "
            "purely after their model, so the third option is the only route the data supports — "
            "which is why Bulk Extract's \"All Weapons\" preset switches it on.</p>"), texGuide);
        texGuideBody->setWordWrap(true);
        tgv->addWidget(texGuideBody);
        pgInfoMat->addWidget(texGuide);

        infoTab->addWidget(infoSub, 1);
    }

    // ── Category: Directories ──────────────────────────────────────────
    auto* dirs = new QGroupBox(QStringLiteral("Directories"), this);
    auto* grid = new QGridLayout(dirs);
    grid->setColumnStretch(1, 1);

    // Reusable row: label | path | check | Browse | Download. Returns the
    // Download button so callers can keep a handle (e.g. to disable it).
    auto addRow = [&](int r, const QString& name, QLineEdit*& edit, QLabel*& chk,
                      std::function<void()> onBrowse,
                      std::function<void()> onDownload,
                      bool downloadEnabled) -> QPushButton* {
        grid->addWidget(new QLabel(name, dirs), r, 0);
        edit = new QLineEdit(dirs);
        grid->addWidget(edit, r, 1);
        chk = new QLabel(dirs);
        chk->setFixedWidth(20);
        chk->setAlignment(Qt::AlignCenter);
        grid->addWidget(chk, r, 2);
        auto* browse = new QToolButton(dirs);
        browse->setText(QStringLiteral("…"));
        browse->setToolTip(QStringLiteral("Browse"));
        // Qualify connect: inside a lambda the unqualified name isn't found.
        QObject::connect(browse, &QToolButton::clicked, this, [onBrowse] { onBrowse(); });
        grid->addWidget(browse, r, 3);
        auto* dl = new QPushButton(QStringLiteral("Download"), dirs);
        dl->setEnabled(downloadEnabled);
        if (onDownload)
            QObject::connect(dl, &QPushButton::clicked, this, [onDownload] { onDownload(); });
        grid->addWidget(dl, r, 4);
        QObject::connect(edit, &QLineEdit::textChanged, this,
                         [this] { updateChecks(); updateVersion(); });
        return dl;
    };

    addRow(0, QStringLiteral("Diablo IV folder"), m_game, m_gameChk,
           [this] {
               const QString d = QFileDialog::getExistingDirectory(
                   this, QStringLiteral("Select your Diablo IV folder"), m_game->text());
               if (!d.isEmpty()) m_game->setText(d);
           },
           nullptr, /*downloadEnabled=*/false);   // can't download your game install

    m_tactDl = addRow(1, QStringLiteral("TACT keys folder"), m_tact, m_tactChk,
           [this] {
               // Pick the FOLDER that holds your key file(s) — every *.txt/*.csv inside
               // is loaded, so you can add / update / remove keys without re-pointing here.
               // Start browsing from the current folder (or the file's parent).
               const QFileInfo cur(m_tact->text().trimmed());
               const QString start = cur.isDir() ? cur.absoluteFilePath()
                                   : cur.isFile() ? cur.absolutePath() : m_tact->text();
               const QString d = QFileDialog::getExistingDirectory(
                   this, QStringLiteral("Select the folder containing your TACT keys (*.txt / *.csv)"),
                   start);
               if (!d.isEmpty()) m_tact->setText(d);
           },
           [this] { downloadTactKeys(); }, /*downloadEnabled=*/true);

    m_d4dataDl = addRow(2, QStringLiteral("d4data folder"), m_d4data, m_d4dataChk,
           [this] {
               const QString d = QFileDialog::getExistingDirectory(
                   this, QStringLiteral("Select d4data folder"), m_d4data->text());
               if (!d.isEmpty()) m_d4data->setText(d);
           },
           [this] { downloadD4Data(); }, /*downloadEnabled=*/true);

    generalTab->addWidget(dirs);

    // ── Category: Game ─────────────────────────────────────────────────
    auto* game = new QGroupBox(QStringLiteral("Game data"), this);
    auto* gl = new QGridLayout(game);
    gl->addWidget(new QLabel(QStringLiteral("Game build (CASC product)"), game), 0, 0);
    m_product = new QComboBox(game);
    m_product->setEditable(true);
    m_product->addItems({QStringLiteral("fenris"), QStringLiteral("fenrisb"),
                         QStringLiteral("fenrisdev")});
    m_product->setToolTip(QStringLiteral(
        "Which product to open when the game folder's .build.info lists more than one active\n"
        "build — retail and PTR use the same file format.\n\n"
        "fenris = retail.  A PTR install is normally a SEPARATE folder with its own .build.info,\n"
        "so the usual way to browse it is to point \"Diablo IV game folder\" above at that folder;\n"
        "this setting only matters when one folder carries several active rows.\n\n"
        "If no row matches, the first active one is opened instead — the log line\n"
        "\"CASC: .build.info product=… version=…\" says which one actually won."));
    gl->addWidget(m_product, 0, 1);
    gl->setColumnStretch(1, 1);
    generalTab->addWidget(game);

    // ── Category: Updates ──────────────────────────────────────────────
    // Update check (notify-only — nothing is downloaded). Tells you an update EXISTS before you
    // commit to a Download, and can run at startup.
    // Its own box rather than a fifth row inside the Directories grid: checking for a new d4data
    // commit is not a path, and sitting in that grid it read as an attribute of the folder above it.
    {
        auto* upd = new QGroupBox(QStringLiteral("Updates"), this);
        auto* updRow = new QHBoxLayout(upd);
        m_updCheckBtn = new QPushButton(QStringLiteral("Check for updates"), upd);
        m_updCheckBtn->setToolTip(QStringLiteral(
            "Ask whether newer d4data / TACT keys exist — nothing is downloaded.\n"
            "d4data: compares your checkout's commit against the remote repo (git ls-remote).\n"
            "TACT keys: a conditional HEAD request against the keys file."));
        m_updAuto = new QCheckBox(QStringLiteral("Check at startup"), upd);
        m_updAuto->setChecked(QSettings().value(QStringLiteral("updates/checkAtStartup"), false).toBool());
        m_updAuto->setToolTip(QStringLiteral(
            "Run this check each time the tool opens and notify you only if an update is available."));
        m_updStatus = new QLabel(upd);
        m_updStatus->setStyleSheet(QStringLiteral("color:#888;"));
        m_updStatus->setWordWrap(true);
        // Show the previous result (with its age) so the line isn't blank until you press the button.
        {
            const QString prev = QSettings().value(QStringLiteral("updates/lastResult")).toString();
            if (!prev.isEmpty()) m_updStatus->setText(prev + lastCheckedSuffix());
        }
        updRow->addWidget(m_updCheckBtn);
        updRow->addWidget(m_updAuto);
        updRow->addWidget(m_updStatus, 1);
        QObject::connect(m_updCheckBtn, &QPushButton::clicked, this, [this] { runUpdateCheck(); });
        QObject::connect(m_updAuto, &QCheckBox::toggled, this,
                         [](bool on) { QSettings().setValue(QStringLiteral("updates/checkAtStartup"), on); });
        generalTab->addWidget(upd);
    }

    // ── Category: View ─────────────────────────────────────────────────
    auto* view = new QGroupBox(QStringLiteral("Startup && layout"), this);
    auto* vl = new QVBoxLayout(view);
    m_rememberTab = new QCheckBox(QStringLiteral("Remember the last open tab"), view);
    m_rememberTab->setToolTip(QStringLiteral(
        "Reopen the tab you were last on when the app starts."));
    vl->addWidget(m_rememberTab);
    {
        auto* ia = new QCheckBox(QStringLiteral("Build every index on startup"), view);
        ia->setChecked(QSettings().value(QStringLiteral("ui/indexAllOnStartup"), false).toBool());
        ia->setToolTip(QStringLiteral(
            "Run File ▸ Index ▸ Index all automatically, a few seconds after launch: appearance metadata,\n"
            "icons, asset links, hover info, back trophies, wardrobe animations, shop products,\n"
            "clip labels, and every tab's own scans.\n\n"
            "Worth it right after a game patch, or if you routinely run audits and bulk exports —\n"
            "a half-built index quietly gives a worse result (the icon audit abstains, exports skip\n"
            "clips, the Catalogue lists fewer bundles).\n\n"
            "The cost is a busy first minute: several minutes of background work on a cold cache,\n"
            "and the window may pause as each tab is built.\n\n"
            "Supersedes \"Pre-load tabs\" below — Index all already builds every tab, so enabling\n"
            "this disables that rather than doing the work twice."));
        QObject::connect(ia, &QCheckBox::toggled, this, [](bool on) {
            QSettings().setValue(QStringLiteral("ui/indexAllOnStartup"), on); });
        vl->addWidget(ia);
    }
    {
        auto* pw = new QCheckBox(QStringLiteral("Pre-load tabs a few seconds after startup"), view);
        pw->setChecked(QSettings().value(QStringLiteral("ui/prewarmTabs"), true).toBool());
        pw->setToolTip(QStringLiteral(
            "Quietly build the tabs you have not opened yet, a few seconds after launch, so the\n"
            "first click on Wardrobe or Stable is instant instead of pausing to assemble a\n"
            "character.\n\n"
            "The cost is that the work happens whether you wanted it or not: building those tabs\n"
            "creates a 3D viewport and assembles a full model, so the window can pause for around\n"
            "a second — and briefly flash — a few seconds after startup.\n\n"
            "Turn this OFF if that pause bothers you. The tabs then build on first click instead,\n"
            "which is the same work at a moment you chose."));
        QObject::connect(pw, &QCheckBox::toggled, this, [](bool on) {
            QSettings().setValue(QStringLiteral("ui/prewarmTabs"), on); });
        vl->addWidget(pw);
    }
    m_rememberPanels = new QCheckBox(QStringLiteral("Remember panel sizes (column widths)"), view);
    m_rememberPanels->setToolTip(QStringLiteral(
        "Keep each tab's splitter/column widths across launches. Off = every tab opens at its "
        "default layout and drags aren't saved."));
    m_rememberPanels->setChecked(QSettings().value(QStringLiteral("view/rememberPanels"), true).toBool());
    QObject::connect(m_rememberPanels, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("view/rememberPanels"), on);   // read live by PanelPersist
    });
    vl->addWidget(m_rememberPanels);
    // (The Files "only decrypted" filter is now a per-tab toggle in the Files tab itself; the
    //  developer-mode + locale-packs controls moved to their own "Dev" category at the bottom.)
    interfaceTab->addWidget(view);

    // ── Category: On-hover previews & info ─────────────────────────────
    // One place to tune every tab's hover popups: dwell delay, the image preview itself,
    // scroll-to-zoom, initial size, and per-tab lines of information. All persist live.
    {
        auto* hov = new QGroupBox(QStringLiteral("On-hover previews && info"), this);
        auto* hl = new QVBoxLayout(hov);
        auto liveChk = [this, hov](const QString& key, const QString& label, bool def,
                                   const QString& tip = QString()) {
            auto* cb = new QCheckBox(label, hov);
            if (!tip.isEmpty()) cb->setToolTip(tip);
            cb->setChecked(QSettings().value(key, def).toBool());
            QObject::connect(cb, &QCheckBox::toggled, this, [this, key](bool on) {
                QSettings().setValue(key, on);
                emit settingsChanged();
            });
            return cb;
        };
        {   // Delay (typed, 0–5 s) + initial preview size on one row.
            auto* r = new QHBoxLayout();
            r->addWidget(new QLabel(QStringLiteral("Popup delay:"), hov));
            auto* delay = new QDoubleSpinBox(hov);
            delay->setRange(0.0, 5.0);
            delay->setDecimals(2);
            delay->setSingleStep(0.1);
            delay->setSuffix(QStringLiteral(" s"));
            delay->setKeyboardTracking(true);   // typed values apply as you type
            delay->setValue(QSettings().value(QStringLiteral("hover/delaySec"), 0.5).toDouble());
            QObject::connect(delay, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                             [this](double v) { QSettings().setValue(QStringLiteral("hover/delaySec"), v);
                                                emit settingsChanged(); });
            r->addWidget(delay);
            r->addSpacing(16);
            r->addWidget(new QLabel(QStringLiteral("Preview size:"), hov));
            auto* px = new QSpinBox(hov);
            px->setRange(64, 1024);
            px->setSingleStep(32);
            px->setSuffix(QStringLiteral(" px"));
            px->setValue(QSettings().value(QStringLiteral("hover/previewPx"), 256).toInt());
            QObject::connect(px, QOverload<int>::of(&QSpinBox::valueChanged), this,
                             [this](int v) { QSettings().setValue(QStringLiteral("hover/previewPx"), v);
                                             emit settingsChanged(); });
            r->addWidget(px);
            r->addStretch(1);
            hl->addLayout(r);
        }
        {   // Global preview toggles — one compact row.
            auto* r = new QHBoxLayout();
            r->addWidget(liveChk(QStringLiteral("hover/imagePreview"),
                                 QStringLiteral("Image preview on hover"), true));
            r->addWidget(liveChk(QStringLiteral("hover/scrollZoom"),
                                 QStringLiteral("Scroll to zoom the preview"), true));
            r->addWidget(liveChk(QStringLiteral("hover/colour"),
                                 QStringLiteral("Colour-code hover info"), true,
                                 QStringLiteral("Name stays white and the filename grey; series, "
                                                "flavour text, rarity (in D4's item-quality colours) "
                                                "and season info get their own tints.")));
            r->addStretch(1);
            hl->addLayout(r);
        }
        // Per-tab information lines — a two-column grid per tab, Textures-funnel style.
        auto section = [&](const QString& title,
                           const QVector<QPair<const char*, const char*>>& items) {
            auto* t = new QLabel(title, hov);
            t->setStyleSheet(QStringLiteral("color:#dedede;font-weight:bold;"));
            hl->addWidget(t);
            auto* g = new QGridLayout();
            g->setContentsMargins(0, 0, 0, 4);
            g->setHorizontalSpacing(14);
            g->setVerticalSpacing(2);
            int i = 0;
            for (const auto& it : items) {
                g->addWidget(liveChk(QStringLiteral("hover/") + QLatin1String(it.first),
                                     QLatin1String(it.second), true), i / 2, i % 2);
                ++i;
            }
            hl->addLayout(g);
        };
        section(QStringLiteral("Textures tab"), {
            {"tex/sno",    "SNO · Filename"},   {"tex/dims",   "Dimensions + format"},
            {"tex/size",   "File size"},        {"tex/frames", "Frame count"},
            {"tex/usedby", "Used-by count"},    {"tex/latest", "\"New this update\" badge"}});
        section(QStringLiteral("Models tab"), {
            {"mdl/sno",    "SNO · Filename"},   {"mdl/name",   "Name (translated)"},
            {"mdl/coll",   "Collection"},       {"mdl/counts", "Part/vert/tri counts (loaded model)"},
            {"mdl/anim",   "Animated / rigged badge"}, {"mdl/tags", "Class / gender / type tags"},
            {"mdl/variants", "Variant count"},  {"mdl/latest", "\"New this update\" badge"},
            {"mdl/rarity", "Rarity"},           {"mdl/introduced", "Introduced in (season/expansion)"}});
        section(QStringLiteral("Wardrobe tab"), {
            {"w2/sno",      "SNO · Filename"},  {"w2/name",     "Name"},
            {"w2/collName", "Collection name"}, {"w2/collDesc", "Collection description"},
            {"w2/itemDesc", "Item description"},{"w2/dyeable",  "Dyeable (equipped slot)"},
            {"w2/rarity",   "Rarity"},          {"w2/introduced", "Introduced in (season/expansion)"},
            {"w2/season",   "Season-item flag"}});
        section(QStringLiteral("Stable tab"), {
            {"st/name",     "Name"},            {"st/desc",     "Description"},
            {"st/collType", "Collection + type"}});
        auto* note = new QLabel(QStringLiteral(
            "Item descriptions, rarity, season and introduced-in come from the game's Item / "
            "StoreProduct / enUS StringList data (indexed in the background, cached per build — "
            "rebuilds automatically after a game patch or d4data update). Lines with no authored "
            "data are simply omitted."), hov);
        note->setStyleSheet(QStringLiteral("color:#888;"));
        note->setWordWrap(true);
        hl->addWidget(note);
        interfaceTab->addWidget(hov);
    }

    // ── Category: Icon indicators ──────────────────────────────────────
    // A small badge painted over an item's original icon showing whether a renderable MODEL
    // exists behind it: green ✓ when present, red ✗ when the icon loads but the model is missing.
    // Each iconated tab enables the two indicators independently (so you can flag ONLY the
    // icon-without-model cases). Off by default. Applied on the next icon repaint (scroll / reopen).
    {
        auto* icons = new QGroupBox(QStringLiteral("Icon indicators"), this);
        auto* il = new QGridLayout(icons);
        il->addWidget(new QLabel(QStringLiteral("Tab")), 0, 0);
        il->addWidget(new QLabel(QStringLiteral("✓ has model")), 0, 1);
        il->addWidget(new QLabel(QStringLiteral("✗ icon, no model")), 0, 2);
        auto badgeChk = [this, icons](const QString& key) {
            auto* cb = new QCheckBox(icons);
            cb->setChecked(QSettings().value(key, false).toBool());
            QObject::connect(cb, &QCheckBox::toggled, this, [this, key](bool on) {
                QSettings().setValue(key, on);
                emit settingsChanged();   // repaint icons in the open tabs immediately
            });
            return cb;
        };
        struct TabRow { const char* label; const char* key; };
        static const TabRow tabRows[] = { {"Models", "models"}, {"Wardrobe", "wardrobe"}, {"Stable", "stable"} };
        int row = 1;
        for (const TabRow& t : tabRows) {
            il->addWidget(new QLabel(QString::fromLatin1(t.label)), row, 0);
            il->addWidget(badgeChk(QStringLiteral("icons/%1/showPresent").arg(QLatin1String(t.key))), row, 1, Qt::AlignCenter);
            il->addWidget(badgeChk(QStringLiteral("icons/%1/showMissing").arg(QLatin1String(t.key))), row, 2, Qt::AlignCenter);
            ++row;
        }
        interfaceTab->addWidget(icons);
    }

    // ── Category: Diagnostics ──────────────────────────────────────────
    // One fixed file, replaced each launch, so a bug report is "send me data\D4AssetBrowser.log"
    // rather than "reproduce it, then use Help -> Export log before you close anything".
    {
        auto* diag = new QGroupBox(QStringLiteral("Diagnostics"), this);
        auto* dl = new QVBoxLayout(diag);
        auto* chk = new QCheckBox(QStringLiteral("Write a log file automatically"), diag);
        chk->setChecked(AppLog::fileLogging());
        chk->setToolTip(QStringLiteral(
            "Mirror everything the tool logs to a file on disk, replaced fresh on every launch so it\n"
            "is always just the current session.\n\n"
            "Takes effect immediately — no restart. Help -> Export log still works either way."));
        dl->addWidget(chk);

        auto* pathLbl = new QLabel(diag);
        pathLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        pathLbl->setWordWrap(true);
        pathLbl->setStyleSheet(QStringLiteral("color:#999;"));
        auto refreshPath = [pathLbl] {
            pathLbl->setText(AppLog::fileLogging()
                ? QStringLiteral("Writing to:  %1").arg(AppLog::filePath())
                : QStringLiteral("Off — nothing is being written to disk."));
        };
        refreshPath();
        dl->addWidget(pathLbl);

        auto* row = new QHBoxLayout();
        auto* openBtn = new QPushButton(QStringLiteral("Open containing folder"), diag);
        auto* copyBtn = new QPushButton(QStringLiteral("Copy path"), diag);
        row->addWidget(openBtn);
        row->addWidget(copyBtn);
        row->addStretch(1);
        dl->addLayout(row);
        // Diagnostics sits with Caches & reset, not among everyday settings: both are things you
        // reach for when something is wrong, and neither changes what the app normally does.
        maintTab->addWidget(diag);

        QObject::connect(chk, &QCheckBox::toggled, this, [refreshPath](bool on) {
            AppLog::setFileLogging(on);
            refreshPath();
            // Written THROUGH the log itself, so the file's first line records why it exists.
            if (on) qInfo("log: file logging enabled from Settings");
            else    qInfo("log: file logging disabled from Settings");
        });
        QObject::connect(openBtn, &QPushButton::clicked, this, [] {
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(QFileInfo(AppLog::filePath()).absolutePath()));
        });
        QObject::connect(copyBtn, &QPushButton::clicked, this, [] {
            QGuiApplication::clipboard()->setText(AppLog::filePath());
        });
    }

    // ── Category: Settings profile ─────────────────────────────────────
    // Export/import EVERY preference + the saved ensembles (thumbnails included) as one JSON, so a
    // setup survives a reinstall or moves between machines. (Moved here from the File menu.) Values
    // are stored typed ({t,v}) because QSettings holds bools/ints/doubles/string-lists AND byte
    // arrays (window geometry) — a naive string dump would corrupt the binary ones.
    {
        auto* prof = new QGroupBox(QStringLiteral("Settings profile"), this);
        auto* pl = new QVBoxLayout(prof);
        auto* note = new QLabel(QStringLiteral(
            "Save every preference (and saved ensembles) to a JSON file, or load one on another machine."), prof);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color:#999;"));
        pl->addWidget(note);
        auto* row = new QHBoxLayout();
        auto* expBtn = new QPushButton(QStringLiteral("Export profile…"), prof);
        auto* impBtn = new QPushButton(QStringLiteral("Import profile…"), prof);
        auto* rstBtn = new QPushButton(QStringLiteral("Reset all to defaults…"), prof);
        row->addWidget(expBtn);
        row->addWidget(impBtn);
        row->addStretch(1);
        row->addWidget(rstBtn);
        pl->addLayout(row);
        generalTab->addWidget(prof);

        // Reset every preference to its default. The configured data PATHS (game / d4data / TACT
        // keys / CASC product) are preserved so the tool still finds its data after the reset.
        QObject::connect(rstBtn, &QPushButton::clicked, this, [this] {
            if (QMessageBox::warning(this, QStringLiteral("Reset settings"),
                    QStringLiteral("Reset ALL preferences to their defaults?\n\nThis clears every option "
                                   "(export settings, panel layouts, filters, icon toggles, saved-look "
                                   "references, window geometry). Your Diablo IV / d4data / TACT-keys "
                                   "paths are kept. This cannot be undone.\n\nRestart the app afterwards."),
                    QMessageBox::Reset | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Reset)
                return;
            QSettings s;
            QHash<QString, QVariant> keep;   // preserve the data paths + CASC product
            for (const QString& k : s.allKeys())
                if (k.startsWith(QLatin1String("paths/")) || k.startsWith(QLatin1String("casc/")))
                    keep.insert(k, s.value(k));
            s.clear();
            for (auto it = keep.constBegin(); it != keep.constEnd(); ++it) s.setValue(it.key(), it.value());
            s.sync();
            // Re-baseline, or Cancel would put everything back. This button and Import below write
            // QSettings directly rather than through a widget, so the open-time snapshot no longer
            // describes anything the user wants kept — and the derived-namespace sweep in reject()
            // would strip every export/ · retarget/ · hotkeys/ key this just cleared or created.
            // Both actions carry their own confirmation, so they are the user's answer already.
            snapshotLiveSettings();
            QMessageBox::information(this, QStringLiteral("Reset settings"),
                QStringLiteral("All preferences reset to defaults. Restart the app for a clean state."));
        });

        QObject::connect(expBtn, &QPushButton::clicked, this, [this] {
            const QString path = QFileDialog::getSaveFileName(this,
                QStringLiteral("Export settings profile"),
                QStringLiteral("D4AssetBrowser_profile.json"), QStringLiteral("JSON (*.json)"));
            if (path.isEmpty()) return;
            QSettings s;
            QJsonObject keys;
            for (const QString& k : s.allKeys()) {
                const QVariant v = s.value(k);
                QJsonObject e;
                switch (v.typeId()) {
                case QMetaType::Bool:       e[QStringLiteral("t")] = QStringLiteral("b"); e[QStringLiteral("v")] = v.toBool(); break;
                case QMetaType::Int:
                case QMetaType::LongLong:   e[QStringLiteral("t")] = QStringLiteral("i"); e[QStringLiteral("v")] = v.toLongLong(); break;
                case QMetaType::Double:     e[QStringLiteral("t")] = QStringLiteral("d"); e[QStringLiteral("v")] = v.toDouble(); break;
                case QMetaType::QStringList: {
                    QJsonArray a; for (const QString& x : v.toStringList()) a.append(x);
                    e[QStringLiteral("t")] = QStringLiteral("sl"); e[QStringLiteral("v")] = a; break;
                }
                case QMetaType::QByteArray: e[QStringLiteral("t")] = QStringLiteral("ba");
                                            e[QStringLiteral("v")] = QString::fromLatin1(v.toByteArray().toBase64()); break;
                default:                    e[QStringLiteral("t")] = QStringLiteral("s"); e[QStringLiteral("v")] = v.toString(); break;
                }
                keys[k] = e;
            }
            QJsonObject thumbs;   // ensemble snapshot PNGs, so the tiles travel too
            const QDir ed(AppPaths::dataDir() + QStringLiteral("/ensembles"));
            for (const QFileInfo& fi : ed.entryInfoList({QStringLiteral("*.png")}, QDir::Files)) {
                QFile f(fi.absoluteFilePath());
                if (f.open(QIODevice::ReadOnly))
                    thumbs[fi.fileName()] = QString::fromLatin1(f.readAll().toBase64());
            }
            QJsonObject root;
            root[QStringLiteral("app")] = QStringLiteral("D4AssetBrowser");
            root[QStringLiteral("version")] = QApplication::applicationVersion();
            root[QStringLiteral("keys")] = keys;
            root[QStringLiteral("ensembleThumbs")] = thumbs;
            QFile out(path);
            if (out.open(QIODevice::WriteOnly)) {
                out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
                QMessageBox::information(this, QStringLiteral("Export settings profile"),
                                         QStringLiteral("Settings profile exported to:\n%1").arg(path));
            } else {
                QMessageBox::warning(this, QStringLiteral("Export settings profile"),
                                     QStringLiteral("Couldn't write:\n%1").arg(path));
            }
        });
        QObject::connect(impBtn, &QPushButton::clicked, this, [this] {
            const QString path = QFileDialog::getOpenFileName(this,
                QStringLiteral("Import settings profile"), QString(), QStringLiteral("JSON (*.json)"));
            if (path.isEmpty()) return;
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) return;
            const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
            if (root.value(QStringLiteral("app")).toString() != QLatin1String("D4AssetBrowser")) {
                QMessageBox::warning(this, QStringLiteral("Import settings profile"),
                                     QStringLiteral("Not a D4AssetBrowser settings profile."));
                return;
            }
            if (QMessageBox::question(this, QStringLiteral("Import settings profile"),
                    QStringLiteral("Apply this profile? Existing preferences with the same keys are "
                                   "overwritten (paths included). A restart is recommended after."))
                != QMessageBox::Yes)
                return;
            QSettings s;
            const QJsonObject keys = root.value(QStringLiteral("keys")).toObject();
            for (auto it = keys.constBegin(); it != keys.constEnd(); ++it) {
                const QJsonObject e = it.value().toObject();
                const QString t = e.value(QStringLiteral("t")).toString();
                const QJsonValue v = e.value(QStringLiteral("v"));
                if (t == QLatin1String("b"))       s.setValue(it.key(), v.toBool());
                else if (t == QLatin1String("i"))  s.setValue(it.key(), qint64(v.toDouble()));
                else if (t == QLatin1String("d"))  s.setValue(it.key(), v.toDouble());
                else if (t == QLatin1String("sl")) {
                    QStringList sl; for (const QJsonValue& x : v.toArray()) sl << x.toString();
                    s.setValue(it.key(), sl);
                } else if (t == QLatin1String("ba"))
                    s.setValue(it.key(), QByteArray::fromBase64(v.toString().toLatin1()));
                else                               s.setValue(it.key(), v.toString());
            }
            s.sync();
            const QJsonObject thumbs = root.value(QStringLiteral("ensembleThumbs")).toObject();
            const QString edir = AppPaths::dataDir() + QStringLiteral("/ensembles");
            QDir().mkpath(edir);
            for (auto it = thumbs.constBegin(); it != thumbs.constEnd(); ++it) {
                QFile tf(edir + QLatin1Char('/') + QFileInfo(it.key()).fileName());
                if (tf.open(QIODevice::WriteOnly))
                    tf.write(QByteArray::fromBase64(it.value().toString().toLatin1()));
            }
            snapshotLiveSettings();   // see the Reset button above — Cancel must not undo an import
            QMessageBox::information(this, QStringLiteral("Import settings profile"),
                QStringLiteral("Profile applied. Restart the app so every tab picks it up cleanly."));
        });
    }

    // ── Category: Models ───────────────────────────────────────────────
    // Folded in from the old "File > Models settings…" mini-dialog (now removed) so all app
    // preferences live in one place. These persist live (no OK needed), like the Wardrobe toggles.
    // Titles say what the box CONTAINS, not which tab it is on — the tab already says that, and
    // "Models tab" inside a tab called Models is a frame with no information in its label.
    auto* models = new QGroupBox(QStringLiteral("Browsing && loading"), this);
    auto* ml = new QVBoxLayout(models);
    auto mdlChk = [this, models, ml](const QString& key, const QString& label, bool def, const QString& tip) {
        auto* cb = new QCheckBox(label, models);
        if (!tip.isEmpty()) cb->setToolTip(tip);
        cb->setChecked(QSettings().value(key, def).toBool());
        QObject::connect(cb, &QCheckBox::toggled, this, [key](bool on) { QSettings().setValue(key, on); });
        ml->addWidget(cb);
        return cb;
    };
    // Two sections, because these answered two different questions in one undivided run: what
    // happens as you move through the LIST, and how the loaded model is DRAWN. The three GPU-risky
    // / fidelity toggles in particular were scattered among the list-behaviour ones.
    ml->addWidget(sectionLabel(models, QStringLiteral("Browsing")));
    // Auto-Load — relocated from the preview header's toolbar button. Applied live by ModelsTab
    // (onSettingsChanged re-reads it), like the other Models toggles here.
    mdlChk(QStringLiteral("models/autoLoad"),
        QStringLiteral("Auto-load the selected model"), true,
        QStringLiteral("Load and render a model as soon as you select it. Off = selecting only "
                       "shows its data; double-click a row to load it on demand (useful when "
                       "browsing or multi-selecting for export)."));
    m_mdlRememberLast = mdlChk(QStringLiteral("models/rememberLast"), QStringLiteral("Remember last selected model"), false, QString());
    m_mdlHover        = mdlChk(QStringLiteral("models/hoverPreview"), QStringLiteral("Show a preview pop-up on hover"), true, QString());
    mdlChk(QStringLiteral("models/rememberPanels"),
        QStringLiteral("Remember the right-hand panel layout"), true,
        QStringLiteral("Restore which panels (Info, Parts, Animations…) are open, their order and "
                       "their heights when the tool re-opens. Off = start with Info + Parts."));
    // (No "remember preview settings" checkbox — the viewport toggles always persist; a switch
    //  that nothing read was just noise.)

    ml->addWidget(sectionLabel(models, QStringLiteral("Preview rendering")));
    mdlChk(QStringLiteral("models/baseColorOnly"),
        QStringLiteral("Base colour only (faster loads)"), false,
        QStringLiteral("Decode ONE texture per material — base colour — instead of up to ten "
                       "(normal, ORM, emissive, detail normal/roughness, translucency, mask, "
                       "dye mask and ramp).\n\n"
                       "Models load substantially faster and use far less memory. The preview "
                       "renders flat-lit: no surface detail, glow or dye tinting.\n\n"
                       "Models tab only — it does not affect the Wardrobe or Stable previews, "
                       "or anything you export."));
    mdlChk(QStringLiteral("models/fillSkin"),
        QStringLiteral("Fill skin materials with the class body's skin textures"), true,
        QStringLiteral("Armor pieces carry a black 'skin' placeholder material that the game fills "
                       "with the character's body skin at runtime — the Wardrobe does the same. "
                       "On: the preview borrows the class/gender body-skin textures (e.g. "
                       "barF_P00_BOD) so skin shows instead of black cutouts. Applies on OK."));
    mdlChk(QStringLiteral("models/clothSim"),
        QStringLiteral("Simulate cloth in the model preview (may crash some GPUs)"), false,
        QStringLiteral("Run the cloth/physics simulation on models in the Models preview so capes/"
                       "cloth swing. This is the heaviest paint-time code and can crash unstable "
                       "drivers on certain models — OFF by default (the mesh still shows, just "
                       "static). Exports are unaffected. The Wardrobe tab has its own cloth toggle."));
    mdlChk(QStringLiteral("models/autoRender3D"),
        QStringLiteral("Auto-render 3D icons for rows in view (may crash some GPUs)"), false,
        QStringLiteral("In the 3D / Original+3D icon modes, automatically render thumbnails for the "
                       "list rows scrolled into view. This drives a burst of offscreen GPU renders — "
                       "which crashes unstable drivers — so it's OFF by default. When off, 3D icons "
                       "still show cached/persisted thumbnails and fill in as you view models "
                       "(right-click ▸ Render icon renders on demand)."));
    modelsTab->addWidget(models);
    // Note: model/texture EXPORT options (including "bake dye") live in Export > Export settings —
    // the single source of truth for everything that affects exported files.

    // ── Category: Wardrobe ─────────────────────────────────────────────
    // Real-time: write each Wardrobe option the instant it changes and notify the
    // open Wardrobe tab, so the panels update without waiting for OK.
    auto live = [this](const QString& key, QCheckBox* cb, bool rebuild) {
        QObject::connect(cb, &QCheckBox::toggled, this, [this, key, cb, rebuild] {
            QSettings().setValue(key, cb->isChecked());
            emit wardrobeLiveChanged(rebuild);
        });
    };

    auto* wardrobe2 = new QGroupBox(QStringLiteral("Outfit && preview"), this);
    auto* w2l = new QVBoxLayout(wardrobe2);
    m_w2NudeBase = new QCheckBox(QStringLiteral("Show nude base body when nothing is equipped"), wardrobe2);
    w2l->addWidget(m_w2NudeBase);
    m_w2AutoAnimate = new QCheckBox(QStringLiteral("Auto Animate — react to weapon changes"), wardrobe2);
    m_w2AutoAnimate->setToolTip(QStringLiteral(
        "Play the animation the game's OWN wardrobe would play whenever the weapon loadout\n"
        "changes: it draws the new weapon, then settles into that loadout's idle on a loop.\n\n"
        "Resolved from the shipped data (ItemType.eWeaponClass → the matching *_ui_wardrobe\n"
        "AnimSet), so unarmed, one weapon, dual wield and each two-hander get their own pair.\n\n"
        "Note the game has no wardrobe \"sheathe\" — a weapon change always draws and settles,\n"
        "it never puts anything away. Armour and cosmetic changes do not trigger it."));
    w2l->addWidget(m_w2AutoAnimate);
    auto* dbg2Lbl = new QLabel(QStringLiteral(
        "<b>Panels</b> — side panels toggle from the icon strip in the tab itself:"), wardrobe2);
    dbg2Lbl->setStyleSheet(QStringLiteral("color:#aaa;margin-top:6px;"));
    w2l->addWidget(dbg2Lbl);
    m_w2SecEnsembles = new QCheckBox(QStringLiteral("Ensembles (saved looks)"), wardrobe2);
    // (Parts / Materials / Material textures / Texture preview are NOT settings any more — those
    // sections are stacking panels toggled from the Wardrobe tab's own icon strip, exactly like
    // the Models tab. The checkbox below governs whether that layout is remembered.)
    m_w2RememberPanels = new QCheckBox(QStringLiteral("Remember the right-hand panel layout"), wardrobe2);
    m_w2RememberPanels->setToolTip(QStringLiteral(
        "Restore which side panels are up, their order and their heights when the tool re-opens.\n"
        "Off: the Wardrobe starts with the panel column collapsed to its icon strip."));
    m_w2ShowLog      = new QCheckBox(QStringLiteral("Debug log (status text + Copy debug button)"), wardrobe2);
    for (QCheckBox* cb : {m_w2SecEnsembles, m_w2RememberPanels, m_w2ShowLog})
        w2l->addWidget(cb);
    live(QStringLiteral("wardrobe2/nudeBase"),         m_w2NudeBase,    /*rebuild=*/true);
    live(QStringLiteral("wardrobe2/autoAnimate"),      m_w2AutoAnimate, false);
    live(QStringLiteral("wardrobe2/viewport/ensembles"), m_w2SecEnsembles, false);
    live(QStringLiteral("wardrobe2/rememberPanels"),   m_w2RememberPanels, false);
    live(QStringLiteral("wardrobe2/dbg/log"),          m_w2ShowLog,     false);
    wardrobeTab->addWidget(wardrobe2);

    // ── Category: Weapons (Wardrobe) ───────────────────────────────────
    // Live-persisted like the group above; each toggle notifies the open Wardrobe tab, which
    // refills the weapon lists / reseats the held weapons as needed.
    {
        auto* weap = new QGroupBox(QStringLiteral("Weapons"), this);
        auto* wl = new QVBoxLayout(weap);
        auto mkWeap = [this, weap, wl, &live](const QString& key, const QString& label, bool def,
                                              const QString& tip) {
            auto* cb = new QCheckBox(label, weap);
            cb->setToolTip(tip);
            cb->setChecked(QSettings().value(key, def).toBool());
            live(key, cb, /*rebuild=*/true);   // reseat/refill via wardrobeLiveChanged
            wl->addWidget(cb);
            return cb;
        };
        mkWeap(QStringLiteral("wardrobe2/weap/classRestrict"),
               QStringLiteral("Class-restricted weapons"), true,
               QStringLiteral("ON: only weapon types the class can use in-game (the authored "
                              "fUsableByClass table); slots the class can't fill grey out.\n"
                              "OFF: every weapon type lists for every class."));
        mkWeap(QStringLiteral("wardrobe2/weap/autoUpright"),
               QStringLiteral("Auto-correct upside-down weapons"), true,
               QStringLiteral("Measure each seated weapon's blade direction and flip it upright when it "
                              "points downward. Catches per-class socket differences automatically; turn "
                              "off to see the raw game-data seating."));
        mkWeap(QStringLiteral("wardrobe2/weap/flipMain"),
               QStringLiteral("Flip main hand (180°)"), false,
               QStringLiteral("Rotate the main-hand weapon a half turn about the grip."));
        mkWeap(QStringLiteral("wardrobe2/weap/flipOff"),
               QStringLiteral("Flip off hand (180°)"), false,
               QStringLiteral("Rotate the off-hand weapon a half turn about the grip."));
        mkWeap(QStringLiteral("wardrobe2/weap/invMain"),
               QStringLiteral("Invert main hand (upside down)"), false,
               QStringLiteral("Turn the main-hand weapon upside down (180° about the blade axis)."));
        mkWeap(QStringLiteral("wardrobe2/weap/invOff"),
               QStringLiteral("Invert off hand (upside down)"), false,
               QStringLiteral("Turn the off-hand weapon upside down (180° about the blade axis)."));
        wardrobeTab->addWidget(weap);
    }

    // ── Category: Performance ──────────────────────────────────────────
    // Toggle how the Wardrobe assembles the model. All are read at the next rebuild (no restart).
    auto* perf = new QGroupBox(QStringLiteral("Performance"), this);
    auto* pfl = new QVBoxLayout(perf);
    auto mkPerf = [this, perf, pfl](const QString& key, const QString& label, bool def, const QString& tip) {
        auto* cb = new QCheckBox(label, perf);
        cb->setToolTip(tip);
        cb->setChecked(QSettings().value(key, def).toBool());
        QObject::connect(cb, &QCheckBox::toggled, this, [key](bool on) { QSettings().setValue(key, on); });
        pfl->addWidget(cb);
        return cb;
    };
    QCheckBox* pfCoalesce = mkPerf(QStringLiteral("wardrobe2/perf/coalesce"),
           QStringLiteral("Merge rapid selections (recommended)"), true,
           QStringLiteral("Clicking quickly through a list rebuilds only the item you settle on, "
                          "instead of one full rebuild per click."));
    QCheckBox* pfAsync = mkPerf(QStringLiteral("wardrobe2/perf/asyncLoad"),
           QStringLiteral("Background loading (recommended)"), true,
           QStringLiteral("Decode textures on a worker thread so the UI stays responsive while loading. "
                          "Keeps the current model on screen until the new one is ready (no untextured flash)."));
    QCheckBox* pfTex = mkPerf(QStringLiteral("wardrobe2/perf/texCache"),
           QStringLiteral("Reuse decoded textures (recommended)"), true,
           QStringLiteral("Keep a bounded, temporary pool of decoded material textures (hard-capped at "
                          "256 MB, auto-evicted) so changing one item reuses the rest instead of "
                          "re-decoding the whole outfit. Colours still update instantly. Turn off to "
                          "decode everything fresh each rebuild and use the least memory."));
    QCheckBox* pfVram = mkPerf(QStringLiteral("wardrobe2/perf/vramPool"),
           QStringLiteral("Reuse uploaded textures on the GPU (experimental)"), false,
           QStringLiteral("Keep unchanged materials' textures resident in video memory so changing one "
                          "item skips re-uploading the rest — saves the GPU transfer on top of the decode. "
                          "Colours still update instantly. Bounded and auto-evicted; turn off to free VRAM."));
    wardrobeTab->addWidget(perf);

    // ── Tab: Export ────────────────────────────────────────────────────
    // Folded in from the old Export menu's "Export settings…" dialog (single source of truth for
    // everything that affects exported files/captures). Options persist live; Cancel reverts the
    // whole dialog (see the open-time snapshot below). settingsChanged() is emitted so the tabs'
    // mirrored checkboxes (Models "Tex"/"Anim") stay in sync.
    {
        // Six group boxes stacked in one column was several screens of scrolling with no ordering
        // principle — the sequence was simply the order they were written in. Grouped into
        // sub-tabs by WHAT IS BEING EXPORTED, which is the question you arrive with:
        //   Models   — the .glb itself
        //   Images   — texture output and screenshot/GIF capture
        //   Tabs     — the two tabs with export rules of their own (Wardrobe, Catalogue)
        //   Naming   — file-name templates, which apply to all of the above
        // (Retarget/modding is NOT here: it already lives on the Experimental tab.)
        auto* exSub = new QTabWidget(this);
        exSub->setDocumentMode(true);
        // Same reasoning as the outer tab bar in the constructor: a half-elided page label is
        // unreadable, and this strip's longest entry just grew by a word. The outer bar's width is
        // folded into showEvent()'s wanted size; this one rides on the page sizeHint, so give it
        // the same no-elide policy rather than letting it be the first thing to clip.
        exSub->tabBar()->setElideMode(Qt::ElideNone);
        exSub->tabBar()->setExpanding(false);
        exSub->setUsesScrollButtons(true);
        auto makeExPage = [exSub](const QString& title) {
            auto* w = new QWidget(exSub);
            auto* v = new QVBoxLayout(w);
            v->setContentsMargins(8, 8, 8, 8);
            v->setSpacing(8);
            exSub->addTab(w, title);
            return v;
        };
        QVBoxLayout* pgModels = makeExPage(QStringLiteral("Models"));
        QVBoxLayout* pgImages = makeExPage(QStringLiteral("Images"));
        QVBoxLayout* pgTabs   = makeExPage(QStringLiteral("Wardrobe, Catalogue && Bulk"));
        QVBoxLayout* pgNaming = makeExPage(QStringLiteral("File names"));
        exportTab->addWidget(exSub);

        // Was "Textures" and held two unrelated things: how the TEXTURES TAB writes image files,
        // and whether MODEL exports embed/bake their maps. The second set has moved to the Models
        // page where the rest of the .glb options are — this box is now only about texture files.
        auto* exBox = new QGroupBox(QStringLiteral("Texture export (Textures tab)"), this);
        auto* fx = new QFormLayout(exBox);
        auto exChk = [this](QFormLayout* form, const QString& key, const QString& label, bool def, const QString& tip) {
            auto* cb = new QCheckBox(label);
            if (!tip.isEmpty()) cb->setToolTip(tip);
            cb->setChecked(QSettings().value(key, def).toBool());
            QObject::connect(cb, &QCheckBox::toggled, this, [key](bool on) {
                QSettings().setValue(key, on); });   // live-persist; tabs re-sync on close
            form->addRow(QString(), cb);
            m_exportResetActions.push_back([cb, def] { cb->setChecked(def); });   // Restore Defaults
            return cb;
        };

        auto* texFmt = new QComboBox(exBox);
        texFmt->addItems({QStringLiteral("PNG"), QStringLiteral("JPEG")});
        texFmt->setCurrentText(QSettings().value(QStringLiteral("tex/format"), QStringLiteral("png"))
                                   .toString().contains(QLatin1String("jp")) ? QStringLiteral("JPEG")
                                                                             : QStringLiteral("PNG"));
        QObject::connect(texFmt, &QComboBox::currentTextChanged, this, [](const QString& t) {
            QSettings().setValue(QStringLiteral("tex/format"), t.toLower()); });
        m_exportResetActions.push_back([texFmt] { texFmt->setCurrentText(QStringLiteral("PNG")); });
        fx->addRow(QStringLiteral("Image format:"), texFmt);

        exChk(fx, QStringLiteral("tex/trim"), QStringLiteral("Trim frames to non-transparent bounds"), false, QString());

        auto* texDir = new QLineEdit(QSettings().value(QStringLiteral("tex/lastDir")).toString(), exBox);
        auto* texBrowse = new QPushButton(QStringLiteral("Browse…"), exBox);
        QObject::connect(texBrowse, &QPushButton::clicked, this, [this, texDir] {
            const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Texture output folder"), texDir->text());
            if (!d.isEmpty()) texDir->setText(d);
        });
        QObject::connect(texDir, &QLineEdit::textChanged, this, [this](const QString& t) {
            if (!t.isEmpty()) QSettings().setValue(QStringLiteral("tex/lastDir"), t); });
        auto* texDirRow = new QHBoxLayout();
        texDirRow->addWidget(texDir, 1); texDirRow->addWidget(texBrowse);
        auto* texDirWrap = new QWidget(exBox); texDirWrap->setLayout(texDirRow);
        fx->addRow(QStringLiteral("Output folder:"), texDirWrap);

        pgImages->addWidget(exBox);

        // ── File-name templates (d4analyzer-style) ──
        // Placeholders: {{FileName}} {{SNO}} and, for texframes, {{FrameIdx}} {{FrameName}}.
        // Applied to every export path (single, batch, bulk) — illegal filename chars sanitized.
        {
            auto* nmBox = new QGroupBox(QStringLiteral("Templates"), this);
            auto* fn = new QFormLayout(nmBox);
            auto tplRow = [&](const QString& label, const char* key, const QString& def,
                              const QString& tip) {
                auto* ed = new QLineEdit(QSettings().value(QLatin1String(key), def).toString(), nmBox);
                ed->setToolTip(tip);
                ed->setPlaceholderText(def);
                const QString k = QLatin1String(key);
                QObject::connect(ed, &QLineEdit::textChanged, this, [k, def](const QString& t) {
                    QSettings().setValue(k, t.trimmed().isEmpty() ? def : t);
                });
                m_exportResetActions.push_back([ed, def] { ed->setText(def); });
                fn->addRow(label, ed);
            };
            const QString phTip = QStringLiteral(
                "Placeholders: {{FileName}} = game name · {{SNO}} = numeric id");
            tplRow(QStringLiteral("Textures:"), "export/nameTexture",
                   QStringLiteral("{{FileName}}"),
                   phTip + QStringLiteral("\ne.g.  {{FileName}} [{{SNO}}]"));
            tplRow(QStringLiteral("TexFrames:"), "export/nameFrame",
                   QStringLiteral("{{FileName}} [{{SNO}}] - {{FrameIdx}} {{FrameName}}"),
                   phTip + QStringLiteral(" · {{FrameIdx}} · {{FrameName}}\nThe default matches "
                                          "d4analyzer AND re-imports via the icon_overrides folder."));
            tplRow(QStringLiteral("Models (.glb):"), "export/nameModel",
                   QStringLiteral("{{FileName}}"),
                   phTip + QStringLiteral("\ne.g.  {{FileName}} [{{SNO}}]"));
            // Outfits sit with the other templates rather than in the Wardrobe box: it is a file
            // NAME setting, and having one of them somewhere else is how you end up with two
            // half-remembered places to look. Its placeholder set is larger because an outfit is
            // up to ten pieces rather than one asset, so the list goes in the tooltip.
            tplRow(QStringLiteral("Outfits (.glb):"), "export/wardrobeNameTemplate",
                   QStringLiteral("{{Class}}_{{Gender}}_outfit"),
                   QStringLiteral(
                       "Wardrobe outfits. Placeholders (case-insensitive):\n"
                       "  {{FileName}}    the equipped torso's asset name — the same meaning as above\n"
                       "  {{Class}}       Barbarian, Necromancer, …\n"
                       "  {{Gender}}      M or F\n"
                       "  {{Collection}}  the torso's collection, else the first slot that has one\n"
                       "  {{Name}}        the torso's in-game display name\n"
                       "  {{Helm}} {{Torso}} {{Gloves}} {{Legs}} {{Boots}}\n"
                       "  {{Main}} {{Off}} {{Trophy}}\n"
                       "  {{Date}}        YYYY-MM-DD\n\n"
                       "Empty slots collapse away instead of leaving gaps or stray underscores.\n"
                       "\"Export both genders\" appends _M / _F after this.\n"
                       "e.g.  {{Collection}}_{{Class}}_{{Gender}}"));
            pgNaming->addWidget(nmBox);
            pgNaming->addStretch(1);
        }

        // ── Model (.glb) ──
        auto* mdlBox = new QGroupBox(QStringLiteral("Model export (.glb)"), this);
        auto* fmd = new QFormLayout(mdlBox);

        // This box was one flat run of eleven controls under a single "<b>Animations to embed</b>"
        // heading, so "Include base body", the texture options and the notification toggle all read
        // as animation settings. Four sections now, in the order the file is assembled: what mesh
        // goes in, what it is painted with, what moves it, and what else lands on disk beside it.

        // ── Geometry ───────────────────────────────────────────────────
        fmd->addRow(sectionLabel(mdlBox, QStringLiteral("Geometry")));

        // Not Wardrobe-specific despite living in the Wardrobe box originally: submesh filtering is
        // a property of a model export wherever it is triggered — Models, Stable, Bulk Extract and
        // every context-menu export route through this setting.
        exChk(fmd, QStringLiteral("export/exportFxSim"),
              QStringLiteral("Include FX / SIM / FORM submeshes"), false,
              QStringLiteral("Export effect, cloth-simulation and transformation-form submeshes even "
                             "when the viewport's FX/SIM/FORM buttons are hiding them.\n\n"
                             "Off (the default) means the export matches what you see."));

        // One-time migration of the old key, so an existing setup keeps its choice. Placed here
        // rather than in a helper because it is a single boolean and this is its only reader.
        {
            QSettings s;
            if (!s.contains(QStringLiteral("export/includeBaseBody"))
                && s.contains(QStringLiteral("retarget/fitReference")))
                s.setValue(QStringLiteral("export/includeBaseBody"),
                           s.value(QStringLiteral("retarget/fitReference")));
        }
        exChk(fmd, QStringLiteral("export/includeBaseBody"),
              QStringLiteral("Include base body"), false,
              QStringLiteral(
                  "Append the class's nude body under exported armor, so you can see how a piece sits "
                  "on the figure.\n\n"
                  "WHAT IT ADDS: the test999 suite for the piece's class and gender "
                  "(barF_test999_TRS / _GLV / _LEG / _BTS …) — the bare-skin body the game uses as the "
                  "undressed look. All four slots are merged into a SINGLE mesh under one "
                  "\"__baseBody\" material, so in Blender you select that one material slot and delete "
                  "it in a single action once you are done.\n\n"
                  "WHAT IT IS FOR: checking clipping and proportions, and as a weight-transfer source "
                  "when retargeting to another game's body. Delete it before your final export.\n\n"
                  "WORKS ON: player armor and other pieces that share the player rig (at least 50 bones "
                  "must match). Weapons, mounts and props are skipped — they have no body to fit to. "
                  "Exporting a test999 piece itself adds nothing.\n\n"
                  "The body arrives UNTEXTURED by design — it is a reference, not part of the asset."));

        exChk(fmd, QStringLiteral("export/includeBaseHead"),
              QStringLiteral("Include base head"), false,
              QStringLiteral(
                  "Append the class's default head under exported armor, for scale and for checking "
                  "how a helm or hood sits.\n\n"
                  "WHAT IT ADDS: the \"_HED\" submesh of the class's P00 head appearance "
                  "(barF_P00 → barF_P00_HED) and NOTHING else. That appearance also carries teeth, "
                  "tongue, eyeballs, eyelashes, eyeshadow, facial hair and a duplicate body — all "
                  "hidden behind the face in game, all in the way on a bare reference. It arrives as "
                  "one \"__baseHead\" material you can select and delete in a single action.\n\n"
                  "Independent of \"Include base body\": tick both for a full figure, or just this one "
                  "when you are fitting headgear.\n\n"
                  "WORKS ON: pieces sharing the player rig (at least 50 bones must match). "
                  "Untextured by design."));

        // ── Textures ───────────────────────────────────────────────────
        // Moved off the Images page: these decide what a MODEL export contains, and the only thing
        // they shared with the texture-file settings was the word "texture". Keeping them here also
        // makes "Needs Include textures above" in the last tooltip literally true.
        //
        // Split into two headings. Everything under "Textures inside the .glb" changes the FILE;
        // everything under "Loose texture files beside it" adds SEPARATE files and leaves the .glb
        // alone. Under one heading they read as five variations of the same thing, and the question
        // people actually arrive with — "which of these gets me PNGs, and how do they differ?" —
        // had to be answered by opening three tooltips and holding them side by side.
        fmd->addRow(sectionLabel(mdlBox, QStringLiteral("Textures inside the .glb")));
        exChk(fmd, QStringLiteral("export/includeTex"), QStringLiteral("Include textures in the model .glb"), true,
              QStringLiteral("Embed the material textures in exported .glb models. Same as the Models tab's \"Tex\" checkbox."));
        exChk(fmd, QStringLiteral("export/bakeDye"), QStringLiteral("Bake applied dye into textures"), false,
              QStringLiteral("Export a dyed outfit with the dye baked into the base-colour texture (matches the "
                             "preview). Off = the undyed base look."));
        exChk(fmd, QStringLiteral("export/bakeDetail"),
              QStringLiteral("Bake surface detail into normal / roughness maps"), false,
              QStringLiteral("Bake the tiled leather/fabric/metal detail maps into the exported normal + roughness "
                             "so the model keeps its surface grain in Blender. Slower export.\n\n"
                             "Applies to every model export: Models, Wardrobe, Stable and Bulk Extract."));
        // ── Folder layout ──────────────────────────────────────────────────────────────────────
        // MIRRORS the Bulk Extract tab's combo on one key (export/folderLayout), which is the rule
        // for a state reachable from two panels. It was a Bulk-only control, but exportModels() has
        // six other callers — multi-select export, the context-menu batch, drag-out, "export all",
        // the both-genders twin — all asking the same question and all answered "flat" by default.
        fmd->addRow(sectionLabel(mdlBox, QStringLiteral("Folder layout")));
        {
            auto* lay = new QComboBox(mdlBox);
            lay->addItem(QStringLiteral("Flat — everything in the folder you chose"), ExportLayout::kFlat());
            lay->addItem(QStringLiteral("Subfolders by class"), ExportLayout::kClass());
            lay->addItem(QStringLiteral("Subfolders by type"),  ExportLayout::kType());
            lay->addItem(QStringLiteral("Subfolders by model"), ExportLayout::kModel());
            { const int i = lay->findData(ExportLayout::mode()); lay->setCurrentIndex(i >= 0 ? i : 0); }
            lay->setToolTip(QStringLiteral(
                "How an export of SEVERAL models arranges them. Whichever you pick, each folder gets\n"
                "the same layout inside it: the models, plus textures\\, deps\\ and buffers\\ for\n"
                "whichever of those you have switched on.\n\n"
                "Subfolders by model is the one to pick when you want each asset self-contained —\n"
                "its .glb, its textures and its raw sources together, ready to move elsewhere.\n\n"
                "Exporting a SINGLE model ignores this: there is nothing to group, and quietly\n"
                "turning one file into a folder is not what you asked for.\n\n"
                "By class and by type read the appearance's tags, so they do nothing for a Textures\n"
                "run — the Bulk Extract tab greys them out in that mode.\n\n"
                "The same setting as the layout box on the Bulk Extract tab; changing either moves\n"
                "the other."));
            QObject::connect(lay, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [lay] {
                QSettings().setValue(QStringLiteral("export/folderLayout"), lay->currentData().toString());
            });
            m_exportResetActions.push_back([lay] { lay->setCurrentIndex(0); });
            fmd->addRow(QStringLiteral("Multi-model exports"), lay);
        }

        fmd->addRow(sectionLabel(mdlBox, QStringLiteral("Loose texture files beside it")));
        exChk(fmd, QStringLiteral("export/looseTextures"),
              QStringLiteral("Write the .glb's own four maps as PNGs"), false,
              QStringLiteral(
                  "Saves a copy of the four maps the .glb already contains as separate PNG files,\n"
                  "in a textures\\ folder beside it:\n\n"
                  "    <model>_<material>_basecolor.png\n"
                  "    <model>_<material>_normal.png\n"
                  "    <model>_<material>_orm.png\n"
                  "    <model>_<material>_emissive.png\n\n"
                  "The .glb itself does not change — it keeps its embedded copies and stays one\n"
                  "self-contained file, so importing it works exactly as before. This is purely for\n"
                  "when you also want the maps as loose files.\n\n"
                  "These are the glTF versions, NOT the game's originals. \"orm\" is one image with\n"
                  "three different maps stuffed into its colour channels — occlusion in red,\n"
                  "roughness in green, metalness in blue — and the normal map's blue channel has\n"
                  "been rebuilt rather than decoded. If you want the raw game maps, use the option\n"
                  "below instead; they can both be on.\n\n"
                  "NEEDS \"Include textures\" above. This copies what the .glb embedded, so with\n"
                  "that off there is nothing to copy and you get an empty result.\n\n"
                  "Only covers models this run actually exported — see Settings ▸ Information for\n"
                  "how the three loose-texture options compare."));
        exChk(fmd, QStringLiteral("export/looseTexturesAll"),
              QStringLiteral("Write every map the materials bind, as the game stores them"), false,
              QStringLiteral(
                  "Everything the model's materials reference, into textures\\material_maps\\ —\n"
                  "not just the four maps a .glb is able to hold.\n\n"
                  "D4 binds up to ten maps per material. glTF has a slot for four of them, so the\n"
                  "other six can only ever leave this tool as files:\n\n"
                  "    · three tiled DETAIL normals + their DETAIL roughness maps\n"
                  "      (the leather / fabric / brushed-metal surface grain)\n"
                  "    · translucency\n"
                  "    · the cutout mask\n"
                  "    · the dye mask and the dye ramp\n\n"
                  "Saved as the GAME stores them — nothing packed together, nothing reconstructed —\n"
                  "and named by shader role.\n\n"
                  "A textures.txt is written beside them listing every map's slot, role, SNO and UV\n"
                  "tiling. Keep it: detail maps repeat 8-20 times across the surface, so a PNG on\n"
                  "its own is not enough to rebuild the material.\n\n"
                  "Works with \"Include textures\" OFF. Unlike the option above, this decodes the\n"
                  "textures itself rather than copying what the .glb embedded.\n\n"
                  "Its own subfolder on purpose: the two sets overlap by role but not by content —\n"
                  "both hold something called a normal map, and only one of them is the raw one."));


        // ── Animations ─────────────────────────────────────────────────
        fmd->addRow(sectionLabel(mdlBox, QStringLiteral("Animations to embed")));
        exChk(fmd, QStringLiteral("export/includeAnim"), QStringLiteral("Include animation"), false,
              QStringLiteral("Embed animation clips in exported .glb models. Same as the Models tab's \"Anim\" checkbox.\n\n"
                             "Master switch for the sources below: with this off, a model export embeds "
                             "none of them.\n\n"
                             "Export ▸ Export animations only ignores it — that command is an "
                             "explicit request for clips."));

        // FIVE INDEPENDENT sources, replacing the old "Animations to embed" combo (previewed-only
        // vs. all) plus a separate pulled checkbox. The combo's "all" conflated a model's OWN
        // clips with the base-rig clips it merely inherits, which made the most useful request
        // — "export this armour piece with no animation, because it has none of its own" —
        // impossible to express. Each box is additive; ticking more can only add clips.
        AnimExportScope::load();   // migrate a pre-2.2.8 config before the boxes read their keys
        exChk(fmd, QStringLiteral("export/animOriginal"),
              QStringLiteral("Original animations (authored for this model)"), true,
              QStringLiteral("Clips whose snoAppearance names this model AND whose own name is in "
                             "its family (barM_base00 → barM_*). The playable set: gameplay, "
                             "emotes, wardrobe and UI poses — 378 clips for barM_base00, the same "
                             "number the Wardrobe tab reports for that body.\n\n"
                             "Cutscene and conversation clips also name this body but are named "
                             "IGC_* / Conv_*; they are the separate option below.\n\n"
                             "A body rig like sorF_base00 has these. A gear piece like sorF_stor191_LEG has "
                             "NONE of its own, so with only this ticked it exports with no animation at all — "
                             "which is usually what you want, because the piece borrows the body's."));
        exChk(fmd, QStringLiteral("export/animSets"),
              QStringLiteral("Cutscene && conversation clips (named outside this model's family)"), false,
              QStringLiteral("Clips authored for THIS body but named outside its own family — the "
                             "in-game cutscene and conversation performances.\n\n"
                             "barM_base00 has 758 clips naming it. They split by name:\n"
                             "    378   barM_*                gameplay, emotes, wardrobe, UI poses\n"
                             "    380   IGC_*, Conv_*, …      cutscene and conversation\n\n"
                             "\"Original\" is the first group, this is the second, and together they "
                             "are the number the ANIMATIONS panel shows on a base rig. Both are "
                             "equally "
                             "authoritative — the game really does play all of them on this body — "
                             "so this is a choice about what you want, not about what is correct.\n\n"
                             "Leave OFF for the playable set; that roughly halves a base-rig export."));
        exChk(fmd, QStringLiteral("export/animPreviewed"),
              QStringLiteral("Previewed animation (the clip playing now)"), true,
              QStringLiteral("The single clip currently playing in the viewport. Applies to the loaded model; "
                             "a batch item that is not the loaded model has no \"previewed\" clip."));
        exChk(fmd, QStringLiteral("export/animPulled"),
              QStringLiteral("Pulled animations (manually pulled)"), false,
              QStringLiteral("Clips you pulled from another model — the gold rows in the ANIMATIONS panel, "
                             "retargeted to this model's skeleton. Loaded model only: a pull exists as list "
                             "state, so there is nothing to pull for other items in a batch."));
        exChk(fmd, QStringLiteral("export/animBase"),
              QStringLiteral("Base animations (matching base-rig clips)"), false,
              QStringLiteral("The base-rig clips a piece INHERITS — sorF_base00's clips for sorF_stor191_LEG, "
                             "resolved through the model's animation family.\n\n"
                             "Deduped against \"Original\": a clip the model already owns is never counted "
                             "twice. Together these two are what the old \"All of the model's animations\" "
                             "meant, now separable.\n\n"
                             "Unconfirmed skeleton-similarity guesses are never exported by any of these."));

        // ── Clip filters ───────────────────────────────────────────────
        // Sources decide WHICH CLIPS BELONG to the model; these decide which of those you want in
        // the file. Kept as a separate section because they cut ACROSS the sources — and because
        // the biggest single win on export size is not a source at all: on barM_base00 eight long
        // clips carry 26% of the animation payload against a 75-frame mean.
        fmd->addRow(sectionLabel(mdlBox, QStringLiteral("Clip filters")));

        auto* maxFr = new QSpinBox(mdlBox);
        maxFr->setRange(0, 5000);
        maxFr->setSingleStep(25);
        maxFr->setSpecialValueText(QStringLiteral("no limit"));   // shown at 0
        maxFr->setSuffix(QStringLiteral(" frames"));
        maxFr->setValue(QSettings().value(QStringLiteral("export/animMaxFrames"), 0).toInt());
        maxFr->setToolTip(QStringLiteral(
            "Skip clips longer than this. 0 = keep every length.\n\n"
            "barM_base00's clips average 75 frames, but a handful run 400-2,661 — cutscene poses, "
            "the character-select loop, long emotes. Those eight clips are about a quarter of the "
            "whole animation payload, and no source toggle can exclude them: they are as "
            "\"original\" as anything else the body owns.\n\n"
            "A cap around 200-300 keeps everything you would realistically animate with."));
        QObject::connect(maxFr, QOverload<int>::of(&QSpinBox::valueChanged), this,
                         [](int v) { QSettings().setValue(QStringLiteral("export/animMaxFrames"), v); });
        m_exportResetActions.push_back([maxFr] { maxFr->setValue(0); });
        fmd->addRow(QStringLiteral("Skip clips longer than:"), maxFr);

        auto* excl = new QLineEdit(QSettings().value(QStringLiteral("export/animExclude")).toString(), mdlBox);
        excl->setPlaceholderText(QStringLiteral("e.g.  _emote_, _ui_, IGC_, Conv_"));
        excl->setToolTip(QStringLiteral(
            "Comma-separated. A clip whose name CONTAINS any of these (case-insensitive) is not "
            "embedded.\n\n"
            "Useful values: \"IGC_, Conv_\" drops cutscene and conversation performances; "
            "\"_emote_\" drops the long emote set; \"_ui_\" drops loading-screen and "
            "character-select poses.\n\n"
            "Leave empty to keep everything."));
        QObject::connect(excl, &QLineEdit::textChanged, this,
                         [](const QString& t) { QSettings().setValue(QStringLiteral("export/animExclude"), t); });
        m_exportResetActions.push_back([excl] { excl->clear(); });
        fmd->addRow(QStringLiteral("Exclude names containing:"), excl);

        {
            auto* fNote = new QLabel(QStringLiteral(
                "<i>Filters apply to Original / Cutscene / Base. A clip you are previewing or have "
                "pulled is an explicit choice and is never filtered out.</i>"), mdlBox);
            fNote->setWordWrap(true);
            fNote->setStyleSheet(QStringLiteral("color:#888;"));
            fmd->addRow(fNote);
        }

        // ── Files written ──────────────────────────────────────────────
        // Not part of the model: extra FILES produced beside it. "Both genders" in particular is a
        // multiplier on everything above — it re-runs the same export for the opposite-gender twin.
        fmd->addRow(sectionLabel(mdlBox, QStringLiteral("Files written")));

        exChk(fmd, QStringLiteral("export/bothGenders"),
              QStringLiteral("Also export the matching opposite-gender item"), false,
              QStringLiteral("Export the male AND female version of whatever you export, as two files "
                             "suffixed _M and _F.\n\n"
                             "Models / Bulk Extract: exporting palF_base01_HLM also writes "
                             "palM_base01_HLM.\n"
                             "Wardrobe: the whole outfit is exported again with every piece swapped "
                             "for its opposite-gender twin.\n\n"
                             "Every other option here still applies to BOTH files — animations, raw "
                             "source files and textures are produced for each. Items with no "
                             "opposite-gender twin in the data are exported once."));

        exChk(fmd, QStringLiteral("export/withDeps"), QStringLiteral("Also export raw source files (.app + .tex)"), false,
              QStringLiteral(
                  "Alongside the exported .glb, write the raw game files it came from — the .app and\n"
                  "every .tex its materials reference — into a \"deps\" subfolder.\n\n"
                  "ONE deps folder per output folder, shared by every model written beside it, so a\n"
                  "texture that ten models reference is stored once rather than ten times. In Bulk\n"
                  "Extract that means it follows your layout choice: Flat gives one deps folder,\n"
                  "Subfolders by Model gives each model its own."));

        // Replaces a note that still quoted "All of the model's animations" — the label of a combo
        // removed when the four scope boxes landed, so it named a control that no longer exists.
        auto* exNote = new QLabel(QStringLiteral(
            "<i>Ticking more animation sources makes larger .glb files and slower exports — every "
            "clip is decoded.</i>"), mdlBox);
        exNote->setWordWrap(true);
        exNote->setStyleSheet(QStringLiteral("color:#888;"));
        fmd->addRow(exNote);

        pgModels->addWidget(mdlBox);
        pgModels->addStretch(1);

        // ── Wardrobe outfit export ────────────────────────────────────────────────────────────
        // Its own box because these apply ONLY to the Wardrobe tab, while everything in "Model
        // export" above applies to every tab — mixing them made it look as though the scope and
        // gender options governed Models and Stable exports too.
        //
        // They stack ON TOP of that box rather than replacing it: "Both genders" runs the SAME
        // export twice, so animations, raw source files and textures all follow whatever is set
        // above without any of those options needing to know this one exists.
        auto* wardBox = new QGroupBox(QStringLiteral("Wardrobe"), this);
        auto* fw = new QFormLayout(wardBox);

        auto* wScope = new QComboBox(wardBox);
        wScope->addItem(QStringLiteral("Everything shown in the preview"), 0);
        wScope->addItem(QStringLiteral("Items only"), 1);
        wScope->addItem(QStringLiteral("Items and untextured character"), 2);
        wScope->setCurrentIndex(qBound(0, QSettings().value(QStringLiteral("export/wardrobeScope"), 0).toInt(), 2));
        wScope->setToolTip(QStringLiteral(
            "What goes into an exported outfit.\n\n"
            "Everything shown in the preview — exactly what the viewport displays.\n\n"
            "Items only — the equipped pieces alone, for fitting onto your own character. The body,\n"
            "    head, hair and eyes are removed, and so are the skin submeshes carried BY the\n"
            "    armour (armor_skin_mat and similar) — that is exposed flesh, not equipment.\n\n"
            "Items and untextured character — every item with its skin submeshes intact, plus the\n"
            "    chosen gender's head and body with no textures at all. No hair, jewelry or other\n"
            "    appearance choices: it is a bare fit reference for checking proportions and\n"
            "    clipping, and a hairstyle would only obscure that."));
        QObject::connect(wScope, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, wScope](int) {
            QSettings().setValue(QStringLiteral("export/wardrobeScope"), wScope->currentData().toInt()); });
        m_exportResetActions.push_back([wScope] { wScope->setCurrentIndex(0); });
        fw->addRow(QStringLiteral("Outfit contents:"), wScope);




        auto* wardNote = new QLabel(QStringLiteral(
            "<i>These apply to the Wardrobe tab only. Everything in \"Model export\" above still "
            "applies as well — including to both files when \"both genders\" is on. "
            "The outfit file name is set under \"File names (templates)\".</i>"), wardBox);
        wardNote->setWordWrap(true);
        wardNote->setStyleSheet(QStringLiteral("color:#888;"));
        fw->addRow(wardNote);

        pgTabs->addWidget(wardBox);

        // ── Catalogue export ────────────────────────────────────────────
        // Catalogue-only options, in the same shape as "Wardrobe export" above: everything in
        // "Textures" and "Model export" still applies, this adds to it.
        //
        // Texframes live HERE, not in the Export menu. They are not a separate export — they are
        // extra files belonging to shop art you are already exporting — so this is a "how should
        // catalogue exports come out" answer, which is what this dialog is for. As menu actions it
        // was four more entries competing with the export you actually meant, and it had to be
        // re-chosen every single time.
        auto* catBox = new QGroupBox(QStringLiteral("Catalogue"), this);
        auto* fc = new QFormLayout(catBox);

        auto* catFrames = new QCheckBox(QStringLiteral("Export all available texframes"), catBox);
        catFrames->setChecked(QSettings().value(QStringLiteral("export/catalogueFrames"), false)
                                  .toBool());
        catFrames->setToolTip(QStringLiteral(
            "Shop art is stored as atlases — one image holding several pictures. Normally the whole\n"
            "sheet is exported as one PNG.\n\n"
            "With this on, every frame is ALSO written out separately, into a \"frames\" subfolder:\n"
            "each card, badge and item icon as its own file.\n\n"
            "Frame rectangles come from the texture's own metadata where the d4data snapshot has it,\n"
            "and from the game's frame table otherwise — which is the usual case for recent bundles,\n"
            "since the snapshot lags the live build by a patch or two.\n\n"
            "Off by default: a bundle can carry five atlases, and most of the time the whole sheet is\n"
            "what you wanted."));
        QObject::connect(catFrames, &QCheckBox::toggled, this, [](bool on) {
            QSettings().setValue(QStringLiteral("export/catalogueFrames"), on); });
        m_exportResetActions.push_back([catFrames] { catFrames->setChecked(false); });
        fc->addRow(catFrames);

        // ── What a bundle export writes ─────────────────────────────────────────────────────────
        // All four default ON, so untouched settings behave exactly as before. They earn their
        // place now that "Export all matching" can point at several hundred bundles at once.
        {
            auto* r = new QHBoxLayout;
            struct Part { const char* key; const char* label; const char* tip; };
            static const Part kParts[] = {
                {"export/catModels",   "Models",
                 "models\\ — every appearance the bundle resolves to, as .glb."},
                {"export/catArt",      "Shop art",
                 "art\\ — the bundle's own shop textures as .png (and art\\frames\\ when texframes "
                 "are on above)."},
                {"export/catIcons",    "Icons",
                 "icons\\ — the per-item inventory icons, decoded from their atlases."},
                {"export/catManifest", "Manifest",
                 "manifest.json — what was in the bundle, what it resolved to, and what it did "
                 "NOT. Forced on while \"Only new\" is enabled, because that is the record it "
                 "reads to decide what to skip."},
            };
            for (const Part& p : kParts) {
                auto* cb = new QCheckBox(QString::fromLatin1(p.label), catBox);
                const QString key = QString::fromLatin1(p.key);
                cb->setChecked(QSettings().value(key, true).toBool());
                cb->setToolTip(QString::fromLatin1(p.tip));
                QObject::connect(cb, &QCheckBox::toggled, this,
                                 [key](bool on) { QSettings().setValue(key, on); });
                m_exportResetActions.push_back([cb] { cb->setChecked(true); });
                r->addWidget(cb);
            }
            r->addStretch(1);
            fc->addRow(QStringLiteral("A bundle export writes:"), r);
        }

        auto* catOnlyNew = new QCheckBox(QStringLiteral("Only new — skip bundles already exported"),
                                         catBox);
        catOnlyNew->setChecked(QSettings().value(QStringLiteral("export/catOnlyNew"), false).toBool());
        catOnlyNew->setToolTip(QStringLiteral(
            "Skip any bundle whose folder already holds a manifest covering the parts selected\n"
            "above. Makes a large export resumable, and after a game patch turns \"Export all\n"
            "matching\" into \"export only what I do not already have\".\n\n"
            "It compares against what the earlier run RECORDED writing, not merely against the\n"
            "folder existing — so an art-only export followed by a full one is NOT skipped, and\n"
            "you cannot end up with a half-written bundle that never gets completed.\n\n"
            "Skipped bundles are counted in the status line rather than passed over in silence."));
        QObject::connect(catOnlyNew, &QCheckBox::toggled, this, [](bool on) {
            QSettings().setValue(QStringLiteral("export/catOnlyNew"), on); });
        m_exportResetActions.push_back([catOnlyNew] { catOnlyNew->setChecked(false); });
        fc->addRow(catOnlyNew);

        auto* catNote = new QLabel(QStringLiteral(
            "<i>The Catalogue tab exports whatever is highlighted in the includes strip or the "
            "contents list — models as .glb, images as .png — and the whole bundle when nothing "
            "is. Everything above applies to those files as usual.</i>"), catBox);
        catNote->setWordWrap(true);
        catNote->setStyleSheet(QStringLiteral("color:#888;"));
        fc->addRow(catNote);

        pgTabs->addWidget(catBox);

        // ── Bulk Extract ──────────────────────────────────────────────────────────────────────
        // The third tab with export rules of its own, so it belongs on this page beside Wardrobe
        // and Catalogue. These three lived as checkboxes on the Bulk tab's option row, which was
        // the same mistake the four before them made ("Textures", "All animations", "Pulled anims",
        // "Raw sources" were consolidated behind that tab's "Export settings…" button for exactly
        // this reason): they are PREFERENCES — set once and forgotten — not per-run choices, and a
        // per-run-looking checkbox that silently persists is the worst of both.
        //
        // Same QSettings keys as before (bulk/coTextures, bulk/buffers, bulk/report) and the same
        // defaults, so an existing profile keeps whatever it had. The Bulk tab's run code reads
        // them from QSettings and is otherwise unchanged.
        auto* bulkBox = new QGroupBox(QStringLiteral("Bulk Extract"), this);
        auto* fb = new QFormLayout(bulkBox);

        auto* bulkCoTex = new QCheckBox(
            QStringLiteral("Write every texture whose name matches an exported model"), bulkBox);
        bulkCoTex->setChecked(QSettings().value(QStringLiteral("bulk/coTextures"), false).toBool());
        bulkCoTex->setToolTip(QStringLiteral(
            "MODELS runs only. Once the .glb files are written, this looks through every texture in\n"
            "the game for ones whose NAME starts with an exported model's name, and decodes those\n"
            "into a textures\\ subfolder under their own game names.\n\n"
            "It ignores materials completely, and that is the point. The two loose-texture options\n"
            "on the Models page ask \"what does this model's material bind?\", which is exact but\n"
            "blind to anything the material does not mention — recolour variants of the same piece,\n"
            "atlas sheets, extras. Matching on the name finds those.\n\n"
            "It is also the only one that survives a re-run set to \"Only new\": those two write maps\n"
            "for models they exported, and on such a run every model is skipped. This one works from\n"
            "what MATCHED, so it still fills the folder.\n\n"
            "Weapon textures carry no class or category marker and are named purely after their\n"
            "model, so this is the only route the data supports — which is why the \"All Weapons\"\n"
            "preset switches it on.\n\n"
            "All three can be on at once; the file names never collide. Settings ▸ Information\n"
            "compares them side by side."));
        QObject::connect(bulkCoTex, &QCheckBox::toggled, this, [](bool on) {
            QSettings().setValue(QStringLiteral("bulk/coTextures"), on); });
        m_exportResetActions.push_back([bulkCoTex] { bulkCoTex->setChecked(false); });
        fb->addRow(bulkCoTex);

        auto* bulkBuffers = new QCheckBox(QStringLiteral("Also write raw game buffers"), bulkBox);
        bulkBuffers->setChecked(QSettings().value(QStringLiteral("bulk/buffers"), false).toBool());
        bulkBuffers->setToolTip(QStringLiteral(
            "Write each extracted item's RAW game buffer into a \"buffers\" subfolder — the\n"
            "BLTE-decoded payload with its native extension (.app for models, .tex for textures),\n"
            "like d4analyzer's buffer export.\n\n"
            "Related to \"Also export raw source files (.app + .tex)\" on the Models page, but not\n"
            "the same:\n"
            "that one writes a deps folder containing each model's .app AND every material's .tex,\n"
            "and it does nothing at all for a TEXTURE run. This writes a buffers folder holding the\n"
            "payloads for whatever the run extracted, models or textures.\n\n"
            "Off by default: it roughly doubles a run's output size."));
        QObject::connect(bulkBuffers, &QCheckBox::toggled, this, [](bool on) {
            QSettings().setValue(QStringLiteral("bulk/buffers"), on); });
        m_exportResetActions.push_back([bulkBuffers] { bulkBuffers->setChecked(false); });
        fb->addRow(bulkBuffers);

        auto* bulkReport = new QCheckBox(QStringLiteral("Write a run report (_bulk_report.csv)"), bulkBox);
        bulkReport->setChecked(QSettings().value(QStringLiteral("bulk/report"), false).toBool());
        bulkReport->setToolTip(QStringLiteral(
            "After each bulk run, write _bulk_report.csv into the output folder: one row per item\n"
            "with name, SNO, status, reason and size.\n\n"
            "The reason column is the only place a per-item failure is written down — the log line\n"
            "says how many failed, this says which and why. Worth leaving on for a long run.\n\n"
            "Off by default: turn it on if you would not just delete the file."));
        QObject::connect(bulkReport, &QCheckBox::toggled, this, [](bool on) {
            QSettings().setValue(QStringLiteral("bulk/report"), on); });
        m_exportResetActions.push_back([bulkReport] { bulkReport->setChecked(false); });
        fb->addRow(bulkReport);

        auto* bulkNote = new QLabel(QStringLiteral(
            "<i>Everything on the Models and File names pages applies to a bulk run as usual &mdash; "
            "including the two other loose-texture options, which the Models page compares with the "
            "one above side by side. The Bulk tab itself carries only per-run choices: what to skip, "
            "how to lay the output out, and how many workers to use.</i>"), bulkBox);
        bulkNote->setWordWrap(true);
        bulkNote->setStyleSheet(QStringLiteral("color:#888;"));
        fb->addRow(bulkNote);

        pgTabs->addWidget(bulkBox);
        pgTabs->addStretch(1);

        // ── Image & GIF capture ─────────────────────────────────────────
        // Screenshot / turntable-GIF output settings (separate from the model .glb export).
        auto* gifBox = new QGroupBox(QStringLiteral("Image && GIF capture"), this);
        auto* gf = new QFormLayout(gifBox);

        // Eleven rows in one undivided form, alternating between the two things this box captures.
        // The four boundaries below were already marked in comments — they are now visible headers,
        // so "Physics steps/frame" no longer looks like a GIF encoder setting.
        // Ahead of the section headers on purpose: this one governs BOTH a still capture and a GIF,
        // so it belongs to the box rather than to either section. Under "GIF quality" it read as a
        // GIF-only option.
        exChk(gf, QStringLiteral("export/transparentBg"),
            QStringLiteral("Transparent background (captures)"), false,
            QStringLiteral("Preview image / GIF captures render the model on a transparent background.\n"
                           "PNG gets true alpha; GIF uses a 1-bit cutout (hard edges). JPEG ignores this."));

        // ── Still image ──────────────────────────────────────────────
        gf->addRow(sectionLabel(gifBox, QStringLiteral("Still image")));
        auto* imgFmt = new QComboBox(gifBox);
        imgFmt->addItem(QStringLiteral("PNG — lossless, keeps alpha"),  QStringLiteral("png"));
        imgFmt->addItem(QStringLiteral("JPEG — smallest, no alpha"),    QStringLiteral("jpg"));
        // Only offered when Qt can actually write it: WebP lives in a separate image-format plugin
        // that is not always deployed, and without this check picking it would just fail at save
        // time with nothing to explain why.
        if (QImageWriter::supportedImageFormats().contains(QByteArrayLiteral("webp")))
            imgFmt->addItem(QStringLiteral("WebP — small, keeps alpha"), QStringLiteral("webp"));
        {
            const int i = imgFmt->findData(QSettings().value(QStringLiteral("export/imageFormat"),
                                                            QStringLiteral("png")).toString());
            imgFmt->setCurrentIndex(i < 0 ? 0 : i);
        }
        imgFmt->setToolTip(QStringLiteral(
            "Container for Save preview image. Sets the save dialog's default — you can still pick\n"
            "another there. JPEG has no alpha channel, so \"Transparent background\" is ignored for it."));
        QObject::connect(imgFmt, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [imgFmt](int) {
            QSettings().setValue(QStringLiteral("export/imageFormat"), imgFmt->currentData().toString()); });
        m_exportResetActions.push_back([imgFmt] { imgFmt->setCurrentIndex(0); });
        gf->addRow(QStringLiteral("Image format:"), imgFmt);

        auto* imgScale = new QComboBox(gifBox);
        for (int p : {25, 50, 75, 100, 200, 300, 400})
            imgScale->addItem(p > 100 ? QStringLiteral("%1%  (%2x supersampled)").arg(p).arg(p / 100)
                                      : QStringLiteral("%1%").arg(p), p);
        {
            const int i = imgScale->findData(QSettings().value(QStringLiteral("export/imageScale"), 100).toInt());
            imgScale->setCurrentIndex(i < 0 ? 3 : i);
        }
        imgScale->setToolTip(QStringLiteral(
            "Resolution of Save preview image, relative to the viewport.\n\n"
            "Above 100% the scene is genuinely RE-RENDERED larger — real detail and much cleaner\n"
            "edges, not an upscale — so it costs one extra render and more VRAM. If the driver\n"
            "refuses the size it steps down automatically.\n"
            "At or below 100% the captured frame is resampled down."));
        QObject::connect(imgScale, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [imgScale](int) {
            QSettings().setValue(QStringLiteral("export/imageScale"), imgScale->currentData().toInt()); });
        m_exportResetActions.push_back([imgScale] { imgScale->setCurrentIndex(3); });
        gf->addRow(QStringLiteral("Image resolution:"), imgScale);

        auto* imgQual = new QSpinBox(gifBox);
        imgQual->setRange(1, 100);
        imgQual->setValue(QSettings().value(QStringLiteral("export/imageQuality"), 92).toInt());
        imgQual->setToolTip(QStringLiteral("Quality for the lossy formats (JPEG, WebP). PNG ignores it."));
        imgQual->setEnabled(imgFmt->currentData().toString() != QLatin1String("png"));
        QObject::connect(imgFmt, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                         [imgFmt, imgQual](int) {
            imgQual->setEnabled(imgFmt->currentData().toString() != QLatin1String("png")); });
        QObject::connect(imgQual, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
            QSettings().setValue(QStringLiteral("export/imageQuality"), v); });
        m_exportResetActions.push_back([imgQual] { imgQual->setValue(92); });
        gf->addRow(QStringLiteral("Image quality:"), imgQual);

        // ── Animated GIF ─────────────────────────────────────────────
        gf->addRow(sectionLabel(gifBox, QStringLiteral("Animated GIF")));
        auto* gifFps = new QSpinBox(gifBox);
        gifFps->setRange(1, 60); gifFps->setSuffix(QStringLiteral(" fps"));
        gifFps->setValue(QSettings().value(QStringLiteral("export/gifFps"), 25).toInt());
        gifFps->setToolTip(QStringLiteral("Turntable-GIF frame rate (animation-loop GIFs use the clip's own rate)."));
        QObject::connect(gifFps, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
            QSettings().setValue(QStringLiteral("export/gifFps"), v); });
        m_exportResetActions.push_back([gifFps] { gifFps->setValue(25); });
        gf->addRow(QStringLiteral("GIF frame rate:"), gifFps);

        auto* gifFrames = new QSpinBox(gifBox);
        gifFrames->setRange(8, 240);
        gifFrames->setValue(QSettings().value(QStringLiteral("export/gifTurntableFrames"), 48).toInt());
        gifFrames->setToolTip(QStringLiteral("Frames per 360° turntable — higher = smoother but larger."));
        QObject::connect(gifFrames, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
            QSettings().setValue(QStringLiteral("export/gifTurntableFrames"), v); });
        m_exportResetActions.push_back([gifFrames] { gifFrames->setValue(48); });
        gf->addRow(QStringLiteral("Turntable frames:"), gifFrames);

        // ── Cloth physics during capture ──
        // Export is not real-time. The live preview settles the cloth with an idle timer whenever a
        // frame takes >60 ms; a capture advances it once per frame and so comes out LESS settled —
        // which reads as twitching in the finished GIF. These two knobs buy that settling back.
        gf->addRow(sectionLabel(gifBox, QStringLiteral("Cloth physics during capture")));
        auto* gifPhys = new QSpinBox(gifBox);
        gifPhys->setRange(1, 8);
        gifPhys->setValue(QSettings().value(QStringLiteral("export/gifPhysicsSteps"), 3).toInt());
        gifPhys->setToolTip(QStringLiteral(
            "Cloth simulation steps per exported frame.\n"
            "1 = fastest export, matches raw frame-stepping (can look twitchy).\n"
            "3-4 = smooth, settled cloth. Higher costs export time, not file size."));
        QObject::connect(gifPhys, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
            QSettings().setValue(QStringLiteral("export/gifPhysicsSteps"), v); });
        m_exportResetActions.push_back([gifPhys] { gifPhys->setValue(3); });
        gf->addRow(QStringLiteral("Physics steps/frame:"), gifPhys);

        auto* gifSub = new QSpinBox(gifBox);
        gifSub->setRange(1, 4);
        gifSub->setValue(QSettings().value(QStringLiteral("export/gifPhysicsSubSteps"), 3).toInt());
        gifSub->setToolTip(QStringLiteral(
            "Solver sub-steps used only while exporting (the preview keeps its own setting).\n"
            "Sub-stepping divides the per-step forces and re-solves — this is what removes\n"
            "high-frequency jitter. Raised, never lowered, relative to the live value."));
        QObject::connect(gifSub, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
            QSettings().setValue(QStringLiteral("export/gifPhysicsSubSteps"), v); });
        m_exportResetActions.push_back([gifSub] { gifSub->setValue(3); });
        gf->addRow(QStringLiteral("Physics sub-steps:"), gifSub);

        // ── Quality / file-size knobs ──
        // Single & here, not &&: sectionLabel wraps the text in <b>…</b>, which makes QLabel treat it as
        // RICH TEXT — the HTML parser handles the ampersand, not the mnemonic parser, so "&&" would
        // render literally. The opposite of the QGroupBox titles above.
        gf->addRow(sectionLabel(gifBox, QStringLiteral("GIF quality & file size")));
        auto* gifDither = new QCheckBox(QStringLiteral("Dither (reduces banding)"), gifBox);
        gifDither->setChecked(QSettings().value(QStringLiteral("export/gifDither"), true).toBool());
        gifDither->setToolTip(QStringLiteral(
            "Ordered (Bayer) dithering when reducing to the GIF palette.\n"
            "The pattern depends only on pixel position, so it is identical in every frame — it\n"
            "removes banding without adding frame-to-frame noise. Turn OFF only if you want flat\n"
            "posterised colour; leaving it off makes band edges crawl across moving cloth."));
        QObject::connect(gifDither, &QCheckBox::toggled, this, [this](bool on) {
            QSettings().setValue(QStringLiteral("export/gifDither"), on); });
        m_exportResetActions.push_back([gifDither] { gifDither->setChecked(true); });
        gf->addRow(QString(), gifDither);

        auto* gifScale = new QSpinBox(gifBox);
        gifScale->setRange(25, 100); gifScale->setSuffix(QStringLiteral(" %"));
        gifScale->setSingleStep(5);
        gifScale->setValue(qBound(25, QSettings().value(QStringLiteral("export/gifScale"), 100).toInt(), 100));
        gifScale->setToolTip(QStringLiteral("Scale the GIF down from the viewport size. The biggest file-size "
                                            "lever — size is roughly quadratic in dimension (e.g. 50%% ≈ ¼ the bytes)."));
        QObject::connect(gifScale, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
            QSettings().setValue(QStringLiteral("export/gifScale"), v); });
        m_exportResetActions.push_back([gifScale] { gifScale->setValue(100); });
        gf->addRow(QStringLiteral("GIF scale:"), gifScale);

        auto* gifColors = new QSpinBox(gifBox);
        gifColors->setRange(16, 256); gifColors->setSingleStep(16);
        gifColors->setValue(qBound(16, QSettings().value(QStringLiteral("export/gifMaxColors"), 256).toInt(), 256));
        gifColors->setToolTip(QStringLiteral("Maximum palette colors. Fewer colors = smaller files (and a coarser "
                                             "palette). 256 = full quality."));
        QObject::connect(gifColors, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
            QSettings().setValue(QStringLiteral("export/gifMaxColors"), v); });
        m_exportResetActions.push_back([gifColors] { gifColors->setValue(256); });
        gf->addRow(QStringLiteral("GIF colors:"), gifColors);

        auto* gifOpt = exChk(gf, QStringLiteral("export/gifOptimize"),
            QStringLiteral("Optimize to target size"), false,
            QStringLiteral("After encoding, if the GIF is over the target size the exporter automatically\n"
                           "trims palette colors then downscales and re-encodes (frames are captured once,\n"
                           "so only re-encoding happens) until it fits. Overrides Scale/Colors when needed."));

        auto* gifTarget = new QSpinBox(gifBox);
        gifTarget->setRange(1, 200); gifTarget->setSuffix(QStringLiteral(" MB"));
        gifTarget->setValue(qBound(1, QSettings().value(QStringLiteral("export/gifTargetMB"), 10).toInt(), 200));
        gifTarget->setToolTip(QStringLiteral("Maximum GIF file size when \"Optimize to target size\" is on."));
        gifTarget->setEnabled(gifOpt->isChecked());
        QObject::connect(gifOpt, &QCheckBox::toggled, gifTarget, &QWidget::setEnabled);
        QObject::connect(gifTarget, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
            QSettings().setValue(QStringLiteral("export/gifTargetMB"), v); });
        m_exportResetActions.push_back([gifTarget] { gifTarget->setValue(10); });
        gf->addRow(QStringLiteral("Target size:"), gifTarget);

        exChk(gf, QStringLiteral("export/gifCropToModel"),
            QStringLiteral("Crop to model  (images and GIFs)"), false,
            QStringLiteral("Trim the empty margin around the model, so the capture is the subject\n"
                           "rather than the viewport. Applies to Save preview image and to both\n"
                           "GIF exports.\n\n"
                           "A GIF uses ONE box for the whole animation — the union of every frame's\n"
                           "silhouette — so the model doesn't swim around as the crop follows it.\n\n"
                           "Costs no extra rendering: an opaque capture renders with the backdrop\n"
                           "drawn but alpha left as model coverage, which is what locates the\n"
                           "subject, and the alpha is stripped again once the box is known. Fewer\n"
                           "pixels also means a smaller file at the same quality."));

        // One transparency path — native alpha, a single render. The "Transparency method" choice
        // that used to sit here offered difference matting as the alternative, which cost a second
        // render of every frame to recover information the framebuffer's own alpha already carried.
        pgImages->addWidget(gifBox);
        pgImages->addStretch(1);

        // ── Applies to every export ────────────────────────────────────
        // Added to exportTab, NOT to one of the sub-pages: this fires for model exports, texture
        // exports, wardrobe outfits, catalogue bundles and bulk extracts alike. It sat on the
        // Models page, which quietly implied it was a .glb option. Below the sub-tab strip it stays
        // visible whichever page you are on — the honest position for a setting that ignores the
        // distinction the sub-tabs are making.
        {
            auto* allBox = new QGroupBox(QStringLiteral("All exports"), this);
            auto* fa = new QFormLayout(allBox);
            exChk(fa, QStringLiteral("export/osNotify"),
                  QStringLiteral("Desktop notification when an export finishes"), true,
                  QStringLiteral("Raise a system notification (Windows Action Center, or your desktop's "
                                 "equivalent) when an export or bulk extract completes.\n\n"
                                 "Adds a tray icon while enabled, because that is what the notification "
                                 "is delivered through."));
            exportTab->addWidget(allBox);
        }

        // ── Retarget & modding ──────────────────────────────────────────
        // Options for porting extracted models onto OTHER games' rigs (weight transfer,
        // re-skinning, socket alignment). All of these only affect .glb exports.
        //
        // Lives on the Experimental tab (added at the end of the tab list) rather than under
        // Export. They are sharp tools — reducing a rig to 26 bones or mirroring for X-Axis
        // Mirror will quietly change geometry — and they were sitting one collapsed dropdown
        // away from the everyday export settings.
        auto* rtBox = new QGroupBox(QStringLiteral("Modding / retarget (.glb export)"), this);
        auto* rt = new QFormLayout(rtBox);

        auto* preset = new QComboBox(rtBox);
        preset->addItems({QStringLiteral("Custom (use the toggles above/below as-is)"),
                          QStringLiteral("Blender (meters, OpenGL normals)"),
                          QStringLiteral("Unreal / Skyrim (centimeters, DirectX normals)"),
                          QStringLiteral("Unity (meters, OpenGL normals)")});
        preset->setCurrentIndex(qBound(0, QSettings().value(QStringLiteral("retarget/enginePreset"), 0).toInt(), 3));
        preset->setToolTip(QStringLiteral(
            "Target-engine preset. Overrides orientation, unit scale and normal-map convention at export:\n"
            "• Blender — Blender-friendly rig on, meters, normal maps untouched.\n"
            "• Unreal / Skyrim — Blender-friendly rig on, ×100 unit scale (cm pipelines), normal-map\n"
            "  green channel flipped (DirectX convention). Note: engines whose glTF importer already\n"
            "  converts meters→cm will double-scale; this preset targets Blender→FBX round-trips.\n"
            "• Unity — plain glTF (Y-up, meters).\n"
            "\"Custom\" uses the individual toggles + the unit scale below."));
        QObject::connect(preset, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
            QSettings().setValue(QStringLiteral("retarget/enginePreset"), i); });
        m_exportResetActions.push_back([preset] { preset->setCurrentIndex(0); });
        rt->addRow(QStringLiteral("Target engine:"), preset);

        auto* unitScale = new QDoubleSpinBox(rtBox);
        unitScale->setRange(0.001, 1000.0);
        unitScale->setDecimals(3);
        unitScale->setValue(QSettings().value(QStringLiteral("retarget/unitScale"), 1.0).toDouble());
        unitScale->setToolTip(QStringLiteral("Multiply all positions/bone translations on export (Custom preset only). "
                                             "D4 units are meters; use 100 for centimeter pipelines."));
        unitScale->setEnabled(preset->currentIndex() == 0);
        QObject::connect(preset, QOverload<int>::of(&QComboBox::currentIndexChanged), unitScale,
                         [unitScale](int i) { unitScale->setEnabled(i == 0); });
        QObject::connect(unitScale, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
            QSettings().setValue(QStringLiteral("retarget/unitScale"), v); });
        m_exportResetActions.push_back([unitScale] { unitScale->setValue(1.0); });
        rt->addRow(QStringLiteral("Unit scale (Custom preset only):"), unitScale);

        exChk(rt, QStringLiteral("export/reconstructNormalZ"),
              QStringLiteral("Rebuild normal-map blue channel (Blender)"), true,
              QStringLiteral("Rebuild the normal map's blue channel (Z=√(1−x²−y²)) so Blender lights it correctly."));

        // Bone naming / orientation (retarget/modding concerns). "Blender-friendly rig" overrides
        // "Rename bones to readable names" on export.
        exChk(rt, QStringLiteral("export/boneNamesTranslated"),
              QStringLiteral("Rename bones to readable names"), false,
              QStringLiteral("Rename exported bones from bone_<hash> to readable labels (head, leftHand, "
                             "pelvis, IK chains…) where the bone is identified, using the verified D4 "
                             "hardpoint/IK data. Unidentified bones keep bone_<hash>."));
        exChk(rt, QStringLiteral("export/hardpointEmpties"),
              QStringLiteral("Export attachment points (hardpoints) as empties"), false,
              QStringLiteral("Attach the model's rig hardpoints (weapon grips, sheaths, trail emitters, "
                             "look-at sockets…) as named empties parented under their bone, using the "
                             "exact game transforms. Handy for snapping props/weapons to sockets in "
                             "Blender or other games. Names come from the verified HP_* table; unknown "
                             "ones export as HP_<hash>. Needs a skinned model."));
        exChk(rt, QStringLiteral("export/blenderFriendly"),
              QStringLiteral("Blender-friendly rig (paired .L/.R bone names)"), false,
              QStringLiteral("Export for Blender's symmetry tools: the model is rotated to Blender's "
                             "character convention (facing −Y, character's left on +X) and bones get "
                             "paired \".L\"/\".R\" names — curated names on the player rig (hand.L, "
                             "shin.R…), geometry-detected mirror pairs (m123.L/m123.R) everywhere else, "
                             "including cloth/hair chains and monsters. Enables X-Mirror, Symmetrize and "
                             "mirrored weight painting. Overrides \"Translated bone names\" on export."));
        exChk(rt, QStringLiteral("export/xMirror"),
              QStringLiteral("Symmetrize rig for Blender's X-Axis Mirror"), true,
              QStringLiteral("With the Blender-friendly rig: rewrite each .R bone's rest orientation to "
                             "the exact mirror of its .L partner so Blender's Pose ▸ X-Axis Mirror and "
                             "paste-flipped posing work correctly (D4 rigs author left/right bones ~180° "
                             "apart). Skinning is preserved exactly and embedded animations are "
                             "re-expressed in the new bone frames (verified in Blender — see "
                             "D4_XMirror_Spec.md). No effect unless Blender-friendly rig is on."));

        exChk(rt, QStringLiteral("retarget/remapWeights"),
              QStringLiteral("Reduce rig to standard humanoid bones"), false,
              QStringLiteral("Merge skin weights up into the 26 identified player-rig anchor bones (pelvis, "
                             "chest, head, upperArm/forearm/hand, thigh/shin/foot .L/.R…) and export ONLY that "
                             "reduced skeleton. Twist/roll/cloth/helper weights fold into the nearest anchor "
                             "ancestor, so vertex groups line up ~1:1 with typical game rigs before weight "
                             "transfer. Player-rig models only (skipped for monsters/props)."));
        exChk(rt, QStringLiteral("retarget/collapseCloth"),
              QStringLiteral("Remove cloth / physics bone chains"), false,
              QStringLiteral("Remove the game's simulated cloth/physics bones from the export and fold their "
                             "weights into the nearest kept ancestor. Target games run their own physics; this "
                             "keeps strand bones out of your vertex groups."));
        // "Include base body" moved to Model export (.glb) — it changes what is IN the file, like
        // every other option there, rather than how the rig is retargeted.
        exChk(rt, QStringLiteral("retarget/setManifest"),
              QStringLiteral("Batch-export the whole armor set (+ manifest.json)"), false,
              QStringLiteral("Batch exports expand the selection to the whole armor set (matching _HLM/_TRS/"
                             "_GLV/_LEG/_BTS siblings by name) and write a manifest.json (file, SNO, name, "
                             "slot) so re-exports after game patches are reproducible."));

        auto* rtNote = new QLabel(QStringLiteral(
            "<i>Typical flow: preset = your target engine, remap + strip cloth on, export, then transfer "
            "weights in Blender from the target game's body. See docs/MODEL_EXPORT.md §5–6.</i>"), rtBox);
        rtNote->setWordWrap(true);
        rtNote->setStyleSheet(QStringLiteral("color:#888;"));
        rt->addRow(rtNote);

        // No "Advanced" collapse any more. It existed to keep these out of the everyday Export
        // view; a dedicated tab does that better, and a collapsed header would have hidden the
        // tab's only content behind a second click.
        //
        // (Moving the box without this left the toggle stranded on the Export tab, driving
        // setVisible on a widget that had gone to another page — the header was still there and
        // did nothing.)
        experimentalTab->addWidget(rtBox);
        experimentalTab->addStretch(1);   // keep the box at the top rather than centred on the page
    }

    // ── Category: Cache ────────────────────────────────────────────────
    // The app caches the metadata crawl and the icon-atlas index to AppData so
    // startup is instant; these buttons clear them to force a rebuild (e.g. after
    // a d4data update or to pick up icon-mapping fixes).
    // "&&" renders a literal "&": a single & in a QGroupBox title is a mnemonic, so
    // "Caches & reset" draws as "Caches reset" with an underlined r.
    auto* cache = new QGroupBox(QStringLiteral("Caches && reset"), this);
    auto* cl = new QVBoxLayout(cache);
    auto* cacheHdr = new QLabel(QStringLiteral("<b>Rebuild caches</b> — clears the on-disk index; rebuilt on next launch:"), cache);
    cacheHdr->setStyleSheet(QStringLiteral("color:#aaa;"));
    cl->addWidget(cacheHdr);
    const QString cacheDir = AppPaths::dataDir();
    auto clearCache = [this, cacheDir](const QStringList& patterns, const QString& what) {
        int n = 0;
        for (const QFileInfo& fi : QDir(cacheDir).entryInfoList(patterns, QDir::Files))
            if (QFile::remove(fi.absoluteFilePath())) ++n;
        QMessageBox::information(this, QStringLiteral("Clear cache"),
            n ? QStringLiteral("Cleared %1 %2 file(s). Restart to rebuild.").arg(n).arg(what)
              : QStringLiteral("No %1 cache to clear.").arg(what));
    };
    auto* cMeta = new QPushButton(QStringLiteral("Clear metadata index (tags / titles / icons)"), cache);
    QObject::connect(cMeta, &QPushButton::clicked, this,
        [clearCache] { clearCache({QStringLiteral("appearance_meta_*.json")}, QStringLiteral("metadata")); });
    auto* cIcons = new QPushButton(QStringLiteral("Clear icon-atlas index"), cache);
    QObject::connect(cIcons, &QPushButton::clicked, this,
        [clearCache] { clearCache({QStringLiteral("icon_index_*.json")}, QStringLiteral("icon-index")); });
    auto* cAll = new QPushButton(QStringLiteral("Clear ALL caches"), cache);
    QObject::connect(cAll, &QPushButton::clicked, this,
        [clearCache] { clearCache({QStringLiteral("appearance_meta_*.json"),
                                   QStringLiteral("icon_index_*.json")}, QStringLiteral("cache")); });
    cl->addWidget(cMeta);
    cl->addWidget(cIcons);
    cl->addWidget(cAll);
    auto* resetHdr = new QLabel(QStringLiteral("<b>Reset saved state</b>:"), cache);
    resetHdr->setStyleSheet(QStringLiteral("color:#aaa;margin-top:8px;"));
    cl->addWidget(resetHdr);
    // Clear the model render/load blocklist (models that crashed the 3D view and are skipped).
    // A no-load path to un-quarantine them — selecting a blocklisted model never loads it.
    auto* cBlock = new QPushButton(QStringLiteral("Clear model render blocklist"), cache);
    QObject::connect(cBlock, &QPushButton::clicked, this, [this] {
        QSettings s;
        const int n = s.value(QStringLiteral("models/renderBlocklist")).toStringList().size();
        s.remove(QStringLiteral("models/renderBlocklist"));
        s.remove(QStringLiteral("models/loadGuard"));
        s.sync();
        emit settingsChanged();   // ModelsTab re-syncs its in-memory blocklist
        QMessageBox::information(this, QStringLiteral("Render blocklist"),
            n ? QStringLiteral("Cleared %1 blocklisted model(s). They can be loaded/rendered again "
                               "(use Reload on a model to view it).").arg(n)
              : QStringLiteral("The render blocklist is already empty."));
    });
    cl->addWidget(cBlock);
    // Clear a wardrobe tab's remembered character/outfit (class, gender, slots, weapons,
    // creator picks, dye, animation, and the crash breadcrumb). The remember feature stays on.
    auto clearWardrobeMem = [this](const QString& prefix, const QString& label) {
        QSettings s;
        for (const QString& k : {QStringLiteral("class"), QStringLiteral("gender"), QStringLiteral("anim"),
                                 QStringLiteral("weaponType"), QStringLiteral("weapon"),
                                 QStringLiteral("weaponType2"), QStringLiteral("weapon2"),
                                 QStringLiteral("dyeSel"), QStringLiteral("sheathed"),
                                 QStringLiteral("_loading"), QStringLiteral("_lastCrash")})
            s.remove(prefix + QStringLiteral("/") + k);
        for (int i = 0; i < 5; ++i) s.remove(prefix + QStringLiteral("/slot/%1").arg(i));
        for (int i = 0; i < 9; ++i) s.remove(prefix + QStringLiteral("/creator/%1").arg(i));
        s.sync();
        QMessageBox::information(this, QStringLiteral("Clear wardrobe memory"),
            QStringLiteral("Cleared %1's remembered character/outfit. Reopen the tab (or restart) to start fresh.").arg(label));
    };
    auto* cWard2 = new QPushButton(QStringLiteral("Clear Wardrobe memory"), cache);
    QObject::connect(cWard2, &QPushButton::clicked, this, [clearWardrobeMem] { clearWardrobeMem(QStringLiteral("wardrobe2"), QStringLiteral("Wardrobe")); });
    cl->addWidget(cWard2);
    // Total cache footprint + one-click purge — the TVFS/idx/CoreTOC/index caches quietly add
    // up to ~100+ MB, and users deserve to see and control that. d4data and the ensembles
    // (user content) are excluded from both the number and the purge.
    {
        qint64 bytes = 0;
        const QDir dd(cacheDir);
        for (const QFileInfo& fi : dd.entryInfoList(QDir::Files))
            bytes += fi.size();
        auto* sizeLbl = new QLabel(QStringLiteral(
            "<span style='color:#888'>Cache files: %1 MB (excludes d4data and ensembles)</span>")
            .arg(bytes / (1024 * 1024)), cache);
        cl->addWidget(sizeLbl);
        auto* clearAll = new QPushButton(QStringLiteral("Clear ALL caches"), cache);
        clearAll->setToolTip(QStringLiteral(
            "Delete every rebuildable cache (TVFS path table, .idx index, CoreTOC, icon atlas, "
            "appearance metadata, asset links, animation index…). Settings, ensembles and d4data "
            "are untouched. Everything rebuilds on the next File ▸ Reload."));
        QObject::connect(clearAll, &QPushButton::clicked, this, [this, cacheDir, sizeLbl] {
            if (QMessageBox::question(this, QStringLiteral("Clear all caches"),
                    QStringLiteral("Delete every rebuildable cache file? They rebuild on the next "
                                   "reload (first reload will be slower).")) != QMessageBox::Yes)
                return;
            qint64 freed = 0;
            const QDir dd(cacheDir);
            const QStringList pats{QStringLiteral("*.bin"), QStringLiteral("*.bin.part"),
                                   QStringLiteral("icon_index_v*.json"),
                                   QStringLiteral("appearance_meta_v*.json")};
            for (const QFileInfo& fi : dd.entryInfoList(pats, QDir::Files)) {
                freed += fi.size();
                QFile::remove(fi.absoluteFilePath());
            }
            sizeLbl->setText(QStringLiteral(
                "<span style='color:#888'>Cleared %1 MB — File ▸ Reload rebuilds them.</span>")
                .arg(freed / (1024 * 1024)));
        });
        cl->addWidget(clearAll);
    }
    auto* cacheLoc = new QLabel(QStringLiteral("<span style='color:#888'>%1</span>").arg(cacheDir), cache);
    cacheLoc->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cacheLoc->setWordWrap(true);
    cl->addWidget(cacheLoc);
    // insertWidget(0), not addWidget: Diagnostics is CONSTRUCTED earlier in this function (it used
    // to live on General) and so reaches the page first. Caches & reset is the reason you open
    // Maintenance, so it goes on top regardless of construction order.
    maintTab->insertWidget(0, cache);

    // Keep categories top-aligned within each page.
    // ── Category: Hotkeys ──────────────────────────────────────────────
    // Rebindable keyboard shortcuts for the Export menu's commands. Each edit writes its
    // QSettings key live; MainWindow re-reads them (settingsChanged) and re-applies the
    // shortcuts without a restart. Clearing an edit unbinds that command.
    {
        // "&&" — single & is a mnemonic; see the Caches && reset note above.
        auto* hkBox = new QGroupBox(QStringLiteral("Export && preview shortcuts"), this);
        auto* hkForm = new QFormLayout(hkBox);
        hkForm->setLabelAlignment(Qt::AlignLeft);
        for (const Hotkeys::Def& d : Hotkeys::defs()) {
            auto* edit = new QKeySequenceEdit(hkBox);
            edit->setMaximumSequenceLength(1);   // one chord per command
            edit->setKeySequence(Hotkeys::seq(d.key, d.def));
            const QString key = d.key, def = d.def;
            QObject::connect(edit, &QKeySequenceEdit::keySequenceChanged, this,
                [this, key](const QKeySequence& ks) {
                    QSettings().setValue(key, ks.toString(QKeySequence::PortableText));
                    emit settingsChanged();   // MainWindow re-applies the shortcut live
                });
            m_exportResetActions.push_back([edit, def] {
                edit->setKeySequence(def.isEmpty() ? QKeySequence() : QKeySequence(def));
            });
            // A tiny Clear button so a command can be unbound without fiddling with keys.
            auto* clr = new QToolButton(hkBox);
            clr->setText(QStringLiteral("✕"));
            clr->setToolTip(QStringLiteral("Unbind"));
            QObject::connect(clr, &QToolButton::clicked, edit, &QKeySequenceEdit::clear);
            auto* rowW = new QWidget(hkBox);
            auto* rowL = new QHBoxLayout(rowW);
            rowL->setContentsMargins(0, 0, 0, 0);
            rowL->addWidget(edit, 1);
            rowL->addWidget(clr);
            hkForm->addRow(d.label, rowW);
        }
        auto* hkHint = new QLabel(
            QStringLiteral("Shortcuts apply to the Export menu on the active tab. "
                           "Click a field and press a key combination; ✕ unbinds it."), this);
        hkHint->setWordWrap(true);
        hkHint->setStyleSheet(QStringLiteral("color:#888;"));
        hotkeysTab->addWidget(hkBox);
        hotkeysTab->addWidget(hkHint);
    }

    // ── Category: Dev ──────────────────────────────────────────────────
    // ("Developer mode" is retired — the Files/String Lists tabs are gone and the viewport
    //  debug panels are simply always available.)
    auto* devBox = new QGroupBox(QStringLiteral("Indexing (advanced)"), this);
    auto* dvl = new QVBoxLayout(devBox);
    m_includeLocale = new QCheckBox(QStringLiteral("Index locale packs (more file names, slower)"), devBox);
    m_includeLocale->setToolTip(QStringLiteral(
        "Also index the locale text/speech/cutscene/video packs. Adds ~2.6M localized names but "
        "slows indexing and isn't needed for models/textures. Off by default; applied on OK "
        "(re-index or restart to take effect).\n"
        "See casc_coverage.txt (next to the exe) for a breakdown of what the archive contains."));
    dvl->addWidget(m_includeLocale);
    maintTab->addWidget(devBox);   // same reasoning as Diagnostics above

    generalTab->addStretch(1);
    interfaceTab->addStretch(1);   // without this its boxes centre vertically on a tall dialog
    modelsTab->addStretch(1);
    wardrobeTab->addStretch(1);
    exportTab->addStretch(1);
    hotkeysTab->addStretch(1);
    maintTab->addStretch(1);
    // Stretch 1: the tabs take ALL surplus height. Without it QBoxLayout shares the extra
    // with the version QLabel (default Preferred policy grows), so enlarging the now-resizable
    // dialog opened a widening gap above OK/Cancel instead of showing more settings.
    root->addWidget(tabs, 1);

    // Tool version (the application's own version, not a game/d4data build).
    m_version = new QLabel(this);
    m_version->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_version->setStyleSheet(QStringLiteral("color:#888"));
    root->addWidget(m_version);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    // Restore Defaults: reset the preference toggles (View / Models / Wardrobe panels / Performance /
    // Export / Preview-popup features) to their defaults. Deliberately leaves directory paths, CASC
    // product, and the tuned Preview-popup camera/lighting/background untouched. OK still applies.
    auto* resetBtn = bb->addButton(QStringLiteral("Restore Defaults"), QDialogButtonBox::ResetRole);
    QObject::connect(resetBtn, &QPushButton::clicked, this,
        [this, pfCoalesce, pfAsync, pfTex, pfVram] {
            if (QMessageBox::question(this, QStringLiteral("Restore Defaults"),
                    QStringLiteral("Reset the View, Models, Wardrobe-panel, Performance, Export and "
                                   "preview-feature options to their defaults?\n\nYour folder paths, "
                                   "CASC product and preview camera/lighting are left unchanged.")) != QMessageBox::Yes)
                return;
            // Non-live (persisted on OK).
            m_rememberTab->setChecked(false);
            m_includeLocale->setChecked(false);
            if (m_rememberPanels) m_rememberPanels->setChecked(true);   // default on (writes live)
            // Models (persist live on toggle).
            m_mdlHover->setChecked(true);
            m_mdlRememberLast->setChecked(false);
            // Live (write + notify the open Wardrobe tab immediately via the live() handlers).
            m_w2NudeBase->setChecked(false);   // nude base off by default
            m_w2RememberPanels->setChecked(true);
            m_w2ShowLog->setChecked(false);
            // Performance (write on toggle).
            pfCoalesce->setChecked(true);
            pfAsync->setChecked(true);   // background loading is the default now
            pfTex->setChecked(true);
            pfVram->setChecked(false);
            // Export tab (each action sets its widget to default → live-writes the key).
            for (const auto& r : m_exportResetActions) r();
            // Preview Settings popup feature toggles (rendering features, not camera/lighting/bg).
            // These are re-applied from settings on the next wardrobe rebuild, which we trigger below.
            {
                QSettings s;
                s.setValue(QStringLiteral("wardrobe2/viewport/detail"),     true);
                s.setValue(QStringLiteral("wardrobe2/viewport/specaa"),     true);
                s.setValue(QStringLiteral("wardrobe2/viewport/shadow"),     true);
                s.setValue(QStringLiteral("wardrobe2/viewport/ssao"),       true);
                s.setValue(QStringLiteral("wardrobe2/viewport/subsurface"), true);
                s.setValue(QStringLiteral("wardrobe2/viewport/hair"),       true);
                s.setValue(QStringLiteral("wardrobe2/viewport/ibl"),        true);
                s.setValue(QStringLiteral("wardrobe2/viewport/mask"),       false);
                s.setValue(QStringLiteral("wardrobe2/viewport/tonemap"),    true);
                s.setValue(QStringLiteral("wardrobe2/viewport/fur"),        true);
            }
            emit settingsChanged();
            emit wardrobeLiveChanged(true);   // re-apply viewport features + refresh the preview
        });
    connect(bb, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    root->addWidget(bb);

    // Load current values.
    m_game->setText(Config::gameDir());
    m_tact->setText(Config::tactKeysPath());
    m_d4data->setText(Config::d4dataDir());
    m_product->setCurrentText(Config::cascProduct().isEmpty() ? QStringLiteral("fenris")
                                                              : Config::cascProduct());
    m_rememberTab->setChecked(
        QSettings().value(QStringLiteral("view/rememberTab"), false).toBool());
    m_includeLocale->setChecked(QSettings().value(QStringLiteral("casc/includeLocalePacks"), false).toBool());
    // Every live-persisted Wardrobe toggle must be restored here. Auto Animate was missing, so it
    // opened unchecked no matter what was stored — the setting was written correctly and simply
    // never displayed, which reads to the user as "settings are not remembered".
    //
    // Defaults MUST match the ones the consumer uses, or the dialog reports a different state from
    // the one the app is running under. Checked against the read sites in WardrobeTab2:
    //   nudeBase false · autoAnimate false · ensembles true · rememberPanels true · dbg/log false
    //
    // Signals blocked while loading: `live()` writes on toggled, so restoring a stored `true` onto
    // a freshly-built (unchecked) box would re-emit wardrobeLiveChanged and make merely OPENING
    // Settings rebuild the wardrobe.
    {
        QSettings s;
        const QSignalBlocker b1(m_w2NudeBase), b2(m_w2AutoAnimate), b3(m_w2SecEnsembles),
                             b4(m_w2RememberPanels), b5(m_w2ShowLog);
        m_w2NudeBase->setChecked(s.value(QStringLiteral("wardrobe2/nudeBase"), false).toBool());
        m_w2AutoAnimate->setChecked(s.value(QStringLiteral("wardrobe2/autoAnimate"), false).toBool());
        m_w2SecEnsembles->setChecked(s.value(QStringLiteral("wardrobe2/viewport/ensembles"), true).toBool());
        m_w2RememberPanels->setChecked(s.value(QStringLiteral("wardrobe2/rememberPanels"), true).toBool());
        m_w2ShowLog->setChecked(s.value(QStringLiteral("wardrobe2/dbg/log"), false).toBool());
    }
    updateChecks();
    updateVersion();
    snapshotLiveSettings();   // remember current live values so Cancel can revert them
}

// ── Which keys Cancel has to undo ────────────────────────────────────────────────────────────
// Live keys persist the instant a widget changes, so Cancel has to put them back. The list below
// was hand-maintained and had drifted: seven keys (export/looseTextures, export/gifDither, both
// export/gifPhysics*, export/catalogueFrames, export/catOnlyNew, models/baseColorOnly) were
// missing, so Cancel silently kept those edits. A hand-written list parallel to ~60 widget
// registrations will always drift again — every new checkbox is one more chance to forget.
//
// So the export-side keys are now DERIVED instead (see kDerivedLivePrefixes + snapshotLiveSettings
// + reject): everything under export/, retarget/ and hotkeys/ is picked up automatically, whether
// or not anyone remembers to list it. Those three namespaces are owned outright by this dialog —
// nothing else in the app writes them — and the dialog is application-modal (MainWindow uses
// exec()), so no other widget can be writing them while it is open.
//
// models/, wardrobe2/ and tex/ stay on the explicit list: those namespaces ALSO carry state written
// by the tabs themselves (panel geometry via PanelPersist, models/renderBlocklist from the render-
// crash recovery, tex/lastDir), and sweeping them wholesale would let Cancel revert things this
// dialog never touched.
static const char* const kDerivedLivePrefixes[] = { "export/", "retarget/", "hotkeys/" };

// Not everything under those prefixes is a dialog WIDGET. Two families are written by the rest of
// the app and must survive Cancel:
//   • "…Dir"        — last-used folders (export/captureDir, written by LookIcon and MainWindow when
//                     the user saves an image). Reverting one would move the next save's default
//                     folder back for no reason the user could connect to pressing Cancel.
//   • "export/warn…" — don't-warn-again flags ticked inside a message box (export/warnGifPhysics).
//                     Reverting one would resurrect a warning the user just dismissed for good.
// Both are naming conventions this codebase already follows, so a future key that follows them is
// excluded automatically — no new name to remember.
//
// Note the asymmetry, which is the whole point of doing it this way round: forgetting to EXCLUDE a
// key means Cancel also reverts a remembered folder — mildly annoying, immediately visible.
// Forgetting to INCLUDE one (the old hand-listed design) means Cancel silently does not cancel,
// which is invisible and is exactly the bug this replaces.
static bool isDerivedLiveKey(const QString& k)
{
    bool inNamespace = false;
    for (const char* p : kDerivedLivePrefixes)
        if (k.startsWith(QLatin1String(p))) { inNamespace = true; break; }
    if (!inNamespace) return false;
    if (k.endsWith(QLatin1String("Dir"))) return false;
    if (k.startsWith(QLatin1String("export/warn"))) return false;
    return true;
}

// The keys Cancel must restore that the prefix sweep above does NOT cover — i.e. the ones in the
// shared models/ · wardrobe2/ · tex/ namespaces. Anything under export/, retarget/ or hotkeys/ is
// derived and does not belong here; a few are kept below only because they are cheap and make the
// snapshot correct even for a key that does not exist yet on a fresh profile.
static QStringList liveSettingKeys()
{
    return {
        QStringLiteral("models/hoverPreview"), QStringLiteral("models/rememberLast"),
        QStringLiteral("models/autoLoad"),
        QStringLiteral("models/rememberPanels"), QStringLiteral("models/fillSkin"),
        QStringLiteral("models/autoRender3D"), QStringLiteral("models/clothSim"),
        QStringLiteral("models/baseColorOnly"),   // was missing: Cancel kept this edit

        QStringLiteral("wardrobe2/nudeBase"),
        QStringLiteral("wardrobe2/autoAnimate"),
        QStringLiteral("wardrobe2/rememberPanels"),
        QStringLiteral("wardrobe2/dbg/log"), QStringLiteral("wardrobe2/viewport/ensembles"),
        QStringLiteral("wardrobe2/perf/coalesce"), QStringLiteral("wardrobe2/perf/asyncLoad"),
        QStringLiteral("wardrobe2/perf/texCache"), QStringLiteral("wardrobe2/perf/vramPool"),
        QStringLiteral("tex/format"), QStringLiteral("tex/trim"), QStringLiteral("tex/lastDir"),
        QStringLiteral("export/gifFps"), QStringLiteral("export/gifTurntableFrames"),
        QStringLiteral("export/gifScale"), QStringLiteral("export/gifMaxColors"),
        QStringLiteral("export/gifOptimize"), QStringLiteral("export/gifTargetMB"),
        QStringLiteral("export/gifCropToModel"),
        QStringLiteral("export/imageFormat"), QStringLiteral("export/imageScale"),
        QStringLiteral("export/imageQuality"),
        QStringLiteral("export/transparentBg"),
        QStringLiteral("export/includeTex"), QStringLiteral("export/bakeDye"),
        QStringLiteral("export/bakeDetail"), QStringLiteral("export/reconstructNormalZ"),
        QStringLiteral("export/includeAnim"),
        // The five animation SOURCES (util/AnimExportScope.h). These replaced export/animScope
        // and export/includePulledAnims, which AnimExportScope::load() migrates from once.
        QStringLiteral("export/animOriginal"), QStringLiteral("export/animPreviewed"),
        QStringLiteral("export/animPulled"),   QStringLiteral("export/animBase"),
        QStringLiteral("export/animSets"),
        QStringLiteral("export/animMaxFrames"), QStringLiteral("export/animExclude"),
        QStringLiteral("export/looseTexturesAll"),
        QStringLiteral("export/withDeps"), QStringLiteral("export/boneNamesTranslated"),
        QStringLiteral("export/wardrobeScope"),
        QStringLiteral("export/exportFxSim"), QStringLiteral("export/bothGenders"),
        QStringLiteral("export/osNotify"), QStringLiteral("export/wardrobeNameTemplate"),
        QStringLiteral("export/hardpointEmpties"),
        QStringLiteral("export/blenderFriendly"), QStringLiteral("export/xMirror"),
        QStringLiteral("retarget/enginePreset"), QStringLiteral("retarget/unitScale"),
        QStringLiteral("retarget/remapWeights"), QStringLiteral("retarget/collapseCloth"),
        QStringLiteral("export/includeBaseBody"), QStringLiteral("export/includeBaseHead"),
        QStringLiteral("retarget/setManifest"),
        QStringLiteral("hotkeys/exportSelection"), QStringLiteral("hotkeys/exportToLast"),
        QStringLiteral("hotkeys/exportAnimations"), QStringLiteral("hotkeys/saveImage"),
        QStringLiteral("hotkeys/turntable"), QStringLiteral("hotkeys/animLoop"),
        // Bulk Extract preferences (the box on the Wardrobe/Catalogue page). Explicit, not derived:
        // bulk/ also holds tab-owned state this dialog never touches — outDir, filter, mode, the
        // saved queues, presets, splitter position — so the namespace cannot be swept wholesale.
        QStringLiteral("bulk/coTextures"), QStringLiteral("bulk/buffers"),
        QStringLiteral("bulk/report"),
    };
}

void SettingsDialog::snapshotLiveSettings()
{
    QSettings s;
    m_snapshot.clear();
    for (const QString& k : liveSettingKeys())
        m_snapshot.insert(k, s.value(k));   // invalid QVariant if the key was unset
    // …plus every key that currently EXISTS under a derived namespace. A key that does not exist
    // yet is deliberately not inserted here: reject() removes any derived key that appeared while
    // the dialog was open, which covers the first-ever write to a setting without needing to know
    // its name in advance. Between the two, a new export/ checkbox is Cancel-safe the day it is
    // added, with nothing to remember.
    for (const QString& k : s.allKeys())
        if (isDerivedLiveKey(k) && !m_snapshot.contains(k)) m_snapshot.insert(k, s.value(k));
}

void SettingsDialog::reject()
{
    // Undo everything the live widgets wrote since the dialog opened, so Cancel truly cancels.
    // Only touch keys that actually changed → a plain Cancel with no edits is a no-op (no rebuild).
    QSettings s;
    bool changed = false;
    for (auto it = m_snapshot.constBegin(); it != m_snapshot.constEnd(); ++it) {
        if (s.value(it.key()) == it.value()) continue;   // unchanged
        changed = true;
        if (it.value().isValid()) s.setValue(it.key(), it.value());
        else                      s.remove(it.key());
    }
    // Derived namespaces: drop any key that did not exist when the dialog opened. This is the half
    // of the sweep that catches the FIRST write to a setting — snapshotLiveSettings() can only
    // record keys that already exist, so without this a brand-new export/ toggle would survive
    // Cancel on a profile that had never set it. Collected first, removed after: removing while
    // iterating allKeys() would invalidate it.
    QStringList appeared;
    for (const QString& k : s.allKeys())
        if (isDerivedLiveKey(k) && !m_snapshot.contains(k)) appeared << k;
    for (const QString& k : appeared) { s.remove(k); changed = true; }
    if (changed) {
        s.sync();
        emit settingsChanged();          // re-sync tabs' mirrored checkboxes
        emit wardrobeLiveChanged(true);  // revert the wardrobe preview to the restored panel state
    }
    QDialog::reject();
}

void SettingsDialog::showExportTab()
{
    if (m_tabs && m_exportTabIndex >= 0) m_tabs->setCurrentIndex(m_exportTabIndex);
}

void SettingsDialog::updateChecks()
{
    // First-run cue: draw a soft amber border on a required path field until it points somewhere
    // valid, so a new user can see at a glance what still needs setting (Game + D4 Data are required;
    // TACT keys are optional). Cleared once the path validates.
    static const QString kBad = QStringLiteral("QLineEdit { border: 1px solid #c8873a; border-radius: 3px; }");
    auto flag = [](QLineEdit* e, bool ok) { if (e) e->setStyleSheet(ok ? QString() : kBad); };

    const QString g = m_game->text().trimmed();
    const bool gameOk = !g.isEmpty() && QFile::exists(QDir(g).filePath(QStringLiteral(".build.info")));
    m_gameChk->setText(checkMark(gameOk));
    flag(m_game, gameOk);

    // TACT path may be a single key file OR a folder containing *.txt/*.csv keys.
    const QString tactPath = m_tact->text().trimmed();
    const QFileInfo tactFi(tactPath);
    bool tactOk = tactFi.isFile();
    if (!tactOk && tactFi.isDir())
        tactOk = !QDir(tactPath).entryList(
            {QStringLiteral("*.txt"), QStringLiteral("*.csv")}, QDir::Files).isEmpty();
    m_tactChk->setText(checkMark(tactOk));
    if (m_tactDl)
        m_tactDl->setText(tactOk ? QStringLiteral("Update") : QStringLiteral("Download"));

    const QString d = m_d4data->text().trimmed();
    const bool d4ok = !d.isEmpty() &&
        QFile::exists(QDir(d).filePath(QStringLiteral("json/base/CoreTOC.dat.json")));
    m_d4dataChk->setText(checkMark(d4ok));
    flag(m_d4data, d4ok);
    // Already-installed → the button checks for and applies updates instead.
    if (m_d4dataDl)
        m_d4dataDl->setText(d4ok ? QStringLiteral("Update") : QStringLiteral("Download"));
}

void SettingsDialog::updateVersion()
{
    m_version->setText(QStringLiteral("D4AssetBrowser  v%1")
                           .arg(QApplication::applicationVersion()));
}

// Notify-only: report whether newer d4data / TACT keys exist. Downloads nothing.
void SettingsDialog::runUpdateCheck()
{
    if (m_updStatus) {
        m_updStatus->setStyleSheet(QStringLiteral("color:#888;"));
        m_updStatus->setText(QStringLiteral("Checking…"));
    }
    if (m_updCheckBtn) m_updCheckBtn->setEnabled(false);
    auto* uc = new UpdateCheck(this);
    // NB: a manual check always reports the truth — it deliberately does NOT record/suppress
    // anything. Only the startup notification tracks "already told you about this version".
    connect(uc, &UpdateCheck::finished, this,
            [this](int d4, int tact, const QString& detail, const QString&, const QString&) {
        if (m_updCheckBtn) m_updCheckBtn->setEnabled(true);
        if (!m_updStatus) return;
        const bool any = d4 == UpdateCheck::UpdateAvailable || tact == UpdateCheck::UpdateAvailable;
        m_updStatus->setStyleSheet(any ? QStringLiteral("color:#d8a23a;font-weight:bold;")
                                       : QStringLiteral("color:#888;"));
        m_updStatus->setText(detail + lastCheckedSuffix());   // UpdateCheck just stamped it → "just now"
    });
    uc->start();
}

void SettingsDialog::downloadTactKeys()
{
    // qtbase here was built without QtNetwork, so fetch via curl (ships with
    // Windows 10+) — same external-tool pattern as the d4data git download.
    const QString curl = QStandardPaths::findExecutable(QStringLiteral("curl"));
    if (curl.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("TACT keys"),
            QStringLiteral("curl was not found on PATH. It ships with Windows 10/11; "
                           "otherwise download the keys file manually and Browse to it."));
        return;
    }

    // Download into the folder the user selected (or the current file's parent);
    // fall back to AppData when nothing is set yet.
    const QFileInfo curFi(m_tact->text().trimmed());
    const QString dir = curFi.isDir()  ? curFi.absoluteFilePath()
                      : curFi.isFile() ? curFi.absolutePath()
                                       : AppPaths::dataDir();
    QDir().mkpath(dir);
    const QString dest = QDir(dir).filePath(QStringLiteral("d4_tact_keys_clean.txt"));
    const bool existed = QFileInfo::exists(dest);

    m_tactDl->setEnabled(false);
    m_tactDl->setText(existed ? QStringLiteral("Checking…") : QStringLiteral("Downloading…"));

    // NB: a conditional fetch (-z / If-Modified-Since) does NOT work against this host —
    // raw.githubusercontent.com serves ETag but no Last-Modified, so the server answers 200
    // unconditionally and the old "304 → already up to date" branch could never fire: it silently
    // re-downloaded every single time. Fetch to a temp file and compare CONTENT instead, replacing
    // the real file only when the bytes actually differ (same rule UpdateCheck uses).
    const QString tmp = dest + QStringLiteral(".new");
    auto argsFor = [&tmp](int i) {
        return QStringList{QStringLiteral("-s"), QStringLiteral("-L"), QStringLiteral("-f"),
                           QStringLiteral("-w"), QStringLiteral("%{http_code}"),
                           QStringLiteral("-o"), tmp,
                           QString::fromLatin1(kTactKeysUrls[i])};
    };
    const QStringList args = argsFor(0);

    // Sequential candidate retry: on failure, re-run curl against the next known location before
    // giving up (the upstream file has moved once already).
    auto attempt = std::make_shared<std::function<void(int)>>();
    auto* proc = new QProcess(this);
    connect(proc, &QProcess::finished, this,
            [this, proc, dest, tmp, dir, existed, attempt, argsFor](int code, QProcess::ExitStatus) {
                const QString http = QString::fromLatin1(
                    proc->readAllStandardOutput()).trimmed();
                const QFileInfo tf(tmp);

                if (code != 0 || !http.startsWith(QLatin1Char('2')) || !tf.isFile() || tf.size() <= 0) {
                    QFile::remove(tmp);
                    constexpr int kN = int(sizeof(kTactKeysUrls) / sizeof(kTactKeysUrls[0]));
                    const int nextIdx = proc->property("urlIdx").toInt() + 1;
                    if (nextIdx < kN) { (*attempt)(nextIdx); return; }   // reuse proc, try next URL
                    proc->deleteLater();
                    m_tactDl->setEnabled(true);
                    QMessageBox::warning(this, QStringLiteral("TACT keys"),
                        QStringLiteral("Download failed (curl exit %1, HTTP %2).").arg(code).arg(http));
                    updateChecks();
                    return;
                }
                proc->deleteLater();
                m_tactDl->setEnabled(true);

                auto hashOf = [](const QString& p) -> QByteArray {
                    QFile f(p);
                    if (!f.open(QIODevice::ReadOnly)) return {};
                    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha1);
                };
                const QByteArray th = hashOf(tmp);
                const bool same = existed && !th.isEmpty() && th == hashOf(dest);
                m_tact->setText(dir);   // folder-based: point at the containing folder

                QString primary;
                if (same) {
                    QFile::remove(tmp);   // identical — leave the user's file untouched
                    primary = QStringLiteral("Primary list: already up to date.");
                } else {
                    if (existed) QFile::remove(dest);
                    QFile::rename(tmp, dest);
                    const QFileInfo fi(dest);
                    primary = QStringLiteral("Primary list: %1 (%2 bytes).")
                                  .arg(existed ? QStringLiteral("updated") : QStringLiteral("downloaded"))
                                  .arg(fi.size());
                }
                // Both sources, always — folder mode merges whatever is present, so a second file
                // only ever adds keys.
                downloadSecondaryKeys(dir, primary);
                updateChecks();   // restores the Update/Download label
            });
    *attempt = [proc, curl, argsFor](int idx) {
        proc->setProperty("urlIdx", idx);
        proc->start(curl, argsFor(idx));
    };
    (*attempt)(0);
}

void SettingsDialog::downloadSecondaryKeys(const QString& dir, const QString& primaryMsg)
{
    const QString curl = QStandardPaths::findExecutable(QStringLiteral("curl"));
    const QString dest = QDir(dir).filePath(QString::fromLatin1(kCascLibKeysFile));
    if (curl.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("TACT keys"),
            primaryMsg + QStringLiteral("\n\nSecondary list skipped — curl not on PATH."));
        return;
    }
    const QString tmp = dest + QStringLiteral(".new");
    auto* proc = new QProcess(this);
    connect(proc, &QProcess::finished, this,
            [this, proc, dest, tmp, primaryMsg](int code, QProcess::ExitStatus) {
                const QString http = QString::fromLatin1(proc->readAllStandardOutput()).trimmed();
                proc->deleteLater();
                const QFileInfo tf(tmp);
                QString second;
                if (code != 0 || !http.startsWith(QLatin1Char('2')) || !tf.isFile() || tf.size() <= 0) {
                    QFile::remove(tmp);
                    // Non-fatal by design: the primary list is the one that matters, and a failed
                    // secondary must not look like the whole download failed.
                    second = QStringLiteral("Secondary list (CascLib): unavailable "
                                            "(curl %1, HTTP %2) — primary keys are still in place.")
                                 .arg(code).arg(http);
                } else {
                    if (QFileInfo::exists(dest)) QFile::remove(dest);
                    QFile::rename(tmp, dest);
                    second = QStringLiteral("Secondary list (CascLib KeyService.cs): saved as %1.\n"
                                            "Only its \"Diablo IV Retail\" section is read; the "
                                            "Overwatch/WoW keys in the same file are ignored.")
                                 .arg(QString::fromLatin1(kCascLibKeysFile));
                }
                QMessageBox::information(this, QStringLiteral("TACT keys"),
                    primaryMsg + QStringLiteral("\n") + second
                    + QStringLiteral("\n\nBoth files live in the keys folder and are merged on load. "
                                     "Use \"Verify TACT Keys.bat\" to see which keys this game build "
                                     "can actually use."));
                updateChecks();
            });
    proc->start(curl, QStringList{QStringLiteral("-s"), QStringLiteral("-L"), QStringLiteral("-f"),
                                  QStringLiteral("-w"), QStringLiteral("%{http_code}"),
                                  QStringLiteral("-o"), tmp,
                                  QString::fromLatin1(kCascLibKeysUrl)});
}

void SettingsDialog::downloadD4Data()
{
    DependencyDialog dlg(this);
    connect(&dlg, &DependencyDialog::d4dataInstalled, this, [this](const QString& p) {
        m_d4data->setText(p);
        updateChecks();
        updateVersion();
    });
    dlg.exec();
}

void SettingsDialog::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    if (m_sized) return;
    m_sized = true;   // only the FIRST show; after that the user's own size wins
    const QScreen* scr = screen() ? screen() : QGuiApplication::primaryScreen();
    if (!scr) return;
    const QRect avail = scr->availableGeometry();   // excludes the taskbar/dock
    // The wanted size comes from the PAGES, not from sizeHint(). QScrollArea::sizeHint() ends with
    // boundedTo(36*fontHeight, 24*fontHeight) — a hard cap that setSizeAdjustPolicy cannot lift,
    // because QScrollArea overrides sizeHint outright. Now that every page is inside one, the tab
    // widget's hint is ~576x384 whatever the content is, so clamping to it made the dialog SMALLER
    // than before the fix: 590px wide (the Directories grid then needs a horizontal scrollbar) and
    // ~460px tall on a monitor with 1300px to spare. The clamp is meant to be a ceiling, not the
    // binding term.
    QSize want = sizeHint();
    for (int i = 0; m_tabs && i < m_tabs->count(); ++i)
        if (auto* sa = qobject_cast<QScrollArea*>(m_tabs->widget(i)))
            if (QWidget* pg = sa->widget())
                want = want.expandedTo(pg->sizeHint() + QSize(48, 120));   // bars + tabbar + buttons
    // …and wide enough for the TAB BAR itself. The loop above only ever asked for the widest PAGE,
    // so with nine tabs the bar was routinely the binding term and nothing accounted for it: the
    // dialog opened narrower than its own tab strip and Qt elided the labels to "eneral"/"Experim".
    // tabBar()->sizeHint() is the full un-elided strip (setElideMode(ElideNone) in the constructor
    // is what makes that true); + 24 covers the frame and the bar's leading indent.
    if (m_tabs && m_tabs->tabBar())
        want.setWidth(qMax(want.width(), m_tabs->tabBar()->sizeHint().width() + 24));
    want = want.expandedTo(size());   // never below the width the constructor already asked for
    resize(qMin(want.width(),  int(avail.width()  * 0.90)),
           qMin(want.height(), int(avail.height() * 0.90)));
    // Re-centre after the resize, so a clamped dialog is not left hanging off an edge.
    move(avail.center() - QPoint(width() / 2, height() / 2));
}

void SettingsDialog::accept()
{
    Config::setGameDir(m_game->text().trimmed());
    Config::setTactKeysPath(m_tact->text().trimmed());
    Config::setD4dataDir(m_d4data->text().trimmed());
    Config::setCascProduct(m_product->currentText().trimmed().isEmpty()
                               ? QStringLiteral("fenris")
                               : m_product->currentText().trimmed());
    QSettings().setValue(QStringLiteral("view/rememberTab"), m_rememberTab->isChecked());
    QSettings().setValue(QStringLiteral("casc/includeLocalePacks"), m_includeLocale->isChecked());
    // Models / Wardrobe / Performance / Export toggles are written live on change; nothing to persist
    // here. Notify the host once so tabs re-sync their mirrored checkboxes (e.g. Models Tex/Anim).
    emit settingsChanged();
    QDialog::accept();
}
