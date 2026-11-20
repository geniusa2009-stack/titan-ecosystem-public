// TITAN Engine — local IPC transport (Unix domain sockets on macOS/Linux).
// Windows named-pipe transport is not implemented yet; see docs/PLATFORM_COMPATIBILITY.md.
// Original TITAN code.
#pragma once

#include "titan/json.h"
#include "titan/protocol.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace titan::ipc {

// Owns a file descriptor.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd();
    Fd(Fd&& o) noexcept : fd_(o.release()) {}
    Fd& operator=(Fd&& o) noexcept;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    int release() {
        int f = fd_;
        fd_ = -1;
        return f;
    }
    explicit operator bool() const { return fd_ >= 0; }
    void reset();

private:
    int fd_ = -1;
};

// Creates a listening socket at `path` (mode 0600, stale socket file replaced). Throws.
Fd listenUnix(const std::string& path);
// Accepts a pending connection; rejects peers running as a different user. -1 fd on none.
Fd acceptPeer(int listenFd, std::string* rejectReason = nullptr);
Fd connectUnix(const std::string& path, int timeoutMs);
void setNonBlocking(int fd);

// Buffered, framed, non-blocking connection used by the engine host's poll loop.
class Connection {
public:
    explicit Connection(Fd fd) : fd_(std::move(fd)) { setNonBlocking(fd_.get()); }
    int fd() const { return fd_.get(); }
    // Reads what is available. Returns false when the peer closed or the stream is corrupt.
    bool readAvailable(std::vector<std::string>& framesOut);
    void queue(const Json& message) { out_ += protocol::encodeFrame(message); }
    bool wantsWrite() const { return !out_.empty(); }
    bool flush();  // false on fatal write error

    bool negotiated = false;
    std::string clientName;

private:
    Fd fd_;
    protocol::FrameDecoder decoder_;
    std::string out_;
};

// Blocking client used by tests, the protocol checker and headless tools.
class Client {
public:
    bool connect(const std::string& path, int timeoutMs = 2000);
    bool send(const Json& message);
    // Next message (response or event); nullopt on timeout or disconnect.
    std::optional<Json> receive(int timeoutMs);
    // Sends a request and waits for its response; events received meanwhile are appended.
    std::optional<Json> call(const std::string& method, Json params = Json::object(), int timeoutMs = 5000,
                             std::vector<Json>* events = nullptr);
    bool connected() const { return static_cast<bool>(fd_); }
    void close() { fd_.reset(); }

private:
    Fd fd_;
    protocol::FrameDecoder decoder_;
    uint64_t nextId_ = 1;
};

}  // namespace titan::ipc
