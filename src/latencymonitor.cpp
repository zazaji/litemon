#include "latencymonitor.h"

#include <QDateTime>
#include <QTcpSocket>
#include <QTimer>

namespace {
// The probe measures a full HTTP round trip: DNS + connect + HEAD request +
// first response bytes. A pure TCP connect is not enough — a transparent
// proxy's local stack accepts the handshake in ~1 ms for every destination,
// which would smear all hosts into the same near-zero line. First response
// bytes can only come from the real site, so the value is the actual access
// latency; an unreachable site never responds and records nothing.
constexpr int kProbeTimeoutMs = 3000;
constexpr quint16 kPort = 80;
} // namespace

LatencyMonitor::LatencyMonitor(QObject *parent) : QObject(parent) {}

void LatencyMonitor::start(int intervalMs) {
    if (!timer_) {
        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, &LatencyMonitor::measureNow);
    }
    if (intervalMs > 0) timer_->start(intervalMs);
    QTimer::singleShot(1500, this, &LatencyMonitor::measureNow);
}

void LatencyMonitor::stopNow() {
    if (timer_) timer_->stop();
}

void LatencyMonitor::measureNow() {
    if (running_ || hosts_.isEmpty()) return;
    running_ = true;
    samples_.clear();
    pending_ = hosts_.size();

    for (const QString &host : hosts_) {
        // "host" or "host:port" (last colon; IPv6 literals unsupported).
        QString hostname = host;
        quint16 port = kPort;
        const int colon = host.lastIndexOf(':');
        if (colon > 0) {
            hostname = host.left(colon);
            port = host.mid(colon + 1).toUShort();
        }
        auto *socket = new QTcpSocket(this);
        auto *timer = new QTimer(socket);
        timer->setSingleShot(true);
        const qint64 startedMs = QDateTime::currentMSecsSinceEpoch();
        const auto done = [this, socket, timer, host, startedMs](bool success) {
            // First completion (response, error or timeout) wins; disconnect so
            // the other signals cannot double-count this host.
            socket->disconnect();
            timer->stop();
            if (success) {
                PingSample s;
                s.timestamp = QDateTime::currentSecsSinceEpoch();
                s.host = host;
                s.ms = static_cast<double>(QDateTime::currentMSecsSinceEpoch() - startedMs);
                samples_.append(s);
            }
            socket->abort();
            socket->deleteLater();
            if (--pending_ == 0) {
                running_ = false;
                emit finished(samples_);
            }
        };
        connect(timer, &QTimer::timeout, socket, [done] { done(false); });
        connect(socket, &QTcpSocket::connected, socket, [socket, hostname, port, done] {
            Q_UNUSED(done);
            socket->write(QStringLiteral("HEAD / HTTP/1.1\r\nHost: %1:%2\r\n"
                                         "Connection: close\r\nUser-Agent: litemon-netping\r\n\r\n")
                              .arg(hostname).arg(port).toUtf8());
        });
        connect(socket, &QTcpSocket::readyRead, socket, [done] { done(true); });
        connect(socket, &QTcpSocket::errorOccurred, socket,
                [done](QAbstractSocket::SocketError) { done(false); });
        timer->start(kProbeTimeoutMs);
        socket->connectToHost(hostname, port);
    }
}
