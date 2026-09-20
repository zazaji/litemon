#include "appconfig.h"

#include <QDir>
#include <QProcessEnvironment>
#include <QSettings>
#include <algorithm>

QString AppConfig::configPath() {
    const auto env = QProcessEnvironment::systemEnvironment();
    QString base = env.value("XDG_CONFIG_HOME");
    if (base.isEmpty()) base = QDir::homePath() + "/.config";
    base += "/litemon";
    QDir().mkpath(base);
    return base + "/litemon.ini";
}

AppConfig AppConfig::load() {
    QSettings s(configPath(), QSettings::IniFormat);
    AppConfig c;
    c.sampleIntervalSec = std::clamp(s.value("sampling/system_seconds", c.sampleIntervalSec).toInt(), 60, 300);
    c.gpuIntervalSec = std::clamp(s.value("sampling/gpu_seconds", c.gpuIntervalSec).toInt(), 60, 600);
    c.gpuIntervalSec = std::max(c.gpuIntervalSec, c.sampleIntervalSec);
    c.detailRetentionDays = std::clamp(s.value("retention/detail_days", s.value("retention/raw_days", c.detailRetentionDays)).toInt(), 6, 30);
    c.archiveRetentionDays = std::clamp(s.value("retention/archive_days", s.value("retention/ten_minute_days", c.archiveRetentionDays)).toInt(), 30, 3650);
    c.maintenanceIntervalSec = std::clamp(s.value("maintenance/interval_seconds", c.maintenanceIntervalSec).toInt(), 30, 3600);
    c.historyTargetPoints = std::clamp(s.value("ui/history_target_points", c.historyTargetPoints).toInt(), 200, 3000);
    c.collectGpu = s.value("sampling/collect_gpu", c.collectGpu).toBool();
    return c;
}

void AppConfig::save() const {
    QSettings s(configPath(), QSettings::IniFormat);
    s.setValue("sampling/system_seconds", sampleIntervalSec);
    s.setValue("sampling/gpu_seconds", gpuIntervalSec);
    s.setValue("sampling/collect_gpu", collectGpu);
    s.setValue("retention/detail_days", detailRetentionDays);
    s.setValue("retention/archive_days", archiveRetentionDays);
    s.setValue("maintenance/interval_seconds", maintenanceIntervalSec);
    s.setValue("ui/history_target_points", historyTargetPoints);
    s.sync();
}
