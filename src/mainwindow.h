#pragma once

#include "database.h"
#include "chartwidget.h"

#include <QMainWindow>
#include <QHash>
#include <QVector>

class QLabel;
class QGridLayout;
class QComboBox;
class QPushButton;
class QStackedWidget;
class QTimer;
class QScrollArea;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QString &dbPath, QWidget *parent=nullptr);

private slots:
    void refreshLatest();
    void refreshHistory();
    void gpuSelectionChanged();
    void showSettings();
    void saveDiagnostics();
    void exportCurrentCsv();
    void showAbout();

private:
    struct Card { QLabel *value=nullptr; QLabel *detail=nullptr; };
    Card makeCard(const QString &title, QGridLayout *grid, int row, int col);
    QWidget *makeHistoryPage(ChartWidget **a, ChartWidget **b);
    void refreshCpuCoreCharts(qint64 from, qint64 to);
    qint64 selectedSpanSeconds() const;
    void setupUi();
    void updateDbStatus();
    static QString fmtPercent(double v);
    static QString fmtTemp(double v);
    static QString fmtPower(double v);

    MetricsDatabase db_;
    QStackedWidget *stack_=nullptr;
    QComboBox *range_=nullptr;
    QPushButton *prevBtn_=nullptr;
    QPushButton *nextBtn_=nullptr;
    QLabel *periodLabel_=nullptr;
    qint64 timeOffset_=0;
    QComboBox *gpuSelector_=nullptr;
    QLabel *updated_=nullptr;
    QLabel *dbStatus_=nullptr;
    Card cpuCard_,memCard_,diskCard_,netCard_,batteryCard_;
    QHash<QString,Card> gpuCards_;
    QWidget *gpuCardsContainer_=nullptr;
    QGridLayout *gpuCardsLayout_=nullptr;

    ChartWidget *cpuUsage_=nullptr,*cpuTemp_=nullptr,*mem_=nullptr,*swap_=nullptr,*net_=nullptr,*diskIo_=nullptr,*diskSpace_=nullptr,*battery_=nullptr,*batteryPower_=nullptr,*gpuUsage_=nullptr,*gpuAux_=nullptr,*gpuMemory_=nullptr;
    QLabel *cpuCoresLabel_=nullptr;
    QScrollArea *cpuCoresScroll_=nullptr;
    QGridLayout *cpuCoresLayout_=nullptr;
    QVector<ChartWidget*> cpuCoreCharts_;
    QVector<int> cpuCoreIds_;
    QTimer *timer_=nullptr;
    int latestTicks_=0;
};
