#include "linuxutils.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <limits>

namespace LinuxUtils {

QString readText(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readAll()).trimmed();
}

std::optional<qint64> readInt64(const QString &path) {
    bool ok = false;
    const auto s = readText(path);
    const qint64 v = s.toLongLong(&ok);
    if (!ok) return std::nullopt;
    return v;
}

std::optional<double> readDouble(const QString &path) {
    bool ok = false;
    const auto s = readText(path);
    const double v = s.toDouble(&ok);
    if (!ok) return std::nullopt;
    return v;
}

QString commandPath(const QString &name) {
    return QStandardPaths::findExecutable(name);
}

QByteArray runCommand(const QString &program, const QStringList &args, int timeoutMs, int *exitCode) {
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(program, args, QIODevice::ReadOnly);
    if (!p.waitForStarted(500)) {
        if (exitCode) *exitCode = -1;
        return {};
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.terminate();
        if (!p.waitForFinished(250)) p.kill();
        p.waitForFinished(250);
        if (exitCode) *exitCode = -2;
        return p.readAllStandardOutput();
    }
    if (exitCode) *exitCode = p.exitCode();
    return p.readAllStandardOutput();
}

double parseNumber(const QString &text) {
    QString s = text.trimmed();
    if (s.isEmpty() || s.compare("N/A", Qt::CaseInsensitive) == 0 || s == "-") return std::numeric_limits<double>::quiet_NaN();
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok ? v : std::numeric_limits<double>::quiet_NaN();
}

QString humanRate(double mibPerSec) {
    if (!std::isfinite(mibPerSec)) return "—";
    if (mibPerSec < 1.0) return QString::number(mibPerSec * 1024.0, 'f', 0) + " KiB/s";
    if (mibPerSec < 1024.0) return QString::number(mibPerSec, 'f', 1) + " MiB/s";
    return QString::number(mibPerSec / 1024.0, 'f', 2) + " GiB/s";
}

QString humanBytesMiB(double mib) {
    if (!std::isfinite(mib)) return "—";
    if (mib < 1024.0) return QString::number(mib, 'f', 0) + " MiB";
    return QString::number(mib / 1024.0, 'f', 1) + " GiB";
}

PsiValues parsePsiContent(const QString &content) {
    PsiValues out;
    // Format: "some avg10=0.00 avg300=0.00 avg600=0.00 avg20=0.00"
    //         "full avg10=0.00 ..." (absent on the cpu resource)
    const auto lines = content.split('\n', Qt::SkipEmptyParts);
    for (const auto &line : lines) {
        const auto tok = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (tok.size() < 2) continue;
        double *target = nullptr;
        if (tok[0] == "some") target = &out.some;
        else if (tok[0] == "full") target = &out.full;
        if (!target) continue;
        for (const auto &t : tok.mid(1)) {
            if (t.startsWith("avg10=")) {
                *target = t.mid(6).toDouble();
                break;
            }
        }
    }
    return out;
}

std::optional<ProcSample> parseProcStat(const QString &content) {
    // comm (field 2) sits in parens and may itself contain spaces and ')',
    // so the payload ends at the LAST ')'; numeric fields resume from field 3.
    const auto open = static_cast<qsizetype>(content.indexOf('('));
    const auto close = static_cast<qsizetype>(content.lastIndexOf(')'));
    if (open <= 0 || close < open) return std::nullopt;
    ProcSample s;
    s.pid = content.left(open).trimmed().toLongLong();
    s.name = content.mid(open + 1, close - open - 1);
    // rest[N-3] holds stat field N (state=3, utime=14, stime=15,
    // num_threads=20, rss=24 — rss is in pages).
    const auto rest = content.mid(close + 1).split(' ', Qt::SkipEmptyParts);
    if (s.pid <= 0 || rest.size() < 22) return std::nullopt;
    s.state = rest[0];
    s.cpuTicks = rest[11].toULongLong() + rest[12].toULongLong();
    s.threads = rest[17].toLongLong();
    s.rssPages = rest[21].toLongLong();
    return s;
}

std::optional<qint64> parseProcStatusSwap(const QString &content) {
    const auto pos = static_cast<qsizetype>(content.indexOf(QLatin1String("VmSwap:")));
    if (pos < 0) return std::nullopt;
    // The separator after "VmSwap:" is whitespace (usually tabs).
    const auto tok = content.mid(pos + 7).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (tok.isEmpty()) return std::nullopt;
    bool ok = false;
    const qint64 kb = tok[0].toLongLong(&ok);
    return ok ? std::optional<qint64>(kb) : std::nullopt;
}

QVector<SensorInfo> readHwmonSensors(QVector<FanInfo> *fans) {
    QVector<SensorInfo> temps;
    // Callers reuse the vector across refreshes; start from a clean slate
    // or fan entries accumulate on every poll.
    if (fans) fans->clear();
    const QDir hw("/sys/class/hwmon");
    for (const auto &e : hw.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString base = hw.filePath(e);
        const QString chip = readText(base + "/name").trimmed();
        if (chip.isEmpty()) continue;
        const QDir dir(base);
        for (const auto &tf : dir.entryList({"temp*_input"}, QDir::Files)) {
            const auto raw = readDouble(base + "/" + tf);
            if (!raw) continue;
            const double c = *raw / 1000.0;
            if (c < -100.0 || c > 250.0) continue; // placeholder / unset channel
            SensorInfo s;
            s.chip = chip;
            s.label = readText(base + "/" + tf.left(tf.size() - 6) + "_label").trimmed();
            if (s.label.isEmpty()) s.label = chip;
            s.tempC = c;
            temps.push_back(s);
        }
        if (fans) {
            for (const auto &ff : dir.entryList({"fan*_input"}, QDir::Files)) {
                const auto raw = readDouble(base + "/" + ff);
                if (!raw) continue;
                FanInfo f;
                f.chip = chip;
                f.label = readText(base + "/" + ff.left(ff.size() - 6) + "_label").trimmed();
                if (f.label.isEmpty()) f.label = chip;
                f.rpm = *raw;
                fans->push_back(f);
            }
        }
    }
    return temps;
}

QString readBatteryStatus() {
    QDir ps("/sys/class/power_supply");
    QStringList statuses;
    for (const auto &e : ps.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString base = ps.filePath(e);
        if (readText(base + "/type").compare("Battery", Qt::CaseInsensitive) != 0) continue;
        statuses << readText(base + "/status");
    }
    return statuses.join(" + ");
}

double readSwapTotalMiB() {
    for (const auto &line : readText("/proc/meminfo").split('\n')) {
        if (line.startsWith("SwapTotal:")) {
            // value is in kB
            return parseNumber(line.section(':', 1).trimmed().section(' ', 0, 0)) / 1024.0;
        }
    }
    return lmNaN();
}

double readMemTotalMiB() {
    for (const auto &line : readText("/proc/meminfo").split('\n')) {
        if (line.startsWith("MemTotal:")) {
            // value is in kB
            return parseNumber(line.section(':', 1).trimmed().section(' ', 0, 0)) / 1024.0;
        }
    }
    return lmNaN();
}

double readMemAvailableMiB() {
    for (const auto &line : readText("/proc/meminfo").split('\n')) {
        if (line.startsWith("MemAvailable:")) {
            // value is in kB
            return parseNumber(line.section(':', 1).trimmed().section(' ', 0, 0)) / 1024.0;
        }
    }
    return lmNaN();
}

std::optional<OomInfo> parseOomKernelLine(const QString &line) {
    // short-unix: "<epoch.micros> <host> kernel: <message>"
    const int sp = line.indexOf(' ');
    if (sp <= 0) return std::nullopt;
    const qint64 ts = static_cast<qint64>(std::llround(line.left(sp).toDouble()));
    const QString msg = line.mid(sp + 1);
    static const QRegularExpression head(
        QStringLiteral("Out of memory: Killed process (\\d+) \\(([^)]*)\\)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = head.match(msg);
    if (!m.hasMatch()) return std::nullopt;
    OomInfo e;
    e.timestamp = ts;
    e.pid = m.captured(1).toLongLong();
    e.name = m.captured(2);
    e.source = QStringLiteral("kernel");
    static const QRegularExpression rss(QStringLiteral("anon-rss:(\\d+)kB"));
    const auto r = rss.match(msg);
    if (r.hasMatch()) e.rssMiB = r.captured(1).toDouble() / 1024.0;
    // "UID:1000" (newer kernels) or "UID 1000" (older)
    static const QRegularExpression uid(QStringLiteral("\\bUID[ :](\\d+)"));
    const auto u = uid.match(msg);
    if (u.hasMatch()) e.detail = QStringLiteral("uid %1").arg(u.captured(1));
    return e;
}

std::optional<OomInfo> parseOomOomdLine(const QString &line) {
    const int sp = line.indexOf(' ');
    if (sp <= 0) return std::nullopt;
    const qint64 ts = static_cast<qint64>(std::llround(line.left(sp).toDouble()));
    const QString msg = line.mid(sp + 1);
    if (!msg.contains(QStringLiteral("systemd-oomd["))) return std::nullopt;
    const int k = msg.indexOf(QStringLiteral(": Killed "));
    if (k < 0) return std::nullopt;
    // Target is the cgroup path between "Killed " and " due to ".
    const int after = k + static_cast<int>(qstrlen(": Killed "));
    int end = msg.indexOf(QStringLiteral(" due to "), after);
    QString detail;
    if (end >= 0) {
        detail = msg.mid(end + 1); // "due to memory pressure for ..."
    } else {
        end = msg.size();
    }
    const QString target = msg.mid(after, end - after);
    OomInfo e;
    e.timestamp = ts;
    e.source = QStringLiteral("systemd-oomd");
    e.detail = detail;
    const int slash = target.lastIndexOf('/');
    e.name = slash >= 0 ? target.mid(slash + 1) : target;
    if (e.name.endsWith(QStringLiteral(".service")))
        e.name.chop(static_cast<int>(qstrlen(".service")));
    return e;
}

QVector<OomInfo> readOomEvents(int days) {
    QVector<OomInfo> out;
    const QString since = QStringLiteral("--since=%1 days ago").arg(days);
    int rc = -1;
    const QByteArray kernel = runCommand(QStringLiteral("journalctl"),
        {QStringLiteral("-k"), QStringLiteral("-q"), QStringLiteral("--no-pager"),
         QStringLiteral("-o"), QStringLiteral("short-unix"), since}, 30000, &rc);
    if (rc == 0 || !kernel.isEmpty()) {
        const QStringList lines = QString::fromLocal8Bit(kernel).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const auto &l : lines) {
            if (auto e = parseOomKernelLine(l)) out.push_back(*e);
        }
    }
    rc = -1;
    const QByteArray oomd = runCommand(QStringLiteral("journalctl"),
        {QStringLiteral("-u"), QStringLiteral("systemd-oomd"), QStringLiteral("-q"),
         QStringLiteral("--no-pager"), QStringLiteral("-o"), QStringLiteral("short-unix"), since}, 30000, &rc);
    if (rc == 0 || !oomd.isEmpty()) {
        const QStringList lines = QString::fromLocal8Bit(oomd).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const auto &l : lines) {
            if (auto e = parseOomOomdLine(l)) out.push_back(*e);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const OomInfo &a, const OomInfo &b) { return a.timestamp > b.timestamp; });
    return out;
}

} // namespace LinuxUtils
