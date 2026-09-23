#include "appconfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTextStream>
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
    c.traySensor1 = s.value("tray/sensor1").toString();
    c.traySensor2 = s.value("tray/sensor2").toString();
    const QString t = s.value("ui/theme").toString();
    c.theme = (t == "light" || t == "dark") ? t : "system";
    c.autostart = s.value("ui/autostart", c.autostart).toBool();
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
    s.setValue("tray/sensor1", traySensor1);
    s.setValue("tray/sensor2", traySensor2);
    s.setValue("ui/theme", theme);
    s.setValue("ui/autostart", autostart);
    s.sync();
}

QString AppConfig::autostartFilePath() {
    const auto env = QProcessEnvironment::systemEnvironment();
    QString base = env.value("XDG_CONFIG_HOME");
    if (base.isEmpty()) base = QDir::homePath() + "/.config";
    return base + "/autostart/io.github.litemon.LiteMon.desktop";
}

bool AppConfig::setAutostartEnabled(bool enable, QString *error) {
    const QString path = autostartFilePath();
    if (!enable) {
        if (QFile::exists(path) && !QFile::remove(path)) {
            if (error) *error = QObject::tr("Could not remove %1").arg(path);
            return false;
        }
        return true;
    }
    // Exec points at the running binary so a relocated install stays valid.
    const QString exec = QCoreApplication::applicationFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QObject::tr("Could not write %1").arg(path);
        return false;
    }
    QTextStream out(&f);
    out << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=LiteMon\n"
        << "Comment=LiteMon lightweight local metrics monitor\n"
        << "Exec=" << exec << "\n"
        << "Icon=io.github.litemon.LiteMon\n"
        << "Terminal=false\n"
        << "Categories=System;Monitor;\n"
        << "X-GNOME-Autostart-enabled=true\n";
    f.close();
    return true;
}
