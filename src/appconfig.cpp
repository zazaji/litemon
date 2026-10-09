#include "appconfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>
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
    c.traySensor3 = s.value("tray/sensor3").toString();
    c.traySensor4 = s.value("tray/sensor4").toString();
    // Fan alarms legitimately exceed 100 (RPM range tops at 10000); the old
    // 0..100 clamp silently truncated those, so it only bounds the floor now.
    c.trayAlarm1 = std::clamp(s.value("tray/alarm1", c.trayAlarm1).toDouble(), 0.0, 10000.0);
    c.trayAlarm2 = std::clamp(s.value("tray/alarm2", c.trayAlarm2).toDouble(), 0.0, 10000.0);
    c.trayAlarm3 = std::clamp(s.value("tray/alarm3", c.trayAlarm3).toDouble(), 0.0, 10000.0);
    c.trayAlarm4 = std::clamp(s.value("tray/alarm4", c.trayAlarm4).toDouble(), 0.0, 10000.0);
    c.traySeparate = s.value("tray/separate", c.traySeparate).toBool();
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
    s.setValue("tray/sensor3", traySensor3);
    s.setValue("tray/sensor4", traySensor4);
    s.setValue("tray/alarm1", trayAlarm1);
    s.setValue("tray/alarm2", trayAlarm2);
    s.setValue("tray/alarm3", trayAlarm3);
    s.setValue("tray/alarm4", trayAlarm4);
    s.setValue("tray/separate", traySeparate);
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
    // Preferred path on Linux: manage real systemd user units so the toggle
    // is visible in `systemctl --user` and survives across session types.
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString runtime = env.value("XDG_RUNTIME_DIR");
    const QString systemctl = QStandardPaths::findExecutable(QStringLiteral("systemctl"));
    if (!systemctl.isEmpty() && !runtime.isEmpty()
        && QFileInfo::exists(runtime + QStringLiteral("/systemd"))) {
        // Transient systemd-run GUI instances cannot be enabled, so install a
        // permanent unit next to the collector's and enable both units.
        QString configHome = env.value("XDG_CONFIG_HOME");
        if (configHome.isEmpty()) configHome = QDir::homePath() + "/.config";
        const QString unitDir = configHome + QStringLiteral("/systemd/user");
        QDir().mkpath(unitDir);
        QFile unit(unitDir + QStringLiteral("/litemon-gui.service"));
        if (!unit.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error) *error = QObject::tr("Could not write %1").arg(unit.fileName());
            return false;
        }
        QTextStream out(&unit);
        out << "[Unit]\n"
            << "Description=LiteMon GUI\n"
            << "PartOf=graphical-session.target\n"
            << "After=graphical-session.target\n\n"
            << "[Service]\n"
            << "ExecStart=" << QCoreApplication::applicationFilePath() << "\n"
            << "Restart=on-failure\n\n"
            << "[Install]\n"
            << "WantedBy=graphical-session.target\n";
        out.flush();
        unit.close();
        auto run = [&systemctl](const QStringList &args) {
            QProcess p;
            p.start(systemctl, args);
            return p.waitForFinished(10000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
        };
        // Reload picks up the freshly written unit; best effort either way.
        run({QStringLiteral("--user"), QStringLiteral("daemon-reload")});
        const QStringList units = {QStringLiteral("litemon-collector.service"),
                                   QStringLiteral("litemon-gui.service")};
        QStringList toggle = {QStringLiteral("--user"), enable ? QStringLiteral("enable")
                                                               : QStringLiteral("disable")};
        toggle += units;
        if (!run(toggle)) {
            if (error) *error = QObject::tr("Could not toggle litemon user services");
            return false;
        }
        if (enable) {
            // Idempotent when the collector already runs; the running GUI is
            // left alone so no second instance is started.
            run({QStringLiteral("--user"), QStringLiteral("start"),
                 QStringLiteral("litemon-collector.service")});
        }
        // A legacy XDG entry would launch a second GUI alongside the unit.
        QFile::remove(autostartFilePath());
        return true;
    }
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

bool AppConfig::setCollectorRunning(bool run, QString *error) {
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString runtime = env.value("XDG_RUNTIME_DIR");
    const QString systemctl = QStandardPaths::findExecutable(QStringLiteral("systemctl"));
    if (systemctl.isEmpty() || runtime.isEmpty()
        || !QFileInfo::exists(runtime + QStringLiteral("/systemd"))) {
        if (error) *error = QObject::tr("systemd user session is not available");
        return false;
    }
    QProcess p;
    p.start(systemctl, {QStringLiteral("--user"), run ? QStringLiteral("start") : QStringLiteral("stop"),
                        QStringLiteral("litemon-collector.service")});
    if (!p.waitForFinished(10000) || p.exitCode() != 0) {
        if (error) *error = QObject::tr("Could not %1 litemon-collector.service")
                                 .arg(run ? QStringLiteral("start") : QStringLiteral("stop"));
        return false;
    }
    return true;
}

