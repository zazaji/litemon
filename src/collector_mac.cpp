// macOS implementations of the SystemCollector sampling hooks declared in
// collector.h. The Linux counterparts live in collector.cpp guarded by
// #ifndef __APPLE__; everything shared (constructor, collect() dispatch,
// preferMount dedup, daily-process drain) stays in collector.cpp.
#include "collector.h"
#include "macosutils.h"

#include <QDateTime>
#include <QHash>
#include <QStringList>

#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

#include <ifaddrs.h>
#include <net/if.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kTopN = 20;

// host_processor_info returns one CPU_STATE_MAX-wide tick row per core.
struct MachCpuSample {
    quint64 total = 0;
    quint64 idle = 0;
    bool valid = false;
};

MachCpuSample ticksFromRow(const integer_t *row) {
    const quint64 user = static_cast<quint64>(row[CPU_STATE_USER]);
    const quint64 sys = static_cast<quint64>(row[CPU_STATE_SYSTEM]);
    const quint64 idle = static_cast<quint64>(row[CPU_STATE_IDLE]);
    const quint64 nice = static_cast<quint64>(row[CPU_STATE_NICE]);
    return {user + sys + idle + nice, idle + nice, true};
}
} // namespace

SystemCollector::CpuTicks SystemCollector::readCpuTicks() const {
    processor_info_array_t info = nullptr;
    mach_msg_type_number_t count = 0;
    natural_t coreCount = 0;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                            &coreCount, &info, &count) != KERN_SUCCESS) {
        return {};
    }
    CpuTicks sum;
    for (natural_t i = 0; i < coreCount; ++i) {
        const auto row = ticksFromRow(info + i * CPU_STATE_MAX);
        sum.total += row.total;
        sum.idle += row.idle;
        sum.valid = true;
    }
    const vm_size_t size = static_cast<vm_size_t>(coreCount) * CPU_STATE_MAX * sizeof(integer_t);
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(info), size);
    return sum;
}

QMap<int, SystemCollector::CpuTicks> SystemCollector::readCpuCoreTicks() const {
    processor_info_array_t info = nullptr;
    mach_msg_type_number_t count = 0;
    natural_t coreCount = 0;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                            &coreCount, &info, &count) != KERN_SUCCESS) {
        return {};
    }
    QMap<int, CpuTicks> out;
    for (natural_t i = 0; i < coreCount; ++i) {
        const auto row = ticksFromRow(info + i * CPU_STATE_MAX);
        out.insert(static_cast<int>(i), {row.total, row.idle, true});
    }
    const vm_size_t size = static_cast<vm_size_t>(coreCount) * CPU_STATE_MAX * sizeof(integer_t);
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(info), size);
    return out;
}

void SystemCollector::collectMemory(SystemMetric &m) const {
    const double totalMiB = MacUtils::memTotalMiB();
    if (!(totalMiB > 0.0)) return;
    vm_statistics64_data_t vmstat;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vmstat), &count) != KERN_SUCCESS) {
        return;
    }
    const double pageSize = [] { vm_size_t v = 0; host_page_size(mach_host_self(), &v); return v > 0 ? static_cast<double>(v) : 4096.0; }();
    // Activity-Monitor-style "used": everything that is neither free nor
    // inactive (inactive pages are cached file/anonymous data, reclaimable).
    const double freeMiB = static_cast<double>(vmstat.free_count) * pageSize / 1048576.0;
    const double inactiveMiB = static_cast<double>(vmstat.inactive_count) * pageSize / 1048576.0;
    const double usedMiB = totalMiB - freeMiB - inactiveMiB;
    m.memoryUsedPct = std::clamp(usedMiB / totalMiB * 100.0, 0.0, 100.0);

    // vm.swapusage returns a binary struct xsw_usage (the sysctl(8) CLI is
    // what formats the "total = ...M used = ...M" string). Layout on current
    // kernels (32 bytes): uint64 total, uint64 avail, uint64 used, then
    // uint32 pagesize and a flag word — verified live on the target machine.
    // The buffer must be the full 32 bytes; a truncated one gets ENOMEM.
    struct XswUsage {
        quint64 total = 0, avail = 0, used = 0;
        quint32 pageSize = 0, flags = 0;
    };
    XswUsage swu;
    size_t len = sizeof(swu);
    if (sysctlbyname("vm.swapusage", &swu, &len, nullptr, 0) == 0 && len >= sizeof(swu) && swu.total > 0) {
        m.swapUsedPct = std::clamp(static_cast<double>(swu.used) / static_cast<double>(swu.total) * 100.0, 0.0, 100.0);
    }
}

void SystemCollector::collectLoad(SystemMetric &m) const {
    double load = 0;
    if (getloadavg(&load, 1) == 1) m.load1 = load;
}

// The SMC answers a curated set of keys without root: the hottest P-core
// channel stands in for a package temperature, and the curated Sensors-page
// list carries CPU/GPU/NAND plus the fans. NVMe temperature on Apple Silicon
// is the NAND flash channel mean (there is no nvme hwmon on this platform).
void SystemCollector::collectTemperature(SystemMetric &m) const {
    const double t = MacUtils::cpuTemperatureC();
    if (std::isfinite(t)) m.cpuTemperatureC = t;
}

void SystemCollector::collectPsi(SystemMetric &m) const { Q_UNUSED(m); } // no kernel PSI on macOS

void SystemCollector::collectSensors(SystemMetric &m) const {
    m.sensors = MacUtils::readSensors(&m.fans);
    for (const auto &s : m.sensors) {
        if (s.label == QLatin1String("NAND") && std::isfinite(s.tempC)) {
            m.nvmeTemperatureC = s.tempC;
            break;
        }
    }
}

void SystemCollector::collectBattery(SystemMetric &m) const {
    const auto b = MacUtils::readBattery();
    if (!b.present) return;
    if (std::isfinite(b.percent)) m.batteryPercent = b.percent;
    if (std::isfinite(b.powerW)) m.batteryPowerW = b.powerW;
    if (std::isfinite(b.temperatureC)) m.batteryTemperatureC = b.temperatureC;
    m.batteryStatus = b.status;
    // Health barely changes: recompute once per UTC day, keep yesterday's
    // value across glitched reads — same policy as the Linux path.
    const qint64 healthDay = QDateTime::currentSecsSinceEpoch() / 86400;
    if (healthDay != lastHealthDay_) {
        lastHealthDay_ = healthDay;
        if (std::isfinite(b.health)) cachedHealth_ = b.health;
    }
    if (std::isfinite(cachedHealth_)) m.batteryHealthPercent = cachedHealth_;
}

void SystemCollector::collectProcesses(SystemMetric &m) const {
    // Differential scan: cpuNanos is cumulative CPU time per process, so the
    // share inside the window is the nanosecond delta over the wall time.
    const auto list = MacUtils::listProcesses();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const double dt = lastProcSampleMs_ > 0 ? static_cast<double>(nowMs - lastProcSampleMs_) / 1000.0 : 0.0;
    QHash<qint64, quint64> cur;
    QVector<ProcInfo> procs;
    procs.reserve(list.size());
    for (const auto &mp : list) {
        const bool haveCpu = std::isfinite(mp.cpuNanos);
        if (haveCpu) cur[mp.pid] = static_cast<quint64>(mp.cpuNanos);
        ProcInfo p;
        p.pid = mp.pid;
        p.name = mp.name;
        p.state = mp.state;
        p.threads = mp.threads;
        p.rssMiB = mp.rssMiB;
        p.swapMiB = lmNaN(); // per-process swap is not exposed by the kernel
        if (dt > 0 && haveCpu) {
            const auto prev = lastProcTicks_.constFind(p.pid);
            if (prev != lastProcTicks_.constEnd() && mp.cpuNanos >= static_cast<double>(*prev)) {
                p.cpuPercent = (mp.cpuNanos - static_cast<double>(*prev)) / 1e9 / dt * 100.0;
            }
        }
        if (std::isfinite(p.cpuPercent) && !p.name.isEmpty()) {
            auto &agg = dailyProcs_[p.name];
            agg.name = p.name;
            agg.cpuSec += p.cpuPercent / 100.0 * dt;
            if (std::isfinite(p.rssMiB) && !(agg.maxRssMiB > p.rssMiB)) agg.maxRssMiB = p.rssMiB;
        }
        procs.push_back(p);
    }
    lastProcTicks_ = std::move(cur);
    lastProcSampleMs_ = nowMs;

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

void SystemCollector::collectDisks(SystemMetric &m) const {
    // getmntinfo(MNT_NOWAIT) reads the mount table without stat()-ing any
    // mount point, so unreachable network volumes cannot stall the sampler.
    struct statfs *mnts = nullptr;
    const int count = getmntinfo(&mnts, MNT_NOWAIT);
    if (count <= 0 || !mnts) {
        m.disks.clear();
        return;
    }
    // Local, user-visible, writable filesystems only; autofs/devfs/loopback-style
    // pseudo filesystems carry no meaningful usage, and macOS mounts a pile of
    // read-only system images (CoreSimulator runtimes, cryptexd toolchains,
    // .dmg payloads) that would otherwise flood the list at ~97-100% used.
    const QStringList localFs = {"apfs", "hfs", "exfat", "msdos", "ntfs", "udf", "smbfs", "afpfs"};
    bool haveData = false;
    DiskInfo dataEntry;
    // Key by backing device so the same volume mounted at two paths shows once.
    QHash<QString, DiskInfo> byMount;
    auto preferMount = [](const QString &a, const QString &b) {
        if (a == "/") return b != "/";
        if (b == "/") return false;
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    };
    for (int i = 0; i < count; ++i) {
        const struct statfs &fs = mnts[i];
        const QString fsType = QString::fromLocal8Bit(fs.f_fstypename);
        if (!localFs.contains(fsType, Qt::CaseInsensitive)) continue;
        if (!(fs.f_flags & MNT_LOCAL)) continue;
        if (fs.f_flags & MNT_RDONLY) continue;
        const QString mount = QString::fromLocal8Bit(fs.f_mntonname);
        if (mount.startsWith("/Library/Developer/CoreSimulator/")
            || mount.contains("/com.apple.security.cryptexd")) continue;
        // The APFS container reports / (sealed system volume, nearly empty)
        // and /System/Volumes/Data (everything the user writes). Show one
        // entry for the container with the Data volume's usage; the other
        // firmlinks (Preboot, Update, Home, ...) are internal slices.
        if (mount == "/System/Volumes/Data") {
            haveData = true;
            dataEntry = DiskInfo();
            dataEntry.mountPoint = "/";
            if (fs.f_blocks > 0) {
                dataEntry.usedPct = static_cast<double>(fs.f_blocks - fs.f_bavail)
                    / static_cast<double>(fs.f_blocks) * 100.0;
            }
            continue;
        }
        if (mount.startsWith("/System/Volumes/")) continue;
        if (mount == "/private/var/vm" || mount == "/net" || mount == "/home" || mount == "/dev") continue;
        if (!(fs.f_blocks > 0)) continue;
        DiskInfo di;
        di.mountPoint = mount;
        di.usedPct = std::clamp(
            static_cast<double>(fs.f_blocks - fs.f_bavail) / static_cast<double>(fs.f_blocks) * 100.0,
            0.0, 100.0);
        const QString dev = QString::fromLocal8Bit(fs.f_mntfromname);
        auto it = byMount.find(dev);
        if (it == byMount.end()) {
            byMount.insert(dev, di);
        } else if (preferMount(mount, it->mountPoint)) {
            *it = di;
        }
    }
    if (haveData) {
        byMount.insert("/", dataEntry);
    }
    m.disks.clear();
    for (const auto &d : byMount) { m.disks.append(d); }
    // Fallback when the mount table was unreadable: at least show root.
    if (m.disks.isEmpty()) {
        struct statfs root{};
        if (statfs("/", &root) == 0 && root.f_blocks > 0) {
            DiskInfo di;
            di.mountPoint = "/";
            di.usedPct = std::clamp(
                static_cast<double>(root.f_blocks - root.f_bavail) / static_cast<double>(root.f_blocks) * 100.0,
                0.0, 100.0);
            m.disks.append(di);
        }
    }
    std::stable_sort(m.disks.begin(), m.disks.end(), [](const DiskInfo &a, const DiskInfo &b) {
        if ((a.mountPoint == "/") != (b.mountPoint == "/")) { return a.mountPoint == "/"; }
        return a.mountPoint < b.mountPoint;
    });
}

SystemCollector::IoCounters SystemCollector::readNetworkCounters() const {
    // AF_LINK records carry the per-interface byte counters (if_data).
    // Summing en* (physical/Wi-Fi) and utun* (VPN tunnels) mirrors the Linux
    // rule of counting real egress while skipping bridges and AirDrop-side
    // duplicates (bridge/awdl/llw/anpi) that would double-count en0 traffic.
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) return {};
    quint64 rx = 0, tx = 0;
    bool any = false;
    for (const ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;
        const QString name = QString::fromLatin1(ifa->ifa_name);
        const bool counted = name.startsWith("en") || name.startsWith("utun");
        if (!counted) continue;
        const auto *d = static_cast<const if_data *>(ifa->ifa_data);
        if (!d) continue;
        rx += d->ifi_ibytes;
        tx += d->ifi_obytes;
        any = true;
    }
    freeifaddrs(list);
    return {rx, tx, any};
}

// Per-disk byte counters come from the IOBlockStorageDriver registry
// entries' Statistics dictionary ("Bytes (Read)"/"Bytes (Write)"); the
// shared collect() dispatch multiplies by 512 for the rate, so the byte
// counters are pre-divided by 512 into the sector fields.
QVector<SystemCollector::DiskIoCounters> SystemCollector::readDiskCounters() const {
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOBlockStorageDriver"), &it) != KERN_SUCCESS) {
        return {};
    }
    QVector<DiskIoCounters> out;
    io_object_t obj;
    while ((obj = IOIteratorNext(it)) != 0) {
        CFMutableDictionaryRef props = nullptr;
        if (IORegistryEntryCreateCFProperties(obj, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props) {
            CFDictionaryRef stats = nullptr;
            if (CFDictionaryGetValueIfPresent(props, CFSTR("Statistics"), reinterpret_cast<const void **>(&stats))
                && stats && CFGetTypeID(stats) == CFDictionaryGetTypeID()) {
                const auto statBytes = [](CFDictionaryRef d, CFStringRef key) -> long long {
                    CFNumberRef n = nullptr;
                    if (CFDictionaryGetValueIfPresent(d, key, reinterpret_cast<const void **>(&n))
                        && n && CFGetTypeID(n) == CFNumberGetTypeID()) {
                        long long v = 0;
                        if (CFNumberGetValue(n, kCFNumberSInt64Type, &v) && v >= 0) return v;
                    }
                    return -1;
                };
                const long long readBytes = statBytes(static_cast<CFDictionaryRef>(stats), CFSTR("Bytes (Read)"));
                const long long writeBytes = statBytes(static_cast<CFDictionaryRef>(stats), CFSTR("Bytes (Write)"));
                if (readBytes >= 0 || writeBytes >= 0) {
                    DiskIoCounters c;
                    c.device = QString::fromLatin1("iodisk%1").arg(out.size());
                    c.readSectors = readBytes > 0 ? static_cast<quint64>(readBytes) / 512 : 0;
                    c.writeSectors = writeBytes > 0 ? static_cast<quint64>(writeBytes) / 512 : 0;
                    out.push_back(c);
                }
            }
            CFRelease(props);
        }
        IOObjectRelease(obj);
    }
    IOObjectRelease(it);
    return out;
}
