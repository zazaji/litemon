#include "mainwindow.h"
#include "linuxutils.h"
#ifdef __APPLE__
#include "macosutils.h"
#endif
#include "treemapwidget.h"
#include "appconfig.h"
#include "diagnostics.h"
#include "settingsdialog.h"

#include <QComboBox>
#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTextStream>
#include <QDateTime>
#include <QFrame>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringList>
#include <QCloseEvent>
#include <QMenu>
#include <QApplication>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QPalette>
#include <QSettings>
#include <QStandardPaths>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QSystemTrayIcon>
#include <QStorageInfo>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <cstdlib>
#include <unistd.h>
#include <functional>

using namespace LinuxUtils;

// Display-only live reads (capacities, load, battery status, sensors) that
// never get persisted: /proc on Linux, MacUtils on macOS.
namespace {
#if defined(__APPLE__)
double memTotalLive() { return MacUtils::memTotalMiB(); }
double memAvailLive() { return MacUtils::memAvailableMiB(); }
double swapTotalLive() { return MacUtils::swapTotalMiB(); }
double load1Live() { double l = 0; return getloadavg(&l, 1) == 1 ? l : lmNaN(); }
QString batteryStatusLive() { return MacUtils::readBattery().status; }
QVector<SensorInfo> readLiveSensors(QVector<FanInfo> *fans) { return MacUtils::readSensors(fans); }
QVector<FanInfo> readLiveFans() { return MacUtils::readFans(); }
// macOS has no kernel oom-killer and no queryable equivalent: empty page.
QVector<OomInfo> readOomEventsLive(int) { return {}; }
#else
double memTotalLive() { return readMemTotalMiB(); }
double memAvailLive() { return readMemAvailableMiB(); }
double swapTotalLive() { return readSwapTotalMiB(); }
double load1Live() { return parseNumber(readText("/proc/loadavg").section(' ', 0, 0)); }
QString batteryStatusLive() { return readBatteryStatus(); }
QVector<SensorInfo> readLiveSensors(QVector<FanInfo> *fans) { return readHwmonSensors(fans); }
QVector<FanInfo> readLiveFans() { QVector<FanInfo> out; readHwmonSensors(&out); return out; }
QVector<OomInfo> readOomEventsLive(int days) { return LinuxUtils::readOomEvents(days); }
// True if this user can actually read the system journal files holding kernel
// oom-killer messages. journalctl hides the failure (exit 0, empty output),
// so probe the files directly: group membership (systemd-journal/adm/wheel)
// and per-user ACLs both end up as ordinary file permission.
bool oomJournalReadable() {
    QByteArray machineId;
    QFile mid(QStringLiteral("/etc/machine-id"));
    if (mid.open(QIODevice::ReadOnly)) {
        machineId = mid.readAll().trimmed();
    } else {
        mid.setFileName(QStringLiteral("/var/lib/dbus/machine-id"));
        if (mid.open(QIODevice::ReadOnly)) machineId = mid.readAll().trimmed();
    }
    const QString live = QString::fromLatin1(machineId);
    for (const QString &root : {QStringLiteral("/run/log/journal"), QStringLiteral("/var/log/journal")}) {
        QDir base(root);
        QStringList ids = base.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (!live.isEmpty()) {
            ids.removeAll(live);
            ids.prepend(live);  // the dir journalctl will actually use goes first
        }
        for (const QString &id : ids) {
            QDir d(base.filePath(id));
            const QStringList files = d.entryList({QStringLiteral("system*.journal")}, QDir::Files);
            for (const QString &f : files) {
                QFile probe(d.filePath(f));
                if (probe.open(QIODevice::ReadOnly)) { probe.close(); return true; }
            }
        }
    }
    return false;  // no readable system journal -> nothing the page could show
}
#endif
} // namespace

// Live /proc process sampling cadence (overview top-process card + process
// table). Everything the GUI measures itself stays at minute-level granularity.
constexpr qint64 kLiveProcSampleMs = 60000;

// Processes page shows the top-100 rows only, ranked by CPU.
constexpr int kProcRowsMax = 100;

// Table cell that sorts numerically while showing a formatted string.
class NumericItem : public QTableWidgetItem {
public:
    NumericItem(const QString &text, double sortValue) : QTableWidgetItem(text), sort_(sortValue) {}
    bool operator<(const QTableWidgetItem &other) const override {
        const auto *num = dynamic_cast<const NumericItem *>(&other);
        return num ? sort_ < num->sort_ : QTableWidgetItem::operator<(other);
    }
private:
    double sort_;
};

static QLabel *titleLabel(const QString &text){auto*l=new QLabel(text);QFont f=l->font();f.setPointSizeF(f.pointSizeF()+6);f.setBold(true);l->setFont(f);return l;}
static QLabel *sectionLabel(const QString &text){auto*l=new QLabel(text);QFont f=l->font();f.setPointSizeF(f.pointSizeF()+2);f.setBold(true);l->setFont(f);return l;}

MainWindow::MainWindow(const QString &dbPath, QWidget *parent) : QMainWindow(parent), db_(dbPath,"litemon-gui") {
    QString error; if(!db_.open(&error)){setWindowTitle("LiteMon - database error");}
    setupUi();
    // Restore the window geometry from the previous session; fall back to
    // the built-in default on first run.
    {
        QSettings s(AppConfig::configPath(), QSettings::IniFormat);
        if (!restoreGeometry(s.value("ui/window_geometry").toByteArray())) resize(1180, 760);
    }
    timer_=new QTimer(this); connect(timer_,&QTimer::timeout,this,&MainWindow::refreshLatest); timer_->start(2000);
    refreshLatest(); refreshHistory();
}

void MainWindow::persistWindowGeometry() {
    QSettings s(AppConfig::configPath(), QSettings::IniFormat);
    s.setValue("ui/window_geometry", saveGeometry());
    s.sync();
}

MainWindow::Card MainWindow::makeCard(const QString &title,QGridLayout *grid,int row,int col,int minHeight){
    auto*f=new QFrame;
    f->setObjectName(QStringLiteral("LiteMonCard"));
    f->setFrameShape(QFrame::NoFrame);
    f->setStyleSheet(QStringLiteral("QFrame#LiteMonCard { background: palette(base); border: 1px solid palette(mid); border-radius: 12px; }"));
    f->setMinimumHeight(minHeight);
    f->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    const bool compact = minHeight < 120; // sensor cards: half width, 2/3 height
    auto*l=new QVBoxLayout(f);
    l->setContentsMargins(compact ? 10 : 16, compact ? 8 : 14, compact ? 10 : 16, compact ? 8 : 14);
    l->setSpacing(compact ? 2 : 4);
    auto*t=new QLabel(title);
    t->setStyleSheet(QStringLiteral("color: palette(placeholder-text); font-weight: 600;"));
    t->setMinimumWidth(0); // allow grid to constrain width
    t->setTextFormat(Qt::PlainText);
    t->setWordWrap(false);
    auto*v=new QLabel(QStringLiteral("—"));
    QFont vf=v->font();vf.setPointSizeF(vf.pointSizeF()+(compact?6:10));vf.setBold(true);v->setFont(vf);
    auto*d=new QLabel(tr("Waiting for collector…"));
    d->setWordWrap(true);
    d->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    l->addWidget(t);l->addWidget(v);l->addWidget(d);l->addStretch(1);
    grid->addWidget(f,row,col);
    return{v,d,f};
}

// Appearance switch: palette switching, including a built-in dark palette.
// The original platform palette is captured once for "system" restore.
void MainWindow::applyTheme(const QString &theme) {
    auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!app) return;
    static QPalette platformPalette = app->palette();
    static QString platformStyle = QApplication::style()->objectName();
    // Forcing light/dark also switches to Fusion: desktop styles like Breeze
    // draw menus and status bars from KColorScheme and ignore QPalette there.
    // Fusion restyles everything from the palette.
    if (theme == QStringLiteral("dark")) {
        app->setStyle(QStyleFactory::create("Fusion"));
        QPalette pal;
        pal.setColor(QPalette::Window, 0x2b2f33);
        pal.setColor(QPalette::WindowText, 0xf3f4f5);
        pal.setColor(QPalette::Base, 0x24282c);
        pal.setColor(QPalette::AlternateBase, 0x2b2f33);
        pal.setColor(QPalette::ToolTipBase, 0x24282c);
        pal.setColor(QPalette::ToolTipText, 0xf3f4f5);
        pal.setColor(QPalette::Text, 0xf3f4f5);
        pal.setColor(QPalette::Button, 0x2b2f33);
        pal.setColor(QPalette::ButtonText, 0xf3f4f5);
        pal.setColor(QPalette::BrightText, 0xff5555);
        pal.setColor(QPalette::Link, 0x74b0e8);
        pal.setColor(QPalette::Highlight, 0x3daee2);
        pal.setColor(QPalette::HighlightedText, 0x212427);
        pal.setColor(QPalette::PlaceholderText, 0x878d93);
        pal.setColor(QPalette::Light, 0x34383c);
        pal.setColor(QPalette::Midlight, 0x3b4045);
        pal.setColor(QPalette::Mid, 0x4d5359);
        pal.setColor(QPalette::Dark, 0x1e2124);
        pal.setColor(QPalette::Shadow, 0x101214);
        app->setPalette(pal);
    } else if (theme == QStringLiteral("light")) {
        app->setStyle(QStyleFactory::create("Fusion"));
        app->setPalette(app->style()->standardPalette());
    } else {
        // system: restore whatever the desktop chose
        if (QStyle *s = QStyleFactory::create(platformStyle)) { app->setStyle(s); }
        app->setPalette(platformPalette);
    }
}

QWidget *MainWindow::makeHistoryPage(ChartWidget **a,ChartWidget **b){
    auto*w=new QWidget;
    auto*l=new QVBoxLayout(w);
    l->setContentsMargins(20,10,20,12);
    l->setSpacing(8);
    *a=new ChartWidget;
    *b=new ChartWidget;
    (*a)->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    (*b)->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    l->addWidget(*a,1);
    l->addWidget(*b,1);
    return w;
}

void MainWindow::setupUi(){
    resize(1180,760);setMinimumSize(900,600);setWindowTitle("LiteMon — Local Linux Monitor");
    // The collector is a systemd user unit on Linux; without a user bus
    // (macOS/Windows builds) there is nothing to pause/resume from the UI.
    const QString runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    hasSystemd_ = !QStandardPaths::findExecutable(QStringLiteral("systemctl")).isEmpty()
        && !runtimeDir.isEmpty()
        && QFileInfo::exists(runtimeDir + QStringLiteral("/systemd"));
    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    auto *exportAction = fileMenu->addAction(tr("Export current range as CSV…"));
    auto *diagAction = fileMenu->addAction(tr("Save diagnostics…"));
    collectMenuAction_ = fileMenu->addAction(tr("Pause data collection"));
    connect(collectMenuAction_, &QAction::triggered, this, &MainWindow::toggleCollection);
    collectMenuAction_->setVisible(hasSystemd_);
    fileMenu->addSeparator();
    auto *quitAction = fileMenu->addAction(tr("Quit"));
    auto *editMenu = menuBar()->addMenu(tr("&Edit"));
    auto *settingsAction = editMenu->addAction(tr("Settings…"));
    auto *helpMenu = menuBar()->addMenu(tr("&Help"));
    auto *aboutAction = helpMenu->addAction(tr("About LiteMon"));
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportCurrentCsv);
    connect(diagAction, &QAction::triggered, this, &MainWindow::saveDiagnostics);
    connect(quitAction, &QAction::triggered, this, &MainWindow::quitApp);
    connect(settingsAction, &QAction::triggered, this, &MainWindow::showSettings);
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
    auto*central=new QWidget;auto*root=new QHBoxLayout(central);root->setContentsMargins(0,0,0,0);root->setSpacing(0);setCentralWidget(central);
    dbStatus_=new QLabel(statusBar());
    dbStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dbStatus_->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    statusBar()->addPermanentWidget(dbStatus_,1);
    updated_=new QLabel(tr("No data"));
    updated_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    updated_->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    // No stretch: hugs the far right edge of the status bar.
    statusBar()->addPermanentWidget(updated_,0);
    auto*side=new QWidget;side->setFixedWidth(196);side->setAutoFillBackground(true);
    side->setStyleSheet(QStringLiteral("background: palette(alternate-base);"));
    auto*sl=new QVBoxLayout(side);sl->setContentsMargins(14,12,14,12);sl->setSpacing(6);
    auto*brand=titleLabel(QStringLiteral("LiteMon"));sl->addWidget(brand);
    auto*nav=new QListWidget;
    nav_=nav;
    // OOM page exists only where the journal it reads is actually readable:
    // never on macOS (no oom-killer/oomd), and on Linux only when this user
    // can open the system journal files (permissions are checked once here —
    // grant the group afterwards and restart the GUI to get the page).
#ifndef __APPLE__
    const bool haveOomPage = oomJournalReadable();
#else
    const bool haveOomPage = false;
#endif
    QStringList navItems{tr("Overview"),tr("CPU"),tr("Memory"),tr("Network"),tr("Disk"),tr("GPU"),tr("Battery"),tr("Sensors"),tr("Processes")};
    if (haveOomPage) navItems << tr("OOM");
    nav->addItems(navItems);
    nav->setCurrentRow(0);
    nav->setFrameShape(QFrame::NoFrame);
    nav->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; border: none; outline: none; }"
        "QListWidget::item { padding: 9px 12px; border-radius: 8px; margin: 1px 2px; }"
        "QListWidget::item:selected { background: palette(highlight); color: palette(highlighted-text); }"
        "QListWidget::item:hover:!selected { background: palette(base); }"));
    auto*navScroll=new QScrollArea;navScroll->setWidget(nav);navScroll->setWidgetResizable(true);navScroll->setFrameShape(QFrame::NoFrame);navScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navScroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));
    sl->addWidget(navScroll,1);
    auto*historyLabel=sectionLabel(tr("History"));sl->addWidget(historyLabel);
    auto*navBar=new QHBoxLayout;navBar->setSpacing(4);
    prevBtn_=new QPushButton(QStringLiteral("\u25C0"));prevBtn_->setToolTip(tr("Previous period"));
    nextBtn_=new QPushButton(QStringLiteral("\u25B6"));nextBtn_->setToolTip(tr("Next period"));
    nextBtn_->setEnabled(false);
    navBar->addWidget(prevBtn_);navBar->addStretch(1);navBar->addWidget(nextBtn_);
    sl->addLayout(navBar);
    periodLabel_=new QLabel;
    periodLabel_->setAlignment(Qt::AlignCenter);
    periodLabel_->setStyleSheet(QStringLiteral("font-size:11px;color:palette(mid);"));
    sl->addWidget(periodLabel_);
    range_=new QComboBox;
    range_->addItem(tr("1 hour"),3600);range_->addItem(tr("6 hours"),21600);range_->addItem(tr("24 hours"),86400);range_->addItem(tr("7 days"),604800);range_->addItem(tr("30 days"),2592000);range_->addItem(tr("1 year"),31536000);range_->setCurrentIndex(2);
    sl->addWidget(range_);
    root->addWidget(side);
    auto*right=new QWidget;auto*rl=new QVBoxLayout(right);rl->setContentsMargins(14,6,14,8);rl->setSpacing(6);
    stack_=new QStackedWidget;rl->addWidget(stack_,1);root->addWidget(right,1);

    auto*overviewScroll=new QScrollArea;overviewScroll->setWidgetResizable(true);overviewScroll->setFrameShape(QFrame::NoFrame);
    auto*overview=new QWidget;auto*ov=new QVBoxLayout(overview);ov->setContentsMargins(20,10,20,12);ov->setSpacing(8);
    // Banner shown on the overview whenever the collector stopped reporting;
    // offers a one-click resume.
    collectBanner_=new QWidget;collectBanner_->setObjectName(QStringLiteral("collectBanner"));
    auto*bannerLay=new QHBoxLayout(collectBanner_);bannerLay->setContentsMargins(12,8,12,8);
    bannerLay->addWidget(new QLabel(tr("Data collection is not running")));
    bannerLay->addStretch(1);
    auto*resumeBtn=new QPushButton(tr("Resume collection"));
    bannerLay->addWidget(resumeBtn);
    connect(resumeBtn,&QPushButton::clicked,this,&MainWindow::toggleCollection);
    collectBanner_->setStyleSheet(QStringLiteral(
        "QWidget#collectBanner { background: palette(alternate-base); border: 1px solid palette(mid); border-radius: 8px; }"));
    collectBanner_->setVisible(false);
    ov->addWidget(collectBanner_);
    auto*grid=new QGridLayout;grid->setSpacing(8);grid->setContentsMargins(0,2,0,0);
    grid->setColumnStretch(0,1);grid->setColumnStretch(1,1);grid->setColumnStretch(2,1);
    cpuCard_=makeCard(tr("CPU"),grid,0,0);memCard_=makeCard(tr("Memory"),grid,0,1);netCard_=makeCard(tr("Network"),grid,0,2);batteryCard_=makeCard(tr("Battery"),grid,1,0);
#ifdef __APPLE__
    // macOS has no kernel PSI: no Pressure card, top-process moves into its slot.
    topProcCard_=makeCard(tr("Top process"),grid,1,1);
    fansCard_=makeCard(tr("Fans"),grid,1,2);
#else
    grid->setColumnStretch(3,1);
    pressureCard_=makeCard(tr("Pressure"),grid,1,1);topProcCard_=makeCard(tr("Top process"),grid,1,2);fansCard_=makeCard(tr("Fans"),grid,1,3);
#endif
    overviewGrid_=grid;
    relayoutOverviewAuxRow();
    ov->addLayout(grid);
    diskCardsContainer_=new QWidget;diskCardsLayout_=new QGridLayout(diskCardsContainer_);
    diskCardsLayout_->setSpacing(8);diskCardsLayout_->setContentsMargins(0,0,0,0);
    diskCardsLayout_->setColumnStretch(0,1);diskCardsLayout_->setColumnStretch(1,1);diskCardsLayout_->setColumnStretch(2,1);
    ov->addWidget(diskCardsContainer_);
    gpuCardsContainer_=new QWidget;gpuCardsLayout_=new QGridLayout(gpuCardsContainer_);
    gpuCardsLayout_->setSpacing(8);gpuCardsLayout_->setContentsMargins(0,0,0,0);
    gpuCardsLayout_->setColumnStretch(0,1);gpuCardsLayout_->setColumnStretch(1,1);gpuCardsLayout_->setColumnStretch(2,1);
    ov->addWidget(gpuCardsContainer_);ov->addStretch(1);
    overviewScroll->setWidget(overview);stack_->addWidget(overviewScroll);

    auto*cpuPage=new QWidget;auto*cpuLay=new QVBoxLayout(cpuPage);cpuLay->setContentsMargins(20,10,20,12);cpuLay->setSpacing(8);
    cpuUsage_=new ChartWidget;cpuUsage_->setTitle("CPU utilization","%");cpuUsage_->setFixedYRange(0,100);cpuUsage_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);cpuUsage_->setMinimumHeight(120);cpuLay->addWidget(cpuUsage_,1);
    cpuTemp_=new ChartWidget;cpuTemp_->setTitle("CPU temperature","°C");cpuTemp_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);cpuTemp_->setMinimumHeight(120);cpuLay->addWidget(cpuTemp_,1);
    cpuCoresLabel_=sectionLabel(tr("Per-core utilization"));cpuLay->addWidget(cpuCoresLabel_);
    cpuCoresScroll_=new QScrollArea;cpuCoresScroll_->setWidgetResizable(true);cpuCoresScroll_->setFrameShape(QFrame::NoFrame);
    auto*coresHost=new QWidget;cpuCoresLayout_=new QGridLayout(coresHost);cpuCoresLayout_->setSpacing(6);cpuCoresLayout_->setContentsMargins(0,0,0,0);
    cpuCoresLayout_->setColumnStretch(0,1);cpuCoresLayout_->setColumnStretch(1,1);cpuCoresLayout_->setColumnStretch(2,1);cpuCoresLayout_->setColumnStretch(3,1);
    cpuCoresScroll_->setWidget(coresHost);cpuLay->addWidget(cpuCoresScroll_,2);stack_->addWidget(cpuPage);
    auto*mem=makeHistoryPage(&mem_,&swap_);mem_->setTitle("Memory usage","%");mem_->setFixedYRange(0,100);swap_->setTitle("Swap usage","%");swap_->setFixedYRange(0,100);
    // Per-process treemaps under the history charts: RAM left, swap right.
    auto*treeRow=new QWidget;auto*treeLay=new QHBoxLayout(treeRow);
    treeLay->setContentsMargins(0,0,0,0);treeLay->setSpacing(8);
    memTree_=new TreemapWidget;memTree_->setTitle(tr("Memory by process"));
    swapTree_=new TreemapWidget;swapTree_->setTitle(tr("Swap by process"));
    treeLay->addWidget(memTree_,1);treeLay->addWidget(swapTree_,1);
#ifdef __APPLE__
    // macOS: memory pressure is already visible as the free+cached pools in
    // the memory chart, and swap never fills under normal load — a swap view
    // that stays at zero is pure noise, so don't show it.
    swap_->hide();swapTree_->hide();
#endif
    static_cast<QVBoxLayout*>(mem->layout())->addWidget(treeRow,1);
    memPage_=mem;
    stack_->addWidget(mem);
    auto*netPage=makeHistoryPage(&net_,&diskIo_);net_->setTitle("Network throughput","MiB/s");diskIo_->setTitle("Disk I/O throughput","MiB/s");stack_->addWidget(netPage);
    auto*diskPage=new QWidget;auto*diskLay=new QVBoxLayout(diskPage);diskLay->setContentsMargins(20,10,20,12);diskLay->setSpacing(8);
    auto*diskBar=new QHBoxLayout;diskBar->addWidget(new QLabel(tr("Device:")));diskSelector_=new QComboBox;diskSelector_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);diskBar->addWidget(diskSelector_,1);diskLay->addLayout(diskBar);
    diskUsage_=new ChartWidget;diskUsage_->setTitle("Filesystem usage","%");diskUsage_->setFixedYRange(0,100);diskUsage_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);diskUsage_->setMinimumHeight(120);diskLay->addWidget(diskUsage_,1);
    diskTemp_=new ChartWidget;diskTemp_->setTitle("Device temperature (NVMe)","°C");diskTemp_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);diskTemp_->setMinimumHeight(120);diskLay->addWidget(diskTemp_,1);
    // Per-device I/O is not collected; disk counters are system-wide totals
    // (/proc/diskstats, macOS IOBlockStorageDriver), so label it accordingly.
    diskIoPage_=new ChartWidget;diskIoPage_->setTitle("Disk I/O (all devices)","MiB/s");diskIoPage_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);diskIoPage_->setMinimumHeight(120);diskLay->addWidget(diskIoPage_,1);stack_->addWidget(diskPage);
    auto*gpuPage=makeHistoryPage(&gpuUsage_,&gpuAux_);auto*gplay=static_cast<QVBoxLayout*>(gpuPage->layout());
    auto*gpuBar=new QHBoxLayout;gpuBar->addWidget(new QLabel(tr("Device:")));gpuSelector_=new QComboBox;gpuSelector_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);gpuBar->addWidget(gpuSelector_,1);gplay->insertLayout(0,gpuBar);
    gpuUsage_->setTitle("GPU utilization / memory","%");gpuUsage_->setFixedYRange(0,100);gpuAux_->setTitle("Temperature","°C");gpuPower_=new ChartWidget;gpuPower_->setTitle("Power (discrete GPUs)","W");gpuPower_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
#ifdef __APPLE__
    // Apple Silicon has no per-GPU power counter (unified SoC power only);
    // the chart would sit at NaN forever.
    gpuPower_->hide();
#endif
gplay->addWidget(gpuPower_,1);stack_->addWidget(gpuPage);
    auto*bat=makeHistoryPage(&battery_,&batteryPower_);battery_->setTitle("Battery level / health","%");battery_->setFixedYRange(0,110);batteryPower_->setTitle("Battery charge / discharge power","W");
    batteryReadout_=new QLabel(tr("Waiting for collector…"));
    {
        QFont rf=batteryReadout_->font();rf.setPointSizeF(rf.pointSizeF()+2);rf.setBold(true);batteryReadout_->setFont(rf);
    }
    static_cast<QVBoxLayout*>(bat->layout())->insertWidget(0,batteryReadout_);
    batteryTemp_=new ChartWidget;batteryTemp_->setTitle("Battery temperature","°C");batteryTemp_->setMinimumHeight(150);
    static_cast<QVBoxLayout*>(bat->layout())->addWidget(batteryTemp_);
    batteryPage_=bat;
    stack_->addWidget(bat);

    auto*sensorsHost=new QWidget;auto*sLay=new QVBoxLayout(sensorsHost);sLay->setContentsMargins(20,10,20,12);sLay->setSpacing(8);
    sensorsGrid_=new QGridLayout;sensorsGrid_->setSpacing(8);sensorsGrid_->setContentsMargins(0,2,0,0);
    for (int i=0;i<3;++i) sensorsGrid_->setColumnStretch(i,1);
    sLay->addLayout(sensorsGrid_);sLay->addStretch(1);
    auto*sensorsScroll=new QScrollArea;sensorsScroll->setWidgetResizable(true);sensorsScroll->setFrameShape(QFrame::NoFrame);
    sensorsScroll->setWidget(sensorsHost);sensorsScroll_=sensorsScroll;stack_->addWidget(sensorsScroll);

    auto*processesPage=new QWidget;auto*pLay=new QVBoxLayout(processesPage);pLay->setContentsMargins(20,10,20,12);pLay->setSpacing(8);
    procTable_=new QTableWidget(0,6,processesPage);
    procTable_->setHorizontalHeaderLabels({tr("PID"),tr("Name"),tr("State"),tr("CPU %"),tr("Memory"),tr("Threads")});
    procTable_->verticalHeader()->setVisible(false);
    procTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    procTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    procTable_->setSortingEnabled(true);
    procTable_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);
    procTable_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);
    procTable_->sortByColumn(3,Qt::DescendingOrder);
    pLay->addWidget(procTable_);processesPage_=processesPage;stack_->addWidget(processesPage);

if (haveOomPage) {
    // OOM kills page: parsed live from the system journal (kernel oom-killer
    // + systemd-oomd on Linux), never persisted. Only built when the journal
    // is readable (see haveOomPage above), so the empty state needs no
    // permission hint.
    auto*oomPage=new QWidget;auto*oLay=new QVBoxLayout(oomPage);oLay->setContentsMargins(20,10,20,12);oLay->setSpacing(8);
    auto*oHead=new QHBoxLayout;
    auto*oTitle=new QLabel(tr("OOM kills · last 14 days, from the system journal"));
    auto*oRefresh=new QPushButton(tr("Refresh"));
    oHead->addWidget(oTitle);oHead->addStretch(1);oHead->addWidget(oRefresh);oLay->addLayout(oHead);
    oomHint_=new QLabel;oomHint_->setWordWrap(true);oLay->addWidget(oomHint_);
    oomTable_=new QTableWidget(0,5,oomPage);
    oomTable_->setHorizontalHeaderLabels({tr("Time"),tr("Process"),tr("PID"),tr("RSS at kill"),tr("Source / detail")});
    oomTable_->verticalHeader()->setVisible(false);
    oomTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    oomTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    oomTable_->setSortingEnabled(true);
    oomTable_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);
    oomTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    oLay->addWidget(oomTable_);oomPage_=oomPage;stack_->addWidget(oomPage);
    connect(oRefresh,&QPushButton::clicked,this,&MainWindow::refreshOom);
}

    connect(nav,&QListWidget::currentRowChanged,stack_,&QStackedWidget::setCurrentIndex);connect(nav,&QListWidget::currentRowChanged,this,[this](int){refreshHistory();refreshSensors();refreshProcesses();refreshOom();refreshMemoryTrees();});connect(range_,&QComboBox::currentIndexChanged,this,[this]{ timeOffset_=0; refreshHistory(); });connect(gpuSelector_,&QComboBox::currentIndexChanged,this,&MainWindow::gpuSelectionChanged);connect(diskSelector_,&QComboBox::currentIndexChanged,this,&MainWindow::diskSelectionChanged);
    connect(prevBtn_,&QPushButton::clicked,this,[this]{ timeOffset_+=selectedSpanSeconds(); refreshHistory(); });
    connect(nextBtn_,&QPushButton::clicked,this,[this]{ timeOffset_=qMax(qint64(0),timeOffset_-selectedSpanSeconds()); refreshHistory(); });
    setupTray();
}

void MainWindow::setupTray() {
    trayAvailable_ = QSystemTrayIcon::isSystemTrayAvailable();
    // Always keep the icon object alive: without a tray host show() is a
    // harmless no-op, and the digits config still updates the icon/tooltip.
    trayIcon_ = new QSystemTrayIcon(windowIcon(), this);
    trayMenu_ = new QMenu(this);
    QAction *showAction = trayMenu_->addAction(tr("Show LiteMon"));
    connect(showAction, &QAction::triggered, this, &MainWindow::restoreFromTray);
    // Settings must stay reachable while the window is hidden; on macOS the
    // app runs as an accessory (no Dock icon, no menu bar), so the tray menu
    // is the only way in.
    QAction *settingsTrayAction = trayMenu_->addAction(tr("Settings…"));
    connect(settingsTrayAction, &QAction::triggered, this, &MainWindow::showSettings);
    collectTrayAction_ = trayMenu_->addAction(tr("Pause data collection"));
    connect(collectTrayAction_, &QAction::triggered, this, &MainWindow::toggleCollection);
    collectTrayAction_->setVisible(hasSystemd_);
    trayMenu_->addSeparator();
    QAction *quitAction = trayMenu_->addAction(tr("Quit"));
    connect(quitAction, &QAction::triggered, this, &MainWindow::quitApp);
    trayIcon_->setContextMenu(trayMenu_);
    trayIcon_->setToolTip(QStringLiteral("LiteMon"));
    connect(trayIcon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) restoreFromTray();
    });
    trayIcon_->show();
    trayBlinkTimer_ = new QTimer(this);
    trayBlinkTimer_->setInterval(1000); // one swap per second
    connect(trayBlinkTimer_, &QTimer::timeout, this, [this] {
        trayBlinkOn_ = !trayBlinkOn_;
        updateTrayIcon(); // redraws from the same config/live read path
    });
    updateTrayIcon();
}

void MainWindow::quitApp() {
    forceQuit_ = true;
    // Not close(): with the window hidden in the tray, close() never fires
    // lastWindowClosed and the event loop would keep running.
    persistWindowGeometry();
    QApplication::quit();
}

void MainWindow::restoreFromTray() {
    show();
    setWindowState(windowState() & ~Qt::WindowMinimized);
    raise();
    activateWindow();
}

void MainWindow::setCollectionStoppedUi(bool stopped){
    collectionStopped_=stopped;
    const QString text = stopped ? tr("Resume data collection") : tr("Pause data collection");
    if (collectMenuAction_) collectMenuAction_->setText(text);
    if (collectTrayAction_) collectTrayAction_->setText(text);
    if (collectBanner_) collectBanner_->setVisible(stopped && hasSystemd_);
}

void MainWindow::toggleCollection(){
    QString error;
    if (!AppConfig::setCollectorRunning(collectionStopped_, &error)) {
        QMessageBox::critical(this, tr("Data collection"), error);
        return;
    }
    refreshLatest();
}

void MainWindow::closeEvent(QCloseEvent *event) {
    // Closing the window minimizes to the tray; real exit goes through the
    // File menu or the tray menu (both set forceQuit_).
    if (!forceQuit_ && trayAvailable_ && trayIcon_) {
        persistWindowGeometry(); // session-end shutdown skips the quit path
        hide();
        event->ignore();
        return;
    }
    persistWindowGeometry(); // last-on-exit size survives restart
    event->accept();
}

// Tray icon carries raw digits only (no units): draw one to four values in the
// panel's inverse tone (Linux picks white/black from the system color scheme;
// macOS keeps white with a dark outline, since its translucent menu bar reads
// dark on most wallpapers regardless of the appearance setting). One value
// fills the canvas, two stack in rows, three or four form a 2x2 grid. A slot
// whose value reached its configured alarm threshold blinks: the glyphs and a
// red badge behind them alternate once per second (trayBlinkTimer_).
QIcon MainWindow::trayDigitsIcon(const QVector<double> &values, const QVector<bool> &alarms) const {
    QPixmap pm(128, 128);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const int count = static_cast<int>(qMin<qsizetype>(4, values.size()));
#if defined(__APPLE__)
    // Glyphs at 1.2-1.3x the Linux size; the slight canvas overflow clips
    // invisibly at the menu bar's rendered height.
    const double basePixel = count == 1 ? 149.0 : count == 2 ? 96.0 : 80.0;
#else
    const double basePixel = count == 1 ? 124.0 : count == 2 ? 80.0 : 62.0;
#endif
    const double stroke = 2.0;
    // Per-slot caps keep neighbouring glyphs from overlapping: two values read
    // better stacked full-width (128px rows) than squeezed side by side, so
    // only the 2x2 grid narrows cells to 64px.
    const double maxRowWidth = count <= 2 ? 124.0 : 62.0;
    const double maxRowHeight = count == 1 ? 110.0 : 60.0;
    // Slot centers: one value fills the canvas, two stack top and bottom,
    // three or four form a 2x2 grid (a lone third value centers on the bottom
    // row).
    const auto slotCenter = [count](int i) {
        if (count == 1) return QPointF(64.0, 64.0);
        if (count == 2) return QPointF(64.0, i == 0 ? 32.0 : 96.0);
        const double x = (i < 2 || count == 4) ? 32.0 + 64.0 * (i % 2) : 64.0;
        return QPointF(x, i < 2 ? 32.0 : 96.0);
    };
    // Single source for both paint passes: shrink the row into its cell, then
    // center it on the slot.
    const auto positionedGlyph = [&](int i) {
        const QString text = QString::number(static_cast<long long>(std::llround(values[static_cast<qsizetype>(i)])));
        QFont f = p.font();
        f.setPixelSize(static_cast<int>(basePixel));
        p.setFont(f);
        QPainterPath row;
        row.addText(0, 0, p.font(), text);
        const QRectF natural = row.boundingRect();
        const double scale = qMin(1.0, qMin(maxRowWidth / natural.width(), maxRowHeight / natural.height()));
        if (scale < 1.0) {
            f.setPixelSize(qMax(1, static_cast<int>(basePixel * scale)));
            p.setFont(f);
            row = QPainterPath();
            row.addText(0, 0, p.font(), text);
        }
        const QRectF bb = row.boundingRect();
        const QPointF c = slotCenter(i);
        row.translate(c.x() - (bb.left() + bb.right()) / 2.0, c.y() + bb.height() / 2.0 - bb.bottom());
        return row;
    };
    QPainterPath combined;
    for (int i = 0; i < count; ++i) combined.connectPath(positionedGlyph(i));
#if defined(__APPLE__)
    // macOS 26's menu bar is translucent glass tinted by the wallpaper, and
    // the appearance preference (including automatic switching) does not
    // necessarily match the tone behind the digits. Keep the glyphs white
    // with a dark outline there — legible on both menu bar tones.
    const bool darkPanel = true;
#else
    // Panel background follows the system color scheme; platforms that don't
    // report one fall back to palette lightness.
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    const bool darkPanel = scheme == Qt::ColorScheme::Dark
        || (scheme == Qt::ColorScheme::Unknown
            && QGuiApplication::palette().color(QPalette::Window).lightness() < 160);
#endif
    p.setPen(QPen(darkPanel ? QColor(0, 0, 0, 160) : QColor(255, 255, 255, 200),
                  stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(darkPanel ? Qt::white : Qt::black);
    p.drawPath(combined);
    // Alarm blink: while a slot sits at its threshold the icon swaps once per
    // second between two phases. "On": the triggered rows are repainted
    // wholesale in red (stroke and fill); a fill-only overlay would be
    // swallowed by the black outline pass. Off: a red badge behind the row
    // with the digits back in the panel tone, so the alarm stays visible on
    // either panel tint without resting on static red.
    if (!alarms.isEmpty()) {
        const QColor alarmRed(0xe0, 0x20, 0x20);
        if (!trayBlinkOn_) {
            p.setPen(Qt::NoPen);
            p.setBrush(alarmRed);
            for (int i = 0; i < count; ++i) {
                if (!alarms.value(i)) continue;
                QRectF badge = positionedGlyph(i).boundingRect().adjusted(-6, -6, 6, 6);
                badge = badge.intersected(QRectF(0, 0, pm.width(), pm.height()));
                p.drawRoundedRect(badge, 12, 12);
            }
            p.setPen(QPen(darkPanel ? QColor(0, 0, 0, 160) : QColor(255, 255, 255, 200),
                          stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(darkPanel ? Qt::white : Qt::black);
            p.drawPath(combined);
        } else {
            QPainterPath alarmed;
            for (int i = 0; i < count; ++i) {
                if (!alarms.value(i)) continue;
                alarmed.connectPath(positionedGlyph(i));
            }
            p.setPen(QPen(alarmRed, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(alarmRed);
            p.drawPath(alarmed);
        }
    }
    p.end();
    return QIcon(pm);
}

void MainWindow::updateTrayIcon() {
    if (!trayIcon_) return;
    const AppConfig c = AppConfig::load();
    const QString keys[] = {c.traySensor1, c.traySensor2, c.traySensor3, c.traySensor4};
    const double alarmFor[] = {c.trayAlarm1, c.trayAlarm2, c.trayAlarm3, c.trayAlarm4};
    QVector<double> values;
    QVector<bool> alarms;
    QStringList tooltip;
    QVector<SensorInfo> temps;
    bool hwmonRead = false;
    for (int i = 0; i < 4; ++i) {
        const QString &key = keys[i];
        if (key.isEmpty()) continue;
        const double alarm = alarmFor[i];
        const QStringList parts = key.split(QLatin1Char('|'));
        if (parts.at(0) == QLatin1String("usage")) {
            if (parts.size() != 2) continue;
            if (parts.at(1) == QLatin1String("cpu")) {
                // CPU usage comes from the newest collector sample (kept by
                // refreshLatest), not from a stored time series.
                const double cpu = latestSystem_.cpuUsage;
                if (std::isfinite(cpu)) {
                    values << cpu;
                    alarms << (alarm > 0.0 && cpu >= alarm);
                    tooltip << QStringLiteral("CPU %1 %").arg(cpu, 0, 'f', 1);
                }
                continue;
            }
            if (parts.at(1) != QLatin1String("memory")) continue;
            // Memory usage is a live-only metric: derive used% from
            // /proc/meminfo (used = total - available).
            const double totalMiB = memTotalLive();
            const double availMiB = memAvailLive();
            if (std::isfinite(totalMiB) && std::isfinite(availMiB) && totalMiB > 0.0) {
                const double pct = (totalMiB - availMiB) / totalMiB * 100.0;
                values << pct;
                alarms << (alarm > 0.0 && pct >= alarm);
                tooltip << QStringLiteral("Memory %1 %").arg(pct, 0, 'f', 1);
            }
            continue;
        }
        if (parts.at(0) == QLatin1String("diskusage")) {
            // diskusage|<mount point>: used-capacity percentage of one disk,
            // taken from the newest collector sample (kept by refreshLatest).
            if (parts.size() != 2) continue;
            const QString &mount = parts.at(1);
            for (const auto &d : latestSystem_.disks) {
                if (d.mountPoint != mount || !std::isfinite(d.usedPct)) continue;
                values << d.usedPct;
                alarms << (alarm > 0.0 && d.usedPct >= alarm);
                tooltip << QStringLiteral("Disk %1 %2 %").arg(mount, QString::number(d.usedPct, 'f', 1));
                break;
            }
            continue;
        }
        if (parts.at(0) == QLatin1String("fan")) {
            // Fan keys carry three segments: fan|chip|label.
            if (parts.size() != 3) continue;
            if (!hwmonRead) { temps = readLiveSensors(&liveFans_); hwmonRead = true; }
            for (const auto &f : liveFans_) {
                if (f.chip == parts.at(1) && f.label == parts.at(2) && std::isfinite(f.rpm)) {
                    values << f.rpm;
                    alarms << (alarm > 0.0 && f.rpm >= alarm);
                    tooltip << QStringLiteral("%1 · %2 %3 RPM").arg(f.chip, f.label).arg(f.rpm, 0, 'f', 0);
                    break;
                }
            }
            continue;
        }
        if (!hwmonRead) { temps = readLiveSensors(&liveFans_); hwmonRead = true; }
        for (const auto &s : temps) {
            if (s.chip == parts.at(0) && s.label == parts.at(1) && std::isfinite(s.tempC)) {
                values << s.tempC;
                alarms << (alarm > 0.0 && s.tempC >= alarm);
                tooltip << QStringLiteral("%1 · %2 %3 °C").arg(s.chip, s.label).arg(s.tempC, 0, 'f', 1);
                break;
            }
        }
    }
    // Blink runs only while some slot is at its alarm threshold; a newly
    // triggered alarm always opens on the red-glyph phase.
    bool anyAlarm = false;
    for (bool a : alarms) if (a) { anyAlarm = true; break; }
    if (trayBlinkTimer_) {
        if (anyAlarm) {
            if (!trayBlinkTimer_->isActive()) {
                trayBlinkOn_ = true;
                trayBlinkTimer_->start();
            }
        } else if (trayBlinkTimer_->isActive()) {
            trayBlinkTimer_->stop();
        }
    }
    if (values.isEmpty()) {
        trayIcon_->setIcon(windowIcon());
        trayIcon_->setToolTip(QStringLiteral("LiteMon"));
        for (QSystemTrayIcon *t : extraTrayIcons_) t->hide();
        return;
    }
    if (!c.traySeparate) {
        // Combined: all enabled digits share the primary tray icon.
        trayIcon_->setIcon(trayDigitsIcon(values, alarms));
        trayIcon_->setToolTip(QStringLiteral("LiteMon · %1").arg(tooltip.join(QStringLiteral(" · "))));
        for (QSystemTrayIcon *t : extraTrayIcons_) t->hide();
        return;
    }
    // Separate: one tray icon per value, each drawing a single large digit.
    // The first slot reuses trayIcon_; further slots lazily grow extraTrayIcons_.
    trayIcon_->setIcon(trayDigitsIcon(QVector<double>{values.first()}, QVector<bool>{alarms.value(0)}));
    trayIcon_->setToolTip(QStringLiteral("LiteMon · %1").arg(tooltip.at(0)));
    const int extra = static_cast<int>(values.size()) - 1;
    while (extraTrayIcons_.size() < extra) {
        auto *t = new QSystemTrayIcon(windowIcon(), this);
        t->setContextMenu(trayMenu_);
        connect(t, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) restoreFromTray();
        });
        extraTrayIcons_.append(t);
    }
    for (int i = 0; i < extraTrayIcons_.size(); ++i) {
        QSystemTrayIcon *t = extraTrayIcons_.at(i);
        if (i < extra) {
            t->setIcon(trayDigitsIcon(QVector<double>{values.at(i + 1)}, QVector<bool>{alarms.value(i + 1)}));
            t->setToolTip(QStringLiteral("LiteMon · %1").arg(tooltip.at(i + 1)));
            if (!t->isVisible()) t->show();
        } else if (t->isVisible()) {
            t->hide();
        }
    }
}

QString MainWindow::fmtPercent(double v){return std::isfinite(v)?QString::number(v,'f',0)+"%":"—";} QString MainWindow::fmtTemp(double v){return std::isfinite(v)?QString::number(v,'f',0)+" °C":"—";} QString MainWindow::fmtPower(double v){return std::isfinite(v)?QString::number(v,'f',1)+" W":"—";}

QString MainWindow::batteryReadoutText(const QString &status, const SystemMetric &m) const {
    if (status.isEmpty()) return tr("No battery detected");
    QString text = status;
    if (std::isfinite(m.batteryPowerW)) {
        // Collector signs power by battery state: positive = charging, negative = discharging.
        if (m.batteryPowerW > 0.05) text += tr(" · Charging power %1").arg(fmtPower(m.batteryPowerW));
        else if (m.batteryPowerW < -0.05) text += tr(" · Discharging power %1").arg(fmtPower(-m.batteryPowerW));
        else text += " · " + fmtPower(0.0);
    }
    if (std::isfinite(m.batteryPercent)) text += " · " + fmtPercent(m.batteryPercent);
    if (std::isfinite(m.batteryTemperatureC)) text += " · " + fmtTemp(m.batteryTemperatureC);
    return text;
}

void MainWindow::updateDbStatus(){
    // WAL mode keeps recent writes in -wal/-shm sidecars; count them so the
    // displayed size matches real disk usage.
    const QString base=db_.path();
    const qint64 bytes=QFileInfo(base).size()+QFileInfo(base+QStringLiteral("-wal")).size()+QFileInfo(base+QStringLiteral("-shm")).size();
    const double mb=static_cast<double>(bytes)/1048576.0;
    dbStatus_->setText(tr("Database: %1 · %2 MB").arg(base).arg(mb,0,'f',1));
    dbStatus_->setToolTip(base);
}

void MainWindow::refreshLatest(){
    updateDbStatus();
    if (++latestTicks_ % 5 == 0) refreshHistory();
    const auto m=db_.latestSystem();
    if(!m){updated_->setText("Waiting for collector data\n"+db_.path());setCollectionStoppedUi(true);return;}
    const auto &x=*m;latestSystem_=x;
    // A collector that stopped silently still leaves old rows behind: treat a
    // stale timestamp as "collection paused" instead of freezing the UI.
    const qint64 staleAfter=qMax<qint64>(120, 2*static_cast<qint64>(AppConfig::load().sampleIntervalSec)+60);
    setCollectionStoppedUi(QDateTime::currentSecsSinceEpoch()-x.timestamp > staleAfter);
    updated_->setText("Updated "+QDateTime::fromSecsSinceEpoch(x.timestamp).toString("yyyy-MM-dd HH:mm:ss"));
    cpuCard_.value->setText(fmtPercent(x.cpuUsage));
    // load1 is display-only (never persisted): read it live from /proc.
    const double liveLoad1 = load1Live();
    QString cpuDetail = fmtTemp(x.cpuTemperatureC)+" · Load "+(std::isfinite(liveLoad1)?QString::number(liveLoad1,'f',2):"—");
    if (!x.cpuCores.isEmpty()) { cpuDetail += " · " + QString::number(x.cpuCores.size()) + " cores"; }
    cpuCard_.detail->setText(cpuDetail);
    // Memory/swap percentages come straight from the collector sample; the
    // capacity is constant per boot and read live from /proc/meminfo.
    const double liveMemTotalMiB = memTotalLive();
    const double memUsedMiB = (std::isfinite(x.memoryUsedPct) && std::isfinite(liveMemTotalMiB))
        ? liveMemTotalMiB * x.memoryUsedPct / 100.0 : lmNaN();
    memCard_.value->setText(fmtPercent(x.memoryUsedPct));
    memCard_.detail->setText(humanBytesMiB(memUsedMiB)+" / "+humanBytesMiB(liveMemTotalMiB));
    netCard_.value->setText("↓ "+humanRate(x.networkRxMiBs));netCard_.detail->setText("↑ "+humanRate(x.networkTxMiBs));
    // battery_status is display-only (never persisted): read it live.
    const QString liveBatteryStatus = batteryStatusLive();
    batteryCard_.value->setText(fmtPercent(x.batteryPercent));batteryCard_.detail->setText((liveBatteryStatus.isEmpty()?"No battery":liveBatteryStatus)+" · "+fmtPower(x.batteryPowerW)+" · health "+fmtPercent(x.batteryHealthPercent));
    batteryReadout_->setText(batteryReadoutText(liveBatteryStatus, x));
    // No-battery machines (desktops, LXC containers) report all-NaN battery
    // metrics forever; hide the whole battery UI instead of showing empty
    // charts. Reappear as soon as a battery shows up.
    const bool sampleHasBattery = std::isfinite(x.batteryPercent) || std::isfinite(x.batteryPowerW)
        || std::isfinite(x.batteryHealthPercent) || std::isfinite(x.batteryTemperatureC)
        || !liveBatteryStatus.isEmpty();
    if (sampleHasBattery) {
        noBatterySamples_ = 0;
        if (!batteryCardVisible_) setBatteryUiVisible(true);
    } else if (batteryCardVisible_ && ++noBatterySamples_ >= 3) {
        setBatteryUiVisible(false);
    }

    // Fans card: live RPM read, display-only (never persisted).
    {
        const QVector<FanInfo> fans = readLiveFans();
        if (fans.isEmpty()) {
            fansCard_.value->setText(QStringLiteral("—"));
            fansCard_.detail->setText(tr("No fans detected"));
        } else {
            QStringList fanDetail;
            double bestRpm = lmNaN();
            for (const auto &f : fans) {
                if (!std::isfinite(f.rpm)) continue;
                fanDetail << QStringLiteral("%1 %2 RPM").arg(f.label).arg(f.rpm, 0, 'f', 0);
                if (!std::isfinite(bestRpm) || f.rpm > bestRpm) bestRpm = f.rpm;
            }
            fansCard_.value->setText(std::isfinite(bestRpm) ? QString::number(bestRpm, 'f', 0) + " RPM" : QStringLiteral("—"));
            fansCard_.detail->setText(fanDetail.join(" · "));
        }
        // LXC containers and fanless boards never report fans: drop the card
        // after a few empty reads instead of pinning a permanent "—".
        if (!fans.isEmpty()) {
            noFanSamples_ = 0;
            if (!fansCardVisible_) setFansCardVisible(true);
        } else if (fansCardVisible_ && ++noFanSamples_ >= 3) {
            setFansCardVisible(false);
        }
    }

    // NVMe temperature is absent on machines without NVMe (SATA-only, LXC):
    // hide the chart after a few NaN samples.
    if (std::isfinite(x.nvmeTemperatureC)) {
        noNvmeSamples_ = 0;
        if (!nvmeTempVisible_) setNvmeTempVisible(true);
    } else if (nvmeTempVisible_ && ++noNvmeSamples_ >= 3) {
        setNvmeTempVisible(false);
    }

    // Dynamic disk cards on overview
    while(auto*item=diskCardsLayout_->takeAt(0)){if(item->widget())item->widget()->deleteLater();delete item;}diskCards_.clear();
    int dc=0;for(const auto&d:x.disks){QString diskTitle=d.mountPoint;auto card=makeCard(diskTitle,diskCardsLayout_,dc/3,dc%3);
        // The DB stores only the usage percentage; the capacity is constant
        // per mount and read live from the filesystem.
        QStorageInfo vol(d.mountPoint);
        const double totalGiB = static_cast<double>(vol.bytesTotal())/1073741824.0;
        const double usedGiB = (std::isfinite(d.usedPct) && vol.bytesTotal()>0)
            ? static_cast<double>(vol.bytesTotal())/1073741824.0 * d.usedPct / 100.0 : lmNaN();
        card.value->setText(fmtPercent(d.usedPct));
        card.detail->setText((std::isfinite(usedGiB)?QString::number(usedGiB,'f',1):"—")+" / "+(std::isfinite(totalGiB)?QString::number(totalGiB,'f',1):"—")+" GiB");
        diskCards_[d.mountPoint]=card;++dc;}
    if(x.disks.isEmpty()){auto*l=new QLabel("No disk data yet.");l->setWordWrap(true);diskCardsLayout_->addWidget(l,0,0);}

    // Update disk selector for Disk page
    const QString curDisk=diskSelector_?diskSelector_->currentData().toString():QString();
    QStringList diskMounts;for(const auto&d:x.disks)diskMounts<<d.mountPoint;
    bool diskSame=(diskSelector_->count()==diskMounts.size());
    if(diskSame){for(int i=0;i<diskMounts.size();++i){if(diskSelector_->itemData(i).toString()!=diskMounts[i]){diskSame=false;break;}}}
    if(!diskSame){diskSelector_->blockSignals(true);diskSelector_->clear();for(const auto&mp:diskMounts)diskSelector_->addItem(mp,mp);int idx=diskSelector_->findData(curDisk);if(idx<0&&!diskMounts.isEmpty())idx=0;diskSelector_->setCurrentIndex(idx);diskSelector_->blockSignals(false);diskSelectionChanged();}

    const auto gpus=db_.latestGpus();
    while(auto*item=gpuCardsLayout_->takeAt(0)){if(item->widget())item->widget()->deleteLater();delete item;}gpuCards_.clear();
    int c=0;for(const auto&g:gpus){QString gpuTitle=g.vendor+" · "+g.name;if(gpuTitle.length()>24)gpuTitle=gpuTitle.left(21)+"…";auto card=makeCard(gpuTitle,gpuCardsLayout_,c/3,c%3);card.value->setText(g.state=="suspended"?"Suspended":fmtPercent(g.utilization));QStringList d;d<<fmtTemp(g.temperatureC)<<fmtPower(g.powerW);if(std::isfinite(g.frequencyMHz))d<<QString::number(g.frequencyMHz,'f',0)+" MHz";if(std::isfinite(g.memoryUsedPct))d<<tr("VRAM")+" "+fmtPercent(g.memoryUsedPct);else if(g.vendor=="Intel")d<<"shared memory";card.detail->setText(d.join(" · "));gpuCards_[g.id]=card;++c;}
    if(gpus.isEmpty()){auto*l=new QLabel("No GPU/NPU metrics yet. AMD: amdgpu driver; Intel: install intel-gpu-tools; NVIDIA: proprietary driver provides nvidia-smi; Huawei Ascend: install npu-smi.");l->setWordWrap(true);gpuCardsLayout_->addWidget(l,0,0);}

    const QString current=gpuSelector_->currentData().toString();
    const auto ids=db_.gpuIds(true); // hide signal-less devices (empty charts)
    bool same = (gpuSelector_->count() == ids.size());
    if (same) {
        for (int i = 0; i < ids.size(); ++i) {
            if (gpuSelector_->itemData(i).toString() != ids[i]) { same = false; break; }
        }
    }
    if (!same) {
        // Rebuild only when the device set changed so the user's selection is
        // never clobbered by the 2-second refresh. Fall back to the first
        // device when the previous selection disappeared.
        gpuSelector_->blockSignals(true);
        gpuSelector_->clear();
        for (const auto &id : ids) { gpuSelector_->addItem(db_.gpuName(id)+"  ["+id+"]",id); }
        int idx=gpuSelector_->findData(current);
        if (idx < 0 && !ids.isEmpty()) { idx = 0; }
        gpuSelector_->setCurrentIndex(idx);
        gpuSelector_->blockSignals(false);
        gpuSelectionChanged();
    }

    // Pressure overview card: kernel PSI "some" avg10 per resource.
    // PSI is display-only (never persisted): read it live from /proc/pressure.
#ifndef __APPLE__
    bool anyPsi = false; double worstPsi = -1.0; QStringList psiDetail;
    const auto psiCpu = parsePsiContent(readText("/proc/pressure/cpu"));
    const auto psiMem = parsePsiContent(readText("/proc/pressure/memory"));
    const auto psiIo = parsePsiContent(readText("/proc/pressure/io"));
    const std::pair<const char *, double> psiRes[] = {{"CPU", psiCpu.some}, {"Mem", psiMem.some}, {"IO", psiIo.some}};
    for (const auto &r : psiRes) {
        if (!std::isfinite(r.second)) continue;
        anyPsi = true;
        if (r.second > worstPsi) worstPsi = r.second;
        psiDetail << QStringLiteral("%1 %2%").arg(r.first).arg(r.second, 0, 'f', 0);
    }
    pressureCard_.value->setText(anyPsi ? psiLevelText(worstPsi) : QStringLiteral("—"));
    pressureCard_.detail->setText(anyPsi ? psiDetail.join(" · ") : tr("PSI unavailable (kernel too old or disabled)"));
#endif

    // Top process card from the live /proc sample.
    // Live /proc resampling runs at 1-minute minimum cadence: the CPU% delta
    // window becomes a 60s average, matching the minute-scale sampling the
    // rest of the app mirrors from the collector.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (lastProcSampleMs_ == 0 || nowMs - lastProcSampleMs_ >= kLiveProcSampleMs) {
        latestProcs_ = sampleProcesses();
        refreshMemoryTrees();
    }
    if (latestProcs_.isEmpty()) { topProcCard_.value->setText(QStringLiteral("—")); topProcCard_.detail->setText(tr("No process data")); }
    else {
        const auto &p = latestProcs_[0];
        topProcCard_.value->setText(p.name);
        QStringList d;
        if (std::isfinite(p.cpuPercent)) d << QStringLiteral("CPU %1%").arg(p.cpuPercent, 0, 'f', 1);
        if (std::isfinite(p.rssMiB)) d << humanBytesMiB(p.rssMiB);
        if (p.threads > 0) d << QStringLiteral("%1 threads").arg(p.threads);
        topProcCard_.detail->setText(d.isEmpty() ? tr("Measuring…") : d.join(" · "));
    }
    if (stack_->currentWidget() == sensorsScroll_) refreshSensors();
    if (stack_->currentWidget() == processesPage_) refreshProcesses();
    updateTrayIcon();
}

// The overview's second card row reflows so hidden cards (battery / fans on
// machines without them) don't leave gaps in the grid.
void MainWindow::relayoutOverviewAuxRow() {
    if (!overviewGrid_) return;
    QVector<QPair<QWidget*, bool>> items;
    items.append({batteryCard_.frame, batteryCardVisible_});
#ifdef __APPLE__
    items.append({topProcCard_.frame, true});
#else
    items.append({pressureCard_.frame, true});
    items.append({topProcCard_.frame, true});
#endif
    items.append({fansCard_.frame, fansCardVisible_});
    int col = 0;
    for (const auto &it : items) {
        if (!it.first) continue;
        overviewGrid_->removeWidget(it.first);
        it.first->setVisible(it.second);
        if (it.second) overviewGrid_->addWidget(it.first, 1, col++);
    }
}

void MainWindow::setBatteryUiVisible(bool visible) {
    batteryCardVisible_ = visible;
    if (batteryCard_.frame) batteryCard_.frame->setVisible(visible);
    if (batteryPage_) {
        batteryPage_->setVisible(visible);
        if (nav_) {
            const int row = stack_->indexOf(batteryPage_);
            if (row >= 0 && row < nav_->count()) nav_->item(row)->setHidden(!visible);
            // Leaving a hidden page selected would show a blank pane.
            if (!visible && stack_->currentWidget() == batteryPage_) nav_->setCurrentRow(0);
        }
    }
    relayoutOverviewAuxRow();
}

void MainWindow::setFansCardVisible(bool visible) {
    fansCardVisible_ = visible;
    if (fansCard_.frame) fansCard_.frame->setVisible(visible);
    relayoutOverviewAuxRow();
}

void MainWindow::setNvmeTempVisible(bool visible) {
    nvmeTempVisible_ = visible;
    if (diskTemp_) diskTemp_->setVisible(visible);
}

qint64 MainWindow::selectedSpanSeconds()const{return range_->currentData().toLongLong();}

static QVector<ChartWidget::Point> points(const QVector<SystemMetric>&h,const std::function<double(const SystemMetric&)>&f){QVector<ChartWidget::Point>o;o.reserve(h.size());for(const auto&m:h)o.push_back({m.timestamp,f(m)});return o;}

void MainWindow::refreshHistory(){
    const qint64 now=QDateTime::currentSecsSinceEpoch(),span=selectedSpanSeconds();
    const qint64 to=now-timeOffset_,from=to-span;
    const auto h=db_.systemHistory(from,to,AppConfig::load().historyTargetPoints);
    cpuUsage_->setSeries({{"Usage",points(h,[](const auto&m){return m.cpuUsage;})}});cpuTemp_->setSeries({{"Temperature",points(h,[](const auto&m){return m.cpuTemperatureC;})}});
    mem_->setSeries({{"Used",points(h,[](const auto&m){return m.memoryUsedPct;})}});swap_->setSeries({{"Used",points(h,[](const auto&m){return m.swapUsedPct;})}});
    net_->setSeries({{"Download",points(h,[](const auto&m){return m.networkRxMiBs;})},{"Upload",points(h,[](const auto&m){return m.networkTxMiBs;})}});diskIo_->setSeries({{"Read",points(h,[](const auto&m){return m.diskReadMiBs;})},{"Write",points(h,[](const auto&m){return m.diskWriteMiBs;})}});
    battery_->setSeries({{"Level",points(h,[](const auto&m){return m.batteryPercent;})},{"Health",points(h,[](const auto&m){return m.batteryHealthPercent;})}});batteryPower_->setSeries({{"Power",points(h,[](const auto&m){return m.batteryPowerW;})}});batteryTemp_->setSeries({{"Temperature",points(h,[](const auto&m){return m.batteryTemperatureC;})}});
    diskTemp_->setSeries({{"Temperature",points(h,[](const auto&m){return m.nvmeTemperatureC;})}});diskIoPage_->setSeries({{"Read",points(h,[](const auto&m){return m.diskReadMiBs;})},{"Write",points(h,[](const auto&m){return m.diskWriteMiBs;})}});
    // Hide per-core charts for large time ranges (>7 days)
    const bool showCores = span <= 604800;
    cpuCoresLabel_->setVisible(showCores);
    cpuCoresScroll_->setVisible(showCores);
    if (showCores) { refreshCpuCoreCharts(from, to); }
    // Navigation buttons: disable "next" when at present, disable "prev" when
    // no data exists before the current window.
    nextBtn_->setEnabled(timeOffset_ > 0);
    const qint64 earliestTs = db_.earliestTimestamp();
    prevBtn_->setEnabled(earliestTs > 0 && from > earliestTs);
    // Update period label
    const auto fromDt = QDateTime::fromSecsSinceEpoch(from);
    const auto toDt   = QDateTime::fromSecsSinceEpoch(to);
    QString text;
    if (fromDt.date() == toDt.date()) {
        text = fromDt.toString(QStringLiteral("HH:mm")) + QStringLiteral(" \u2014 ") + toDt.toString(QStringLiteral("HH:mm"));
    } else {
        text = fromDt.toString(QStringLiteral("MM-dd HH:mm")) + QStringLiteral(" \u2014 ") + toDt.toString(QStringLiteral("MM-dd HH:mm"));
    }
    periodLabel_->setText(text);
    gpuSelectionChanged();
    diskSelectionChanged();
}

void MainWindow::refreshCpuCoreCharts(qint64 from, qint64 to){
    // One chart per core, created separately. Rebuild the widgets only when
    // the core set changes so the per-core views never flicker or reset.
    const auto ids=db_.cpuCoreIds();
    if (ids != cpuCoreIds_) {
        cpuCoreIds_=ids;
        while (auto *item=cpuCoresLayout_->takeAt(0)) { if (item->widget()) item->widget()->deleteLater(); delete item; }
        cpuCoreCharts_.clear();
        int c=0;
        for (int core : ids) {
            auto *ch=new ChartWidget;
            ch->setTitle(QString("Core %1").arg(core),"%");
            ch->setFixedYRange(0,100);
            ch->setMinimumSize(180,140);
            ch->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            cpuCoresLayout_->addWidget(ch, c/4, c%4);
            cpuCoreCharts_.push_back(ch);
            ++c;
        }
        if (ids.isEmpty()) {
            auto *l=new QLabel(tr("No per-core CPU data yet — waiting for the collector."));
            l->setWordWrap(true);
            l->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
            cpuCoresLayout_->addWidget(l,0,0,1,4);
        }
    }
    if (cpuCoreCharts_.isEmpty()) return;
    const auto samples=db_.cpuCoreHistory(from,to,AppConfig::load().historyTargetPoints);
    QHash<int,QVector<ChartWidget::Point>> per;
    for (const auto &s : samples) { per[s.core].push_back({s.timestamp,s.utilization}); }
    for (int i=0;i<cpuCoreIds_.size();++i) {
        const int core=cpuCoreIds_[i];
        cpuCoreCharts_[i]->setSeries({{QString("Core %1").arg(core),per.value(core)}});
    }
}

QString MainWindow::sensorStateText(double tempC) const {
    // Generic tiers; per-channel hwmon trip points (tempN_max/crit) would
    // refine this but are absent on many desktop chips.
    if (!std::isfinite(tempC)) return "—";
    if (tempC >= 90.0) return tr("Critical");
    if (tempC >= 75.0) return tr("Hot");
    if (tempC >= 60.0) return tr("Warm");
    return tr("Normal");
}

QString MainWindow::psiLevelText(double psi) const {
    // Tiers for PSI "some" avg10: under ~10% of the time window stalled is
    // unremarkable; over ~30% the resource is actively a bottleneck.
    if (!std::isfinite(psi)) return "—";
    if (psi < 10.0) return tr("Low");
    if (psi < 30.0) return tr("Medium");
    return tr("High");
}

void MainWindow::refreshSensors() {
    if (!sensorsGrid_) return;
    // Full per-channel detail is read live (hwmon on Linux, SMC on macOS);
    // sensor readings are never persisted, so the sensors page must not
    // depend on stored data.
    const QVector<SensorInfo> temps = readLiveSensors(&liveFans_);
    while (auto *item = sensorsGrid_->takeAt(0)) { if (item->widget()) item->widget()->deleteLater(); delete item; }
    int i = 0;
    for (const auto &s : temps) {
        auto card = makeCard(s.chip + " · " + s.label, sensorsGrid_, i / 6, i % 6, 80);
        card.value->setText(fmtTemp(s.tempC));
        card.detail->setText(sensorStateText(s.tempC));
        ++i;
    }
    for (const auto &f : liveFans_) {
        auto card = makeCard(f.chip + " · " + f.label, sensorsGrid_, i / 6, i % 6, 80);
        card.value->setText(std::isfinite(f.rpm) ? QString::number(f.rpm, 'f', 0) + " RPM" : QStringLiteral("—"));
        card.detail->setText(tr("Cooling fan"));
        ++i;
    }
    if (i == 0) {
        auto *l = new QLabel(tr("No hwmon sensors found."));
        l->setWordWrap(true);
        sensorsGrid_->addWidget(l, 0, 0);
    }
}

QVector<ProcInfo> MainWindow::sampleProcesses() {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const double dt = lastProcSampleMs_ > 0 ? static_cast<double>(nowMs - lastProcSampleMs_) / 1000.0 : 0.0;
    QHash<qint64, quint64> cur;
    QVector<ProcInfo> out;
#ifdef __APPLE__
    // macOS: enumerate processes via sysctl; cpuNanos is cumulative CPU time
    // in nanoseconds, so the share inside the window is the ns delta.
    const auto list = MacUtils::listProcesses();
    out.reserve(list.size());
    for (const auto &mp : list) {
        const bool haveCpu = std::isfinite(mp.cpuNanos);
        if (haveCpu) cur[mp.pid] = static_cast<quint64>(mp.cpuNanos);
        ProcInfo p;
        p.pid = mp.pid;
        p.name = mp.name;
        p.state = mp.state;
        p.threads = mp.threads;
        p.rssMiB = mp.rssMiB;
        p.swapMiB = lmNaN(); // per-process swap is not exposed by the kernel
        if (dt > 0 && haveCpu) {
            const auto prev = lastProcTicks_.constFind(mp.pid);
            if (prev != lastProcTicks_.constEnd() && mp.cpuNanos >= static_cast<double>(*prev)) {
                p.cpuPercent = (mp.cpuNanos - static_cast<double>(*prev)) / 1e9 / dt * 100.0;
            }
        }
        out.push_back(p);
    }
    lastProcTicks_ = std::move(cur);
    lastProcSampleMs_ = nowMs;
#else
    QDir proc("/proc");
    proc.setNameFilters({"[0-9]*"});
    proc.setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
    // mingw does not declare sysconf; the /proc scan yields nothing on Windows.
#if defined(_SC_CLK_TCK) && defined(_SC_PAGESIZE)
    static const double clkTicks = [] { const long v = sysconf(_SC_CLK_TCK); return v > 0 ? static_cast<double>(v) : 100.0; }();
    static const double pageMiB = [] { const long v = sysconf(_SC_PAGESIZE); return v > 0 ? static_cast<double>(v) / 1048576.0 : 4.0 / 1024.0; }();
#else
    static const double clkTicks = 100.0;
    static const double pageMiB = 4.0 / 1024.0;
#endif
    out.reserve(proc.count());
    for (const auto &e : proc.entryList()) {
        const auto s = parseProcStat(readText("/proc/" + e + "/stat"));
        if (!s) continue; // process vanished between listing and read
        cur[s->pid] = s->cpuTicks;
        ProcInfo p;
        p.pid = s->pid;
        p.name = s->name;
        p.state = s->state;
        p.threads = s->threads;
        p.rssMiB = static_cast<double>(s->rssPages) * pageMiB;
        p.swapMiB = static_cast<double>(parseProcStatusSwap(readText("/proc/" + e + "/status")).value_or(0)) / 1024.0;
        if (dt > 0) {
            const auto prev = lastProcTicks_.constFind(s->pid);
            if (prev != lastProcTicks_.constEnd() && s->cpuTicks >= *prev) {
                p.cpuPercent = static_cast<double>(s->cpuTicks - *prev) / clkTicks / dt * 100.0;
            }
        }
        out.push_back(p);
    }
    lastProcTicks_ = cur;
    lastProcSampleMs_ = nowMs;
#endif
    // Composite ranking (CPU%+2)×(10MB+RSS MB), same formula as the
    // collector's persisted ranking; memory-heavy users lead at idle CPU.
    const auto score = [](const ProcInfo &p) {
        const double cpu = std::isfinite(p.cpuPercent) ? p.cpuPercent : 0.0;
        const double rss = std::isfinite(p.rssMiB) ? p.rssMiB : 0.0;
        return (cpu + 2.0) * (10.0 + rss);
    };
    std::stable_sort(out.begin(), out.end(), [&score](const ProcInfo &a, const ProcInfo &b) {
        return score(a) > score(b);
    });
    return out;
}

void MainWindow::refreshProcesses() {
    if (!procTable_ || stack_->currentWidget() != processesPage_) return;
    const int rows = qMin(kProcRowsMax, static_cast<int>(latestProcs_.size()));
    procTable_->setSortingEnabled(false);
    procTable_->setRowCount(rows);
    for (int r = 0; r < rows; ++r) {
        const auto &p = latestProcs_[r];
        procTable_->setItem(r, 0, new NumericItem(QString::number(p.pid), static_cast<double>(p.pid)));
        procTable_->setItem(r, 1, new QTableWidgetItem(p.name));
        procTable_->setItem(r, 2, new QTableWidgetItem(p.state));
        procTable_->setItem(r, 3, new NumericItem(std::isfinite(p.cpuPercent) ? QString::number(p.cpuPercent, 'f', 1) : QStringLiteral("—"),
                                                  std::isfinite(p.cpuPercent) ? p.cpuPercent : -1.0));
        procTable_->setItem(r, 4, new NumericItem(humanBytesMiB(p.rssMiB), std::isfinite(p.rssMiB) ? p.rssMiB : -1.0));
        procTable_->setItem(r, 5, new NumericItem(QString::number(p.threads), static_cast<double>(p.threads)));
    }
    procTable_->setSortingEnabled(true);
}

// Treemap blocks for the Memory page: aggregate the live sample by process
// name, keep every consumer >= 0.1% of the domain capacity as its own block,
// and fold the long tail into one muted bucket so tiny entries don't paint
// unreadable slivers. Unused capacity becomes a white "free" block (RAM
// labels it Free + cache: unaccounted RAM is free plus page cache); swap
// has no consumers when nothing is swapped.
// OOM kills table: newest first, PID column sorts numerically via its
// integer display role. Empty state doubles as the journal-permission hint.
void MainWindow::refreshOom() {
    if (!oomTable_ || !oomHint_) return;
    const QVector<OomInfo> events = readOomEventsLive(14);
    oomTable_->setSortingEnabled(false);
    oomTable_->setRowCount(0);
    for (const auto &e : events) {
        const int r = oomTable_->rowCount(); oomTable_->insertRow(r);
        oomTable_->setItem(r, 0, new QTableWidgetItem(QDateTime::fromSecsSinceEpoch(e.timestamp).toString("yyyy-MM-dd HH:mm:ss")));
        oomTable_->setItem(r, 1, new QTableWidgetItem(e.name));
        auto *pidIt = new QTableWidgetItem;
        if (e.pid > 0) pidIt->setData(Qt::DisplayRole, static_cast<qlonglong>(e.pid));
        else pidIt->setText(QStringLiteral("—"));
        oomTable_->setItem(r, 2, pidIt);
        oomTable_->setItem(r, 3, new QTableWidgetItem(std::isfinite(e.rssMiB) ? humanBytesMiB(e.rssMiB) : QStringLiteral("—")));
        QStringList src; src << e.source; if (!e.detail.isEmpty()) src << e.detail;
        oomTable_->setItem(r, 4, new QTableWidgetItem(src.join(QStringLiteral(" · "))));
    }
    oomTable_->setSortingEnabled(true);
    oomTable_->sortByColumn(0, Qt::DescendingOrder);
    if (events.isEmpty()) {
        oomHint_->setText(tr("No OOM kills in the last 14 days."));
    } else {
        oomHint_->setText(tr("%1 OOM kill event(s) in the last 14 days.").arg(events.size()));
    }
}

void MainWindow::refreshMemoryTrees() {
    if (!memTree_ || !swapTree_) return;
    struct Agg { double mem = 0.0; double swap = 0.0; };
    QHash<QString, Agg> byName;
    for (const auto &p : latestProcs_) {
        auto &a = byName[p.name];
        if (std::isfinite(p.rssMiB)) a.mem += p.rssMiB;
        if (std::isfinite(p.swapMiB)) a.swap += p.swapMiB;
    }
    auto build = [&](const QHash<QString, Agg> &agg, bool useSwap, double total, TreemapWidget *w) {
        QVector<TreemapWidget::Block> blocks;
        TreemapWidget::Block others;
        others.label = tr("Others <0.1% each");
        others.muted = true;
        const bool haveTotal = std::isfinite(total) && total > 0.0;
        if (haveTotal) {
            for (auto it = agg.constBegin(); it != agg.constEnd(); ++it) {
                const double v = useSwap ? it->swap : it->mem;
                if (!(v > 0.0)) continue;
                if (v / total < 0.001) { others.value += v; continue; }
                blocks.push_back({it.key(), v, false, false});
            }
            std::stable_sort(blocks.begin(), blocks.end(),
                [](const TreemapWidget::Block &a, const TreemapWidget::Block &b) { return a.value > b.value; });
            double used = others.value;
            for (const auto &b : blocks) used += b.value;
            if (others.value > 0.0) blocks.push_back(others); // after the used sum: counted once
            const double freeMiB = total - std::min(used, total);
            if (freeMiB > 0.0) {
                blocks.push_back({useSwap ? tr("Free") : tr("Free + cache"),
                                  freeMiB, false, true});
            }
        }
        w->setData(blocks, haveTotal ? total : 0.0);
    };
    build(byName, false, memTotalLive(), memTree_);
    // swap_total is display-only (never persisted): read it live.
    const double liveSwapTotalMiB = swapTotalLive();
    const bool swapExists = std::isfinite(liveSwapTotalMiB) && liveSwapTotalMiB > 0.0;
    swapTree_->setEmptyText(swapExists ? tr("Nothing swapped out") : tr("No swap configured"));
    build(byName, true, liveSwapTotalMiB, swapTree_);
}

void MainWindow::gpuSelectionChanged(){const QString id=gpuSelector_?gpuSelector_->currentData().toString():QString();if(id.isEmpty()){gpuUsage_->setSeries({});gpuAux_->setSeries({});gpuPower_->setSeries({});return;}const qint64 to=QDateTime::currentSecsSinceEpoch()-timeOffset_,from=to-selectedSpanSeconds();const auto h=db_.gpuHistory(id,from,to,AppConfig::load().historyTargetPoints);gpuUsage_->setSeries({{"Utilization",points(h,[](const auto&m){return m.gpus.isEmpty()?lmNaN():m.gpus[0].utilization;})},{"Memory",points(h,[](const auto&m){return m.gpus.isEmpty()?lmNaN():m.gpus[0].memoryUsedPct;})}});gpuAux_->setSeries({{"Temperature",points(h,[](const auto&m){return m.gpus.isEmpty()?lmNaN():m.gpus[0].temperatureC;})}});gpuPower_->setSeries({{"Power",points(h,[](const auto&m){return m.gpus.isEmpty()?lmNaN():m.gpus[0].powerW;})}});}

void MainWindow::diskSelectionChanged(){
    const QString mount=diskSelector_?diskSelector_->currentData().toString():QString();
    if(mount.isEmpty()){diskUsage_->setSeries({});return;}
    const qint64 to=QDateTime::currentSecsSinceEpoch()-timeOffset_,from=to-selectedSpanSeconds();
    const auto h=db_.diskHistory(mount,from,to,AppConfig::load().historyTargetPoints);
    QVector<ChartWidget::Point> usedPts; usedPts.reserve(h.size());
    for(const auto&d:h){usedPts.push_back({d.timestamp,d.usedPct});}
    diskUsage_->setSeries({{"Used",usedPts}});
}


void MainWindow::showSettings() {
    // Disk tray options come from the newest collector sample: that list is
    // already deduplicated and filtered by the collector (no CIFS statfs
    // hangs), so the dialog never enumerates mounts itself.
    QStringList diskMounts;
    for (const auto &d : latestSystem_.disks) diskMounts << d.mountPoint;
    SettingsDialog d(this, diskMounts);
    if (d.exec() == QDialog::Accepted) {
        applyTheme(AppConfig::load().theme); // appearance may have changed
        updateTrayIcon(); // tray digits config may have changed
        QMessageBox::information(this, tr("Settings saved"),
            tr("Settings were saved. Restart the LiteMon collector service for sampling and retention changes to take effect."));
    }
}

void MainWindow::saveDiagnostics() {
    const QString path = QFileDialog::getSaveFileName(this, tr("Save diagnostics"), "litemon-diagnostics.json", tr("JSON files (*.json)"));
    if (path.isEmpty()) return;
    QString error;
    if (!Diagnostics::writeReport(db_.path(), path, &error))
        QMessageBox::critical(this, tr("Diagnostics"), error);
}

void MainWindow::exportCurrentCsv() {
    const QString path = QFileDialog::getSaveFileName(this, tr("Export current range"), "litemon-history.csv", tr("CSV files (*.csv)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::critical(this, tr("Export"), f.errorString());
        return;
    }
    const qint64 to = QDateTime::currentSecsSinceEpoch() - timeOffset_;
    const qint64 from = to - selectedSpanSeconds();
    const auto h = db_.systemHistory(from, to, 10000);
    QTextStream out(&f);
    out << "timestamp,cpu_usage,cpu_temp,memory_used_pct,swap_used_pct,network_rx_mibs,network_tx_mibs,disk_read_mibs,disk_write_mibs,battery_percent,battery_power_w,battery_health_percent\n";
    for (const auto &m : h) {
        auto n=[](double v){ return std::isfinite(v) ? QString::number(v, 'g', 12) : QString(); };
        out << m.timestamp << ',' << n(m.cpuUsage) << ',' << n(m.cpuTemperatureC) << ','
            << n(m.memoryUsedPct) << ',' << n(m.swapUsedPct) << ',' << n(m.networkRxMiBs) << ',' << n(m.networkTxMiBs) << ','
            << n(m.diskReadMiBs) << ',' << n(m.diskWriteMiBs) << ','
            << n(m.batteryPercent) << ',' << n(m.batteryPowerW) << ',' << n(m.batteryHealthPercent) << '\n';
    }
}

void MainWindow::showAbout() {
    QMessageBox::about(this, tr("About LiteMon"),
        tr("<b>LiteMon 2.0</b><br>A lightweight native historical monitor for Linux desktops.<br><br>"
           "Local-first · SQLite · Qt 6 · Intel/NVIDIA aware · No cloud dependency."));
}
