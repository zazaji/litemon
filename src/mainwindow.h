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
class QAction;
class QListWidget;
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
    void toggleCollection();

private:
    struct Card { QLabel *value=nullptr; QLabel *detail=nullptr; QWidget *frame=nullptr; };
    Card makeCard(const QString &title, QGridLayout *grid, int row, int col, int minHeight = 120);
    QWidget *makeHistoryPage(ChartWidget **a, ChartWidget **b);
    void refreshCpuCoreCharts(qint64 from, qint64 to);
    void refreshSensors();
    void refreshProcesses();
    void refreshOom();
    void refreshMemoryTrees();
    QVector<ProcInfo> sampleProcesses();
    QString sensorStateText(double tempC) const;
    QString psiLevelText(double psi) const;
    qint64 selectedSpanSeconds() const;
    void setupUi();
    void closeEvent(QCloseEvent *event) override;
    void setupTray();
    void quitApp();
    void updateTrayIcon();
    QIcon trayDigitsIcon(const QVector<double> &values, const QVector<bool> &alarms) const;
    void updateDbStatus();
    // Sync the File menu, tray menu and the overview banner with whether the
    // collector is currently stopped (no fresh samples).
    void setCollectionStoppedUi(bool stopped);
    QString batteryReadoutText(const QString &status, const SystemMetric &m) const;
    // Data-driven UI hiding: machines without the hardware (LXC containers,
    // desktops without a battery) must not show permanently-empty panels.
    // A panel hides after several consecutive empty samples and comes back
    // as soon as the data appears.
    void relayoutOverviewAuxRow();
    void setBatteryUiVisible(bool visible);
    void setFansCardVisible(bool visible);
    void setNvmeTempVisible(bool visible);
    static QString fmtPercent(double v);
    static QString fmtTemp(double v);
    static QString fmtPower(double v);

    MetricsDatabase db_;
    SystemMetric latestSystem_;
    QStackedWidget *stack_=nullptr;
    QListWidget *nav_=nullptr;
    QGridLayout *overviewGrid_=nullptr;
    QWidget *batteryPage_=nullptr;
    bool batteryCardVisible_=true;
    bool fansCardVisible_=true;
    bool nvmeTempVisible_=true;
    int noBatterySamples_=0;
    int noFanSamples_=0;
    int noNvmeSamples_=0;
    QComboBox *range_=nullptr;
    QPushButton *prevBtn_=nullptr;
    QPushButton *nextBtn_=nullptr;
    QLabel *periodLabel_=nullptr;
    qint64 timeOffset_=0;
    QComboBox *gpuSelector_=nullptr;
    QLabel *updated_=nullptr;
    QLabel *dbStatus_=nullptr;
    Card cpuCard_,memCard_,netCard_,batteryCard_,pressureCard_,topProcCard_,fansCard_;
    QLabel *batteryReadout_=nullptr;
    QHash<QString,Card> gpuCards_;
    QWidget *gpuCardsContainer_=nullptr;
    QGridLayout *gpuCardsLayout_=nullptr;
    QHash<QString,Card> diskCards_;
    QWidget *diskCardsContainer_=nullptr;
    QGridLayout *diskCardsLayout_=nullptr;

    ChartWidget *cpuUsage_=nullptr,*cpuTemp_=nullptr,*mem_=nullptr,*swap_=nullptr,*net_=nullptr,*diskIo_=nullptr,*diskUsage_=nullptr,*battery_=nullptr,*batteryPower_=nullptr,*batteryTemp_=nullptr,*diskTemp_=nullptr,*diskIoPage_=nullptr,*gpuUsage_=nullptr,*gpuAux_=nullptr,*gpuPower_=nullptr;
    QVector<FanInfo> liveFans_;
    QWidget *sensorsScroll_=nullptr;
    QGridLayout *sensorsGrid_=nullptr;
    QWidget *processesPage_=nullptr;
    QTableWidget *procTable_=nullptr;
    QWidget *oomPage_=nullptr;
    QTableWidget *oomTable_=nullptr;
    QLabel *oomHint_=nullptr;
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
    QWidget *collectBanner_=nullptr;
    QAction *collectMenuAction_=nullptr;
    QAction *collectTrayAction_=nullptr;
    bool collectionStopped_=false;
    bool hasSystemd_=false;
    QSystemTrayIcon *trayIcon_=nullptr;
    // Separate-tray mode: one extra icon per enabled slot beyond the first
    // (created lazily by updateTrayIcon, hidden when slots shrink).
    QVector<QSystemTrayIcon*> extraTrayIcons_;
    // Alarm blink: while any tray slot sits at its alarm threshold the icon
    // alternates between red glyphs and a red badge behind panel-tone glyphs;
    // trayBlinkTimer_ flips trayBlinkOn_ once per second and runs only while
    // an alarm is active.
    QTimer *trayBlinkTimer_ = nullptr;
    bool trayBlinkOn_ = true;
    QMenu *trayMenu_=nullptr; // shared by every tray icon
    bool trayAvailable_=false;
    bool forceQuit_=false;
};
