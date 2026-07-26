#pragma once
#include <QString>

class CascReader;
class SnoIndex;

// Startup sanity check that the game's model format still parses the way the tool expects.
// Diablo IV patches occasionally change binary layouts (e.g. the 2.5 meta-storage change that
// broke community tools for weeks); this catches that early and says so plainly, instead of
// exports silently coming out wrong.
namespace FormatProbe {

struct Result {
    bool    ran      = false;   // false ⇒ couldn't probe (no CASC / model not in index)
    bool    ok       = false;   // true ⇒ a known model parsed with a plausible skeleton
    int     boneCount = 0;
    QString probedName;         // the appearance we tested (e.g. "barM_base00")
    QString summary;            // one-line status for the log / status bar
    QString warning;            // non-empty ⇒ show the user (format likely changed)
};

// Parse a known-stable player body (`barM_base00`, else any `*_base00`) from CASC and check
// the skeleton parses with a sane bone count. Cheap (one model). Never throws.
Result run(CascReader* reader, const SnoIndex* index);

}  // namespace FormatProbe
