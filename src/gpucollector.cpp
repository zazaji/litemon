#include "gpucollector.h"
#include "linuxutils.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>

using namespace LinuxUtils;

QVector<GpuMetric> GpuCollector::collect() {
    QVector<GpuMetric> out;
    auto intel = collectIntel();
    auto nvidia = collectNvidia();
    out.reserve(intel.size() + nvidia.size());
    for (auto &g : intel) out.push_back(g);
    for (auto &g : nvidia) out.push_back(g);
    return out;
}

QString GpuCollector::normalizeNvidiaBusId(const QString &bus) {
    const QString b = bus.trimmed().toLower();
    static const QRegularExpression fullRe(QStringLiteral("^([0-9a-f]{8}):(.+)$"));
    const auto full = fullRe.match(b);
    if (full.hasMatch()) {
        return full.captured(1).right(4) + QStringLiteral(":") + full.captured(2);
    }
    static const QRegularExpression shortRe(QStringLiteral("^([0-9a-f]{1,2}):([0-9a-f]{2}\\.[0-9])$"));
    const auto sh = shortRe.match(b);
    if (sh.hasMatch()) {
        QString busPart = sh.captured(1);
        if (busPart.size() < 2) { busPart.prepend(QStringLiteral("0")); }
        return QStringLiteral("0000:") + busPart + QStringLiteral(":") + sh.captured(2);
    }
    return b;
}

QStringList GpuCollector::nvidiaPciDevices() const {
    QStringList out;
    QDir d(QStringLiteral("/sys/bus/pci/devices"));
    const auto entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &e : entries) {
        const QString base = d.filePath(e);
        if (readText(base + QStringLiteral("/vendor")).toLower() != QStringLiteral("0x10de")) { continue; }
        const QString cls = readText(base + QStringLiteral("/class")).toLower();
        if (!cls.startsWith(QStringLiteral("0x0300")) && !cls.startsWith(QStringLiteral("0x0302"))) { continue; }
        out.push_back(e);
    }
    return out;
}

GpuMetric GpuCollector::nvidiaPlaceholder(const QString &bdf, const QString &stateOverride) {
    GpuMetric g;
    g.id = QStringLiteral("nvidia:") + bdf;
    g.vendor = QStringLiteral("NVIDIA");
    g.name = QStringLiteral("NVIDIA GPU");
    g.driver = QStringLiteral("nvidia");
    QString state = stateOverride;
    if (state.isEmpty()) {
        state = readText(QStringLiteral("/sys/bus/pci/devices/") + bdf + QStringLiteral("/power/runtime_status"));
    }
    if (state.isEmpty()) { state = QStringLiteral("unknown"); }
    g.state = state;
    if (state.compare(QStringLiteral("suspended"), Qt::CaseInsensitive) == 0) {
        g.utilization = 0.0;
    }
    return g;
}

bool GpuCollector::allNvidiaRuntimeSuspended(QStringList *pciDevices) const {
    const QStringList devices = nvidiaPciDevices();
    if (devices.isEmpty()) { return false; }
    if (pciDevices) { *pciDevices = devices; }
    for (const auto &e : devices) {
        const QString state = readText(QStringLiteral("/sys/bus/pci/devices/") + e + QStringLiteral("/power/runtime_status")).toLower();
        if (state != QStringLiteral("suspended")) { return false; }
    }
    return true;
}

QVector<GpuMetric> GpuCollector::collectNvidia() {
    const QStringList pciDevices = nvidiaPciDevices();
    QStringList suspended;
    if (!pciDevices.isEmpty() && allNvidiaRuntimeSuspended(&suspended)) {
        // Important on Optimus laptops: calling nvidia-smi can wake a sleeping dGPU.
        QVector<GpuMetric> out;
        for (const auto &pci : suspended) {
            out.push_back(nvidiaPlaceholder(pci, QStringLiteral("suspended")));
        }
        return out;
    }

    const QString smi = commandPath("nvidia-smi");
    if (smi.isEmpty()) {
        // Hardware is present but the driver tool is missing: still report the
        // device (metrics unknown) so it stays visible and selectable instead
        // of silently vanishing from the GUI.
        QVector<GpuMetric> out;
        for (const auto &pci : pciDevices) {
            out.push_back(nvidiaPlaceholder(pci));
        }
        return out;
    }

    const QStringList args = {
        "--query-gpu=index,name,utilization.gpu,memory.used,memory.total,temperature.gpu,power.draw,clocks.current.graphics,pci.bus_id",
        "--format=csv,noheader,nounits"
    };
    int code = -1;
    const QByteArray raw = runCommand(smi, args, 5000, &code);
    if (code != 0 && raw.isEmpty()) {
        // Query failed (e.g. driver hiccup): fall back to placeholders for
        // known devices so they do not flap in and out of the UI.
        QVector<GpuMetric> out;
        for (const auto &pci : pciDevices) {
            out.push_back(nvidiaPlaceholder(pci));
        }
        return out;
    }

    QVector<GpuMetric> out;
    QSet<QString> seen;
    const auto lines = QString::fromUtf8(raw).split('\n', Qt::SkipEmptyParts);
    for (const auto &line : lines) {
        const auto cols = line.split(',');
        if (cols.size() < 9) continue;
        GpuMetric g;
        const QString index = cols[0].trimmed();
        const QString bus = normalizeNvidiaBusId(cols[8]);
        g.id = "nvidia:" + (bus.isEmpty() ? index : bus);
        if (!bus.isEmpty()) { seen.insert(bus); }
        g.vendor = "NVIDIA";
        g.name = cols[1].trimmed();
        g.driver = "nvidia";
        g.state = "active";
        g.utilization = parseNumber(cols[2]);
        g.memoryUsedMiB = parseNumber(cols[3]);
        g.memoryTotalMiB = parseNumber(cols[4]);
        g.temperatureC = parseNumber(cols[5]);
        g.powerW = parseNumber(cols[6]);
        g.frequencyMHz = parseNumber(cols[7]);
        out.push_back(g);
    }
    // A known device absent from nvidia-smi output (e.g. per-GPU suspend on a
    // multi-GPU box) keeps a placeholder row so its ID stays stable.
    for (const auto &pci : pciDevices) {
        if (!seen.contains(pci.toLower())) {
            out.push_back(nvidiaPlaceholder(pci));
        }
    }
    return out;
}

QVector<QJsonObject> GpuCollector::parseIntelGpuTopObjects(const QByteArray &raw) {
    QVector<QJsonObject> objects;
    bool inString = false;
    bool escape = false;
    int depth = 0;
    int start = -1;
    for (int i = 0; i < raw.size(); ++i) {
        const char c = raw.at(i);
        if (inString) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '{') {
            if (depth == 0) start = i;
            ++depth;
        } else if (c == '}') {
            if (depth > 0) --depth;
            if (depth == 0 && start >= 0) {
                const QByteArray one = raw.mid(start, i - start + 1);
                QJsonParseError err;
                const auto doc = QJsonDocument::fromJson(one, &err);
                if (err.error == QJsonParseError::NoError && doc.isObject()) objects.push_back(doc.object());
                start = -1;
            }
        }
    }
    return objects;
}

static double jsonNumber(const QJsonObject &o, const QString &key) {
    const auto v = o.value(key);
    return v.isDouble() ? v.toDouble() : lmNaN();
}

GpuMetric GpuCollector::collectIntelCard(const QString &cardPath, const QString &cardName) {
    GpuMetric g;
    g.id = "intel:" + cardName;
    g.vendor = "Intel";
    g.name = "Intel GPU (" + cardName + ")";
    g.driver = QFileInfo(cardPath + "/device/driver").symLinkTarget().section('/', -1);
    if (g.driver.isEmpty()) g.driver = "i915/xe";
    g.state = readText(cardPath + "/device/power/runtime_status");
    if (g.state.isEmpty()) g.state = "active";
    if (g.state.compare("suspended", Qt::CaseInsensitive) == 0) g.utilization = 0.0;

    // Cheap sysfs path first (available on some DRM drivers).
    for (const QString &p : {cardPath + "/device/gpu_busy_percent", cardPath + "/gpu_busy_percent"}) {
        if (auto v = readDouble(p)) { g.utilization = *v; break; }
    }
    g.temperatureC = findIntelTemperature(cardPath);
    g.frequencyMHz = findIntelFrequency(cardPath);
    const auto vram = findIntelVram(cardPath);
    g.memoryUsedMiB = vram.first;
    g.memoryTotalMiB = vram.second;

    // Intel XPU-SMI is the preferred fallback for Xe/Arc when available.
    // Select by PCI BDF so multi-GPU systems never mix devices.
    const QString xpu = commandPath("xpu-smi");
    if (!xpu.isEmpty() && g.state.compare("suspended", Qt::CaseInsensitive) != 0) {
        const QString bdf = QFileInfo(cardPath + "/device").canonicalFilePath().section('/', -1);
        if (!bdf.isEmpty()) {
            const QString fields = "name,utilization.gpu,memory.used,memory.total,temperature.gpu,power.draw,clocks.current.graphics,pci.bus_id";
            const QStringList args = {"--query-gpu=" + fields, "--id", bdf, "--format=csv,noheader,nounits"};
            int xpuCode = -1;
            const QByteArray xpuRaw = runCommand(xpu, args, 1800, &xpuCode);
            if (xpuCode == 0) {
                const QString line = QString::fromUtf8(xpuRaw).split('\n', Qt::SkipEmptyParts).value(0);
                const auto cols = line.split(',');
                if (cols.size() >= 8) {
                    if (!cols[0].trimmed().isEmpty()) g.name = cols[0].trimmed();
                    const double util = parseNumber(cols[1]); if (std::isfinite(util)) g.utilization = util;
                    const double mu = parseNumber(cols[2]); if (std::isfinite(mu)) g.memoryUsedMiB = mu;
                    const double mt = parseNumber(cols[3]); if (std::isfinite(mt)) g.memoryTotalMiB = mt;
                    const double temp = parseNumber(cols[4]); if (std::isfinite(temp)) g.temperatureC = temp;
                    const double pwr = parseNumber(cols[5]); if (std::isfinite(pwr)) g.powerW = pwr;
                    const double freq = parseNumber(cols[6]); if (std::isfinite(freq)) g.frequencyMHz = freq;
                }
            }
        }
    }

    // i915 PMU path. Debian's intel_gpu_top supports JSON output and per-device selection.
    if (!std::isfinite(g.utilization) || !std::isfinite(g.powerW) || !std::isfinite(g.frequencyMHz)) {
        const QString top = commandPath("intel_gpu_top");
        if (!top.isEmpty() && g.state.toLower() != "suspended" && g.driver.toLower() != "xe") {
            const QString dev = "/dev/dri/" + cardName;
            const QStringList args = {"-J", "-s", "300", "-n", "2", "-d", "drm:" + dev, "-o", "-"};
            int code = -1;
            const QByteArray raw = runCommand(top, args, 1300, &code);
            Q_UNUSED(code);
            const auto objs = parseIntelGpuTopObjects(raw);
            if (!objs.isEmpty()) {
                const QJsonObject o = objs.last();
                const auto engines = o.value("engines").toObject();
                double maxBusy = lmNaN();
                for (auto it = engines.begin(); it != engines.end(); ++it) {
                    if (!it.value().isObject()) continue;
                    const double busy = jsonNumber(it.value().toObject(), "busy");
                    if (std::isfinite(busy) && (!std::isfinite(maxBusy) || busy > maxBusy)) maxBusy = busy;
                }
                if (std::isfinite(maxBusy)) g.utilization = std::clamp(maxBusy, 0.0, 100.0);
                const auto freq = o.value("frequency").toObject();
                const double actual = jsonNumber(freq, "actual");
                if (std::isfinite(actual)) g.frequencyMHz = actual;
                const auto power = o.value("power").toObject();
                const double gpuPower = jsonNumber(power, "GPU");
                if (std::isfinite(gpuPower)) g.powerW = gpuPower;
            }
        }
    }
    return g;
}

QVector<GpuMetric> GpuCollector::collectIntel() {
    QVector<GpuMetric> out;
    QDir drm("/sys/class/drm");
    const QRegularExpression cardRe("^card[0-9]+$");
    const auto entries = drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &name : entries) {
        if (!cardRe.match(name).hasMatch()) continue;
        const QString card = drm.filePath(name);
        if (readText(card + "/device/vendor").toLower() != "0x8086") continue;
        out.push_back(collectIntelCard(card, name));
    }
    return out;
}

double GpuCollector::findIntelTemperature(const QString &cardPath) {
    QDir hw(cardPath + "/device/hwmon");
    double best = lmNaN();
    for (const auto &h : hw.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir d(hw.filePath(h));
        const auto files = d.entryList({"temp*_input"}, QDir::Files);
        for (const auto &f : files) {
            if (auto v = readDouble(d.filePath(f))) {
                const double c = *v / 1000.0;
                if (c > 0 && c < 150 && (!std::isfinite(best) || c > best)) best = c;
            }
        }
    }
    return best;
}

double GpuCollector::findIntelFrequency(const QString &cardPath) {
    const QStringList paths = {
        cardPath + "/gt_cur_freq_mhz",
        cardPath + "/device/gt_cur_freq_mhz",
        cardPath + "/device/tile0/gt0/freq0/cur_freq",
        cardPath + "/device/tile0/gt0/freq0/act_freq"
    };
    for (const auto &p : paths) if (auto v = readDouble(p)) return *v;
    return lmNaN();
}

QPair<double,double> GpuCollector::findIntelVram(const QString &cardPath) {
    const QStringList usedPaths = {
        cardPath + "/device/mem_info_vram_used",
        cardPath + "/device/mem_info_local_used"
    };
    const QStringList totalPaths = {
        cardPath + "/device/mem_info_vram_total",
        cardPath + "/device/mem_info_local_total"
    };
    double used = lmNaN(), total = lmNaN();
    for (const auto &p : usedPaths) if (auto v = readDouble(p)) { used = *v / 1048576.0; break; }
    for (const auto &p : totalPaths) if (auto v = readDouble(p)) { total = *v / 1048576.0; break; }
    return {used, total};
}
