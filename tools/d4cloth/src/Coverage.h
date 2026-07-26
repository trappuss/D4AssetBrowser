#pragma once
// d4cloth — authored-data coverage report (harness requirement §5 / plan §6).
//
// For each ClothData block: every header field and every one of the 27 arrays — present?
// size vs the counts, min/max/mean, index validity, and whether the APP RUNTIME actually
// consumes it today (a static map audited from GLModelWidget.cpp / ModelParser.cpp).
// Plus the decisive analyses: attachmentLengths correlation candidates (H1), cluster
// partition check (H3), driver-array shape (H2), pinned-vs-kinematicCount, and hex dumps
// of the arrays nobody has decoded yet.

#include "ClothDoc.h"

#include <QString>

struct ModelGeometry;   // model/ModelGeometry.h (piece skeleton for cross-analysis)

namespace d4cloth {

// Full text report for one ClothData block. `geo` (optional) enables the analyses that
// need the piece's skeleton (driver-frame ↔ bone-rest matching, cage-vert ↔ bone matching).
QString coverageReport(const ClothDoc& doc, const ModelGeometry* geo);

// One-line-per-array machine-readable summary (CSV) for regression tracking.
QString coverageCsv(const ClothDoc& doc);

} // namespace d4cloth
