#pragma once
// How a multi-model export lays its files out on disk — ONE implementation, shared by every path
// that writes more than one model.
//
// This used to be a Bulk Extract combo and nothing else, which meant the Models tab's own batch
// paths — multi-select export, the context-menu batch, drag-out, "export all", the both-genders
// twin: six call sites into exportModels() — had no answer to the same question and were always
// flat. "Where do the files go" is a property of exporting models, not of the tab you started from.
//
// The layout picks the GROUP FOLDER only. What appears inside each group — the models themselves
// plus deps\, textures\ and buffers\ for whichever options are on — is fixed and identical in every
// mode, which is what makes Flat simply "one group, at the root" rather than a special case.
//
// The rule is the ENTRY POINT, not the item count. The single-model paths — Ctrl+E, its
// both-genders twin, drag-out — pass applyLayout=false: a global "by Class" silently turning Ctrl+E
// into barbarian\foo.glb is a surprise the caller cannot undo, and drag-out rebuilds its own paths
// so a folder would make the drag carry nothing. The batch paths pass true even for a one-row
// selection, because that IS the right answer: exporting one more barbarian into a folder you
// already grouped by class belongs in Barbarian\ with the rest, not loose at the top.
#include <QDir>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>

#include "index/AppearanceMeta.h"
#include "util/NameTemplate.h"

namespace ExportLayout {

// Stored in export/folderLayout as a STABLE STRING, not the combo index it used to be. An index is
// exactly the identity this codebase warns against persisting: inserting a mode reorders every
// saved value, and the two qBound() clamps that guarded the old key had to be found and corrected
// by hand every time the list grew.
//
// Class and Type name AppearanceMeta tag groups, so they are the group's real name. Model is a
// sentinel and starts with '_' so it can never collide with a tag group, which are all plain words.
inline QString kFlat()  { return QString(); }
inline QString kClass() { return QStringLiteral("Class"); }
inline QString kType()  { return QStringLiteral("Type"); }
inline QString kModel() { return QStringLiteral("_model"); }

// Class and Type resolve through AppearanceMeta, whose tag map is keyed by APPEARANCE sno. Handed a
// texture sno it returns nothing, so a Textures-mode run under either mode files every single item
// under "_misc" — the option looks like it did something and did not. Callers offering these in a
// non-appearance context should disable them; see BulkExtractorTab's mode switch.
inline bool needsAppearanceTags(const QString& mode)
{
    return mode == kClass() || mode == kType();
}

// Only ids this build understands. Anything else — a newer build's mode, a hand-edited INI — has to
// read as Flat: an unrecognised value would otherwise be taken for a tag-group name, match nothing,
// and file the whole run under "_misc" while the combo (findData → -1) displayed "Flat". Fail to
// where the user pointed, never to somewhere they did not ask for.
inline bool isKnown(const QString& mode)
{
    return mode.isEmpty() || mode == kClass() || mode == kType() || mode == kModel();
}

// The mode in force. Migrates the old bulk/organize combo INDEX once, then never looks at it again.
inline QString mode()
{
    QSettings s;
    if (!s.contains(QStringLiteral("export/folderLayout"))
        && s.contains(QStringLiteral("bulk/organize"))) {
        // 0 Flat · 1 Class · 2 Type · 3 by Model. Index 3 existed only briefly, in the build that
        // added by-Model before this key did — mapping it to Flat would silently discard a choice
        // those profiles had already made. Anything else is corrupt, and Flat is the safe reading.
        switch (s.value(QStringLiteral("bulk/organize"), 0).toInt()) {
            case 1:  s.setValue(QStringLiteral("export/folderLayout"), kClass()); break;
            case 2:  s.setValue(QStringLiteral("export/folderLayout"), kType());  break;
            case 3:  s.setValue(QStringLiteral("export/folderLayout"), kModel()); break;
            default: s.setValue(QStringLiteral("export/folderLayout"), kFlat());  break;
        }
        s.remove(QStringLiteral("bulk/organize"));   // migrated; leaving it is a dead key
        s.sync();
    }
    const QString m = s.value(QStringLiteral("export/folderLayout")).toString();
    return isKnown(m) ? m : kFlat();
}

// Folder name for one model under kModel(): the .glb's own stem, so the folder and the file inside
// it always agree. `tpl` is the export/nameModel template, passed in because NameTemplate::model()
// opens QSettings per call and this runs once per item.
inline QString modelFolder(const QString& tpl, int sno, const QString& name)
{
    QString stem = NameTemplate::apply(tpl, name, sno).trimmed();
    // NameTemplate strips path separators but knows nothing about directory rules: a trailing dot
    // or a reserved device name makes mkpath fail, and every write into that folder then fails with
    // nothing in the log to say why.
    while (stem.endsWith(QLatin1Char('.'))) stem.chop(1);
    static const QStringList kReserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),  QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9") };
    if (kReserved.contains(stem, Qt::CaseInsensitive)) stem.prepend(QLatin1Char('_'));
    if (stem.isEmpty()) stem = name;
    return stem.isEmpty() ? QStringLiteral("model") : stem;
}

// Which tag value this appearance belongs to, given that group's value list already in hand.
// "_misc" when it carries none — never empty, so a run cannot scatter files into the parent.
//
// Takes the list rather than the group name because tagGroups() rebuilds its whole map on every
// call, walking every indexed appearance; resolving it per item turns grouping into an
// O(items x appearances) stall on the GUI thread. group() resolves it once and passes it in.
inline QString tagFolderIn(const QStringList& values, int sno)
{
    const QSet<QString> tags = AppearanceMeta::instance().tagsFor(sno);
    for (const QString& v : values) if (tags.contains(v)) return v;
    return QStringLiteral("_misc");
}

// Convenience for a one-off lookup (the Bulk tab's subfolderFor). Prefer tagFolderIn in a loop.
inline QString tagFolder(const QString& mode, int sno)
{
    if (!needsAppearanceTags(mode) || !AppearanceMeta::instance().ready()) return QStringLiteral("_misc");
    return tagFolderIn(AppearanceMeta::instance().tagGroups().value(mode), sno);
}

struct Group {
    QString folder;                              // empty = the destination itself (Flat)
    QVector<QPair<int, QString>> items;          // (sno, name)
};

// Split `items` into destination groups. Flat returns exactly one group with an empty folder, so a
// caller can always treat the result uniformly instead of special-casing it.
inline QVector<Group> group(const QString& mode, const QVector<QPair<int, QString>>& items)
{
    if (mode.isEmpty() || !isKnown(mode) || items.isEmpty()) return { Group{ QString(), items } };

    // QMap, not QHash: folders come out in a stable order, so the log and the progress read the
    // same way on every run over the same selection.
    QMap<QString, QVector<QPair<int, QString>>> by;
    if (mode == kModel()) {
        const QString tpl = QSettings().value(QStringLiteral("export/nameModel"),
                                              QStringLiteral("{{FileName}}")).toString();
        for (const auto& it : items) by[modelFolder(tpl, it.first, it.second)].append(it);
    } else if (!AppearanceMeta::instance().ready()) {
        return { Group{ QString(), items } };   // no tags yet — one _misc folder helps nobody
    } else {
        const QStringList values = AppearanceMeta::instance().tagGroups().value(mode);
        for (const auto& it : items) by[tagFolderIn(values, it.first)].append(it);
    }
    QVector<Group> out;
    out.reserve(by.size());
    for (auto g = by.constBegin(); g != by.constEnd(); ++g) out.append(Group{ g.key(), g.value() });
    return out;
}

// The destination for one group, resolved against the run's root.
inline QString folderFor(const QString& root, const Group& g)
{
    return g.folder.isEmpty() ? root : QDir(root).filePath(g.folder);
}

}   // namespace ExportLayout
