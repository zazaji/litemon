#include "macosutils.h"

#ifdef __APPLE__

#include <QDateTime>

#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>

#include <CoreFoundation/CoreFoundation.h>

#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// kIOMainPortDefault is marked macOS 12.0+ while we build with an 11.0
// deployment target; the runtime we ship on is far newer, silence the gate.
#pragma clang diagnostic ignored "-Wunguarded-availability-new"

namespace MacUtils {

QVector<MacProc> listProcesses() {
    // One sysctl call for pid/name/state of every process, then libproc for
    // per-pid details (RSS, threads, CPU nanoseconds).
    size_t len = 0;
    if (sysctlbyname("kern.proc.all", nullptr, &len, nullptr, 0) != 0 || len == 0) return {};
    QByteArray buf(len, '\0');
    if (sysctlbyname("kern.proc.all", buf.data(), &len, nullptr, 0) != 0) return {};
    const int procCount = static_cast<int>(len / sizeof(kinfo_proc));
    const auto *procs = reinterpret_cast<const kinfo_proc *>(buf.constData());

    QVector<MacProc> out;
    out.reserve(procCount);
    for (int i = 0; i < procCount; ++i) {
        const kinfo_proc &kp = procs[i];
        const pid_t pid = kp.kp_proc.p_pid;
        if (pid <= 0) continue;

        MacProc p;
        p.pid = pid;
        char stateChar = 'S';
        switch (kp.kp_proc.p_stat) {
            case SRUN: stateChar = 'R'; break;
            case SSTOP: stateChar = 'T'; break;
            case SZOMB: stateChar = 'Z'; break;
            case SIDL: stateChar = 'I'; break;
            case SSLEEP:
            default: stateChar = 'S'; break;
        }
        p.state = QString(QChar(stateChar));
        p.name = QString::fromLocal8Bit(kp.kp_proc.p_comm);

        // proc_pidpath resolves the full binary path for processes we own;
        // for foreign/system processes it fails and p_comm (max 15 chars)
        // stays the name.
        char pathBuf[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_pidpath(pid, pathBuf, sizeof(pathBuf)) > 0) {
            const QString full = QString::fromLocal8Bit(pathBuf);
            const auto slash = full.lastIndexOf('/');
            p.name = slash >= 0 ? full.mid(slash + 1) : full;
        }

        proc_taskinfo ti{};
        if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) == sizeof(ti)) {
            p.rssMiB = static_cast<double>(ti.pti_resident_size) / 1048576.0;
            p.threads = static_cast<qint64>(ti.pti_threadnum);
            p.cpuNanos = static_cast<double>(ti.pti_total_user + ti.pti_total_system);
        }
        out.push_back(p);
    }
    return out;
}

namespace {
CFNumberRef dictNumber(CFMutableDictionaryRef dict, const char *key) {
    if (!dict) return nullptr;
    const CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFNumberRef v = nullptr;
    if (CFDictionaryGetValueIfPresent(dict, k, reinterpret_cast<const void **>(&v)) && CFGetTypeID(v) != CFNumberGetTypeID()) v = nullptr;
    CFRelease(k);
    return v; // borrowed reference, valid while dict lives
}

double dictDouble(CFMutableDictionaryRef dict, const char *key) {
    if (CFNumberRef n = dictNumber(dict, key)) {
        double d = 0;
        if (CFNumberGetValue(n, kCFNumberDoubleType, &d)) return d;
    }
    return std::nan("");
}

bool dictBool(CFMutableDictionaryRef dict, const char *key) {
    if (!dict) return false;
    const CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFBooleanRef v = nullptr;
    if (CFDictionaryGetValueIfPresent(dict, k, reinterpret_cast<const void **>(&v))
        && (!v || CFGetTypeID(v) != CFBooleanGetTypeID())) v = nullptr;
    CFRelease(k);
    return v ? CFBooleanGetValue(v) : false;
}
} // namespace

MacBattery readBattery() {
    MacBattery b;

    // AppleSmartBattery IOService registry: capacity, design capacity,
    // voltage/instantaneous current for power draw. Accessible without
    // privileges (same source Stats/iStat use).
    const io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSmartBattery"));
    if (svc) {
        CFMutableDictionaryRef props = nullptr;
        if (IORegistryEntryCreateCFProperties(svc, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props) {
            b.present = true;
            const double design = dictDouble(props, "DesignCapacity");
            double maxCap = dictDouble(props, "NominalChargeCapacity");
            if (!(maxCap > 0)) maxCap = dictDouble(props, "MaxCapacity");
            if (design > 0 && maxCap > 0) b.health = maxCap / design * 100.0;
            const double voltageMV = dictDouble(props, "Voltage");
            const double currentMA = dictDouble(props, "InstantAmperage");
            if (std::isfinite(voltageMV) && std::isfinite(currentMA)) {
                const double watts = std::abs(voltageMV * currentMA) / 1e9;
                const bool external = dictBool(props, "ExternalConnected");
                const bool charging = dictBool(props, "IsCharging");
                b.powerW = (external && !charging) ? watts : (charging ? watts : -watts);
                // Discharging at ~0 W would read -0.0 and format as "-0.0 W";
                // snap near-zero to exact zero so it groups with idle.
                if (std::abs(b.powerW) < 0.05) b.powerW = 0.0;
                b.status = charging ? QStringLiteral("Charging")
                         : external  ? QStringLiteral("AC Attached")
                                     : QStringLiteral("Discharging");
            }
            const double cur = dictDouble(props, "CurrentCapacity");
            if (std::isfinite(cur)) b.percent = cur;
            const double temp = dictDouble(props, "Temperature");
            if (std::isfinite(temp) && temp > 0) b.temperatureC = temp / 100.0;
            CFRelease(props);
        }
        IOObjectRelease(svc);
    }

    // Percent fallback through the blessed IOPS API when the registry read
    // did not yield one.
    if (!std::isfinite(b.percent)) {
        if (CFTypeRef blob = IOPSCopyPowerSourcesInfo()) {
            if (CFArrayRef list = IOPSCopyPowerSourcesList(blob)) {
                for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
                    const CFTypeRef src = CFArrayGetValueAtIndex(list, i);
                    if (CFDictionaryRef desc = IOPSGetPowerSourceDescription(blob, src)) {
                        CFNumberRef pct = nullptr;
                        if (CFDictionaryGetValueIfPresent(desc, CFSTR(kIOPSCurrentCapacityKey), reinterpret_cast<const void **>(&pct)) && pct) {
                            double d = 0;
                            if (CFNumberGetValue(pct, kCFNumberDoubleType, &d)) { b.percent = d; b.present = true; }
                        }
                    }
                }
                CFRelease(list);
            }
            CFRelease(blob);
        }
    }
    if (std::isfinite(b.health)) b.health = std::clamp(b.health, 0.0, 150.0);
    return b;
}

double memTotalMiB() {
    uint64_t bytes = 0;
    size_t len = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &len, nullptr, 0) != 0) return std::nan("");
    return static_cast<double>(bytes) / 1048576.0;
}

double memAvailableMiB() {
    vm_statistics64_data_t vmstat;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vmstat), &count) != KERN_SUCCESS) {
        return std::nan("");
    }
    vm_size_t page = 0;
    host_page_size(mach_host_self(), &page);
    const double pageMiB = (page > 0 ? static_cast<double>(page) : 4096.0) / 1048576.0;
    // Complement of the collector's "used" definition: free + inactive.
    return (static_cast<double>(vmstat.free_count) + static_cast<double>(vmstat.inactive_count)) * pageMiB;
}

double swapTotalMiB() {
    struct XswUsage { quint64 total = 0, avail = 0, used = 0; quint32 pageSize = 0, flags = 0; };
    XswUsage swu;
    size_t len = sizeof(swu);
    if (sysctlbyname("vm.swapusage", &swu, &len, nullptr, 0) != 0 || len < sizeof(swu)) return std::nan("");
    return static_cast<double>(swu.total) / 1048576.0;
}

// --- AppleSMC user client -------------------------------------------------
// Two-step read protocol (KEYINFO then READ_BYTES, command selector in
// data8) per the well-known osx-cpu-temp user-client layout. Apple Silicon
// temperature/fan keys are "flt" (32-bit IEEE float), not the classic
// sp78/fpe2 fixed-point types of Intel Macs, so both are decoded here.
namespace {
constexpr unsigned kSmcKernelIndex = 2;
constexpr char kSmcCmdReadBytes = 5;
constexpr char kSmcCmdReadKeyInfo = 9;

struct SmcVers { char major; char minor; char build; char reserved; UInt16 release; };
struct SmcPLimit { UInt16 version; UInt16 length; UInt32 cpuPLimit; UInt32 gpuPLimit; UInt32 memPLimit; };
struct SmcKeyInfo { UInt32 dataSize; UInt32 dataType; char dataAttributes; };
struct SmcKeyData {
    UInt32 key;
    SmcVers vers;
    SmcPLimit pLimit;
    SmcKeyInfo keyInfo;
    char result;
    char status;
    char data8;
    UInt32 data32;
    char bytes[32];
};

quint32 smcKeyToInt(const char *key) {
    return (quint32(static_cast<quint8>(key[0])) << 24) | (quint32(quint8(key[1])) << 16)
         | (quint32(quint8(key[2])) << 8) | quint32(quint8(key[3]));
}

io_connect_t smcConnection() {
    // One user-client connection per process; opening needs no privileges.
    static io_connect_t conn = [] {
        const io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
        if (!svc) return static_cast<io_connect_t>(0);
        io_connect_t c = 0;
        const kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &c);
        IOObjectRelease(svc);
        return kr == KERN_SUCCESS ? c : 0;
    }();
    return conn;
}

bool smcReadKey(const char *key, quint32 *dataSizeOut, quint32 *typeOut, quint8 *out) {
    const io_connect_t conn = smcConnection();
    if (!conn) return false;
    SmcKeyData in{}, res{};
    size_t sz = sizeof(res);
    in.key = smcKeyToInt(key);
    in.data8 = kSmcCmdReadKeyInfo;
    if (IOConnectCallStructMethod(conn, kSmcKernelIndex, &in, sizeof(in), &res, &sz) != KERN_SUCCESS
        || res.keyInfo.dataSize == 0 || res.keyInfo.dataSize > sizeof(res.bytes)) {
        return false;
    }
    const UInt32 size = res.keyInfo.dataSize;
    if (dataSizeOut) *dataSizeOut = size;
    if (typeOut) *typeOut = res.keyInfo.dataType;
    memset(&res, 0, sizeof(res));
    size_t rsz = sizeof(res);
    in.keyInfo.dataSize = size;
    in.data8 = kSmcCmdReadBytes;
    if (IOConnectCallStructMethod(conn, kSmcKernelIndex, &in, sizeof(in), &res, &rsz) != KERN_SUCCESS) return false;
    if (out) memcpy(out, res.bytes, size);
    return true;
}

char typeChar(quint32 type, int shift) { return static_cast<char>((type >> shift) & 0xff); }

} // namespace

double smcTemperature(const char *key) {
    quint32 size = 0, type = 0;
    quint8 buf[32] = {};
    if (!smcReadKey(key, &size, &type, buf)) return lmNaN();
    const char t0 = typeChar(type, 24), t1 = typeChar(type, 16);
    if (t0 == 'f' && t1 == 'l' && size >= 4) { // "flt "
        float f = 0;
        memcpy(&f, buf, 4);
        return static_cast<double>(f);
    }
    if (t0 == 's' && t1 == 'p' && size >= 2) { // sp78 fixed point
        const int v = static_cast<int16_t>((static_cast<uint16_t>(buf[0]) << 8) | buf[1]);
        return v / 256.0;
    }
    if (t0 == 'u' && t1 == 'i' && typeChar(type, 8) == '8') return static_cast<double>(buf[0]);
    if (t0 == 'u' && t1 == 'i' && typeChar(type, 8) == '1') { // ui16, big endian
        return static_cast<double>((static_cast<quint16>(buf[0]) << 8) | buf[1]);
    }
    return lmNaN();
}

double cpuTemperatureC() {
    // P-core channels are Tp01..Tp0Y on Apple Silicon; Intel Macs answer the
    // TC* package keys. Every key that is missing simply returns NaN.
    static const char *keys[] = {
        "Tp01", "Tp02", "Tp04", "Tp05", "Tp06", "Tp08", "Tp09", "Tp0A",
        "Tp0C", "Tp0D", "Tp0E", "Tp0F", "Tp0G", "Tp0H", "Tp0J", "Tp0K",
        "Tp0L", "Tp0M", "Tp0N", "Tp0P", "Tp0Q", "Tp0R", "Tp0S", "Tp0T",
        "Tp0U", "Tp0X", "Tp0Y",
        "TC0P", "TC0D", "TC0E", "TC0F", "TC0H", "TCAD",
    };
    double best = lmNaN();
    for (const char *k : keys) {
        const double v = smcTemperature(k);
        if (std::isfinite(v) && (!std::isfinite(best) || v > best)) best = v;
    }
    return best;
}

QVector<SensorInfo> readSensors(QVector<FanInfo> *fans) {
    QVector<SensorInfo> out;
    const double cpu = cpuTemperatureC();
    if (std::isfinite(cpu)) out.push_back({QStringLiteral("SMC"), QStringLiteral("CPU"), cpu});

    double gpu = lmNaN();
    for (const char *k : {"Tg05", "Tg0D", "Tg04", "TG0P"}) {
        const double v = smcTemperature(k);
        if (std::isfinite(v) && (!std::isfinite(gpu) || v > gpu)) gpu = v;
    }
    if (std::isfinite(gpu)) out.push_back({QStringLiteral("SMC"), QStringLiteral("GPU"), gpu});

    // NAND flash channels: the closest stand-in for an NVMe composite
    // temperature on Apple Silicon (the storage controller reports the
    // flash channels, typically well below a Linux nvme composite value).
    static const char *nandKeys[] = {
        "TD00", "TD01", "TD02", "TD04", "TD05", "TD08", "TD0C", "TD0D",
        "TD0E", "TD0H", "TD14", "TD18", "TD1C", "TD24",
    };
    double nandSum = 0.0;
    int nandN = 0;
    for (const char *k : nandKeys) {
        const double v = smcTemperature(k);
        if (std::isfinite(v)) { nandSum += v; ++nandN; }
    }
    if (nandN > 0) {
        out.push_back({QStringLiteral("SMC"), QStringLiteral("NAND"), nandSum / nandN});
    }

    if (fans) *fans = readFans();
    return out;
}

QVector<FanInfo> readFans() {
    QVector<FanInfo> out;
    quint8 buf[4] = {};
    quint32 size = 0;
    int fanCount = 0;
    if (smcReadKey("FNum", &size, nullptr, buf)) fanCount = qMin<int>(buf[0], 8);
    for (int i = 0; i < fanCount; ++i) {
        FanInfo f;
        f.chip = QStringLiteral("SMC");
        f.label = QStringLiteral("Fan %1").arg(i);
        char ac[5] = {'F', static_cast<char>('0' + i), 'A', 'c', 0};
        const double rpm = smcTemperature(ac);
        if (std::isfinite(rpm)) f.rpm = rpm;
        out.push_back(f);
    }
    return out;
}

double gpuUtilizationPct() {
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS) {
        return std::nan("");
    }
    double best = std::nan("");
    io_object_t obj;
    while ((obj = IOIteratorNext(it)) != 0) {
        if (CFTypeRef stats = IORegistryEntryCreateCFProperty(obj, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0)) {
            if (CFGetTypeID(stats) == CFDictionaryGetTypeID()) {
                CFNumberRef n = nullptr;
                if (CFDictionaryGetValueIfPresent(static_cast<CFDictionaryRef>(stats), CFSTR("Device Utilization %"),
                                                  reinterpret_cast<const void **>(&n))
                    && n && CFGetTypeID(n) == CFNumberGetTypeID()) {
                    double d = 0;
                    if (CFNumberGetValue(n, kCFNumberDoubleType, &d) && std::isfinite(d)) {
                        best = std::isfinite(best) ? std::max(best, d) : d;
                    }
                }
            }
            CFRelease(stats);
        }
        IOObjectRelease(obj);
    }
    IOObjectRelease(it);
    return best;
}

QString gpuModelName() {
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it) == KERN_SUCCESS) {
        io_object_t obj;
        while ((obj = IOIteratorNext(it)) != 0) {
            if (CFTypeRef val = IORegistryEntryCreateCFProperty(obj, CFSTR("model"), kCFAllocatorDefault, 0)) {
                if (CFGetTypeID(val) == CFStringGetTypeID()) {
                    char buf[96] = {};
                    if (CFStringGetCString(static_cast<CFStringRef>(val), buf, sizeof(buf), kCFStringEncodingUTF8)
                        && buf[0]) {
                        CFRelease(val);
                        IOObjectRelease(obj);
                        IOObjectRelease(it);
                        return QString::fromLatin1(buf);
                    }
                }
                CFRelease(val);
            }
            IOObjectRelease(obj);
        }
        IOObjectRelease(it);
    }
    return QStringLiteral("Apple GPU");
}

void hideDockIcon() {
    // Menu-bar-only app presence: with the tray icon in the menu bar the Dock
    // icon only duplicates it. Accessory policy also keeps the app out of
    // Cmd-Tab; the settings window still shows and works normally.
    // Done through the ObjC runtime so this file stays plain C++.
    if (Class cls = objc_getClass("NSApplication")) {
        // objc_msgSend has no prototype in modern SDK headers — every call must
        // go through an explicitly cast function pointer.
        using SendAppFn = id (*)(id, SEL);
        if (id app = reinterpret_cast<SendAppFn>(objc_msgSend)(reinterpret_cast<id>(cls),
                                                               sel_registerName("sharedApplication"))) {
            using SetPolicyFn = void (*)(id, SEL, long);
            reinterpret_cast<SetPolicyFn>(objc_msgSend)(
                app, sel_registerName("setActivationPolicy:"), 1 /* NSApplicationActivationPolicyAccessory */);
        }
    }
}

} // namespace MacUtils
#endif // __APPLE__
