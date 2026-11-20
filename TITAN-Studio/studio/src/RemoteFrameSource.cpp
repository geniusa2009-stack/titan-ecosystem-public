// TITAN Studio — reader side of the TITAN frame channel. Original TITAN code.
#include "RemoteFrameSource.h"

#include "TitanFrameChannel.h"

#include <QMutexLocker>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

RemoteFrameSource::~RemoteFrameSource() { detach(); }

bool RemoteFrameSource::attach(const QString& shmName, QString* error) {
    detach();
    const QByteArray name = shmName.toUtf8();
    const int fd = shm_open(name.constData(), O_RDONLY, 0);
    if (fd < 0) {
        if (error) *error = QStringLiteral("shm_open(%1) failed: errno %2").arg(shmName).arg(errno);
        return false;
    }
    struct stat st {};
    void* mem = MAP_FAILED;
    if (fstat(fd, &st) == 0 && st.st_size >= off_t(TITAN_FRAME_HEADER_BYTES))
        mem = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mem == MAP_FAILED) {
        if (error) *error = QStringLiteral("cannot map frame channel %1").arg(shmName);
        return false;
    }
    const auto* h = static_cast<const TitanFrameChannelHeader*>(mem);
    // Read the geometry once; the checks below are overflow-free (slot_bytes is bounded before multiplying).
    const uint64_t size = uint64_t(st.st_size);
    const uint64_t headerBytes = h->header_bytes, slotBytes = h->slot_bytes;
    const bool geometryOk = headerBytes >= sizeof(TitanFrameChannelHeader) && headerBytes <= size && slotBytes > 0 &&
                            slotBytes <= size && headerBytes + slotBytes * TITAN_FRAME_SLOTS <= size;
    if (h->magic != TITAN_FRAME_MAGIC || h->version != TITAN_FRAME_VERSION || h->slot_count != TITAN_FRAME_SLOTS ||
        !geometryOk) {
        munmap(mem, size_t(st.st_size));
        if (error) *error = QStringLiteral("frame channel %1 has an unexpected layout").arg(shmName);
        return false;
    }
    QMutexLocker lock(&m_mutex);
    m_header = h;
    m_bytes = size_t(st.st_size);
    m_headerBytes = headerBytes;
    m_slotBytes = slotBytes;
    m_generation.fetch_add(1);
    return true;
}

void RemoteFrameSource::detach() {
    QMutexLocker lock(&m_mutex);
    if (m_header) munmap(const_cast<TitanFrameChannelHeader*>(m_header), m_bytes);
    m_header = nullptr;
    m_bytes = 0;
    m_headerBytes = m_slotBytes = 0;
}

bool RemoteFrameSource::attached() const {
    QMutexLocker lock(&m_mutex);
    return m_header != nullptr;
}

void RemoteFrameSource::setRepaintHook(std::function<void()> hook) {
    QMutexLocker lock(&m_mutex);
    m_hook = std::move(hook);
}

void RemoteFrameSource::onFrameEvent(quint64, double captureToReadyMs) {
    m_announced.fetch_add(1, std::memory_order_relaxed);
    m_captureToReady.store(captureToReadyMs, std::memory_order_relaxed);
    // Called under the lock so setRepaintHook({}) in a destructor cannot race with a running hook.
    // Hooks only post a queued call, so this is short.
    QMutexLocker lock(&m_mutex);
    if (m_hook) m_hook();
}

bool RemoteFrameSource::copyLatest(quint64 have, Frame& out) {
    QMutexLocker lock(&m_mutex);
    if (!m_header) return false;
    const uint32_t slot = __atomic_load_n(&m_header->latest_slot, __ATOMIC_ACQUIRE);
    if (slot >= TITAN_FRAME_SLOTS) return false;
    const TitanFrameSlot& s = m_header->ring[slot];
    const uint64_t seq = __atomic_load_n(&s.seq, __ATOMIC_ACQUIRE);
    if (seq == 0 || (seq & 1u)) return false;
    const uint64_t frame = s.frame;
    if (frame <= have) return false;
    // Metadata is read inside the seqlock and may be torn; it is only used after bounds checks against the
    // geometry cached at attach(), so a torn or bogus value can at worst produce a frame that the second
    // seq check then discards, never an out-of-bounds read.
    const uint32_t w = s.width, h = s.height, stride = s.stride;
    if (w == 0 || h == 0 || uint64_t(stride) < uint64_t(w) * 4 || uint64_t(stride) * h > m_slotBytes) return false;
    const auto* src = reinterpret_cast<const char*>(m_header) + m_headerBytes + uint64_t(slot) * m_slotBytes;
    out.pixels.resize(qsizetype(w) * h * 4);
    for (uint32_t row = 0; row < h; ++row)
        std::memcpy(out.pixels.data() + qsizetype(row) * w * 4, src + uint64_t(row) * stride, size_t(w) * 4);
    out.size = QSize(int(w), int(h));
    out.number = frame;
    out.captureNs = int64_t(s.capture_ns);
    out.engineGpuMs = s.gpu_ms;
    std::memcpy(out.view, s.view, sizeof(out.view));
    out.cameraRevision = s.camera_rev;
    if (m_copyHook) m_copyHook();
    // Seqlock read side: the fence keeps every pixel/metadata load above from being satisfied after the
    // re-read of seq (an acquire load alone only orders the loads that follow it), at both the compiler
    // and the hardware (arm64 dmb) level.
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&s.seq, __ATOMIC_RELAXED) != seq) {  // overwritten while copying
        m_torn.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}
