#include "index/IconAudit.h"

#include "app/AppPaths.h"
#include "casc/CascReader.h"
#include "index/AppearanceMeta.h"
#include "index/DadOverride.h"
#include "index/IconIndex.h"
#include "index/ItemDef.h"
// The Catalogue's own contents descent. Comment on its own line (verify-src matches the directive
// to end of line), and the whole point of the move: this section reports what the TAB will draw.
#include "index/ProductContents.h"
#include "index/SnoIndex.h"
#include "index/StoreProductIndex.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <exception>

namespace {

constexpr int kGroupActor = 1;
constexpr int kGroupAppearance = 9;

// ── Catalogue icon coverage ─────────────────────────────────────────────────────────────────────
//
// The audit above proves the APPEARANCE handles resolve (10,769 ok / 5 no-sprite on a current
// build). That is only the first of the three routes the Catalogue's item strip tries per row:
//
//   1. the appearance's own icon      AppearanceMeta::iconFor -> IconIndex
//   2. the PRODUCT's art handles      Product::art / cardArt   -> IconIndex      <- measured here
//   3. the payload actor's portrait   portraitFor              -> IconIndex
//
// 42% of store products resolve to no appearance at all, so for those the strip is showing route 2
// or nothing. A blank row cannot say which route was tried or why it failed, and every cause looks
// identical on screen. This section classifies every product art handle so the report can.
//
// THREAD SAFETY — the constraint that shapes this whole function. IconAudit::run is called from a
// worker (MainWindow::autoIconAudit), and IconIndex::iconImage() touches m_atlasCache, which its
// own header documents as GUI-thread-only. So nothing here may call iconImage(). has() and
// atlasFor() are pure lookups into the frame table, immutable once ready(), and CascReader is
// documented safe for concurrent reads — those three are the entire toolkit used below.
//
// The consequence is honest rather than hidden: a handle that is indexed AND whose atlas payload
// reads is reported as "should render". A decode failure or a degenerate UV rect would still blank
// it, and IconIndex::iconImage already warns once per atlas with the reason, so the app log is
// where those surface.
// The payload actor's portrait handle, or 0. A text scan rather than a parse, exactly as
// CatalogueTab::portraitFor does it - an actor file runs to thousands of lines and the field is a
// plain integer. Kept byte-identical in behaviour to the tab's copy, because this section exists
// to report what the tab will draw.
quint32 portraitOf(const QString& d4dataDir, const QString& payloadName)
{
    if (payloadName.isEmpty()) return 0;
    QFile f(d4dataDir + QStringLiteral("/json/base/meta/Actor/") + payloadName
            + QStringLiteral(".acr.json"));
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QByteArray t = f.readAll();
    const int k = t.indexOf("\"hPortraitImage\":");
    if (k < 0) return 0;
    int i = k + 17;
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
    int e = i;
    while (e < t.size() && t[e] >= '0' && t[e] <= '9') ++e;
    return e > i ? t.mid(i, e - i).toUInt() : 0;
}

QString catalogueIconSection(const QString& d4dataDir, const SnoIndex* index, CascReader* reader)
{
    const IconIndex& ii = IconIndex::instance();
    const StoreProductIndex& sp = StoreProductIndex::instance();
    if (!ii.ready())
        return QStringLiteral(
            "CATALOGUE ICON COVERAGE — skipped: the icon index was not ready.\n"
            "Re-run from File > Icon audit once the tab has finished loading.\n");
    if (!sp.ready())
        return QStringLiteral(
            "CATALOGUE ICON COVERAGE — skipped: the store-product index was not ready.\n"
            "It builds later than the appearance index, so the automatic run at startup usually\n"
            "misses it. Open the Catalogue tab, let it settle, then File > Icon audit.\n");

    // Every product exactly once: bundles, their children, the loose ones and the locked ones.
    QSet<int> snos;
    for (int s : sp.bundles()) {
        snos.insert(s);
        if (const auto* p = sp.product(s))
            for (int k : p->children) snos.insert(k);
    }
    for (int s : sp.loose())  snos.insert(s);
    for (int s : sp.locked()) snos.insert(s);

    // SORTED, because a QSet iterates in hash order and Qt 6 randomises the hash seed per process.
    // This report exists to be diffed against the previous run; an unstable row order would make
    // every diff noise, and it would also shuffle which entries land in the capped sample below.
    QVector<int> ordered(snos.begin(), snos.end());
    std::sort(ordered.begin(), ordered.end());

    // Can this atlas be read? Answered from the INDEX and the encryption manifest, never by
    // reading the atlas.
    //
    // The first cut here called readPayloadBySno() for a yes/no, which fully BLTE-decompresses a
    // 1024x1024 texture and throws the bytes away — a few hundred distinct icon atlases is
    // hundreds of megabytes of decompression added to a startup worker. payloadVariants() is an
    // index lookup, and encryptedSnos() is one manifest the reader already builds for the
    // "only encrypted" filters; both are mutex-guarded and documented for concurrent callers.
    //
    // Cached per ATLAS rather than per handle: hundreds of handles share one.
    //
    // Named for the QUESTION it answers rather than for atlases: the grid-thumbnail section below
    // asks it about shop-art textures, and an icon atlas and a shop card are both group-44
    // textures, so one cache serves both and a texture shared between the two is probed once.
    QHash<int, bool> payloadReadable;
    auto payloadOk = [&](int texSno) {
        if (!reader || !reader->isReady()) return true;   // unknown; do not report it as locked
        const auto it = payloadReadable.constFind(texSno);
        if (it != payloadReadable.constEnd()) return it.value();
        bool ok = reader->payloadVariants(quint64(texSno)).payload > 0;
        if (ok) {
            // Present in the index is not the same as readable: a container locked under a TACT
            // key we do not hold expands to nothing at decode time.
            const QByteArray key = reader->encryptedSnos().value(texSno);
            if (!key.isEmpty() && !reader->haveTactKey(key)) ok = false;
        }
        payloadReadable.insert(texSno, ok);
        return ok;
    };

    struct Tally { int products = 0, withArt = 0, handles = 0, okH = 0, notIndexed = 0, locked = 0; };
    Tally all;
    QHash<QString, Tally> bySeason;
    QHash<int, int>       deadByAtlas;     // atlas sno -> handles on it whose payload will not read
    QStringList samples;

    for (int s : ordered) {
        const auto* p = sp.product(s);
        if (!p) continue;
        const QString season = p->seasonName.isEmpty()
                                   ? (p->season ? QStringLiteral("season %1").arg(p->season)
                                                : QStringLiteral("(no season)"))
                                   : p->seasonName;
        Tally& t = bySeason[season];
        ++all.products; ++t.products;

        QVector<quint32> handles = p->art;
        handles += p->cardArt;
        // Counted on a real handle, not on a non-empty list: a product whose art is {0} carries no
        // art, and counting it would overstate the "carry art" column against the handle column.
        bool counted = false;
        for (quint32 h : handles) {
            if (!h) continue;
            if (!counted) { counted = true; ++all.withArt; ++t.withArt; }
            ++all.handles; ++t.handles;
            // has(), not atlasFor() != 0. A frame whose atlasSno is genuinely 0 would otherwise be
            // reported as "never indexed", which is the opposite diagnosis — and the whole value of
            // this section is not lying about the cause.
            const int atlas = ii.atlasFor(h);
            if (!ii.has(h)) {
                ++all.notIndexed; ++t.notIndexed;
                if (samples.size() < 40)
                    samples << QStringLiteral("NOT-INDEXED  handle=%1  %2  [%3]  %4")
                                   .arg(h).arg(p->name, season, p->title);
            } else if (!payloadOk(atlas)) {
                ++all.locked; ++t.locked; ++deadByAtlas[atlas];
                if (samples.size() < 40)
                    samples << QStringLiteral("ATLAS-LOCKED handle=%1 atlas=%2  %3  [%4]  %5")
                                   .arg(h).arg(atlas).arg(p->name, season, p->title);
            } else {
                ++all.okH; ++t.okH;
            }
        }
    }

    QStringList out;
    out << QStringLiteral("CATALOGUE ICON COVERAGE")
        << QStringLiteral("=======================")
        << QString()
        << QStringLiteral("Product ART handles only - route 2 of the strip's three. The appearance")
        << QStringLiteral("route is the audit above; the portrait route is a last resort for")
        << QStringLiteral("headstones. NOT-INDEXED means IconIndex has no frame for the handle, so")
        << QStringLiteral("its atlas was never scanned - the expected shape of a post-patch gap.")
        << QStringLiteral("ATLAS-LOCKED means the frame exists but the atlas payload does not read,")
        << QStringLiteral("which is a TACT key we do not hold. Anything else is reported as")
        << QStringLiteral("renderable; a decode or UV fault would still blank it and IconIndex logs")
        << QStringLiteral("one warning per atlas when that happens, so check the app log too.")
        << QString()
        << QStringLiteral("%1 products  -  %2 carry art  -  %3 handles: %4 renderable, "
                          "%5 NOT-INDEXED, %6 ATLAS-LOCKED")
               .arg(all.products).arg(all.withArt).arg(all.handles)
               .arg(all.okH).arg(all.notIndexed).arg(all.locked)
        << QString()
        << QStringLiteral("BY SEASON")
        << QStringLiteral("  %1 %2 %3 %4 %5 %6 %7")
               .arg(QStringLiteral("season"), -22)
               .arg(QStringLiteral("products"), 9)
               .arg(QStringLiteral("art"), 6)
               .arg(QStringLiteral("handles"), 8)
               .arg(QStringLiteral("renders"), 8)
               .arg(QStringLiteral("NOT-IDX"), 8)
               .arg(QStringLiteral("LOCKED"), 8);
    QStringList seasons = bySeason.keys();
    std::sort(seasons.begin(), seasons.end());
    for (const QString& k : seasons) {
        const Tally& t = bySeason[k];
        out << QStringLiteral("  %1 %2 %3 %4 %5 %6 %7")
                   .arg(k, -22)
                   .arg(t.products, 9).arg(t.withArt, 6).arg(t.handles, 8)
                   .arg(t.okH, 8).arg(t.notIndexed, 8).arg(t.locked, 8);
    }

    if (!deadByAtlas.isEmpty()) {
        out << QString() << QStringLiteral("ATLASES THAT WILL NOT READ (handles affected)");
        QVector<QPair<int, int>> rows;
        for (auto i = deadByAtlas.constBegin(); i != deadByAtlas.constEnd(); ++i)
            rows.push_back({i.value(), i.key()});
        // Total order: equal counts fall back to the atlas sno, so the ranking is stable between
        // runs rather than depending on QHash iteration.
        std::sort(rows.begin(), rows.end(),
                  [](const QPair<int, int>& a, const QPair<int, int>& b) {
                      return a.first != b.first ? a.first > b.first : a.second < b.second;
                  });
        for (const auto& r : rows)
            out << QStringLiteral("  %1 handles  atlas sno %2").arg(r.first, 6).arg(r.second);
    }

    if (!samples.isEmpty()) {
        // Naming the total matters: "first 40" of 41 and "first 40" of 4,000 are very different
        // reports, and the capped one used to read identically to the complete one.
        const int bad = all.notIndexed + all.locked;
        out << QString()
            << (bad > samples.size()
                    ? QStringLiteral("SAMPLE (%1 of %2 failing handles)").arg(samples.size()).arg(bad)
                    : QStringLiteral("ALL %1 FAILING HANDLES").arg(bad));
        for (const QString& s : samples) out << QStringLiteral("  ") + s;
    }

    // ── GRID THUMBNAILS: the rows you actually scroll ────────────────────────────────────────────
    //
    // Everything above measures the item STRIP, which resolves per appearance through IconIndex.
    // The main list does something entirely different: CatalogueTab::cardImage() and then
    // heroImage() look a group-44 texture up BY NAME, from templates built off the product's own
    // SNO name. Neither route stands in for the other, so a renderable handle above says nothing
    // about whether the tile in the grid is blank.
    //
    // THE ROW UNIVERSE IS bundles + loose + locked. NOT children: a child product is reachable
    // only by opening its bundle, so measuring the strip's ~9,400 here would flatter the numbers
    // with thousands of rows nobody scrolls past.
    //
    // WHY TWO CANDIDATE LISTS. CatalogueTab holds two independent definitions of where shop art
    // lives, and they are not the same set:
    //
    //   uiArtCandidates()          12 names  ->  shopTextures(), the detail pane, "Has icon"
    //   cardImage() + heroImage()  10 names  ->  the THUMBNAIL in the list
    //
    // The filter's list is the larger one, so a product only IT can find passes "Has icon", takes
    // a row in the list, and still draws blank. That is the exact shape of "lots of icons are
    // missing while I scroll", so it is measured here rather than argued about: FILTER-ONLY is the
    // size of that gap, and TEMPLATES THE THUMBNAIL ROUTE MISSES names the templates responsible.
    //
    // The two lists below are COPIES of the tab's. The copy is the whole point - this section
    // exists to measure the divergence - but if either list in CatalogueTab.cpp grows a template,
    // the matching one here has to grow with it or the report understates coverage.
    if (!index) {
        out << QString()
            << QStringLiteral("GRID THUMBNAILS - skipped: the audit was called without an SNO "
                              "index, so group-44 names cannot be resolved.")
            << QString();
        return out.join(QLatin1Char('\n'));
    }

    // Lowercased exactly as CatalogueTab::ensureNameMaps keys it, from the same group.
    QHash<QString, int> texByName;
    {
        const int gTex = SnoIndex::groupIdByName(QStringLiteral("Texture"), 44);
        const QVector<SnoEntry>& texs = index->entries(gTex);
        texByName.reserve(texs.size());
        for (const SnoEntry& e : texs) texByName.insert(e.name.toLower(), e.snoId);
    }

    // Product name -> the stem the art templates are built on. Bundle_ is dropped because the
    // templates re-add it; a loose product never carries it in the first place.
    auto bareOf = [](const QString& productName) {
        QString bare = productName;
        if (bare.startsWith(QLatin1String("Bundle_"), Qt::CaseInsensitive)) bare = bare.mid(7);
        return bare;
    };
    // cardImage()'s four, then heroImage()'s six, in the order renderVisibleThumbs tries them.
    auto thumbCandidates = [](const QString& bare) {
        QStringList c;
        c << QStringLiteral("2DUI_Bundle_") + bare
          << QStringLiteral("2DInventory_Bundle_") + bare
          << QStringLiteral("2DInventory_") + bare
          << QStringLiteral("2DUI_") + bare;
        for (const char* sfx : { "_background", "_WebImage", "_details" })
            c << QStringLiteral("2DUI_Bundle_") + bare + QString::fromLatin1(sfx);
        for (const char* sfx : { "_background", "_WebImage", "_details" })
            c << QStringLiteral("2DUI_") + bare + QString::fromLatin1(sfx);
        return c;
    };
    // uiArtCandidates(), verbatim - including the _icons suffix on both stems, which is what the
    // thumbnail list above does not have.
    auto filterCandidates = [](const QString& bare) {
        static const char* const kSfx[] = { "", "_details", "_background", "_WebImage", "_icons" };
        QStringList c;
        for (const char* sfx : kSfx)
            c << QStringLiteral("2DUI_Bundle_") + bare + QString::fromLatin1(sfx);
        c << QStringLiteral("2DInventory_Bundle_") + bare;
        for (const char* sfx : kSfx)
            c << QStringLiteral("2DUI_") + bare + QString::fromLatin1(sfx);
        c << QStringLiteral("2DInventory_") + bare;
        return c;
    };
    // ── The structural scan, as CatalogueTab::ensureDerivedArt does it ───────────────────────────
    // The tab does not rely on the template list alone any more, and neither can this section: it
    // also finds art whose name CONTAINS the product's as a whole token span, preceded by a
    // prefix, on a texture the icon index treats as an atlas. Without this the audit reports fewer
    // rendering rows than the app actually draws, and the next person to read it sees a regression
    // that is not there. Measured: 948 by template, 957 with the scan.
    // Keyed to the ATLAS SNO, not to a bool: the rest of this section separates RENDERS from
    // TEX-LOCKED with payloadOk(), and a scan that threw the sno away could not be asked. A product
    // whose only derived art sits on a locked atlas would then be counted as rendering while the
    // tab draws it blank - inflating the one number this section exists to keep honest.
    QHash<QString, int> derivedArt;   // lowercased product stem -> first UI atlas naming it
    {
        QSet<QString> stems;
        for (int s : ordered)
            if (const auto* p = sp.product(s)) {
                const QString bare = bareOf(p->name);
                if (bare.contains(QLatin1Char('_'))) stems.insert(bare.toLower());
            }
        if (!stems.isEmpty()) {
            const int gTex = SnoIndex::groupIdByName(QStringLiteral("Texture"), 44);
            for (const SnoEntry& e : index->entries(gTex)) {
                if (!ii.isAtlas(e.snoId)) continue;
                const QStringList tk = e.name.toLower().split(QLatin1Char('_'), Qt::SkipEmptyParts);
                for (int i = 1; i < tk.size(); ++i) {     // from 1: the span must have a prefix
                    QString span = tk.at(i);
                    for (int j = i + 1; j <= tk.size(); ++j) {
                        // First wins, matching the tab's shortest-name-first preference closely
                        // enough for a coverage count; this asks "is there any", not "which".
                        if (stems.contains(span) && !derivedArt.contains(span))
                            derivedArt.insert(span, e.snoId);
                        if (j == tk.size()) break;
                        span += QLatin1Char('_') + tk.at(j);
                    }
                }
            }
        }
    }
    auto derivedHit = [&](const QString& productName) {
        const auto it = derivedArt.constFind(bareOf(productName).toLower());
        // payloadOk, exactly as probe() applies it to a template hit. Named art that will not
        // decrypt is TEX-LOCKED, not RENDERS, whichever route found it.
        return it != derivedArt.constEnd() && payloadOk(it.value());
    };

    // Existence and readability are answered separately so "the name is not in the game" and "the
    // texture is TACT-locked" cannot be reported as the same failure. hitName receives the
    // candidate that resolved, which is what makes the template histogram possible.
    auto probe = [&](const QStringList& cands, bool* anyName, QString* hitName) {
        bool ok = false;
        for (const QString& c : cands) {
            const auto it = texByName.constFind(c.toLower());
            if (it == texByName.constEnd()) continue;
            if (anyName) *anyName = true;
            if (!payloadOk(it.value())) continue;
            if (hitName && hitName->isEmpty()) *hitName = c;
            ok = true;
            break;
        }
        return ok;
    };

    // Same three lists refresh() concatenates to build the list, in the same order. Deduplicated
    // because a product appearing twice would be counted twice; sorted for a stable diff.
    QSet<int> rowSet;
    for (int s : sp.bundles()) rowSet.insert(s);
    for (int s : sp.loose())   rowSet.insert(s);
    for (int s : sp.locked())  rowSet.insert(s);
    QVector<int> rows(rowSet.begin(), rowSet.end());
    std::sort(rows.begin(), rows.end());

    struct GTally { int rows = 0, renders = 0, filterOnly = 0, locked = 0, noArt = 0, handle = 0; };
    GTally gAll;
    QHash<QString, GTally> gBySeason;
    QHash<QString, int>    missedTemplate;   // "2DUI_Bundle_*_icons" -> rows only it could find
    QStringList gFilterSamples, gBlankSamples;
    int scanOnly = 0;   // rows only the structural scan can name
    // ── The fourth route, measured ───────────────────────────────────────────────────────────────
    // The tab tries the payload actor's own portrait after the other three fail, and nothing has
    // ever counted it - so "no art handles at all" was reported as a final figure when it was an
    // upper bound. These two say how much of that bucket the tab actually fills.
    //
    // Asked of the row's leaf CONTENTS, via ProductContents, because that is what the tab asks -
    // portraitIconHandle walks contentSnos and takes each child's payload actor. Reading the ROW's
    // own payloadName instead (what this did first) is empty for every bundle by construction: a
    // bundle has no payload, its children do. That is why this line reported 0 for a route the
    // Catalogue visibly fills, and why the descent now lives in a header both files include rather
    // than in a second copy here.
    //
    // Two buckets it still cannot answer, kept apart because they mean different things:
    //   LOCKED   - an encrypted record. Unknowable to anyone without the TACT key, ever.
    //   NO NAME  - every leaf carries only a payload SNO. Resolving it means SnoIndex::nameForSno,
    //              which writes a mutable lazy cache and is NOT safe from this worker thread, so
    //              the audit under-reports here by design. The tab, on the GUI thread, resolves it.
    int portraitFills = 0, portraitLocked = 0, portraitNoName = 0;
    QHash<QString, quint32> portraitCache;   // payload name -> handle (0 included: do not re-open)

    // ── The THIRD route, which this section never measured ──────────────────────────────────────
    // The tab tries four things per row, in this order: art named after the product, the product's
    // own art handle, the icon of what it CONTAINS, then the payload actor's portrait. Route 3 was
    // absent here, so the portrait was being asked about a SUPERSET of the rows it actually
    // rescues - every row route 3 already covers was counted as still blank - and the figure could
    // not be read either way. Measuring it puts the four routes in the tab's own order.
    //
    // Resolution mirrors CatalogueTab::contentIconHandle: leaf -> payload name -> candidate
    // appearance names -> AppearanceMeta::iconFor -> IconIndex::has. Both naming rules, because
    // both is what the tab uses: the armour convention (cosmeticAppearanceNames) and the item's
    // own name (withSelfName), the second being the one every weapon, mount and trophy needs.
    int contentFills = 0;
    QHash<QString, quint32> contentCache;   // payload name -> handle (0 cached as a real answer)
    QHash<QString, int> appByName;          // lowercased appearance name -> sno
    for (const SnoEntry& e : index->entries(kGroupAppearance))
        appByName.insert(e.name.toLower(), e.snoId);
    const AppearanceMeta& amRef = AppearanceMeta::instance();
    auto contentIconOf = [&](const QString& payloadName) -> quint32 {
        if (payloadName.isEmpty()) return 0;
        const auto c = contentCache.constFind(payloadName);
        if (c != contentCache.constEnd()) return c.value();
        QStringList cand = AppearanceMeta::cosmeticAppearanceNames(payloadName);
        if (cand.isEmpty()) cand = AppearanceMeta::withSelfName(QStringList(), payloadName);
        quint32 found = 0;
        for (const QString& nm : cand) {
            const auto a = appByName.constFind(nm.toLower());
            if (a == appByName.constEnd()) continue;
            const quint32 h = amRef.iconFor(a.value());
            if (h && ii.has(h)) { found = h; break; }
        }
        contentCache.insert(payloadName, found);
        return found;
    };

    // ── Is the handle fallback worth having? ─────────────────────────────────────────────────────
    //
    // The HANDLE column says a non-rendering row HAS a renderable art handle. It does NOT say the
    // handle is that product's own art, and that distinction decides whether wiring the grid to it
    // is a fix or a regression. Product::art is built from twelve named fields in a fixed order
    // with zeros skipped, and the index does not record which field a surviving handle came from -
    // so "the first renderable handle" may well be hCategoryIcon, a generic per-class glyph shared
    // by every product of that type. The item strip hit exactly this, and its comment records the
    // symptom: a row of identical class symbols instead of the actual pieces.
    //
    // Seventy Season 15 rows all showing the same sorcerer glyph is worse than seventy blanks,
    // because it looks like real data. So the handles are counted before anything is wired to them:
    // if the rows that would newly render resolve to roughly as many DISTINCT handles, the art is
    // per-product and the naive fallback is sound. If they collapse onto a handful, the fallback
    // first needs the art handles tagged by the field they came from - a change to
    // StoreProductIndex and its cache version, not a two-line fallback in the tab.
    // THREE POLICIES, MEASURED SIDE BY SIDE rather than one chosen in advance. The first run here
    // took "the first renderable handle in art, then cardArt" and scored 0.18 distinct handles per
    // row - one picture stamped across as many as 99 rows. That is a property of the FIELD, not of
    // the products: art is twelve fields flattened in a fixed order, and its head is dominated by
    // hSplashImage / hCategoryIcon, which a whole promo family shares by design.
    //
    // cardArt is a different field entirely - arCardArtVariants, {hCardImage, hCardHoverImage} -
    // and a card image is exactly what a grid tile is. It was invisible in the first run because
    // the probe concatenated art + cardArt and art always won. Measuring the two separately is the
    // difference between "the index needs a new field-tagged cache" and "the right field was
    // already on the product".
    struct Policy {
        QString name;
        QHash<quint32, int>           use;           // handle -> rows that would pick it
        QHash<QString, QSet<quint32>> distinctBySeason;
        QHash<quint32, QString>       example;       // handle -> one product that picks it
        int rows = 0;
        int fromCasc = 0;   // recovered from the binary, where the art offsets are a histogram
    };
    Policy pCard{ QStringLiteral("cardArt only  (arCardArtVariants - the shop's own card image)") };
    Policy pArt { QStringLiteral("art only      (twelve flattened fields, first non-zero)") };
    Policy pBoth{ QStringLiteral("cardArt, then art  (cardArt preferred, art as the fallback)") };
    auto record = [](Policy& pol, quint32 h, const QString& season, const QString& product,
                     bool fromCasc) {
        ++pol.rows;
        ++pol.use[h];
        pol.distinctBySeason[season].insert(h);
        if (!pol.example.contains(h)) pol.example.insert(h, product);
        if (fromCasc) ++pol.fromCasc;
    };

    for (int s : rows) {
        const auto* p = sp.product(s);
        if (!p) continue;
        const QString season = p->seasonName.isEmpty()
                                   ? (p->season ? QStringLiteral("season %1").arg(p->season)
                                                : QStringLiteral("(no season)"))
                                   : p->seasonName;
        GTally& t = gBySeason[season];
        ++gAll.rows; ++t.rows;

        const QString bare = bareOf(p->name);
        bool thumbName = false, filtName = false;
        QString filtHit;   // only the filter's hit is reported; the thumbnail's is never named
        bool thumbOk = probe(thumbCandidates(bare), &thumbName, nullptr);
        // The scan, exactly where the tab applies it: after the templates, never instead of them.
        if (!thumbOk && derivedHit(p->name)) { thumbOk = true; ++scanOnly; }
        const bool filtOk  = probe(filterCandidates(bare), &filtName, &filtHit);

        // Does the AUTHORED art handle render? Tracked for every row that does not already draw,
        // because it is the fallback the grid could use and currently does not - the strip has
        // used it all along. Reported as a column rather than acted on here.
        bool handleOk = false;
        if (!thumbOk) {
            auto firstRenderable = [&](const QVector<quint32>& hs) -> quint32 {
                for (quint32 h : hs)
                    if (h && ii.has(h) && payloadOk(ii.atlasFor(h))) return h;
                return 0;
            };
            const quint32 hCard = firstRenderable(p->cardArt);
            const quint32 hArt  = firstRenderable(p->art);
            if (hCard) record(pCard, hCard, season, p->name, p->fromCasc);
            if (hArt)  record(pArt,  hArt,  season, p->name, p->fromCasc);
            // What the grid would actually do: prefer the card image, fall back to the flattened
            // list only when there is no card art at all.
            const quint32 hBoth = hCard ? hCard : hArt;
            if (hBoth) record(pBoth, hBoth, season, p->name, p->fromCasc);
            // The HANDLE column keeps its original meaning - this row could render by SOME handle -
            // so the season table stays comparable with the previous run.
            handleOk = (hBoth != 0);
            if (handleOk) { ++gAll.handle; ++t.handle; }
            // Only where every earlier route has failed - the same order the tab uses, so this
            // counts the rows the portrait actually rescues rather than every row that has one.
            if (!handleOk) {
                if (p->encrypted) {
                    ++portraitLocked;
                } else {
                    // Routes 3 and 4, in the tab's order and over the same leaves, so a row is
                    // attributed to the route that actually draws it rather than to both.
                    bool byContent = false, byPortrait = false, sawName = false;
                    const QVector<int> leaves = ProductContents::leafContentSnos(sp, *p);
                    for (int cs : leaves) {
                        const auto* c = sp.product(cs);
                        if (!c || c->payloadName.isEmpty()) continue;
                        sawName = true;
                        if (contentIconOf(c->payloadName)) { byContent = true; break; }
                    }
                    if (!byContent) {
                        for (int cs : leaves) {
                            const auto* c = sp.product(cs);
                            if (!c || c->payloadName.isEmpty()) continue;
                            auto pc = portraitCache.constFind(c->payloadName);
                            if (pc == portraitCache.constEnd())
                                pc = portraitCache.insert(c->payloadName,
                                                          portraitOf(d4dataDir, c->payloadName));
                            const quint32 ph = pc.value();
                            if (ph && ii.has(ph)) { byPortrait = true; break; }
                        }
                    }
                    if (byContent)       ++contentFills;
                    else if (byPortrait) ++portraitFills;
                    else if (!sawName)   ++portraitNoName;
                }
            }
        }

        if (thumbOk) {
            ++gAll.renders; ++t.renders;
        } else if (filtOk) {
            ++gAll.filterOnly; ++t.filterOnly;
            // The template, not the instance: the product stem is folded back to * so a hundred
            // rows failing the same way collapse to one line that names the missing template.
            QString shape = filtHit;
            // Guarded: QString::replace with an empty needle inserts between every character, and
            // a product literally named "Bundle_" would produce exactly that.
            if (!bare.isEmpty()) shape.replace(bare, QStringLiteral("*"));
            ++missedTemplate[shape];
            if (gFilterSamples.size() < 25)
                gFilterSamples << QStringLiteral("FILTER-ONLY  %1  [%2]  found %3")
                                      .arg(p->name, season, filtHit);
        } else if (thumbName || filtName) {
            ++gAll.locked; ++t.locked;
        } else {
            ++gAll.noArt; ++t.noArt;
            if (gBlankSamples.size() < 15)
                gBlankSamples << QStringLiteral("NO-ART-BY-NAME  %1  [%2]%3")
                                     .arg(p->name, season,
                                          handleOk ? QStringLiteral("  (art handle WOULD render)")
                                                   : QString());
        }
    }

    out << QString()
        << QStringLiteral("GRID THUMBNAILS")
        << QStringLiteral("===============")
        << QString()
        << QStringLiteral("The rows the Catalogue list shows - bundles, loose and locked products,")
        << QStringLiteral("not their children. RENDERS means the tab finds a named texture whose")
        << QStringLiteral("payload reads - by filename template, or by the structural scan that")
        << QStringLiteral("catches prefixes the templates do not know. FILTER-ONLY means")
        << QStringLiteral("uiArtCandidates finds one")
        << QStringLiteral("and the thumbnail route does not: the row passes \"Has icon\", takes a")
        << QStringLiteral("place in the list, and draws blank. TEX-LOCKED means the name exists but")
        << QStringLiteral("the payload will not read. NO-ART means no candidate name exists at all.")
        << QStringLiteral("HANDLE counts rows that do not render and whose authored art handle")
        << QStringLiteral("would - the fallback the strip already uses and the grid does not.")
        << QString()
        << QStringLiteral("%1 rows: %2 render, %3 FILTER-ONLY, %4 TEX-LOCKED, %5 no art by name "
                          "- %6 of the non-rendering rows have a renderable art handle")
               .arg(gAll.rows).arg(gAll.renders).arg(gAll.filterOnly)
               .arg(gAll.locked).arg(gAll.noArt).arg(gAll.handle)
        << QStringLiteral("  of the %1 that render, %2 are named by a template and %3 only by the "
                          "structural scan (a prefix the template list does not know).")
               .arg(gAll.renders).arg(gAll.renders - scanOnly).arg(scanOnly)
        << QStringLiteral("  CONTENT: %1 more rows are filled by the icon of what the row CONTAINS "
                          "- the tab's third route, asked of each row's leaf contents.")
               .arg(contentFills)
        << QStringLiteral("  PORTRAIT: %1 more rows again, by the payload actor's own portrait - "
                          "the tab's last route, and the only picture a companion or a mount "
                          "trophy has. Counted only where CONTENT already failed, so the two "
                          "numbers do not double-count a row.")
               .arg(portraitFills)
        << QStringLiteral("    not checked: %1 locked (encrypted record - unknowable to anyone "
                          "without the key) and %2 whose leaves name no payload actor in this "
                          "snapshot (only a SNO; resolving it needs SnoIndex::nameForSno, which "
                          "this worker thread must not touch - the tab does resolve those).")
               .arg(portraitLocked).arg(portraitNoName)
        << QString()
        << QStringLiteral("BY SEASON")
        << QStringLiteral("  %1 %2 %3 %4 %5 %6 %7 %8")
               .arg(QStringLiteral("season"), -22)
               .arg(QStringLiteral("rows"), 7)
               .arg(QStringLiteral("renders"), 8)
               .arg(QStringLiteral("FILT-ONLY"), 10)
               .arg(QStringLiteral("LOCKED"), 7)
               .arg(QStringLiteral("NO-ART"), 7)
               .arg(QStringLiteral("HANDLE"), 7)
               .arg(QStringLiteral("DISTINCT"), 9);
    QStringList gSeasons = gBySeason.keys();
    std::sort(gSeasons.begin(), gSeasons.end());
    for (const QString& k : gSeasons) {
        const GTally& t = gBySeason[k];
        out << QStringLiteral("  %1 %2 %3 %4 %5 %6 %7 %8")
                   .arg(k, -22)
                   .arg(t.rows, 7).arg(t.renders, 8).arg(t.filterOnly, 10)
                   .arg(t.locked, 7).arg(t.noArt, 7).arg(t.handle, 7)
                   .arg(pBoth.distinctBySeason.value(k).size(), 9);
    }

    {
        // The whole question in one ratio per policy. 1.00 means every recoverable row shows its
        // own picture; 0.18 means one image stamped across five rows on average, and far worse in
        // the clusters. A policy only earns the grid if its ratio is close to 1.
        out << QString()
            << QStringLiteral("HANDLE FALLBACK - WOULD IT SHOW REAL ART?")
            << QStringLiteral("  Three field policies, same rows. DISTINCT/ROW near 1.00 means the")
            << QStringLiteral("  art is per-product; a low ratio means one image repeated, which is")
            << QStringLiteral("  worse than a blank because it looks like real data.")
            << QString()
            << QStringLiteral("  %1 %2 %3 %4 %5")
                   .arg(QStringLiteral("policy"), -58)
                   .arg(QStringLiteral("rows"), 7)
                   .arg(QStringLiteral("distinct"), 9)
                   .arg(QStringLiteral("per row"), 9)
                   .arg(QStringLiteral("worst"), 7);
        const Policy* const pols[] = { &pCard, &pArt, &pBoth };
        for (const Policy* pol : pols) {
            int worst = 0;
            for (auto i2 = pol->use.constBegin(); i2 != pol->use.constEnd(); ++i2)
                worst = qMax(worst, i2.value());
            out << QStringLiteral("  %1 %2 %3 %4 %5")
                       .arg(pol->name, -58)
                       .arg(pol->rows, 7)
                       .arg(int(pol->use.size()), 9)
                       .arg(pol->rows ? double(pol->use.size()) / double(pol->rows) : 0.0, 9, 'f', 2)
                       .arg(worst, 7);
        }

        for (const Policy* pol : pols) {
            if (pol->use.isEmpty()) continue;
            QVector<QPair<int, quint32>> hot;
            for (auto i2 = pol->use.constBegin(); i2 != pol->use.constEnd(); ++i2)
                hot.push_back({ i2.value(), i2.key() });
            // Total order - count, then handle - so the ranking does not depend on QHash iteration.
            std::sort(hot.begin(), hot.end(), [](const QPair<int, quint32>& a,
                                                 const QPair<int, quint32>& b) {
                return a.first != b.first ? a.first > b.first : a.second < b.second;
            });
            out << QString() << QStringLiteral("  %1").arg(pol->name);
            out << QStringLiteral("    %1 of these rows came from the game binary, where the art "
                                  "offsets are a histogram rather than a named field")
                       .arg(pol->fromCasc);
            if (hot.first().first > 1) {
                out << QStringLiteral("    MOST-REPEATED (a shared image, not per-product art):");
                for (int i3 = 0; i3 < hot.size() && i3 < 6 && hot[i3].first > 1; ++i3)
                    out << QStringLiteral("      %1 rows  handle=%2  e.g. %3")
                               .arg(hot[i3].first, 6).arg(hot[i3].second)
                               .arg(pol->example.value(hot[i3].second));
            } else {
                out << QStringLiteral("    No handle is picked by more than one row - the art is "
                                      "per-product and this policy is sound.");
            }
        }
    }

    if (!missedTemplate.isEmpty()) {
        out << QString()
            << QStringLiteral("TEMPLATES THE THUMBNAIL ROUTE MISSES (rows recoverable by adding it)");
        QVector<QPair<int, QString>> tr;
        for (auto i = missedTemplate.constBegin(); i != missedTemplate.constEnd(); ++i)
            tr.push_back({ i.value(), i.key() });
        // Total order - count first, then the template name - so the ranking is identical between
        // runs instead of depending on QHash iteration.
        std::sort(tr.begin(), tr.end(), [](const QPair<int, QString>& a, const QPair<int, QString>& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        for (const auto& r : tr)
            out << QStringLiteral("  %1 rows  %2").arg(r.first, 6).arg(r.second);
    }

    if (!gFilterSamples.isEmpty()) {
        out << QString()
            << (gAll.filterOnly > gFilterSamples.size()
                    ? QStringLiteral("SAMPLE (%1 of %2 FILTER-ONLY rows)")
                          .arg(gFilterSamples.size()).arg(gAll.filterOnly)
                    : QStringLiteral("ALL %1 FILTER-ONLY ROWS").arg(gAll.filterOnly));
        for (const QString& s : gFilterSamples) out << QStringLiteral("  ") + s;
    }
    if (!gBlankSamples.isEmpty()) {
        out << QString()
            << (gAll.noArt > gBlankSamples.size()
                    ? QStringLiteral("SAMPLE (%1 of %2 rows with no art by name)")
                          .arg(gBlankSamples.size()).arg(gAll.noArt)
                    : QStringLiteral("ALL %1 ROWS WITH NO ART BY NAME").arg(gAll.noArt));
        for (const QString& s : gBlankSamples) out << QStringLiteral("  ") + s;
    }

    out << QString();
    return out.join(QLatin1Char('\n'));
}


// Same class/gender fallback rules as the crawl's pickHandle.
quint32 pickHandle(const QVector<QPair<quint32, quint32>>& inv, int classIdx, bool female)
{
    quint32 m = 0, f = 0;
    if (classIdx >= 0 && classIdx < inv.size()) {
        m = inv[classIdx].first;
        f = inv[classIdx].second;
    }
    if (!m && !f)
        for (const auto& e : inv)
            if (e.first || e.second) { m = e.first; f = e.second; break; }
    return female ? (f ? f : m) : (m ? m : f);
}

// snoActor.name straight out of a d4data item JSON (cheap text-free parse).
QString actorFromItemJson(const QString& d4dataDir, const QString& stem)
{
    QFile f(d4dataDir + QStringLiteral("/json/base/meta/Item/") + stem
            + QStringLiteral(".itm.json"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    return obj.value(QStringLiteral("snoActor")).toObject()
              .value(QStringLiteral("name")).toString();
}

}  // namespace

QString IconAudit::run(const QString& d4dataDir, const SnoIndex* index, CascReader* reader)
{
    const AppearanceMeta& am = AppearanceMeta::instance();
    if (!index || !index->isLoaded() || !am.ready())
        return QStringLiteral("Icon audit: appearance index not ready — wait for Indexing to finish.");
    DadOverride& dad = DadOverride::instance();
    if (!dad.ensureLoaded())
        return QStringLiteral("Icon audit: %1 not found/empty — it is fetched automatically on "
                              "the next data change, or place a d4dad.json there manually.")
            .arg(DadOverride::defaultPath());

    // Appearance name → sno and Actor sno → name, straight from the live index.
    QHash<QString, int> name2sno;
    QHash<int, QString> sno2name;
    for (const SnoEntry& e : index->entries(kGroupAppearance)) {
        name2sno.insert(e.name.toLower(), e.snoId);
        sno2name.insert(e.snoId, e.name);
    }
    QHash<quint32, QString> actorNames;
    for (const SnoEntry& a : index->entries(kGroupActor))
        actorNames.insert(quint32(a.snoId), a.name);

    static const QRegularExpression classGenderRe(QStringLiteral("^([a-z]{3})([fm])_"));

    // Pass 1 — expected handles per appearance, from every diablo4.dad item that
    // resolves to it via the shared derivation rules (multiple items may legally
    // claim one appearance; the tool's pick must match ANY of them).
    QHash<int, QSet<quint32>> expected;      // appearance sno → acceptable handles
    QHash<int, QString>       sourceItem;    // appearance sno → one contributing item (report)
    int itemsUsed = 0, actorFromCasc = 0;
    for (auto it = dad.items().constBegin(); it != dad.items().constEnd(); ++it) {
        const DadItem& di = it.value();
        // Restrict claims to the item's usable classes (empty mask = all). Without
        // this, class-specific uniques that share a style number (HLM_uniq101 …)
        // claim every class's appearance and the audit drowns in false DIFFs.
        QStringList classPrefs;   // empty = all 8
        if (!di.usable.isEmpty()) {
            const QStringList& all = AppearanceMeta::heroClassPrefixes();
            const int n = qMin(int(di.usable.size()), int(all.size()));
            for (int i = 0; i < n; ++i)
                if (di.usable.at(i)) classPrefs << all.at(i);
        }
        QStringList candNames = AppearanceMeta::cosmeticAppearanceNames(di.stem.toLower());
        if (candNames.isEmpty()) {
            QString actor = actorFromItemJson(d4dataDir, di.stem);
            if (actor.isEmpty() && reader && reader->isReady()) {
                const ItemDef::ItemInfo info =
                    ItemDef::parseItem(reader->readMetaBySno(quint64(it.key())));
                if (info.snoActor) {
                    actor = actorNames.value(info.snoActor);
                    ++actorFromCasc;
                }
            }
            candNames = AppearanceMeta::styleAppearanceNames(actor, classPrefs);
        }
        // Route 3. Until this was added the audit never entered a WEAPON appearance into its
        // expected-handle set, so it could neither confirm nor DIFF one: every weapon icon was
        // invisible to the audit and its reported coverage overstated itself.
        candNames = AppearanceMeta::withSelfName(candNames, di.stem.toLower());
        if (candNames.isEmpty())
            continue;
        bool used = false;
        for (const QString& nm : candNames) {
            const int s = name2sno.value(nm, 0);
            if (!s)
                continue;
            const auto gm = classGenderRe.match(nm);
            const bool female = gm.hasMatch() && gm.captured(2) == QLatin1String("f");
            const int ci = gm.hasMatch() ? ItemDef::heroClassIndex(gm.captured(1)) : -1;
            quint32 h = di.inv.isEmpty() ? 0 : pickHandle(di.inv, ci, female);
            if (!h) h = di.icon;
            if (!h)
                continue;
            expected[s].insert(h);
            if (!sourceItem.contains(s)) sourceItem.insert(s, di.stem);
            used = true;
        }
        if (used) ++itemsUsed;
    }

    // Reverse map: which appearance name(s) legitimately expect each handle. A DIFF whose
    // tool handle appears here (for a DIFFERENT appearance) is cross-wiring — the solver
    // borrowed another class/style's icon — as opposed to a handle that belongs to nothing.
    QHash<quint32, QStringList> handleOwners;
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it) {
        const QString onm = sno2name.value(it.key());
        for (quint32 h : it.value())
            if (handleOwners[h].size() < 4 && !handleOwners[h].contains(onm))
                handleOwners[h].append(onm);
    }

    // Pass 2 — compare against what the tool resolved.
    const IconIndex& ii = IconIndex::instance();
    const bool spriteCheck = ii.ready();
    QStringList lines;
    int missing = 0, diffs = 0, nosprite = 0, ok = 0;
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it) {
        const int s = it.key();
        const QString nm = sno2name.value(s);
        const quint32 actual = am.iconFor(s);
        if (!actual) {
            ++missing;
            lines << QStringLiteral("MISSING  %1 %2  expected=%3 (item %4)")
                         .arg(s).arg(nm).arg(*it.value().constBegin()).arg(sourceItem.value(s));
            continue;
        }
        if (!it.value().contains(actual)) {
            ++diffs;
            QStringList exp;
            for (quint32 h : it.value()) exp << QString::number(h);
            std::sort(exp.begin(), exp.end());
            // Root-cause hint: does the tool's handle belong to another appearance (cross-wiring)?
            const QStringList owners = handleOwners.value(actual);
            const QString why = owners.isEmpty()
                ? QStringLiteral("  [tool handle owned by no d4dad appearance — wrong item]")
                : QStringLiteral("  [tool handle belongs to: %1 — cross-wired]").arg(owners.join(QLatin1Char(',')));
            lines << QStringLiteral("DIFF     %1 %2  tool=%3 expected={%4} (item %5)%6")
                         .arg(s).arg(nm).arg(actual).arg(exp.join(QLatin1Char(',')),
                                                         sourceItem.value(s)).arg(why);
        } else {
            ++ok;
        }
        if (spriteCheck && !ii.has(actual)) {
            ++nosprite;
            // Distinguish "tool picked a spriteless variant when a good one exists" (fixable in
            // the solver) from "no expected handle has a sprite either" (sprite not in the local
            // atlas — a data/coverage issue, not a solver bug).
            bool expectedHasSprite = false;
            for (quint32 h : it.value()) if (ii.has(h)) { expectedHasSprite = true; break; }
            lines << QStringLiteral("NOSPRITE %1 %2  handle=%3 has no atlas frame%4")
                         .arg(s).arg(nm).arg(actual)
                         .arg(expectedHasSprite
                                  ? QStringLiteral("  [an expected handle DOES have a sprite — solver picked a spriteless variant]")
                                  : QStringLiteral("  [no expected handle has a sprite — likely not in the local atlas]"));
        }
    }
    std::sort(lines.begin(), lines.end());

    const QString summary =
        QStringLiteral("Icon audit: %1 appearances checked (%2 d4dad items, %3 via CASC actor) — "
                       "%4 ok, %5 missing, %6 diffs, %7 no-sprite%8")
            .arg(expected.size()).arg(itemsUsed).arg(actorFromCasc)
            .arg(ok).arg(missing).arg(diffs).arg(nosprite)
            .arg(spriteCheck ? QString() : QStringLiteral(" (sprite check skipped: icon index not ready)"));

    // Atomic write into data/ — readable while the app is still running (unlike the
    // truncating log). It used to go beside the EXE, which meant a downloaded copy grew an
    // icon_audit.txt next to D4AssetBrowser.exe the first time indexing finished, contradicting
    // the README's "everything the tool writes lives in data\". The release smoke test missed it
    // because the app was closed before indexing completed, so the audit never ran.
    // Built before the file is opened, not inside the write block: it does index lookups against
    // the reader, and holding a QSaveFile open across them would keep the report locked.
    //
    // Guarded because this section is a late addition to a diagnostic people rely on: if it throws,
    // the MISSING / DIFF / NOSPRITE lines above — already computed — must still reach disk. A
    // diagnostic that can take out the report it is part of is worse than no diagnostic.
    QString catSection;
    try {
        catSection = catalogueIconSection(d4dataDir, index, reader);
    } catch (const std::exception& e) {
        catSection = QStringLiteral("CATALOGUE ICON COVERAGE — section failed: %1\n")
                         .arg(QString::fromUtf8(e.what()));
    } catch (...) {
        catSection = QStringLiteral("CATALOGUE ICON COVERAGE — section failed.\n");
    }

    const QString outPath = AppPaths::file(QStringLiteral("icon_audit.txt"));
    QSaveFile out(outPath);
    if (out.open(QIODevice::WriteOnly)) {
        QByteArray body = summary.toUtf8();
        body += "\n\n";
        body += catSection.toUtf8();
        body += "\n";
        body += lines.join(QLatin1Char('\n')).toUtf8();
        body += '\n';
        out.write(body);
        out.commit();
    }
    return summary + QStringLiteral("  →  %1").arg(outPath);
}
