#pragma once

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
}
