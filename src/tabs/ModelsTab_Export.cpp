// ModelsTab — export / dependency code, split out of ModelsTab.cpp to keep that translation unit
// smaller (faster incremental builds). These are ModelsTab member definitions plus a few file-local
// helpers used only here. The class declaration lives in ModelsTab.h.

#include "tabs/ModelsTab.h"

#include "app/Config.h"
#include "app/ExportNotifier.h"
#include "app/SehGuard.h"
#include "util/NameTemplate.h"
#include "tabs/BatchSink.h"
#include "casc/CascReader.h"
#include "gl/GLModelWidget.h"
#include "index/SnoIndex.h"
#include "index/SnoListModel.h"
#include "model/AnimParser.h"
#include "model/Hardpoints.h"
#include "model/Material.h"      // parseMaterialJson / MatTexture
#include "tabs/ModelOutliner.h"
#include <QDrag>
#include <QMimeData>
#include <QMutex>
#include <QThread>
#include <atomic>
#include <thread>
#include <vector>
#include <QUrl>  // partExportFlags (outliner camera toggles)
#include "model/ModelExporter.h"
#include "model/ModelGeometry.h"
#include "model/ModelParser.h"
#include "model/Retarget.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QItemSelectionModel>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QRegularExpression>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPair>
#include <QProgressDialog>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QString>
#include <QStringList>
#include <QTableView>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVector>

// Defined (external linkage) in ModelsTab.cpp — the default-look material roster + the .glb material
// builder, shared by single and batch export.
QStringList appearancePalette(const QString& d4, const QString& name);
QVector<ModelExporter::ExportMaterial> buildExportMats(
    const QStringList& palette, const ModelGeometry& geo, const QString& modelName,
    const QString& d4, CascReader* reader, bool wantTex);

// ── File-local export helpers (used only in this TU) ──────────────────────────

// Export the raw source files a model depends on — its .app payload plus each material's
// .tex textures — into `destDir`. These are the BLTE-decoded game payloads (the same bytes
// the tool reads) written with the conventional extension: the source assets behind the .glb.
static void exportModelDeps(quint64 modelSno, const QString& modelName,
                            const QStringList& palette, const QString& d4,
                            CascReader* reader, const QString& destDir)
{
    if (!reader || !reader->isReady() || modelName.isEmpty()) return;
    QDir().mkpath(destDir);
    const QByteArray app = reader->readPayloadBySno(modelSno);
    if (!app.isEmpty()) {
        QFile f(QDir(destDir).filePath(modelName + QStringLiteral(".app")));
        if (f.open(QIODevice::WriteOnly)) f.write(app);
    }
    QSet<qint64> doneTex;
    QSet<QString> seenMat;
    for (const QString& matName : palette) {
        if (matName.isEmpty() || seenMat.contains(matName)) continue;
        seenMat.insert(matName);
        QFile mf(QStringLiteral("%1/json/base/meta/Material/%2.mat.json").arg(d4, matName));
        if (!mf.open(QIODevice::ReadOnly)) continue;
        for (const MatTexture& mt : parseMaterialJson(mf.readAll())) {
            if (mt.texSno == 0 || doneTex.contains(mt.texSno)) continue;
            doneTex.insert(mt.texSno);
            const QByteArray tex = reader->readPayloadBySno(quint64(mt.texSno));
            if (tex.isEmpty()) continue;
            const QString tn = mt.texName.isEmpty() ? QStringLiteral("tex_%1").arg(mt.texSno) : mt.texName;
            QFile tf(QDir(destDir).filePath(tn + QStringLiteral(".tex")));
            if (tf.open(QIODevice::WriteOnly)) tf.write(tex);
        }
    }
}

// ── Fit-reference body (Settings ▸ Export ▸ Retarget & modding) ──────────────
// Append the matching base body mesh (e.g. "barF_base00" for "BarF_base14_TRS",
// resolved from the piece's name prefix) under exported player armor, as a separate
// "__fitReference" material. Joints are remapped by bone-name hash onto the piece's
// skeleton — requires a genuinely shared rig (≥50 matching bones), so weapons,
// monsters and props pass through untouched. Runs BEFORE the retarget transforms so
// the reference body participates in cloth-collapse / anchor-remap like everything else.
static bool appendFitReferenceBody(CascReader* reader, SnoListModel* list,
                                   const QString& pieceName, ModelGeometry& geo)
{
    if (!reader || !reader->isReady() || !list || geo.skeleton.isEmpty())
        return false;
    const int us = pieceName.indexOf(QLatin1Char('_'));
    if (us <= 0)
        return false;
    const QString bodyName = pieceName.left(us) + QStringLiteral("_base00");
    if (pieceName.compare(bodyName, Qt::CaseInsensitive) == 0)
        return false;                                   // exporting the body itself
    int sno = -1;
    for (int r = 0; r < list->rowCount(); ++r)
        if (const SnoEntry* e = list->entryAt(r))
            if (e->name.compare(bodyName, Qt::CaseInsensitive) == 0) { sno = e->snoId; break; }
    if (sno < 0)
        return false;
    ModelGeometry body;   // guarded: parsing an arbitrary reference-body appearance
    if (!seh::runGuarded("fitBody", [&]() {
            body = ModelParser::parseApp(reader->readMetaBySno(quint64(sno)),
                                         reader->readPayloadBySno(quint64(sno))); }))
        return false;
    if (!body.valid || body.skeleton.isEmpty() || body.primitives.isEmpty())
        return false;
    QHash<quint32, int> byHash;
    for (int i = 0; i < geo.skeleton.size(); ++i)
        byHash.insert(geo.skeleton[i].nameHash, i);
    QVector<int> map(body.skeleton.size(), -1);
    int shared = 0;
    for (int i = 0; i < body.skeleton.size(); ++i) {
        map[i] = byHash.value(body.skeleton[i].nameHash, -1);
        if (map[i] >= 0) ++shared;
    }
    if (shared < 50)
        return false;                                   // not the same rig family
    for (MeshPrimitive p : body.primitives) {           // copy each primitive
        p.materialName  = QStringLiteral("__fitReference");
        p.materialIndex = -1;                           // plain default material in the .glb
        for (MeshVertex& v : p.vertices) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const int nj = (v.weights[k] > 0.0f && v.joints[k] < map.size())
                                   ? map[v.joints[k]] : -1;
                if (nj < 0) { v.weights[k] = 0.0f; v.joints[k] = 0; }
                else        { v.joints[k] = quint16(nj); sum += v.weights[k]; }
            }
            if (sum > 1e-6f)
                for (int k = 0; k < 4; ++k) v.weights[k] /= sum;
        }
        geo.primitives.append(p);
    }
    return true;
}

// Human-readable byte size (used by the dependency view below).
static QString fmtBytes(quint64 b)
{
    if (b >= 1024ull * 1024) return QStringLiteral("%1 MB").arg(b / 1048576.0, 0, 'f', 1);
    if (b >= 1024)           return QStringLiteral("%1 KB").arg(b / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(b);
}

// ── ModelsTab member definitions ─────────────────────────────────────────────

void ModelsTab::exportSelectedGlb()
{
    // Distinct selected models (besides the current one) → batch export to a folder.
    QVector<QPair<int, QString>> others;   // (sno, name) excluding the current model
    if (m_list && m_list->selectionModel()) {
        for (const QModelIndex& idx : m_list->selectionModel()->selectedRows())
            if (const SnoEntry* e = m_listModel->entryAt(idx.row()))
                if (e->snoId != m_curSno) others.append({e->snoId, e->name});
    }

    if (!others.isEmpty()) {
        if (!m_reader || !m_reader->isReady()) {
            QMessageBox::warning(this, QStringLiteral("Batch export"),
                QStringLiteral("Open the game archive (File ▸ Settings) to batch-export."));
            return;
        }
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Export %1 model(s) to…").arg(others.size() + 1));
        if (dir.isEmpty())
            return;
        QVector<QPair<int, QString>> all;
        if (m_curSno >= 0 && !m_curName.isEmpty()) all.append({m_curSno, m_curName});
        all += others;
        exportModels(all, dir);
        return;
    }

    // ── Single current model: look-aware, visible parts, Save-As dialog. ──
    exportCurrentModelGlb(QVector<int>(), QString(), /*toLast=*/false);
}

// Write the CURRENT model as .glb. `keep` restricts the export to specific part indices (empty =
// every visible, export-enabled part); `label` overrides the suggested filename; `toLast` skips
// the Save-As dialog and reuses models/lastExportDir.
//
// The viewport context menu routes here rather than through exportSelectedGlb/exportSelectionToLast.
// Those two treat the LIST selection as the scope: they batch-export every selected row by SNO,
// re-parsing each from the archive. That path cannot see the parts tree at all, so "Export Part"
// isolated a part and then exported the whole model anyway — and "Export Model" on a multi-row
// selection exported models you never right-clicked.
void ModelsTab::exportCurrentModelGlb(const QVector<int>& keep, const QString& label, bool toLast)
{
    const QString d4 = Config::d4dataDir();
    const bool wantTex = m_exportTex && m_exportTex->isChecked() && m_reader && m_reader->isReady();
    if (!m_curGeo.valid || m_curGeo.primitives.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Export .glb"),
                             QStringLiteral("No exportable geometry for this model "
                                            "(select a mesh model with the game archive open)."));
        return;
    }
    ModelGeometry geo;
    geo.valid = true;
    geo.skeleton = m_curGeo.skeleton;
    geo.vertexBuffers = m_curGeo.vertexBuffers;
    // A part is exported iff it's visible AND its outliner camera-toggle is on — the camera
    // narrows the old "export what's visible" rule, it never widens it (default: all on).
    QHash<int, bool> expOn;
    if (m_treeModel) m_treeModel->partExportFlags(expOn);
    // An explicit `keep` is the user pointing at specific parts in the viewport menu, so it is
    // honoured verbatim — neither viewport visibility NOR the outliner camera/export toggle filters
    // it. Right-clicking a part and choosing Export exports that part, hidden or not. The
    // no-subset path is the "export what you see" scope and still applies both filters.
    if (keep.isEmpty()) {
        for (int i = 0; i < m_curGeo.primitives.size(); ++i)
            if ((!m_modelView || m_modelView->partVisible(i)) && expOn.value(i, true))
                geo.primitives.push_back(m_curGeo.primitives[i]);
    } else {
        for (int i : keep)
            if (i >= 0 && i < m_curGeo.primitives.size())
                geo.primitives.push_back(m_curGeo.primitives[i]);
    }
    if (geo.primitives.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Export .glb"),
                             QStringLiteral("Every part is hidden or export-disabled (camera "
                                            "toggle) — nothing to export."));
        return;
    }

    QString stem;
    if (label.isEmpty()) {
        // NameTemplate honours the user's export/nameModel pattern and does its own path-illegal
        // stripping. Sanitising it here would eat the spaces, brackets and separators the template
        // exists to produce, so only the context-menu `label` (a raw material name) is scrubbed.
        stem = m_curName.isEmpty() ? QStringLiteral("model") : NameTemplate::model(m_curName, m_curSno);
    } else {
        stem = label;
        stem.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]+")), QStringLiteral("_"));
    }
    if (stem.isEmpty()) stem = QStringLiteral("model");
    const QString lastDir = QSettings().value(QStringLiteral("models/lastExportDir")).toString();
    QString path;
    if (toLast && !lastDir.isEmpty()) {
        path = QDir(lastDir).filePath(stem + QStringLiteral(".glb"));
    } else {
        path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Export .glb"), QDir(lastDir).filePath(stem + QStringLiteral(".glb")),
            QStringLiteral("glTF binary (*.glb)"));
    }
    if (path.isEmpty())
        return;
    QSettings().setValue(QStringLiteral("models/lastExportDir"), QFileInfo(path).absolutePath());

    if (QSettings().value(QStringLiteral("retarget/fitReference"), false).toBool())
        appendFitReferenceBody(m_reader, m_listModel, m_curName, geo);

    // Active-look roster (index == materialIndex), so the export matches the preview.
    const QStringList palette(m_appMatNames.begin(), m_appMatNames.end());
    const QVector<ModelExporter::ExportMaterial> mats =
        buildExportMats(palette, geo, m_curName, d4, m_reader, wantTex);

    // Embed animations when "Anim" is ticked (scope from export settings: playing clip vs all).
    QVector<AnimParser::DecodedAnim> anims; QStringList animNames;
    if (m_exportAnim && m_exportAnim->isChecked()) collectExportAnims(anims, animNames);

    const ModelExporter::Options opt = ModelExporter::optionsFromSettings();
    if (QSettings().value(QStringLiteral("export/hardpointEmpties"), false).toBool())
        Hardpoints::readInto(geo, QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4, m_curName));
    Retarget::applyFromSettings(geo);                          // collapse cloth / remap to anchors
    if (opt.blenderFriendly)
        GLModelWidget::blenderizeSkeletonNames(geo.skeleton);  // .L/.R mirror-paired names
    else if (QSettings().value(QStringLiteral("export/boneNamesTranslated"), false).toBool())
        GLModelWidget::translateSkeletonNames(geo.skeleton);   // bone_<hash> → readable labels
    Hardpoints::resolveBoneIndices(geo);                       // re-index after any skeleton edit
    if (!ModelExporter::exportGlb(geo, path, mats, anims, animNames, opt)) {
        QMessageBox::warning(this, QStringLiteral("Export .glb"),
                             QStringLiteral("Failed to write %1.").arg(QFileInfo(path).fileName()));
        return;
    }

    // Optionally dump the raw source files (.app + .tex) into a "<name>_deps" subfolder.
    if (QSettings().value(QStringLiteral("export/withDeps"), false).toBool())
        exportModelDeps(quint64(m_curSno), m_curName, palette, d4, m_reader,
                        QFileInfo(path).dir().filePath(m_curName + QStringLiteral("_deps")));

    int tris = 0;
    for (const MeshPrimitive& p : geo.primitives) tris += p.indices.size() / 3;
    const bool remapNote = !anims.isEmpty()
        && QSettings().value(QStringLiteral("retarget/remapWeights"), false).toBool();
    ExportNotifier::instance().notify(
        QStringLiteral("Exported %1  ·  %2 parts, %3 tris%4")
            .arg(QFileInfo(path).fileName()).arg(geo.primitives.size()).arg(tris)
            .arg(remapNote ? QStringLiteral("  (rig reduced to 26 bones)") : QString()),
        QFileInfo(path).absolutePath());
}

// Batch-export a set of (sno, name) models to a folder, honoring the Tex/Anim
// export settings. The current model uses its look-aware, visible-parts geometry;
// the rest are parsed fresh with their default-look material roster.
// With retarget/setManifest on, the selection expands to the whole armor set
// (matching _HLM/_TRS/_GLV/_LEG/_BTS siblings) and a manifest.json is written.
// Bulk extractor: export a matched set to dir, reusing exportModels(). When onlyNew is on, skip items
// already extracted to that folder (by <name>.glb on disk OR listed in the _bulk_manifest.json ledger),
// then append the newly-written ones to the ledger. This is what makes "extract only the NEW armors
// after a patch" work: SNO ids are stable, so re-running the same query only exports what's missing.
void ModelsTab::bulkExport(const QVector<QPair<int, QString>>& items, const QString& dir, bool onlyNew,
                           const BatchSink* sink)
{
    if (items.isEmpty() || dir.isEmpty()) return;
    const QString manPath = QDir(dir).filePath(QStringLiteral("_bulk_manifest.json"));
    QJsonArray man;
    { QFile f(manPath); if (f.open(QIODevice::ReadOnly)) man = QJsonDocument::fromJson(f.readAll()).array(); }
    QSet<int> already;
    for (const QJsonValue& v : man) already.insert(v.toObject().value(QStringLiteral("sno")).toInt());

    QVector<QPair<int, QString>> todo;
    int skipped = 0;
    for (const auto& it : items) {
        if (onlyNew && (already.contains(it.first)
                        || QFileInfo::exists(QDir(dir).filePath(
                               NameTemplate::model(it.second, it.first) + QStringLiteral(".glb"))))) {
            ++skipped; continue;
        }
        todo.append(it);
    }
    if (todo.isEmpty()) {
        if (sink && sink->log)
            sink->log(QStringLiteral("Nothing new — all %1 matched item(s) already in this folder.")
                          .arg(items.size()));
        else
            QMessageBox::information(this, QStringLiteral("Bulk extract"),
                QStringLiteral("Nothing new to extract — all %1 matched item(s) are already in that folder.")
                    .arg(items.size()));
        return;
    }
    if (sink && sink->log && skipped > 0)
        sink->log(QStringLiteral("Skipping %1 already-present item(s).").arg(skipped));

    QStringList failReasons;
    exportModels(todo, dir, sink, &failReasons);   // hardened batch pipeline (per-item SEH guard)

    // Append the items that now have a .glb on disk to the ledger; anything without one failed.
    QStringList failed;
    for (const auto& it : todo) {
        if (already.contains(it.first)) continue;
        const QString glbFile = NameTemplate::model(it.second, it.first) + QStringLiteral(".glb");
        if (!QFileInfo::exists(QDir(dir).filePath(glbFile))) { failed << it.second; continue; }
        man.append(QJsonObject{{QStringLiteral("sno"),  it.first},
                               {QStringLiteral("name"), it.second},
                               {QStringLiteral("file"), glbFile},
                               {QStringLiteral("date"), QDateTime::currentDateTime().toString(Qt::ISODate)}});
        already.insert(it.first);
    }
    QFile mf(manPath);
    if (mf.open(QIODevice::WriteOnly | QIODevice::Truncate))
        mf.write(QJsonDocument(man).toJson(QJsonDocument::Indented));
    if (!failed.isEmpty() || !failReasons.isEmpty()) {
        // WHY each item failed — the reasoned list from the pipeline, plus any strays it missed.
        QStringList lines = failReasons;
        for (const QString& n : failed) {
            bool covered = false;
            for (const QString& r : failReasons) if (r.startsWith(n + QStringLiteral(" — "))) { covered = true; break; }
            if (!covered) lines << (n + QStringLiteral(" — no .glb produced"));
        }
        QFile ff(QDir(dir).filePath(QStringLiteral("_bulk_failed.txt")));
        if (ff.open(QIODevice::WriteOnly | QIODevice::Truncate)) ff.write(lines.join('\n').toUtf8());
    }
    const QString summary = QStringLiteral("Bulk extract: %1 exported, %2 already present, %3 failed%4.")
                                .arg(todo.size() - failed.size()).arg(skipped).arg(failed.size())
                                .arg(failed.isEmpty() ? QString() : QStringLiteral(" (see _bulk_failed.txt)"));
    if (sink && sink->log) sink->log(summary);
    emit scanStatus(summary);
}

void ModelsTab::exportModels(const QVector<QPair<int, QString>>& models, const QString& dir,
                             const BatchSink* sink, QStringList* failures)
{
    if (models.isEmpty() || dir.isEmpty() || !m_reader || !m_reader->isReady()) return;
    const QString d4 = Config::d4dataDir();
    // Flags come from SETTINGS, not the Models tab's mirror checkboxes — so the Bulk Extract
    // tab's own option toggles (which write these keys) govern its runs directly.
    const bool wantTex  = QSettings().value(QStringLiteral("export/includeTex"), true).toBool();
    const bool wantAnim = QSettings().value(QStringLiteral("export/includeAnim"), false).toBool();
    const bool animAll  = QSettings().value(QStringLiteral("export/animScope"), 0).toInt() == 1;
    const bool wantDeps = QSettings().value(QStringLiteral("export/withDeps"), false).toBool();
    const bool setAware = QSettings().value(QStringLiteral("retarget/setManifest"), false).toBool();
    // Batch animation export needs the clip index; kick it off if it hasn't run and say so ONCE.
    // (Marshaled to the GUI thread when this pipeline runs on the Bulk Extract worker — the scan
    // spins up its own machinery with GUI-thread affinity.)
    bool warnedAnimIdx = false;
    if (wantAnim && animAll && !m_animatedScanned) {
        if (QThread::currentThread() == thread()) ensureAnimatedIndex();
        else QMetaObject::invokeMethod(this, [this] { ensureAnimatedIndex(); }, Qt::QueuedConnection);
    }

    // Set-aware expansion: any selected piece named <base>_<slot> pulls in its missing
    // siblings across the five gear slots (name match against the model list).
    static const QStringList kSlots{QStringLiteral("HLM"), QStringLiteral("TRS"),
                                    QStringLiteral("GLV"), QStringLiteral("LEG"),
                                    QStringLiteral("BTS")};
    QVector<QPair<int, QString>> jobs = models;
    if (setAware && m_listModel) {
        QSet<int> have;
        for (const auto& m : jobs) have.insert(m.first);
        const int orig = jobs.size();
        for (int i = 0; i < orig; ++i) {
            const QString& nm = jobs[i].second;
            const int us = nm.lastIndexOf(QLatin1Char('_'));
            if (us <= 0) continue;
            const QString suf = nm.mid(us + 1).toUpper();
            if (!kSlots.contains(suf)) continue;
            for (const QString& s : kSlots) {
                if (s == suf) continue;
                const QString want = nm.left(us + 1) + s;
                for (int r = 0; r < m_listModel->rowCount(); ++r)
                    if (const SnoEntry* e = m_listModel->entryAt(r))
                        if (!have.contains(e->snoId)
                            && e->name.compare(want, Qt::CaseInsensitive) == 0) {
                            jobs.append({e->snoId, e->name});
                            have.insert(e->snoId);
                            break;
                        }
            }
        }
    }

    QJsonArray manifest;
    int ok = 0, fail = 0, step = 0;
    // Progress: the caller's sink (Bulk Extract's live console) when present, else the
    // classic modal dialog for the in-tab batch exports.
    std::unique_ptr<QProgressDialog> prog;
    if (!sink) {
        prog = std::make_unique<QProgressDialog>(QStringLiteral("Exporting models…"),
                                                 QStringLiteral("Cancel"), 0, jobs.size(), this);
        prog->setWindowModality(Qt::WindowModal);
    }
    QMutex shared;   // counters / manifest / failure list — shared across the parallel workers
    auto reportFail = [&](const QString& name, const QString& why) {
        QMutexLocker l(&shared);
        ++fail;
        if (failures) failures->append(QStringLiteral("%1 — %2").arg(name, why));
        if (sink && sink->log) sink->log(QStringLiteral("  ✗ %1 — %2").arg(name, why));
    };
    // One model, start to finish (parse → materials → optional anims → glb). Called from the
    // serial loop OR from N parallel workers — every shared-state mutation locks `shared`.
    auto processItem = [&](const QPair<int, QString>& m) {
        // ── Per-item HARDWARE-FAULT guard. The interactive load path wraps parse/decode in
        // seh::runGuarded, but this batch loop never did — so ONE malformed/encrypted model
        // (access violation in the parser or BC decoder) killed the whole app mid-run. Now a
        // bad item logs its fault and the run continues.
        bool wrote = false;
        QString glbPath = QDir(dir).filePath(NameTemplate::model(m.second, m.first) + QStringLiteral(".glb"));
        seh::HardwareFault fault;
        const bool survived = seh::runGuarded("bulk-export", [&]() {
            ModelGeometry geo;
            QStringList pal;
            // The current-model fast path reads GUI-owned state (outliner flags, viewport part
            // visibility) — only safe on the GUI thread. On the Bulk Extract worker, parse fresh
            // like any other item (same output, default parts).
            if (m.first == m_curSno && m_curGeo.valid && QThread::currentThread() == thread()) {
                geo.valid = true; geo.skeleton = m_curGeo.skeleton;
                geo.vertexBuffers = m_curGeo.vertexBuffers;
                QHash<int, bool> expOn;   // same rule as the single export: visible AND camera-on
                if (m_treeModel) m_treeModel->partExportFlags(expOn);
                for (int i = 0; i < m_curGeo.primitives.size(); ++i)
                    if ((!m_modelView || m_modelView->partVisible(i)) && expOn.value(i, true))
                        geo.primitives.push_back(m_curGeo.primitives[i]);
                pal = QStringList(m_appMatNames.begin(), m_appMatNames.end());
            } else {
                const QByteArray meta = m_reader->readMetaBySno(quint64(m.first));
                const QByteArray payload = m_reader->readPayloadBySno(quint64(m.first));
                if (payload.isEmpty()) { reportFail(m.second, QStringLiteral("no payload (encrypted or missing)")); return; }
                geo = ModelParser::parseApp(meta, payload);
                pal = appearancePalette(d4, m.second);
            }
            if (!geo.valid || geo.primitives.isEmpty()) {
                reportFail(m.second, QStringLiteral("no exportable geometry (parse failed or empty)"));
                return;
            }
            if (QSettings().value(QStringLiteral("retarget/fitReference"), false).toBool())
                appendFitReferenceBody(m_reader, m_listModel, m.second, geo);
            const auto mats = buildExportMats(pal, geo, m.second, d4, m_reader, wantTex);
            // Animations: the loaded model uses the interactive collector (honours the list
            // selection); every OTHER batch item decodes its own clip set against its own
            // skeleton — this is the fix for "bulk export never included animations".
            QVector<AnimParser::DecodedAnim> anims; QStringList animNames;
            if (wantAnim && m.first == m_curSno && QThread::currentThread() == thread()) {
                collectExportAnims(anims, animNames);   // reads the GUI anim-list selection
            } else if (wantAnim && animAll) {
                if (!m_animatedScanned) {
                    if (!warnedAnimIdx && sink && sink->log) {
                        sink->log(QStringLiteral("  ⚠ animation index still building — clips are "
                                                 "skipped this run; re-run once “Indexing” finishes"));
                        warnedAnimIdx = true;
                    }
                } else {
                    for (const QString& nm : animClipsFor(m.first, m.second.toLower())) {
                        const AnimParser::DecodedAnim a = decodeAnimForSkeleton(nm, geo);
                        if (a.valid && !a.bones.isEmpty()) { anims << a; animNames << nm; }
                    }
                    if (sink && sink->log && !anims.isEmpty())
                        sink->log(QStringLiteral("      + %1 animation clip(s)").arg(anims.size()));
                }
            }
            const ModelExporter::Options opt = ModelExporter::optionsFromSettings();
            if (QSettings().value(QStringLiteral("export/hardpointEmpties"), false).toBool())
                Hardpoints::readInto(geo, QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4, m.second));
            Retarget::applyFromSettings(geo);                          // collapse cloth / remap to anchors
            if (opt.blenderFriendly)
                GLModelWidget::blenderizeSkeletonNames(geo.skeleton);  // .L/.R mirror-paired names
            else if (QSettings().value(QStringLiteral("export/boneNamesTranslated"), false).toBool())
                GLModelWidget::translateSkeletonNames(geo.skeleton);   // bone_<hash> → readable labels
            Hardpoints::resolveBoneIndices(geo);                       // re-index after any skeleton edit
            if (ModelExporter::exportGlb(geo, glbPath, mats, anims, animNames, opt)) {
                wrote = true;
                {
                    QMutexLocker l(&shared);
                    ++ok;
                    if (sink && sink->log) {
                        int nv = 0;
                        for (const MeshPrimitive& p : geo.primitives) nv += int(p.vertices.size());
                        sink->log(QStringLiteral("  ✓ %1  (%2 parts, %3 verts)")
                                      .arg(m.second).arg(geo.primitives.size()).arg(nv));
                    }
                    if (setAware) {
                        const int us = m.second.lastIndexOf(QLatin1Char('_'));
                        const QString suf = us > 0 ? m.second.mid(us + 1).toUpper() : QString();
                        QJsonObject e{{"file", NameTemplate::model(m.second, m.first) + QStringLiteral(".glb")},
                                      {"sno", m.first},
                                      {"name", m.second}};
                        if (kSlots.contains(suf)) e["slot"] = suf;
                        manifest.append(e);
                    }
                }
                if (wantDeps) exportModelDeps(quint64(m.first), m.second, pal, d4, m_reader,
                                              QDir(dir).filePath(m.second + QStringLiteral("_deps")));
            } else {
                reportFail(m.second, QStringLiteral("glb write failed"));
            }
        }, &fault);
        if (!survived) {
            if (wrote) {   // .glb landed but a post-step (deps dump) faulted — not a lost item
                if (sink && sink->log)
                    sink->log(QStringLiteral("  ⚠ %1 exported, but a post-export step crashed (%2)")
                                  .arg(m.second, fault.what));
            } else {
                reportFail(m.second, QStringLiteral("CRASHED (%1) — skipped, run continues")
                                         .arg(fault.what.isEmpty() ? QStringLiteral("hardware fault")
                                                                   : fault.what));
            }
        }
    };   // processItem

    // Parallel model export (Bulk Extract runs): geometry/material/glb work per item is fully
    // independent, so it parallelizes with byte-identical output. Animation and fit-reference
    // exports stay serial — those paths share caches/GUI state the workers must not race on.
    const bool fitRef = QSettings().value(QStringLiteral("retarget/fitReference"), false).toBool();
    int par = 1;
    if (sink) {
        const int cfg = QSettings().value(QStringLiteral("bulk/parallel"), -1).toInt();
        par = qBound(1, cfg <= 0 ? QThread::idealThreadCount() : cfg, 16);
    }
    if (par > 1 && (wantAnim || fitRef)) {
        par = 1;
        if (sink && sink->log)
            sink->log(QStringLiteral("   (parallel off for this run — animation / fit-reference exports are serial-only)"));
    }
    if (par > 1 && jobs.size() > 1) {
        std::atomic<int> next{0}, done{0};
        auto workerFn = [&]() {
            for (;;) {
                if (sink->canceled && sink->canceled()) break;   // also holds here while paused
                const int i = next.fetch_add(1);
                if (i >= jobs.size()) break;
                processItem(jobs[i]);
                const int d = done.fetch_add(1) + 1;
                if (sink->progress) sink->progress(d, jobs.size());
            }
        };
        std::vector<std::thread> pool;
        pool.reserve(par);
        for (int t = 0; t < par; ++t) pool.emplace_back(workerFn);
        for (auto& th : pool) th.join();
        step = done.load();
    } else {
        for (const auto& m : jobs) {
            if (prog && prog->wasCanceled()) break;
            if (sink && sink->canceled && sink->canceled()) break;
            if (prog) {
                prog->setValue(step);
                prog->setLabelText(QStringLiteral("Exporting %1…").arg(m.second));
            }
            if (sink && sink->progress) sink->progress(step, jobs.size());
            ++step;
            if (!sink) QCoreApplication::processEvents();   // legacy modal path only
            processItem(m);
        }
    }
    if (prog) prog->setValue(jobs.size());
    if (sink && sink->progress) sink->progress(step, jobs.size());   // final tick
    if (setAware && !manifest.isEmpty()) {
        // Reproducible re-exports after game patches: what was exported, from which SNOs.
        QJsonObject root{{"generator", QStringLiteral("Diablo4AssetBrowserNative")},
                         {"exported", QDateTime::currentDateTime().toString(Qt::ISODate)},
                         {"models", manifest}};
        QFile mf(QDir(dir).filePath(QStringLiteral("manifest.json")));
        if (mf.open(QIODevice::WriteOnly))
            mf.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    }
    QSettings().setValue(QStringLiteral("models/lastExportDir"), dir);
    ExportNotifier::instance().notify(
        QStringLiteral("Exported %1 model(s)%2%3")
            .arg(ok).arg(fail ? QStringLiteral(", %1 failed").arg(fail) : QString(),
                 setAware && !manifest.isEmpty() ? QStringLiteral("  (+ manifest.json)") : QString()),
        dir);
}

// Read-only dependency view: model .app → its materials → each material's .tex textures,
// with stored (index) sizes. Reuses the same .mat.json → MatTexture resolution as the raw
// dependency export.
void ModelsTab::showModelDependencies(int sno, const QString& name) { showDependencies(sno, name); }

void ModelsTab::showDependencies(int sno, const QString& name)
{
    if (!m_reader || !m_reader->isReady()) return;
    const QString d4 = Config::d4dataDir();
    const QStringList palette = (sno == m_curSno)
        ? QStringList(m_appMatNames.begin(), m_appMatNames.end())
        : appearancePalette(d4, name);

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Dependencies — %1").arg(name));
    dlg.resize(600, 500);
    auto* lay = new QVBoxLayout(&dlg);
    auto* summary = new QLabel(&dlg);
    lay->addWidget(summary);
    auto* tree = new QTreeWidget(&dlg);
    tree->setHeaderLabels({QStringLiteral("Dependency"), QStringLiteral("SNO"), QStringLiteral("Size")});
    tree->setColumnWidth(0, 340);
    // This list is reference data users paste into notes / bug reports, so make it fully copyable:
    // multi-select rows, Ctrl+C, a right-click Copy / Copy-all, and a Copy-all button (below).
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    lay->addWidget(tree, 1);

    // One item → a tab-separated line ("<indent><name>\t<sno>\t<size>"), empty columns dropped.
    auto itemLine = [](QTreeWidgetItem* it) -> QString {
        int depth = 0; for (QTreeWidgetItem* p = it->parent(); p; p = p->parent()) ++depth;
        QStringList cols;
        for (int c = 0; c < it->columnCount(); ++c) { const QString t = it->text(c); if (!t.isEmpty()) cols << t; }
        return QString(depth * 2, QLatin1Char(' ')) + cols.join(QLatin1Char('\t'));
    };
    // The tree is exactly 3 levels deep (model → material → texture), so walk it with nested loops.
    auto copyAll = [tree, itemLine]() {
        QStringList lines;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* a = tree->topLevelItem(i); lines << itemLine(a);
            for (int j = 0; j < a->childCount(); ++j) {
                QTreeWidgetItem* b = a->child(j); lines << itemLine(b);
                for (int k = 0; k < b->childCount(); ++k) lines << itemLine(b->child(k));
            }
        }
        if (!lines.isEmpty()) QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    };
    auto copySelected = [tree, itemLine, copyAll]() {
        const QList<QTreeWidgetItem*> sel = tree->selectedItems();
        if (sel.isEmpty()) { copyAll(); return; }   // nothing selected → copy the whole list
        QStringList lines; for (QTreeWidgetItem* it : sel) lines << itemLine(it);
        QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    };
    auto* copySc = new QShortcut(QKeySequence::Copy, tree);   // Ctrl+C
    QObject::connect(copySc, &QShortcut::activated, tree, copySelected);
    QObject::connect(tree, &QTreeWidget::customContextMenuRequested, tree, [tree, copySelected, copyAll](const QPoint& p) {
        QMenu m(tree);
        m.addAction(QStringLiteral("Copy"), copySelected);
        m.addAction(QStringLiteral("Copy all"), copyAll);
        m.exec(tree->viewport()->mapToGlobal(p));
    });

    auto* modelItem = new QTreeWidgetItem(tree, {name + QStringLiteral(".app"),
        QString::number(sno), fmtBytes(m_reader->payloadSize(quint64(sno)))});
    modelItem->setExpanded(true);

    QSet<QString> seenMat;
    QSet<qint64>  uniqueTex; quint64 texTotal = 0;
    for (const QString& matName : palette) {
        if (matName.isEmpty() || seenMat.contains(matName)) continue;
        seenMat.insert(matName);
        QFile mf(QStringLiteral("%1/json/base/meta/Material/%2.mat.json").arg(d4, matName));
        if (!mf.open(QIODevice::ReadOnly)) continue;
        auto* matItem = new QTreeWidgetItem(modelItem, {matName, QString(), QString()});
        matItem->setExpanded(true);
        for (const MatTexture& mt : parseMaterialJson(mf.readAll())) {
            if (mt.texSno == 0) continue;
            const quint64 sz = m_reader->payloadSize(quint64(mt.texSno));
            const QString tn = mt.texName.isEmpty() ? QStringLiteral("tex_%1").arg(mt.texSno) : mt.texName;
            new QTreeWidgetItem(matItem, {QStringLiteral("%1.tex  (%2)").arg(tn, mt.role),
                                          QString::number(mt.texSno), fmtBytes(sz)});
            if (!uniqueTex.contains(mt.texSno)) { uniqueTex.insert(mt.texSno); texTotal += sz; }
        }
    }
    summary->setText(QStringLiteral("%1 material(s) · %2 unique texture(s) · %3 total (stored)")
                         .arg(seenMat.size()).arg(uniqueTex.size()).arg(fmtBytes(texTotal)));

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    auto* copyBtn = bb->addButton(QStringLiteral("Copy all"), QDialogButtonBox::ActionRole);
    copyBtn->setToolTip(QStringLiteral("Copy the whole dependency list to the clipboard (Ctrl+C copies the selection)"));
    QObject::connect(copyBtn, &QPushButton::clicked, &dlg, copyAll);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    lay->addWidget(bb);
    dlg.exec();
}

// Export the RIG + selected animations only (no mesh) — a Blender-ready clip library.
void ModelsTab::exportAnimationsOnly(bool toLast)
{
    if (!m_curGeo.valid || m_curGeo.skeleton.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Export animations"),
            QStringLiteral("Load a rigged model first (the skeleton comes from the current model)."));
        return;
    }
    QVector<AnimParser::DecodedAnim> anims; QStringList names;
    collectExportAnims(anims, names);   // multi-selection → scope → playing clip
    if (anims.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Export animations"),
            QStringLiteral("No animations selected. Ctrl/Shift-select one or more clips in the "
                           "ANIMATIONS list (or play one), then export."));
        return;
    }
    const QString d4 = Config::d4dataDir();
    const QString suggested = (m_curName.isEmpty() ? QStringLiteral("model") : m_curName)
                              + QStringLiteral("_anims.glb");
    QString path;
    if (toLast) {
        const QString dir = QSettings().value(QStringLiteral("models/animExportDir")).toString();
        if (dir.isEmpty()) { exportAnimationsOnly(false); return; }   // nothing remembered → prompt
        path = QDir(dir).filePath(suggested);
    } else {
        path = QFileDialog::getSaveFileName(this, QStringLiteral("Export animations only"),
                                            suggested, QStringLiteral("glTF binary (*.glb)"));
    }
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".glb"), Qt::CaseInsensitive)) path += QStringLiteral(".glb");

    ModelGeometry geo;                  // rig only — no primitives
    geo.valid = true;
    geo.skeleton = m_curGeo.skeleton;
    geo.nBaseBones = m_curGeo.nBaseBones;
    const ModelExporter::Options opt = ModelExporter::optionsFromSettings();
    if (QSettings().value(QStringLiteral("export/hardpointEmpties"), false).toBool())
        Hardpoints::readInto(geo, QStringLiteral("%1/json/base/meta/Appearance/%2.app.json").arg(d4, m_curName));
    // No retarget on a clip library: remap/collapse would drop the very bones the clips drive.
    if (opt.blenderFriendly)
        GLModelWidget::blenderizeSkeletonNames(geo.skeleton);
    else if (QSettings().value(QStringLiteral("export/boneNamesTranslated"), false).toBool())
        GLModelWidget::translateSkeletonNames(geo.skeleton);
    Hardpoints::resolveBoneIndices(geo);
    const bool ok = ModelExporter::exportGlb(geo, path, {}, anims, names, opt);
    if (ok) {
        const QString folder = QFileInfo(path).absolutePath();
        QSettings().setValue(QStringLiteral("models/animExportDir"), folder);  // for "to last dir"
        ExportNotifier::instance().notify(
            QStringLiteral("Exported %1 animation clip(s), rig only").arg(anims.size()), folder);
    } else {
        QMessageBox::warning(this, QStringLiteral("Export animations"), QStringLiteral("Export failed."));
    }
}

// ── Drag-out export: the selected model row(s) leave the app as real .glb files ──────────────
// Exported to a temp folder at drag start (synchronous, wait cursor — a second or two with
// textures), then handed to the OS drag as file URLs, so Explorer/Blender receive a normal
// file drop. Capped so a 500-row selection can't accidentally trigger a batch export.
void ModelsTab::startModelDrag()
{
    if (!m_list || !m_listModel) return;
    QVector<QPair<int, QString>> items;
    for (const QModelIndex& ix : m_list->selectionModel()->selectedRows(0)) {
        if (ix.parent().isValid()) continue;
        if (const SnoEntry* e = m_listModel->entryAt(ix.row()))
            items.append({e->snoId, e->name});
    }
    if (items.isEmpty()) return;
    if (items.size() > 10) {
        showToast(QStringLiteral("Drag-out is capped at 10 models — use Export for big batches"));
        return;
    }
    const QString dir = QDir::temp().filePath(QStringLiteral("d4ab_drag"));
    QDir().mkpath(dir);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    exportModels(items, dir);
    QApplication::restoreOverrideCursor();
    QList<QUrl> urls;
    for (const auto& m : items) {
        const QString p = QDir(dir).filePath(m.second + QStringLiteral(".glb"));
        if (QFileInfo::exists(p)) urls << QUrl::fromLocalFile(p);
    }
    if (urls.isEmpty()) {
        showToast(QStringLiteral("Nothing exportable in the selection"));
        return;
    }
    auto* mime = new QMimeData;
    mime->setUrls(urls);
    auto* drag = new QDrag(m_list);
    drag->setMimeData(mime);
    drag->exec(Qt::CopyAction);
}

GLModelWidget* ModelsTab::previewWidget() { return m_modelView; }

bool ModelsTab::hasExportSelection() const
{
    if (m_curGeo.valid && !m_curGeo.primitives.isEmpty()) return true;
    return m_list && m_list->selectionModel() && !m_list->selectionModel()->selectedRows().isEmpty();
}

void ModelsTab::exportSelection() { exportSelectedGlb(); }   // prompts (batch → folder, single → Save As)

QString ModelsTab::exportNoun() const
{
    int n = 0;
    if (m_list && m_list->selectionModel()) n = m_list->selectionModel()->selectedRows().size();
    if (n == 0 && m_curGeo.valid) n = 1;
    return n > 1 ? QStringLiteral("selected models") : QStringLiteral("selected model");
}

// Rig-only animation export is available once a rigged model is loaded.
bool ModelsTab::hasAnimExport() const
{
    return m_curGeo.valid && !m_curGeo.skeleton.isEmpty();
}

// Count-aware label: singular when one clip (or the playing one) is targeted, else plural.
QString ModelsTab::animExportLabel() const
{
    int n = m_anims ? m_anims->selectedItems().size() : 0;
    if (n == 0 && !m_playingAnim.isEmpty()) n = 1;
    return n == 1 ? QStringLiteral("Export animation only (.glb)…")
                  : QStringLiteral("Export animations only (.glb)…");
}

void ModelsTab::exportAnimations() { exportAnimationsOnly(); }

void ModelsTab::exportSelectionToLast()
{
    const QString dir = QSettings().value(QStringLiteral("models/lastExportDir")).toString();
    if (dir.isEmpty()) { exportSelectedGlb(); return; }   // no remembered folder → prompt
    QVector<QPair<int, QString>> all;
    if (m_curSno >= 0 && !m_curName.isEmpty()) all.append({m_curSno, m_curName});
    if (m_list && m_list->selectionModel())
        for (const QModelIndex& idx : m_list->selectionModel()->selectedRows())
            if (const SnoEntry* e = m_listModel->entryAt(idx.row()))
                if (e->snoId != m_curSno) all.append({e->snoId, e->name});
    if (all.isEmpty()) { exportSelectedGlb(); return; }
    // Only a multi-row selection is a batch. For the current model alone, go through the
    // look-aware, visible-parts writer — exportModels() re-parses from the archive with the
    // DEFAULT look and every part, which is not the model the user is looking at.
    if (all.size() == 1 && all.first().first == m_curSno) {
        exportCurrentModelGlb(QVector<int>(), QString(), /*toLast=*/true);
        return;
    }
    exportModels(all, dir);
}
