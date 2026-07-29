#include "app/SettingsDialog.h"

#include "app/AppPaths.h"

#include "app/Config.h"
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
#include <QTabWidget>
#include <QToolButton>
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

    auto* root = new QVBoxLayout(this);

    // Grouped into tabs so the dialog stays compact as categories grow. Each page is a
    // plain scrolling column of QGroupBox categories; the version line + OK/Cancel live
    // below the tabs so they're always visible.
    auto* tabs = new QTabWidget(this);
    auto makeTab = [tabs](const QString& title) -> QVBoxLayout* {
        auto* page = new QWidget(tabs);
        auto* lay  = new QVBoxLayout(page);
        tabs->addTab(page, title);
        return lay;
    };
    QVBoxLayout* generalTab  = makeTab(QStringLiteral("General"));
    QVBoxLayout* modelsTab   = makeTab(QStringLiteral("Models"));    // its own tab, like Wardrobe
    QVBoxLayout* wardrobeTab = makeTab(QStringLiteral("Wardrobe"));
    QVBoxLayout* exportTab   = makeTab(QStringLiteral("Export"));
    m_exportTabIndex = tabs->count() - 1;   // remember for showExportTab()
    QVBoxLayout* hotkeysTab  = makeTab(QStringLiteral("Hotkeys"));
    QVBoxLayout* maintTab    = makeTab(QStringLiteral("Maintenance"));
    QVBoxLayout* infoTab     = makeTab(QStringLiteral("Information"));
    m_tabs = tabs;   // kept so callers can open the dialog on a specific tab

    // ── Tab: Information ───────────────────────────────────────────────
    // A read-only reference explaining how the tool reads the game and what each
    // file type is, so it's not a black box. Plain scrolling rich text.
    {
        auto* info = new QTextBrowser(this);
        info->setOpenExternalLinks(false);
        info->setStyleSheet(QStringLiteral(
            "QTextBrowser{background:#1e1e1e;border:1px solid #3a3a3a;border-radius:4px;"
            "color:#cccccc;padding:8px;}"));
        info->setHtml(QStringLiteral(R"HTML(
<style>
  h2 { color:#8ab4f8; margin:14px 0 4px 0; }
  h3 { color:#d8a23a; margin:12px 0 2px 0; }
  code { background:#2c2c2c; color:#e0e0e0; padding:1px 4px; border-radius:2px; }
  p, li { line-height:1.4; }
  .k { color:#9ccc65; font-weight:bold; }
</style>

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

<h2>Materials &amp; textures &mdash; <code>.tex</code> &rarr; images</h2>
<p>Each material (from its <code>.mat.json</code> &rarr; tUberMaterial) lists <b>texture bindings</b>
&mdash; BASE_COLOR, NORMAL, ROUGHNESS/METAL/AO, EMISSIVE &mdash; plus scalar MaterialValues
(metallic, roughness, AO, emissive strength). A <span class="k">.tex</span> is the actual texture
payload, block-compressed (BC1/BC3/BC4/BC5/BC7). The tool CPU-decodes those blocks (bit-exact vs
reference) and can embed a model's base-colour map in the <code>.glb</code> or export loose
<code>.png</code>s. Detail maps carry their own tiling scale (e.g. 8&ndash;20&times;).</p>

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

<h2>Cloth &amp; physics</h2>
<p>Skirts, capes and chains use the game's authored <b>NvCloth</b> data, parsed straight from each
piece's ClothData: collision capsules (bone-bound tubes around the limbs), low-poly simulation
cages, per-vertex pin/motion constraints, and per-piece tuning from the matching
<code>.clt.json</code>. The viewport simulates those cages against the capsules and drives the
skinned cloth from them, matching how the game does it.</p>

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

<h2>SNO &mdash; the id behind everything</h2>
<p>Every asset has an <b>SNO</b> (Sequence Number Object) id. The tool's indexes map friendly
names &harr; SNO &harr; the numeric CASC path, which is how a click on "Fur-Lined Pants" ends up
reading the right blob out of packed storage.</p>
)HTML"));
        infoTab->addWidget(info);
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

    // ── Update check (notify-only — nothing is downloaded) ─────────────
    // Tells you an update EXISTS before you commit to a Download, and can run at startup.
    {
        auto* updRow = new QHBoxLayout();
        m_updCheckBtn = new QPushButton(QStringLiteral("Check for updates"), dirs);
        m_updCheckBtn->setToolTip(QStringLiteral(
            "Ask whether newer d4data / TACT keys exist — nothing is downloaded.\n"
            "d4data: compares your checkout's commit against the remote repo (git ls-remote).\n"
            "TACT keys: a conditional HEAD request against the keys file."));
        m_updAuto = new QCheckBox(QStringLiteral("Check at startup"), dirs);
        m_updAuto->setChecked(QSettings().value(QStringLiteral("updates/checkAtStartup"), false).toBool());
        m_updAuto->setToolTip(QStringLiteral(
            "Run this check each time the tool opens and notify you only if an update is available."));
        m_updStatus = new QLabel(dirs);
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
        auto* updWrap = new QWidget(dirs);
        updWrap->setLayout(updRow);
        grid->addWidget(updWrap, 3, 0, 1, 5);
        QObject::connect(m_updCheckBtn, &QPushButton::clicked, this, [this] { runUpdateCheck(); });
        QObject::connect(m_updAuto, &QCheckBox::toggled, this,
                         [](bool on) { QSettings().setValue(QStringLiteral("updates/checkAtStartup"), on); });
    }

    generalTab->addWidget(dirs);

    // ── Category: Game ─────────────────────────────────────────────────
    auto* game = new QGroupBox(QStringLiteral("Game data"), this);
    auto* gl = new QGridLayout(game);
    gl->addWidget(new QLabel(QStringLiteral("Game build (CASC product)"), game), 0, 0);
    m_product = new QComboBox(game);
    m_product->setEditable(true);
    m_product->addItems({QStringLiteral("fenris"), QStringLiteral("fenrisb"),
                         QStringLiteral("fenrisdev")});
    gl->addWidget(m_product, 0, 1);
    gl->setColumnStretch(1, 1);
    generalTab->addWidget(game);

    // ── Category: View ─────────────────────────────────────────────────
    auto* view = new QGroupBox(QStringLiteral("Interface"), this);
    auto* vl = new QVBoxLayout(view);
    m_rememberTab = new QCheckBox(QStringLiteral("Remember the last open tab"), view);
    m_rememberTab->setToolTip(QStringLiteral(
        "Reopen the tab you were last on when the app starts."));
    vl->addWidget(m_rememberTab);
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
    generalTab->addWidget(view);

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
        generalTab->addWidget(hov);
    }

    // ── Category: Icon indicators ──────────────────────────────────────
    // A small badge painted over an item's original icon showing whether a renderable MODEL
    // exists behind it: green ✓ when present, red ✗ when the icon loads but the model is missing.
    // Each iconated tab enables the two indicators independently (so you can flag ONLY the
    // icon-without-model cases). Off by default. Applied on the next icon repaint (scroll / reopen).
    {
        auto* icons = new QGroupBox(QStringLiteral("Icon indicators (model present / missing)"), this);
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
        generalTab->addWidget(icons);
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
            QMessageBox::information(this, QStringLiteral("Import settings profile"),
                QStringLiteral("Profile applied. Restart the app so every tab picks it up cleanly."));
        });
    }

    // ── Category: Models ───────────────────────────────────────────────
    // Folded in from the old "File > Models settings…" mini-dialog (now removed) so all app
    // preferences live in one place. These persist live (no OK needed), like the Wardrobe toggles.
    auto* models = new QGroupBox(QStringLiteral("Models tab"), this);
    auto* ml = new QVBoxLayout(models);
    auto mdlChk = [this, models, ml](const QString& key, const QString& label, bool def, const QString& tip) {
        auto* cb = new QCheckBox(label, models);
        if (!tip.isEmpty()) cb->setToolTip(tip);
        cb->setChecked(QSettings().value(key, def).toBool());
        QObject::connect(cb, &QCheckBox::toggled, this, [key](bool on) { QSettings().setValue(key, on); });
        ml->addWidget(cb);
        return cb;
    };
    // Auto-Load — relocated from the preview header's toolbar button. Applied live by ModelsTab
    // (onSettingsChanged re-reads it), like the other Models toggles here.
    mdlChk(QStringLiteral("models/autoLoad"),
        QStringLiteral("Auto-load the selected model"), true,
        QStringLiteral("Load and render a model as soon as you select it. Off = selecting only "
                       "shows its data; double-click a row to load it on demand (useful when "
                       "browsing or multi-selecting for export)."));
    mdlChk(QStringLiteral("models/baseColorOnly"),
        QStringLiteral("Base colour only (faster loads)"), false,
        QStringLiteral("Decode ONE texture per material — base colour — instead of up to ten "
                       "(normal, ORM, emissive, detail normal/roughness, translucency, mask, "
                       "dye mask and ramp).\n\n"
                       "Models load substantially faster and use far less memory. The preview "
                       "renders flat-lit: no surface detail, glow or dye tinting.\n\n"
                       "Models tab only — it does not affect the Wardrobe or Stable previews, "
                       "or anything you export."));
    mdlChk(QStringLiteral("models/rememberPanels"),
        QStringLiteral("Remember the right-hand panel layout"), true,
        QStringLiteral("Restore which panels (Info, Parts, Animations…) are open, their order and "
                       "their heights when the tool re-opens. Off = start with Info + Parts."));
    m_mdlHover        = mdlChk(QStringLiteral("models/hoverPreview"), QStringLiteral("Show a preview pop-up on hover"), true, QString());
    m_mdlRememberLast = mdlChk(QStringLiteral("models/rememberLast"), QStringLiteral("Remember last selected model"), false, QString());
    // (No "remember preview settings" checkbox — the viewport toggles always persist; a switch
    //  that nothing read was just noise.)
    mdlChk(QStringLiteral("models/autoRender3D"),
        QStringLiteral("Auto-render 3D icons for rows in view (may crash some GPUs)"), false,
        QStringLiteral("In the 3D / Original+3D icon modes, automatically render thumbnails for the "
                       "list rows scrolled into view. This drives a burst of offscreen GPU renders — "
                       "which crashes unstable drivers — so it's OFF by default. When off, 3D icons "
                       "still show cached/persisted thumbnails and fill in as you view models "
                       "(right-click ▸ Render icon renders on demand)."));
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

    auto* wardrobe2 = new QGroupBox(QStringLiteral("Wardrobe"), this);
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
        auto* exBox = new QGroupBox(QStringLiteral("Textures"), this);
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

        exChk(fx, QStringLiteral("export/includeTex"), QStringLiteral("Include textures in the model .glb"), true,
              QStringLiteral("Embed the material textures in exported .glb models. Same as the Models tab's \"Tex\" checkbox."));
        exChk(fx, QStringLiteral("export/bakeDye"), QStringLiteral("Bake applied dye into textures"), false,
              QStringLiteral("Export a dyed outfit with the dye baked into the base-colour texture (matches the "
                             "preview). Off = the undyed base look."));
        exChk(fx, QStringLiteral("export/bakeDetail"),
              QStringLiteral("Bake surface detail into normal / roughness maps"), false,
              QStringLiteral("Bake the tiled leather/fabric/metal detail maps into the exported normal + roughness "
                             "so the model keeps its surface grain in Blender. Slower export."));

        exportTab->addWidget(exBox);

        // ── File-name templates (d4analyzer-style) ──
        // Placeholders: {{FileName}} {{SNO}} and, for texframes, {{FrameIdx}} {{FrameName}}.
        // Applied to every export path (single, batch, bulk) — illegal filename chars sanitized.
        {
            auto* nmBox = new QGroupBox(QStringLiteral("File names (templates)"), this);
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
            exportTab->addWidget(nmBox);
        }

        // ── Model (.glb) ──
        auto* mdlBox = new QGroupBox(QStringLiteral("Model export (.glb)"), this);
        auto* fmd = new QFormLayout(mdlBox);

        exChk(fmd, QStringLiteral("export/includeAnim"), QStringLiteral("Include animation"), false,
              QStringLiteral("Embed animation clips in exported .glb models. Same as the Models tab's \"Anim\" checkbox."));

        auto* animScope = new QComboBox(mdlBox);
        animScope->addItems({QStringLiteral("Only the clip playing in preview"), QStringLiteral("All of the model's animations")});
        animScope->setCurrentIndex(qBound(0, QSettings().value(QStringLiteral("export/animScope"), 0).toInt(), 1));
        animScope->setToolTip(QStringLiteral("Which animations to embed when \"Anim\" is on. \"All\" decodes every clip (larger files)."));
        QObject::connect(animScope, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
            QSettings().setValue(QStringLiteral("export/animScope"), i); });
        m_exportResetActions.push_back([animScope] { animScope->setCurrentIndex(0); });
        fmd->addRow(QStringLiteral("Animations to embed:"), animScope);

        exChk(fmd, QStringLiteral("export/includePulledAnims"), QStringLiteral("Include pulled animations"), false,
              QStringLiteral("Also embed clips you manually pulled from another model (the gold rows in the "
                             "ANIMATIONS panel, retargeted to this model's skeleton) — not just the model's own clips."));

        exChk(fmd, QStringLiteral("export/withDeps"), QStringLiteral("Also export raw source files (.app + .tex)"), false,
              QStringLiteral("Alongside each exported .glb, write the raw game files it came from into a \"<name>_deps\" subfolder."));

        auto* exNote = new QLabel(QStringLiteral(
            "<i>\"All of the model's animations\" can make large .glb files and takes longer (every clip is decoded).</i>"), mdlBox);
        exNote->setWordWrap(true);
        exNote->setStyleSheet(QStringLiteral("color:#888;"));
        fmd->addRow(exNote);

        exportTab->addWidget(mdlBox);

        // ── Image & GIF capture ─────────────────────────────────────────
        // Screenshot / turntable-GIF output settings (separate from the model .glb export).
        auto* gifBox = new QGroupBox(QStringLiteral("Image && GIF capture"), this);
        auto* gf = new QFormLayout(gifBox);

        // ── Still image ──────────────────────────────────────────────
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

        // ── Quality / file-size knobs ──
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
        exChk(gf, QStringLiteral("export/transparentBg"),
            QStringLiteral("Transparent background (captures)"), false,
            QStringLiteral("Preview image / GIF captures render the model on a transparent background.\n"
                           "PNG gets true alpha; GIF uses a 1-bit cutout (hard edges). JPEG ignores this."));

        exportTab->addWidget(gifBox);

        // ── Retarget & modding ──────────────────────────────────────────
        // Options for porting extracted models onto OTHER games' rigs (weight transfer,
        // re-skinning, socket alignment). All of these only affect .glb exports.
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
        exChk(rt, QStringLiteral("retarget/fitReference"),
              QStringLiteral("Include base body for fit reference (armor)"), false,
              QStringLiteral("Append the matching base body mesh (barM_base00 / barF_base00 / …, resolved from "
                             "the piece's name prefix) under exported player armor, as a separate "
                             "\"__fitReference\" material. Use it in Blender to check clipping/proportions and "
                             "as a weight-transfer source; delete before final export."));
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

        // Progressive disclosure: keep the deep modding/retarget options collapsed behind an
        // "Advanced" header so the everyday Export view stays simple. State is remembered.
        const bool advOpen = QSettings().value(QStringLiteral("ui/exportAdvancedOpen"), false).toBool();
        auto* rtToggle = new QToolButton(this);
        rtToggle->setCheckable(true);
        rtToggle->setChecked(advOpen);
        rtToggle->setAutoRaise(true);
        rtToggle->setToolTip(QStringLiteral("Model-porting options for Blender / other game engines"));
        rtToggle->setStyleSheet(QStringLiteral("QToolButton{color:#ccc;font-weight:bold;border:none;padding:4px 2px;}"));
        auto setRtHeader = [rtToggle](bool open) {
            rtToggle->setText((open ? QStringLiteral("▾  ") : QStringLiteral("▸  "))
                              + QStringLiteral("Advanced — modding / retarget"));
        };
        setRtHeader(advOpen);
        rtBox->setTitle(QString());        // the toggle is the section header now
        rtBox->setVisible(advOpen);
        QObject::connect(rtToggle, &QToolButton::toggled, this, [rtBox, setRtHeader](bool on) {
            rtBox->setVisible(on);
            setRtHeader(on);
            QSettings().setValue(QStringLiteral("ui/exportAdvancedOpen"), on);
        });
        exportTab->addWidget(rtToggle);
        exportTab->addWidget(rtBox);
    }

    // ── Category: Cache ────────────────────────────────────────────────
    // The app caches the metadata crawl and the icon-atlas index to AppData so
    // startup is instant; these buttons clear them to force a rebuild (e.g. after
    // a d4data update or to pick up icon-mapping fixes).
    auto* cache = new QGroupBox(QStringLiteral("Caches & reset"), this);
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
    maintTab->addWidget(cache);

    // Keep categories top-aligned within each page.
    // ── Category: Hotkeys ──────────────────────────────────────────────
    // Rebindable keyboard shortcuts for the Export menu's commands. Each edit writes its
    // QSettings key live; MainWindow re-reads them (settingsChanged) and re-applies the
    // shortcuts without a restart. Clearing an edit unbinds that command.
    {
        auto* hkBox = new QGroupBox(QStringLiteral("Export & preview shortcuts"), this);
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
    auto* devBox = new QGroupBox(QStringLiteral("Dev"), this);
    auto* dvl = new QVBoxLayout(devBox);
    m_includeLocale = new QCheckBox(QStringLiteral("Index locale packs (more file names, slower)"), devBox);
    m_includeLocale->setToolTip(QStringLiteral(
        "Also index the locale text/speech/cutscene/video packs. Adds ~2.6M localized names but "
        "slows indexing and isn't needed for models/textures. Off by default; applied on OK "
        "(re-index or restart to take effect).\n"
        "See casc_coverage.txt (next to the exe) for a breakdown of what the archive contains."));
    dvl->addWidget(m_includeLocale);
    generalTab->addWidget(devBox);

    generalTab->addStretch(1);
    modelsTab->addStretch(1);
    wardrobeTab->addStretch(1);
    exportTab->addStretch(1);
    hotkeysTab->addStretch(1);
    maintTab->addStretch(1);
    root->addWidget(tabs);

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
    m_w2NudeBase->setChecked(QSettings().value(QStringLiteral("wardrobe2/nudeBase"), false).toBool());
    {
        QSettings s;
        m_w2SecEnsembles->setChecked(s.value(QStringLiteral("wardrobe2/viewport/ensembles"), true).toBool());
        m_w2RememberPanels->setChecked(s.value(QStringLiteral("wardrobe2/rememberPanels"), true).toBool());
        m_w2ShowLog->setChecked(s.value(QStringLiteral("wardrobe2/dbg/log"), false).toBool());
    }
    updateChecks();
    updateVersion();
    snapshotLiveSettings();   // remember current live values so Cancel can revert them
}

// The set of keys that persist the instant a widget changes (Models / Wardrobe panels / Performance
// / Export). Cancel restores these to their open-time values; paths/product/View persist on OK and
// are already Cancel-safe, so they're not snapshotted here.
static QStringList liveSettingKeys()
{
    return {
        QStringLiteral("models/hoverPreview"), QStringLiteral("models/rememberLast"),
        QStringLiteral("models/autoLoad"),
        QStringLiteral("models/rememberPanels"), QStringLiteral("models/fillSkin"),
        QStringLiteral("models/autoRender3D"), QStringLiteral("models/clothSim"),
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
        QStringLiteral("export/includeAnim"), QStringLiteral("export/animScope"),
        QStringLiteral("export/includePulledAnims"),
        QStringLiteral("export/withDeps"), QStringLiteral("export/boneNamesTranslated"),
        QStringLiteral("export/hardpointEmpties"),
        QStringLiteral("export/blenderFriendly"), QStringLiteral("export/xMirror"),
        QStringLiteral("retarget/enginePreset"), QStringLiteral("retarget/unitScale"),
        QStringLiteral("retarget/remapWeights"), QStringLiteral("retarget/collapseCloth"),
        QStringLiteral("retarget/fitReference"), QStringLiteral("retarget/setManifest"),
        QStringLiteral("hotkeys/exportSelection"), QStringLiteral("hotkeys/exportToLast"),
        QStringLiteral("hotkeys/exportAnimations"), QStringLiteral("hotkeys/saveImage"),
        QStringLiteral("hotkeys/turntable"), QStringLiteral("hotkeys/animLoop"),
    };
}

void SettingsDialog::snapshotLiveSettings()
{
    QSettings s;
    m_snapshot.clear();
    for (const QString& k : liveSettingKeys())
        m_snapshot.insert(k, s.value(k));   // invalid QVariant if the key was unset
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
    m_version->setText(QStringLiteral("Diablo4AssetBrowserNative  v%1")
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

                if (same) {
                    QFile::remove(tmp);   // identical — leave the user's file untouched
                    QMessageBox::information(this, QStringLiteral("TACT keys"),
                        QStringLiteral("Already up to date — your copy matches the server."));
                } else {
                    if (existed) QFile::remove(dest);
                    QFile::rename(tmp, dest);
                    const QFileInfo fi(dest);
                    QMessageBox::information(this, QStringLiteral("TACT keys"),
                        QStringLiteral("%1 — %2 bytes\n%3")
                            .arg(existed ? QStringLiteral("Updated") : QStringLiteral("Downloaded"))
                            .arg(fi.size()).arg(dest));
                }
                updateChecks();   // restores the Update/Download label
            });
    *attempt = [proc, curl, argsFor](int idx) {
        proc->setProperty("urlIdx", idx);
        proc->start(curl, argsFor(idx));
    };
    (*attempt)(0);
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
