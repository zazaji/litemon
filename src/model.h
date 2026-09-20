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
    double diskUsedGiB = lmNaN();
    double diskTotalGiB = lmNaN();
    double batteryPercent = lmNaN();
    double batteryPowerW = lmNaN();
    double batteryHealthPercent = lmNaN();
    QString batteryStatus;
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
