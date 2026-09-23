#include "settingsdialog.h"
#include "appconfig.h"
#include "linuxutils.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSpinBox>
#include <QVBoxLayout>

static QSpinBox *spin(int lo, int hi, int value, QWidget *parent) {
    auto *s = new QSpinBox(parent);
    s->setRange(lo, hi);
    s->setValue(value);
    return s;
}

static void fillTrayCombo(QComboBox *cb) {
    cb->addItem(SettingsDialog::tr("Off"), QString());
    for (const auto &s : LinuxUtils::readHwmonSensors()) {
        cb->addItem(s.chip + QStringLiteral(" · ") + s.label, s.chip + QStringLiteral("|") + s.label);
    }
    cb->setCurrentIndex(0);
}

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent) {
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
    // Tray digits: pick one or two live sensors; the tray icon shows their
    // values as plain numbers (no units). Read live so the list matches the
    // Sensors page even on machines where nothing is stored yet.
    traySensor1_ = new QComboBox(this);
    traySensor2_ = new QComboBox(this);
    fillTrayCombo(traySensor1_);
    fillTrayCombo(traySensor2_);
    const auto select = [](QComboBox *cb, const QString &key) {
        const int idx = cb->findData(key);
        cb->setCurrentIndex(idx < 0 ? 0 : idx);
    };
    if (c.traySensor1.isEmpty() && !c.traySensor2.isEmpty()) {
        select(traySensor1_, c.traySensor2); // normalize: slot 1 first
    } else {
        select(traySensor1_, c.traySensor1);
        select(traySensor2_, c.traySensor2);
    }
    form->addRow(tr("Tray digits 1 (sensor)"), traySensor1_);
    form->addRow(tr("Tray digits 2 (sensor)"), traySensor2_);
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
    c.traySensor1 = traySensor1_->currentData().toString();
    c.traySensor2 = traySensor2_->currentData().toString();
    if (c.traySensor1.isEmpty()) { c.traySensor1 = c.traySensor2; c.traySensor2.clear(); } // keep slot 1 first
    c.theme = theme_->currentData().toString();
    c.autostart = autostart_->isChecked();
    c.save();
    QString error;
    if (!AppConfig::setAutostartEnabled(c.autostart, &error)) {
        QMessageBox::warning(this, tr("LiteMon Settings"), error);
    }
    accept();
}
