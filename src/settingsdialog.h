#pragma once
#include <QDialog>

class QCheckBox;
class QComboBox;
class QSpinBox;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);
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
};
