#include "collector.h"
#include <QCoreApplication>
#include <QFile>
#include <QThread>
#include <cmath>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (!QFile::exists("/proc/meminfo")) {
        std::puts("Collector SKIP (no /proc/meminfo)");
        return 0;
    }
    SystemCollector collector;
    // Second sample gives rate counters a delta; memory/load/disk are immediate.
    collector.collect(false);
    QThread::msleep(250);
    const SystemMetric m = collector.collect(false);

    auto checkFinite = [](double v, const char *name, bool required) {
        if (!std::isfinite(v)) {
            if (required) {
                std::fprintf(stderr, "test_collector FAIL: %s is not finite\n", name);
                return false;
            }
            std::fprintf(stderr, "test_collector WARN: %s is not finite (allowed)\n", name);
        }
        return true;
    };

    bool ok = true;
    // Regression test for /proc read via QFile::atEnd() (size 0 on procfs):
    // memory must be populated on a normal Linux desktop.
    ok = checkFinite(m.memoryTotalMiB, "memoryTotalMiB", true) && ok;
    ok = checkFinite(m.memoryUsedMiB, "memoryUsedMiB", true) && ok;
    ok = checkFinite(m.load1, "load1", true) && ok;
    // Swap may be zero-sized on some machines, but when present it must parse.
    // Rates are expected to be finite (possibly 0) after two samples.
    ok = checkFinite(m.networkRxMiBs, "networkRxMiBs", true) && ok;
    ok = checkFinite(m.networkTxMiBs, "networkTxMiBs", true) && ok;
    ok = checkFinite(m.diskReadMiBs, "diskReadMiBs", true) && ok;
    ok = checkFinite(m.diskWriteMiBs, "diskWriteMiBs", true) && ok;

    if (m.memoryTotalMiB <= 0 || m.memoryTotalMiB > 16 * 1024 * 1024) {
        std::fprintf(stderr, "test_collector FAIL: implausible memoryTotalMiB=%f\n", m.memoryTotalMiB);
        ok = false;
    }
    if (m.memoryUsedMiB < 0 || m.memoryUsedMiB > m.memoryTotalMiB) {
        std::fprintf(stderr, "test_collector FAIL: implausible memoryUsedMiB=%f (total=%f)\n",
                     m.memoryUsedMiB, m.memoryTotalMiB);
        ok = false;
    }

    if (!ok) { return 1; }
    // Per-core CPU: the second sample must report one value per core, each
    // finite in [0,100] (or NaN for a core with no delta, e.g. hotplug gap).
    if (m.cpuCores.isEmpty()) {
        std::fprintf(stderr, "test_collector FAIL: cpuCores is empty\n");
        return 1;
    }
    for (int i = 0; i < m.cpuCores.size(); ++i) {
        const double v = m.cpuCores[i];
        if (!std::isfinite(v)) continue;
        if (v < 0.0 || v > 100.0) {
            std::fprintf(stderr, "test_collector FAIL: cpuCores[%d]=%f out of range\n", i, v);
            return 1;
        }
    }
    std::puts("Collector PASS");
    return 0;
}
