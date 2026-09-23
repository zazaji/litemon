#pragma once

#include <QString>
#include <QVector>
#include <limits>

inline double lmNaN() { return std::numeric_limits<double>::quiet_NaN(); }

struct GpuMetric {
    QString id;
    QString vendor;
    QString name;
    QString driver;
    QString state = "active";
    double utilization = lmNaN();
    double memoryUsedMiB = lmNaN();
    double memoryTotalMiB = lmNaN();
    double temperatureC = lmNaN();
    double powerW = lmNaN();
    double frequencyMHz = lmNaN();
};

struct DiskInfo {
    qint64 timestamp = 0;
    QString mountPoint;
    double totalGiB = lmNaN();
    double usedGiB = lmNaN();
    double readMiBs = lmNaN();
    double writeMiBs = lmNaN();
};

// One hwmon temperature/fan reading. label falls back to the chip name when
// the kernel exposes no per-channel label.
struct SensorInfo {
    QString chip;
    QString label;
    double tempC = lmNaN();
};

struct FanInfo {
    QString chip;
    QString label;
    double rpm = lmNaN();
};

// One live process snapshot (GUI-side /proc sampling; not stored in history).
struct ProcInfo {
    qint64 pid = 0;
    QString name;
    QString state;
    double cpuPercent = lmNaN();
    double rssMiB = lmNaN();
    // VmSwap from /proc/<pid>/status; NaN when the process has no VmSwap
    // line (never swapped) or the read failed.
    double swapMiB = lmNaN();
    qint64 threads = 0;
};

struct SystemMetric {
    qint64 timestamp = 0;
    double cpuUsage = lmNaN();
    double cpuTemperatureC = lmNaN();
    double load1 = lmNaN();
    // Per-core utilization, indexed by Linux core id (cpu0 -> [0]); NaN when
    // a core reported no delta (e.g. offline between samples).
    QVector<double> cpuCores;
    double memoryUsedMiB = lmNaN();
    double memoryTotalMiB = lmNaN();
    double swapUsedMiB = lmNaN();
    double swapTotalMiB = lmNaN();
    double networkRxMiBs = lmNaN();
    double networkTxMiBs = lmNaN();
    double diskReadMiBs = lmNaN();
    double diskWriteMiBs = lmNaN();
    QVector<DiskInfo> disks;
    double batteryPercent = lmNaN();
    double batteryPowerW = lmNaN();
    double batteryHealthPercent = lmNaN();
    double batteryTemperatureC = lmNaN();
    QString batteryStatus;
    // Mean NVMe composite temperature across nvme hwmon channels; NaN when
    // no nvme hwmon exists. Persisted so long-range temperature charts work.
    double nvmeTemperatureC = lmNaN();
    // Pressure Stall Information, "some" avg10 (0-100). NaN when the kernel
    // has no /proc/pressure (CONFIG_PSI disabled or pre-4.20).
    double psiCpuSome = lmNaN();
    double psiMemSome = lmNaN();
    double psiIoSome = lmNaN();
    QVector<SensorInfo> sensors;
    QVector<FanInfo> fans;
    // Top-20 process ranking by composite score (CPU%+2)×(10MB+RSS MB),
    // computed by the collector and persisted (procs_raw/procs_5m domains);
    // live GUI sampling is separate.
    QVector<ProcInfo> topProcs;
    QVector<GpuMetric> gpus;
};

// One per-core sample returned by MetricsDatabase::cpuCoreHistory().
struct CpuCoreSample {
    qint64 timestamp = 0;
    int core = 0;
    double utilization = lmNaN();
};

// Percentile band for envelope charts (used for long time ranges).
struct BandPoint {
    qint64 timestamp = 0;
    double p01 = lmNaN(); // 1st percentile
    double p25 = lmNaN(); // 25th percentile
    double p50 = lmNaN(); // 50th percentile (median)
    double p75 = lmNaN(); // 75th percentile
    double p99 = lmNaN(); // 99th percentile
};
