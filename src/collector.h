#pragma once

#include "model.h"
#include "gpucollector.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMap>

class SystemCollector {
public:
    SystemCollector();
    SystemMetric collect(bool includeGpu = true);

private:
    struct CpuTicks { quint64 total = 0; quint64 idle = 0; bool valid = false; };

    struct IoCounters { quint64 a = 0; quint64 b = 0; bool valid = false; };

    struct DiskIoCounters { QString device; quint64 readSectors = 0; quint64 writeSectors = 0; };

    CpuTicks readCpuTicks() const;
    QMap<int, CpuTicks> readCpuCoreTicks() const;
    void collectMemory(SystemMetric &m) const;
    void collectLoad(SystemMetric &m) const;
    void collectTemperature(SystemMetric &m) const;
    void collectBattery(SystemMetric &m) const;
    void collectDisks(SystemMetric &m) const;
    IoCounters readNetworkCounters() const;
    QVector<DiskIoCounters> readDiskCounters() const;

    CpuTicks lastCpu_;
    QMap<int, CpuTicks> lastCores_;
    IoCounters lastNetwork_;
    QVector<DiskIoCounters> lastDisk_;
    QElapsedTimer rateTimer_;
    GpuCollector gpuCollector_;
};
