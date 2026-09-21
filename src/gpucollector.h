#pragma once

#include "model.h"
#include <QJsonObject>
#include <QPair>
#include <QStringList>
#include <QVector>

class GpuCollector {
public:
    QVector<GpuMetric> collect();
    static QVector<QJsonObject> parseIntelGpuTopObjects(const QByteArray &raw);
    // Canonicalize an nvidia-smi pci.bus_id to the sysfs BDF form (dddd:bb:dd.f,
    // lowercase) so a GPU keeps one stable ID whether suspended or active.
    static QString normalizeNvidiaBusId(const QString &bus);

    // One NPU parsed from a `npu-smi info` summary table. Ascend 910/310
    // report per-device rows (name/power/temp) paired with per-chip rows
    // (bus id, AICore utilization, memory).
    struct NpuSmiDevice {
        QString name;
        QString busId;
        double powerW = lmNaN();
        double temperatureC = lmNaN();
        double utilization = lmNaN();
        double memoryUsedMiB = lmNaN();
        double memoryTotalMiB = lmNaN();
    };
    static QVector<NpuSmiDevice> parseNpuSmiSummary(const QByteArray &raw);

private:
    QVector<GpuMetric> collectNvidia();
    QVector<GpuMetric> collectIntel();
    QVector<GpuMetric> collectAmd();
    static GpuMetric collectAmdCard(const QString &cardPath, const QString &cardName);
    QVector<GpuMetric> collectHuawei();
    QStringList huaweiPciDevices() const;
    static GpuMetric huaweiPlaceholder(const QString &bdf);
    GpuMetric collectIntelCard(const QString &cardPath, const QString &cardName);
    // NVIDIA VGA/3D PCI device names (BDFs) below /sys/bus/pci/devices.
    QStringList nvidiaPciDevices() const;
    // Placeholder row for a known NVIDIA device with no live metrics.
    // Reads power/runtime_status unless stateOverride is given; a suspended
    // device reports utilization 0, otherwise metrics stay unknown (NaN).
    static GpuMetric nvidiaPlaceholder(const QString &bdf, const QString &stateOverride = {});
    bool allNvidiaRuntimeSuspended(QStringList *pciDevices = nullptr) const;
    // Max valid hwmon temperature below a DRM card (works for i915/xe and amdgpu).
    static double findHwmonTemperature(const QString &cardPath);
    static double findIntelFrequency(const QString &cardPath);
    static QPair<double,double> findIntelVram(const QString &cardPath);
    static QPair<double,double> findAmdVram(const QString &cardPath);
};
