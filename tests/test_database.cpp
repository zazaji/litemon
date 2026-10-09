#include "database.h"
#include <QCoreApplication>
#include <QDate>
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
        m.memoryUsedPct = 30.0 + i;
        DiskInfo d; d.timestamp = m.timestamp; d.mountPoint = "/"; d.usedPct = 40.0 + i;
        m.disks = {d};
        m.networkRxMiBs = 1.0;
        m.networkTxMiBs = 0.25;
        m.batteryPercent = 80.0;
        GpuMetric g; g.id="intel:card0"; g.vendor="Intel"; g.name="Intel GPU"; g.utilization=30+i%10; m.gpus={g};
        if (!db.insert(m, &error)) return 3;
    }
    if (db.schemaVersion() != 9) return 4;
    if (!db.integrityCheck(&error)) return 9;
    if (!db.maintain(now, &error)) return 8;
    // v7: procs_daily table exists and the legacy per-sample ranking tables
    // are gone.
    {
        QSqlDatabase verify = QSqlDatabase::addDatabase("QSQLITE", "test-verify-v7");
        verify.setDatabaseName(tmp.filePath("metrics.sqlite"));
        if (!verify.open()) return 70;
        QSqlQuery v(verify);
        if (!v.exec("SELECT COUNT(*) FROM procs_daily") || !v.next()) return 71;
        if (v.exec("SELECT 1 FROM procs_raw") || v.exec("SELECT 1 FROM procs_5m")) return 72;
        verify.close();
        QSqlDatabase::removeDatabase("test-verify-v7");
    }
    const auto latest = db.latestSystem();
    if (!latest || std::abs(latest->memoryUsedPct - 69.0) > 0.051) return 5;
    {
        const auto dh = db.diskHistory("/", now - 3600, now, 1000);
        if (dh.isEmpty()) return 14;
        if (dh.first().usedPct < 39.9 || dh.first().usedPct > 79.1) return 15;
    }
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
            m.memoryUsedPct = 25.0;
            GpuMetric g; g.id="intel:card0"; g.vendor="Intel"; g.name="Intel GPU"; g.utilization=40.0;
            m.gpus={g};
            if (!db.insert(m, &error)) return 20;
        }
        const qint64 oldTs = now - 8 * 86400; // 8 days ago: beyond detail keep
        SystemMetric old;
        old.timestamp = oldTs;
        old.cpuUsage = 10.0;
        old.memoryUsedPct = 33.0;
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
    // A GPU with only frequency (e.g. Intel iGPU with no hwmon sensors) IS
    // considered signal-bearing — frequency alone is meaningful data.
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
        if (!v.exec() || !v.next() || v.value(0).toLongLong() != 1) return 63;
        v.prepare("SELECT COUNT(*) FROM system_raw WHERE ts=:ts");
        v.bindValue(":ts", now - 102);
        if (!v.exec() || !v.next() || v.value(0).toLongLong() != 0) return 64;
        v.prepare("SELECT COUNT(*) FROM system_raw WHERE ts=:ts");
        v.bindValue(":ts", now - 101);
        if (!v.exec() || !v.next() || v.value(0).toLongLong() != 1) return 65;
        verify.close();
        QSqlDatabase::removeDatabase("test-verify-nosignal");
        if (!db.gpuIds(true).contains("intel:card9")) return 66;
        bool foundIntel = false;
        for (const auto &lg : db.latestGpus()) { if (lg.id == "intel:card9") foundIntel = true; }
        if (!foundIntel) return 67;
        // Fully empty sample (no metrics, no battery, no GPUs) is still skipped.
        SystemMetric m2;
        m2.timestamp = now - 103;
        if (!db.insert(m2, &error)) return 68;
        {
            QSqlDatabase v2 = QSqlDatabase::addDatabase("QSQLITE", "test-verify-empty");
            v2.setDatabaseName(tmp.filePath("metrics.sqlite"));
            if (!v2.open()) return 69;
            QSqlQuery q2(v2);
            q2.prepare("SELECT COUNT(*) FROM system_raw WHERE ts=:ts");
            q2.bindValue(":ts", now - 103);
            if (!q2.exec() || !q2.next() || q2.value(0).toLongLong() != 0) return 70;
            v2.close();
            QSqlDatabase::removeDatabase("test-verify-empty");
        }
    }
    // v7 procs_daily: additive upsert accumulates CPU seconds across merges
    // and keeps the peak RSS per process and day.
    {
        const qint64 dayStart = QDateTime(QDate::currentDate(), QTime(0, 0)).toSecsSinceEpoch();
        ProcDailyAgg a; a.name = "firefox"; a.cpuSec = 10.0; a.maxRssMiB = 500.0;
        if (!db.mergeProcessDaily(dayStart, {a}, &error)) return 80;
        ProcDailyAgg b; b.name = "firefox"; b.cpuSec = 5.0; b.maxRssMiB = 300.0;
        ProcDailyAgg c; c.name = "python"; c.cpuSec = 7.0; c.maxRssMiB = lmNaN();
        if (!db.mergeProcessDaily(dayStart, {b, c}, &error)) return 81;
        QSqlDatabase verify = QSqlDatabase::addDatabase("QSQLITE", "test-verify-procs");
        verify.setDatabaseName(tmp.filePath("metrics.sqlite"));
        if (!verify.open()) return 82;
        QSqlQuery v(verify);
        v.prepare("SELECT cpu_sec, max_rss FROM procs_daily WHERE day=:day AND kind='daily' AND name=:name");
        v.bindValue(":day", dayStart); v.bindValue(":name", "firefox");
        if (!v.exec() || !v.next() || v.value(0).toLongLong() != 15 || v.value(1).toLongLong() != 500) return 83;
        v.bindValue(":day", dayStart); v.bindValue(":name", "python");
        if (!v.exec() || !v.next() || v.value(0).toLongLong() != 7 || v.value(1).toLongLong() != 0) return 84;
        verify.close();
        QSqlDatabase::removeDatabase("test-verify-procs");
    }
    // v9 GPU VRAM: only the used percentage is persisted (x10). 2048/4096 MiB
    // stores 500 (=50.0%) and latestGpus()/gpuHistory() return it as percent;
    // a device with no total (Intel shared memory) stores NULL.
    {
        SystemMetric m;
        m.timestamp = now - 104;
        m.cpuUsage = 5.0;
        GpuMetric g; g.id="nvidia:mem"; g.vendor="NVIDIA"; g.name="NVIDIA GPU";
        g.memoryUsedMiB = 2048.0; g.memoryTotalMiB = 4096.0;
        m.gpus={g};
        if (!db.insert(m, &error)) return 90;
        GpuMetric shared; shared.id="intel:sh"; shared.vendor="Intel"; shared.name="Intel GPU";
        shared.memoryUsedMiB = 256.0; // no total: not convertible to percent
        m.gpus.push_back(shared);
        if (!db.insert(m, &error)) return 91;
        double noTotalStored = lmNaN();
        for (const auto &lg : db.latestGpus()) {
            if (lg.id == "nvidia:mem" && std::abs(lg.memoryUsedPct - 50.0) > 0.06) return 92;
            if (lg.id == "intel:sh") noTotalStored = lg.memoryUsedPct;
        }
        // VRAM present but no total (Intel shared memory) stores NULL percent.
        if (!std::isnan(noTotalStored)) return 94;
        const auto h = db.gpuHistory("nvidia:mem", now - 600, now);
        if (h.isEmpty() || !std::isfinite(h.last().gpus[0].memoryUsedPct)
            || std::abs(h.last().gpus[0].memoryUsedPct - 50.0) > 0.01) return 93;
    }
    // Battery health sparsification: one write per UTC day (plus on a
    // >=0.1% change); rows in between store NULL, and latestSystem() falls
    // back to the newest stored value when the latest row is NULL.
    {
        const qint64 dayStart = now - (now % 86400);
        const qint64 prevDay = dayStart - 86400;
        auto makeBattery = [&](qint64 ts, double health) {
            SystemMetric m;
            m.timestamp = ts;
            m.batteryPercent = 80.0;
            m.batteryHealthPercent = health;
            return m;
        };
        SystemMetric m1 = makeBattery(prevDay + 3600, 97.0); // first of yesterday
        SystemMetric m2 = makeBattery(prevDay + 3610, 97.0); // same day, same value -> NULL
        SystemMetric m3 = makeBattery(prevDay + 3620, 96.5); // value changed -> stored
        SystemMetric m4 = makeBattery(now - 4, 96.5);             // new UTC day -> stored again
        if (!db.insert(m1, &error)) return 95;
        if (!db.insert(m2, &error)) return 95;
        if (!db.insert(m3, &error)) return 95;
        if (!db.insert(m4, &error)) return 95;
        SystemMetric m5;
        m5.timestamp = now - 3; // newest row without battery data
        m5.cpuUsage = 7.0;
        if (!db.insert(m5, &error)) return 95;
        QSqlDatabase verify = QSqlDatabase::addDatabase("QSQLITE", "test-verify-health");
        verify.setDatabaseName(tmp.filePath("metrics.sqlite"));
        if (!verify.open()) return 95;
        QSqlQuery v(verify);
        auto healthAt = [&v](qint64 ts) -> QVariant {
            v.prepare("SELECT battery_health FROM system_raw WHERE ts=:ts");
            v.bindValue(":ts", ts);
            return (v.exec() && v.next()) ? v.value(0) : QVariant();
        };
        if (healthAt(prevDay + 3600) != 970 || !healthAt(prevDay + 3610).isNull()
            || healthAt(prevDay + 3620) != 965 || healthAt(now - 4) != 965) return 96;
        verify.close();
        QSqlDatabase::removeDatabase("test-verify-health");
        const auto withBattery = db.latestSystem();
        if (!withBattery || !std::isfinite(withBattery->batteryHealthPercent)
            || std::abs(withBattery->batteryHealthPercent - 96.5) > 0.06) return 97;
    }
    std::puts("Database PASS");
    return 0;
}
