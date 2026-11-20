// TITAN frame channel: reader (TITAN Studio RemoteFrameSource) against a writer that follows the
// bridge's protocol (TitanBridgeFrames.cpp PollRenderThread). Original TITAN code.
#include "RemoteFrameSource.h"
#include "TitanFrameChannel.h"
#include "titan_test.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

namespace {
// Minimal writer with the same layout and seqlock order as the Unreal bridge.
struct Writer {
    std::string name;
    TitanFrameChannelHeader* h = nullptr;
    size_t bytes = 0;
    uint32_t next = 0;
    Writer(uint32_t w, uint32_t hgt) {
        name = "/titan-test-" + std::to_string(getpid());
        shm_unlink(name.c_str());
        const uint64_t slot = (uint64_t(w) * hgt * 4 + 16383) / 16384 * 16384;
        bytes = TITAN_FRAME_HEADER_BYTES + slot * TITAN_FRAME_SLOTS;
        const int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        CHECK(fd >= 0);
        CHECK(ftruncate(fd, off_t(bytes)) == 0);
        void* m = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        CHECK(m != MAP_FAILED);
        h = static_cast<TitanFrameChannelHeader*>(m);
        std::memset(h, 0, sizeof(*h));
        h->magic = TITAN_FRAME_MAGIC;
        h->version = TITAN_FRAME_VERSION;
        h->slot_count = TITAN_FRAME_SLOTS;
        h->max_width = w;
        h->max_height = hgt;
        h->header_bytes = TITAN_FRAME_HEADER_BYTES;
        h->slot_bytes = slot;
        h->latest_slot = UINT32_MAX;
    }
    ~Writer() {
        munmap(h, bytes);
        shm_unlink(name.c_str());
    }
    uint8_t* pixels(uint32_t s) { return reinterpret_cast<uint8_t*>(h) + h->header_bytes + uint64_t(s) * h->slot_bytes; }
    uint32_t begin(uint64_t frame, uint32_t w, uint32_t hgt) {  // returns the slot being written
        uint32_t s = next % TITAN_FRAME_SLOTS;
        if (s == h->latest_slot) s = (s + 1) % TITAN_FRAME_SLOTS;
        next = s + 1;
        __atomic_store_n(&h->ring[s].seq, frame * 2 - 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);  // same order as the bridge: odd seq before any data
        h->ring[s].frame = frame;
        h->ring[s].width = w;
        h->ring[s].height = hgt;
        h->ring[s].stride = w * 4;
        h->ring[s].format = TITAN_FRAME_FORMAT_BGRA8_SRGB;
        h->ring[s].capture_ns = 1000 + frame;
        return s;
    }
    void end(uint32_t s, uint64_t frame) {
        __atomic_store_n(&h->ring[s].seq, frame * 2, __ATOMIC_RELEASE);
        __atomic_store_n(&h->latest_slot, s, __ATOMIC_RELEASE);
        __atomic_store_n(&h->latest_frame, frame, __ATOMIC_RELEASE);
    }
    void write(uint64_t frame, uint32_t w, uint32_t hgt, uint8_t value) {
        const uint32_t s = begin(frame, w, hgt);
        std::memset(pixels(s), value, size_t(w) * hgt * 4);
        end(s, frame);
    }
};
}  // namespace

TEST("reader rejects a channel with the wrong layout") {
    Writer w(64, 32);
    w.h->version = 99;
    RemoteFrameSource src;
    QString err;
    CHECK(!src.attach(QString::fromStdString(w.name), &err));
    CHECK(!err.isEmpty());
}

TEST("reader returns the newest complete frame, once") {
    Writer w(64, 32);
    RemoteFrameSource src;
    QString err;
    CHECK(src.attach(QString::fromStdString(w.name), &err));
    RemoteFrameSource::Frame f;
    CHECK(!src.copyLatest(0, f));  // nothing written yet
    w.write(1, 64, 32, 0x11);
    w.write(2, 64, 32, 0x22);
    CHECK(src.copyLatest(0, f));
    CHECK(f.number == 2);
    CHECK(f.size.width() == 64 && f.size.height() == 32);
    CHECK(f.pixels.size() == 64 * 32 * 4);
    CHECK(uint8_t(f.pixels[0]) == 0x22 && uint8_t(f.pixels[f.pixels.size() - 1]) == 0x22);
    CHECK(f.captureNs == 1002);
    CHECK(!src.copyLatest(2, f));  // already have frame 2
}

TEST("a frame that is still being written is never returned") {
    Writer w(32, 16);
    RemoteFrameSource src;
    CHECK(src.attach(QString::fromStdString(w.name), nullptr));
    w.write(1, 32, 16, 0x01);
    // Writer starts frame 2 in another slot but has not finished: the reader must keep serving frame 1.
    const uint32_t s = w.begin(2, 32, 16);
    RemoteFrameSource::Frame f;
    CHECK(src.copyLatest(0, f));
    CHECK(f.number == 1 && uint8_t(f.pixels[0]) == 0x01);
    std::memset(w.pixels(s), 0x02, 32 * 16 * 4);
    w.end(s, 2);
    CHECK(src.copyLatest(1, f));
    CHECK(f.number == 2 && uint8_t(f.pixels[0]) == 0x02);
}

TEST("a slot overwritten during the copy is detected (seqlock)") {
    Writer w(32, 16);
    RemoteFrameSource src;
    CHECK(src.attach(QString::fromStdString(w.name), nullptr));
    w.write(1, 32, 16, 0x01);
    // Simulate the writer lapping the reader: latest slot's seq moves to "writing" for a later frame.
    const uint32_t latest = w.h->latest_slot;
    __atomic_store_n(&w.h->ring[latest].seq, uint64_t(7), __ATOMIC_RELEASE);  // odd: in progress
    RemoteFrameSource::Frame f;
    CHECK(!src.copyLatest(0, f));
}

TEST("a concurrent writer never produces an accepted torn frame") {
    // Stress regression with a real writer thread: every frame is one byte value, so a frame the reader
    // accepts must be uniform. Mutation results on an M3: it fails 3/3 when the reader's second seq check
    // is removed, but it passed 5/5 with both seqlock fences removed, so it does NOT demonstrate the fences
    // (they rest on the arm64 / C++ ordering rules, see TitanFrameChannel.h). Same-process only; the
    // cross-process Unreal -> Studio path is validated against the built editor (ue.bridge-conformance).
    constexpr uint32_t kW = 256, kH = 256;
    Writer w(kW, kH);
    RemoteFrameSource src;
    CHECK(src.attach(QString::fromStdString(w.name), nullptr));
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (uint64_t frame = 1; !stop.load(std::memory_order_relaxed); ++frame) {
            const uint32_t s = w.begin(frame, kW, kH);
            std::memset(w.pixels(s), int(frame % 251) + 1, size_t(kW) * kH * 4);
            w.end(s, frame);
        }
    });
    RemoteFrameSource::Frame f;
    quint64 have = 0, accepted = 0, mixed = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < until) {
        if (!src.copyLatest(have, f)) continue;
        have = f.number;
        ++accepted;
        const uint8_t v = uint8_t(f.number % 251) + 1;
        for (qsizetype i = 0; i < f.pixels.size(); i += 61)
            if (uint8_t(f.pixels[i]) != v) { ++mixed; break; }
    }
    stop = true;
    writer.join();
    CHECK(accepted > 10);
    CHECK(mixed == 0);
}

TEST("second seq check: a slot rewritten during the copy is rejected (deterministic)") {
    Writer w(32, 16);
    RemoteFrameSource src;
    CHECK(src.attach(QString::fromStdString(w.name), nullptr));
    w.write(1, 32, 16, 0x01);
    const uint32_t slot = w.h->latest_slot;
    RemoteFrameSource::Frame f;
    // The writer laps the reader at the worst moment: after the pixels were copied, before seq is re-read.
    src.setCopyHookForTesting([&] {
        std::memset(w.pixels(slot), 0x05, 32 * 16 * 4);
        __atomic_store_n(&w.h->ring[slot].seq, uint64_t(5 * 2), __ATOMIC_RELEASE);  // frame 5 completed here
    });
    CHECK(!src.copyLatest(0, f));
    CHECK(src.framesTorn() == 1);
    // Still in progress (odd) at re-check time: also rejected.
    __atomic_store_n(&w.h->ring[slot].seq, uint64_t(2), __ATOMIC_RELEASE);
    src.setCopyHookForTesting([&] { __atomic_store_n(&w.h->ring[slot].seq, uint64_t(7), __ATOMIC_RELEASE); });
    CHECK(!src.copyLatest(0, f));
    CHECK(src.framesTorn() == 2);
    // Control: nothing changes during the copy, so the frame is accepted.
    w.write(9, 32, 16, 0x09);
    src.setCopyHookForTesting({});
    CHECK(src.copyLatest(0, f));
    CHECK(f.number == 9 && uint8_t(f.pixels[0]) == 0x09);
}

TEST("slot geometry is taken from attach(), not re-read from shared memory") {
    Writer w(32, 16);
    RemoteFrameSource src;
    CHECK(src.attach(QString::fromStdString(w.name), nullptr));
    const uint64_t slotBytes = w.h->slot_bytes;
    w.write(1, 32, 16, 0x07);
    // A corrupt or hostile writer changes the geometry after attach: the reader must keep its own copy.
    w.h->header_bytes = 0x7fffffff;
    w.h->slot_bytes = uint64_t(1) << 40;
    RemoteFrameSource::Frame f;
    CHECK(src.copyLatest(0, f));
    CHECK(f.number == 1 && uint8_t(f.pixels[0]) == 0x07 && uint8_t(f.pixels[f.pixels.size() - 1]) == 0x07);
    // A frame that claims more bytes than the attached slot holds is refused (no read past the slot).
    const uint32_t s = w.begin(2, 32, 16);
    w.h->ring[s].height = uint32_t(slotBytes / (32 * 4) + 1);
    w.end(s, 2);
    CHECK(!src.copyLatest(1, f));
}

TEST("attach rejects geometry that does not fit the mapping, including overflow") {
    Writer w(32, 16);
    RemoteFrameSource src;
    w.h->slot_bytes = (UINT64_MAX / TITAN_FRAME_SLOTS) + 2;  // slot_bytes * 3 wraps around to a small value
    CHECK(!src.attach(QString::fromStdString(w.name), nullptr));
    w.h->slot_bytes = 16384;
    w.h->header_bytes = 8;  // smaller than the header itself
    CHECK(!src.attach(QString::fromStdString(w.name), nullptr));
}

TEST("frame size changes and re-attach bump the generation") {
    Writer w(64, 32);
    RemoteFrameSource src;
    CHECK(src.attach(QString::fromStdString(w.name), nullptr));
    const quint64 g1 = src.generation();
    w.write(1, 48, 24, 0x05);  // smaller than the channel maximum
    RemoteFrameSource::Frame f;
    CHECK(src.copyLatest(0, f));
    CHECK(f.size.width() == 48 && f.pixels.size() == 48 * 24 * 4);
    CHECK(src.attach(QString::fromStdString(w.name), nullptr));
    CHECK(src.generation() == g1 + 1);
    src.detach();
    CHECK(!src.attached());
    CHECK(!src.copyLatest(0, f));
}

TEST("frame carries the camera it was rendered with") {
    Writer w(16, 16);
    RemoteFrameSource src;
    CHECK(src.attach(QString::fromStdString(w.name), nullptr));
    const uint32_t s = w.begin(1, 16, 16);
    for (int i = 0; i < 16; ++i) w.h->ring[s].view[i] = float(i + 1);
    w.h->ring[s].camera_rev = 42;
    w.end(s, 1);
    RemoteFrameSource::Frame f;
    CHECK(src.copyLatest(0, f));
    CHECK(f.cameraRevision == 42);
    CHECK(f.view[0] == 1.0f && f.view[15] == 16.0f);
}

TEST("repaint hook runs for each announced frame and can be removed") {
    RemoteFrameSource src;
    int calls = 0;
    src.setRepaintHook([&] { ++calls; });
    src.onFrameEvent(1, 3.5);
    src.onFrameEvent(2, 4.0);
    CHECK(calls == 2);
    CHECK(src.framesAnnounced() == 2);
    CHECK(src.lastCaptureToReadyMs() == 4.0);
    src.setRepaintHook({});
    src.onFrameEvent(3, 1.0);
    CHECK(calls == 2);
}

TITAN_TEST_MAIN()
