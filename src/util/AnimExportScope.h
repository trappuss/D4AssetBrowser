#pragma once
#include <QSettings>
#include <QString>
#include <QStringList>

// ── Which animations an export embeds ───────────────────────────────────────────────────────────
//
// FIVE INDEPENDENT SOURCES, because they answer different questions and the old model could not
// express the useful combinations:
//
//   original   clips whose snoAppearance names this model AND whose own name is in its family
//              (barM_base00 -> barM_*). The playable set: gameplay, emotes, wardrobe, UI poses.
//              378 for barM_base00 — the same number WardrobeTab2 reports, because scanFor
//              pre-filters on the filename prefix before checking snoAppearance.
//   sets       the complement: claimed by this body's snoAppearance but named OUTSIDE its family — IGC_* and
//              Conv_*, the cutscene and conversation performances. 380 for barM_base00. Off by
//              default; the two together are the ANIMATIONS panel total (758).
//
//              MEASURED, after two wrong guesses. All 758 clips name barM_base00 as their FIRST
//              and only snoAppearance, so nothing about reference position separates them; and the
//              original definition of `sets` (the actors'-AnimSets expansion) is EMPTY for a base
//              rig, which has no Actor referencing it — the option changed the export by zero
//              clips and read as broken. The name split is the real distinction.
//   previewed  the single clip currently playing in the viewport.
//   pulled     clips manually pulled from another model (the gold rows in the ANIMATIONS list).
//   base       the matching base-rig clips a piece INHERITS — sorF_base00's clips for
//              sorF_stor191_LEG. Additive and deduped: a clip already supplied by `original` or `sets`
//              is never counted again here, so own-vs-inherited precedence is unambiguous.
//              NB it dedups against original UNION sets, including when `sets` is unticked.
//
// What this REPLACED, and why:
//   export/animScope        0 = "only the clip playing in preview", 1 = "all of the model's
//                           animations". That "all" silently meant own ∪ inherited with no way to
//                           separate them — exactly the distinction that matters when you want a
//                           gear piece to come out with no animation at all.
//   export/includePulledAnims  survives verbatim as `pulled`.
//
// Deliberately NOT a source: the skeleton-similarity fallback (animFamiliesBySkeleton). It is a
// bounded GUESS shown in the list flagged as unconfirmed, and it was leaking into exports — the
// interactive path read the list (so it included guessed rows) while the batch path read
// animClipsFor (so it did not). One setting, two answers, depending on whether the model happened
// to be loaded. Guesses are a viewing aid; they do not belong in a file you ship.
struct AnimExportScope {
    bool original  = true;
    bool sets      = false;
    bool previewed = true;
    bool pulled    = false;
    bool base      = false;


    // Reads the five keys, migrating a pre-2.2.8 configuration on first use so an existing setup
    // keeps behaving the way its owner set it up.
    static AnimExportScope load()
    {
        QSettings s;
        AnimExportScope sc;
        if (!s.contains(QStringLiteral("export/animOriginal"))) {
            // Migration. Old "All of the model's animations" == own + inherited, so it maps to
            // original+base; old "only the clip playing in preview" maps to previewed.
            const bool wasAll = s.value(QStringLiteral("export/animScope"), 0).toInt() == 1;
            sc.original  = wasAll;
            sc.base      = wasAll;
            // NOT migrated on: pre-2.2.8 "all" did include the AnimSet expansion, but that is the
            // cutscene bloat this source was split out to make optional. Defaulting it back on
            // would migrate the bug forward and nobody would ever see the smaller export.
            sc.sets      = false;
            sc.previewed = !wasAll;
            sc.pulled    = s.value(QStringLiteral("export/includePulledAnims"), false).toBool();
            s.setValue(QStringLiteral("export/animOriginal"),  sc.original);
            s.setValue(QStringLiteral("export/animPreviewed"), sc.previewed);
            s.setValue(QStringLiteral("export/animPulled"),    sc.pulled);
            s.setValue(QStringLiteral("export/animBase"),      sc.base);
            s.setValue(QStringLiteral("export/animSets"),      sc.sets);
            return sc;
        }
        sc.original  = s.value(QStringLiteral("export/animOriginal"),  true ).toBool();
        sc.sets      = s.value(QStringLiteral("export/animSets"),      false).toBool();
        sc.previewed = s.value(QStringLiteral("export/animPreviewed"), true ).toBool();
        sc.pulled    = s.value(QStringLiteral("export/animPulled"),    false).toBool();
        sc.base      = s.value(QStringLiteral("export/animBase"),      false).toBool();
        return sc;
    }

    // For logs and tooltips — "original+base", or "nothing selected".
    QString describe() const
    {
        QStringList p;
        if (original)  p << QStringLiteral("original");
        if (sets)      p << QStringLiteral("sets");
        if (previewed) p << QStringLiteral("previewed");
        if (pulled)    p << QStringLiteral("pulled");
        if (base)      p << QStringLiteral("base");
        return p.isEmpty() ? QStringLiteral("nothing selected") : p.join(QLatin1Char('+'));
    }
};
