// TITAN Studio — reader side of the TITAN frame channel (see TitanFrameChannel.h in the Unreal bridge).
// Maps the engine's shared-memory ring read-only and hands the newest complete frame to the viewport's
// render thread. Original TITAN code.
//
// Threading: attach()/detach() on the GUI thread; onFrameEvent() on the I/O thread; copyLatest() on the
// Qt Quick render thread. A mutex guards the mapping; the pixels are validated with the slot seqlock.
#pragma once

#include <QByteArray>
#include <QMutex>
#include <QSize>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>

struct TitanFrameChannelHeader;

class RemoteFrameSource {
public:
    ~RemoteFrameSource();

    bool attach(const QString& shmName, QString* error);
    void detach();
    bool attached() const;

    // I/O thread: a frame finished in the engine. Calls the repaint hook.
    void onFrameEvent(quint64 frame, double captureToReadyMs);
    void setRepaintHook(std::function<void()> hook);

    struct Frame {
        QByteArray pixels;  // BGRA8 (sRGB-encoded), tightly packed, top row first
        QSize size;
        quint64 number = 0;
        int64_t captureNs = 0;  // engine clock (same monotonic clock as Studio on macOS)
        double engineGpuMs = 0;
        float view[16] = {};      // view matrix the engine rendered this frame with (column-major)
        quint32 cameraRevision = 0;
    };
    // Render thread: copies the newest complete frame if it is newer than `have`. Returns false if
    // nothing new is available or the engine overwrote the slot while we copied (seqlock).
    bool copyLatest(quint64 have, Frame& out);

    // Incremented on every attach(): frame numbers restart when the engine re-creates the channel.
    quint64 generation() const { return m_generation.load(); }
    quint64 framesAnnounced() const { return m_announced.load(); }
    quint64 framesTorn() const { return m_torn.load(); }
    double lastCaptureToReadyMs() const { return m_captureToReady.load(); }

    // Tests only: runs inside copyLatest() after the pixels are copied and before seq is re-read, so a
    // test can overwrite the slot at exactly that point and check the second seqlock check deterministically.
    void setCopyHookForTesting(std::function<void()> hook) { m_copyHook = std::move(hook); }

private:
    mutable QMutex m_mutex;
    const TitanFrameChannelHeader* m_header = nullptr;
    size_t m_bytes = 0;
    // Geometry validated once at attach() against the mapping size. The engine never changes it after
    // creating the channel; copyLatest() uses these copies so a corrupt or changed header can never make
    // it read outside the mapping.
    uint64_t m_headerBytes = 0, m_slotBytes = 0;
    std::function<void()> m_copyHook;
    std::function<void()> m_hook;
    std::atomic<quint64> m_announced{0}, m_torn{0}, m_generation{0};
    std::atomic<double> m_captureToReady{0};
};
