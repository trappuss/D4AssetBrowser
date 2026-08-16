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
// Writes a sorted report into data/ (icon_audit.txt, atomic write — readable while
// the app runs) and returns a one-line summary.
//
// Runs synchronously on the CALLER'S thread (a few seconds; CASC reads only for items
// missing from the local d4data snapshot). Call it from a worker, never from a signal
// handler or a slot: MainWindow::autoIconAudit did exactly that and froze the window
// for the whole audit. The manual File ▸ Icon audit still calls it inline, but sets a
// wait cursor so the pause is explained rather than mysterious.
//
// Thread-safe against the rest of the app while it runs — CascReader reads concurrently
// by design, DadOverride::ensureLoaded is documented safe for this caller, and
// AppearanceMeta / IconIndex / SnoIndex are read-only once ready — with ONE exception:
// a reload() rebuilds SnoIndex and AppearanceMeta underneath it. Callers must ensure no
// reload can start while this is in flight (MainWindow defers via m_iconAuditRunning).
namespace IconAudit {

QString run(const QString& d4dataDir, const SnoIndex* index, CascReader* reader);

}  // namespace IconAudit
