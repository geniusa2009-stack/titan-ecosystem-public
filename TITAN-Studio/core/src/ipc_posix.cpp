// TITAN Engine — POSIX Unix-domain-socket transport. Original TITAN code.
#include "titan/ipc.h"

#if defined(_WIN32)
#error "ipc_posix.cpp is POSIX-only; the Windows transport is not implemented yet"
#endif

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace titan::ipc {

Fd::~Fd() { reset(); }

Fd& Fd::operator=(Fd&& o) noexcept {
    if (this != &o) {
        reset();
        fd_ = o.release();
    }
    return *this;
}

void Fd::reset() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

void setNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

namespace {
sockaddr_un makeAddr(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) throw std::runtime_error("socket path too long: " + path);
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return addr;
}

void noSigPipe(int fd) {
#if defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
#endif
}

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
}  // namespace

Fd listenUnix(const std::string& path) {
    sockaddr_un addr = makeAddr(path);
    Fd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd) throw std::runtime_error(std::string("socket(): ") + std::strerror(errno));
    struct stat st{};
    if (::lstat(path.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) throw std::runtime_error("refusing to replace non-socket file: " + path);
        ::unlink(path.c_str());
    }
    mode_t old = ::umask(0177);  // socket created as 0600
    int rc = ::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    ::umask(old);
    if (rc != 0) throw std::runtime_error("bind(" + path + "): " + std::strerror(errno));
    if (::listen(fd.get(), 8) != 0) throw std::runtime_error(std::string("listen(): ") + std::strerror(errno));
    setNonBlocking(fd.get());
    return fd;
}

Fd acceptPeer(int listenFd, std::string* rejectReason) {
    Fd fd(::accept(listenFd, nullptr, nullptr));
    if (!fd) return fd;
    uid_t euid = 0;
    gid_t egid = 0;
    if (::getpeereid(fd.get(), &euid, &egid) != 0 || euid != ::geteuid()) {
        if (rejectReason) *rejectReason = "peer is not the same user";
        return Fd();
    }
    noSigPipe(fd.get());
    return fd;
}

Fd connectUnix(const std::string& path, int timeoutMs) {
    sockaddr_un addr = makeAddr(path);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        Fd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
        if (!fd) return fd;
        if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
            noSigPipe(fd.get());
            return fd;
        }
        if (std::chrono::steady_clock::now() >= deadline) return Fd();
        ::usleep(20 * 1000);  // the server may still be starting
    }
}

bool Connection::readAvailable(std::vector<std::string>& framesOut) {
    char buf[64 * 1024];
    while (true) {
        ssize_t n = ::recv(fd_.get(), buf, sizeof buf, 0);
        if (n > 0) {
            decoder_.feed(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) break;  // orderly shutdown; still deliver what we have
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            while (auto f = decoder_.next()) framesOut.push_back(std::move(*f));
            return !decoder_.failed();
        }
        return false;
    }
    while (auto f = decoder_.next()) framesOut.push_back(std::move(*f));
    return false;
}

bool Connection::flush() {
    while (!out_.empty()) {
        ssize_t n = ::send(fd_.get(), out_.data(), out_.size(), kSendFlags);
        if (n > 0) {
            out_.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        return false;
    }
    return true;
}

bool Client::connect(const std::string& path, int timeoutMs) {
    fd_ = connectUnix(path, timeoutMs);
    return static_cast<bool>(fd_);
}

bool Client::send(const Json& message) {
    if (!fd_) return false;
    std::string frame = protocol::encodeFrame(message);
    size_t off = 0;
    while (off < frame.size()) {
        ssize_t n = ::send(fd_.get(), frame.data() + off, frame.size() - off, kSendFlags);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            fd_.reset();
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

std::optional<Json> Client::receive(int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (fd_) {
        if (auto f = decoder_.next()) return Json::parse(*f);
        if (decoder_.failed()) {
            fd_.reset();
            return std::nullopt;
        }
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return std::nullopt;
        pollfd p{fd_.get(), POLLIN, 0};
        int rc = ::poll(&p, 1, static_cast<int>(left.count()));
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) return std::nullopt;
        char buf[64 * 1024];
        ssize_t n = ::recv(fd_.get(), buf, sizeof buf, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            fd_.reset();
            return std::nullopt;
        }
        decoder_.feed(buf, static_cast<size_t>(n));
    }
    return std::nullopt;
}

std::optional<Json> Client::call(const std::string& method, Json params, int timeoutMs, std::vector<Json>* events) {
    uint64_t id = nextId_++;
    if (!send(protocol::makeRequest(id, method, std::move(params)))) return std::nullopt;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return std::nullopt;
        auto msg = receive(static_cast<int>(left.count()));
        if (!msg) return std::nullopt;
        if (msg->contains("event")) {
            if (events) events->push_back(*msg);
            continue;
        }
        if ((*msg)["id"].isNumber() && (*msg)["id"].asUInt() == id) return msg;
    }
}

}  // namespace titan::ipc
