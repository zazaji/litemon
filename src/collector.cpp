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
    }
    if (!any) return;
    if (energyFull > 0 && energyNow >= 0) m.batteryPercent = std::clamp(energyNow / energyFull * 100.0, 0.0, 100.0);
    else if (pctCount) m.batteryPercent = pctSum / pctCount;
    if (energyDesign > 0 && energyFull > 0) m.batteryHealthPercent = std::clamp(energyFull / energyDesign * 100.0, 0.0, 150.0);
    const bool charging = std::any_of(statuses.cbegin(), statuses.cend(), [](const QString &s){ return s.compare("Charging", Qt::CaseInsensitive) == 0; });
    const bool discharging = std::any_of(statuses.cbegin(), statuses.cend(), [](const QString &s){ return s.compare("Discharging", Qt::CaseInsensitive) == 0; });
    m.batteryPowerW = (discharging && !charging) ? -power : power;
    m.batteryStatus = statuses.join(" + ");
}

void SystemCollector::collectDiskSpace(SystemMetric &m) const {
    QStorageInfo root = QStorageInfo::root();
    if (!root.isValid() || !root.isReady()) return;
    m.diskTotalGiB = static_cast<double>(root.bytesTotal()) / 1073741824.0;
    m.diskUsedGiB = static_cast<double>(root.bytesTotal() - root.bytesAvailable()) / 1073741824.0;
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

SystemCollector::IoCounters SystemCollector::readDiskCounters() const {
    QFile f("/proc/diskstats");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    quint64 readSectors = 0, writeSectors = 0;
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
        if (ok1) { readSectors += r; }
        if (ok2) { writeSectors += w; }
    }
    return {readSectors, writeSectors, true};
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

    const auto disk = readDiskCounters();
    if (disk.valid && lastDisk_.valid && disk.a >= lastDisk_.a && disk.b >= lastDisk_.b) {
        m.diskReadMiBs = static_cast<double>(disk.a - lastDisk_.a) * 512.0 / 1048576.0 / seconds;
        m.diskWriteMiBs = static_cast<double>(disk.b - lastDisk_.b) * 512.0 / 1048576.0 / seconds;
    }
    lastDisk_ = disk;
    rateTimer_.restart();

    collectMemory(m);
    collectLoad(m);
    collectTemperature(m);
    collectBattery(m);
    collectDiskSpace(m);
    if (includeGpu) m.gpus = gpuCollector_.collect();
    return m;
}
