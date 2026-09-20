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
    std::puts("GPU parser PASS");
    return 0;
}
