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

// Combined battery status ("Charging", "Discharging", ... joined with " + ")
// across all /sys/class/power_supply batteries; empty when none exists.
QString readBatteryStatus();

// SwapTotal in MiB from /proc/meminfo; NaN when unavailable.
double readSwapTotalMiB();

// MemTotal in MiB from /proc/meminfo; NaN when unavailable.
double readMemTotalMiB();

// MemAvailable in MiB from /proc/meminfo; NaN when unavailable.
double readMemAvailableMiB();

// OOM kill events from the systemd journal (kernel oom-killer plus
// systemd-oomd), newest first. days: how far back to scan. Returns {}
// when the journal is unreadable or has no matching entries.
QVector<OomInfo> readOomEvents(int days);

// Disk mount grouping over the content of /proc/mounts: one entry per
// backing device. btrfs subvolumes of one partition (/, /home, /swap ...)
// report identical usage numbers, so they collapse into a single entry
// keyed on the primary mount point, with the top-level siblings kept as
// display-only extras. Bind/nested aliases of an already-listed mount are
// hidden entirely (user-confirmed dedup rule).
struct DiskMountGroup {
    QString primary;     // the mount point history keys and tray slots use
    QStringList extras;  // sibling mounts sharing the same capacity
};
// Pure parser over a /proc/mounts payload; empty content yields {} (which is
// also what happens on non-Linux, letting callers fall back to their own
// platform listing).
QVector<DiskMountGroup> parseDiskMountGroups(const QString &content);

// Mount points that should never appear as user-facing disk entries. Shared
// by the collector parser, the GUI and the database reader so all three
// agree on the filter.
bool isUserFacingMount(const QString &mount);

// Parsers (unit-tested), one journalctl "-o short-unix" line each:
//   kernel: "<epoch> <host> kernel: ... Out of memory: Killed process
//           <pid> (<comm>) ... anon-rss:<n>kB ... UID <uid>"
std::optional<OomInfo> parseOomKernelLine(const QString &line);
//   oomd:   "<epoch> <host> systemd-oomd[<pid>]: Killed <cgroup path> due to
//           memory pressure for <path>"
std::optional<OomInfo> parseOomOomdLine(const QString &line);
}
