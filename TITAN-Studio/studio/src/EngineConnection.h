// TITAN Studio — protocol socket on a dedicated I/O thread. Original TITAN code.
//
// Why: with the viewport rendering continuously, the GUI thread is paced by vsync, so engine replies
// read on the GUI thread waited up to a frame (measured 12.9-15.7 ms average "RTT" in 0.6). Reading,
// framing and JSON parsing now happen here; arrival is timestamped on this thread, so the reported
// round trip is the engine's real one, and high-rate events (viewport.frame) never touch the GUI thread.
//
// Threading: lives in its own QThread. Public slots are invoked with Qt::QueuedConnection from the GUI
// thread; signals are delivered to the GUI thread queued.
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

class QLocalSocket;
class RemoteFrameSource;

namespace titan_studio {
// Monotonic clock shared by Studio and the engine on macOS (CLOCK_UPTIME_RAW == mach_absolute_time).
int64_t monotonicNs();
}

class EngineConnection : public QObject {
    Q_OBJECT
public:
    explicit EngineConnection(std::shared_ptr<RemoteFrameSource> frames);
    ~EngineConnection() override;

    std::atomic<quint64> framesNotified{0};

public slots:
    void open(const QString& path);
    void write(const QByteArray& frame);
    void abort();

signals:
    void connected();
    void disconnected();
    void socketError(const QString& message);
    void message(const QJsonObject& msg, qint64 arrivalNs);
    void protocolError(const QString& message);

private:
    void onReadyRead();
    QLocalSocket* m_socket = nullptr;
    QByteArray m_in;
    std::shared_ptr<RemoteFrameSource> m_frames;
};
