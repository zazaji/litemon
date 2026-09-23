#include "collector.h"
#include "linuxutils.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTextStream>
#include <algorithm>
#include <cmath>

#include <QElapsedTimer>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <unistd.h>

namespace {
// Cached ABI constants for per-process CPU share (clock ticks per second,
// page size in MiB). sysconf is not declared by mingw's unistd.h, but these
// values are only consumed by the /proc-based sampling paths, which read
// nothing on Windows.
#if defined(_SC_CLK_TCK) && defined(_SC_PAGESIZE)
const double kClkTicks = [] { const long v = sysconf(_SC_CLK_TCK); return v > 0 ? static_cast<double>(v) : 100.0; }();
const double kPageMiB = [] { const long v = sysconf(_SC_PAGESIZE); return v > 0 ? static_cast<double>(v) / 1048576.0 : 4.0 / 1024.0; }();
#else
const double kClkTicks = 100.0;
const double kPageMiB = 4.0 / 1024.0;
#endif
constexpr int kTopN = 20;
}

using namespace LinuxUtils;

SystemCollector::SystemCollector() {
    lastCpu_ = readCpuTicks();
    lastCores_ = readCpuCoreTicks();
    lastNetwork_ = readNetworkCounters();
    lastDisk_ = readDiskCounters();
    rateTimer_.start();
}

SystemCollector::CpuTicks SystemCollector::readCpuTicks() const {
    QFile f("/proc/stat");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QString line = QString::fromUtf8(f.readLine()).trimmed();
    const auto p = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (p.size() < 5 || p[0] != "cpu") return {};
    quint64 total = 0;
    QVector<quint64> v;
    for (int i = 1; i < p.size(); ++i) {
        bool ok = false; const quint64 x = p[i].toULongLong(&ok); if (!ok) break;
        v.push_back(x);
        if (i <= 8) total += x; // user..steal; guest fields are already included in user/nice
    }
    if (v.size() < 4) return {};
    const quint64 idle = v[3] + (v.size() > 4 ? v[4] : 0);
    return {total, idle, true};
}

QMap<int, SystemCollector::CpuTicks> SystemCollector::readCpuCoreTicks() const {
    QMap<int, CpuTicks> out;
    QFile f("/proc/stat");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    // NOTE: /proc files report size 0, so QFile::atEnd() is true immediately.
    // Read via readAll() and split instead of an atEnd()/readLine() loop.
    const QString content = QString::fromUtf8(f.readAll());
    const auto lines = content.split('\n');
    static const QRegularExpression coreRe(QStringLiteral("^cpu([0-9]+)$"));
    static const QRegularExpression wsRe(QStringLiteral("\\s+"));
    for (const auto &line : lines) {
        const auto p = line.trimmed().split(wsRe, Qt::SkipEmptyParts);
        if (p.isEmpty()) continue;
        const auto coreMatch = coreRe.match(p[0]);
        if (!coreMatch.hasMatch() || p.size() < 5) continue;
        bool okId = false;
        const int core = coreMatch.captured(1).toInt(&okId);
        if (!okId || core < 0 || core > 8192) continue;
        quint64 total = 0;
        QVector<quint64> v;
        for (int i = 1; i < p.size(); ++i) {
            bool ok = false; const quint64 x = p[i].toULongLong(&ok); if (!ok) break;
            v.push_back(x);
            if (i <= 8) total += x; // user..steal; guest fields are already included in user/nice
        }
        if (v.size() < 4) continue;
        const quint64 idle = v[3] + (v.size() > 4 ? v[4] : 0);
        out.insert(core, {total, idle, true});
    }
    return out;
}

static double cpuUsageBetween(quint64 total, quint64 idle, quint64 lastTotal, quint64 lastIdle) {
    if (total <= lastTotal) return lmNaN();
    const quint64 dt = total - lastTotal;
    const quint64 di = idle >= lastIdle ? idle - lastIdle : 0;
    return std::clamp((1.0 - static_cast<double>(di) / static_cast<double>(dt)) * 100.0, 0.0, 100.0);
}

void SystemCollector::collectMemory(SystemMetric &m) const {
    QFile f("/proc/meminfo");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QHash<QString, double> kb;
    // NOTE: /proc files report size 0, so QFile::atEnd() is true immediately.
    // Read via readAll() and split instead of an atEnd()/readLine() loop.
    const QString content = QString::fromUtf8(f.readAll());
    const auto lines = content.split('\n');
    for (const auto &line : lines) {
        const qsizetype colon = line.indexOf(':');
        if (colon < 0) continue;
        const QString key = line.left(colon);
        bool ok = false;
        const double val = line.mid(colon + 1).trimmed().section(' ', 0, 0).toDouble(&ok);
        if (ok) kb[key] = val;
    }
    const double total = kb.value("MemTotal", lmNaN());
    const double available = kb.value("MemAvailable", lmNaN());
    if (std::isfinite(total)) m.memoryTotalMiB = total / 1024.0;
    if (std::isfinite(total) && std::isfinite(available)) m.memoryUsedMiB = (total - available) / 1024.0;
    const double swTotal = kb.value("SwapTotal", lmNaN());
    const double swFree = kb.value("SwapFree", lmNaN());
    if (std::isfinite(swTotal)) m.swapTotalMiB = swTotal / 1024.0;
    if (std::isfinite(swTotal) && std::isfinite(swFree)) m.swapUsedMiB = (swTotal - swFree) / 1024.0;
}

void SystemCollector::collectLoad(SystemMetric &m) const {
    const auto s = readText("/proc/loadavg");
    if (!s.isEmpty()) m.load1 = parseNumber(s.section(' ', 0, 0));
}

void SystemCollector::collectTemperature(SystemMetric &m) const {
    QDir root("/sys/class/hwmon");
    double best = lmNaN();
    int bestScore = -1;
    for (const auto &h : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir d(root.filePath(h));
        const QString chip = readText(d.filePath("name")).toLower();
        const auto files = d.entryList({"temp*_input"}, QDir::Files);
        for (const auto &f : files) {
            if (auto val = readDouble(d.filePath(f))) {
                const double c = *val / 1000.0;
                if (c <= 0 || c >= 150) continue;
                const QString stem = f.left(f.indexOf('_'));
                const QString label = readText(d.filePath(stem + "_label")).toLower();
                int score = 0;
                if (chip.contains("coretemp") || chip.contains("k10temp") || chip.contains("zenpower")) score += 3;
                if (chip.contains("acpitz")) score += 1;
                if (label.contains("package") || label.contains("tctl") || label.contains("tdie")) score += 4;
                if (label.contains("cpu") || label.contains("soc")) score += 2;
                if (score > 0 && (score > bestScore || (score == bestScore && (!std::isfinite(best) || c > best)))) {
                    bestScore = score; best = c;
                }
            }
        }
    }
    if (!std::isfinite(best)) {
        QDir thermal("/sys/class/thermal");
        for (const auto &z : thermal.entryList({"thermal_zone*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString base = thermal.filePath(z);
            const QString type = readText(base + "/type").toLower();
            if (!type.contains("cpu") && !type.contains("x86_pkg") && !type.contains("soc") && !type.contains("acpitz")) continue;
            if (auto v = readDouble(base + "/temp")) {
                const double c = *v / 1000.0;
                if (c > 0 && c < 150) { best = c; break; }
            }
        }
    }
    m.cpuTemperatureC = best;
}

void SystemCollector::collectBattery(SystemMetric &m) const {
    QDir ps("/sys/class/power_supply");
    double energyNow = 0, energyFull = 0, energyDesign = 0, power = 0;
    double pctSum = 0; int pctCount = 0; bool any = false;
    double tempSum = 0; int tempCount = 0;
    QStringList statuses;
    for (const auto &e : ps.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString base = ps.filePath(e);
        if (readText(base + "/type").compare("Battery", Qt::CaseInsensitive) != 0) continue;
        any = true;
        statuses << readText(base + "/status");
        if (auto c = readDouble(base + "/capacity")) { pctSum += *c; ++pctCount; }

        auto readEnergy = [&](const QString &energyFile, const QString &chargeFile) -> double {
            if (auto v = readDouble(base + "/" + energyFile)) return *v;
            const auto charge = readDouble(base + "/" + chargeFile);
            const auto voltage = readDouble(base + "/voltage_now");
            if (charge && voltage) return (*charge) * (*voltage) / 1e6; // uAh*uV -> uWh
            return 0.0;
        };
        energyNow += readEnergy("energy_now", "charge_now");
        energyFull += readEnergy("energy_full", "charge_full");
        energyDesign += readEnergy("energy_full_design", "charge_full_design");
        if (auto p = readDouble(base + "/power_now")) power += *p / 1e6;
        else {
            const auto cur = readDouble(base + "/current_now");
            const auto volt = readDouble(base + "/voltage_now");
            if (cur && volt) power += (*cur) * (*volt) / 1e12;
        }
        // power_supply ABI: temp_input is tenths of a degree Celsius.
        if (auto t = readDouble(base + "/temp_input")) { tempSum += *t / 10.0; ++tempCount; }
    }
    if (!any) return;
    if (energyFull > 0 && energyNow >= 0) m.batteryPercent = std::clamp(energyNow / energyFull * 100.0, 0.0, 100.0);
    else if (pctCount) m.batteryPercent = pctSum / pctCount;
    if (energyDesign > 0 && energyFull > 0) m.batteryHealthPercent = std::clamp(energyFull / energyDesign * 100.0, 0.0, 150.0);
    const bool charging = std::any_of(statuses.cbegin(), statuses.cend(), [](const QString &s){ return s.compare("Charging", Qt::CaseInsensitive) == 0; });
    const bool discharging = std::any_of(statuses.cbegin(), statuses.cend(), [](const QString &s){ return s.compare("Discharging", Qt::CaseInsensitive) == 0; });
    m.batteryPowerW = (discharging && !charging) ? -power : power;
    if (tempCount > 0) m.batteryTemperatureC = tempSum / tempCount;
    m.batteryStatus = statuses.join(" + ");
}

// For dedup: prefer "/" over any other mount point of the same device,
// then the shortest path (bind-mount aliases like /home or /var/tmp of the
// root filesystem would repeat identical numbers in the UI).
static bool preferMount(const QString &a, const QString &b) {
    if (a == "/") return b != "/";
    if (b == "/") return false;
    if (a.size() != b.size()) return a.size() < b.size();
    return a < b;
}

void SystemCollector::collectDisks(SystemMetric &m) const {
    // Parse /proc/mounts directly instead of QStorageInfo::mountedVolumes()
    // because the latter calls stat() on every mount point, which blocks on
    // unreachable network mounts (CIFS/NFS).
    const QStringList skipFs = {"tmpfs", "devtmpfs", "sysfs", "proc", "devpts", "cgroup", "cgroup2", "pstore", "securityfs", "debugfs", "tracefs", "fusectl", "configfs", "hugetlbfs", "mqueue", "binfmt_misc", "autofs", "rpc_pipefs", "nfsd", "overlay", "efivarfs"};
    // Network filesystems block on statfs for as long as their server is
    // unreachable (CIFS hard mounts: forever), so they never get a
    // QStorageInfo. fuseblk is kept — that is how ntfs-3g local disks show up.
    const QStringList networkFs = {"cifs", "smbfs", "smb2", "nfs", "nfs4", "ceph", "afs", "ncpfs", "sshfs", "fuse.sshfs", "fuse.gvfsd-fuse", "fuse.portal", "fuse.lxcfs", "9p", "virtiofs"};
    QHash<QString, DiskInfo> byDevice;
    QFile f("/proc/mounts");
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString content = QString::fromUtf8(f.readAll());
        const auto lines = content.split('\n');
        for (const auto &line : lines) {
            const auto fields = line.split(' ');
            if (fields.size() < 3) continue;
            const QString mount = fields[1];
            const QString fsType = fields[2];
            if (skipFs.contains(fsType, Qt::CaseInsensitive)) continue;
            if (networkFs.contains(fsType, Qt::CaseInsensitive)) continue;
            if (fsType.startsWith("fuse.", Qt::CaseInsensitive) && fsType.compare("fuseblk", Qt::CaseInsensitive) != 0) continue;
            if (mount.startsWith("//")) continue; // CIFS device naming: //server/share
            if (mount.startsWith("/boot/efi") || mount.startsWith("/sys/") || mount.startsWith("/var/lib/waydroid")) continue;
            // Use QStorageInfo only for the specific mount, not all volumes
            QStorageInfo vol(mount);
            if (!vol.isValid() || !vol.isReady()) continue;
            DiskInfo di;
            di.mountPoint = mount;
            di.totalGiB = static_cast<double>(vol.bytesTotal()) / 1073741824.0;
            di.usedGiB = static_cast<double>(vol.bytesTotal() - vol.bytesAvailable()) / 1073741824.0;
            if (di.totalGiB <= 0) continue;
            const QString dev = QString::fromLocal8Bit(vol.device());
            const auto it = byDevice.find(dev);
            if (it == byDevice.end()) { byDevice.insert(dev, di); }
            else if (preferMount(mount, it->mountPoint)) { it->mountPoint = mount; }
        }
    }
    m.disks.clear();
    for (const auto &d : byDevice) { m.disks.append(d); }
    // Fallback when /proc/mounts was unreadable: at least show root.
    if (m.disks.isEmpty()) {
        QStorageInfo root = QStorageInfo::root();
        if (root.isValid() && root.isReady()) {
            DiskInfo di;
            di.mountPoint = "/";
            di.totalGiB = static_cast<double>(root.bytesTotal()) / 1073741824.0;
            di.usedGiB = static_cast<double>(root.bytesTotal() - root.bytesAvailable()) / 1073741824.0;
            if (di.totalGiB > 0) m.disks.append(di);
        }
    }
    std::stable_sort(m.disks.begin(), m.disks.end(), [](const DiskInfo &a, const DiskInfo &b) {
        if ((a.mountPoint == "/") != (b.mountPoint == "/")) { return a.mountPoint == "/"; }
        return a.mountPoint < b.mountPoint;
    });
}

SystemCollector::IoCounters SystemCollector::readNetworkCounters() const {
    QFile f("/proc/net/dev");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    quint64 rx = 0, tx = 0;
    // NOTE: /proc files report size 0, so atEnd() cannot drive the loop.
    const QString content = QString::fromUtf8(f.readAll());
    const auto lines = content.split('\n');
    for (const auto &line : lines) {
        const qsizetype colon = line.indexOf(':');
        if (colon < 0) continue;
        const QString iface = line.left(colon).trimmed();
        if (iface == "lo") continue;
        const QFileInfo fi("/sys/class/net/" + iface);
        const QString real = fi.canonicalFilePath();
        if (real.contains("/devices/virtual/net/")) continue; // avoid VPN/bridge double counting
        const auto fields = line.mid(colon + 1).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (fields.size() < 16) continue;
        bool ok1=false, ok2=false;
        const quint64 r = fields[0].toULongLong(&ok1);
        const quint64 t = fields[8].toULongLong(&ok2);
        if (ok1) { rx += r; }
        if (ok2) { tx += t; }
    }
    return {rx, tx, true};
}

QVector<SystemCollector::DiskIoCounters> SystemCollector::readDiskCounters() const {
    QFile f("/proc/diskstats");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QVector<DiskIoCounters> result;
    // NOTE: /proc files report size 0, so atEnd() cannot drive the loop.
    const QString content = QString::fromUtf8(f.readAll());
    const auto lines = content.split('\n');
    for (const auto &lineStr : lines) {
        const auto fields = lineStr.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (fields.size() < 10) continue;
        const QString dev = fields[2];
        if (dev.startsWith("loop") || dev.startsWith("ram") || dev.startsWith("dm-") || dev.startsWith("md")) continue;
        if (!QFileInfo::exists("/sys/block/" + dev)) continue; // whole physical block devices only
        bool ok1=false, ok2=false;
        const quint64 r = fields[5].toULongLong(&ok1);
        const quint64 w = fields[9].toULongLong(&ok2);
        if (ok1 && ok2) {
            result.append({dev, r, w});
        }
    }
    return result;
}

SystemMetric SystemCollector::collect(bool includeGpu) {
    SystemMetric m;
    m.timestamp = QDateTime::currentSecsSinceEpoch();

    const auto cpu = readCpuTicks();
    if (cpu.valid && lastCpu_.valid) {
        m.cpuUsage = cpuUsageBetween(cpu.total, cpu.idle, lastCpu_.total, lastCpu_.idle);
    }
    lastCpu_ = cpu;

    const auto cores = readCpuCoreTicks();
    if (!cores.isEmpty()) {
        int maxCore = cores.lastKey();
        m.cpuCores.fill(lmNaN(), maxCore + 1);
        for (auto it = cores.constBegin(); it != cores.constEnd(); ++it) {
            const auto last = lastCores_.value(it.key());
            m.cpuCores[it.key()] = (it.value().valid && last.valid)
                ? cpuUsageBetween(it.value().total, it.value().idle, last.total, last.idle)
                : lmNaN();
        }
    }
    lastCores_ = cores;

    const qint64 elapsedMs = std::max<qint64>(1, rateTimer_.elapsed());
    const double seconds = static_cast<double>(elapsedMs) / 1000.0;
    const auto net = readNetworkCounters();
    if (net.valid && lastNetwork_.valid && net.a >= lastNetwork_.a && net.b >= lastNetwork_.b) {
        m.networkRxMiBs = static_cast<double>(net.a - lastNetwork_.a) / 1048576.0 / seconds;
        m.networkTxMiBs = static_cast<double>(net.b - lastNetwork_.b) / 1048576.0 / seconds;
    }
    lastNetwork_ = net;

    // Per-disk I/O: compute total from all physical block devices
    const auto diskIo = readDiskCounters();
    quint64 totalRead = 0, totalWrite = 0;
    for (const auto &d : diskIo) {
        totalRead += d.readSectors;
        totalWrite += d.writeSectors;
    }
    if (lastDisk_.size() > 0) {
        quint64 lastRead = 0, lastWrite = 0;
        for (const auto &d : lastDisk_) {
            lastRead += d.readSectors;
            lastWrite += d.writeSectors;
        }
        if (totalRead >= lastRead && totalWrite >= lastWrite) {
            m.diskReadMiBs = static_cast<double>(totalRead - lastRead) * 512.0 / 1048576.0 / seconds;
            m.diskWriteMiBs = static_cast<double>(totalWrite - lastWrite) * 512.0 / 1048576.0 / seconds;
        }
    }
    lastDisk_ = diskIo;
    rateTimer_.restart();

    collectMemory(m);
    collectLoad(m);
    collectTemperature(m);
    collectBattery(m);
    collectPsi(m);
    collectSensors(m);
    collectProcesses(m);
    collectDisks(m);
    if (includeGpu) m.gpus = gpuCollector_.collect();
    return m;
}

void SystemCollector::collectPsi(SystemMetric &m) const {
    const auto cpu = parsePsiContent(readText("/proc/pressure/cpu"));
    const auto mem = parsePsiContent(readText("/proc/pressure/memory"));
    const auto io = parsePsiContent(readText("/proc/pressure/io"));
    if (std::isfinite(cpu.some)) m.psiCpuSome = cpu.some;
    if (std::isfinite(mem.some)) m.psiMemSome = mem.some;
    if (std::isfinite(io.some)) m.psiIoSome = io.some;
}

void SystemCollector::collectSensors(SystemMetric &m) const {
    QVector<FanInfo> fans;
    m.sensors = readHwmonSensors(&fans);
    m.fans = fans;
    // Named key temperature: mean across nvme hwmon channels, persisted as
    // its own column so hard-drive temperature charts cover long ranges.
    double sum = 0; int n = 0;
    for (const auto &s : m.sensors) {
        if (s.chip.compare("nvme", Qt::CaseInsensitive) == 0) { sum += s.tempC; ++n; }
    }
    if (n > 0) m.nvmeTemperatureC = sum / n;
}

void SystemCollector::collectProcesses(SystemMetric &m) const {
    // Differential /proc scan: CPU share needs the previous sample's tick
    // counts, exactly like the rate counters above.
    QDir proc("/proc");
    proc.setNameFilters({"[0-9]*"});
    proc.setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const double dt = lastProcSampleMs_ > 0 ? static_cast<double>(nowMs - lastProcSampleMs_) / 1000.0 : 0.0;
    QHash<qint64, quint64> cur;
    QVector<ProcInfo> procs;
    procs.reserve(proc.count());
    for (const auto &e : proc.entryList()) {
        const auto s = parseProcStat(readText("/proc/" + e + "/stat"));
        if (!s) continue; // process vanished between listing and read
        cur[s->pid] = s->cpuTicks;
        ProcInfo p;
        p.pid = s->pid;
        p.name = s->name;
        p.state = s->state;
        p.threads = s->threads;
        p.rssMiB = static_cast<double>(s->rssPages) * kPageMiB;
        if (dt > 0) {
            const auto prev = lastProcTicks_.constFind(s->pid);
            if (prev != lastProcTicks_.constEnd() && s->cpuTicks >= *prev) {
                p.cpuPercent = static_cast<double>(s->cpuTicks - *prev) / kClkTicks / dt * 100.0;
            }
        }
        procs.push_back(p);
    }
    lastProcTicks_ = std::move(cur);
    lastProcSampleMs_ = nowMs;

    // Top-20 by composite score (CPU%+2)×(10MB+RSS MB): memory-heavy users
    // lead even at idle CPU. The first sample has no deltas, so scores
    // collapse to RSS ranking there and stay visible from the first report.
    QVector<ProcInfo> ranked = procs;
    const auto score = [](const ProcInfo &p) {
        const double cpu = std::isfinite(p.cpuPercent) ? p.cpuPercent : 0.0;
        const double rss = std::isfinite(p.rssMiB) ? p.rssMiB : 0.0;
        return (cpu + 2.0) * (10.0 + rss);
    };
    std::stable_sort(ranked.begin(), ranked.end(), [&score](const ProcInfo &a, const ProcInfo &b) {
        return score(a) > score(b);
    });
    m.topProcs = ranked.mid(0, kTopN);
}
