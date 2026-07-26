#pragma once
#include <QString>

class SnoIndex;
class CascReader;

// Hidden "Icon audit" diagnostic (File menu): cross-checks every appearance icon
// the tool resolved (AppearanceMeta) against the diablo4.dad database (DadOverride)
// — the reference implementation's per-class handles. Catches silent regressions
// after a game or d4data update:
//   MISSING  — diablo4.dad implies an icon for an appearance, the tool has none;
//   DIFF     — the tool's handle differs from every handle diablo4.dad's items
//              would assign (informational: specificity choices can differ);
//   NOSPRITE — the tool's handle can't be resolved to a sprite by IconIndex.
// Writes a sorted report next to the exe (icon_audit.txt, atomic write — readable
// through the sandbox mount while the app runs) and returns a one-line summary.
//
// Runs synchronously on the caller's thread (a few seconds; CASC reads only for
// items missing from the local d4data snapshot).
namespace IconAudit {

QString run(const QString& d4dataDir, const SnoIndex* index, CascReader* reader);

}  // namespace IconAudit
