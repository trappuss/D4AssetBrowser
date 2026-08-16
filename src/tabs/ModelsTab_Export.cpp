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
#include "model/MaterialDecode.h"
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
#include <QElapsedTimer>
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
#include <QTextStream>
#include "util/AnimExportScope.h"   // which animation sources this run embeds
// Folder layout for a multi-model run, shared with Bulk Extract.
#include "util/ExportLayout.h"
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
// reader/idx/sno let it fall back to the CASC meta binary when there is no .app.json — without
// them an encrypted or newly-patched appearance yields an empty palette and exports untextured.
QStringList appearancePalette(const QString& d4, const QString& name,
                              CascReader* reader = nullptr, const SnoIndex* idx = nullptr,
                              qint64 sno = 0);
QVector<ModelExporter::ExportMaterial> buildExportMats(
    const QStringList& palette, const ModelGeometry& geo, const QString& modelName,
    const QString& d4, CascReader* reader, bool wantTex);

// ── File-local export helpers (used only in this TU) ──────────────────────────

// Appearance group id. kGroupAppearance is file-local to ModelsTab.cpp and not visible here,
// so it is repeated rather than reached for — the alternative is exporting a constant purely
// to share a 9.
constexpr int kApprGroup = 9;

// Opposite-gender twin of a D4 appearance name, or empty when there isn't one.
// Names encode gender as the 4th character of the class prefix: barF_stor23_TRS ↔ barM_stor23_TRS.
// Case is preserved so the twin matches the index's own spelling.
static QString genderTwinName(const QString& name)
{
    if (name.size() < 4) return {};
    const QChar g = name.at(3);
    const QChar lower = g.toLower();
    if (lower != QLatin1Char('m') && lower != QLatin1Char('f')) return {};
    QString twin = name;
    const QChar other = (lower == QLatin1Char('m')) ? QLatin1Char('f') : QLatin1Char('m');
    twin[3] = g.isUpper() ? other.toUpper() : other;
    return twin;
}


// Export the raw source files a model depends on — its .app payload plus each material's
// .tex textures — into `destDir`. These are the BLTE-decoded game payloads (the same bytes
// the tool reads) written with the conventional extension: the source assets behind the .glb.
// Raw source files (.app + every .tex its materials reference) into `destDir`.
//
// destDir is now ONE "deps" folder per output folder, shared by every model written beside it,
// rather than a "<model>_deps" folder each. That is what makes it follow the Bulk Extract layout:
// Flat puts every model's sources in <out>/deps, by-Class in <out>/<Class>/deps, and by-Model in
// <out>/<model>/deps — which is the per-model layout the old code hardcoded, now reachable by
// choosing it rather than imposed on all three.
//
// Sharing the folder deduplicates: models in a run reference the same textures constantly, and a
// .tex already on disk is the same bytes whoever asked for it, so it is skipped rather than
// rewritten. The .app files stay per-model and name their own materials, so which model pulled
// what is still recoverable.
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
    // One deps folder is now shared by every model in an output folder, and processItem() runs on a
    // thread pool — so two workers can reach the same .tex path at once. The check-then-write below
    // is a TOCTOU: open(WriteOnly) truncates, so the loser sees size 0, decides the file is missing,
    // and both write over each other. Serialised here rather than by forcing par=1, because this
    // holds only for the length of one file write and the decode work stays parallel.
    static QMutex depsMx;
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
            const QString tn = mt.texName.isEmpty() ? QStringLiteral("tex_%1").arg(mt.texSno) : mt.texName;
            const QString tp = QDir(destDir).filePath(tn + QStringLiteral(".tex"));
            // Checked BEFORE the CASC read: written by an earlier model in this same folder means
            // identical payload, so both the read and the write are pure cost. This is the dedup
            // that sharing one deps folder buys.
            //
            // Size, not mere existence. QFile::open(WriteOnly) creates and truncates first, so a run
            // killed mid-write leaves a 0-byte .tex — and an existence test would then treat that
            // corpse as done forever, in every later run, with nothing in the log. Anything non-empty
            // is a complete payload, because the write is a single call.
            {
                QMutexLocker lock(&depsMx);
                if (QFileInfo(tp).size() > 0) continue;
                const QByteArray tex = reader->readPayloadBySno(quint64(mt.texSno));
                if (tex.isEmpty()) continue;
                QFile tf(tp);
                if (tf.open(QIODevice::WriteOnly)) tf.write(tex);
            }
        }
    }
}

// ── Loose textures beside the model (Settings ▸ Export ▸ Model export) ───────
// Writes each material's maps as PNGs into `<dir>/textures/`, named
// <model>_<material>_<map>.png.
//
// ADDITIVE, never a substitute: the .glb keeps its embedded copies, so the file stays
// self-contained and drag-and-drop into Blender behaves exactly as before. Turning this on can
// therefore never break an existing workflow — the worst case is unused files on disk. That is the
// whole reason it does not attempt to rewrite the glTF to reference external URIs, which would
// change what a .glb IS and break every caller that assumes it is standalone.
//
// Costs nothing extra: these QImages were already decoded to be embedded, so this is a re-save,
// not a second decode. Skips silently when the setting is off or nothing was resolved — with
// "include textures" off there are no images to write, which is correct rather than an error.
// Write the material textures as loose PNGs beside the .glb.
//
// TWO SCOPES, because "the textures for this model" has two honest answers:
//
//   export/looseTextures     the four maps the .glb itself uses — basecolor / normal / orm /
//                            emissive, as COMPOSITED for glTF. Written to textures\.
//   export/looseTexturesAll  EVERY texture the materials reference, as the GAME stores them.
//                            D4 binds up to ten per material: the four above plus up to three
//                            tiled detail normals, their detail roughness maps, translucency, a
//                            cutout mask, the dye mask and a dye ramp. glTF has no slot for any of
//                            the latter, so files are the only way they leave the tool. Written to
//                            textures\material_maps\.
//
// INDEPENDENT, not nested. Either one alone produces output — asking for every material map is a
// complete request on its own, and making it wait on the other setting would be a switch that
// silently does nothing. Separate folders because the two sets overlap by ROLE (both contain a
// normal map) but not by content: one is reconstructed for glTF, the other is the raw game map.
// Sharing a folder meant whichever was written first won and the other was silently dropped.
//
// Read through MaterialDecode::texturesFor(), which its own header calls the only correct way to
// read a material's textures (it prefers the .mat.json and falls back to the binary, so encrypted
// and snapshot-missing materials still resolve). Names carry the shader ROLE, and a manifest
// records slot, role, SNO and the per-map UV tiling — detail maps tile 8-20x, so a bare PNG
// without its scale is not reconstructable.
static void writeLooseTextures(const QVector<ModelExporter::ExportMaterial>& mats,
                               const QString& dir, const QString& modelName,
                               CascReader* reader, const QString& d4)
{
    const bool four = QSettings().value(QStringLiteral("export/looseTextures"), false).toBool();
    const bool all  = QSettings().value(QStringLiteral("export/looseTexturesAll"), false).toBool();
    if ((!four && !all) || mats.isEmpty() || dir.isEmpty()) return;
    const QString texDir = QDir(dir).filePath(QStringLiteral("textures"));
    const QString rawDir = QDir(texDir).filePath(QStringLiteral("material_maps"));
    int written = 0, extra = 0;
    QSet<QString> done;   // one material can appear at several indices; write each map once
    // Two materials on one model routinely bind the SAME texture — one packed map serving both
    // ROUGHNESS and METALLIC, a shared detail map, a shared dye ramp. The filename carries the
    // material name, so each of those is a separate file, and each used to pay its own decode and
    // its own PNG encode for identical bytes.
    //
    // Both files are still written: skipping the second one loses a map that a consumer looking for
    // <model>_<material>_metallic.png expects, and textures.txt is truncated per model in a batch
    // so the row pointing at the other file would not survive the run either. What is shared is the
    // WORK — encode once, write the same bytes twice. The output is byte-for-byte what it was.
    // Keyed on sno AND name, matching MaterialDecode::texture()'s own cache key — NOT on the sno
    // alone. One sno can decode two different ways: a material with a .mat.json supplies a real
    // texture name and the dimensions come from <name>.tex.json, while an encrypted material takes
    // the binary route with an empty name and the dimensions come from the CASC texture tables,
    // after a payload redirect. Keying on the sno would let the first spelling's bytes be written
    // under the second's filename — the same collision texture()'s key exists to prevent.
    // Remembers the FILENAME each texture was first written to, not its bytes. The duplicate is
    // then produced with a file copy, which costs no memory at all and beats re-running the PNG
    // compressor on a 2048² map by an order of magnitude.
    //
    // This started out holding the encoded bytes, capped at 32 MB per call. That cap was the wrong
    // shape: writeLooseTextures() runs on up to 16 export-pool threads at once, so the honest
    // worst case was half a gigabyte of PNG buffers held to avoid an encode. A few short strings
    // and a QFile::copy do the same job.
    QHash<QString, QString> fileByTex;
    int reused = 0;
    QStringList manifest;

    // `sno` > 0 opts into the encode cache: the raw-map pass passes the texture's sno, the
    // composited four-map pass passes 0 because its images are built per material and share no
    // identity. Encoding to bytes rather than straight to a file is what lets the second write of
    // the same texture skip the PNG compressor.
    auto save = [&](const QImage& img, const QString& intoDir, const QString& base,
                    const QString& suffix, const QString& texKey = QString()) {
        if (img.isNull()) return QString();
        const QString fn = QStringLiteral("%1_%2_%3.png").arg(modelName, base, suffix);
        const QString key = intoDir + QLatin1Char('|') + fn;
        if (done.contains(key)) return QString();
        done.insert(key);
        QDir().mkpath(intoDir);   // only reached when there is content to put in it
        const QString path = QDir(intoDir).filePath(fn);
        // Same texture, second material, second filename: copy the file we already wrote. Identical
        // bytes, no encode, no buffer. QFile::copy will not overwrite, so clear any stale file from
        // an earlier run first — the export otherwise silently keeps the old one.
        if (!texKey.isEmpty()) {
            const auto it = fileByTex.constFind(texKey);
            if (it != fileByTex.constEnd()) {
                QFile::remove(path);
                if (!QFile::copy(QDir(intoDir).filePath(it.value()), path)) return QString();
                ++reused; ++written;
                return fn;
            }
        }
        if (!img.save(path, "PNG")) return QString();
        if (!texKey.isEmpty()) fileByTex.insert(texKey, fn);
        ++written;
        return fn;
    };

    for (const ModelExporter::ExportMaterial& em : mats) {
        const QString base = em.name.isEmpty() ? modelName : em.name;
        if (four) {
            const struct { const QImage* img; const char* suffix; } kMaps[] = {
                {&em.baseColor, "basecolor"}, {&em.normal, "normal"},
                {&em.orm,       "orm"},       {&em.emissive, "emissive"},
            };
            for (const auto& mp : kMaps) save(*mp.img, texDir, base, QLatin1String(mp.suffix));
        }

        if (!all || em.name.isEmpty() || !reader) continue;
        // Every remaining bound texture, by role. Decoded here rather than reused from the four
        // above because those have been composited for glTF (ORM packed, normal reconstructed) —
        // these are the maps as the game stores them.
        for (const MatTexture& mt : MaterialDecode::texturesFor(reader, d4, em.name)) {
            QString role = mt.role.isEmpty() ? QStringLiteral("slot%1").arg(mt.slot) : mt.role.toLower();
            role.replace(QLatin1Char(' '), QLatin1Char('_'));
            const QImage img = MaterialDecode::texture(reader, d4, mt.texName, mt.texSno);
            if (img.isNull()) continue;
            // Roles repeat (three DETAIL_NORMAL slots), so disambiguate rather than overwrite.
            QString suffix = role;
            for (int n = 2; done.contains(rawDir + QLatin1Char('|')
                     + QStringLiteral("%1_%2_%3.png").arg(modelName, base, suffix)); ++n)
                suffix = QStringLiteral("%1_%2").arg(role).arg(n);
            const QString fn = save(img, rawDir, base, suffix,
                                    QString::number(mt.texSno) + QLatin1Char('|') + mt.texName);
            if (fn.isEmpty()) continue;
            ++extra;
            manifest << QStringLiteral("%1\t%2\tslot=%3\trole=%4\tsno=%5\tuScale=%6\tvScale=%7\t%8")
                            .arg(fn, em.name).arg(mt.slot).arg(mt.role.isEmpty() ? QStringLiteral("?") : mt.role)
                            .arg(mt.texSno).arg(mt.uScale).arg(mt.vScale).arg(mt.texName);
        }
    }

    if (!manifest.isEmpty()) {
        QFile mf(QDir(rawDir).filePath(QStringLiteral("textures.txt")));
        if (mf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&mf);
            ts << "file\tmaterial\tslot\trole\tsno\tuScale\tvScale\ttexture\n";
            for (const QString& l : manifest) ts << l << '\n';
        }
    }
    // Says so either way — a silent zero here reads as "the setting does nothing".
    qInfo("export: loose textures — %d file(s) for %s (%d material maps%s)%s",
          written, qUtf8Printable(modelName), extra,
          reused ? qUtf8Printable(QStringLiteral(", %1 copied instead of re-encoded").arg(reused))
                 : "",
          written ? "" : " (nothing resolved; is 'include textures' on?)");
}

// ── Base body ("Include base body", Settings ▸ Export ▸ Model export) ────────────────────────────
//
// Joints are remapped by bone-name hash onto the piece's skeleton — requires a genuinely shared rig
// (≥50 matching bones), so weapons, monsters and props pass through untouched. Runs BEFORE the
// retarget transforms, so the body participates in cloth-collapse / anchor-remap like everything
// else in the file.
//
// THE NUDE BODY IS THE test999 SUITE, not <class><gender>_base00.
//
// Measured in d4data: barF_test999_TRS carries exactly ONE material, `armor_skin_mat` — bare skin.
// barF_base00 carries barF_base00_BTS_mat / _LEG_mat / _TRS_mat, barbarianF_wrath_mat, a hair
// material, fur meshes, fx meshes and sim cages: it is the class's fully-dressed default rig, not
// a body. Appending THAT under an armour piece is what put a whole untextured barbarian in the
// file — the bug this rewrite fixes, not a cosmetic rename.
//
// The suite is four slots (TRS/GLV/LEG/BTS) per class+gender; there is no test999 head, so the
// reference is body-only, which is what a fit check wants.
//
// All four are merged into ONE primitive under a single `__baseBody` material, so in Blender it
// arrives as one material slot you can select and delete in a single action instead of four.
static bool appendBaseBody(CascReader* reader, SnoListModel* list,
                           const QString& pieceName, ModelGeometry& geo)
{
    if (!reader || !reader->isReady() || !list || geo.skeleton.isEmpty())
        return false;
    const int us = pieceName.indexOf(QLatin1Char('_'));
    if (us <= 0)
        return false;
    const QString prefix = pieceName.left(us);          // "barF", "sorM", …
    if (pieceName.contains(QLatin1String("_test999"), Qt::CaseInsensitive))
        return false;                                   // exporting the base body itself

    // Resolve the suite by name. Missing slots are skipped rather than fatal: a class that ships
    // only some of them still gets a usable reference.
    static const char* kSlots[] = {"TRS", "GLV", "LEG", "BTS"};
    QVector<int> snos;
    for (const char* slot : kSlots) {
        const QString want = prefix + QStringLiteral("_test999_") + QString::fromLatin1(slot);
        for (int r = 0; r < list->rowCount(); ++r)
            if (const SnoEntry* e = list->entryAt(r))
                if (e->name.compare(want, Qt::CaseInsensitive) == 0) { snos << e->snoId; break; }
    }
    if (snos.isEmpty()) {
        qInfo("export: base body — no %s_test999_* pieces in the index; nothing appended",
              qUtf8Printable(prefix));
        return false;
    }

    QHash<quint32, int> byHash;
    for (int i = 0; i < geo.skeleton.size(); ++i)
        byHash.insert(geo.skeleton[i].nameHash, i);

    MeshPrimitive merged;
    merged.materialName  = QStringLiteral("__baseBody");
    merged.materialIndex = -1;                          // plain default material in the .glb
    int usedPieces = 0;
    for (int sno : snos) {
        ModelGeometry body;   // guarded: parsing an arbitrary reference-body appearance
        if (!seh::runGuarded("baseBody", [&]() {
                body = ModelParser::parseApp(reader->readMetaBySno(quint64(sno)),
                                             reader->readPayloadBySno(quint64(sno))); }))
            continue;
        if (!body.valid || body.skeleton.isEmpty() || body.primitives.isEmpty())
            continue;
        QVector<int> map(body.skeleton.size(), -1);
        int shared = 0;
        for (int i = 0; i < body.skeleton.size(); ++i) {
            map[i] = byHash.value(body.skeleton[i].nameHash, -1);
            if (map[i] >= 0) ++shared;
        }
        if (shared < 50) continue;                      // not the same rig family
        ++usedPieces;
        for (const MeshPrimitive& p : body.primitives) {
            // Concatenate into the single primitive: indices shift by the running vertex count.
            const quint32 base = quint32(merged.vertices.size());
            for (MeshVertex v : p.vertices) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    const int nj = (v.weights[k] > 0.0f && v.joints[k] < map.size())
                                       ? map[v.joints[k]] : -1;
                    if (nj < 0) { v.weights[k] = 0.0f; v.joints[k] = 0; }
                    else        { v.joints[k] = quint16(nj); sum += v.weights[k]; }
                }
                if (sum > 1e-6f)
                    for (int k = 0; k < 4; ++k) v.weights[k] /= sum;
                merged.vertices.append(v);
            }
            for (quint32 idx : p.indices) merged.indices.append(base + idx);
            merged.doubleSided = merged.doubleSided || p.doubleSided;
        }
    }
    // Never a silent zero: "the setting did nothing" and "the setting is off" must not look alike.
    if (merged.vertices.isEmpty() || merged.indices.isEmpty()) {
        qInfo("export: base body — %s_test999_* resolved %lld piece(s) but produced no geometry "
              "(rig mismatch, or the pieces failed to parse)",
              qUtf8Printable(prefix), qint64(snos.size()));
        return false;
    }
    geo.primitives.append(merged);
    qInfo("export: base body — %s_test999 merged %d piece(s), %lld verts, into one \"__baseBody\" "
          "primitive", qUtf8Printable(prefix), usedPieces, qint64(merged.vertices.size()));
    return true;
}

// ── Base head ("Include base head", Settings ▸ Export ▸ Model export) ────────────────────────────
//
// The head lives in the appearance <class><gender>_P00 (measured: barF_P00 carries barF_P00_HED,
// barF_P00_BOD, base_teethtongue_mat, global_eyeball_mat, Global_Eyelashes_F00_mat,
// Global_Eyeshadow_00_mat and a facial-hair mesh). Only the "_HED" submesh is taken: the rest is
// teeth, tongue, eyeballs, lashes, eyeshadow, stubble and a duplicate body — invisible in game
// behind the face, but very much in the way on a bare reference. That is the same rule the
// Wardrobe's untextured-character export uses (headCore, not hed), kept deliberately identical.
//
// Submesh identity comes from the MATERIAL ROSTER, not from MeshPrimitive::materialName: the
// parser only stores "Material_<index>", so filtering on the primitive's own string would match
// nothing and silently produce an empty head.
static bool appendBaseHead(CascReader* reader, SnoListModel* list, const SnoIndex* index,
                           const QString& pieceName, ModelGeometry& geo)
{
    if (!reader || !reader->isReady() || !list || geo.skeleton.isEmpty())
        return false;
    const int us = pieceName.indexOf(QLatin1Char('_'));
    if (us <= 0)
        return false;
    const QString prefix = pieceName.left(us);
    // druM ships as "druM_newModel_P00"; every other class is "<prefix>_P00".
    const QStringList candidates{prefix + QStringLiteral("_P00"),
                                 prefix + QStringLiteral("_newModel_P00")};
    if (candidates.contains(pieceName, Qt::CaseInsensitive))
        return false;                                   // exporting the head appearance itself
    int sno = -1; QString headName;
    for (const QString& want : candidates) {
        for (int r = 0; r < list->rowCount(); ++r)
            if (const SnoEntry* e = list->entryAt(r))
                if (e->name.compare(want, Qt::CaseInsensitive) == 0) {
                    sno = e->snoId; headName = e->name; break;
                }
        if (sno >= 0) break;
    }
    if (sno < 0) {
        qInfo("export: base head — no %s_P00 appearance in the index; nothing appended",
              qUtf8Printable(prefix));
        return false;
    }
    ModelGeometry head;   // guarded: parsing an arbitrary head appearance
    if (!seh::runGuarded("baseHead", [&]() {
            head = ModelParser::parseApp(reader->readMetaBySno(quint64(sno)),
                                         reader->readPayloadBySno(quint64(sno))); }))
        return false;
    if (!head.valid || head.skeleton.isEmpty() || head.primitives.isEmpty())
        return false;
    const QStringList pal = appearancePalette(Config::d4dataDir(), headName, reader, index, sno);

    QHash<quint32, int> byHash;
    for (int i = 0; i < geo.skeleton.size(); ++i)
        byHash.insert(geo.skeleton[i].nameHash, i);
    QVector<int> map(head.skeleton.size(), -1);
    int shared = 0;
    for (int i = 0; i < head.skeleton.size(); ++i) {
        map[i] = byHash.value(head.skeleton[i].nameHash, -1);
        if (map[i] >= 0) ++shared;
    }
    if (shared < 50) {
        qInfo("export: base head — %s shares only %d bone(s) with %s; not the same rig, skipped",
              qUtf8Printable(headName), shared, qUtf8Printable(pieceName));
        return false;
    }

    MeshPrimitive merged;
    merged.materialName  = QStringLiteral("__baseHead");
    merged.materialIndex = -1;
    int kept = 0;
    for (const MeshPrimitive& p : head.primitives) {
        const QString mat = pal.value(p.materialIndex);
        if (!mat.contains(QLatin1String("_HED"), Qt::CaseInsensitive)) continue;   // head mesh only
        ++kept;
        const quint32 base = quint32(merged.vertices.size());
        for (MeshVertex v : p.vertices) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const int nj = (v.weights[k] > 0.0f && v.joints[k] < map.size())
                                   ? map[v.joints[k]] : -1;
                if (nj < 0) { v.weights[k] = 0.0f; v.joints[k] = 0; }
                else        { v.joints[k] = quint16(nj); sum += v.weights[k]; }
            }
            if (sum > 1e-6f)
                for (int k = 0; k < 4; ++k) v.weights[k] /= sum;
            merged.vertices.append(v);
        }
        for (quint32 idx : p.indices) merged.indices.append(base + idx);
        merged.doubleSided = merged.doubleSided || p.doubleSided;
    }
    if (merged.vertices.isEmpty()) {
        // Distinguish "no roster" from "roster with no _HED": the first is a data/decrypt problem,
        // the second means the naming convention moved and this rule needs revisiting.
        qInfo("export: base head — %s parsed %lld submesh(es) but none matched \"_HED\" (%s)",
              qUtf8Printable(headName), qint64(head.primitives.size()),
              pal.isEmpty() ? "material roster is EMPTY" : "roster present");
        return false;
    }
    geo.primitives.append(merged);
    qInfo("export: base head — %s kept %d \"_HED\" submesh(es), %lld verts, as \"__baseHead\"",
          qUtf8Printable(headName), kept, qint64(merged.vertices.size()));
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

    if (QSettings().value(QStringLiteral("export/includeBaseBody"), false).toBool())
        appendBaseBody(m_reader, m_listModel, m_curName, geo);
    if (QSettings().value(QStringLiteral("export/includeBaseHead"), false).toBool())
        appendBaseHead(m_reader, m_listModel, m_index, m_curName, geo);

    // Same cache as the batch path. One model, but buildExportMats() decodes once per palette
    // SLOT rather than per distinct material, and every empty slot resolves to the same fallback
    // material — so even a single export repeats work. writeLooseTextures() below is inside it too.
    MaterialDecode::TextureCacheScope texCache;

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

    // Same textures\ folder the batch path writes, so a single export and a bulk export of the
    // same piece produce the same layout.
    writeLooseTextures(mats, QFileInfo(path).dir().path(),
                       m_curName.isEmpty() ? QFileInfo(path).completeBaseName() : m_curName,
                       m_reader, Config::d4dataDir());

    // Optionally dump the raw source files (.app + .tex) into a "deps" subfolder beside the .glb —
    // the same folder name and the same shared-folder rule the batch path uses, so a single export
    // and a bulk export of the same piece produce the same layout.
    if (QSettings().value(QStringLiteral("export/withDeps"), false).toBool())
        exportModelDeps(quint64(m_curSno), m_curName, palette, d4, m_reader,
                        QFileInfo(path).dir().filePath(QStringLiteral("deps")));

    int tris = 0;
    for (const MeshPrimitive& p : geo.primitives) tris += p.indices.size() / 3;
    const bool remapNote = !anims.isEmpty()
        && QSettings().value(QStringLiteral("retarget/remapWeights"), false).toBool();
    ExportNotifier::instance().notify(
        QStringLiteral("Exported %1  ·  %2 parts, %3 tris%4%5")
            .arg(QFileInfo(path).fileName()).arg(geo.primitives.size()).arg(tris)
            .arg(remapNote ? QStringLiteral("  (rig reduced to 26 bones)") : QString(),
                 ExportNotifier::glbOptionsLine(opt)),
        QFileInfo(path).absolutePath());

    // ── Opposite-gender twin ──────────────────────────────────────────────────────────────────
    // This is the SINGLE-model path — Ctrl+E, "Export to…", and every context-menu export — so it
    // needs its own hook; the batch expansion in exportModels() never sees these.
    //
    // Routed through exportModels() rather than re-implementing the export: the twin is a
    // different appearance that is not currently loaded, and exportModels already knows how to
    // parse one from its SNO and apply every export setting to it. The bothGenders check there
    // will not re-expand this list, because the twin's own twin is the model just written and is
    // filtered out as already present.
    if (QSettings().value(QStringLiteral("export/bothGenders"), false).toBool() && m_index) {
        const QString twin = genderTwinName(m_curName);
        if (!twin.isEmpty()) {
            int tsno = 0;
            for (const SnoEntry& e : m_index->entries(kApprGroup))
                if (e.name.compare(twin, Qt::CaseInsensitive) == 0) { tsno = e.snoId; break; }
            if (tsno > 0)
                // applyLayout=false: this is the SINGLE-model path. The twin belongs beside the
                // model the user actually exported, not in a subfolder of its own — grouping a pair
                // is the surprise ExportLayout's header says single exports must never spring.
                exportModels({{tsno, twin}}, QFileInfo(path).absolutePath(), nullptr, nullptr,
                             /*applyLayout*/ false);
            else
                qInfo("export: both-genders — no twin '%s' in the index; exported %s alone",
                      qPrintable(twin), qPrintable(m_curName));
        }
    }
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
    // Say so on the way out. A caller that resolved nothing — a Catalogue bundle whose items did
    // not map, a filter that matched none — otherwise gets a completely silent no-op, which reads
    // as "the export is broken" rather than "there was nothing to export". Absence of a log line
    // has twice been the only evidence available while debugging, and both times it cost hours.
    if (items.isEmpty() || dir.isEmpty()) {
        qInfo("bulkExport: nothing to do — %d item(s), dir %s",
              int(items.size()), dir.isEmpty() ? "(none)" : "set");
        return;
    }
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
    // applyLayout=false: Bulk Extract grouped this list itself before calling — it needs the group
    // map for its own buffer and name-matched-texture passes — and `dir` is already the group's
    // folder. Grouping again here would nest a second copy inside it.
    exportModels(todo, dir, sink, &failReasons, /*applyLayout*/ false);

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
    // Unconditionally to the log as well, not only to the sink and the toast: the sink is null for
    // every non-Bulk caller (Catalogue, context menus, multi-select), so those exports left no
    // record of what they produced.
    qInfo("bulkExport: %d exported, %d already present, %d failed → %s",
          int(todo.size() - failed.size()), skipped, int(failed.size()), qUtf8Printable(dir));
    flashScan(QStringLiteral("bulk"), summary);
}

void ModelsTab::exportModels(const QVector<QPair<int, QString>>& models, const QString& dir,
                             const BatchSink* sink, QStringList* failures, bool applyLayout)
{
    if (models.isEmpty() || dir.isEmpty() || !m_reader || !m_reader->isReady()) return;

    // ── One decode cache for the whole batch ──────────────────────────────────────────────────
    // Every path that exports more than one model funnels through here — Bulk Extract, the
    // multi-select export, the context-menu batch, the both-genders twin — so this is the seam
    // where the repeats are.
    //
    // And there are a lot of them. Nothing downstream remembered a decode: buildExportMats() calls
    // buildMat() once per palette SLOT rather than once per distinct material, so a roster naming
    // one material at five indices decoded it five times, and every empty slot resolves to the same
    // fallback material and decoded that again. Across models it was worse — a class's appearances
    // share their leather/fabric/metal detail maps almost completely, and each model re-read and
    // re-BC-decoded every one of them from scratch.
    //
    // The cache is SHARED across threads, not per-thread, because the loop below hands its items to
    // a std::thread pool whenever `sink` is set (every Bulk Extract run). Those workers decode the
    // same maps concurrently, so a per-thread cache would share nothing and save nothing.
    //
    // Constructed here and destroyed when this function returns — after the pool is joined, so no
    // worker can still be using it.
    MaterialDecode::TextureCacheScope texCache;
    QElapsedTimer runT; runT.start();

    // ── Opposite-gender pairing (Settings ▸ Export ▸ Model export) ─────────────────────────────
    // Done by EXPANDING THE LIST rather than by special-casing the export itself, so every option
    // below — textures, animations, raw source files, set-aware expansion, the filename template —
    // applies to the twin identically. This is also why it lives here: Bulk Extract, multi-select
    // export and the context-menu batch paths all funnel through exportModels().
    QVector<QPair<int, QString>> work = models;
    if (QSettings().value(QStringLiteral("export/bothGenders"), false).toBool() && m_index) {
        QSet<int> have;
        for (const auto& m : work) have.insert(m.first);
        int added = 0;
        const QVector<SnoEntry>& all = m_index->entries(kApprGroup);
        QHash<QString, int> byName;
        byName.reserve(all.size());
        for (const SnoEntry& e : all) byName.insert(e.name.toLower(), e.snoId);
        for (const auto& m : models) {
            const QString twin = genderTwinName(m.second);
            if (twin.isEmpty()) continue;
            const int tsno = byName.value(twin.toLower(), 0);
            // Skip when the twin does not exist in the data, or is already in the selection —
            // exporting the same asset twice would just race two writes at one filename.
            if (tsno <= 0 || have.contains(tsno)) continue;
            work.append({tsno, twin});
            have.insert(tsno);
            ++added;
        }
        if (added > 0)
            qInfo("export: both-genders added %d opposite-gender twin(s) to a %lld-model batch",
                  added, qint64(models.size()));
    }
    const QString d4 = Config::d4dataDir();
    // Flags come from SETTINGS, not the Models tab's mirror checkboxes — so the Bulk Extract
    // tab's own option toggles (which write these keys) govern its runs directly.
    const bool wantTex  = QSettings().value(QStringLiteral("export/includeTex"), true).toBool();
    const bool wantAnim = QSettings().value(QStringLiteral("export/includeAnim"), false).toBool();
    // Which SOURCES of animation (util/AnimExportScope.h). "previewed" is meaningless for a batch
    // item that isn't the loaded model, and "pulled" only exists as list state on the loaded model
    // — so for every other item the batch honours `original`, `sets` and `base`, which are all
    // data-derived and therefore answerable for any model.
    //
    // EVERY data-derived source must appear in animAll: it is the sole gate on the branch that
    // calls animClipsFor() below AND on kicking the clip index. Omitting one here does not narrow
    // the result, it silently exports NOTHING for that scope — a sets-only selection, which is the
    // whole reason `sets` exists, would have produced clip-free files with no log line saying so.
    const AnimExportScope animSc = AnimExportScope::load();
    const bool animAll  = animSc.original || animSc.sets || animSc.base;
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
    QVector<QPair<int, QString>> jobs = work;   // `work` = models + any opposite-gender twins
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

    // ── Where each job goes ───────────────────────────────────────────────────────────────────
    // Resolved AFTER the twin and set-aware expansions, so a sibling pulled in by expansion is
    // filed under its own identity rather than the model that requested it.
    //
    // A per-job folder rather than a loop over groups: `dir` is read in only three places inside
    // the item pipeline, and the pipeline runs on a thread pool — turning it into an outer loop
    // would serialise the groups against each other for no reason.
    QHash<int, QString> jobDir;
    if (applyLayout) {
        const QString layout = ExportLayout::mode();
        if (!layout.isEmpty())
            for (const ExportLayout::Group& g : ExportLayout::group(layout, jobs)) {
                const QString f = ExportLayout::folderFor(dir, g);
                for (const auto& it : g.items) jobDir.insert(it.first, f);
            }
    }
    // Every job resolves through this, so the no-layout case is one hash miss, not a branch.
    auto dirFor = [&](int sno) { return jobDir.value(sno, dir); };

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
        const QString outDir = dirFor(m.first);
        if (outDir != dir) QDir().mkpath(outDir);
        QString glbPath = QDir(outDir).filePath(NameTemplate::model(m.second, m.first) + QStringLiteral(".glb"));
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
                // sno + reader so an appearance with no .app.json (encrypted, or newer than the
                // snapshot) still gets its material roster from the binary — otherwise it exports
                // with no materials and lands as an untextured .glb.
                pal = appearancePalette(d4, m.second, m_reader, m_index, m.first);
            }
            if (!geo.valid || geo.primitives.isEmpty()) {
                reportFail(m.second, QStringLiteral("no exportable geometry (parse failed or empty)"));
                return;
            }
            if (QSettings().value(QStringLiteral("export/includeBaseBody"), false).toBool())
                appendBaseBody(m_reader, m_listModel, m.second, geo);
            if (QSettings().value(QStringLiteral("export/includeBaseHead"), false).toBool())
                appendBaseHead(m_reader, m_listModel, m_index, m.second, geo);
            const auto mats = buildExportMats(pal, geo, m.second, d4, m_reader, wantTex);
            // An empty palette means NO materials, which produces a .glb with geometry and nothing
            // on it — visually identical to a successful export until you open it. That is exactly
            // how encrypted armour shipped untextured without a single line in the log. Say it.
            if (wantTex && pal.isEmpty())
                qInfo("export: %s [%lld] — NO material roster (no .app.json and the meta binary "
                      "gave nothing); the .glb will be untextured",
                      qUtf8Printable(m.second), qint64(m.first));
            writeLooseTextures(mats, outDir, m.second, m_reader, d4);
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
                    for (const QString& nm : animClipsFor(m.first, m.second.toLower(),
                                                          animSc.original, animSc.sets, animSc.base)) {
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
                        // Path RELATIVE TO THE RUN ROOT, not a bare filename: with a folder layout
                        // in force the .glb is a level down, and a manifest whose whole purpose is
                        // reproducible re-exports cannot name files that are not there.
                        QJsonObject e{{"file", QDir(dir).relativeFilePath(glbPath)},
                                      {"sno", m.first},
                                      {"name", m.second}};
                        if (kSlots.contains(suf)) e["slot"] = suf;
                        manifest.append(e);
                    }
                }
                if (wantDeps) exportModelDeps(quint64(m.first), m.second, pal, d4, m_reader,
                                              QDir(outDir).filePath(QStringLiteral("deps")));
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
    // independent, so it parallelizes with byte-identical output. Animation and base-body exports
    // stay serial — those paths share caches/GUI state the workers must not race on.
    const bool fitRef = QSettings().value(QStringLiteral("export/includeBaseBody"), false).toBool()
                        || QSettings().value(QStringLiteral("export/includeBaseHead"), false).toBool();
    int par = 1;
    if (sink) {
        const int cfg = QSettings().value(QStringLiteral("bulk/parallel"), -1).toInt();
        par = qBound(1, cfg <= 0 ? QThread::idealThreadCount() : cfg, 16);
    }
    if (par > 1 && (wantAnim || fitRef)) {
        par = 1;
        if (sink && sink->log)
            sink->log(QStringLiteral("   (parallel off for this run — animation / base-body exports are serial-only)"));
    }
    if (par > 1 && jobs.size() > 1) {
        std::atomic<int> next{0}, done{0};
        auto workerFn = [&]() {
            // Join the shared decode cache. Participation is per-thread by design (see
            // TextureCacheScope) — without this the workers, which are where the decoding actually
            // happens, would be the only threads NOT using it.
            MaterialDecode::TextureCacheScope workerCache;
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
        QJsonObject root{{"generator", QStringLiteral("D4AssetBrowser")},
                         {"exported", QDateTime::currentDateTime().toString(Qt::ISODate)},
                         {"models", manifest}};
        QFile mf(QDir(dir).filePath(QStringLiteral("manifest.json")));
        if (mf.open(QIODevice::WriteOnly))
            mf.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    }
    // Env-gated, permanent, in the house style: this is the number that says whether the cache is
    // earning its memory, and whether decoding is even what a slow run is spending its time on.
    // Set D4_DUMP_EXPORTPERF=1 and export a class's appearances; a run with lots of shared detail
    // maps should show hits far outnumbering misses.
    if (!qEnvironmentVariableIsEmpty("D4_DUMP_EXPORTPERF")) {
        const qint64 h = texCache.hits();
        const qint64 m = texCache.misses();
        // jobs, not work: `work` is the requested models plus the opposite-gender twins, while
        // `jobs` adds the set-aware sibling expansion — and jobs is what the loop actually ran, so
        // it is what the per-model figure has to divide by.
        // The state of the cache is part of the measurement, so the line says it. Without this a
        // baseline run and a cached run are two numbers with no way to tell which was which.
        qInfo("export perf: %d model(s) in %lld ms — texture decodes %lld, served from cache %lld "
              "(%lld%% reuse, %lld MB not re-decoded) [cache %s]",
              int(jobs.size()), runT.elapsed(), m, h,
              (h + m) ? (100 * h / (h + m)) : 0, texCache.bytesSaved() >> 20,
              texCache.disabled() ? "OFF (D4_NO_TEXCACHE)" : "on");
    }
    QSettings().setValue(QStringLiteral("models/lastExportDir"), dir);
    ExportNotifier::instance().notify(
        QStringLiteral("Exported %1 model(s)%2%3%4")
            .arg(ok).arg(fail ? QStringLiteral(", %1 failed").arg(fail) : QString(),
                 setAware && !manifest.isEmpty() ? QStringLiteral("  (+ manifest.json)") : QString(),
                 // Re-resolved rather than reusing the per-model `opt` inside the loop above: it is
                 // out of scope here, and optionsFromSettings() is deterministic, so this is the
                 // same value every model in the run was written with.
                 ExportNotifier::glbOptionsLine(ModelExporter::optionsFromSettings())),
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
        : appearancePalette(d4, name, m_reader, m_index, sno);

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
    // applyLayout=false: this stages into a temp folder and then rebuilds each path by hand to
    // hand the drag its file:// URLs. A layout would move the files under it and every exists()
    // below would miss, so the drag would silently carry nothing.
    exportModels(items, dir, nullptr, nullptr, /*applyLayout*/ false);
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
    // The REAL count, from the same function the exporter decodes. It used to report the list
    // SELECTION size, which is 0 for the normal case of "export what the scope says" — so the menu
    // said nothing about how many clips were about to be written, and a scope change that halved
    // the export looked identical from here.
    const int n = plannedExportAnimNames().size();
    if (n == 0) return QStringLiteral("Export animations only (.glb) — no clips in scope");
    if (n == 1) return QStringLiteral("Export animation only (.glb) — 1 clip…");
    return QStringLiteral("Export animations only (.glb) — %1 clips…").arg(n);
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
