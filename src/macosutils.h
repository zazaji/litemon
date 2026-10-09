#pragma once

#ifdef __APPLE__

#include "model.h"

#include <QString>
#include <QVector>

// macOS live sampling helpers shared by the collector and the GUI's live
// process page. Everything here degrades gracefully: callers get empty
// vectors / NaN fields when an API is unavailable, never a crash.
namespace MacUtils {

// One process from sysctl KERN_PROC_ALL + libproc. cpuNanos is the process
// lifetime user+system CPU time; callers compute shares differentially.
struct MacProc {
    qint64 pid = 0;
    QString name;
    QString state;
    qint64 threads = 0;
    double rssMiB = lmNaN();
    double cpuNanos = lmNaN();
    double swapMiB = 0.0;
};

QVector<MacProc> listProcesses();

struct MacBattery {
    bool present = false;
    double percent = lmNaN();
    double powerW = lmNaN();   // positive = charging, negative = discharging
    double health = lmNaN();   // max/design capacity in percent
    double temperatureC = lmNaN();
    QString status;            // "Charging", "Discharging", "AC Attached"
};
MacBattery readBattery();

// Total physical memory in MiB; NaN on failure.
double memTotalMiB();

// RAM currently reclaimable (free + inactive pages), MiB; NaN on failure.
double memAvailableMiB();

// Swap capacity configured on the machine, MiB; 0/NaN when swap is off.
double swapTotalMiB();

// Hottest CPU core temperature via the AppleSMC user client (no root
// needed). NaN when no known temperature key answers.
double cpuTemperatureC();

// One raw SMC temperature key ("Tp01" style 4-char key). NaN on failure.
double smcTemperature(const char *key);

// Curated sensor list for the Sensors page: CPU (hottest core), GPU and
// NAND (the NVMe stand-in on Apple Silicon) entries with chip "SMC", plus
// the cooling fans written through *fans when given.
QVector<SensorInfo> readSensors(QVector<FanInfo> *fans = nullptr);

// Fans-only read (FNum + one RPM key per fan instead of the full temperature
// sweep): cheap enough for the 2 s overview poll.
QVector<FanInfo> readFans();

// Integrated-GPU telemetry from the IOAccelerator registry (Apple Silicon).
// gpuUtilizationPct is "Device Utilization %" from PerformanceStatistics;
// NaN on machines/OS versions where the key is missing.
double gpuUtilizationPct();
// Marketing model name ("Apple M1 Pro") from the registry, "Apple GPU" when
// the firmware string is unavailable.
QString gpuModelName();

// Menu-bar-only app presence: hide the Dock icon so the tray icon stays the
// single LiteMon presence (NSApplicationActivationPolicyAccessory). Call once
// at startup, after QApplication exists.
void hideDockIcon();

} // namespace MacUtils
#endif // __APPLE__
