#include "mainwindow.h"
#include "linuxutils.h"
#include "appconfig.h"
#include "diagnostics.h"
#include "settingsdialog.h"

#include <QComboBox>
#include <QAction>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
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
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <functional>

using namespace LinuxUtils;

static QLabel *titleLabel(const QString &text){auto*l=new QLabel(text);QFont f=l->font();f.setPointSizeF(f.pointSizeF()+6);f.setBold(true);l->setFont(f);return l;}
static QLabel *sectionLabel(const QString &text){auto*l=new QLabel(text);QFont f=l->font();f.setPointSizeF(f.pointSizeF()+2);f.setBold(true);l->setFont(f);return l;}

MainWindow::MainWindow(const QString &dbPath, QWidget *parent) : QMainWindow(parent), db_(dbPath,"litemon-gui") {
    QString error; if(!db_.open(&error)){setWindowTitle("LiteMon - database error");}
    setupUi();
    timer_=new QTimer(this); connect(timer_,&QTimer::timeout,this,&MainWindow::refreshLatest); timer_->start(2000);
    refreshLatest(); refreshHistory();
}

MainWindow::Card MainWindow::makeCard(const QString &title,QGridLayout *grid,int row,int col){
    auto*f=new QFrame;
    f->setObjectName(QStringLiteral("LiteMonCard"));
    f->setFrameShape(QFrame::NoFrame);
    f->setStyleSheet(QStringLiteral("QFrame#LiteMonCard { background: palette(base); border: 1px solid palette(mid); border-radius: 12px; }"));
    f->setMinimumHeight(120);
    f->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto*l=new QVBoxLayout(f);
    l->setContentsMargins(16,14,16,14);
    l->setSpacing(4);
    auto*t=new QLabel(title);
    t->setStyleSheet(QStringLiteral("color: palette(placeholder-text); font-weight: 600;"));
    t->setMinimumWidth(0); // allow grid to constrain width
    t->setTextFormat(Qt::PlainText);
    t->setWordWrap(false);
    auto*v=new QLabel(QStringLiteral("—"));
    QFont vf=v->font();vf.setPointSizeF(vf.pointSizeF()+10);vf.setBold(true);v->setFont(vf);
    auto*d=new QLabel(tr("Waiting for collector…"));
    d->setWordWrap(true);
    d->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    l->addWidget(t);l->addWidget(v);l->addWidget(d);l->addStretch(1);
    grid->addWidget(f,row,col);
    return{v,d};
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
    connect(quitAction, &QAction::triggered, this, &QWidget::close);
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
    nav->addItems({tr("Overview"),tr("CPU"),tr("Memory"),tr("Network / Disk"),tr("GPU"),tr("Battery")});
    nav->setCurrentRow(0);
    nav->setFrameShape(QFrame::NoFrame);
    nav->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; border: none; outline: none; }"
        "QListWidget::item { padding: 9px 12px; border-radius: 8px; margin: 1px 2px; }"
        "QListWidget::item:selected { background: palette(highlight); color: palette(highlighted-text); }"
        "QListWidget::item:hover:!selected { background: palette(base); }"));
    sl->addWidget(nav,1);
    sl->addStretch(1);
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
    cpuCard_=makeCard(tr("CPU"),grid,0,0);memCard_=makeCard(tr("Memory"),grid,0,1);diskCard_=makeCard(tr("Root disk"),grid,0,2);netCard_=makeCard(tr("Network"),grid,1,0);batteryCard_=makeCard(tr("Battery"),grid,1,1);
    ov->addLayout(grid);
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
    auto*mem=makeHistoryPage(&mem_,&swap_);mem_->setTitle("Memory used","GiB");swap_->setTitle("Swap used","GiB");stack_->addWidget(mem);
    auto*io=makeHistoryPage(&net_,&diskIo_);net_->setTitle("Network throughput","MiB/s");diskIo_->setTitle("Disk throughput","MiB/s");auto*ioLayout=static_cast<QVBoxLayout*>(io->layout());diskSpace_=new ChartWidget;diskSpace_->setTitle("Root filesystem occupancy","GiB");diskSpace_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);ioLayout->addWidget(diskSpace_,1);stack_->addWidget(io);
    auto*gpuPage=makeHistoryPage(&gpuUsage_,&gpuAux_);auto*gplay=static_cast<QVBoxLayout*>(gpuPage->layout());
    auto*gpuBar=new QHBoxLayout;gpuBar->addWidget(new QLabel(tr("Device:")));gpuSelector_=new QComboBox;gpuSelector_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);gpuBar->addWidget(gpuSelector_,1);gplay->insertLayout(0,gpuBar);
    gpuUsage_->setTitle("GPU utilization","%");gpuUsage_->setFixedYRange(0,100);gpuAux_->setTitle("Temperature / Power","°C / W");gpuMemory_=new ChartWidget;gpuMemory_->setTitle("GPU memory (discrete GPUs)","GiB");gpuMemory_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);gplay->addWidget(gpuMemory_,1);stack_->addWidget(gpuPage);
    auto*bat=makeHistoryPage(&battery_,&batteryPower_);battery_->setTitle("Battery level / health","%");battery_->setFixedYRange(0,110);batteryPower_->setTitle("Battery charge / discharge power","W");stack_->addWidget(bat);

    connect(nav,&QListWidget::currentRowChanged,stack_,&QStackedWidget::setCurrentIndex);connect(nav,&QListWidget::currentRowChanged,this,[this](int){refreshHistory();});connect(range_,&QComboBox::currentIndexChanged,this,[this]{ timeOffset_=0; refreshHistory(); });connect(gpuSelector_,&QComboBox::currentIndexChanged,this,&MainWindow::gpuSelectionChanged);
    connect(prevBtn_,&QPushButton::clicked,this,[this]{ timeOffset_+=selectedSpanSeconds(); refreshHistory(); });
    connect(nextBtn_,&QPushButton::clicked,this,[this]{ timeOffset_=qMax(qint64(0),timeOffset_-selectedSpanSeconds()); refreshHistory(); });
}

QString MainWindow::fmtPercent(double v){return std::isfinite(v)?QString::number(v,'f',0)+"%":"—";} QString MainWindow::fmtTemp(double v){return std::isfinite(v)?QString::number(v,'f',0)+" °C":"—";} QString MainWindow::fmtPower(double v){return std::isfinite(v)?QString::number(v,'f',1)+" W":"—";}

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
    const auto m=db_.latestSystem();if(!m){updated_->setText("Waiting for collector data\n"+db_.path());return;}const auto &x=*m;updated_->setText("Updated "+QDateTime::fromSecsSinceEpoch(x.timestamp).toString("yyyy-MM-dd HH:mm:ss"));
    cpuCard_.value->setText(fmtPercent(x.cpuUsage));
    QString cpuDetail = fmtTemp(x.cpuTemperatureC)+" · Load "+(std::isfinite(x.load1)?QString::number(x.load1,'f',2):"—");
    if (!x.cpuCores.isEmpty()) { cpuDetail += " · " + QString::number(x.cpuCores.size()) + " cores"; }
    cpuCard_.detail->setText(cpuDetail);
    const double memPct=(std::isfinite(x.memoryUsedMiB)&&x.memoryTotalMiB>0)?x.memoryUsedMiB/x.memoryTotalMiB*100:lmNaN();memCard_.value->setText(fmtPercent(memPct));memCard_.detail->setText(humanBytesMiB(x.memoryUsedMiB)+" / "+humanBytesMiB(x.memoryTotalMiB));
    const double diskPct=(std::isfinite(x.diskUsedGiB)&&x.diskTotalGiB>0)?x.diskUsedGiB/x.diskTotalGiB*100:lmNaN();diskCard_.value->setText(fmtPercent(diskPct));diskCard_.detail->setText((std::isfinite(x.diskUsedGiB)?QString::number(x.diskUsedGiB,'f',1):"—")+" / "+(std::isfinite(x.diskTotalGiB)?QString::number(x.diskTotalGiB,'f',1):"—")+" GiB");
    netCard_.value->setText("↓ "+humanRate(x.networkRxMiBs));netCard_.detail->setText("↑ "+humanRate(x.networkTxMiBs));
    batteryCard_.value->setText(fmtPercent(x.batteryPercent));batteryCard_.detail->setText((x.batteryStatus.isEmpty()?"No battery":x.batteryStatus)+" · "+fmtPower(x.batteryPowerW)+" · health "+fmtPercent(x.batteryHealthPercent));

    const auto gpus=db_.latestGpus();
    while(auto*item=gpuCardsLayout_->takeAt(0)){if(item->widget())item->widget()->deleteLater();delete item;}gpuCards_.clear();
    int c=0;for(const auto&g:gpus){QString gpuTitle=g.vendor+" · "+g.name;if(gpuTitle.length()>24)gpuTitle=gpuTitle.left(21)+"…";auto card=makeCard(gpuTitle,gpuCardsLayout_,c/3,c%3);card.value->setText(g.state=="suspended"?"Suspended":fmtPercent(g.utilization));QStringList d;d<<fmtTemp(g.temperatureC)<<fmtPower(g.powerW);if(std::isfinite(g.frequencyMHz))d<<QString::number(g.frequencyMHz,'f',0)+" MHz";if(std::isfinite(g.memoryTotalMiB)&&g.memoryTotalMiB>0)d<<humanBytesMiB(g.memoryUsedMiB)+" / "+humanBytesMiB(g.memoryTotalMiB);else if(g.vendor=="Intel")d<<"shared memory";card.detail->setText(d.join(" · "));gpuCards_[g.id]=card;++c;}
    if(gpus.isEmpty()){auto*l=new QLabel("No Intel/NVIDIA GPU metrics yet. Intel: install intel-gpu-tools; NVIDIA: proprietary driver provides nvidia-smi.");l->setWordWrap(true);gpuCardsLayout_->addWidget(l,0,0);}

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
}

qint64 MainWindow::selectedSpanSeconds()const{return range_->currentData().toLongLong();}

static QVector<ChartWidget::Point> points(const QVector<SystemMetric>&h,const std::function<double(const SystemMetric&)>&f){QVector<ChartWidget::Point>o;o.reserve(h.size());for(const auto&m:h)o.push_back({m.timestamp,f(m)});return o;}

void MainWindow::refreshHistory(){
    const qint64 now=QDateTime::currentSecsSinceEpoch(),span=selectedSpanSeconds();
    const qint64 to=now-timeOffset_,from=to-span;
    const auto h=db_.systemHistory(from,to,AppConfig::load().historyTargetPoints);
    cpuUsage_->setSeries({{"Usage",points(h,[](const auto&m){return m.cpuUsage;})}});cpuTemp_->setSeries({{"Temperature",points(h,[](const auto&m){return m.cpuTemperatureC;})}});
    mem_->setSeries({{"Used",points(h,[](const auto&m){return m.memoryUsedMiB/1024.0;})},{"Total",points(h,[](const auto&m){return m.memoryTotalMiB/1024.0;})}});swap_->setSeries({{"Swap",points(h,[](const auto&m){return m.swapUsedMiB/1024.0;})}});
    net_->setSeries({{"Download",points(h,[](const auto&m){return m.networkRxMiBs;})},{"Upload",points(h,[](const auto&m){return m.networkTxMiBs;})}});diskIo_->setSeries({{"Read",points(h,[](const auto&m){return m.diskReadMiBs;})},{"Write",points(h,[](const auto&m){return m.diskWriteMiBs;})}});diskSpace_->setSeries({{"Used",points(h,[](const auto&m){return m.diskUsedGiB;})},{"Total",points(h,[](const auto&m){return m.diskTotalGiB;})}});
    battery_->setSeries({{"Level",points(h,[](const auto&m){return m.batteryPercent;})},{"Health",points(h,[](const auto&m){return m.batteryHealthPercent;})}});batteryPower_->setSeries({{"Power",points(h,[](const auto&m){return m.batteryPowerW;})}});
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

void MainWindow::gpuSelectionChanged(){const QString id=gpuSelector_?gpuSelector_->currentData().toString():QString();if(id.isEmpty()){gpuUsage_->setSeries({});gpuAux_->setSeries({});gpuMemory_->setSeries({});return;}const qint64 to=QDateTime::currentSecsSinceEpoch(),from=to-selectedSpanSeconds();const auto h=db_.gpuHistory(id,from,to,AppConfig::load().historyTargetPoints);gpuUsage_->setSeries({{"Usage",points(h,[](const auto&m){return m.gpus.isEmpty()?lmNaN():m.gpus[0].utilization;})}});gpuAux_->setSeries({{"Temperature",points(h,[](const auto&m){return m.gpus.isEmpty()?lmNaN():m.gpus[0].temperatureC;})},{"Power",points(h,[](const auto&m){return m.gpus.isEmpty()?lmNaN():m.gpus[0].powerW;})}});gpuMemory_->setSeries({{"Used",points(h,[](const auto&m){return m.gpus.isEmpty()?lmNaN():m.gpus[0].memoryUsedMiB/1024.0;})},{"Total",points(h,[](const auto&m){return m.gpus.isEmpty()?lmNaN():m.gpus[0].memoryTotalMiB/1024.0;})}});}


void MainWindow::showSettings() {
    SettingsDialog d(this);
    if (d.exec() == QDialog::Accepted) {
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
    out << "timestamp,cpu_usage,cpu_temp,load1,memory_used_mib,memory_total_mib,network_rx_mibs,network_tx_mibs,disk_read_mibs,disk_write_mibs,disk_used_gib,disk_total_gib,battery_percent,battery_power_w,battery_health_percent\n";
    for (const auto &m : h) {
        auto n=[](double v){ return std::isfinite(v) ? QString::number(v, 'g', 12) : QString(); };
        out << m.timestamp << ',' << n(m.cpuUsage) << ',' << n(m.cpuTemperatureC) << ',' << n(m.load1) << ','
            << n(m.memoryUsedMiB) << ',' << n(m.memoryTotalMiB) << ',' << n(m.networkRxMiBs) << ',' << n(m.networkTxMiBs) << ','
            << n(m.diskReadMiBs) << ',' << n(m.diskWriteMiBs) << ',' << n(m.diskUsedGiB) << ',' << n(m.diskTotalGiB) << ','
            << n(m.batteryPercent) << ',' << n(m.batteryPowerW) << ',' << n(m.batteryHealthPercent) << '\n';
    }
}

void MainWindow::showAbout() {
    QMessageBox::about(this, tr("About LiteMon"),
        tr("<b>LiteMon 2.0</b><br>A lightweight native historical monitor for Linux desktops.<br><br>"
           "Local-first · SQLite · Qt 6 · Intel/NVIDIA aware · No cloud dependency."));
}
