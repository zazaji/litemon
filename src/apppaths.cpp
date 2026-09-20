#include "apppaths.h"
#include <QDir>
#include <QProcessEnvironment>

namespace {
QString home() { return QDir::homePath(); }
QString xdg(const char *name, const QString &fallback) {
    const QString v = QProcessEnvironment::systemEnvironment().value(QString::fromLatin1(name));
    return v.isEmpty() ? fallback : v;
}
QString ensure(const QString &p) { QDir().mkpath(p); return p; }
}
namespace AppPaths {
QString dataDir() { return ensure(xdg("XDG_DATA_HOME", home() + "/.local/share") + "/litemon"); }
QString cacheDir() { return ensure(xdg("XDG_CACHE_HOME", home() + "/.cache") + "/litemon"); }
QString logDir() { return ensure(dataDir() + "/logs"); }
QString databasePath() { return dataDir() + "/metrics.sqlite"; }
QString collectorLockPath() { return cacheDir() + "/collector.lock"; }
}
