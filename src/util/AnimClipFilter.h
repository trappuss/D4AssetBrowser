#pragma once
#include <QSettings>
#include <QString>
#include <QStringList>

// ── Per-clip export filters ─────────────────────────────────────────────────────────────────────
//
// AnimExportScope answers "which clips BELONG to this model". This answers "which of those do I
// actually want in the file". They are deliberately separate: the sources are a statement about
// the game data and have one right answer per model, while these are a statement about the export
// you happen to want today.
//
// WHY LENGTH IS THE USEFUL AXIS. Measured on barM_base00's authored set: 378 clips, 28,559
// keyframes, mean 75. Eight clips carry 26% of the whole payload — barM_DSL999_Khelit_PR_base at
// 2,661 frames, barM_ui_characterSelect_loop at 1,360, then six emotes between 421 and 860. A cap
// a little above the mean removes almost nothing you would animate with and a large slice of the
// bytes; no source toggle can express that, because those clips are as "original" as any other.
//
// Applied to the DATA-DERIVED sources only (original / sets / base). `previewed` and `pulled` are
// explicit acts — the clip you are playing, the clip you pulled — and are never filtered out from
// under you, the same rule that lets a multi-selection in the ANIMATIONS list override the scope.
struct AnimClipFilter {
    int         maxFrames = 0;   // 0 = no limit
    QStringList exclude;         // case-insensitive substrings; any match drops the clip

    static AnimClipFilter load()
    {
        QSettings s;
        AnimClipFilter f;
        f.maxFrames = s.value(QStringLiteral("export/animMaxFrames"), 0).toInt();
        const QString raw = s.value(QStringLiteral("export/animExclude")).toString();
        // Comma-separated, trimmed, empties dropped — so "a, ,b" is two patterns, not three, and a
        // trailing comma cannot produce an empty pattern that matches every clip.
        for (const QString& p : raw.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString t = p.trimmed();
            if (!t.isEmpty()) f.exclude << t;
        }
        return f;
    }

    bool excludes(const QString& clipName) const
    {
        for (const QString& p : exclude)
            if (clipName.contains(p, Qt::CaseInsensitive)) return true;
        return false;
    }

    bool active() const { return maxFrames > 0 || !exclude.isEmpty(); }

    QString describe() const
    {
        QStringList p;
        if (maxFrames > 0) p << QStringLiteral("<=%1 frames").arg(maxFrames);
        if (!exclude.isEmpty()) p << QStringLiteral("not [%1]").arg(exclude.join(QLatin1Char('|')));
        return p.isEmpty() ? QStringLiteral("none") : p.join(QStringLiteral(", "));
    }
};
