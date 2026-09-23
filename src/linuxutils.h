#pragma once

#include "model.h"

#include <QString>
#include <QStringList>
#include <QByteArray>
#include <optional>

namespace LinuxUtils {
QString readText(const QString &path);
std::optional<qint64> readInt64(const QString &path);
std::optional<double> readDouble(const QString &path);
QString commandPath(const QString &name);
QByteArray runCommand(const QString &program, const QStringList &args, int timeoutMs, int *exitCode = nullptr);
double parseNumber(const QString &text);
QString humanRate(double mibPerSec);
QString humanBytesMiB(double mib);

// One /proc/pressure/<resource> file: "some"/"full" avg10 values (0-100).
// full is NaN on the cpu resource (kernel exposes only "some" there).
struct PsiValues { double some = lmNaN(); double full = lmNaN(); };
PsiValues parsePsiContent(const QString &content);

// One /proc/<pid>/stat record. cpuTicks is utime+stime in clock ticks.
struct ProcSample {
    qint64 pid = 0;
    QString name;
    QString state;
    quint64 cpuTicks = 0;
    qint64 rssPages = 0;
    qint64 threads = 0;
};
std::optional<ProcSample> parseProcStat(const QString &content);

// VmSwap in kB from a /proc/<pid>/status payload; nullopt when the process
// has no VmSwap line (never swapped).
std::optional<qint64> parseProcStatusSwap(const QString &content);

// Live hwmon enumeration (/sys/class/hwmon): temperatures and fan speeds.
QVector<SensorInfo> readHwmonSensors(QVector<FanInfo> *fans = nullptr);
}
