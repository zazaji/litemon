#include "settingsdialog.h"
#include "appconfig.h"
#include "linuxutils.h"
#ifdef __APPLE__
#include "macosutils.h"
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSpinBox>
#include <QVBoxLayout>
#include <cmath>

static QSpinBox *spin(int lo, int hi, int value, QWidget *parent) {
    auto *s = new QSpinBox(parent);
    s->setRange(lo, hi);
    s->setValue(value);
    return s;
}

static void fillTrayCombo(QComboBox *cb, const QStringList &diskMounts) {
    cb->addItem(SettingsDialog::tr("Off"), QString());
    // Special sources read live by the GUI itself (memory from /proc/meminfo
    // or MacUtils, CPU usage from the latest collector sample).
    cb->addItem(SettingsDialog::tr("Memory usage (%)"), QStringLiteral("usage|memory"));
    cb->addItem(SettingsDialog::tr("CPU usage (%)"), QStringLiteral("usage|cpu"));
    // Per-disk used-capacity percentage, from the newest collector sample.
    for (const QString &mp : diskMounts)
        cb->addItem(SettingsDialog::tr("Disk %1 usage (%)").arg(mp),
                    QStringLiteral("diskusage|") + mp);
#if defined(__APPLE__)
    // macOS: temperatures and fans come from the SMC (no /sys/class/hwmon).
    for (const auto &s : MacUtils::readSensors())
        cb->addItem(s.chip + QStringLiteral(" · ") + s.label, s.chip + QStringLiteral("|") + s.label);
    for (const auto &f : MacUtils::readFans())
        cb->addItem(f.chip + QStringLiteral(" · ") + f.label + QStringLiteral(" (RPM)"),
                    QStringLiteral("fan|") + f.chip + QStringLiteral("|") + f.label);
#else
    QVector<FanInfo> linuxFans;
    for (const auto &s : LinuxUtils::readHwmonSensors(&linuxFans))
        cb->addItem(s.chip + QStringLiteral(" · ") + s.label, s.chip + QStringLiteral("|") + s.label);
    for (const auto &f : linuxFans)
        cb->addItem(f.chip + QStringLiteral(" · ") + f.label + QStringLiteral(" (RPM)"),
                    QStringLiteral("fan|") + f.chip + QStringLiteral("|") + f.label);
#endif
    cb->setCurrentIndex(0);
}

SettingsDialog::SettingsDialog(QWidget *parent, const QStringList &diskMounts)
    : QDialog(parent), diskMounts_(diskMounts) {
    setWindowTitle(tr("LiteMon Settings"));
    setMinimumWidth(430);
    const AppConfig c = AppConfig::load();
    auto *root = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("Settings are stored per user. Restart the collector service after changing sampling or retention."), this);
    hint->setWordWrap(true);
    root->addWidget(hint);
    auto *form = new QFormLayout;
    // Sampling intervals are edited in whole minutes; AppConfig keeps seconds.
    systemInterval_ = spin(1, 5, qMax(1, c.sampleIntervalSec / 60), this);
    gpuInterval_ = spin(1, 10, c.gpuIntervalSec / 60, this);
    systemInterval_->setSuffix(tr(" min"));
    gpuInterval_->setSuffix(tr(" min"));
    detailDays_ = spin(6, 30, c.detailRetentionDays, this);
    archiveDays_ = spin(30, 3650, c.archiveRetentionDays, this);
    gpuEnabled_ = new QCheckBox(tr("Collect Intel / NVIDIA GPU metrics"), this);
    gpuEnabled_->setChecked(c.collectGpu);
    form->addRow(tr("System sample (minutes)"), systemInterval_);
    form->addRow(tr("GPU sample (minutes)"), gpuInterval_);
    form->addRow(tr("Fine detail retention (days, min 6)"), detailDays_);
    form->addRow(tr("Compressed 5-minute archive (days)"), archiveDays_);
    form->addRow(QString(), gpuEnabled_);
    theme_ = new QComboBox(this);
    theme_->addItem(tr("System"), "system");
    theme_->addItem(tr("Light"), "light");
    theme_->addItem(tr("Dark"), "dark");
    theme_->setCurrentIndex(qMax(0, theme_->findData(c.theme)));
    form->addRow(tr("Theme"), theme_);
    autostart_ = new QCheckBox(tr("Start LiteMon automatically at login"), this);
    autostart_->setChecked(c.autostart);
    form->addRow(QString(), autostart_);
    // Tray digits: pick up to four live sensors; the tray shows their values
    // as plain numbers (no units), all on one icon or one icon each. Read
    // live so the list matches the Sensors page even on machines where
    // nothing is stored yet.
    QComboBox *const sensors[] = {
        traySensor1_ = new QComboBox(this), traySensor2_ = new QComboBox(this),
        traySensor3_ = new QComboBox(this), traySensor4_ = new QComboBox(this)};
    for (QComboBox *cb : sensors) fillTrayCombo(cb, diskMounts_);
    const auto select = [](QComboBox *cb, const QString &key) {
        const int idx = cb->findData(key);
        cb->setCurrentIndex(idx < 0 ? 0 : idx);
    };
    // Compact configured slots to the front so the tray never shows a gap;
    // alarm thresholds travel with their sensor key.
    const QString keys[] = {c.traySensor1, c.traySensor2, c.traySensor3, c.traySensor4};
    const double keyAlarms[] = {c.trayAlarm1, c.trayAlarm2, c.trayAlarm3, c.trayAlarm4};
    QString compact[4];
    double compactAlarm[4] = {};
    int enabled = 0;
    for (int i = 0; i < 4; ++i)
        if (!keys[i].isEmpty()) { compact[enabled] = keys[i]; compactAlarm[enabled] = keyAlarms[i]; ++enabled; }
    for (int i = 0; i < 4; ++i) select(sensors[i], compact[i]);
    traySeparate_ = new QCheckBox(tr("Separate tray icons (one icon per value)"), this);
    traySeparate_->setChecked(c.traySeparate);
    form->addRow(tr("Tray layout"), traySeparate_);
    // Sensor rows are added by the loop below, each paired with its alarm
    // spin — adding them here as well would duplicate every row.
    // Alarm thresholds: when a slot's value reaches the threshold its digit
    // turns red. The threshold is in the sensor's own unit: % for usage
    // sources, RPM for fans, °C for temperature sensors. 0 disables the
    // alarm ("Off").
    auto alarmSpin = [this]() {
        auto *s = new QSpinBox(this);
        s->setRange(0, 200);
        s->setSpecialValueText(tr("Off"));
        return s;
    };
    QSpinBox *const alarmSpins[] = {
        trayAlarm1_ = alarmSpin(), trayAlarm2_ = alarmSpin(),
        trayAlarm3_ = alarmSpin(), trayAlarm4_ = alarmSpin()};
    const auto applyAlarmUnit = [](QComboBox *sensor, QSpinBox *spin) {
        const QString key = sensor->currentData().toString();
        if (key.startsWith(QStringLiteral("usage|"))) {
            spin->setRange(0, 100);
            spin->setSuffix(tr(" %"));
        } else if (key.startsWith(QStringLiteral("diskusage|"))) {
            spin->setRange(0, 100);
            spin->setSuffix(tr(" %"));
        } else if (key.startsWith(QStringLiteral("fan|"))) {
            spin->setRange(0, 10000);
            spin->setSuffix(tr(" RPM"));
        } else {
            spin->setRange(0, 200);
            spin->setSuffix(tr(" °C"));
        }
    };
    for (int i = 0; i < 4; ++i) {
        QComboBox *cb = sensors[i];
        QSpinBox *spin = alarmSpins[i];
        // Capture the widgets, not the local array: the lambda outlives the ctor.
        connect(cb, &QComboBox::currentIndexChanged, this, [applyAlarmUnit, cb, spin]{ applyAlarmUnit(cb, spin); });
        applyAlarmUnit(cb, spin);
        spin->setValue(static_cast<int>(std::lround(compactAlarm[i])));
        form->addRow(tr("Tray digits %1 (sensor)").arg(i + 1), cb);
        form->addRow(tr("Tray digits %1 alarm (at)").arg(i + 1), spin);
    }
    root->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

void SettingsDialog::save() {
    AppConfig c = AppConfig::load();
    c.sampleIntervalSec = systemInterval_->value() * 60;
    c.gpuIntervalSec = qMax(gpuInterval_->value() * 60, c.sampleIntervalSec);
    c.detailRetentionDays = detailDays_->value();
    c.archiveRetentionDays = archiveDays_->value();
    c.collectGpu = gpuEnabled_->isChecked();
    // Compact the enabled slots to the front; alarms travel with their slot.
    const QString rawKey[] = {traySensor1_->currentData().toString(), traySensor2_->currentData().toString(),
                              traySensor3_->currentData().toString(), traySensor4_->currentData().toString()};
    const double rawAlarm[] = {static_cast<double>(trayAlarm1_->value()), static_cast<double>(trayAlarm2_->value()),
                               static_cast<double>(trayAlarm3_->value()), static_cast<double>(trayAlarm4_->value())};
    QString key[4];
    double alarm[4] = {};
    int n = 0;
    for (int i = 0; i < 4; ++i)
        if (!rawKey[i].isEmpty()) { key[n] = rawKey[i]; alarm[n] = rawAlarm[i]; ++n; }
    c.traySensor1 = key[0]; c.traySensor2 = key[1]; c.traySensor3 = key[2]; c.traySensor4 = key[3];
    c.trayAlarm1 = alarm[0]; c.trayAlarm2 = alarm[1]; c.trayAlarm3 = alarm[2]; c.trayAlarm4 = alarm[3];
    c.traySeparate = traySeparate_->isChecked();
    c.theme = theme_->currentData().toString();
    c.autostart = autostart_->isChecked();
    c.save();
    QString error;
    if (!AppConfig::setAutostartEnabled(c.autostart, &error)) {
        QMessageBox::warning(this, tr("LiteMon Settings"), error);
    }
    accept();
}
