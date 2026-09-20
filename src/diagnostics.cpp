#include "diagnostics.h"
#include "appconfig.h"
#include "linuxutils.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QOperatingSystemVersion>
#include <QProcessEnvironment>
#include <QStringList>
#include <QSysInfo>

#ifndef LITEMON_VERSION
#define LITEMON_VERSION "dev"
#endif

using namespace LinuxUtils;

QJsonObject Diagnostics::collect(const QString &databasePath) {
    const AppConfig c = AppConfig::load();
    QJsonObject tools;
    static const QStringList kTools = {"nvidia-smi", "intel_gpu_top", "xpu-smi", "smartctl"};
    for (const QString &tool : kTools) {
        const QString path = commandPath(tool);
        tools.insert(tool, path.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(path));
    }
    const QFileInfo db(databasePath);
    QJsonObject o {
        {"generated_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"litemon_version", LITEMON_VERSION},
        {"kernel", QSysInfo::kernelVersion()},
        {"architecture", QSysInfo::currentCpuArchitecture()},
        {"product", QSysInfo::prettyProductName()},
        {"qt_runtime", qVersion()},
        {"database_path", databasePath},
        {"database_exists", db.exists()},
        {"database_bytes", static_cast<double>(db.exists() ? db.size() : 0)},
        {"config_path", AppConfig::configPath()},
        {"sample_interval_seconds", c.sampleIntervalSec},
        {"gpu_interval_seconds", c.gpuIntervalSec},
        {"detail_retention_days", c.detailRetentionDays},
        {"archive_retention_days", c.archiveRetentionDays},
        {"tools", tools}
    };
    return o;
}

bool Diagnostics::writeReport(const QString &databasePath, const QString &outputPath, QString *error) {
    QFile f(outputPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(collect(databasePath)).toJson(QJsonDocument::Indented));
    return true;
}
