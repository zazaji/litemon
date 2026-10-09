#pragma once

#include <QString>

struct AppConfig {
    // Minimum sampling cadence is 60 seconds: finer sampling multiplies
    // database growth (per-core rows scale with core count) while adding no
    // useful signal to minute-scale history charts.
    int sampleIntervalSec = 60;
    int gpuIntervalSec = 60;
    // Fine-grained detail rows: calendar days kept (minimum 6).
    int detailRetentionDays = 7;
    // Compressed 5-minute-average archive: days kept.
    int archiveRetentionDays = 365;
    int maintenanceIntervalSec = 60;
    int historyTargetPoints = 900;
    bool collectGpu = true;
    // Tray icon digits: sensor keys "chip|label" from the live hwmon scan.
    // Empty string = slot disabled; the UI compacts enabled slots to the front
    // (order is normalized on save/load).
    QString traySensor1;
    QString traySensor2;
    QString traySensor3;
    QString traySensor4;
    // Alarm thresholds for the tray slots, in the sensor's own unit: % for
    // usage sources (e.g. memory), RPM for fans, °C for temperature sensors.
    // 0 disables the alarm; when a slot's value reaches the threshold the
    // digit is drawn in red.
    double trayAlarm1 = 0.0;
    double trayAlarm2 = 0.0;
    double trayAlarm3 = 0.0;
    double trayAlarm4 = 0.0;
    // false: all enabled slots share one tray icon (digits side by side);
    // true: each enabled slot gets its own tray icon showing one large digit.
    bool traySeparate = false;
    // Appearance: "system" | "light" | "dark" (palette switch, style kept).
    QString theme = "system";
    // Launch the GUI automatically at login (systemd user units when
    // available, XDG autostart entry otherwise).
    bool autostart = false;

    static AppConfig load();
    void save() const;
    static QString configPath();
    // systemd user session: enable/disable litemon-collector.service and a
    // freshly written litemon-gui.service. Without systemd this falls back to
    // ~/.config/autostart/io.github.litemon.LiteMon.desktop. Returns false
    // with a message in *error when the toggle cannot be applied.
    static bool setAutostartEnabled(bool enable, QString *error);
    // Start/stop the collector systemd user service from the UI (Pause/Resume
    // data collection). Returns false with a message in *error when no
    // systemd user session is available (e.g. macOS/Windows builds).
    static bool setCollectorRunning(bool run, QString *error);
    static QString autostartFilePath();
};
