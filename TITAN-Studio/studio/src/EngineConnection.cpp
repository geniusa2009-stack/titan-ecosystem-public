// TITAN Studio — protocol socket on a dedicated I/O thread. Original TITAN code.
#include "EngineConnection.h"

#include "RemoteFrameSource.h"

#include <QJsonDocument>
#include <QLocalSocket>
#include <QtEndian>

#include <chrono>

namespace titan_studio {
int64_t monotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace titan_studio

namespace {
constexpr quint32 kMaxFrame = 16u * 1024u * 1024u;
}

EngineConnection::EngineConnection(std::shared_ptr<RemoteFrameSource> frames) : m_frames(std::move(frames)) {}

EngineConnection::~EngineConnection() = default;

void EngineConnection::open(const QString& path) {
    if (!m_socket) {
        m_socket = new QLocalSocket(this);  // created on the I/O thread
        connect(m_socket, &QLocalSocket::readyRead, this, &EngineConnection::onReadyRead);
        connect(m_socket, &QLocalSocket::connected, this, &EngineConnection::connected);
        connect(m_socket, &QLocalSocket::disconnected, this, &EngineConnection::disconnected);
        connect(m_socket, &QLocalSocket::errorOccurred, this,
                [this](QLocalSocket::LocalSocketError) { emit socketError(m_socket->errorString()); });
    }
    m_in.clear();
    m_socket->abort();
    m_socket->connectToServer(path);
}

void EngineConnection::write(const QByteArray& frame) {
    if (m_socket && m_socket->state() == QLocalSocket::ConnectedState) {
        m_socket->write(frame);
        m_socket->flush();
    }
}

void EngineConnection::abort() {
    if (m_socket) m_socket->abort();
    m_in.clear();
}

void EngineConnection::onReadyRead() {
    m_in += m_socket->readAll();
    const qint64 arrival = titan_studio::monotonicNs();
    qsizetype pos = 0;
    while (m_in.size() - pos >= 4) {
        const quint32 n = qFromBigEndian<quint32>(m_in.constData() + pos);
        if (n > kMaxFrame) {
            emit protocolError(QStringLiteral("oversized frame; closing connection"));
            m_socket->abort();
            m_in.clear();
            return;
        }
        if (quint32(m_in.size() - pos) < 4 + n) break;
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromRawData(m_in.constData() + pos + 4, qsizetype(n)), &perr);
        pos += qsizetype(4 + n);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            emit protocolError(QStringLiteral("bad message: ") + perr.errorString());
            continue;
        }
        const QJsonObject msg = doc.object();
        // High-rate frame notifications go straight to the frame source (render thread reads it).
        if (m_frames && msg.value(QLatin1String("event")).toString() == QLatin1String("viewport.frame")) {
            const QJsonObject d = msg.value(QLatin1String("data")).toObject();
            m_frames->onFrameEvent(quint64(d.value(QLatin1String("frame")).toDouble()),
                                   d.value(QLatin1String("captureToReadyMs")).toDouble());
            framesNotified.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        emit message(msg, arrival);
    }
    m_in.remove(0, pos);
}
