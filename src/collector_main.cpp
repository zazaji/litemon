#include "apppaths.h"
#include "appconfig.h"
#include "collector.h"
#include "database.h"
#include "diagnostics.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdio>

#ifndef LITEMON_VERSION
#define LITEMON_VERSION "dev"
#endif

static QJsonValue jsonNumber(double v) {
    return std::isfinite(v) ? QJsonValue(v) : QJsonValue(QJsonValue::Null);
}

static QJsonObject toJson(const SystemMetric &m) {
    QJsonObject o {
        {"timestamp", m.timestamp}, {"cpu_usage", jsonNumber(m.cpuUsage)},
        {"cpu_temp", jsonNumber(m.cpuTemperatureC)}, {"load1", jsonNumber(m.load1)},
        {"memory_used_mib", jsonNumber(m.memoryUsedMiB)}, {"memory_total_mib", jsonNumber(m.memoryTotalMiB)},
        {"swap_used_mib", jsonNumber(m.swapUsedMiB)}, {"swap_total_mib", jsonNumber(m.swapTotalMiB)},
        {"network_rx_mibs", jsonNumber(m.networkRxMiBs)}, {"network_tx_mibs", jsonNumber(m.networkTxMiBs)},
        {"disk_read_mibs", jsonNumber(m.diskReadMiBs)}, {"disk_write_mibs", jsonNumber(m.diskWriteMiBs)},
        {"disk_used_gib", jsonNumber(m.diskUsedGiB)}, {"disk_total_gib", jsonNumber(m.diskTotalGiB)},
        {"battery_percent", jsonNumber(m.batteryPercent)}, {"battery_power_w", jsonNumber(m.batteryPowerW)},
        {"battery_health", jsonNumber(m.batteryHealthPercent)}, {"battery_status", m.batteryStatus}
    };
    QJsonArray gpus;
    for (const auto &g : m.gpus) {
        gpus.append(QJsonObject {
            {"id", g.id}, {"vendor", g.vendor}, {"name", g.name}, {"driver", g.driver}, {"state", g.state},
            {"utilization", jsonNumber(g.utilization)}, {"memory_used_mib", jsonNumber(g.memoryUsedMiB)},
            {"memory_total_mib", jsonNumber(g.memoryTotalMiB)}, {"temperature_c", jsonNumber(g.temperatureC)},
            {"power_w", jsonNumber(g.powerW)}, {"frequency_mhz", jsonNumber(g.frequencyMHz)}
        });
    }
    o["gpus"] = gpus;
    QJsonArray cores;
    for (double v : m.cpuCores) { cores.append(jsonNumber(v)); }
    o["cpu_cores"] = cores;
    return o;
}

static void printJson(const QJsonObject &o) {
    const QByteArray json = QJsonDocument(o).toJson(QJsonDocument::Indented);
    std::fwrite(json.constData(), 1, static_cast<size_t>(json.size()), stdout);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("LiteMon");
    QCoreApplication::setApplicationName("litemon");
    QCoreApplication::setApplicationVersion(LITEMON_VERSION);

    const AppConfig fileConfig = AppConfig::load();

    QCommandLineParser p;
    p.setApplicationDescription("LiteMon lightweight local metrics collector");
    p.addHelpOption();
    p.addVersionOption();
    p.addOption({"snapshot", "Print one JSON hardware snapshot and exit."});
    p.addOption({"diagnostics", "Print a JSON diagnostics report and exit."});
    p.addOption({"health-check", "Check database integrity and required runtime paths."});
    p.addOption({"db", "SQLite database path.", "path", AppPaths::databasePath()});
    p.addOption({"interval", "System sampling interval in seconds.", "seconds", QString::number(fileConfig.sampleIntervalSec)});
    p.addOption({"gpu-interval", "GPU sampling interval in seconds.", "seconds", QString::number(fileConfig.gpuIntervalSec)});
    p.addOption({"no-gpu", "Disable GPU collection for this run."});
    p.process(app);

    const QString dbPath = p.value("db");
    if (p.isSet("diagnostics")) {
        printJson(Diagnostics::collect(dbPath));
        return 0;
    }

    MetricsDatabase db(dbPath, "litemon-collector");
    QString error;
    if (p.isSet("health-check")) {
        if (!db.open(&error)) {
            qCritical("database open failed: %s", qPrintable(error));
            return 3;
        }
        if (!db.integrityCheck(&error)) {
            qCritical("database integrity check failed: %s", qPrintable(error));
            return 4;
        }
        qInfo("LiteMon health check OK (schema=%d, db=%s)", db.schemaVersion(), qPrintable(dbPath));
        return 0;
    }

    SystemCollector collector;
    if (p.isSet("snapshot")) {
        collector.collect(false);
        QThread::msleep(250);
        printJson(toJson(collector.collect(!p.isSet("no-gpu"))));
        return 0;
    }

    QLockFile lock(AppPaths::collectorLockPath());
    lock.setStaleLockTime(0);
    if (!lock.tryLock(100)) {
        qCritical("LiteMon collector is already running");
        return 2;
    }

    if (!db.open(&error)) {
        qCritical("Database: %s", qPrintable(error));
        return 3;
    }

    const int interval = std::clamp(p.value("interval").toInt(), 60, 300);
    const int gpuInterval = std::max(interval, std::clamp(p.value("gpu-interval").toInt(), 60, 600));
    const bool collectGpu = fileConfig.collectGpu && !p.isSet("no-gpu");
    const RetentionPolicy retention { fileConfig.detailRetentionDays, fileConfig.archiveRetentionDays };
    qint64 lastGpu = 0;
    qint64 lastMaintain = 0;
    collector.collect(false);

    auto tick = [&] {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const bool withGpu = collectGpu && (now - lastGpu) >= gpuInterval;
        auto metric = collector.collect(withGpu);
        if (withGpu) lastGpu = now;

        QString e;
        if (!db.insert(metric, &e)) qWarning("Insert: %s", qPrintable(e));
        if ((now - lastMaintain) >= fileConfig.maintenanceIntervalSec) {
            if (!db.maintain(now, retention, &e)) qWarning("Maintenance: %s", qPrintable(e));
            else if (!db.checkpoint(&e)) qWarning("Checkpoint: %s", qPrintable(e));
            lastMaintain = now;
        }
    };

    tick();
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, tick);
    timer.start(interval * 1000);
    return app.exec();
}
