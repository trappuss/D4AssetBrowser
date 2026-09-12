#pragma once
// ── One inventory of "what is viewport state" for the three 3D tabs ─────────────────────────────
//
// Models, Wardrobe and Stable each own a full copy of the same rendering controls — lighting rig,
// shading features, overlays, cloth, fur/FX, background, shade mode — under three different
// QSettings prefixes. Nothing has ever held the three lists in one place, and they have drifted in
// every way three hand-maintained copies can:
//
//   * LEAF NAMES differ.   Models writes viewport/shadows (plural); Wardrobe writes
//                          viewport/shadow. Each tab is internally consistent, so nothing is
//                          broken — but they are not the same setting, and a fix applied to one
//                          spelling silently misses the other.
//   * GROUPING differs.    Models and Wardrobe keep fur and FX under viewport/; Stable split them
//                          into fur/ and fx/, and calls the background group gfx/ rather than
//                          viewport/.
//   * DEFAULTS differ.     Stable renders skin SSS at 0.15 while Models and Wardrobe use 0.24,
//                          which is also what GLModelWidget documents as its own default. Stable
//                          is internally consistent about it (slider and consumer agree), so it
//                          reads as a deliberate per-tab look for creatures rather than a bug —
//                          recorded here so the next person does not "fix" it in either direction
//                          without deciding which answer is wanted.
//   * COVERAGE differs.    Restore Defaults reset ten wardrobe2/viewport keys by name and nothing
//                          else, so it silently did nothing for the Models and Stable tabs.
//
// This header does NOT try to unify the three key schemes. Renaming a shipped key orphans every
// user's saved value for no gain, and the two spellings are load-bearing in their own tabs. What it
// does is name the three groups once, say which subtrees under each ARE render state, and give one
// correct way to reset a group.
//
// ── Why reset by REMOVAL, not by writing defaults ──────────────────────────────────────────────
// The obvious design is a table of key → default value. It is also the design that guarantees a new
// class of bug: the table's value and the reader's own fallback are two copies of one number, and
// when they disagree the app renders at one and reports the other. That is exactly the R3 defect
// (slider default 15, renderer default 24) which shipped for months.
//
// Removing the key instead makes every reader fall back to the default it already carries at its
// read site — which is, by construction, the value the code actually uses. There is nothing here to
// keep in sync, and a new setting is covered the day it is added without touching this file.
//
// ── What is deliberately NOT reset ─────────────────────────────────────────────────────────────
// The user's own saved presets (light presets, camera presets, cloth presets) live under these same
// subtrees and are work product, not configuration — a "Restore Defaults" that deletes them is a
// data-loss bug wearing a reassuring label. kKeepPrefixes below lists them and resetGroup() skips
// anything beneath them.
//
// Panel layout (panels/shown, panels/sizes) and the tab's CONTENT selection (equipped pieces, the
// current mount, camera position) are not render state and are not touched here.

#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ViewportSettings {

struct Group {
    QString prefix;        // QSettings group, e.g. "wardrobe2"
    QString label;         // user-facing tab name, e.g. "Wardrobe"
    QStringList subtrees;  // key prefixes under `prefix` that are render state (trailing '/')
};

// The three 3D tabs. Textures and Catalogue have no viewport and are absent on purpose.
//
// Subtrees only — no bare root-level keys. Each tab keeps ~40 ungrouped keys at its root that mix
// render state with content selection (a background colour next to the remembered mount), and no
// mechanical rule separates them. Resetting a subtree is provably safe; guessing at a flat key is
// not, so the flat keys stay out and this is documented rather than quietly partial.
inline const QVector<Group>& groups()
{
    static const QVector<Group> kGroups = {
        { QStringLiteral("models"), QStringLiteral("Models"),
          { QStringLiteral("light/"),   QStringLiteral("viewport/"), QStringLiteral("view/"),
            QStringLiteral("cloth/"),   QStringLiteral("detail/"),   QStringLiteral("rig/") } },
        { QStringLiteral("wardrobe2"), QStringLiteral("Wardrobe"),
          { QStringLiteral("light/"),   QStringLiteral("viewport/"), QStringLiteral("view/"),
            QStringLiteral("cloth/"),   QStringLiteral("detail/"),   QStringLiteral("rig/") } },
        // Stable's own scheme: gfx/ where the others use viewport/ for background, fur/ and fx/
        // split out, overlays under ovl/ instead of rig/ + view/overlays. See the header note.
        { QStringLiteral("stable2"), QStringLiteral("Stable"),
          { QStringLiteral("light/"),   QStringLiteral("gfx/"),      QStringLiteral("view/"),
            QStringLiteral("cloth/"),   QStringLiteral("detail/"),   QStringLiteral("fur/"),
            QStringLiteral("fx/"),      QStringLiteral("ovl/") } },
    };
    return kGroups;
}

// Saved presets, relative to a group. Everything beneath these survives a reset.
// "cloth/preset" sits INSIDE the cloth/ subtree, which is the whole reason this list exists: a
// plain subtree removal would have taken the user's saved cloth setups with it.
inline QStringList keepPrefixes()
{
    return { QStringLiteral("preset/"),        // light presets, and preset/%1/%2
             QStringLiteral("campreset/"),     // camera presets
             QStringLiteral("cloth/preset") }; // cloth presets (+ cloth/preset/exists)
}

// Remove every render key in this group, so each reader falls back to its own default.
// Returns the number of keys removed — callers use it to report "reset N settings" honestly, and a
// zero tells you the group was already at defaults rather than that the call did nothing.
inline int resetGroup(const Group& g)
{
    QSettings s;
    s.beginGroup(g.prefix);
    const QStringList all = s.allKeys();          // relative to the group
    const QStringList keep = keepPrefixes();
    int n = 0;
    for (const QString& k : all) {
        bool isRender = false;
        for (const QString& sub : g.subtrees)
            if (k.startsWith(sub)) { isRender = true; break; }
        if (!isRender) continue;
        bool keepIt = false;
        for (const QString& kp : keep)
            if (k.startsWith(kp)) { keepIt = true; break; }
        if (keepIt) continue;
        s.remove(k);
        ++n;
    }
    s.endGroup();
    return n;
}

// Every group at once. Returns the total removed.
inline int resetAll()
{
    int n = 0;
    for (const Group& g : groups()) n += resetGroup(g);
    return n;
}

}  // namespace ViewportSettings
