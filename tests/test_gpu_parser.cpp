#include "gpucollector.h"
#include <QCoreApplication>
#include <cmath>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QByteArray fixture = R"JSON([
      {"period":{"duration":300},"frequency":{"actual":500},"power":{"GPU":1.2,"Package":8.4},"engines":{"Render/3D/0":{"busy":12.3}}},
      {"period":{"duration":300},"frequency":{"actual":650},"power":{"GPU":2.4,"Package":10.1},"engines":{"Render/3D/0":{"busy":44.0},"Video/0":{"busy":8.0}}}
    ])JSON";
    const auto objects = GpuCollector::parseIntelGpuTopObjects(fixture);
    if (objects.size() != 2) return 1;
    const auto last = objects.last();
    if (std::abs(last.value("power").toObject().value("GPU").toDouble() - 2.4) > 0.001) return 2;
    if (std::abs(last.value("frequency").toObject().value("actual").toDouble() - 650.0) > 0.001) return 3;
    // nvidia-smi bus IDs must canonicalize to the sysfs BDF form so a GPU
    // keeps one stable ID whether suspended (sysfs) or active (nvidia-smi).
    if (GpuCollector::normalizeNvidiaBusId("00000000:01:00.0") != "0000:01:00.0") return 4;
    if (GpuCollector::normalizeNvidiaBusId("00000000:0A:00.0") != "0000:0a:00.0") return 5;
    if (GpuCollector::normalizeNvidiaBusId("01:00.0") != "0000:01:00.0") return 6;
    if (GpuCollector::normalizeNvidiaBusId("1:00.0") != "0000:01:00.0") return 7;
    if (GpuCollector::normalizeNvidiaBusId("0000:01:00.0") != "0000:01:00.0") return 8;

    // npu-smi summary table: device row (index, name, power, temperature,
    // huge-page counter) paired with a chip row (bus id, AICore %, memory).
    // The trailing "0 / 0" huge-page counter must not be mistaken for power.
    const QByteArray npuSmi = R"(
+-------------------------------------------------------------------------------------------+
| npu-smi 24.1.rc2                                Version: 24.1.rc2                         |
+---------------------------+-----------------+---------------------------------------------+
| NPU     Name              | Health          | Power(W)     Temp(C)         Huge- pages(page/page) |
| Chip    Device            | Bus-Id          | AICore(%)    Memory-Usage(MB)                |
+===========================+=================+=============================================+
| 0         910B4           | OK              | 63.2         45              0    / 0      |
| 0         0               | 0000:C1:00.0    | 0            737  / 32768                     |
+===========================+=================+=============================================+
)";
    const auto npus = GpuCollector::parseNpuSmiSummary(npuSmi);
    if (npus.size() != 1) return 9;
    if (npus[0].name != "910B4") return 10;
    if (npus[0].busId != "0000:c1:00.0") return 10;
    if (std::abs(npus[0].powerW - 63.2) > 0.01) return 10;
    if (std::abs(npus[0].temperatureC - 45.0) > 0.01) return 10;
    if (std::abs(npus[0].utilization - 0.0) > 0.01) return 10;
    if (std::abs(npus[0].memoryUsedMiB - 737.0 / 1.048576) > 0.5) return 10;
    if (std::abs(npus[0].memoryTotalMiB - 32768.0 / 1.048576) > 0.5) return 10;
    // No trailing newline after the last device/chip pair.
    const auto npus2 = GpuCollector::parseNpuSmiSummary(
        "| 1         310P3           | OK              | 40.0         51              0    / 0\n"
        "| 1         0               | 0000:41:00.0    | 35           1024 / 24576");
    if (npus2.size() != 1) return 11;
    if (npus2[0].name != "310P3" || npus2[0].busId != "0000:41:00.0") return 11;
    if (std::abs(npus2[0].utilization - 35.0) > 0.01) return 11;
    std::puts("GPU parser PASS");
    return 0;
}
