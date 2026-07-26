#pragma once
#include <QRegularExpression>
#include <QSettings>
#include <QString>

// d4analyzer-style export filename templates. Placeholders:
//   {{FileName}}  — the asset's game name (e.g. "barM_sets53_hlm")
//   {{SNO}}       — the numeric SNO id
//   {{FrameIdx}}  — texframe index (frame exports only)
//   {{FrameName}} — texframe label from the atlas descriptor (frame exports only)
// Configured in Settings ▸ Export ▸ File names; defaults preserve the tool's existing names.
namespace NameTemplate {

inline QString apply(QString tpl, const QString& fileName, qint64 sno,
                     int frameIdx = -1, const QString& frameName = QString())
{
    tpl.replace(QLatin1String("{{FileName}}"), fileName);
    tpl.replace(QLatin1String("{{SNO}}"), QString::number(sno));
    tpl.replace(QLatin1String("{{FrameIdx}}"), frameIdx >= 0 ? QString::number(frameIdx) : QString());
    tpl.replace(QLatin1String("{{FrameName}}"), frameName);
    static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|]"));
    tpl.replace(bad, QStringLiteral("_"));
    const QString out = tpl.simplified();          // collapse doubles from empty fields
    return out.isEmpty() ? fileName : out;         // never produce an empty file name
}

// The three configured templates (with the tool's historical formats as defaults).
inline QString texture(const QString& fileName, qint64 sno)
{
    return apply(QSettings().value(QStringLiteral("export/nameTexture"),
                                   QStringLiteral("{{FileName}}")).toString(), fileName, sno);
}
inline QString model(const QString& fileName, qint64 sno)
{
    return apply(QSettings().value(QStringLiteral("export/nameModel"),
                                   QStringLiteral("{{FileName}}")).toString(), fileName, sno);
}
inline QString frame(const QString& fileName, qint64 sno, int frameIdx, const QString& frameName)
{
    // Default = d4analyzer's TexFrames format, which the icon_overrides re-import also parses.
    return apply(QSettings().value(QStringLiteral("export/nameFrame"),
                                   QStringLiteral("{{FileName}} [{{SNO}}] - {{FrameIdx}} {{FrameName}}")).toString(),
                 fileName, sno, frameIdx, frameName);
}

}  // namespace NameTemplate
