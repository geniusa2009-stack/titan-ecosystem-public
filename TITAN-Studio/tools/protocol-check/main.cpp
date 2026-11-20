// titan-protocol-check — conformance checker for any TITAN editor-protocol server
// (the reference host today; the Unreal-side TitanStudioBridge when it is built).
// Original TITAN code.
//
// usage: titan-protocol-check --socket <path> --workdir <empty-dir> [--shutdown] [--report <file>]
//                             [--call-timeout <ms>] [--viewport [--frame-timeout <ms>]]
// --viewport adds the engine-rendered viewport checks (servers with capability "viewport.frames",
// i.e. the Unreal bridge): frames must arrive in shared memory, contain rendered content, and change
// when the camera moves and when an object is edited. Frames are saved as BMP files in the workdir.
#include "TitanFrameChannel.h"
#include "titan/ipc.h"
#include "titan/project.h"
#include "titan/protocol.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using titan::Json;

namespace {
struct Step {
    std::string name;
    bool pass;
    std::string detail;
    double ms;
};

// Read-only view of an engine frame channel (see TitanFrameChannel.h).
struct FrameView {
    const TitanFrameChannelHeader* h = nullptr;
    size_t bytes = 0;
    ~FrameView() {
        if (h) munmap(const_cast<TitanFrameChannelHeader*>(h), bytes);
    }
    std::string open(const std::string& name) {
        if (h) munmap(const_cast<TitanFrameChannelHeader*>(h), bytes);  // re-open after a resize
        h = nullptr;
        bytes = 0;
        const int fd = shm_open(name.c_str(), O_RDONLY, 0);
        if (fd < 0) return "shm_open(" + name + ") failed";
        struct stat st {};
        void* mem = MAP_FAILED;
        if (fstat(fd, &st) == 0) mem = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (mem == MAP_FAILED) return "mmap failed";
        h = static_cast<const TitanFrameChannelHeader*>(mem);
        bytes = size_t(st.st_size);
        if (h->magic != TITAN_FRAME_MAGIC || h->version != TITAN_FRAME_VERSION) return "bad frame channel header";
        return {};
    }
    struct Frame {
        std::vector<uint8_t> px;
        uint32_t w = 0, height = 0, rev = 0;
        uint64_t number = 0;
    };
    bool latest(Frame& f) const {
        const uint32_t slot = __atomic_load_n(&h->latest_slot, __ATOMIC_ACQUIRE);
        if (slot >= TITAN_FRAME_SLOTS) return false;
        const TitanFrameSlot& s = h->ring[slot];
        const uint64_t seq = __atomic_load_n(&s.seq, __ATOMIC_ACQUIRE);
        if (seq == 0 || (seq & 1u)) return false;
        f.w = s.width;
        f.height = s.height;
        f.rev = s.camera_rev;
        f.number = s.frame;
        const auto* src = reinterpret_cast<const uint8_t*>(h) + h->header_bytes + uint64_t(slot) * h->slot_bytes;
        f.px.resize(size_t(f.w) * f.height * 4);
        for (uint32_t y = 0; y < f.height; ++y) std::memcpy(f.px.data() + size_t(y) * f.w * 4, src + size_t(y) * s.stride, size_t(f.w) * 4);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);  // seqlock read side, see TitanFrameChannel.h
        return __atomic_load_n(&s.seq, __ATOMIC_RELAXED) == seq;
    }
};

double lumaStdDev(const FrameView::Frame& f) {
    double sum = 0, sq = 0;
    const size_t n = size_t(f.w) * f.height;
    for (size_t i = 0; i < n; ++i) {
        const double l = 0.0722 * f.px[i * 4] + 0.7152 * f.px[i * 4 + 1] + 0.2126 * f.px[i * 4 + 2];
        sum += l;
        sq += l * l;
    }
    const double mean = sum / double(n);
    return std::sqrt(std::max(0.0, sq / double(n) - mean * mean));
}

double meanAbsDiff(const FrameView::Frame& a, const FrameView::Frame& b) {
    if (a.px.size() != b.px.size() || a.px.empty()) return 0;
    double d = 0;
    for (size_t i = 0; i < a.px.size(); i += 4)
        for (int c = 0; c < 3; ++c) d += std::fabs(double(a.px[i + c]) - double(b.px[i + c]));
    return d / double(a.px.size() / 4 * 3);
}

void writeBmp(const std::string& path, const FrameView::Frame& f) {
    // 32-bit BGRA, top-down (negative height); readable by sips/Preview.
    std::ofstream o(path, std::ios::binary);
    const uint32_t data = uint32_t(f.px.size()), fileSize = 54 + data;
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { std::memcpy(hdr + at, &v, 4); };
    put32(2, fileSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, f.w);
    put32(22, uint32_t(-int32_t(f.height)));
    hdr[26] = 1;
    hdr[28] = 32;
    put32(34, data);
    o.write(reinterpret_cast<const char*>(hdr), 54);
    o.write(reinterpret_cast<const char*>(f.px.data()), std::streamsize(f.px.size()));
}
}  // namespace

int main(int argc, char** argv) {
    std::string socket, workdir, reportPath;
    bool shutdown = false, viewport = false;
    int callTimeout = 10000, frameTimeout = 600000;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--socket" && i + 1 < argc) socket = argv[++i];
        else if (a == "--workdir" && i + 1 < argc) workdir = argv[++i];
        else if (a == "--report" && i + 1 < argc) reportPath = argv[++i];
        else if (a == "--call-timeout" && i + 1 < argc) callTimeout = std::stoi(argv[++i]);
        else if (a == "--frame-timeout" && i + 1 < argc) frameTimeout = std::stoi(argv[++i]);
        else if (a == "--viewport") viewport = true;
        else if (a == "--shutdown") shutdown = true;
    }
    if (socket.empty() || workdir.empty()) {
        std::cerr << "usage: titan-protocol-check --socket <path> --workdir <dir> [--shutdown] [--report <file>]\n";
        return 2;
    }
    std::filesystem::remove_all(workdir);
    const std::string projectDir = (std::filesystem::path(workdir) / "ConformanceProject").string();

    titan::ipc::Client c;
    std::vector<Step> steps;
    auto run = [&](const std::string& name, const std::function<std::string()>& fn) {
        auto t0 = std::chrono::steady_clock::now();
        std::string err;
        try {
            err = fn();
        } catch (const std::exception& e) {
            err = e.what();
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        steps.push_back({name, err.empty(), err, ms});
        std::printf("[%s] %-34s %8.3f ms %s\n", err.empty() ? "PASS" : "FAIL", name.c_str(), ms, err.c_str());
        return err.empty();
    };
    auto call = [&](const std::string& method, Json params = Json::object()) -> Json {
        auto r = c.call(method, std::move(params), callTimeout);
        if (!r) throw std::runtime_error("no response to " + method);
        if (r->contains("error")) throw std::runtime_error(method + " -> " + (*r)["error"].dump());
        return (*r)["result"];
    };

    titan::ObjectId target = 0;
    run("connect", [&]() -> std::string { return c.connect(socket, 10000) ? "" : "cannot connect to " + socket; });
    run("session.hello negotiates v1", [&]() -> std::string {
        Json p = Json::object();
        p["client"] = "titan-protocol-check";
        p["protocol"]["min"] = 1;
        p["protocol"]["max"] = 1;
        Json r = call("session.hello", p);
        return r["protocol"].asNumber() == 1 ? "" : "unexpected protocol " + r["protocol"].dump();
    });
    run("project.create + scene snapshot", [&]() -> std::string {
        Json p = Json::object();
        p["path"] = projectDir;
        p["name"] = "Conformance";
        Json r = call("project.create", p);
        const auto& objs = r["scene"]["objects"].asArray();
        if (objs.empty()) return std::string("empty scene");
        for (const auto& o : objs)
            if (o["type"].asString() == "mesh" && o["shape"].asString() != "plane") target = o["id"].asUInt();
        return target ? std::string() : std::string("no editable mesh in scene");
    });
    run("selection.set", [&]() -> std::string {
        Json p = Json::object();
        p["ids"] = Json(Json::Array{Json(target)});
        return call("selection.set", p)["ids"].at(0).asUInt() == target ? "" : "selection not echoed";
    });
    run("object.update transform + event", [&]() -> std::string {
        Json p = Json::object();
        p["id"] = target;
        p["set"]["transform"]["position"] = Json(Json::Array{Json(1.5), Json(2.5), Json(3.5)});
        std::vector<Json> events;
        auto r = c.call("object.update", p, callTimeout, &events);
        if (!r || r->contains("error")) return std::string("update failed");
        // The scene.changed broadcast may arrive before or after the response, possibly behind
        // events from earlier steps (e.g. selection.changed).
        auto isChange = [&](const Json& e) {
            return e["event"].isString() && e["event"].asString() == "scene.changed" &&
                   e["data"]["upserted"].size() > 0 && e["data"]["upserted"].at(0)["id"].asUInt() == target;
        };
        for (const auto& e : events)
            if (isChange(e)) return std::string();
        for (int i = 0; i < 8; ++i) {
            auto e = c.receive(500);
            if (!e) break;
            if (isChange(*e)) return std::string();
        }
        return std::string("no scene.changed event for the edited object");
    });
    run("object.get reflects edit", [&]() -> std::string {
        Json p = Json::object();
        p["id"] = target;
        return call("object.get", p)["transform"]["position"].at(1).asNumber() == 2.5 ? "" : "position mismatch";
    });
    run("edit.undo / edit.redo", [&]() -> std::string {
        if (!call("edit.undo")["applied"].asBool()) return "undo not applied";
        Json p = Json::object();
        p["id"] = target;
        if (call("object.get", p)["transform"]["position"].at(1).asNumber() == 2.5) return "undo did not revert";
        if (!call("edit.redo")["applied"].asBool()) return "redo not applied";
        return call("object.get", p)["transform"]["position"].at(1).asNumber() == 2.5 ? "" : "redo mismatch";
    });
    run("project.save", [&]() -> std::string { return call("project.save")["state"]["dirty"].asBool() ? "still dirty" : ""; });
    run("project.close", [&]() -> std::string { call("project.close"); return std::string(); });
    run("project.open restores edit", [&]() -> std::string {
        Json p = Json::object();
        p["path"] = projectDir;
        Json r = call("project.open", p);
        for (const auto& o : r["scene"]["objects"].asArray())
            if (o["id"].asUInt() == target)
                return o["transform"]["position"].at(2).asNumber() == 3.5 ? "" : "persisted value mismatch";
        return "object id not stable across reopen";
    });
    if (viewport) {
        FrameView fv;
        FrameView::Frame first, moved, before, after;
        uint32_t rev = 0;
        auto camera = [&](double ex, double ey, double ez) {
            Json p = Json::object();
            p["eye"] = Json(Json::Array{Json(ex), Json(ey), Json(ez)});
            const double fx = 0 - ex, fy = 0.6 - ey, fz = 0 - ez, len = std::sqrt(fx * fx + fy * fy + fz * fz);
            p["forward"] = Json(Json::Array{Json(fx / len), Json(fy / len), Json(fz / len)});
            p["fovY"] = 50.0;
            p["revision"] = double(++rev);
            call("viewport.camera", p);
        };
        // Waits for a frame rendered with the current camera revision, `settle` frames after the first one
        // (Unreal's temporal effects converge over several frames).
        auto waitFrame = [&](FrameView::Frame& out, uint64_t settle) -> std::string {
            uint64_t firstNumber = 0;
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(frameTimeout)) {
                (void)c.receive(50);  // drain events (viewport.frame etc.)
                FrameView::Frame f;
                if (!fv.latest(f) || f.rev != rev) continue;
                if (!firstNumber) firstNumber = f.number;
                if (f.number >= firstNumber + settle) {
                    out = std::move(f);
                    return {};
                }
            }
            return "no frame for camera revision " + std::to_string(rev) + " within " + std::to_string(frameTimeout) + " ms";
        };
        run("viewport.configure maps the frame channel", [&]() -> std::string {
            Json p = Json::object();
            p["width"] = 480;
            p["height"] = 270;
            Json r = call("viewport.configure", p);
            return fv.open(r["shm"].asString());
        });
        run("engine finishes compiling shaders", [&]() -> std::string {
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(frameTimeout)) {
                Json s = call("engine.status");
                if (!s["unreal"].isObject() || s["unreal"]["shadersRemaining"].asNumber() == 0) return {};
                (void)c.receive(2000);
            }
            return "shaders still compiling";
        });
        run("Unreal renders a frame with real content", [&]() -> std::string {
            Json p = Json::object();
            p["continuous"] = true;
            call("viewport.render", p);
            camera(6.0, 3.2, 8.0);
            if (auto e = waitFrame(first, 30); !e.empty()) return e;
            writeBmp((std::filesystem::path(workdir) / "frame-camera-a.bmp").string(), first);
            const double sd = lumaStdDev(first);
            return sd > 3.0 ? "" : "frame is flat (luma std-dev " + std::to_string(sd) + ")";
        });
        run("camera change re-renders the view", [&]() -> std::string {
            camera(-7.0, 2.0, 5.0);
            if (auto e = waitFrame(moved, 30); !e.empty()) return e;
            writeBmp((std::filesystem::path(workdir) / "frame-camera-b.bmp").string(), moved);
            const double d = meanAbsDiff(first, moved);
            return d > 2.0 ? "" : "image did not change with the camera (mean diff " + std::to_string(d) + ")";
        });
        run("object edit is visible in the rendered frame", [&]() -> std::string {
            camera(6.0, 3.2, 8.0);
            if (auto e = waitFrame(before, 30); !e.empty()) return e;
            Json p = Json::object();
            p["id"] = target;
            p["set"]["transform"]["position"] = Json(Json::Array{Json(-1.0), Json(2.6), Json(0.0)});
            call("object.update", p);
            camera(6.0, 3.2, 8.0);  // new revision: wait for frames rendered after the edit
            if (auto e = waitFrame(after, 30); !e.empty()) return e;
            writeBmp((std::filesystem::path(workdir) / "frame-edit-before.bmp").string(), before);
            writeBmp((std::filesystem::path(workdir) / "frame-edit-after.bmp").string(), after);
            call("edit.undo");
            Json q = Json::object();
            q["continuous"] = false;
            call("viewport.render", q);
            const double d = meanAbsDiff(before, after);
            return d > 0.3 ? "" : "edit not visible in the frame (mean diff " + std::to_string(d) + ")";
        });
        // Studio first configures a tiny viewport (e.g. 320x16) and then its real size. Re-creating the channel
        // while readbacks of the old size are pooled crashed Unreal (render thread SIGSEGV in memmove).
        run("viewport resizes keep rendering at the new size", [&]() -> std::string {
            Json on = Json::object();
            on["continuous"] = true;
            const uint32_t sizes[][2] = {{320, 16}, {802, 828}, {480, 270}, {802, 828}};
            for (const auto& sz : sizes) {
                Json p = Json::object();
                p["width"] = double(sz[0]);
                p["height"] = double(sz[1]);
                Json r = call("viewport.configure", p);
                if (auto e = fv.open(r["shm"].asString()); !e.empty()) return e;
                call("viewport.render", on);
                camera(6.0, 3.2, 8.0);
                FrameView::Frame f;
                if (auto e = waitFrame(f, 10); !e.empty())
                    return std::to_string(sz[0]) + "x" + std::to_string(sz[1]) + ": " + e;
                if (f.w != sz[0] || f.height != sz[1])
                    return "frame is " + std::to_string(f.w) + "x" + std::to_string(f.height) + ", expected " +
                           std::to_string(sz[0]) + "x" + std::to_string(sz[1]);
            }
            Json off = Json::object();
            off["continuous"] = false;
            call("viewport.render", off);
            return call("engine.status")["metrics"]["requests"].asNumber() > 0 ? "" : "engine stopped answering";
        });
    }
    run("engine.status metrics", [&]() -> std::string {
        Json r = call("engine.status");
        return r["metrics"]["requests"].asNumber() > 0 ? "" : "no metrics";
    });
    if (shutdown) run("engine.shutdown", [&]() -> std::string { call("engine.shutdown"); return std::string(); });

    int failed = 0;
    Json report = Json::object();
    Json arr = Json::array();
    for (const auto& s : steps) {
        failed += s.pass ? 0 : 1;
        Json j = Json::object();
        j["step"] = s.name;
        j["pass"] = s.pass;
        j["ms"] = s.ms;
        if (!s.pass) j["detail"] = s.detail;
        arr.push_back(j);
    }
    report["socket"] = socket;
    report["steps"] = arr;
    report["failed"] = failed;
    if (!reportPath.empty()) titan::writeFileAtomic(reportPath, report.dump(2) + "\n");
    std::printf("%zu steps, %d failed\n", steps.size(), failed);
    return failed == 0 ? 0 : 1;
}
