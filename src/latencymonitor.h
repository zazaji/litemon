#pragma once

#include "model.h"

#include <QObject>
#include <QStringList>
#include <QVector>

class QTimer;

// Periodically measures TCP-connect latency to a list of websites. Each round
// opens an async connection to port 443 per host and records the elapsed
// milliseconds — but only for successful connects. Unreachable hosts record
// nothing, so their history keeps a gap (the chart draws a red band there).
//
// The object is meant to live on a dedicated QThread (moveToThread before
// start()): socket notifiers must be serviced by an event loop that the
// metric sampling loop can never block, otherwise every host's measured
// latency degenerates to "how long the sampling thread was busy".
class LatencyMonitor : public QObject {
    Q_OBJECT
public:
    explicit LatencyMonitor(QObject *parent = nullptr);

    void setHosts(const QStringList &hosts) { hosts_ = hosts; }

    // Call in the worker thread (e.g. from QThread::started): arms the round
    // timer and schedules one probe shortly after startup.
    void start(int intervalMs);

    // Stops the timer; call before stopping the worker thread so shutdown is
    // free of active timers on a foreign thread.
    void stopNow();

    // Starts one asynchronous round (one socket per host). Results arrive as
    // one batch via finished() once every host has connected or failed. Not
    // reentrant: rounds that overlap the previous one are dropped.
    void measureNow();

signals:
    void finished(const QVector<PingSample> &samples);

private:
    QStringList hosts_;
    QVector<PingSample> samples_;
    QTimer *timer_ = nullptr;
    int pending_ = 0;
    bool running_ = false;
};
