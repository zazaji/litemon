#pragma once

#include "model.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVector>
#include <optional>

struct RetentionPolicy {
    int detailDays = 7;
    int archiveDays = 365;
};

class MetricsDatabase {
public:
    explicit MetricsDatabase(const QString &path, const QString &connectionName = {});
    ~MetricsDatabase();
    bool open(QString *error = nullptr);
    bool insert(const SystemMetric &m, QString *error = nullptr);
    bool maintain(qint64 now, const RetentionPolicy &policy = {}, QString *error = nullptr);
    bool maintain(qint64 now, QString *error) { return maintain(now, RetentionPolicy{}, error); }
    std::optional<SystemMetric> latestSystem() const;
    QVector<GpuMetric> latestGpus() const;
    // Latest top-20 rankings: {cpu-ranked, mem-ranked}.
    QVector<ProcInfo> latestProcs() const;
    QVector<SystemMetric> systemHistory(qint64 from, qint64 to, int targetPoints = 900) const;
    QVector<BandPoint> systemHistoryBands(qint64 from, qint64 to, int numBands = 100) const;
    QVector<SystemMetric> gpuHistory(const QString &gpuId, qint64 from, qint64 to, int targetPoints = 900) const;
    QVector<int> cpuCoreIds() const;
    QVector<CpuCoreSample> cpuCoreHistory(qint64 from, qint64 to, int targetPoints = 900) const;
    QStringList gpuIds(bool signalOnly = false) const;
    QString gpuName(const QString &id) const;
    QStringList diskMountPoints() const;
    QVector<DiskInfo> diskHistory(const QString &mountPoint, qint64 from, qint64 to, int targetPoints = 900) const;
    QString path() const { return path_; }
    int schemaVersion() const;
    bool integrityCheck(QString *error = nullptr) const;
    bool checkpoint(QString *error = nullptr);
    qint64 earliestTimestamp() const;

private:
    QString sourceTable(qint64 span) const;
    QString gpuSourceTable(qint64 span) const;
    QString cpuSourceTable(qint64 span) const;
    bool execSchema(QString *error);
    bool migrate(QString *error);
    bool migrateToV3(QString *error);
    bool migrateToV4(QString *error);
    bool migrateToV5(QString *error);
    bool hasTable(const QString &name) const;
    QStringList tableColumns(const QString &table) const;
    static void bindScaledOrNull(QSqlQuery &q, const QString &name, double value, double scale);

    QString path_;
    QString connectionName_;
    mutable QSqlDatabase db_;
};
