#include "mainwindow.h"
#include "appconfig.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QStandardPaths>

#ifndef LITEMON_VERSION
#define LITEMON_VERSION "dev"
#endif

static QString defaultDbPath() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(base);
    return base + "/metrics.sqlite";
}

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
    MainWindow::applyTheme(AppConfig::load().theme);

    QCommandLineParser p;
    p.setApplicationDescription("LiteMon native Qt Linux monitor");
    p.addHelpOption();
    p.addVersionOption();
    p.addOption({"db", "SQLite database path.", "path", defaultDbPath()});
    p.process(app);

    MainWindow window(p.value("db"));
    window.show();
    return app.exec();
}
