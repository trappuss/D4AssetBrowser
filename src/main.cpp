// Diablo4AssetBrowserNative — entry point.
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

#include "app/AppPaths.h"
#include "app/LogConsole.h"
#include "app/MainWindow.h"
#include "app/SehGuard.h"

namespace {
QFile g_logFile;

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
    static QMutex s_logMutex;
    QMutexLocker s_lock(&s_logMutex);

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
    QApplication::setOrganizationName("Diablo4AssetBrowser");
    QApplication::setApplicationName("Diablo4AssetBrowserNative");
    QApplication::setApplicationVersion("2.1.0");

    // Portable: every QSettings() default-ctor writes to an INI in the beside-exe data/ folder
    // (no Windows registry). Must run before any QSettings use. Combined with AppPaths, the whole
    // tool is self-contained — no registry keys, no %AppData%.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, AppPaths::dataDir());

    // Runtime log next to the exe (truncated each launch) for diagnostics.
    g_logFile.setFileName(QDir(QCoreApplication::applicationDirPath())
                              .filePath(QStringLiteral("D4AssetBrowser.log")));
    g_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    qInstallMessageHandler(logHandler);
    qInfo("Diablo4AssetBrowserNative v%s starting",
          QApplication::applicationVersion().toLatin1().constData());

    // Show real checkmarks (not the platform's filled blue box) on every toggle.
    installCheckmarkStyle(app);

    // Window/taskbar icon — painter-drawn (self-contained; no file to lose in a portable copy):
    // the same gold faceted gem on charcoal as the exe's embedded res/app.ico.
    {
        QPixmap pm(256, 256);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x2a, 0x2a, 0x2c));
        p.drawRoundedRect(QRectF(6, 6, 244, 244), 44, 44);
        const QPointF c(128, 132);
        const double gw = 78, gh = 104;   // gem half-extents
        auto facet = [&](const QPointF& a, const QPointF& b, const QColor& col) {
            p.setBrush(col);
            p.drawPolygon(QPolygonF() << c << a << b);
        };
        const QPointF top(c.x(), c.y() - gh), bot(c.x(), c.y() + gh);
        const QPointF lft(c.x() - gw, c.y()), rgt(c.x() + gw, c.y());
        facet(top, rgt, QColor(0xf0, 0xc0, 0x5e));   // lit upper-right
        facet(top, lft, QColor(0xe2, 0xae, 0x46));
        facet(bot, lft, QColor(0xa8, 0x79, 0x26));
        facet(bot, rgt, QColor(0x8f, 0x66, 0x1e));   // shadowed lower-right
        p.setBrush(QColor(0x8a, 0x14, 0x14));        // Diablo-red core
        p.drawPolygon(QPolygonF() << QPointF(c.x(), c.y() - gh * 0.38)
                                  << QPointF(c.x() + gw * 0.38, c.y())
                                  << QPointF(c.x(), c.y() + gh * 0.38)
                                  << QPointF(c.x() - gw * 0.38, c.y()));
        p.setPen(QPen(QColor(0x5a, 0x43, 0x18), 5));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(QPolygonF() << top << rgt << bot << lft);
        p.end();
        app.setWindowIcon(QIcon(pm));
    }

    MainWindow window;
    window.show();
    return QApplication::exec();
}
