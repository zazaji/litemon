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

private:
    QVector<GpuMetric> collectNvidia();
    QVector<GpuMetric> collectIntel();
    GpuMetric collectIntelCard(const QString &cardPath, const QString &cardName);
    // NVIDIA VGA/3D PCI device names (BDFs) below /sys/bus/pci/devices.
    QStringList nvidiaPciDevices() const;
    // Placeholder row for a known NVIDIA device with no live metrics.
    // Reads power/runtime_status unless stateOverride is given; a suspended
    // device reports utilization 0, otherwise metrics stay unknown (NaN).
    static GpuMetric nvidiaPlaceholder(const QString &bdf, const QString &stateOverride = {});
    bool allNvidiaRuntimeSuspended(QStringList *pciDevices = nullptr) const;
    static double findIntelTemperature(const QString &cardPath);
    static double findIntelFrequency(const QString &cardPath);
    static QPair<double,double> findIntelVram(const QString &cardPath);
};
