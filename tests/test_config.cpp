#include "appconfig.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("LiteMonTest");
    QCoreApplication::setApplicationName("litemon-test");
    QTemporaryDir tmp;
    qputenv("XDG_CONFIG_HOME", tmp.path().toUtf8());
    AppConfig c;
    c.sampleIntervalSec = 70;
    c.gpuIntervalSec = 130;
    c.detailRetentionDays = 9;
    c.archiveRetentionDays = 400;
    c.collectGpu = false;
    c.save();
    const AppConfig r = AppConfig::load();
    auto check = [](bool ok, const char *msg) {
        if (!ok) {
            std::fprintf(stderr, "test_config FAIL: %s\n", msg);
            return false;
        }
        return true;
    };
    bool ok = true;
    ok = check(r.sampleIntervalSec == 70, "sampleIntervalSec") && ok;
    ok = check(r.gpuIntervalSec == 130, "gpuIntervalSec") && ok;
    ok = check(r.detailRetentionDays == 9, "detailRetentionDays") && ok;
    ok = check(r.archiveRetentionDays == 400, "archiveRetentionDays") && ok;
    ok = check(!r.collectGpu, "collectGpu") && ok;
    if (!ok) { return 1; }
    return 0;
}
