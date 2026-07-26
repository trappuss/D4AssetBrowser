#pragma once
#include <QSettings>
#include <QString>

// On-hover preview/info settings (Settings ▸ General ▸ On-hover). One tiny read-through layer so
// every tab's hover popup honours the same knobs: dwell delay, preview on/off, scroll-zoom,
// initial size, and per-tab info-line toggles (keys like "tex/sno", "mdl/name", "w2/collDesc").
namespace HoverInfo {

inline int delayMs()
{
    const double s = QSettings().value(QStringLiteral("hover/delaySec"), 0.5).toDouble();
    return int(qBound(0.0, s, 5.0) * 1000.0);
}
inline bool imagePreview() { return QSettings().value(QStringLiteral("hover/imagePreview"), true).toBool(); }
inline bool scrollZoom()   { return QSettings().value(QStringLiteral("hover/scrollZoom"), true).toBool(); }
inline int  previewPx()
{
    return qBound(64, QSettings().value(QStringLiteral("hover/previewPx"), 256).toInt(), 1024);
}
// Per-tab info-line toggle, e.g. on("w2/collName"). Default ON — hovers are informative until trimmed.
inline bool on(const char* key, bool def = true)
{
    return QSettings().value(QStringLiteral("hover/") + QLatin1String(key), def).toBool();
}
// Colour-code hover info (name stays white, filename grey, the rest semantic). Default ON.
inline bool colourCode() { return QSettings().value(QStringLiteral("hover/colour"), true).toBool(); }

// Shared palette so every tab's hover lines agree.
namespace Col {
inline constexpr const char* kName   = "#ffffff";   // item name — white
inline constexpr const char* kFile   = "#9a9a9a";   // sno · filename — grey
inline constexpr const char* kSeries = "#e8c46a";   // collection / series — app gold
inline constexpr const char* kFlavor = "#c9b48a";   // description flavour text — parchment
inline constexpr const char* kInfo   = "#7fb2e5";   // season / introduced-in — cool blue
inline constexpr const char* kGood   = "#8fbf8f";   // affirmative state (dyeable) — green
inline constexpr const char* kMeta   = "#b0b0b0";   // counts, sizes, dimensions — light grey
inline constexpr const char* kNew    = "#e0803c";   // ★ new this update — ember orange
}

}  // namespace HoverInfo
