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
    // Empty string = slot disabled; slot 2 alone without slot 1 is treated as
    // slot 1 by the UI (order is normalized on load).
    QString traySensor1;
    QString traySensor2;
    // Appearance: "system" | "light" | "dark" (palette switch, style kept).
    QString theme = "system";
    // Launch the GUI automatically at login (XDG autostart entry).
    bool autostart = false;

    static AppConfig load();
    void save() const;
    static QString configPath();
    // Create or remove ~/.config/autostart/io.github.litemon.LiteMon.desktop
    // so the desktop session starts the GUI at login. Returns false with a
    // message in *error when the entry cannot be written or removed.
    static bool setAutostartEnabled(bool enable, QString *error);
    static QString autostartFilePath();
};
