// D4AssetBrowser — entry point.
//
// A native C++/Qt6 rewrite of the asset browser in d4analyzer's exact stack:
//   C++17 · Qt 6 Widgets · OpenGL 4.5 · CascLib · fastgltf + tinygltf · Draco.
#include <QApplication>
#include <QDateTime>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QMutex>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QSettings>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTextStream>

#include "app/AppLog.h"
#include "app/AppPaths.h"
#include "util/QueryTerm.h"   // startup self-test for the shared name-query matcher
#include "app/LogConsole.h"
#include "app/MainWindow.h"
#include "app/SehGuard.h"

namespace {
QFile g_logFile;
// Guards g_logFile. The message handler runs on EVERY thread, so opening or closing the file from
// the Settings dialog while a worker is mid-write would tear the QFile out from under it. Both the
// handler and AppLog::setFileLogging take this.
QMutex g_logMutex;

// Replace the platform's filled "blue box" checked indicator with a real
// checkmark everywhere (checkboxes, menu toggles, tree/table checks). We draw
// a tick to a PNG once and reference it from a global stylesheet — the only
// way QSS can show an actual glyph inside an indicator subcontrol.
void installCheckmarkStyle(QApplication& app)
{
    const QString png = AppPaths::file(QStringLiteral("checkmark.png"));
    {
        QPixmap pm(28, 28);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(QColor(0xff, 0x6b, 0x5b));   // Diablo-red tick
        pen.setWidthF(3.2);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.drawPolyline(QPolygonF{QPointF(6, 15), QPointF(12, 21), QPointF(22, 7)});
        p.end();
        pm.save(png, "PNG");
    }
    QString url = png;
    url.replace(QLatin1Char('\\'), QLatin1Char('/'));
    app.setStyleSheet(QStringLiteral(
        // ── Check indicators: bordered box + drawn tick (no filled blue box) ──
        "QCheckBox::indicator, QMenu::indicator, QGroupBox::indicator,"
        "QTreeView::indicator, QTreeWidget::indicator,"
        "QTableView::indicator, QListView::indicator, QListWidget::indicator {"
        " width:15px; height:15px; border:1px solid #6a6a6a;"
        " border-radius:3px; background:#2c2c2c; }"
        "QCheckBox::indicator:hover, QMenu::indicator:hover,"
        "QTreeView::indicator:hover, QTableView::indicator:hover {"
        " border-color:#b0453c; }"
        "QCheckBox::indicator:checked, QMenu::indicator:checked,"
        "QGroupBox::indicator:checked, QTreeView::indicator:checked,"
        "QTreeWidget::indicator:checked, QTableView::indicator:checked,"
        "QListView::indicator:checked, QListWidget::indicator:checked {"
        " border-color:#a01818; image:url(\"%1\"); }"
        "QCheckBox::indicator:indeterminate, QTreeView::indicator:indeterminate,"
        "QTreeWidget::indicator:indeterminate {"
        " border-color:#a01818; background:#5a1414; }"
        // ── Selection: flat, full-row Diablo red (no rounded, separated cells) ──
        "QTableView, QTreeView, QListView, QTreeWidget, QListWidget {"
        " selection-background-color:#8a1414; selection-color:#ffffff;"
        " outline:0; }"
        "QTableView::item, QTreeView::item, QListView::item,"
        "QTreeWidget::item, QListWidget::item {"
        " border:0; border-radius:0; }"
        "QTableView::item:selected, QTreeView::item:selected, QListView::item:selected,"
        "QTreeWidget::item:selected, QListWidget::item:selected {"
        " background:#8a1414; color:#ffffff; border:0; border-radius:0; }"
        "QTableView::item:hover, QTreeView::item:hover, QListView::item:hover,"
        "QTreeWidget::item:hover, QListWidget::item:hover {"
        " background:#3a2020; }").arg(url));
}

void logHandler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    // Qt routes messages from ANY thread to this single global handler. Without a
    // lock, concurrent logging from a worker thread + the GUI thread tears the
    // shared g_logFile / QTextStream writes apart (garbled output) and crashes on
    // the non-reentrant QFile. Serialize the whole handler.
    //
    // The mutex is now a file-scope one shared with AppLog::setFileLogging, not a local static:
    // toggling the setting closes this QFile, and doing that while a worker thread is inside the
    // write below is the same tear this lock exists to prevent.
    QMutexLocker s_lock(&g_logMutex);

    const char* lvl = "INFO";
    switch (type) {
        case QtDebugMsg:    lvl = "DBG ";  break;
        case QtWarningMsg:  lvl = "WARN";  break;
        case QtCriticalMsg: lvl = "CRIT";  break;
        case QtFatalMsg:    lvl = "FATAL"; break;
        default:            lvl = "INFO";  break;
    }
    const QString line = QStringLiteral("%1 %2  %3")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
        .arg(QLatin1String(lvl), msg);
    if (g_logFile.isOpen()) {
        QTextStream(&g_logFile) << line << '\n';
        g_logFile.flush();
    }
    fprintf(stderr, "%s\n", line.toLocal8Bit().constData());
    LogBuffer::instance().append(line);   // mirror to the in-app console window
}
}   // namespace

// Defined at file scope, NOT inside the anonymous namespace above: a qualified name from another
// namespace may not be DEFINED inside an unnamed one (C2888), which this codebase has hit before.
QString AppLog::filePath()
{
    return AppPaths::file(QStringLiteral("D4AssetBrowser.log"));
}

bool AppLog::fileLogging()
{
    QMutexLocker lock(&g_logMutex);
    return g_logFile.isOpen();
}

void AppLog::setFileLogging(bool on)
{
    QSettings().setValue(QStringLiteral("log/autoFile"), on);
    QMutexLocker lock(&g_logMutex);
    if (on) {
        if (g_logFile.isOpen()) return;
        g_logFile.setFileName(AppPaths::file(QStringLiteral("D4AssetBrowser.log")));
        // Truncate: ONE file that is always the current session, never an ever-growing history.
        // That is the whole point of it — a fixed path someone can be pointed at.
        g_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    } else if (g_logFile.isOpen()) {
        g_logFile.close();
    }
}

int main(int argc, char** argv)
{
    // Install the structured-exception translator on the GUI thread so a
    // hardware fault (access violation from a bad model or a GPU-driver crash)
    // in a guarded section becomes a catchable C++ exception rather than an
    // immediate process kill. Background load threads install it themselves.
    seh::installSehTranslator();

    // Request an OpenGL 4.5 core context (matches d4analyzer's
    // QOpenGLFunctions_4_5_Core) so compressed BC/BPTC uploads are available.
    QSurfaceFormat fmt;
    fmt.setVersion(4, 5);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSamples(4);   // 4x MSAA: smooths mesh silhouettes and anti-aliases fur strand
                         // edges (the shell pass uses alpha-to-coverage, which needs MSAA).
    fmt.setStencilBufferSize(8);   // selection silhouette masks the part into stencil, then draws
                                   // the outline only where stencil is clear. Without this Qt
                                   // allocates depth-only and GLModelWidget falls back to wireframe.
    QSurfaceFormat::setDefaultFormat(fmt);

    // Two QOpenGLWidgets live at once (texture preview + model viewport). Sharing
    // the GL context across them is the Qt-recommended setup and avoids the case
    // where only the first-created widget renders. Must be set before QApplication.
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    LogBuffer::instance();   // construct on the GUI thread (queued log delivery)
    QApplication::setOrganizationName("D4AssetBrowser");
    QApplication::setApplicationName("D4AssetBrowser");
    QApplication::setApplicationVersion("2.2.8");

    // Portable: every QSettings() default-ctor writes to an INI in the beside-exe data/ folder
    // (no Windows registry). Must run before any QSettings use. Combined with AppPaths, the whole
    // tool is self-contained — no registry keys, no %AppData%.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, AppPaths::dataDir());

    // ── Carry settings across the rename ────────────────────────────────────────────────────────
    // QSettings keys off organisation + application name, so renaming to D4AssetBrowser orphaned
    // every existing preference: game folder, export dirs, panel layout, ensembles. The old file is
    // still sitting in data/ under the previous names — copy it once rather than making everyone
    // re-configure. Only when the new file does not exist yet, so it can never clobber newer
    // settings, and the old one is left in place as a manual fallback.
    {
        const QString oldIni = AppPaths::dataDir()
            + QStringLiteral("/Diablo4AssetBrowser/Diablo4AssetBrowserNative.ini");
        const QString newDir = AppPaths::dataDir() + QStringLiteral("/D4AssetBrowser");
        const QString newIni = newDir + QStringLiteral("/D4AssetBrowser.ini");
        if (QFile::exists(oldIni) && !QFile::exists(newIni)) {
            QDir().mkpath(newDir);
            if (QFile::copy(oldIni, newIni))
                qInfo("settings: migrated from the pre-rename file (%s)", qPrintable(oldIni));
        }
    }

    // ── Renamed setting keys ────────────────────────────────────────────────────────────────────
    // Renaming a key silently RESETS it: the new name has never been written, so its default wins
    // and the user's choice vanishes with no error and nothing in the log. That is exactly what
    // happened when the Wardrobe-only export options moved to Model export and lost their
    // "wardrobe" prefix — "it used to work" was correct, and the rename was the regression.
    // Carry each old value across once, then leave the old key alone (harmless, and it keeps a
    // downgrade working).
    {
        QSettings s;
        static const struct { const char* from; const char* to; } kRenamed[] = {
            {"export/wardrobeBothGenders", "export/bothGenders"},
            {"export/wardrobeFxSim",       "export/exportFxSim"},
        };
        for (const auto& r : kRenamed) {
            const QString from = QLatin1String(r.from), to = QLatin1String(r.to);
            if (s.contains(from) && !s.contains(to)) {
                s.setValue(to, s.value(from));
                qInfo("settings: carried %s -> %s", r.from, r.to);
            }
        }
    }

    // The shared name-query matcher, used by three filters that must agree. Microseconds; a
    // failure means Bulk Extract and the Models list would disagree, so say so loudly rather
    // than shipping a silent divergence.
    if (const QString qtErr = QueryTerm::selfTest(); !qtErr.isEmpty())
        qWarning().noquote() << "SELF-TEST FAILED —" << qtErr;

    // Superseded cache versions accumulate otherwise — back_trophy_v1..v4 were all still present,
    // and appearance_meta/icon_index are 2-3 MB each per version. Keep the numbers in step with the
    // kCacheVersion constants they mirror; a stale number here only under-prunes, never deletes a
    // live cache, because pruneOldCaches removes strictly LOWER versions.
    AppPaths::pruneOldCaches(QStringLiteral("back_trophy_v"),    4,  QStringLiteral(".json"));
    AppPaths::pruneOldCaches(QStringLiteral("appearance_meta_v"), 23, QStringLiteral(".json"));
    AppPaths::pruneOldCaches(QStringLiteral("icon_index_v"),      4,  QStringLiteral(".json"));
    AppPaths::pruneOldCaches(QStringLiteral("stable_index_v"),    6,  QStringLiteral(".bin"));
    AppPaths::pruneOldCaches(QStringLiteral("asset_links_v"),     1,  QStringLiteral(".bin"));
    AppPaths::pruneOldCaches(QStringLiteral("coretoc_v"),         3,  QStringLiteral(".bin"));
    AppPaths::pruneOldCaches(QStringLiteral("tex_info_v"),        2,  QStringLiteral(".bin"));
    AppPaths::pruneOldCaches(QStringLiteral("store_products_v"), 4,  QStringLiteral(".json"));
    AppPaths::pruneOldCaches(QStringLiteral("item_hover_v"),      2,  QStringLiteral(".json"));
    // The remaining five. All are at v1 today so these are no-ops — which is the point: the first
    // bump of any of them would otherwise orphan the old file forever, and tvfs_paths alone is
    // ~50 MB. Cheaper to add the line now than to remember at bump time.
    AppPaths::pruneOldCaches(QStringLiteral("wardrobe_anims_v"),  1,  QStringLiteral(".json"));
    AppPaths::pruneOldCaches(QStringLiteral("latest_v"),          2,  QStringLiteral(".bin"));
    AppPaths::pruneOldCaches(QStringLiteral("build_history_v"),   1,  QStringLiteral(".bin"));
    AppPaths::pruneOldCaches(QStringLiteral("casc_index_v"),      1,  QStringLiteral(".bin"));
    AppPaths::pruneOldCaches(QStringLiteral("tvfs_paths_v"),      1,  QStringLiteral(".bin"));

    // Runtime log inside data/ (truncated each launch) for diagnostics.
    //
    // This used to write beside the EXE, which contradicted the three places that tell people where
    // to find it — README's portable-layout table, README's bug-report instruction and
    // RELEASE_README.txt all say data\D4AssetBrowser.log — and contradicted AppPaths.h's own promise
    // that everything the tool writes lives in data/. A release smoke test from a fresh unzip caught
    // it: data/ appeared correctly but held no log. Anyone following the bug-report instructions
    // would have found nothing at the path they were given.
    //
    // dataDir() is safe here: QApplication is constructed, and QSettings::setPath above has already
    // forced its one-time mkpath.
    // Gated by Settings -> General -> "Write a log file automatically", default ON. Default ON is
    // deliberate: the file costs nothing until something goes wrong, and the one time it matters is
    // the run nobody thought to enable it for.
    qInstallMessageHandler(logHandler);
    AppLog::setFileLogging(QSettings().value(QStringLiteral("log/autoFile"), true).toBool());
    qInfo("D4AssetBrowser v%s starting",
          QApplication::applicationVersion().toLatin1().constData());

    // Show real checkmarks (not the platform's filled blue box) on every toggle.
    installCheckmarkStyle(app);

    // Window/taskbar icon — the same artwork as the exe's embedded res/app.ico, loaded from the
    // Qt resource that res/app.qrc compiles into the binary. Still self-contained: a portable copy
    // has no file to lose. It replaces a painter-drawn gem that existed only because there was no
    // embedded image to use.
    //
    // NOTE these are two different mechanisms and BOTH are needed: this one gives the WINDOW and
    // taskbar their icon at runtime; res/app.rc gives the .exe FILE its icon in Explorer and on the
    // desktop. Neither substitutes for the other.
    {
        const QIcon appIcon(QStringLiteral(":/app_256.png"));
        if (!appIcon.isNull()) app.setWindowIcon(appIcon);
        else qWarning("app icon: :/app_256.png missing from the Qt resource — using the default");
    }

    MainWindow window;
    window.show();
    return QApplication::exec();
}
