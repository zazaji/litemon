#pragma once
#include <QDialog>

class QCheckBox;
class QComboBox;
class QSpinBox;
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr, const QStringList &diskMounts = QStringList());
private slots:
    void save();
private:
    QSpinBox *systemInterval_ = nullptr;
    QSpinBox *gpuInterval_ = nullptr;
    QSpinBox *detailDays_ = nullptr;
    QSpinBox *archiveDays_ = nullptr;
    QCheckBox *gpuEnabled_ = nullptr;
    QComboBox *theme_ = nullptr;
    QCheckBox *autostart_ = nullptr;
    QComboBox *traySensor1_ = nullptr;
    QComboBox *traySensor2_ = nullptr;
    QComboBox *traySensor3_ = nullptr;
    QComboBox *traySensor4_ = nullptr;
    QStringList diskMounts_;
    QSpinBox *trayAlarm1_ = nullptr;
    QSpinBox *trayAlarm2_ = nullptr;
    QSpinBox *trayAlarm3_ = nullptr;
    QSpinBox *trayAlarm4_ = nullptr;
    QCheckBox *traySeparate_ = nullptr;
};
