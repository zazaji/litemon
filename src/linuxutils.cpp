#include "linuxutils.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
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

} // namespace LinuxUtils
