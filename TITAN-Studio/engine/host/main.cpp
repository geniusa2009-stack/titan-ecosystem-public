// TITAN Engine Host — process entry point. Serves the TITAN editor protocol over a local
// Unix domain socket and owns the canonical project/scene state. Original TITAN code.
//
// Usage: titan-engine-host --socket <path> [--extensions <dir>]... [--project <dir>]
//                          [--parent-pid <pid>] [--log <file>]
#include "engine_session.h"

#include "titan/ipc.h"
#include "titan/protocol.h"
#include "titan/version.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

std::unique_ptr<std::ofstream> g_logFile;

void logLine(int level, const std::string& source, const std::string& message) {
    static const char* names[] = {"debug", "info", "warning", "error"};
    char ts[32];
    std::time_t now = std::time(nullptr);
    std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    std::string line = std::string(ts) + " [" + names[level < 0 || level > 3 ? 1 : level] + "] " + source + ": " +
                       message;
    std::cerr << line << std::endl;
    if (g_logFile) *g_logFile << line << std::endl;
}

void usage() {
    std::cerr << "usage: titan-engine-host --socket <path> [--extensions <dir>]... [--project <dir>]"
                 " [--parent-pid <pid>] [--log <file>]\n";
}
}  // namespace

int main(int argc, char** argv) {
    std::string socketPath, projectPath, logPath;
    std::vector<std::filesystem::path> extensionDirs;
    long parentPid = 0;
    double autosaveSeconds = 20;
    std::string templatesDir;  // user scene templates
    std::string extensionState;  // persisted disabled-extension list  // crash-recovery point interval while the scene has unsaved changes
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--socket") socketPath = value();
        else if (a == "--extensions") extensionDirs.emplace_back(value());
        else if (a == "--project") projectPath = value();
        else if (a == "--parent-pid") parentPid = std::strtol(value().c_str(), nullptr, 10);
        else if (a == "--log") logPath = value();
        else if (a == "--templates") templatesDir = value();
        else if (a == "--extension-state") extensionState = value();
        else if (a == "--autosave-seconds") autosaveSeconds = std::strtod(value().c_str(), nullptr);
        else if (a == "--version") {
            std::cout << titan::kProductName << " host " << titan::kEngineVersion << " protocol "
                      << titan::kProtocolVersion << "\n";
            return 0;
        } else {
            usage();
            return 2;
        }
    }
    if (socketPath.empty()) {
        usage();
        return 2;
    }
    if (!logPath.empty()) g_logFile = std::make_unique<std::ofstream>(logPath, std::ios::app);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    titan::EngineSession session(extensionDirs, logLine, extensionState);

    session.autosaveInterval = std::chrono::milliseconds(long(autosaveSeconds * 1000));
    session.templatesDir = templatesDir;
    if (!projectPath.empty()) {
        std::vector<titan::Json> ignored;
        auto r = session.handle(titan::protocol::makeRequest(0, "project.open", [&] {
            titan::Json p = titan::Json::object();
            p["path"] = projectPath;
            return p;
        }()), ignored);
        if (r.contains("error")) logLine(3, "engine", "auto-open failed: " + r["error"].getString("message"));
    }

    titan::ipc::Fd listener;
    try {
        listener = titan::ipc::listenUnix(socketPath);
    } catch (const std::exception& e) {
        logLine(3, "engine", e.what());
        return 1;
    }
    logLine(1, "engine", std::string(titan::kProductName) + " host " + titan::kEngineVersion + " listening on " +
                             socketPath + " (pid " + std::to_string(::getpid()) + ")");
    // Machine-readable readiness line for supervisors (TITAN Studio waits for it).
    std::cout << "TITAN_HOST_READY " << socketPath << std::endl;

    std::vector<std::unique_ptr<titan::ipc::Connection>> clients;
    bool shutdownRequested = false;

    while (!g_stop && !shutdownRequested) {
        if (parentPid > 0 && ::getppid() != parentPid) {
            logLine(2, "engine", "parent process exited; shutting down");
            break;
        }
        std::vector<pollfd> fds;
        fds.push_back({listener.get(), POLLIN, 0});
        for (auto& c : clients)
            fds.push_back({c->fd(), static_cast<short>(POLLIN | (c->wantsWrite() ? POLLOUT : 0)), 0});
        int rc = ::poll(fds.data(), fds.size(), 250);
        session.tick();  // periodic crash-recovery point (no-op unless there are unsaved changes)
        if (rc < 0) {
            if (errno == EINTR) continue;
            logLine(3, "engine", std::string("poll: ") + std::strerror(errno));
            break;
        }
        if (fds[0].revents & POLLIN) {
            while (true) {
                std::string why;
                titan::ipc::Fd fd = titan::ipc::acceptPeer(listener.get(), &why);
                if (!fd) {
                    if (!why.empty()) logLine(2, "engine", "rejected connection: " + why);
                    if (errno == EAGAIN || errno == EWOULDBLOCK || why.empty()) break;
                    continue;
                }
                clients.push_back(std::make_unique<titan::ipc::Connection>(std::move(fd)));
                logLine(1, "engine", "client connected");
            }
        }
        std::vector<titan::Json> broadcast;
        std::vector<size_t> dead;
        for (size_t i = 0; i < clients.size(); ++i) {
            auto& c = *clients[i];
            const short rev = fds[i + 1].revents;
            if (rev & (POLLIN | POLLHUP | POLLERR)) {
                std::vector<std::string> frames;
                bool alive = c.readAvailable(frames);
                for (const auto& f : frames) {
                    titan::Json req;
                    try {
                        req = titan::Json::parse(f);
                    } catch (const std::exception& e) {
                        c.queue(titan::protocol::makeError(nullptr, titan::protocol::kInvalidRequest, e.what()));
                        continue;
                    }
                    const std::string method = req["method"].isString() ? req["method"].asString() : "";
                    if (!c.negotiated && method != "session.hello") {
                        c.queue(titan::protocol::makeError(req["id"], titan::protocol::kNotNegotiated,
                                                           "call session.hello first"));
                        continue;
                    }
                    std::vector<titan::Json> events;
                    titan::Json resp = session.handle(req, events);
                    if (method == "session.hello" && resp.contains("result")) {
                        c.negotiated = true;
                        c.clientName = req["params"].getString("client", "unknown");
                        logLine(1, "engine", "negotiated protocol with " + c.clientName);
                    }
                    c.queue(resp);
                    for (auto& e : events) broadcast.push_back(std::move(e));
                    if (method == "engine.shutdown") shutdownRequested = true;
                }
                if (!alive) dead.push_back(i);
            }
        }
        for (auto& c : clients)
            if (c->negotiated)
                for (const auto& e : broadcast) c->queue(e);
        for (size_t i = 0; i < clients.size(); ++i)
            if (!clients[i]->flush() && std::find(dead.begin(), dead.end(), i) == dead.end()) dead.push_back(i);
        std::sort(dead.rbegin(), dead.rend());
        for (size_t i : dead) {
            clients.erase(clients.begin() + static_cast<long>(i));
            logLine(1, "engine", "client disconnected");
        }
    }
    // Best-effort final flush so a shutdown response reaches the requester.
    for (auto& c : clients) c->flush();
    session.extensions().unloadAll();
    ::unlink(socketPath.c_str());
    logLine(1, "engine", "host stopped");
    return 0;
}
