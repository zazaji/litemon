#pragma once
#include <QJsonObject>
#include <QString>

class Diagnostics {
public:
    static QJsonObject collect(const QString &databasePath);
    static bool writeReport(const QString &databasePath, const QString &outputPath, QString *error = nullptr);
};
