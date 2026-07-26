#pragma once
// ── PanelBox: a right-column stacking panel, shared by the Models and Wardrobe tabs ──────────
// TITLE ……… [▲][▼][✕] over its content. Panels are toggled from a vertical icon strip and live
// in a vertical QSplitter: several stack at once, drag the handles to size them, ▲▼ to reorder,
// ✕ to hide. No fold arrow and no pin — showing/hiding IS the toggle. Plain QWidget (no
// Q_OBJECT → no moc, header-only): the owning tab connects to the public buttons directly.
//
// SIZING CONTRACT — the whole reason panels used to break when dragged:
//   · the panel is Expanding with a small floor, so a drag can shrink it to a header + sliver;
//   · GREEDY content (anything vertically Expanding — tables, tree lists, the tabbed pages)
//     fills the panel and scrolls past it;
//   · everything else (label grids) rides in a scroll area under a trailing stretch, so a tall
//     panel shows honest blank at the bottom and a short one grows a scrollbar.
// What must NEVER happen is a fixed/maximum height on the content: QBoxLayout, handed spare
// room it cannot give to anything, springs the leftover out as gaps BETWEEN the items — which
// is exactly the 250px hole that used to open between the PARTS title and its table.

#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include "BrowserTab.h"   // kHdrQss — the shared neutral panel-title style

constexpr int kPanelHeadH = 20;

// How tall a panel's content would LIKE to be the first time it's opened, published as a
// dynamic property on the content widget (Models: autoSizeTable) and read by
// PanelBox::preferredHeight. A hint, never a constraint.
constexpr const char* kWantH = "d4PanelWantH";

class PanelBox : public QWidget {
public:
    PanelBox(const QString& title, QWidget* content, QWidget* parent)
        : QWidget(parent), body(content)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(1);
        auto* head = new QWidget(this);
        head->setFixedHeight(kPanelHeadH);
        head->setStyleSheet(QStringLiteral("background:#2b2b2b;border-radius:3px;"));
        auto* h = new QHBoxLayout(head);
        h->setContentsMargins(5, 1, 3, 1);
        h->setSpacing(2);
        // The TITLE label is handed back to the tab, which keeps writing live counts into it
        // ("PARTS · 3 of 5 shown") — the header hosts the same label the pages always used.
        label = new QLabel(title, head);
        label->setStyleSheet(QLatin1String(kHdrQss));
        h->addWidget(label, 1);
        auto mk = [&](const QString& glyph, const QString& tip) {
            auto* b = new QToolButton(head);
            b->setText(glyph);
            b->setToolTip(tip);
            b->setAutoRaise(true);
            b->setFixedSize(16, 18);
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(QStringLiteral(
                "QToolButton{border:none;background:transparent;color:#8a8a8a;}"
                "QToolButton:hover{color:#e0e0e0;}"));
            h->addWidget(b);
            return b;
        };
        up    = mk(QStringLiteral("▲"), QStringLiteral("Move this panel up"));
        down  = mk(QStringLiteral("▼"), QStringLiteral("Move this panel down"));
        close = mk(QStringLiteral("✕"), QStringLiteral("Hide this panel"));
        v->addWidget(head);
        content->setParent(this);
        greedy = content->sizePolicy().verticalPolicy() & QSizePolicy::ExpandFlag;
        if (greedy) {
            v->addWidget(content, 1);   // fills the panel, scrolls itself past it
        } else {
            // Label-grid content: hug the top, scroll when squeezed. Without the stretch the
            // form's rows would spring apart to eat the spare height.
            auto* sc = new QScrollArea(this);
            sc->setWidgetResizable(true);
            sc->setFrameShape(QFrame::NoFrame);
            sc->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            auto* holder = new QWidget(sc);
            auto* hv = new QVBoxLayout(holder);
            hv->setContentsMargins(0, 0, 0, 0);
            hv->setSpacing(0);
            hv->addWidget(content);
            hv->addStretch(1);
            sc->setWidget(holder);
            v->addWidget(sc, 1);
        }
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        setMinimumHeight(kPanelHeadH + 38);   // a drag can always squeeze a panel to a sliver
    }

    // The height this panel asks for when it first comes up. Tables/lists publish their row
    // count via the kWantH property; everything else falls back to its natural hint. Clamped
    // both ends: one fat panel must not swallow the column, a thin one must not be a slit.
    int preferredHeight() const
    {
        int c = body ? body->property(kWantH).toInt() : 0;
        if (c <= 0 && body) c = body->sizeHint().height();
        return kPanelHeadH + qBound(80, c, 400);
    }

    QToolButton* up    = nullptr;
    QToolButton* down  = nullptr;
    QToolButton* close = nullptr;
    QLabel*      label = nullptr;
    QWidget*     body  = nullptr;   // the registered content (NOT the scroll wrapper)
    bool         greedy = false;    // content fills the panel rather than hugging the top
};

// The floor the splitter will actually enforce for a pane — whichever is larger, what we asked
// for or what its layout insists on.
inline int panelBoxFloor(QWidget* w)
{
    return qMax(w->minimumHeight(), w->minimumSizeHint().height());
}

// A panel that's just come up should ARRIVE at a height that fits what's in it, rather than the
// equal split QSplitter hands out blind — opening ANIMATIONS next to INFO shouldn't cut the
// clip list to half a column, and opening INFO shouldn't hand a dozen label rows 500px of
// nothing. The newcomer's preferred height is taken from the panels already up, in proportion
// to how much SLACK each has above its floor, so nothing gets pushed below its minimum.
inline void panelBoxArrive(QSplitter* stack, PanelBox* box)
{
    if (!stack || !box) return;
    const int me = stack->indexOf(box);
    if (me < 0) return;
    QList<int> sizes = stack->sizes();
    QVector<int> others;
    int slack = 0;
    for (int i = 0; i < stack->count(); ++i) {
        if (i == me || stack->widget(i)->isHidden()) continue;
        const int s = qMax(0, sizes.value(i) - panelBoxFloor(stack->widget(i)));
        if (s > 0) { others << i; slack += s; }
    }
    if (others.isEmpty()) return;   // only panel up, or the rest are already at their floor —
                                    // the splitter's own distribution is the best available
    const int want = qMin(box->preferredHeight() - sizes.value(me), slack);
    if (want <= 0) return;          // it already has at least what it asked for
    int left = want;
    for (int k = 0; k < others.size() && left > 0; ++k) {
        const int i = others[k];
        const int mine = qMax(0, sizes[i] - panelBoxFloor(stack->widget(i)));
        const int give = (k == others.size() - 1) ? left
                                                  : qMin(left, qRound(double(want) * mine / slack));
        sizes[i] -= give;
        left     -= give;
    }
    sizes[me] += want - left;
    stack->setSizes(sizes);
}
