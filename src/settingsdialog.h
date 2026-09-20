#pragma once
#include <QDialog>

class QCheckBox;
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
};
