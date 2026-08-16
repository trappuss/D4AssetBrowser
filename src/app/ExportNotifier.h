#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

#include "model/ModelExporter.h"   // Options — what glbOptionsLine reports

// App-wide export notifications. Any tab calls ExportNotifier::instance().notify(summary, folder)
// after a successful export; MainWindow listens and shows ONE consistent toast (with a "Show in
// folder" action) — so every tab reports exports the same way instead of a mix of modal dialogs,
// status-bar text and per-tab toasts.
class ExportNotifier : public QObject {
    Q_OBJECT
public:
    static ExportNotifier& instance() { static ExportNotifier n; return n; }
    // `folder` is the directory to reveal (omit for none). `text` is a short human summary.
    void notify(const QString& text, const QString& folder = QString()) { emit exported(text, folder); }

    // ── "Which convention is this .glb in?" ───────────────────────────────────────────────────
    // A one-line summary for appending to a MODEL export's toast: "  ·  Blender axes, scale x100".
    //
    // Takes the resolved ModelExporter::Options rather than reading QSettings, and that is the
    // whole design. An option's SETTING and its EFFECT are not the same thing here:
    //   • retarget/enginePreset OVERRIDES export/blenderFriendly outright (Blender and Unreal force
    //     it on, Unity forces it off), so reporting the raw key would assert Blender axes on a file
    //     exported as plain Y-up glTF, and omit them on one that was converted.
    //   • Options is what every export path passes to exportGlb(), so anything in it is true of the
    //     file that was just written, on every path, with no per-tab knowledge needed here.
    //
    // Deliberately reports NOTHING about textures, dye, detail baking, loose maps or raw deps.
    // Those settings are honoured by some export paths and not others, so a shared settings-derived
    // line would confidently name options the file does not have — worse than saying nothing. A
    // path that wants to report one of those owns a fact this function does not have, and should
    // append it at its own call site.
    static QString glbOptionsLine(const ModelExporter::Options& opt)
    {
        QStringList parts;
        // Always stated: the axis convention is the single most common "why is my import rotated"
        // question, and it is the one thing every path resolves identically.
        parts << (opt.blenderFriendly ? QStringLiteral("Blender axes") : QStringLiteral("glTF axes"));
        if (opt.unitScale != 1.0f)
            parts << QStringLiteral("scale x%1").arg(double(opt.unitScale), 0, 'g', 4);
        if (opt.flipNormalGreen)     parts << QStringLiteral("DirectX normals");
        if (!opt.reconstructNormalZ) parts << QStringLiteral("normal Z as decoded");
        return QStringLiteral("  ·  ") + parts.join(QStringLiteral(", "));
    }

signals:
    void exported(const QString& text, const QString& folder);

private:
    ExportNotifier() = default;
};
