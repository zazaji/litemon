#include "mainwindow.h"
#include "linuxutils.h"
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
#include <QSettings>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <unistd.h>
#include <functional>

using namespace LinuxUtils;

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
    return{v,d};
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
    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    auto *exportAction = fileMenu->addAction(tr("Export current range as CSV…"));
    auto *diagAction = fileMenu->addAction(tr("Save diagnostics…"));
    fileMenu->addSeparator();
    auto *quitAction = fileMenu->addAction(tr("Quit"));
    auto *editMenu = menuBar()->addMenu(tr("&Edit"));
    auto *settingsAction = editMenu->addAction(tr("Settings…"));
    auto *helpMenu = menuBar()->addMenu(tr("&Help"));
    auto *aboutAction = helpMenu->addAction(tr("About LiteMon"));
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportCurrentCsv);
    connect(diagAction, &QAction::triggered, this, &MainWindow::saveDiagnostics);
    connect(quitAction, &QAction::triggered, this, [this]{ forceQuit_=true; close(); });
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
    statusBar()->addPermanentWidget(updated_,1);
    auto*side=new QWidget;side->setFixedWidth(196);side->setAutoFillBackground(true);
    side->setStyleSheet(QStringLiteral("background: palette(alternate-base);"));
    auto*sl=new QVBoxLayout(side);sl->setContentsMargins(14,12,14,12);sl->setSpacing(6);
    auto*brand=titleLabel(QStringLiteral("LiteMon"));sl->addWidget(brand);
    auto*nav=new QListWidget;
    nav->addItems({tr("Overview"),tr("CPU"),tr("Memory"),tr("Network"),tr("Disk"),tr("GPU"),tr("Battery"),tr("Sensors"),tr("Processes")});
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
    auto*grid=new QGridLayout;grid->setSpacing(8);grid->setContentsMargins(0,2,0,0);
    grid->setColumnStretch(0,1);grid->setColumnStretch(1,1);grid->setColumnStretch(2,1);
    cpuCard_=makeCard(tr("CPU"),grid,0,0);memCard_=makeCard(tr("Memory"),grid,0,1);netCard_=makeCard(tr("Network"),grid,0,2);batteryCard_=makeCard(tr("Battery"),grid,1,0);
    pressureCard_=makeCard(tr("Pressure"),grid,1,1);topProcCard_=makeCard(tr("Top process"),grid,1,2);
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
    cpuUsage_=new ChartWidget;cpuUsage_->setTitle("CPU utilization","%");cpuUsage_->setFixedYRange(0,100);cpuUsage_->setMinimumHeight(150);cpuLay->addWidget(cpuUsage_);
    cpuTemp_=new ChartWidget;cpuTemp_->setTitle("CPU temperature","°C");cpuTemp_->setMinimumHeight(150);cpuLay->addWidget(cpuTemp_);
    cpuCoresLabel_=sectionLabel(tr("Per-core utilization"));cpuLay->addWidget(cpuCoresLabel_);
    cpuCoresScroll_=new QScrollArea;cpuCoresScroll_->setWidgetResizable(true);cpuCoresScroll_->setFrameShape(QFrame::NoFrame);
    auto*coresHost=new QWidget;cpuCoresLayout_=new QGridLayout(coresHost);cpuCoresLayout_->setSpacing(6);cpuCoresLayout_->setContentsMargins(0,0,0,0);
    cpuCoresLayout_->setColumnStretch(0,1);cpuCoresLayout_->setColumnStretch(1,1);cpuCoresLayout_->setColumnStretch(2,1);cpuCoresLayout_->setColumnStretch(3,1);
    cpuCoresScroll_->setWidget(coresHost);cpuLay->addWidget(cpuCoresScroll_,1);stack_->addWidget(cpuPage);
    auto*mem=makeHistoryPage(&mem_,&swap_);mem_->setTitle("Memory used","GiB");swap_->setTitle("Swap used","GiB");
    // Per-process treemaps under the history charts: RAM left, swap right.
    auto*treeRow=new QWidget;auto*treeLay=new QHBoxLayout(treeRow);
    treeLay->setContentsMargins(0,0,0,0);treeLay->setSpacing(8);
    memTree_=new TreemapWidget;memTree_->setTitle(tr("Memory by process"));
    swapTree_=new TreemapWidget;swapTree_->setTitle(tr("Swap by process"));
    treeLay->addWidget(memTree_,1);treeLay->addWidget(swapTree_,1);
    static_cast<QVBoxLayout*>(mem->layout())->addWidget(treeRow,1);
    memPage_=mem;
    stack_->addWidget(mem);
    auto*netPage=makeHistoryPage(&net_,&diskIo_);net_->setTitle("Network throughput","MiB/s");diskIo_->setTitle("Disk I/O throughput","MiB/s");stack_->addWidget(netPage);
    auto*diskPage=new QWidget;auto*diskLay=new QVBoxLayout(diskPage);diskLay->setContentsMargins(20,10,20,12);diskLay->setSpacing(8);
    auto*diskBar=new QHBoxLayout;diskBar->addWidget(new QLabel(tr("Device:")));diskSelector_=new QComboBox;diskSelector_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);diskBar->addWidget(diskSelector_,1);diskLay->addLayout(diskBar);
    diskUsage_=new ChartWidget;diskUsage_->setTitle("Filesystem usage","GiB");diskUsage_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);diskLay->addWidget(diskUsage_,1);
    diskTemp_=new ChartWidget;diskTemp_->setTitle("Device temperature (NVMe)","°C");diskTemp_->setMinimumHeight(150);diskLay->addWidget(diskTemp_);stack_->addWidget(diskPage);
    auto*gpuPage=makeHistoryPage(&gpuUsage_,&gpuAux_);auto*gplay=static_cast<QVBoxLayout*>(gpuPage->layout());
    auto*gpuBar=new QHBoxLayout;gpuBar->addWidget(new QLabel(tr("Device:")));gpuSelector_=new QComboBox;gpuSelector_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);gpuBar->addWidget(gpuSelector_,1);gplay->insertLayout(0,gpuBar);
    gpuUsage_->setTitle("GPU utilization","%");gpuUsage_->setFixedYRange(0,100);gpuAux_->setTitle("Temperature / Power","°C / W");gpuMemory_=new ChartWidget;gpuMemory_->setTitle("GPU memory (discrete GPUs)","GiB");gpuMemory_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);gplay->addWidget(gpuMemory_,1);stack_->addWidget(gpuPage);
    auto*bat=makeHistoryPage(&battery_,&batteryPower_);battery_->setTitle("Battery level / health","%");battery_->setFixedYRange(0,110);batteryPower_->setTitle("Battery charge / discharge power","W");
    batteryReadout_=new QLabel(tr("Waiting for collector…"));
    {
        QFont rf=batteryReadout_->font();rf.setPointSizeF(rf.pointSizeF()+2);rf.setBold(true);batteryReadout_->setFont(rf);
    }
    static_cast<QVBoxLayout*>(bat->layout())->insertWidget(0,batteryReadout_);
    batteryTemp_=new ChartWidget;batteryTemp_->setTitle("Battery temperature","°C");batteryTemp_->setMinimumHeight(150);
    static_cast<QVBoxLayout*>(bat->layout())->addWidget(batteryTemp_);
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

    connect(nav,&QListWidget::currentRowChanged,stack_,&QStackedWidget::setCurrentIndex);connect(nav,&QListWidget::currentRowChanged,this,[this](int){refreshHistory();refreshSensors();refreshProcesses();refreshMemoryTrees();});connect(range_,&QComboBox::currentIndexChanged,this,[this]{ timeOffset_=0; refreshHistory(); });connect(gpuSelector_,&QComboBox::currentIndexChanged,this,&MainWindow::gpuSelectionChanged);connect(diskSelector_,&QComboBox::currentIndexChanged,this,&MainWindow::diskSelectionChanged);
    connect(prevBtn_,&QPushButton::clicked,this,[this]{ timeOffset_+=selectedSpanSeconds(); refreshHistory(); });
    connect(nextBtn_,&QPushButton::clicked,this,[this]{ timeOffset_=qMax(qint64(0),timeOffset_-selectedSpanSeconds()); refreshHistory(); });
    setupTray();
}

void MainWindow::setupTray() {
    trayAvailable_ = QSystemTrayIcon::isSystemTrayAvailable();
    // Always keep the icon object alive: without a tray host show() is a
    // harmless no-op, and the digits config still updates the icon/tooltip.
    trayIcon_ = new QSystemTrayIcon(windowIcon(), this);
    auto *menu = new QMenu(this);
    QAction *showAction = menu->addAction(tr("Show LiteMon"));
    connect(showAction, &QAction::triggered, this, &MainWindow::restoreFromTray);
    menu->addSeparator();
    QAction *quitAction = menu->addAction(tr("Quit"));
    connect(quitAction, &QAction::triggered, this, [this]{ forceQuit_=true; close(); });
    trayIcon_->setContextMenu(menu);
    trayIcon_->setToolTip(QStringLiteral("LiteMon"));
    connect(trayIcon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) restoreFromTray();
    });
    trayIcon_->show();
    updateTrayIcon();
}

void MainWindow::restoreFromTray() {
    show();
    setWindowState(windowState() & ~Qt::WindowMinimized);
    raise();
    activateWindow();
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

// Tray icon carries raw digits only (no units): draw the values with a dark
// outline + white fill so they stay legible on both dark and light panels.
QIcon MainWindow::trayDigitsIcon(const QVector<double> &values) const {
    QPixmap pm(128, 128);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const int count = static_cast<int>(qMin<qsizetype>(2, values.size()));
    const double basePixel = count == 1 ? 112.0 : 56.0;
    double stroke = 2.0;
    QPainterPath combined;
    for (int i = 0; i < count; ++i) {
        const QString text = QString::number(static_cast<long long>(std::llround(values[static_cast<qsizetype>(i)])));
        const double band = 128.0 / count;
        const double center = band * (count == 1 ? 0.5 : i + 0.5);
        QFont f = p.font();
        f.setPixelSize(static_cast<int>(basePixel));
        p.setFont(f);
        QPainterPath row;
        row.addText(0, 0, p.font(), text);
        double pixel = basePixel;
        if (row.boundingRect().width() > 124.0) { // shrink until even 4 digits fit
            pixel = basePixel * 124.0 / row.boundingRect().width();
            f.setPixelSize(qMax(1, static_cast<int>(pixel)));
            p.setFont(f);
            row = QPainterPath();
            row.addText(0, 0, p.font(), text);
        }
        const QRectF bb = row.boundingRect();
        row.translate(64.0 - (bb.left() + bb.right()) / 2.0, center + bb.height() / 2.0 - bb.bottom());
        combined.connectPath(row);
        stroke = qMax(stroke, pixel * 0.055); // outline proportional to glyphs
    }
    p.setPen(QPen(Qt::black, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::white);
    p.drawPath(combined);
    p.end();
    return QIcon(pm);
}

void MainWindow::updateTrayIcon() {
    if (!trayIcon_) return;
    const AppConfig c = AppConfig::load();
    const QString keys[] = {c.traySensor1, c.traySensor2};
    QVector<double> values;
    QStringList tooltip;
    if (!c.traySensor1.isEmpty()) {
        const QVector<SensorInfo> temps = LinuxUtils::readHwmonSensors(&liveFans_);
        for (const QString &key : keys) {
            if (key.isEmpty()) continue;
            const QStringList parts = key.split(QLatin1Char('|'));
            if (parts.size() != 2) continue;
            for (const auto &s : temps) {
                if (s.chip == parts.at(0) && s.label == parts.at(1) && std::isfinite(s.tempC)) {
                    values << s.tempC;
                    tooltip << QStringLiteral("%1 · %2 %3 °C").arg(s.chip, s.label).arg(s.tempC, 0, 'f', 1);
                    break;
                }
            }
        }
    }
    if (values.isEmpty()) {
        trayIcon_->setIcon(windowIcon());
        trayIcon_->setToolTip(QStringLiteral("LiteMon"));
        return;
    }
    trayIcon_->setIcon(trayDigitsIcon(values));
    trayIcon_->setToolTip(QStringLiteral("LiteMon · %1").arg(tooltip.join(QStringLiteral(" · "))));
}

QString MainWindow::fmtPercent(double v){return std::isfinite(v)?QString::number(v,'f',0)+"%":"—";} QString MainWindow::fmtTemp(double v){return std::isfinite(v)?QString::number(v,'f',0)+" °C":"—";} QString MainWindow::fmtPower(double v){return std::isfinite(v)?QString::number(v,'f',1)+" W":"—";}

QString MainWindow::batteryReadoutText(const SystemMetric &m) const {
    if (m.batteryStatus.isEmpty()) return tr("No battery detected");
    QString text = m.batteryStatus;
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
    const auto m=db_.latestSystem();if(!m){updated_->setText("Waiting for collector data\n"+db_.path());return;}const auto &x=*m;latestSystem_=x;updated_->setText("Updated "+QDateTime::fromSecsSinceEpoch(x.timestamp).toString("yyyy-MM-dd HH:mm:ss"));
    cpuCard_.value->setText(fmtPercent(x.cpuUsage));
    QString cpuDetail = fmtTemp(x.cpuTemperatureC)+" · Load "+(std::isfinite(x.load1)?QString::number(x.load1,'f',2):"—");
    if (!x.cpuCores.isEmpty()) { cpuDetail += " · " + QString::number(x.cpuCores.size()) + " cores"; }
    cpuCard_.detail->setText(cpuDetail);
    const double memPct=(std::isfinite(x.memoryUsedMiB)&&x.memoryTotalMiB>0)?x.memoryUsedMiB/x.memoryTotalMiB*100:lmNaN();memCard_.value->setText(fmtPercent(memPct));memCard_.detail->setText(humanBytesMiB(x.memoryUsedMiB)+" / "+humanBytesMiB(x.memoryTotalMiB));
    netCard_.value->setText("↓ "+humanRate(x.networkRxMiBs));netCard_.detail->setText("↑ "+humanRate(x.networkTxMiBs));
    batteryCard_.value->setText(fmtPercent(x.batteryPercent));batteryCard_.detail->setText((x.batteryStatus.isEmpty()?"No battery":x.batteryStatus)+" · "+fmtPower(x.batteryPowerW)+" · health "+fmtPercent(x.batteryHealthPercent));
    batteryReadout_->setText(batteryReadoutText(x));

    // Dynamic disk cards on overview
    while(auto*item=diskCardsLayout_->takeAt(0)){if(item->widget())item->widget()->deleteLater();delete item;}diskCards_.clear();
    int dc=0;for(const auto&d:x.disks){QString diskTitle=d.mountPoint;auto card=makeCard(diskTitle,diskCardsLayout_,dc/3,dc%3);const double pct=(std::isfinite(d.usedGiB)&&d.totalGiB>0)?d.usedGiB/d.totalGiB*100:lmNaN();card.value->setText(fmtPercent(pct));card.detail->setText((std::isfinite(d.usedGiB)?QString::number(d.usedGiB,'f',1):"—")+" / "+(std::isfinite(d.totalGiB)?QString::number(d.totalGiB,'f',1):"—")+" GiB");diskCards_[d.mountPoint]=card;++dc;}
    if(x.disks.isEmpty()){auto*l=new QLabel("No disk data yet.");l->setWordWrap(true);diskCardsLayout_->addWidget(l,0,0);}

    // Update disk selector for Disk page
    const QString curDisk=diskSelector_?diskSelector_->currentData().toString():QString();
    QStringList diskMounts;for(const auto&d:x.disks)diskMounts<<d.mountPoint;
    bool diskSame=(diskSelector_->count()==diskMounts.size());
    if(diskSame){for(int i=0;i<diskMounts.size();++i){if(diskSelector_->itemData(i).toString()!=diskMounts[i]){diskSame=false;break;}}}
    if(!diskSame){diskSelector_->blockSignals(true);diskSelector_->clear();for(const auto&mp:diskMounts)diskSelector_->addItem(mp,mp);int idx=diskSelector_->findData(curDisk);if(idx<0&&!diskMounts.isEmpty())idx=0;diskSelector_->setCurrentIndex(idx);diskSelector_->blockSignals(false);diskSelectionChanged();}

    const auto gpus=db_.latestGpus();
    while(auto*item=gpuCardsLayout_->takeAt(0)){if(item->widget())item->widget()->deleteLater();delete item;}gpuCards_.clear();
    int c=0;for(const auto&g:gpus){QString gpuTitle=g.vendor+" · "+g.name;if(gpuTitle.length()>24)gpuTitle=gpuTitle.left(21)+"…";auto card=makeCard(gpuTitle,gpuCardsLayout_,c/3,c%3);card.value->setText(g.state=="suspended"?"Suspended":fmtPercent(g.utilization));QStringList d;d<<fmtTemp(g.temperatureC)<<fmtPower(g.powerW);if(std::isfinite(g.frequencyMHz))d<<QString::number(g.frequencyMHz,'f',0)+" MHz";if(std::isfinite(g.memoryTotalMiB)&&g.memoryTotalMiB>0)d<<humanBytesMiB(g.memoryUsedMiB)+" / "+humanBytesMiB(g.memoryTotalMiB);else if(g.vendor=="Intel")d<<"shared memory";card.detail->setText(d.join(" · "));gpuCards_[g.id]=card;++c;}
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
    bool anyPsi = false; double worstPsi = -1.0; QStringList psiDetail;
    const std::pair<const char *, double> psiRes[] = {{"CPU", x.psiCpuSome}, {"Mem", x.psiMemSome}, {"IO", x.psiIoSome}};
    for (const auto &r : psiRes) {
        if (!std::isfinite(r.second)) continue;
        anyPsi = true;
        if (r.second > worstPsi) worstPsi = r.second;
        psiDetail << QStringLiteral("%1 %2%").arg(r.first).arg(r.second, 0, 'f', 0);
    }
    pressureCard_.value->setText(anyPsi ? psiLevelText(worstPsi) : QStringLiteral("—"));
    pressureCard_.detail->setText(anyPsi ? psiDetail.join(" · ") : tr("PSI unavailable (kernel too old or disabled)"));

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

qint64 MainWindow::selectedSpanSeconds()const{return range_->currentData().toLongLong();}

static QVector<ChartWidget::Point> points(const QVector<SystemMetric>&h,const std::function<double(const SystemMetric&)>&f){QVector<ChartWidget::Point>o;o.reserve(h.size());for(const auto&m:h)o.push_back({m.timestamp,f(m)});return o;}

void MainWindow::refreshHistory(){
    const qint64 now=QDateTime::currentSecsSinceEpoch(),span=selectedSpanSeconds();
    const qint64 to=now-timeOffset_,from=to-span;
    const auto h=db_.systemHistory(from,to,AppConfig::load().historyTargetPoints);
    cpuUsage_->setSeries({{"Usage",points(h,[](const auto&m){return m.cpuUsage;})}});cpuTemp_->setSeries({{"Temperature",points(h,[](const auto&m){return m.cpuTemperatureC;})}});
    mem_->setSeries({{"Used",points(h,[](const auto&m){return m.memoryUsedMiB/1024.0;})},{"Total",points(h,[](const auto&m){return m.memoryTotalMiB/1024.0;})}});swap_->setSeries({{"Swap",points(h,[](const auto&m){return m.swapUsedMiB/1024.0;})}});
    net_->setSeries({{"Download",points(h,[](const auto&m){return m.networkRxMiBs;})},{"Upload",points(h,[](const auto&m){return m.networkTxMiBs;})}});diskIo_->setSeries({{"Read",points(h,[](const auto&m){return m.diskReadMiBs;})},{"Write",points(h,[](const auto&m){return m.diskWriteMiBs;})}});
    battery_->setSeries({{"Level",points(h,[](const auto&m){return m.batteryPercent;})},{"Health",points(h,[](const auto&m){return m.batteryHealthPercent;})}});batteryPower_->setSeries({{"Power",points(h,[](const auto&m){return m.batteryPowerW;})}});batteryTemp_->setSeries({{"Temperature",points(h,[](const auto&m){return m.batteryTemperatureC;})}});
    diskTemp_->setSeries({{"Temperature",points(h,[](const auto&m){return m.nvmeTemperatureC;})}});
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
    // Full per-channel detail is read live from /sys/class/hwmon; history
    // keeps only the per-chip summary (see sensorsToJson), so the sensors
    // page must not depend on stored data.
    const QVector<SensorInfo> temps = readHwmonSensors(&liveFans_);
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
    QDir proc("/proc");
    proc.setNameFilters({"[0-9]*"});
    proc.setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const double dt = lastProcSampleMs_ > 0 ? static_cast<double>(nowMs - lastProcSampleMs_) / 1000.0 : 0.0;
    // mingw does not declare sysconf; the /proc scan yields nothing on Windows.
#if defined(_SC_CLK_TCK) && defined(_SC_PAGESIZE)
    static const double clkTicks = [] { const long v = sysconf(_SC_CLK_TCK); return v > 0 ? static_cast<double>(v) : 100.0; }();
    static const double pageMiB = [] { const long v = sysconf(_SC_PAGESIZE); return v > 0 ? static_cast<double>(v) / 1048576.0 : 4.0 / 1024.0; }();
#else
    static const double clkTicks = 100.0;
    static const double pageMiB = 4.0 / 1024.0;
#endif
    QHash<qint64, quint64> cur;
    QVector<ProcInfo> out;
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
    build(byName, false, latestSystem_.memoryTotalMiB, memTree_);
    const bool swapExists = std::isfinite(latestSystem_.swapTotalMiB) && latestSystem_.swapTotalMiB > 0.0;
    swapTree_->setEmptyText(swapExists ? tr("Nothing swapped out") : tr("No swap configured"));
    build(byName, true, latestSystem_.swapTotalMiB, swapTree_);
}

void MainWindow::gpuSelectionChanged(){const QString id=gpuSelector_?gpuSelector_->currentData().toString():QString();if(id.isEmpty()){gpuUsage_->setSeries({});gpuAux_->setSeries({});gpuMemory_->setSeries({});return;}const qint64 to=QDateTime::currentSecsSinceEpoch(),from=to-selectedSpanSeconds();const auto h=db_.gpuHistory(id,from,to,AppConfig::load().historyTargetPoints);gpuUsage_->setSeries({{"Usage",points(h,[](const auto&m){return m.gpus.isEmpty()?lmNaN():m.gpus[0].utilization;})}});gpuAux_->setSeries({{"Temperature",points(h,[](const auto&m){return m.gpus.isEmpty()?lmNaN():m.gpus[0].temperatureC;})},{"Power",points(h,[](const auto&m){return m.gpus.isEmpty()?lmNaN():m.gpus[0].powerW;})}});gpuMemory_->setSeries({{"Used",points(h,[](const auto&m){return m.gpus.isEmpty()?lmNaN():m.gpus[0].memoryUsedMiB/1024.0;})},{"Total",points(h,[](const auto&m){return m.gpus.isEmpty()?lmNaN():m.gpus[0].memoryTotalMiB/1024.0;})}});}

void MainWindow::diskSelectionChanged(){
    const QString mount=diskSelector_?diskSelector_->currentData().toString():QString();
    if(mount.isEmpty()){diskUsage_->setSeries({});return;}
    const qint64 to=QDateTime::currentSecsSinceEpoch(),from=to-selectedSpanSeconds();
    const auto h=db_.diskHistory(mount,from,to,AppConfig::load().historyTargetPoints);
    QVector<ChartWidget::Point> usedPts,totalPts; usedPts.reserve(h.size()); totalPts.reserve(h.size());
    for(const auto&d:h){usedPts.push_back({d.timestamp,d.usedGiB});totalPts.push_back({d.timestamp,d.totalGiB});}
    diskUsage_->setSeries({{"Used",usedPts},{"Total",totalPts}});
}


void MainWindow::showSettings() {
    SettingsDialog d(this);
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
    const qint64 to = QDateTime::currentSecsSinceEpoch();
    const qint64 from = to - selectedSpanSeconds();
    const auto h = db_.systemHistory(from, to, 10000);
    QTextStream out(&f);
    out << "timestamp,cpu_usage,cpu_temp,load1,memory_used_mib,memory_total_mib,network_rx_mibs,network_tx_mibs,disk_read_mibs,disk_write_mibs,battery_percent,battery_power_w,battery_health_percent\n";
    for (const auto &m : h) {
        auto n=[](double v){ return std::isfinite(v) ? QString::number(v, 'g', 12) : QString(); };
        out << m.timestamp << ',' << n(m.cpuUsage) << ',' << n(m.cpuTemperatureC) << ',' << n(m.load1) << ','
            << n(m.memoryUsedMiB) << ',' << n(m.memoryTotalMiB) << ',' << n(m.networkRxMiBs) << ',' << n(m.networkTxMiBs) << ','
            << n(m.diskReadMiBs) << ',' << n(m.diskWriteMiBs) << ','
            << n(m.batteryPercent) << ',' << n(m.batteryPowerW) << ',' << n(m.batteryHealthPercent) << '\n';
    }
}

void MainWindow::showAbout() {
    QMessageBox::about(this, tr("About LiteMon"),
        tr("<b>LiteMon 2.0</b><br>A lightweight native historical monitor for Linux desktops.<br><br>"
           "Local-first · SQLite · Qt 6 · Intel/NVIDIA aware · No cloud dependency."));
}
