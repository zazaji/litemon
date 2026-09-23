#include "database.h"
#include "appconfig.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QTime>
#include <QUuid>
#include <cmath>
#include <algorithm>

// Storage format (schema v4): SQLite has no float16 type — REAL always costs
// 8 bytes — so low-precision metrics are stored as scaled INTEGERs
// (fixed-point). SQLite packs small integers in 1-3 bytes, roughly 3-4x
// smaller than REALs, with precision matched to display needs:
//
//   percent (cpu, per-core, battery)  x10    (0.1%)
//   temperature ............... x10    (0.1 C)
//   load1 ..................... x100
//   memory/swap (MiB) ......... x1     (whole MiB)
//   rates (MiB/s) ............. x1000  (0.001 MiB/s)
//   disk usage (GiB) .......... x100   (0.01 GiB)
//   power (W) ................. x100   (0.01 W)
//   frequency (MHz) ........... x1
// NULL still means unknown; in-memory model stays double, scaling happens
// only at the database boundary (insert: x scale, read: / scale).

MetricsDatabase::MetricsDatabase(const QString &path, const QString &connectionName)
    : path_(path), connectionName_(connectionName.isEmpty() ? "litemon-" + QUuid::createUuid().toString(QUuid::WithoutBraces) : connectionName) {}

MetricsDatabase::~MetricsDatabase() {
    if (db_.isValid()) db_.close();
    const QString n = connectionName_;
    db_ = {};
    QSqlDatabase::removeDatabase(n);
}

bool MetricsDatabase::open(QString *error) {
    QDir().mkpath(QFileInfo(path_).absolutePath());
    db_ = QSqlDatabase::addDatabase("QSQLITE", connectionName_);
    db_.setDatabaseName(path_);
    if (!db_.open()) { if (error) *error = db_.lastError().text(); return false; }
    QSqlQuery q(db_);
    q.exec("PRAGMA journal_mode=WAL");
    q.exec("PRAGMA synchronous=NORMAL");
    q.exec("PRAGMA busy_timeout=3000");
    q.exec("PRAGMA wal_autocheckpoint=1000");
    q.exec("PRAGMA journal_size_limit=16777216");
    q.exec("PRAGMA temp_store=MEMORY");
    if (!execSchema(error)) return false;
    return migrate(error);
}

bool MetricsDatabase::execSchema(QString *error) {
    // v4 layout: two tables per domain. *_raw holds fine samples for the last
    // `detailDays` calendar days; *_5m holds compressed 5-minute averages for
    // the long-term archive. All metric columns are scaled INTEGERs (see top).
    // Per-core CPU utilization lives in its own domain (cpu_cores_*) because
    // the core count varies per machine.
    const QString systemCols = R"(
        ts INTEGER PRIMARY KEY,
        cpu_usage INTEGER, cpu_temp INTEGER, load1 INTEGER,
        mem_used INTEGER, mem_total INTEGER, swap_used INTEGER, swap_total INTEGER,
        net_rx INTEGER, net_tx INTEGER, disk_read INTEGER, disk_write INTEGER,
        disk_used INTEGER, disk_total INTEGER,
        battery_percent INTEGER, battery_power INTEGER, battery_health INTEGER, battery_status TEXT,
        psi_cpu INTEGER, psi_mem INTEGER, psi_io INTEGER, sensors TEXT,
        nvme_temp INTEGER, bat_temp INTEGER
    )";
    const QString gpuCols = R"(
        ts INTEGER NOT NULL, id TEXT NOT NULL, vendor TEXT, name TEXT, driver TEXT, state TEXT,
        util INTEGER, mem_used INTEGER, mem_total INTEGER, temp INTEGER, power INTEGER, freq INTEGER,
        PRIMARY KEY(ts,id)
    )";
    const QString cpuCoreCols = R"(
        ts INTEGER NOT NULL, core INTEGER NOT NULL, util INTEGER,
        PRIMARY KEY(ts,core)
    )";
    const QString diskCols = R"(
        ts INTEGER NOT NULL, mount TEXT NOT NULL, total INTEGER, used INTEGER,
        PRIMARY KEY(ts,mount)
    )";
    const QString procsCols = R"(
        ts INTEGER NOT NULL, kind TEXT NOT NULL, rank INTEGER NOT NULL,
        name TEXT NOT NULL, pid INTEGER, cpu INTEGER, rss INTEGER,
        PRIMARY KEY(ts,kind,rank)
    )";
    const QString procs5Cols = R"(
        ts INTEGER NOT NULL, kind TEXT NOT NULL, name TEXT NOT NULL,
        avg_cpu INTEGER, max_rss INTEGER,
        PRIMARY KEY(ts,kind,name)
    )";
    QSqlQuery q(db_);
    const QStringList stmts = {
        "CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL)",
        "CREATE TABLE IF NOT EXISTS system_raw (" + systemCols + ")",
        "CREATE TABLE IF NOT EXISTS system_5m (" + systemCols + ")",
        "CREATE TABLE IF NOT EXISTS gpu_raw (" + gpuCols + ")",
        "CREATE TABLE IF NOT EXISTS gpu_5m (" + gpuCols + ")",
        "CREATE TABLE IF NOT EXISTS cpu_cores_raw (" + cpuCoreCols + ")",
        "CREATE TABLE IF NOT EXISTS cpu_cores_5m (" + cpuCoreCols + ")",
        "CREATE TABLE IF NOT EXISTS disks_raw (" + diskCols + ")",
        "CREATE TABLE IF NOT EXISTS disks_5m (" + diskCols + ")",
        "CREATE TABLE IF NOT EXISTS procs_raw (" + procsCols + ")",
        "CREATE TABLE IF NOT EXISTS procs_5m (" + procs5Cols + ")",
        "CREATE INDEX IF NOT EXISTS idx_gpu_raw_id_ts ON gpu_raw(id,ts)",
        "CREATE INDEX IF NOT EXISTS idx_gpu_5m_id_ts ON gpu_5m(id,ts)",
        "CREATE INDEX IF NOT EXISTS idx_cpu_cores_raw_core_ts ON cpu_cores_raw(core,ts)",
        "CREATE INDEX IF NOT EXISTS idx_cpu_cores_5m_core_ts ON cpu_cores_5m(core,ts)",
        "CREATE INDEX IF NOT EXISTS idx_disks_raw_mount_ts ON disks_raw(mount,ts)",
        "CREATE INDEX IF NOT EXISTS idx_disks_5m_mount_ts ON disks_5m(mount,ts)",
        "CREATE INDEX IF NOT EXISTS idx_procs_raw_ts ON procs_raw(ts)",
        "CREATE INDEX IF NOT EXISTS idx_procs_5m_ts ON procs_5m(ts)"
    };
    for (const auto &s : stmts) if (!q.exec(s)) { if (error) *error = q.lastError().text(); return false; }
    return true;
}

void MetricsDatabase::bindScaledOrNull(QSqlQuery &q, const QString &name, double value, double scale) {
    if (std::isfinite(value)) { q.bindValue(name, static_cast<qlonglong>(std::llround(value * scale))); }
    else { q.bindValue(name, QVariant()); }
}

static double sqlScaled(const QVariant &v, double scale) { return v.isNull() ? lmNaN() : v.toDouble() / scale; }

// Live hwmon snapshot as JSON ("{"temperatures":[...],"fans":[...]}").
// Per-channel detail (e.g. one temperature per CPU core) is deliberately NOT
// persisted: it is redundant with cpu_usage history and duplicates within the
// same chip, so each chip contributes only its hottest channel here. The GUI
// enumerates full detail live from /sys/class/hwmon. Stored per raw sample;
// display only needs the latest row, so it is not aggregated into system_5m.
static QString sensorsToJson(const QVector<SensorInfo> &sensors, const QVector<FanInfo> &fans) {
    QHash<QString, QJsonObject> hottest;
    for (const auto &s : sensors) {
        QJsonObject o;
        o.insert("chip", s.chip);
        o.insert("label", s.label);
        o.insert("tempC", static_cast<double>(std::llround(s.tempC * 10.0)) / 10.0);
        const auto it = hottest.find(s.chip);
        if (it == hottest.end() || s.tempC > it->value("tempC").toDouble()) hottest.insert(s.chip, o);
    }
    QJsonObject root;
    QJsonArray temps;
    for (const auto &o : hottest) temps.append(o);
    root.insert("temperatures", temps);
    QJsonArray frs;
    for (const auto &f : fans) {
        QJsonObject o;
        o.insert("chip", f.chip);
        o.insert("label", f.label);
        o.insert("rpm", std::llround(f.rpm));
        frs.append(o);
    }
    root.insert("fans", frs);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

static void parseSensorsJson(const QString &json, QVector<SensorInfo> &sensors, QVector<FanInfo> &fans) {
    const auto doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject()) return;
    const auto root = doc.object();
    for (const auto &v : root.value("temperatures").toArray()) {
        const auto o = v.toObject();
        SensorInfo s;
        s.chip = o.value("chip").toString();
        s.label = o.value("label").toString();
        s.tempC = o.value("tempC").toDouble();
        if (s.chip.isEmpty()) continue;
        sensors.push_back(s);
    }
    for (const auto &v : root.value("fans").toArray()) {
        const auto o = v.toObject();
        FanInfo f;
        f.chip = o.value("chip").toString();
        f.label = o.value("label").toString();
        f.rpm = o.value("rpm").toDouble();
        if (f.chip.isEmpty()) continue;
        fans.push_back(f);
    }
}

// A GPU row carries signal when at least one primary metric (utilization,
// temperature, power, memory used) has a real reading. Rows with only
// secondary data (e.g. a bare frequency value, or all-NaN placeholders)
// are redundant: they cost disk space while rendering as empty charts, so
// they are neither stored nor shown.
static bool gpuHasSignal(const GpuMetric &g) {
    return std::isfinite(g.utilization) || std::isfinite(g.temperatureC)
        || std::isfinite(g.powerW) || std::isfinite(g.memoryUsedMiB)
        || std::isfinite(g.frequencyMHz);
}

// Mount points that should never appear as user-facing disk entries. Shared
// by diskMountPoints() and latestSystem() so both agree on the filter.
static bool isUserFacingMount(const QString &m) {
    static const QStringList skipPrefixes = {"/sys", "/var/lib/waydroid", "/boot/efi"};
    static const QStringList skipExact = {"/root", "/var/tmp"};
    for (const auto &p : skipPrefixes) { if (m.startsWith(p)) return false; }
    for (const auto &e : skipExact) { if (m == e) return false; }
    return true;
}

bool MetricsDatabase::insert(const SystemMetric &m, QString *error) {
    // Drop redundant data before touching the database: GPUs without signal,
    // offline per-core slots (NaN), and samples where every field is empty.
    QVector<GpuMetric> gpus;
    gpus.reserve(m.gpus.size());
    for (const auto &g : m.gpus) {
        if (gpuHasSignal(g)) gpus.push_back(g);
    }
    auto sysEmpty = [&] {
        const double vals[] = {m.cpuUsage, m.cpuTemperatureC, m.load1,
            m.memoryUsedMiB, m.memoryTotalMiB, m.swapUsedMiB, m.swapTotalMiB,
            m.networkRxMiBs, m.networkTxMiBs, m.diskReadMiBs, m.diskWriteMiBs,
            m.batteryPercent, m.batteryPowerW, m.batteryHealthPercent,
            m.psiCpuSome, m.psiMemSome, m.psiIoSome};
        for (double v : vals) { if (std::isfinite(v)) return false; }
        if (!m.batteryStatus.isEmpty()) return false;
        if (!gpus.isEmpty()) return false;
        if (!m.disks.isEmpty()) return false;
        if (!m.sensors.isEmpty() || !m.fans.isEmpty()) return false;
        if (!m.topProcs.isEmpty()) return false;
        for (double v : m.cpuCores) { if (std::isfinite(v)) return false; }
        return true;
    };
    if (sysEmpty()) return true;
    if (!db_.transaction()) { if (error) *error = db_.lastError().text(); return false; }
    QSqlQuery q(db_);
    q.prepare(R"(INSERT OR REPLACE INTO system_raw
        (ts,cpu_usage,cpu_temp,load1,mem_used,mem_total,swap_used,swap_total,
         net_rx,net_tx,disk_read,disk_write,disk_used,disk_total,
         battery_percent,battery_power,battery_health,battery_status,
         psi_cpu,psi_mem,psi_io,sensors,nvme_temp,bat_temp)
        VALUES(:ts,:cpu,:ct,:load,:mu,:mt,:su,:st,:nrx,:ntx,:dr,:dw,:du,:dt,:bp,:bw,:bh,:bs,:pc,:pm,:pi,:sn,:nt,:bt))");
    q.bindValue(":ts", m.timestamp);
    bindScaledOrNull(q,":cpu",m.cpuUsage,10.0); bindScaledOrNull(q,":ct",m.cpuTemperatureC,10.0); bindScaledOrNull(q,":load",m.load1,100.0);
    bindScaledOrNull(q,":mu",m.memoryUsedMiB,1.0); bindScaledOrNull(q,":mt",m.memoryTotalMiB,1.0); bindScaledOrNull(q,":su",m.swapUsedMiB,1.0); bindScaledOrNull(q,":st",m.swapTotalMiB,1.0);
    bindScaledOrNull(q,":nrx",m.networkRxMiBs,1000.0); bindScaledOrNull(q,":ntx",m.networkTxMiBs,1000.0); bindScaledOrNull(q,":dr",m.diskReadMiBs,1000.0); bindScaledOrNull(q,":dw",m.diskWriteMiBs,1000.0);
    // Store root disk (first disk) in system_raw for backward compatibility
    const double rootUsed = m.disks.isEmpty() ? lmNaN() : m.disks[0].usedGiB;
    const double rootTotal = m.disks.isEmpty() ? lmNaN() : m.disks[0].totalGiB;
    bindScaledOrNull(q,":du",rootUsed,100.0); bindScaledOrNull(q,":dt",rootTotal,100.0); bindScaledOrNull(q,":bp",m.batteryPercent,10.0); bindScaledOrNull(q,":bw",m.batteryPowerW,100.0); bindScaledOrNull(q,":bh",m.batteryHealthPercent,10.0);
    if (m.batteryStatus.isEmpty()) q.bindValue(":bs", QVariant());
    else q.bindValue(":bs", m.batteryStatus);
    bindScaledOrNull(q,":pc",m.psiCpuSome,100.0); bindScaledOrNull(q,":pm",m.psiMemSome,100.0); bindScaledOrNull(q,":pi",m.psiIoSome,100.0);
    if (m.sensors.isEmpty() && m.fans.isEmpty()) q.bindValue(":sn", QVariant());
    else q.bindValue(":sn", sensorsToJson(m.sensors, m.fans));
    bindScaledOrNull(q,":nt",m.nvmeTemperatureC,10.0); bindScaledOrNull(q,":bt",m.batteryTemperatureC,10.0);
    if (!q.exec()) { db_.rollback(); if (error) *error = q.lastError().text(); return false; }

    q.prepare(R"(INSERT OR REPLACE INTO gpu_raw VALUES(:ts,:id,:vendor,:name,:driver,:state,:u,:mu,:mt,:temp,:power,:freq))");
    for (const auto &g : gpus) {
        q.bindValue(":ts",m.timestamp); q.bindValue(":id",g.id); q.bindValue(":vendor",g.vendor); q.bindValue(":name",g.name); q.bindValue(":driver",g.driver); q.bindValue(":state",g.state);
        bindScaledOrNull(q,":u",g.utilization,10.0); bindScaledOrNull(q,":mu",g.memoryUsedMiB,1.0); bindScaledOrNull(q,":mt",g.memoryTotalMiB,1.0); bindScaledOrNull(q,":temp",g.temperatureC,10.0); bindScaledOrNull(q,":power",g.powerW,100.0); bindScaledOrNull(q,":freq",g.frequencyMHz,1.0);
        if (!q.exec()) { db_.rollback(); if (error) *error = q.lastError().text(); return false; }
    }

    q.prepare("INSERT OR REPLACE INTO cpu_cores_raw VALUES(:ts,:core,:u)");
    for (int core = 0; core < m.cpuCores.size(); ++core) {
        if (!std::isfinite(m.cpuCores[core])) continue; // offline / no-delta core: nothing to store
        q.bindValue(":ts", m.timestamp); q.bindValue(":core", core);
        bindScaledOrNull(q, ":u", m.cpuCores[core], 10.0);
        if (!q.exec()) { db_.rollback(); if (error) *error = q.lastError().text(); return false; }
    }

    // Write per-disk data
    q.prepare("INSERT OR REPLACE INTO disks_raw VALUES(:ts,:mount,:total,:used)");
    for (const auto &d : m.disks) {
        q.bindValue(":ts", m.timestamp);
        q.bindValue(":mount", d.mountPoint);
        bindScaledOrNull(q, ":total", d.totalGiB, 100.0);
        bindScaledOrNull(q, ":used", d.usedGiB, 100.0);
        if (!q.exec()) { db_.rollback(); if (error) *error = q.lastError().text(); return false; }
    }

    // Persist the top-20 composite ranking (kind 'top'); folding by process
    // name happens in maintain().
    q.prepare("INSERT OR REPLACE INTO procs_raw VALUES(:ts,:kind,:rank,:name,:pid,:cpu,:rss)");
    for (int r = 0; r < m.topProcs.size(); ++r) {
        const ProcInfo &p = m.topProcs[r];
        q.bindValue(":ts", m.timestamp);
        q.bindValue(":kind", QLatin1String("top"));
        q.bindValue(":rank", r);
        q.bindValue(":name", p.name);
        q.bindValue(":pid", static_cast<qlonglong>(p.pid));
        bindScaledOrNull(q, ":cpu", p.cpuPercent, 10.0);
        bindScaledOrNull(q, ":rss", p.rssMiB, 1.0);
        if (!q.exec()) { db_.rollback(); if (error) *error = q.lastError().text(); return false; }
    }

    return db_.commit();
}

bool MetricsDatabase::maintain(qint64 now, const RetentionPolicy &policy, QString *error) {
    QSqlQuery q(db_);
    // Never fold the still-open 5-minute bucket; everything older is a
    // completed bucket. Folding is idempotent (INSERT OR REPLACE recomputes
    // the same bucket rows), so a missed run loses nothing. Raw columns are
    // already scaled integers, hence plain ROUND(AVG(col)).
    const qint64 openBucket = (now / 300) * 300;
    const QString aggSys5 = QString(R"(
      INSERT OR REPLACE INTO system_5m
      (ts,cpu_usage,cpu_temp,load1,mem_used,mem_total,swap_used,swap_total,net_rx,net_tx,disk_read,disk_write,disk_used,disk_total,battery_percent,battery_power,battery_health,battery_status,psi_cpu,psi_mem,psi_io,nvme_temp,bat_temp)
      SELECT (ts/300)*300, CAST(ROUND(AVG(cpu_usage)) AS INTEGER),CAST(ROUND(AVG(cpu_temp)) AS INTEGER),CAST(ROUND(AVG(load1)) AS INTEGER),CAST(ROUND(AVG(mem_used)) AS INTEGER),CAST(ROUND(AVG(mem_total)) AS INTEGER),CAST(ROUND(AVG(swap_used)) AS INTEGER),CAST(ROUND(AVG(swap_total)) AS INTEGER),
             CAST(ROUND(AVG(net_rx)) AS INTEGER),CAST(ROUND(AVG(net_tx)) AS INTEGER),CAST(ROUND(AVG(disk_read)) AS INTEGER),CAST(ROUND(AVG(disk_write)) AS INTEGER),CAST(ROUND(AVG(disk_used)) AS INTEGER),CAST(ROUND(AVG(disk_total)) AS INTEGER),CAST(ROUND(AVG(battery_percent)) AS INTEGER),CAST(ROUND(AVG(battery_power)) AS INTEGER),CAST(ROUND(AVG(battery_health)) AS INTEGER),MAX(battery_status),
             CAST(ROUND(AVG(psi_cpu)) AS INTEGER),CAST(ROUND(AVG(psi_mem)) AS INTEGER),CAST(ROUND(AVG(psi_io)) AS INTEGER),
             CAST(ROUND(AVG(nvme_temp)) AS INTEGER),CAST(ROUND(AVG(bat_temp)) AS INTEGER)
      FROM system_raw WHERE ts < %1 GROUP BY (ts/300))").arg(openBucket);
    const QString aggGpu5 = QString(R"(
      INSERT OR REPLACE INTO gpu_5m
      SELECT (ts/300)*300,id,MAX(vendor),MAX(name),MAX(driver),MAX(state),CAST(ROUND(AVG(util)) AS INTEGER),CAST(ROUND(AVG(mem_used)) AS INTEGER),CAST(ROUND(AVG(mem_total)) AS INTEGER),CAST(ROUND(AVG(temp)) AS INTEGER),CAST(ROUND(AVG(power)) AS INTEGER),CAST(ROUND(AVG(freq)) AS INTEGER)
      FROM gpu_raw WHERE ts < %1 GROUP BY (ts/300),id)").arg(openBucket);
    const QString aggCpu5 = QString(R"(
      INSERT OR REPLACE INTO cpu_cores_5m
      SELECT (ts/300)*300,core,CAST(ROUND(AVG(util)) AS INTEGER)
      FROM cpu_cores_raw WHERE ts < %1 GROUP BY (ts/300),core)").arg(openBucket);
    const QString aggDisks5 = QString(R"(
      INSERT OR REPLACE INTO disks_5m
      SELECT (ts/300)*300,mount,CAST(ROUND(AVG(total)) AS INTEGER),CAST(ROUND(AVG(used)) AS INTEGER)
      FROM disks_raw WHERE ts < %1 GROUP BY (ts/300),mount)").arg(openBucket);
    // Top-20 rankings fold by process name: average CPU share and peak RSS
    // survive per bucket, so long ranges can rank processes without keeping
    // every fine sample.
    const QString aggProcs5 = QString(R"(
      INSERT OR REPLACE INTO procs_5m
      SELECT (ts/300)*300,kind,name,CAST(ROUND(AVG(cpu)) AS INTEGER),CAST(ROUND(MAX(rss)) AS INTEGER)
      FROM procs_raw WHERE ts < %1 GROUP BY (ts/300),kind,name)").arg(openBucket);
    // Day rollover rule: keep at least `detailDays` calendar days of fine
    // data (today plus the preceding days), at minimum 6 days. Older detail
    // rows were folded into the archive above, so deleting them only clears
    // what is already compressed.
    const int detailDays = std::max(6, policy.detailDays);
    const qint64 dayStart = QDateTime(QDate::currentDate(), QTime(0, 0)).toSecsSinceEpoch();
    const qint64 detailCutoff = dayStart - static_cast<qint64>(detailDays - 1) * 86400;
    const qint64 archiveCutoff = now - static_cast<qint64>(std::max(30, policy.archiveDays)) * 86400;
    // Purge GPU rows that carry no signal at all (every metric NULL): e.g. a
    // nvidia-smi placeholder that never produced real data. Frequency-only
    // rows are kept — that is all a sysfs-only Intel iGPU can report, and the
    // GPU page must stay selectable for it (matches gpuHasSignal above).
    const QString purgeNoSignalGpu =
        "DELETE FROM gpu_raw WHERE util IS NULL AND temp IS NULL AND power IS NULL AND mem_used IS NULL AND freq IS NULL";
    const QString purgeNoSignalGpu5m =
        "DELETE FROM gpu_5m WHERE util IS NULL AND temp IS NULL AND power IS NULL AND mem_used IS NULL AND freq IS NULL";
    const QStringList stmts = {
        aggSys5, aggGpu5, aggCpu5, aggDisks5, aggProcs5,
        purgeNoSignalGpu, purgeNoSignalGpu5m,
        QString("DELETE FROM system_raw WHERE ts < %1").arg(detailCutoff),
        QString("DELETE FROM gpu_raw WHERE ts < %1").arg(detailCutoff),
        QString("DELETE FROM cpu_cores_raw WHERE ts < %1").arg(detailCutoff),
        QString("DELETE FROM disks_raw WHERE ts < %1").arg(detailCutoff),
        QString("DELETE FROM procs_raw WHERE ts < %1").arg(detailCutoff),
        QString("DELETE FROM system_5m WHERE ts < %1").arg(archiveCutoff),
        QString("DELETE FROM gpu_5m WHERE ts < %1").arg(archiveCutoff),
        QString("DELETE FROM cpu_cores_5m WHERE ts < %1").arg(archiveCutoff),
        QString("DELETE FROM disks_5m WHERE ts < %1").arg(archiveCutoff),
        QString("DELETE FROM procs_5m WHERE ts < %1").arg(archiveCutoff)
    };
    for (const auto &s : stmts) if (!q.exec(s)) { if (error) *error = q.lastError().text(); return false; }
    // Give space back to the filesystem at most once a day. A failed VACUUM
    // (e.g. a concurrent reader holds the database) is non-fatal and simply
    // retried on a later run.
    qint64 lastVacuum = 0;
    {
        QSqlQuery meta(db_);
        if (meta.exec("SELECT value FROM metadata WHERE key='last_vacuum'") && meta.next()) {
            lastVacuum = meta.value(0).toLongLong();
        }
    }
    if (now - lastVacuum >= 86400) {
        if (q.exec("VACUUM")) {
            QSqlQuery meta(db_);
            meta.prepare("INSERT OR REPLACE INTO metadata(key,value) VALUES('last_vacuum',:ts)");
            meta.bindValue(":ts", now);
            meta.exec();
        }
    }
    return true;
}


bool MetricsDatabase::hasTable(const QString &name) const {
    QSqlQuery q(db_);
    q.prepare("SELECT 1 FROM sqlite_master WHERE type='table' AND name=:name");
    q.bindValue(":name", name);
    return q.exec() && q.next();
}

QStringList MetricsDatabase::tableColumns(const QString &table) const {
    QStringList out;
    QSqlQuery q(db_);
    if (q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        while (q.next()) out << q.value(1).toString();
    }
    return out;
}

bool MetricsDatabase::migrate(QString *error) {
    QSqlQuery q(db_);
    if (!q.exec("INSERT OR IGNORE INTO metadata(key,value) VALUES('schema_version','2')")) {
        if (error) *error = q.lastError().text();
        return false;
    }
    if (!q.exec("UPDATE metadata SET value='2' WHERE key='schema_version' AND CAST(value AS INTEGER) < 2")) {
        if (error) *error = q.lastError().text();
        return false;
    }
    if (schemaVersion() < 3) {
        if (!migrateToV3(error)) { return false; }
    }
    if (schemaVersion() < 4) {
        if (!migrateToV4(error)) { return false; }
    }
    if (schemaVersion() < 6) {
        if (!migrateToV5(error)) { return false; }
    }
    return true;
}

// v2 -> v3: two tables per domain (raw + 5-minute archive) with scaled
// INTEGER metric columns. Legacy *_1m/*_10m data is folded into the new
// archive first (10-minute buckets are split, then finer 1-minute data
// overwrites the overlap), so no history is lost; old tables are dropped.
bool MetricsDatabase::migrateToV3(QString *error) {
    auto fail = [&](const QSqlQuery &query) {
        if (error) { *error = query.lastError().text(); }
        QSqlQuery rb(db_);
        rb.exec("ROLLBACK");
        QSqlQuery timeout(db_);
        timeout.exec("PRAGMA busy_timeout=3000");
        return false;
    };
    QSqlQuery q(db_);
    if (!q.exec("PRAGMA busy_timeout=30000")) { return fail(q); }
    if (!q.exec("BEGIN IMMEDIATE")) { return fail(q); }
    // Scale expressions mapping legacy unscaled REAL columns to v3 integers.
    const char *sysScale[17] = {
        "CAST(ROUND(cpu_usage*10) AS INTEGER)", "CAST(ROUND(cpu_temp*10) AS INTEGER)", "CAST(ROUND(load1*100) AS INTEGER)",
        "CAST(ROUND(mem_used) AS INTEGER)", "CAST(ROUND(mem_total) AS INTEGER)", "CAST(ROUND(swap_used) AS INTEGER)", "CAST(ROUND(swap_total) AS INTEGER)",
        "CAST(ROUND(net_rx*1000) AS INTEGER)", "CAST(ROUND(net_tx*1000) AS INTEGER)", "CAST(ROUND(disk_read*1000) AS INTEGER)", "CAST(ROUND(disk_write*1000) AS INTEGER)",
        "CAST(ROUND(disk_used*100) AS INTEGER)", "CAST(ROUND(disk_total*100) AS INTEGER)",
        "CAST(ROUND(battery_percent*10) AS INTEGER)", "CAST(ROUND(battery_power*100) AS INTEGER)", "CAST(ROUND(battery_health*10) AS INTEGER)",
        "battery_status"
    };
    const char *gpuScale[6] = {
        "CAST(ROUND(util*10) AS INTEGER)", "CAST(ROUND(mem_used) AS INTEGER)", "CAST(ROUND(mem_total) AS INTEGER)",
        "CAST(ROUND(temp*10) AS INTEGER)", "CAST(ROUND(power*100) AS INTEGER)", "CAST(ROUND(freq) AS INTEGER)"
    };
    const char *sysAvg[17] = {
        "CAST(ROUND(AVG(cpu_usage)*10) AS INTEGER)", "CAST(ROUND(AVG(cpu_temp)*10) AS INTEGER)", "CAST(ROUND(AVG(load1)*100) AS INTEGER)",
        "CAST(ROUND(AVG(mem_used)) AS INTEGER)", "CAST(ROUND(AVG(mem_total)) AS INTEGER)", "CAST(ROUND(AVG(swap_used)) AS INTEGER)", "CAST(ROUND(AVG(swap_total)) AS INTEGER)",
        "CAST(ROUND(AVG(net_rx)*1000) AS INTEGER)", "CAST(ROUND(AVG(net_tx)*1000) AS INTEGER)", "CAST(ROUND(AVG(disk_read)*1000) AS INTEGER)", "CAST(ROUND(AVG(disk_write)*1000) AS INTEGER)",
        "CAST(ROUND(AVG(disk_used)*100) AS INTEGER)", "CAST(ROUND(AVG(disk_total)*100) AS INTEGER)",
        "CAST(ROUND(AVG(battery_percent)*10) AS INTEGER)", "CAST(ROUND(AVG(battery_power)*100) AS INTEGER)", "CAST(ROUND(AVG(battery_health)*10) AS INTEGER)",
        "MAX(battery_status)"
    };
    const char *gpuAvg[6] = {
        "CAST(ROUND(AVG(util)*10) AS INTEGER)", "CAST(ROUND(AVG(mem_used)) AS INTEGER)", "CAST(ROUND(AVG(mem_total)) AS INTEGER)",
        "CAST(ROUND(AVG(temp)*10) AS INTEGER)", "CAST(ROUND(AVG(power)*100) AS INTEGER)", "CAST(ROUND(AVG(freq)) AS INTEGER)"
    };
    QString sysCols, gpuCols, sysAvgCols, gpuAvgCols;
    for (int i = 0; i < 17; ++i) { if (i > 0) { sysCols += ","; sysAvgCols += ","; } sysCols += QString::fromLatin1(sysScale[i]); sysAvgCols += QString::fromLatin1(sysAvg[i]); }
    for (int i = 0; i < 6; ++i) { if (i > 0) { gpuCols += ","; gpuAvgCols += ","; } gpuCols += QString::fromLatin1(gpuScale[i]); gpuAvgCols += QString::fromLatin1(gpuAvg[i]); }
    const bool haveSys1 = hasTable("system_1m");
    const bool haveGpu1 = hasTable("gpu_1m");
    const bool haveSys10 = hasTable("system_10m");
    const bool haveGpu10 = hasTable("gpu_10m");
    const QStringList stmts = {
        // 1. Rebuild raw tables with scaled INTEGER columns.
        "CREATE TABLE system_raw_new (ts INTEGER PRIMARY KEY, cpu_usage INTEGER, cpu_temp INTEGER, load1 INTEGER, mem_used INTEGER, mem_total INTEGER, swap_used INTEGER, swap_total INTEGER, net_rx INTEGER, net_tx INTEGER, disk_read INTEGER, disk_write INTEGER, disk_used INTEGER, disk_total INTEGER, battery_percent INTEGER, battery_power INTEGER, battery_health INTEGER, battery_status TEXT)",
        "INSERT INTO system_raw_new SELECT ts," + sysCols + " FROM system_raw",
        "DROP TABLE system_raw",
        "ALTER TABLE system_raw_new RENAME TO system_raw",
        "CREATE TABLE gpu_raw_new (ts INTEGER NOT NULL, id TEXT NOT NULL, vendor TEXT, name TEXT, driver TEXT, state TEXT, util INTEGER, mem_used INTEGER, mem_total INTEGER, temp INTEGER, power INTEGER, freq INTEGER, PRIMARY KEY(ts,id))",
        "INSERT INTO gpu_raw_new SELECT ts,id,vendor,name,driver,state," + gpuCols + " FROM gpu_raw",
        "DROP TABLE gpu_raw",
        "ALTER TABLE gpu_raw_new RENAME TO gpu_raw",
        // 2. Backfill the 5-minute archive: split legacy 10-minute buckets,
        // then let finer 1-minute data overwrite the overlap.
        haveSys10 ? "INSERT OR REPLACE INTO system_5m SELECT ts," + sysCols + " FROM system_10m" : "SELECT 1",
        haveSys10 ? "INSERT OR REPLACE INTO system_5m SELECT (ts+300)," + sysCols + " FROM system_10m" : "SELECT 1",
        haveGpu10 ? "INSERT OR REPLACE INTO gpu_5m SELECT ts,id,vendor,name,driver,state," + gpuCols + " FROM gpu_10m" : "SELECT 1",
        haveGpu10 ? "INSERT OR REPLACE INTO gpu_5m SELECT (ts+300),id,vendor,name,driver,state," + gpuCols + " FROM gpu_10m" : "SELECT 1",
        haveSys1 ? "INSERT OR REPLACE INTO system_5m SELECT (ts/300)*300," + sysAvgCols + " FROM system_1m GROUP BY (ts/300)" : "SELECT 1",
        haveGpu1 ? "INSERT OR REPLACE INTO gpu_5m SELECT (ts/300)*300,id,MAX(vendor),MAX(name),MAX(driver),MAX(state)," + gpuAvgCols + " FROM gpu_1m GROUP BY (ts/300),id" : "SELECT 1",
        // 3. Drop legacy tables, refresh indexes, stamp the version.
        "DROP TABLE IF EXISTS system_1m",
        "DROP TABLE IF EXISTS gpu_1m",
        "DROP TABLE IF EXISTS system_10m",
        "DROP TABLE IF EXISTS gpu_10m",
        "CREATE INDEX IF NOT EXISTS idx_gpu_raw_id_ts ON gpu_raw(id,ts)",
        "CREATE INDEX IF NOT EXISTS idx_gpu_5m_id_ts ON gpu_5m(id,ts)",
        "UPDATE metadata SET value='3' WHERE key='schema_version'"
    };
    for (const auto &s : stmts) {
        if (!q.exec(s)) { return fail(q); }
    }
    if (!q.exec("COMMIT")) { return fail(q); }
    q.exec("PRAGMA busy_timeout=3000");
    return true;
}

// v3 -> v4: per-core CPU domain (cpu_cores_raw + cpu_cores_5m). No legacy
// data exists for it, so the migration only creates the tables.
bool MetricsDatabase::migrateToV4(QString *error) {
    QSqlQuery q(db_);
    const QStringList stmts = {
        "CREATE TABLE IF NOT EXISTS cpu_cores_raw (ts INTEGER NOT NULL, core INTEGER NOT NULL, util INTEGER, PRIMARY KEY(ts,core))",
        "CREATE TABLE IF NOT EXISTS cpu_cores_5m (ts INTEGER NOT NULL, core INTEGER NOT NULL, util INTEGER, PRIMARY KEY(ts,core))",
        "CREATE INDEX IF NOT EXISTS idx_cpu_cores_raw_core_ts ON cpu_cores_raw(core,ts)",
        "CREATE INDEX IF NOT EXISTS idx_cpu_cores_5m_core_ts ON cpu_cores_5m(core,ts)",
        "UPDATE metadata SET value='4' WHERE key='schema_version'"
    };
    for (const auto &s : stmts) {
        if (!q.exec(s)) { if (error) *error = q.lastError().text(); return false; }
    }
    return true;
}

// v4 -> v6: PSI columns, a live hwmon sensor snapshot (system_raw only —
// JSON text, deliberately not aggregated) and named key-temperature columns
// (mean NVMe composite, battery). Version 6 covers every v5-cycle addition
// so an already-stamped v5 database is still upgraded in place.
bool MetricsDatabase::migrateToV5(QString *error) {
    QSqlQuery q(db_);
    const QStringList psiCols = {"psi_cpu", "psi_mem", "psi_io", "nvme_temp", "bat_temp"};
    for (const QString &table : {QStringLiteral("system_raw"), QStringLiteral("system_5m")}) {
        const auto cols = tableColumns(table);
        if (cols.isEmpty()) continue; // table not created yet; execSchema will include v5 columns
        for (const auto &c : psiCols) {
            if (cols.contains(c)) continue;
            if (!q.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 INTEGER").arg(table, c))) {
                if (error) *error = q.lastError().text();
                return false;
            }
        }
        if (table == QLatin1String("system_raw") && !cols.contains("sensors")) {
            if (!q.exec("ALTER TABLE system_raw ADD COLUMN sensors TEXT")) {
                if (error) *error = q.lastError().text();
                return false;
            }
        }
    }
    if (!q.exec("UPDATE metadata SET value='6' WHERE key='schema_version'")) {
        if (error) *error = q.lastError().text();
        return false;
    }
    return true;
}

int MetricsDatabase::schemaVersion() const {
    QSqlQuery q(db_);
    if (!q.exec("SELECT value FROM metadata WHERE key='schema_version'") || !q.next()) return 0;
    return q.value(0).toInt();
}

bool MetricsDatabase::integrityCheck(QString *error) const {
    QSqlQuery q(db_);
    if (!q.exec("PRAGMA quick_check") || !q.next()) {
        if (error) *error = q.lastError().text();
        return false;
    }
    const QString result = q.value(0).toString();
    if (result != "ok") {
        if (error) *error = result;
        return false;
    }
    return true;
}

bool MetricsDatabase::checkpoint(QString *error) {
    QSqlQuery q(db_);
    if (!q.exec("PRAGMA wal_checkpoint(PASSIVE)")) {
        if (error) *error = q.lastError().text();
        return false;
    }
    return true;
}

qint64 MetricsDatabase::earliestTimestamp() const {
    QSqlQuery q(db_);
    if (q.exec("SELECT MIN(ts) FROM system_raw") && q.next()) return q.value(0).toLongLong();
    return 0;
}

std::optional<SystemMetric> MetricsDatabase::latestSystem() const {
    QSqlQuery q(db_);
    if (!q.exec("SELECT * FROM system_raw ORDER BY ts DESC LIMIT 1") || !q.next()) return std::nullopt;
    SystemMetric m;
    m.timestamp=q.value(0).toLongLong(); m.cpuUsage=sqlScaled(q.value(1),10.0); m.cpuTemperatureC=sqlScaled(q.value(2),10.0); m.load1=sqlScaled(q.value(3),100.0);
    m.memoryUsedMiB=sqlScaled(q.value(4),1.0); m.memoryTotalMiB=sqlScaled(q.value(5),1.0); m.swapUsedMiB=sqlScaled(q.value(6),1.0); m.swapTotalMiB=sqlScaled(q.value(7),1.0);
    m.networkRxMiBs=sqlScaled(q.value(8),1000.0); m.networkTxMiBs=sqlScaled(q.value(9),1000.0); m.diskReadMiBs=sqlScaled(q.value(10),1000.0); m.diskWriteMiBs=sqlScaled(q.value(11),1000.0);
    m.batteryPercent=sqlScaled(q.value(14),10.0); m.batteryPowerW=sqlScaled(q.value(15),100.0); m.batteryHealthPercent=sqlScaled(q.value(16),10.0); m.batteryStatus=q.value(17).toString();
    m.psiCpuSome=sqlScaled(q.value(18),100.0); m.psiMemSome=sqlScaled(q.value(19),100.0); m.psiIoSome=sqlScaled(q.value(20),100.0);
    parseSensorsJson(q.value(21).toString(), m.sensors, m.fans);
    m.nvmeTemperatureC=sqlScaled(q.value(22),10.0); m.batteryTemperatureC=sqlScaled(q.value(23),10.0);
    // Load every mounted volume recorded at the latest disks_raw timestamp.
    // The GUI overview cards and the Disk page selector are both driven from
    // this list, so a LIMIT 1 here would leave only "/" visible in the UI.
    {
        QSqlQuery dq(db_);
        if (dq.exec("SELECT mount,total,used FROM disks_raw WHERE ts=(SELECT MAX(ts) FROM disks_raw) ORDER BY mount")) {
            while (dq.next()) {
                const QString mount = dq.value(0).toString();
                if (!isUserFacingMount(mount)) continue;
                DiskInfo d;
                d.mountPoint = mount;
                d.totalGiB = sqlScaled(dq.value(1), 100.0);
                d.usedGiB = sqlScaled(dq.value(2), 100.0);
                m.disks.push_back(d);
            }
        }
        // Root filesystem first so it leads the overview cards.
        std::stable_sort(m.disks.begin(), m.disks.end(), [](const DiskInfo &a, const DiskInfo &b) {
            return (a.mountPoint == "/") != (b.mountPoint == "/") && a.mountPoint == "/";
        });
    }
    {
        QSqlQuery cores(db_);
        if (cores.exec("SELECT core,util FROM cpu_cores_raw WHERE ts=(SELECT MAX(ts) FROM cpu_cores_raw) ORDER BY core")) {
            int maxCore = -1;
            QVector<QPair<int,double>> rows;
            while (cores.next()) {
                const int core = cores.value(0).toInt();
                if (core < 0 || core > 8192) continue;
                maxCore = std::max(maxCore, core);
                rows.push_back({core, sqlScaled(cores.value(1), 10.0)});
            }
            if (maxCore >= 0) {
                m.cpuCores.fill(lmNaN(), maxCore + 1);
                for (const auto &r : rows) { m.cpuCores[r.first] = r.second; }
            }
        }
    }
    return m;
}

QVector<GpuMetric> MetricsDatabase::latestGpus() const {
    QVector<GpuMetric> out;
    QSqlQuery q(db_);
    // Latest row per device: a transient miss for one GPU on a tick must not
    // hide it while its sibling still reports.
    if (!q.exec("SELECT id,vendor,name,driver,state,util,mem_used,mem_total,temp,power,freq FROM gpu_raw AS g WHERE ts=(SELECT MAX(ts) FROM gpu_raw WHERE id=g.id) ORDER BY vendor,name")) return out;
    while (q.next()) {
        GpuMetric g; g.id=q.value(0).toString(); g.vendor=q.value(1).toString(); g.name=q.value(2).toString(); g.driver=q.value(3).toString(); g.state=q.value(4).toString();
        g.utilization=sqlScaled(q.value(5),10.0); g.memoryUsedMiB=sqlScaled(q.value(6),1.0); g.memoryTotalMiB=sqlScaled(q.value(7),1.0); g.temperatureC=sqlScaled(q.value(8),10.0); g.powerW=sqlScaled(q.value(9),100.0); g.frequencyMHz=sqlScaled(q.value(10),1.0);
        if (!gpuHasSignal(g)) continue; // hide signal-less devices (e.g. an Intel iGPU with no readable sensors)
        out.push_back(g);
    }
    return out;
}

QVector<ProcInfo> MetricsDatabase::latestProcs() const {
    QVector<ProcInfo> out;
    QSqlQuery q(db_);
    if (!q.exec("SELECT rank,name,pid,cpu,rss FROM procs_raw WHERE ts=(SELECT MAX(ts) FROM procs_raw) AND kind='top' ORDER BY rank")) return out;
    while (q.next()) {
        ProcInfo p;
        p.name = q.value(1).toString();
        p.pid = q.value(2).toLongLong();
        p.cpuPercent = sqlScaled(q.value(3), 10.0);
        p.rssMiB = sqlScaled(q.value(4), 1.0);
        out.push_back(p);
    }
    return out;
}

QString MetricsDatabase::sourceTable(qint64 span) const {
    const auto c = AppConfig::load();
    return span <= static_cast<qint64>(std::max(6, c.detailRetentionDays)) * 86400 ? "system_raw" : "system_5m";
}
QString MetricsDatabase::gpuSourceTable(qint64 span) const {
    const auto c = AppConfig::load();
    return span <= static_cast<qint64>(std::max(6, c.detailRetentionDays)) * 86400 ? "gpu_raw" : "gpu_5m";
}
QString MetricsDatabase::cpuSourceTable(qint64 span) const {
    const auto c = AppConfig::load();
    return span <= static_cast<qint64>(std::max(6, c.detailRetentionDays)) * 86400 ? "cpu_cores_raw" : "cpu_cores_5m";
}

QVector<int> MetricsDatabase::cpuCoreIds() const {
    QVector<int> ids;
    QSqlQuery q(db_);
    if (q.exec("SELECT DISTINCT core FROM cpu_cores_raw UNION SELECT DISTINCT core FROM cpu_cores_5m ORDER BY core")) {
        while (q.next()) {
            const int core = q.value(0).toInt();
            if (core >= 0 && core <= 8192) { ids.push_back(core); }
        }
    }
    return ids;
}

QVector<CpuCoreSample> MetricsDatabase::cpuCoreHistory(qint64 from, qint64 to, int targetPoints) const {
    QVector<CpuCoreSample> out;
    const qint64 span = std::max<qint64>(1, to - from);
    const qint64 bucket = std::max<qint64>(1, span / std::max(100, targetPoints));
    const QString table = cpuSourceTable(span);
    QSqlQuery q(db_);
    q.prepare(QString("SELECT (ts/:bucket)*:bucket AS b,core,AVG(util) FROM %1 WHERE ts BETWEEN :from AND :to GROUP BY b,core ORDER BY b").arg(table));
    q.bindValue(":bucket", bucket); q.bindValue(":from", from); q.bindValue(":to", to);
    if (!q.exec()) return out;
    while (q.next()) {
        CpuCoreSample s;
        s.timestamp = q.value(0).toLongLong();
        s.core = q.value(1).toInt();
        s.utilization = sqlScaled(q.value(2), 10.0);
        out.push_back(s);
    }
    return out;
}

QVector<SystemMetric> MetricsDatabase::systemHistory(qint64 from, qint64 to, int targetPoints) const {
    QVector<SystemMetric> out;
    const qint64 span = std::max<qint64>(1, to-from);
    const qint64 bucket = std::max<qint64>(1, span / std::max(100,targetPoints));
    const QString table = sourceTable(span);
    QSqlQuery q(db_);
    q.prepare(QString(R"(SELECT (ts/:bucket)*:bucket AS b,AVG(cpu_usage),AVG(cpu_temp),AVG(load1),AVG(mem_used),AVG(mem_total),AVG(swap_used),AVG(swap_total),AVG(net_rx),AVG(net_tx),AVG(disk_read),AVG(disk_write),AVG(battery_percent),AVG(battery_power),AVG(battery_health),AVG(psi_cpu),AVG(psi_mem),AVG(psi_io),AVG(nvme_temp),AVG(bat_temp) FROM %1 WHERE ts BETWEEN :from AND :to GROUP BY b ORDER BY b)").arg(table));
    q.bindValue(":bucket",bucket); q.bindValue(":from",from); q.bindValue(":to",to);
    if (!q.exec()) return out;
    while(q.next()) { SystemMetric m; m.timestamp=q.value(0).toLongLong(); m.cpuUsage=sqlScaled(q.value(1),10.0); m.cpuTemperatureC=sqlScaled(q.value(2),10.0); m.load1=sqlScaled(q.value(3),100.0); m.memoryUsedMiB=sqlScaled(q.value(4),1.0); m.memoryTotalMiB=sqlScaled(q.value(5),1.0); m.swapUsedMiB=sqlScaled(q.value(6),1.0); m.swapTotalMiB=sqlScaled(q.value(7),1.0); m.networkRxMiBs=sqlScaled(q.value(8),1000.0); m.networkTxMiBs=sqlScaled(q.value(9),1000.0); m.diskReadMiBs=sqlScaled(q.value(10),1000.0); m.diskWriteMiBs=sqlScaled(q.value(11),1000.0); m.batteryPercent=sqlScaled(q.value(12),10.0); m.batteryPowerW=sqlScaled(q.value(13),100.0); m.batteryHealthPercent=sqlScaled(q.value(14),10.0); m.psiCpuSome=sqlScaled(q.value(15),100.0); m.psiMemSome=sqlScaled(q.value(16),100.0); m.psiIoSome=sqlScaled(q.value(17),100.0); m.nvmeTemperatureC=sqlScaled(q.value(18),10.0); m.batteryTemperatureC=sqlScaled(q.value(19),10.0); out.push_back(m); }
    return out;
}

QVector<SystemMetric> MetricsDatabase::gpuHistory(const QString &gpuId, qint64 from, qint64 to, int targetPoints) const {
    QVector<SystemMetric> out;
    const qint64 span = std::max<qint64>(1,to-from); const qint64 bucket=std::max<qint64>(1, span/std::max(100,targetPoints)); const QString table=gpuSourceTable(span);
    QSqlQuery q(db_); q.prepare(QString(R"(SELECT (ts/:bucket)*:bucket AS b,AVG(util),AVG(mem_used),AVG(mem_total),AVG(temp),AVG(power),AVG(freq) FROM %1 WHERE id=:id AND ts BETWEEN :from AND :to GROUP BY b ORDER BY b)").arg(table));
    q.bindValue(":bucket",bucket); q.bindValue(":id",gpuId); q.bindValue(":from",from); q.bindValue(":to",to); if(!q.exec()) return out;
    while(q.next()) { SystemMetric m; m.timestamp=q.value(0).toLongLong(); GpuMetric g; g.id=gpuId; g.utilization=sqlScaled(q.value(1),10.0); g.memoryUsedMiB=sqlScaled(q.value(2),1.0); g.memoryTotalMiB=sqlScaled(q.value(3),1.0); g.temperatureC=sqlScaled(q.value(4),10.0); g.powerW=sqlScaled(q.value(5),100.0); g.frequencyMHz=sqlScaled(q.value(6),1.0); m.gpus={g}; out.push_back(m); }
    return out;
}

QStringList MetricsDatabase::gpuIds(bool signalOnly) const {
    QStringList ids; QSqlQuery q(db_);
    const QString where = signalOnly
        ? " WHERE util IS NOT NULL OR temp IS NOT NULL OR power IS NOT NULL OR mem_used IS NOT NULL OR freq IS NOT NULL"
        : QString();
    if(q.exec(QString("SELECT DISTINCT id FROM gpu_raw%1 UNION SELECT DISTINCT id FROM gpu_5m%1 ORDER BY id").arg(where)))
        while(q.next()) ids<<q.value(0).toString();
    return ids; }
QString MetricsDatabase::gpuName(const QString &id) const {
    QSqlQuery q(db_);
    const char *tables[] = {"gpu_raw", "gpu_5m"};
    for (const char *t : tables) {
        q.prepare(QString("SELECT name FROM %1 WHERE id=:id ORDER BY ts DESC LIMIT 1").arg(QString::fromLatin1(t)));
        q.bindValue(":id", id);
        if (q.exec() && q.next()) {
            const QString name = q.value(0).toString();
            if (!name.isEmpty()) { return name; }
        }
    }
    return id;
}

QStringList MetricsDatabase::diskMountPoints() const {
    QStringList mounts;
    QSqlQuery q(db_);
    if (q.exec("SELECT DISTINCT mount FROM disks_raw UNION SELECT DISTINCT mount FROM disks_5m ORDER BY mount")) {
        while (q.next()) {
            const QString m = q.value(0).toString();
            if (isUserFacingMount(m)) mounts << m;
        }
    }
    return mounts;
}

QVector<DiskInfo> MetricsDatabase::diskHistory(const QString &mountPoint, qint64 from, qint64 to, int targetPoints) const {
    QVector<DiskInfo> out;
    const qint64 span = std::max<qint64>(1, to - from);
    const qint64 bucket = std::max<qint64>(1, span / std::max(100, targetPoints));
    const auto c = AppConfig::load();
    const QString table = span <= static_cast<qint64>(std::max(6, c.detailRetentionDays)) * 86400 ? "disks_raw" : "disks_5m";
    QSqlQuery q(db_);
    q.prepare(QString(R"(SELECT (ts/:bucket)*:bucket AS b, AVG(total), AVG(used) FROM %1 WHERE mount=:mount AND ts BETWEEN :from AND :to GROUP BY b ORDER BY b)").arg(table));
    q.bindValue(":bucket", bucket);
    q.bindValue(":mount", mountPoint);
    q.bindValue(":from", from);
    q.bindValue(":to", to);
    if (!q.exec()) return out;
    while (q.next()) {
        DiskInfo d;
        d.timestamp = q.value(0).toLongLong();
        d.mountPoint = mountPoint;
        d.totalGiB = sqlScaled(q.value(1), 100.0);
        d.usedGiB = sqlScaled(q.value(2), 100.0);
        out.push_back(d);
    }
    return out;
}
