#include "mainwindow.h"
#include "appconfig.h"
#include "apppaths.h"
#ifdef Q_OS_MACOS
#include "macosutils.h"
#endif

#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPainter>
#include <QPixmap>
#include <QStandardPaths>

#ifndef LITEMON_VERSION
#define LITEMON_VERSION "dev"
#endif

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("litemon");
    app.setApplicationDisplayName("LiteMon");
    QCoreApplication::setApplicationVersion(LITEMON_VERSION);
    // Installed hicolor SVG → generic monitor theme icon → drawn fallback, so
    // the window and tray icon both carry something even before install.
    QIcon appIcon = QIcon::fromTheme(QStringLiteral("io.github.litemon.LiteMon"));
    if (appIcon.isNull()) appIcon = QIcon::fromTheme(QStringLiteral("utilities-system-monitor"));
    if (appIcon.isNull()) {
        QPixmap pm(64, 64);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x3d, 0x6d, 0xb8));
        p.drawRoundedRect(2, 2, 60, 60, 12, 12);
        QFont f = p.font();
        f.setBold(true);
        f.setPixelSize(30);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(pm.rect(), Qt::AlignCenter, QStringLiteral("LM"));
        p.end();
        appIcon = QIcon(pm);
    }
    app.setWindowIcon(appIcon);
#ifdef Q_OS_MACOS
    // Tray-only on macOS: the menu bar already carries the LiteMon tray icon,
    // so a Dock icon would just duplicate it.
    MacUtils::hideDockIcon();
#endif
    MainWindow::applyTheme(AppConfig::load().theme);

    // Login can legitimately start two GUIs (session restore plus the
    // autostart unit), and a duplicate only fights over the database and the
    // tray, so bind a per-user socket and let the first instance own it.
    // RuntimeLocation is per-user on every platform we ship; if it is missing
    // (no XDG_RUNTIME_DIR) the guard simply stays off rather than blocking.
#ifdef Q_OS_MACOS
    const QString guardBase = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
#else
    const QString guardBase = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
#endif
    const QString guardPath = guardBase + QStringLiteral("/litemon-gui.socket");
    QLocalServer *guard = nullptr;
    if (!guardBase.isEmpty()) {
        QLocalSocket probe;
        probe.connectToServer(guardPath);
        if (probe.waitForConnected(300)) {
            probe.abort(); // the running instance's guard raises its window
            return 0;
        }
        QFile::remove(guardPath); // socket left behind by a crashed instance
        guard = new QLocalServer(&app);
        if (!guard->listen(guardPath)) {
            guard->deleteLater();
            guard = nullptr;
        }
    }

    QCommandLineParser p;
    p.setApplicationDescription("LiteMon native Qt Linux monitor");
    p.addHelpOption();
    p.addVersionOption();
    // Same default as the collector (AppPaths), so GUI and collector always
    // agree on the database; on macOS QStandardPaths would land in
    // ~/Library/Application Support and split from the collector's XDG path.
    p.addOption({"db", "SQLite database path.", "path", AppPaths::databasePath()});
    p.process(app);

    MainWindow window(p.value("db"));
    if (guard) {
        QObject::connect(guard, &QLocalServer::newConnection, &window, [guard, &window] {
            if (QLocalSocket *c = guard->nextPendingConnection()) c->abort();
            // A second launch means the user is asking for the monitor.
            window.setWindowState((window.windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
            window.show();
            window.raise();
            window.activateWindow();
        });
    }
    window.show();
    return app.exec();
}
