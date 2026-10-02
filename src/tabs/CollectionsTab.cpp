#include "tabs/CollectionsTab.h"

#include "app/Config.h"
#include "index/SeriesIndex.h"

#include <QAbstractItemView>
#include <QAction>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
// Role layout on a tree row. A COLLECTION row carries kMember = -1; a member row carries its index
// into the entry's member list. Both carry kEntry, so every handler resolves the same way and no
// caller has to know which kind of row it is looking at.
constexpr int kEntry  = Qt::UserRole;
constexpr int kMember = Qt::UserRole + 1;
constexpr int kProdSno = Qt::UserRole + 2;   // the member's StoreProduct sno, for the jump

// The combo's own "no filter" sentinel. Stored as DATA, never as the display text — a label is a
// translation away from changing and would silently reset everyone's saved filter.
const char kAny[] = "*";

QString sourceColour(SeriesIndex::Category c)
{
    switch (c) {
        case SeriesIndex::Season:        return QStringLiteral("#7fb2e0");
        case SeriesIndex::Shop:          return QStringLiteral("#e8c46a");
        case SeriesIndex::Uncategorised: break;
    }
    return QStringLiteral("#8f8f8f");
}
}   // namespace

CollectionsTab::CollectionsTab(QWidget* parent) : BrowserTab(parent)
{
    buildUi();
}

void CollectionsTab::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    // ── filter bar ──────────────────────────────────────────────────────────────────────────────
    auto* bar = new QHBoxLayout;
    bar->setSpacing(6);

    auto* title = new QLabel(QStringLiteral("Collections"), this);
    title->setStyleSheet(QLatin1String(kHdrQss));
    bar->addWidget(title);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("Search collections and items…  (Ctrl+F)"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedHeight(kBarH);
    m_search->setMinimumWidth(220);
    bar->addWidget(m_search, 1);

    auto* srcLbl = new QLabel(QStringLiteral("Source:"), this);
    srcLbl->setStyleSheet(QLatin1String(kSubHdrQss));
    bar->addWidget(srcLbl);
    m_source = new QComboBox(this);
    m_source->setFixedHeight(kBarH);
    m_source->addItem(QStringLiteral("Any"), QLatin1String(kAny));
    for (int c = SeriesIndex::Season; c <= SeriesIndex::Uncategorised; ++c)
        m_source->addItem(SeriesIndex::categoryLabel(SeriesIndex::Category(c)),
                          QString::number(c));
    bar->addWidget(m_source);

    auto* kindLbl = new QLabel(QStringLiteral("Kind:"), this);
    kindLbl->setStyleSheet(QLatin1String(kSubHdrQss));
    bar->addWidget(kindLbl);
    m_kind = new QComboBox(this);
    m_kind->setFixedHeight(kBarH);
    m_kind->addItem(QStringLiteral("Any"), QLatin1String(kAny));
    bar->addWidget(m_kind);

    m_count = new QLabel(this);
    m_count->setStyleSheet(QLatin1String(kSubHdrQss));
    bar->addWidget(m_count);
    root->addLayout(bar);

    // ── tree + details ──────────────────────────────────────────────────────────────────────────
    m_split = new QSplitter(Qt::Horizontal, this);

    m_tree = new QTreeWidget(m_split);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({QStringLiteral("Collection"), QStringLiteral("Items"),
                             QStringLiteral("Source")});
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_split->addWidget(m_tree);

    m_details = new QTextBrowser(m_split);
    m_details->setOpenExternalLinks(false);
    m_details->setStyleSheet(QLatin1String(kPanelQss));
    m_split->addWidget(m_details);
    m_split->setStretchFactor(0, 3);
    m_split->setStretchFactor(1, 2);
    root->addWidget(m_split, 1);

    // ── restore saved view state ────────────────────────────────────────────────────────────────
    QSettings s;
    const QByteArray sp = s.value(QStringLiteral("collections/split")).toByteArray();
    if (!sp.isEmpty()) m_split->restoreState(sp);
    const int savedSrc = m_source->findData(
        s.value(QStringLiteral("collections/source"), QLatin1String(kAny)).toString());
    if (savedSrc >= 0) m_source->setCurrentIndex(savedSrc);
    // The Kind list is built FROM the crawl, which has not run yet, so there is nothing to match
    // the saved value against here. Hold it and let rebuild() apply it once the list exists.
    m_wantKind = s.value(QStringLiteral("collections/kind"), QLatin1String(kAny)).toString();

    // ── wiring ──────────────────────────────────────────────────────────────────────────────────
    connect(m_search, &QLineEdit::textChanged, this, [this] { rebuild(); });
    connect(m_source, &QComboBox::currentIndexChanged, this, [this] {
        QSettings().setValue(QStringLiteral("collections/source"),
                             m_source->currentData().toString());
        rebuild();
    });
    connect(m_kind, &QComboBox::currentIndexChanged, this, [this] {
        if (m_filling) return;      // repopulating the list is not the user choosing a kind
        m_wantKind = m_kind->currentData().toString();
        QSettings().setValue(QStringLiteral("collections/kind"), m_wantKind);
        rebuild();
    });
    connect(m_split, &QSplitter::splitterMoved, this, [this] {
        QSettings().setValue(QStringLiteral("collections/split"), m_split->saveState());
    });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this] {
        if (!m_filling) updateDetails();
    });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &CollectionsTab::showRowMenu);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int) {
        if (!it) return;
        const int sno = it->data(0, kProdSno).toInt();
        if (sno > 0) emit revealBundleRequested(sno);
    });
    auto* find = new QShortcut(QKeySequence::Find, this);
    connect(find, &QShortcut::activated, this, [this] {
        m_search->setFocus(Qt::ShortcutFocusReason);
        m_search->selectAll();
    });
}

void CollectionsTab::refresh()
{
    SeriesIndex& idx = SeriesIndex::instance();
    idx.ensureBuilt(Config::d4dataDir());
    if (!m_wired) {
        m_wired = true;
        connect(&idx, &SeriesIndex::readyChanged, this, [this] { rebuild(); });
    }
    rebuild();
}

void CollectionsTab::reset()
{
    m_tree->clear();
    m_details->clear();
    m_count->clear();
}

void CollectionsTab::rebuild()
{
    const SeriesIndex& idx = SeriesIndex::instance();
    m_filling = true;
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();

    if (!idx.ready()) {
        const bool waiting = idx.waitingOnProducts();
        m_count->setText(idx.building() ? QStringLiteral("building…")
                       : waiting        ? QStringLiteral("waiting…")
                                        : QStringLiteral("not indexed"));
        m_details->setHtml(
            idx.building() ? QStringLiteral("<p style='color:#bbb'>Reading each product's set "
                                            "name out of the game data…</p>")
          : waiting        ? QStringLiteral("<p style='color:#bbb'>Waiting for the shop index. A "
                                            "set's name is a property of the product that sold it, "
                                            "so there is nothing to group until those are "
                                            "built.</p>")
                           : QStringLiteral("<p style='color:#bbb'>No collection index yet. Build "
                                            "it from <b>File ▸ Index</b>, or point the app at a "
                                            "d4data snapshot.</p>"));
        m_tree->setUpdatesEnabled(true);
        m_filling = false;
        return;
    }

    // Keep the Kind list in step with the data, preserving the user's pick across rebuilds.
    {
        const QStringList kinds = idx.kinds();
        if (m_kind->count() != int(kinds.size()) + 1) {
            m_kind->clear();
            m_kind->addItem(QStringLiteral("Any"), QLatin1String(kAny));
            for (const QString& k : kinds) m_kind->addItem(k, k);
            const int i = m_kind->findData(m_wantKind);
            m_kind->setCurrentIndex(i >= 0 ? i : 0);
            m_wantKind = m_kind->currentData().toString();   // a kind that no longer exists
        }
    }

    const QString needle = m_search->text().trimmed();
    const QVariant srcSel = m_source->currentData();
    const bool anySrc = srcSel.toString() == QLatin1String(kAny);
    const SeriesIndex::Category wantSrc = SeriesIndex::Category(srcSel.toInt());
    const QString wantKind = m_wantKind;
    const bool anyKind = wantKind.isEmpty() || wantKind == QLatin1String(kAny);

    int shown = 0, shownMembers = 0;
    const QVector<SeriesIndex::Entry>& all = idx.entries();
    for (int ei = 0; ei < int(all.size()); ++ei) {
        const SeriesIndex::Entry& e = all[ei];
        if (!anySrc && e.category != wantSrc) continue;
        if (!anyKind && !e.kinds.contains(wantKind)) continue;

        // A search hit on the collection NAME keeps every member; a hit on a member keeps that
        // member. Otherwise you can find a set but not see which piece matched.
        const bool nameHit = needle.isEmpty() || e.name.contains(needle, Qt::CaseInsensitive);
        QVector<int> keep;
        for (int mi = 0; mi < int(e.members.size()); ++mi) {
            const SeriesIndex::Member& m = e.members[mi];
            if (!anyKind && m.kind != wantKind) continue;
            if (!nameHit && !m.name.contains(needle, Qt::CaseInsensitive)
                         && !m.stem.contains(needle, Qt::CaseInsensitive)) continue;
            keep.push_back(mi);
        }
        if (keep.isEmpty()) continue;

        auto* row = new QTreeWidgetItem(m_tree);
        row->setText(0, e.name);
        row->setText(1, QString::number(keep.size()));
        row->setText(2, SeriesIndex::categoryLabel(e.category));
        row->setForeground(2, QColor(sourceColour(e.category)));
        row->setData(0, kEntry, ei);
        row->setData(0, kMember, -1);
        if (!e.classes.isEmpty())
            row->setToolTip(0, QStringLiteral("Per-class pieces: %1")
                                   .arg(e.classes.join(QStringLiteral(", "))));

        for (int mi : keep) {
            const SeriesIndex::Member& m = e.members[mi];
            auto* kid = new QTreeWidgetItem(row);
            kid->setText(0, m.name.isEmpty() ? m.stem : m.name);
            kid->setText(1, m.kind);
            kid->setText(2, m.cls);
            kid->setData(0, kEntry, ei);
            kid->setData(0, kMember, mi);
            kid->setData(0, kProdSno, m.sno);
            kid->setToolTip(0, m.locked
                ? QStringLiteral("%1\nTACT-locked — the record exists, its contents do not decrypt")
                      .arg(m.product)
                : QStringLiteral("%1\nSold by %2 — double-click to open it in the Catalogue")
                      .arg(m.stem, m.product));
        }
        ++shown;
        shownMembers += int(keep.size());
    }

    m_count->setText(QStringLiteral("%1 of %2 · %3 items")
                         .arg(shown).arg(int(all.size())).arg(shownMembers));
    m_tree->setUpdatesEnabled(true);
    m_filling = false;
    if (m_tree->topLevelItemCount() > 0) {
        m_tree->setCurrentItem(m_tree->topLevelItem(0));
        return;
    }
    // Two very different nothings, and a blank pane told them apart for nobody.
    if (all.isEmpty())
        m_details->setHtml(QStringLiteral(
            "<p style='color:#dedede'><b>The index built, and found no collections.</b></p>"
            "<p style='color:#bbb'>Every store product was read and none named a set. The count of "
            "products read and the count that carried a <code>Series</code> row are both written to "
            "the log each run — look for <i>series index:</i> in <b>Help ▸ Diagnostic "
            "output</b>.</p>"));
    else
        m_details->setHtml(QStringLiteral(
            "<p style='color:#bbb'>No collection matches the current search and filters. "
            "%1 are indexed.</p>").arg(int(all.size())));
}

void CollectionsTab::updateDetails()
{
    const QTreeWidgetItem* it = m_tree->currentItem();
    if (!it) { m_details->clear(); return; }
    const SeriesIndex& idx = SeriesIndex::instance();
    const int ei = it->data(0, kEntry).toInt();
    if (ei < 0 || ei >= int(idx.entries().size())) { m_details->clear(); return; }
    const SeriesIndex::Entry& e = idx.entries()[ei];

    QStringList h;
    h << QStringLiteral("<div style='color:#dedede;font-size:13px'><b>%1</b></div>")
             .arg(e.name.toHtmlEscaped());
    h << QStringLiteral("<div><span style='color:%1'>%2</span></div>")
             .arg(sourceColour(e.category),
                  SeriesIndex::categoryLabel(e.category).toHtmlEscaped());
    h << QStringLiteral("<div style='color:#9a9a9a;margin-top:4px'>%1</div>")
             .arg(SeriesIndex::categoryWhy(e.category).toHtmlEscaped());
    h << QStringLiteral("<hr style='border:1px solid #3a3a3a'>");

    auto line = [&h](const QString& k, const QString& v) {
        if (v.isEmpty()) return;
        h << QStringLiteral("<div><span style='color:#9a9a9a'>%1</span> %2</div>")
                 .arg(k.toHtmlEscaped(), v.toHtmlEscaped());
    };
    line(QStringLiteral("Pieces:"), QString::number(e.memberCount()));
    if (e.seasonLinked > 0)
        line(QStringLiteral("Named a season:"), QStringLiteral("%1 of %2")
                 .arg(e.seasonLinked).arg(e.memberCount()));
    if (e.passLocked > 0)
        line(QStringLiteral("Pass-gated (never sold alone):"), QString::number(e.passLocked));
    if (e.lockedCount > 0)
        line(QStringLiteral("TACT-locked:"), QString::number(e.lockedCount));
    if (!e.seasons.isEmpty())  line(QStringLiteral("Season:"),  e.seasons.join(QStringLiteral(", ")));
    if (!e.branches.isEmpty()) line(QStringLiteral("Patch:"),   e.branches.join(QStringLiteral(", ")));
    if (!e.classes.isEmpty())  line(QStringLiteral("Classes:"), e.classes.join(QStringLiteral(", ")));
    {
        QStringList ks;
        for (auto i = e.kinds.constBegin(); i != e.kinds.constEnd(); ++i)
            ks << QStringLiteral("%1 ×%2").arg(i.key()).arg(i.value());
        line(QStringLiteral("Kinds:"), ks.join(QStringLiteral(", ")));
    }
    // The authored string, verbatim. The heading above it is normalised; showing both is what lets
    // a wrong normalisation be spotted from the UI instead of from a re-crawl.
    line(QStringLiteral("Authored as:"), e.raw);

    const int mi = it->data(0, kMember).toInt();
    if (mi >= 0 && mi < int(e.members.size())) {
        const SeriesIndex::Member& m = e.members[mi];
        h << QStringLiteral("<hr style='border:1px solid #3a3a3a'>");
        h << QStringLiteral("<div style='color:#dedede'><b>%1</b></div>")
                 .arg((m.name.isEmpty() ? m.stem : m.name).toHtmlEscaped());
        line(QStringLiteral("Asset:"),   m.stem);
        line(QStringLiteral("Kind:"),    m.kind);
        line(QStringLiteral("Class:"),   m.cls);
        line(QStringLiteral("Product:"), m.product);
        if (m.locked)
            h << QStringLiteral("<div style='color:#9a9a9a'>TACT-locked: the record exists, its "
                                "contents do not decrypt.</div>");
    }
    m_details->setHtml(h.join(QString()));
}

void CollectionsTab::showRowMenu(const QPoint& pos)
{
    QTreeWidgetItem* it = m_tree->itemAt(pos);
    if (!it) return;
    const SeriesIndex& idx = SeriesIndex::instance();
    const int ei = it->data(0, kEntry).toInt();
    if (ei < 0 || ei >= int(idx.entries().size())) return;
    const SeriesIndex::Entry& e = idx.entries()[ei];
    const int mi = it->data(0, kMember).toInt();

    QMenu menu(this);
    QAction* copyColl = menu.addAction(QStringLiteral("Copy collection name"));
    QAction* copyFile = nullptr;
    QAction* reveal   = nullptr;
    if (mi >= 0 && mi < int(e.members.size())) {
        copyFile = menu.addAction(QStringLiteral("Copy asset name"));
        if (e.members[mi].sno > 0)
            reveal = menu.addAction(QStringLiteral("Show the product in the Catalogue"));
    }
    QAction* chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
    if (!chosen) return;
    if (chosen == copyColl)
        QGuiApplication::clipboard()->setText(e.name);
    else if (copyFile && chosen == copyFile)
        QGuiApplication::clipboard()->setText(e.members[mi].stem);
    else if (reveal && chosen == reveal)
        emit revealBundleRequested(e.members[mi].sno);
}
