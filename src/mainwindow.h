#pragma once

#include "database.h"
#include "chartwidget.h"

#include <QMainWindow>
#include <QIcon>
#include <QHash>
#include <QVector>

class QLabel;
class QGridLayout;
class QComboBox;
class QPushButton;
class QStackedWidget;
class QTimer;
class QScrollArea;
class QTableWidget;
class QSystemTrayIcon;
class QCloseEvent;
class TreemapWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QString &dbPath, QWidget *parent=nullptr);
    static void applyTheme(const QString &theme);
    void persistWindowGeometry();

private slots:
    void refreshLatest();
    void refreshHistory();
    void gpuSelectionChanged();
    void diskSelectionChanged();
    void showSettings();
    void saveDiagnostics();
    void exportCurrentCsv();
    void showAbout();
    void restoreFromTray();

private:
    struct Card { QLabel *value=nullptr; QLabel *detail=nullptr; };
    Card makeCard(const QString &title, QGridLayout *grid, int row, int col, int minHeight = 120);
    QWidget *makeHistoryPage(ChartWidget **a, ChartWidget **b);
    void refreshCpuCoreCharts(qint64 from, qint64 to);
    void refreshSensors();
    void refreshProcesses();
    void refreshMemoryTrees();
    QVector<ProcInfo> sampleProcesses();
    QString sensorStateText(double tempC) const;
    QString psiLevelText(double psi) const;
    qint64 selectedSpanSeconds() const;
    void setupUi();
    void closeEvent(QCloseEvent *event) override;
    void setupTray();
    void updateTrayIcon();
    QIcon trayDigitsIcon(const QVector<double> &values) const;
    void updateDbStatus();
    QString batteryReadoutText(const SystemMetric &m) const;
    static QString fmtPercent(double v);
    static QString fmtTemp(double v);
    static QString fmtPower(double v);

    MetricsDatabase db_;
    SystemMetric latestSystem_;
    QStackedWidget *stack_=nullptr;
    QComboBox *range_=nullptr;
    QPushButton *prevBtn_=nullptr;
    QPushButton *nextBtn_=nullptr;
    QLabel *periodLabel_=nullptr;
    qint64 timeOffset_=0;
    QComboBox *gpuSelector_=nullptr;
    QLabel *updated_=nullptr;
    QLabel *dbStatus_=nullptr;
    Card cpuCard_,memCard_,netCard_,batteryCard_,pressureCard_,topProcCard_;
    QLabel *batteryReadout_=nullptr;
    QHash<QString,Card> gpuCards_;
    QWidget *gpuCardsContainer_=nullptr;
    QGridLayout *gpuCardsLayout_=nullptr;
    QHash<QString,Card> diskCards_;
    QWidget *diskCardsContainer_=nullptr;
    QGridLayout *diskCardsLayout_=nullptr;

    ChartWidget *cpuUsage_=nullptr,*cpuTemp_=nullptr,*mem_=nullptr,*swap_=nullptr,*net_=nullptr,*diskIo_=nullptr,*diskUsage_=nullptr,*battery_=nullptr,*batteryPower_=nullptr,*batteryTemp_=nullptr,*diskTemp_=nullptr,*gpuUsage_=nullptr,*gpuAux_=nullptr,*gpuMemory_=nullptr;
    QVector<FanInfo> liveFans_;
    QWidget *sensorsScroll_=nullptr;
    QGridLayout *sensorsGrid_=nullptr;
    QWidget *processesPage_=nullptr;
    QTableWidget *procTable_=nullptr;
    QHash<qint64,quint64> lastProcTicks_;
    qint64 lastProcSampleMs_=0;
    QVector<ProcInfo> latestProcs_;
    TreemapWidget *memTree_=nullptr;
    TreemapWidget *swapTree_=nullptr;
    QWidget *memPage_=nullptr;
    QComboBox *diskSelector_=nullptr;
    QLabel *cpuCoresLabel_=nullptr;
    QScrollArea *cpuCoresScroll_=nullptr;
    QGridLayout *cpuCoresLayout_=nullptr;
    QVector<ChartWidget*> cpuCoreCharts_;
    QVector<int> cpuCoreIds_;
    QTimer *timer_=nullptr;
    int latestTicks_=0;
    QSystemTrayIcon *trayIcon_=nullptr;
    bool trayAvailable_=false;
    bool forceQuit_=false;
};
