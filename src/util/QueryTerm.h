#pragma once
#include <QString>
#include <QStringList>
#include <QLatin1Char>

// ── The ONE definition of what a search term matches ────────────────────────────────────────────
//
// The name-box query language (space = AND, leading '-' = exclude, leading '#' = metadata-only)
// is parsed in three places that must agree or Bulk Extract silently returns a different set from
// the list it was filtered in: SnoListModel::setFilter/rebuild, ModelsTab::queryEntries, and the
// textures branch of BulkExtractorTab::computeMatches. Each used to do its own bare
// hay.contains(term); this helper replaces that call in all three so a syntax addition lands
// everywhere at once instead of in whichever parser someone remembered.
//
// Syntax handled HERE: '|' inside a term is OR — "jwl|test999" matches a name containing either.
// Combined with the outer AND: "barf_|barm_ _hlm|_trs" = (barf_ OR barm_) AND (_hlm OR _trs).
// A '-' term with alternatives excludes when ANY alternative matches, i.e. -a|b == NOT(a OR b),
// which is what reading "exclude a or b" means.
//
// This exists for the factory Bulk Extract presets: "all customization for a class" is a union of
// naming families (hair + jewelry + base bodies + …), which an AND-only language cannot say.
namespace QueryTerm {

inline bool matches(const QString& hay, const QString& term)
{
    if (!term.contains(QLatin1Char('|')))
        return hay.contains(term, Qt::CaseInsensitive);
    const QStringList alts = term.split(QLatin1Char('|'), Qt::SkipEmptyParts);
    if (alts.isEmpty()) return true;   // a term that is ONLY pipes constrains nothing
    for (const QString& a : alts)
        if (hay.contains(a, Qt::CaseInsensitive)) return true;
    return false;
}

// ── Self-test ───────────────────────────────────────────────────────────────────────────────────
// Returns "" on success, or the first failing case. Called once at startup (see main.cpp), which
// costs microseconds and is the cheapest guard against the specific regression this header exists
// to prevent: someone "simplifying" one of the three matchers back to a bare contains(). That
// would not fail to compile and would not crash — Bulk Extract would just quietly return a
// different set from the list it was filtered in, which is precisely the bug the comment at
// ModelsTab::queryEntries warns about.
inline QString selfTest()
{
    struct Case { const char* hay; const char* term; bool want; };
    static const Case kCases[] = {
        // plain terms behave exactly as before
        {"barF_H03",            "barf_h",              true },
        {"barF_H03",            "druf_h",              false},
        {"barF_H03",            "BARF_H",              true },   // case-insensitive
        // alternatives
        {"barF_H03",            "druf_h|barf_h",       true },
        {"barF_H03",            "druf_h|necf_h",       false},
        {"jwl00_warM",          "jwl|test999",         true },
        {"palF_base00",         "palf_base00|palm_base00", true },
        {"palF_base01_TRS",     "palf_base00|palm_base00", false},  // base01 must NOT match
        // degenerate input must not match everything
        {"anything",            "|",                   true },
        {"anything",            "zzz|",                false},
    };
    for (const auto& c : kCases) {
        const QString hay = QString::fromLatin1(c.hay), term = QString::fromLatin1(c.term);
        if (matches(hay, term) != c.want)
            return QStringLiteral("QueryTerm: \"%1\" vs \"%2\" expected %3")
                       .arg(hay, term, c.want ? QStringLiteral("match") : QStringLiteral("no match"));
    }
    return QString();
}

}  // namespace QueryTerm
