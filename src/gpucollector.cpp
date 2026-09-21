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
    auto amd = collectAmd();
    auto npu = collectHuawei();
    out.reserve(intel.size() + nvidia.size() + amd.size() + npu.size());
    for (auto &g : intel) out.push_back(g);
    for (auto &g : nvidia) out.push_back(g);
    for (auto &g : amd) out.push_back(g);
    for (auto &g : npu) out.push_back(g);
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
    g.temperatureC = findHwmonTemperature(cardPath);
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
            // intel_gpu_top streams JSON lines every -s milliseconds until it
            // is killed; there is no sample-count flag, so rely on the
            // runCommand timeout to cut it off.
            const QString dev = "/dev/dri/" + cardName;
            const QStringList args = {"-J", "-s", "300", "-d", "drm:" + dev, "-o", "-"};
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

double GpuCollector::findHwmonTemperature(const QString &cardPath) {
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

QPair<double,double> GpuCollector::findAmdVram(const QString &cardPath) {
    double used = lmNaN(), total = lmNaN();
    if (auto v = readDouble(cardPath + "/device/mem_info_vram_used")) { used = *v / 1048576.0; }
    if (auto v = readDouble(cardPath + "/device/mem_info_vram_total")) { total = *v / 1048576.0; }
    return {used, total};
}

// --- AMD (amdgpu) ---

GpuMetric GpuCollector::collectAmdCard(const QString &cardPath, const QString &cardName) {
    GpuMetric g;
    g.id = "amd:" + cardName;
    g.vendor = "AMD";
    g.name = "AMD GPU (" + cardName + ")";
    // amdgpu exposes a human-readable product name on newer kernels.
    const QString product = readText(cardPath + "/device/product_name").trimmed();
    if (!product.isEmpty()) { g.name = "AMD " + product; }
    g.driver = QFileInfo(cardPath + "/device/driver").symLinkTarget().section('/', -1);
    if (g.driver.isEmpty()) { g.driver = "amdgpu"; }
    g.state = readText(cardPath + "/device/power/runtime_status");
    if (g.state.isEmpty()) { g.state = "active"; }
    if (g.state.compare("suspended", Qt::CaseInsensitive) == 0) { g.utilization = 0.0; }

    // gpu_busy_percent is the standard amdgpu utilization report (0-100).
    for (const QString &p : {cardPath + "/device/gpu_busy_percent", cardPath + "/gpu_busy_percent"}) {
        if (auto v = readDouble(p)) { g.utilization = *v; break; }
    }
    g.temperatureC = findHwmonTemperature(cardPath);

    // hwmon: power1_average/power1_input in microwatts, freq1_input for the
    // gfx clock. amdgpu reports the clock in Hz; guard the magnitude in case
    // another driver exposes kHz or MHz directly.
    QDir hw(cardPath + "/device/hwmon");
    for (const auto &h : hw.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir d(hw.filePath(h));
        for (const QString &pf : {QStringLiteral("power1_average"), QStringLiteral("power1_input")}) {
            if (auto v = readDouble(d.filePath(pf))) { g.powerW = *v / 1.0e6; break; }
        }
        if (auto v = readDouble(d.filePath(QStringLiteral("freq1_input")))) {
            double mhz = *v;
            if (mhz > 1.0e6) { mhz /= 1.0e6; }
            else if (mhz > 1000.0) { mhz /= 1000.0; }
            g.frequencyMHz = mhz;
        }
        if (std::isfinite(g.powerW) && std::isfinite(g.frequencyMHz)) { break; }
    }

    const auto vram = findAmdVram(cardPath);
    g.memoryUsedMiB = vram.first;
    g.memoryTotalMiB = vram.second;
    return g;
}

QVector<GpuMetric> GpuCollector::collectAmd() {
    QVector<GpuMetric> out;
    QDir drm("/sys/class/drm");
    const QRegularExpression cardRe("^card[0-9]+$");
    const auto entries = drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &name : entries) {
        if (!cardRe.match(name).hasMatch()) continue;
        const QString card = drm.filePath(name);
        // AMD Radeon/Radeon Pro use PCI vendor 0x1002 (ATI/AMD/Advanced Micro Devices).
        if (readText(card + "/device/vendor").toLower() != "0x1002") continue;
        out.push_back(collectAmdCard(card, name));
    }
    return out;
}

// --- Huawei Ascend NPU ---

QStringList GpuCollector::huaweiPciDevices() const {
    QStringList out;
    QDir d(QStringLiteral("/sys/bus/pci/devices"));
    const auto entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &e : entries) {
        const QString base = d.filePath(e);
        if (readText(base + QStringLiteral("/vendor")).toLower() != QStringLiteral("0x19e5")) { continue; }
        // Ascend NPUs enumerate as processing accelerators (0x0b40); keep the
        // display classes too so hybrid cards are not missed.
        const QString cls = readText(base + QStringLiteral("/class")).toLower();
        if (!cls.startsWith(QStringLiteral("0x0b40")) && !cls.startsWith(QStringLiteral("0x0300"))
            && !cls.startsWith(QStringLiteral("0x0302"))) { continue; }
        out.push_back(e);
    }
    return out;
}

GpuMetric GpuCollector::huaweiPlaceholder(const QString &bdf) {
    GpuMetric g;
    g.id = QStringLiteral("npu:") + bdf.toLower();
    g.vendor = QStringLiteral("Huawei");
    g.name = QStringLiteral("Huawei NPU");
    g.driver = QStringLiteral("davinci");
    QString state = readText(QStringLiteral("/sys/bus/pci/devices/") + bdf + QStringLiteral("/power/runtime_status"));
    if (state.isEmpty()) { state = QStringLiteral("unknown"); }
    g.state = state;
    if (state.compare(QStringLiteral("suspended"), Qt::CaseInsensitive) == 0) { g.utilization = 0.0; }
    return g;
}

QVector<GpuMetric> GpuCollector::collectHuawei() {
    QVector<GpuMetric> out;
    const QStringList pci = huaweiPciDevices();
    if (pci.isEmpty()) { return out; }

    const auto placeholders = [&]() {
        for (const auto &bdf : pci) { out.push_back(huaweiPlaceholder(bdf)); }
    };

    const QString smi = commandPath(QStringLiteral("npu-smi"));
    if (smi.isEmpty()) {
        // Hardware present but the management tool is missing: keep the device
        // visible (metrics unknown) instead of dropping it from the GUI.
        placeholders();
        return out;
    }
    int code = -1;
    const QByteArray raw = runCommand(smi, {QStringLiteral("info")}, 5000, &code);
    const auto devs = parseNpuSmiSummary(raw);
    if (code != 0 || devs.isEmpty()) {
        placeholders();
        return out;
    }

    QSet<QString> covered;
    for (const auto &d : devs) {
        GpuMetric g;
        g.vendor = QStringLiteral("Huawei");
        g.driver = QStringLiteral("davinci");
        g.state = QStringLiteral("active");
        g.name = d.name.isEmpty() ? QStringLiteral("Huawei NPU")
                                  : QStringLiteral("Huawei NPU (") + d.name + QStringLiteral(")");
        g.utilization = d.utilization;
        g.temperatureC = d.temperatureC;
        g.powerW = d.powerW;
        g.memoryUsedMiB = d.memoryUsedMiB;
        g.memoryTotalMiB = d.memoryTotalMiB;
        const QString bus = d.busId.toLower();
        if (!bus.isEmpty()) {
            g.id = QStringLiteral("npu:") + bus;
            covered.insert(bus);
            const QString drv = QFileInfo(QStringLiteral("/sys/bus/pci/devices/") + bus
                + QStringLiteral("/driver")).symLinkTarget().section('/', -1);
            if (!drv.isEmpty()) { g.driver = drv; }
        } else {
            g.id = QStringLiteral("npu:") + QString::number(static_cast<int>(out.size()));
        }
        out.push_back(g);
    }
    // A known PCI device not covered by npu-smi output keeps a placeholder row
    // so its ID stays stable in the GUI.
    for (const auto &bdf : pci) {
        if (!covered.contains(bdf.toLower())) { out.push_back(huaweiPlaceholder(bdf)); }
    }
    return out;
}

QVector<GpuCollector::NpuSmiDevice> GpuCollector::parseNpuSmiSummary(const QByteArray &raw) {
    QVector<NpuSmiDevice> out;
    // Chip rows are recognized by a BDF cell; they carry AICore utilization and
    // memory and follow their device row (name/power/temperature).
    static const QRegularExpression bdfRe(QStringLiteral("^([0-9a-fA-F]{4}):([0-9a-fA-F]{2}):([0-9a-fA-F]{2}\\.[0-9a-fA-F])$"));

    NpuSmiDevice pending;
    bool pendingValid = false;
    auto flush = [&]() {
        if (pendingValid) { out.push_back(pending); }
        pending = NpuSmiDevice{};
        pendingValid = false;
    };

    const auto lines = QString::fromUtf8(raw).split('\n');
    for (const auto &line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.startsWith('|')) { continue; }
        const auto segs = trimmed.split('|');
        QStringList cells;
        for (int i = 1; i < segs.size(); ++i) { cells.append(segs.at(i).trimmed()); }
        if (cells.isEmpty() || cells.at(0).isEmpty() || !cells.at(0).at(0).isDigit()) { continue; }

        int bdfCell = -1;
        for (int i = 0; i < cells.size(); ++i) {
            if (bdfRe.match(cells.at(i)).hasMatch()) { bdfCell = i; break; }
        }
        if (!pendingValid) {
            pendingValid = true; // tolerate tables whose device row is missing
        }
        if (bdfCell < 0) {
            // Device row: first cell holds the NPU index and the product name.
            const auto nameToks = cells.at(0).split(' ', Qt::SkipEmptyParts);
            QString name;
            for (int k = 1; k < static_cast<int>(nameToks.size()); ++k) {
                const auto tok = nameToks.at(k);
                bool onlyDigits = true;
                for (const QChar &c : tok) { if (!c.isDigit()) { onlyDigits = false; break; } }
                if (!onlyDigits) { name = tok; break; }
            }
            if (!name.isEmpty()) { pending.name = name; }
            // Power (may be fractional) and temperature share a later cell,
            // followed by other "X / Y" counters that must be skipped.
            for (int i = 1; i < static_cast<int>(cells.size()); ++i) {
                const auto toks = cells.at(i).split(' ', Qt::SkipEmptyParts);
                int slash = -1;
                for (int k = 0; k < static_cast<int>(toks.size()); ++k) {
                    if (toks.at(k) == QStringLiteral("/")) { slash = k; break; }
                }
                const int start = (slash >= 0) ? slash + 2 : 0;
                for (int k = start; k < static_cast<int>(toks.size()); ++k) {
                    bool ok = false;
                    const double v = toks.at(k).toDouble(&ok);
                    if (!ok) { continue; }
                    if (!std::isfinite(pending.powerW)) { pending.powerW = v; continue; }
                    if (!std::isfinite(pending.temperatureC) && v > -100.0 && v < 150.0) {
                        pending.temperatureC = v;
                    }
                }
                if (std::isfinite(pending.powerW) && std::isfinite(pending.temperatureC)) { break; }
            }
        } else {
            if (pending.busId.isEmpty()) { pending.busId = cells.at(bdfCell).toLower(); }
            for (int i = bdfCell + 1; i < static_cast<int>(cells.size()); ++i) {
                const auto toks = cells.at(i).split(' ', Qt::SkipEmptyParts);
                int slash = -1;
                for (int k = 0; k < static_cast<int>(toks.size()); ++k) {
                    if (toks.at(k) == QStringLiteral("/")) { slash = k; break; }
                }
                if (slash <= 0 || slash + 1 >= static_cast<int>(toks.size())) { continue; }
                // "AICore%  used / total MB": the token before the pair is the
                // AICore utilization when present.
                pending.memoryUsedMiB = parseNumber(toks.at(slash - 1)) / 1.048576;
                pending.memoryTotalMiB = parseNumber(toks.at(slash + 1)) / 1.048576;
                if (slash >= 2) { pending.utilization = parseNumber(toks.at(0)); }
                break;
            }
            if (std::isfinite(pending.memoryTotalMiB)) {
                flush(); // later chip rows belong to the next device row
            }
        }
    }
    flush();
    return out;
}
