#include "settingsdialog.h"
#include "appconfig.h"

#include <QCheckBox>
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

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("LiteMon Settings"));
    setMinimumWidth(430);
    const AppConfig c = AppConfig::load();
    auto *root = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("Settings are stored per user. Restart the collector service after changing sampling or retention."), this);
    hint->setWordWrap(true);
    root->addWidget(hint);
    auto *form = new QFormLayout;
    systemInterval_ = spin(60, 300, c.sampleIntervalSec, this);
    gpuInterval_ = spin(60, 600, c.gpuIntervalSec, this);
    detailDays_ = spin(6, 30, c.detailRetentionDays, this);
    archiveDays_ = spin(30, 3650, c.archiveRetentionDays, this);
    gpuEnabled_ = new QCheckBox(tr("Collect Intel / NVIDIA GPU metrics"), this);
    gpuEnabled_->setChecked(c.collectGpu);
    form->addRow(tr("System sample (seconds)"), systemInterval_);
    form->addRow(tr("GPU sample (seconds)"), gpuInterval_);
    form->addRow(tr("Fine detail retention (days, min 6)"), detailDays_);
    form->addRow(tr("Compressed 5-minute archive (days)"), archiveDays_);
    form->addRow(QString(), gpuEnabled_);
    root->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

void SettingsDialog::save() {
    AppConfig c = AppConfig::load();
    c.sampleIntervalSec = systemInterval_->value();
    c.gpuIntervalSec = qMax(gpuInterval_->value(), c.sampleIntervalSec);
    c.detailRetentionDays = detailDays_->value();
    c.archiveRetentionDays = archiveDays_->value();
    c.collectGpu = gpuEnabled_->isChecked();
    c.save();
    accept();
}
