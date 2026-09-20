#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
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
