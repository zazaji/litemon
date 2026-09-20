#include "database.h"
#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QDateTime>
#include <cmath>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;
    MetricsDatabase db(tmp.filePath("metrics.sqlite"), "test-db");
    QString error;
    if (!db.open(&error)) return 2;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (int i = 0; i < 40; ++i) {
        SystemMetric m;
        m.timestamp = now - 200 + i * 5;
        m.cpuUsage = 10 + (i % 20);
        m.memoryUsedMiB = 4096 + i;
        m.memoryTotalMiB = 32768;
        m.networkRxMiBs = 1.0;
        m.networkTxMiBs = 0.25;
        m.batteryPercent = 80.0;
        GpuMetric g; g.id="intel:card0"; g.vendor="Intel"; g.name="Intel GPU"; g.utilization=30+i%10; m.gpus={g};
        if (!db.insert(m, &error)) return 3;
    }
    if (db.schemaVersion() != 4) return 4;
    if (!db.integrityCheck(&error)) return 9;
    if (!db.maintain(now, &error)) return 8;
    const auto latest = db.latestSystem();
    if (!latest || latest->memoryTotalMiB != 32768) return 5;
    if (db.systemHistory(now-3600, now).isEmpty()) return 6;
    if (db.gpuHistory("intel:card0", now-3600, now).isEmpty()) return 7;
    // A second GPU seen only on an older tick must still be reported by
    // latestGpus() (latest row per device, not just the global max ts), and
    // its name must resolve so it stays selectable in the GUI.
    {
        SystemMetric m;
        m.timestamp = now - 150;
        GpuMetric g; g.id="nvidia:0000:01:00.0"; g.vendor="NVIDIA"; g.name="NVIDIA GPU"; g.state="active"; g.utilization=42;
        m.gpus={g};
        if (!db.insert(m, &error)) return 10;
        const auto latestGpus = db.latestGpus();
        bool hasIntel = false, hasNvidia = false;
        for (const auto &gpu : latestGpus) {
            if (gpu.id == "intel:card0") hasIntel = true;
            if (gpu.id == "nvidia:0000:01:00.0") hasNvidia = true;
        }
        if (!hasIntel || !hasNvidia) return 11;
        if (db.gpuName("nvidia:0000:01:00.0") != "NVIDIA GPU") return 12;
        if (!db.gpuIds().contains("nvidia:0000:01:00.0")) return 13;
    }
    // v3 compression: completed 5-minute buckets fold into system_5m/gpu_5m
    // as scaled integers, and week-old detail rows are cleared only after
    // being folded (compress-then-clear).
    {
        const qint64 bucket = (now - 3600) / 300 * 300; // completed bucket 1h ago
        for (int i = 0; i < 5; ++i) {
            SystemMetric m;
            m.timestamp = bucket + static_cast<qint64>(i) * 60;
            m.cpuUsage = 50.0;
            m.memoryUsedMiB = 8192;
            m.memoryTotalMiB = 32768;
            GpuMetric g; g.id="intel:card0"; g.vendor="Intel"; g.name="Intel GPU"; g.utilization=40.0;
            m.gpus={g};
            if (!db.insert(m, &error)) return 20;
        }
        const qint64 oldTs = now - 8 * 86400; // 8 days ago: beyond detail keep
        SystemMetric old;
        old.timestamp = oldTs;
        old.cpuUsage = 10.0;
        old.memoryTotalMiB = 32768;
        GpuMetric og; og.id="intel:card0"; og.vendor="Intel"; og.name="Intel GPU"; og.utilization=10.0;
        old.gpus={og};
        if (!db.insert(old, &error)) return 21;
        if (!db.maintain(now, &error)) return 22;
        QSqlDatabase verify = QSqlDatabase::addDatabase("QSQLITE", "test-verify");
        verify.setDatabaseName(tmp.filePath("metrics.sqlite"));
        if (!verify.open()) return 23;
        QSqlQuery v(verify);
        if (!v.exec("SELECT COUNT(*) FROM system_5m") || !v.next() || v.value(0).toLongLong() == 0) return 24;
        if (!v.exec("SELECT COUNT(*) FROM gpu_5m") || !v.next() || v.value(0).toLongLong() == 0) return 25;
        // Scaled-integer storage: cpu 50% is stored as integer 500.
        if (!v.exec("SELECT typeof(cpu_usage), cpu_usage FROM system_5m LIMIT 1") || !v.next()) return 26;
        if (v.value(0).toString() != "integer") return 27;
        v.prepare("SELECT cpu_usage FROM system_5m WHERE ts=:ts");
        v.bindValue(":ts", bucket);
        if (!v.exec() || !v.next() || v.value(0).toLongLong() != 500) return 28;
        // The 8-day-old detail row is gone from raw...
        v.prepare("SELECT COUNT(*) FROM system_raw WHERE ts=:ts");
        v.bindValue(":ts", oldTs);
        if (!v.exec() || !v.next() || v.value(0).toLongLong() != 0) return 29;
        // ...but its bucket survives in the archive.
        v.prepare("SELECT COUNT(*) FROM system_5m WHERE ts=:ts");
        v.bindValue(":ts", oldTs / 300 * 300);
        if (!v.exec() || !v.next() || v.value(0).toLongLong() == 0) return 30;
        verify.close();
        QSqlDatabase::removeDatabase("test-verify");
        // Scaled values round-trip through the public API.
        const auto hist = db.systemHistory(bucket, bucket + 299);
        if (hist.isEmpty() || std::abs(hist.first().cpuUsage - 50.0) > 0.051) return 31;
    }
    // v4 per-core CPU: completed buckets fold per (bucket, core) into
    // cpu_cores_5m as scaled integers; history and latest read them back.
    {
        const qint64 bucket = (now - 7200) / 300 * 300; // completed bucket 2h ago
        for (int i = 0; i < 5; ++i) {
            SystemMetric m;
            m.timestamp = bucket + static_cast<qint64>(i) * 60;
            m.cpuCores = {50.0, 60.0};
            if (!db.insert(m, &error)) return 40;
        }
        if (!db.maintain(now, &error)) return 41;
        QSqlDatabase verify = QSqlDatabase::addDatabase("QSQLITE", "test-verify-cores");
        verify.setDatabaseName(tmp.filePath("metrics.sqlite"));
        if (!verify.open()) return 42;
        QSqlQuery v(verify);
        if (!v.exec("SELECT COUNT(*) FROM cpu_cores_5m") || !v.next() || v.value(0).toLongLong() == 0) return 43;
        if (!v.exec("SELECT DISTINCT typeof(util) FROM cpu_cores_5m") || !v.next() || v.value(0).toString() != "integer") return 44;
        v.prepare("SELECT util FROM cpu_cores_5m WHERE ts=:ts AND core=:core");
        v.bindValue(":ts", bucket); v.bindValue(":core", 0);
        if (!v.exec() || !v.next() || v.value(0).toLongLong() != 500) return 45;
        v.bindValue(":ts", bucket); v.bindValue(":core", 1);
        if (!v.exec() || !v.next() || v.value(0).toLongLong() != 600) return 46;
        verify.close();
        QSqlDatabase::removeDatabase("test-verify-cores");
        const auto ids = db.cpuCoreIds();
        if (!ids.contains(0) || !ids.contains(1)) return 47;
        const auto ch = db.cpuCoreHistory(bucket, bucket + 299);
        double c0 = lmNaN(), c1 = lmNaN();
        for (const auto &s : ch) {
            if (s.core == 0 && !std::isfinite(c0)) c0 = s.utilization;
            if (s.core == 1 && !std::isfinite(c1)) c1 = s.utilization;
        }
        if (std::abs(c0 - 50.0) > 0.051 || std::abs(c1 - 60.0) > 0.051) return 48;
        const auto latestSys = db.latestSystem();
        if (!latestSys || latestSys->cpuCores.size() != 2) return 49;
        if (std::abs(latestSys->cpuCores[0] - 50.0) > 0.051 || std::abs(latestSys->cpuCores[1] - 60.0) > 0.051) return 50;
    }
    // No-signal GPU rows (primary metrics all NaN, e.g. an Intel iGPU with
    // no readable sensors, or a fully empty sample) are neither stored nor
    // reported, while the useful part of the sample still is.
    {
        SystemMetric m;
        m.timestamp = now - 101;
        m.cpuUsage = 5.0;
        GpuMetric g; g.id="intel:card9"; g.vendor="Intel"; g.name="Intel GPU"; g.frequencyMHz=700.0;
        m.gpus={g};
        if (!db.insert(m, &error)) return 60;
        SystemMetric empty;
        empty.timestamp = now - 102;
        if (!db.insert(empty, &error)) return 61;
        QSqlDatabase verify = QSqlDatabase::addDatabase("QSQLITE", "test-verify-nosignal");
        verify.setDatabaseName(tmp.filePath("metrics.sqlite"));
        if (!verify.open()) return 62;
        QSqlQuery v(verify);
        v.prepare("SELECT COUNT(*) FROM gpu_raw WHERE id=:id");
        v.bindValue(":id", "intel:card9");
        if (!v.exec() || !v.next() || v.value(0).toLongLong() != 0) return 63;
        v.prepare("SELECT COUNT(*) FROM system_raw WHERE ts=:ts");
        v.bindValue(":ts", now - 102);
        if (!v.exec() || !v.next() || v.value(0).toLongLong() != 0) return 64;
        v.prepare("SELECT COUNT(*) FROM system_raw WHERE ts=:ts");
        v.bindValue(":ts", now - 101);
        if (!v.exec() || !v.next() || v.value(0).toLongLong() != 1) return 65;
        verify.close();
        QSqlDatabase::removeDatabase("test-verify-nosignal");
        if (db.gpuIds(true).contains("intel:card9")) return 66;
        for (const auto &lg : db.latestGpus()) { if (lg.id == "intel:card9") return 67; }
    }
    std::puts("Database PASS");
    return 0;
}
