// titan-ue-bench — measures the existing UE 5.8.4 renderer through the TITAN bridge before any TITAN
// renderer work (directive: benchmark Nanite, Lumen, VSM, RDG, GPU Scene, streaming/memory first).
// Original TITAN code.
//
// usage: titan-ue-bench --socket <path> --project <dir> [--width 1280 --height 720]
//                       [--warmup 6] [--seconds 10] [--stress N] [--out report.json]
//
// For each renderer configuration it sets console variables through unreal.setCVar, renders the TITAN
// viewport continuously, waits `warmup` seconds, then samples engine.status twice a second. Reported
// values come from Unreal itself (RHIGetGPUFrameCycles, GGameThreadTime/GRenderThreadTime, RHI memory).
#include "titan/ipc.h"
#include "titan/project.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using titan::Json;

namespace {
struct Config {
    const char* name;
    std::vector<std::pair<const char*, const char*>> cvars;
};

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t i = std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5));
    return v[i];
}
}  // namespace

int main(int argc, char** argv) {
    std::string socket, project, out;
    int width = 1280, height = 720, warmup = 6, seconds = 10, stress = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--socket") socket = next();
        else if (a == "--project") project = next();
        else if (a == "--out") out = next();
        else if (a == "--width") width = std::stoi(next());
        else if (a == "--height") height = std::stoi(next());
        else if (a == "--warmup") warmup = std::stoi(next());
        else if (a == "--seconds") seconds = std::stoi(next());
        else if (a == "--stress") stress = std::stoi(next());
    }
    if (socket.empty() || project.empty()) {
        std::fprintf(stderr, "usage: titan-ue-bench --socket <path> --project <dir> [--stress N] [--out file]\n");
        return 2;
    }
    titan::ipc::Client c;
    if (!c.connect(socket, 10000)) {
        std::fprintf(stderr, "cannot connect to %s\n", socket.c_str());
        return 1;
    }
    auto call = [&](const std::string& m, Json p = Json::object()) -> Json {
        auto r = c.call(m, std::move(p), 600000);
        if (!r || r->contains("error")) {
            std::fprintf(stderr, "%s failed: %s\n", m.c_str(), r ? (*r)["error"].dump().c_str() : "no response");
            std::exit(1);
        }
        return (*r)["result"];
    };
    auto sleepPump = [&](double s) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(s * 1000));
        while (std::chrono::steady_clock::now() < end) (void)c.receive(50);
    };

    Json hello = Json::object();
    hello["client"] = "titan-ue-bench";
    hello["protocol"]["min"] = 1;
    hello["protocol"]["max"] = 1;
    const Json h = call("session.hello", hello);
    Json p = Json::object();
    p["path"] = project;
    p["force"] = true;
    if (std::filesystem::exists(std::filesystem::path(project) / "project.titan")) call("project.open", p);
    else {
        p["name"] = "RendererBench";
        call("project.create", p);
    }
    for (int i = 0; i < stress; ++i) {  // grid of extra primitives (each one Unreal actor + draw)
        Json o = Json::object();
        o["type"] = "mesh";
        o["shape"] = (i % 3 == 0) ? "sphere" : (i % 3 == 1) ? "cube" : "cylinder";
        const int side = 1 + int(std::sqrt(double(stress)));
        o["transform"]["position"] = Json(Json::Array{Json(-12.0 + 24.0 * (i % side) / side), Json(0.5),
                                                      Json(-12.0 + 24.0 * (i / side) / side)});
        o["transform"]["scale"] = Json(Json::Array{Json(0.6), Json(0.6), Json(0.6)});
        o["name"] = "Bench " + std::to_string(i);
        call("object.create", o);
    }
    Json vc = Json::object();
    vc["width"] = width;
    vc["height"] = height;
    call("viewport.configure", vc);
    Json cam = Json::object();
    cam["eye"] = Json(Json::Array{Json(9.0), Json(5.0), Json(12.0)});
    cam["forward"] = Json(Json::Array{Json(-0.55), Json(-0.3), Json(-0.78)});
    cam["fovY"] = 50.0;
    cam["revision"] = 1.0;
    call("viewport.camera", cam);
    Json cont = Json::object();
    cont["continuous"] = true;
    call("viewport.render", cont);

    // Wait for shader compilation to finish; first-launch numbers would otherwise measure the compiler.
    for (;;) {
        Json s = call("engine.status");
        if (s["unreal"]["shadersRemaining"].asNumber() == 0) break;
        std::printf("waiting for %d shaders to compile...\n", int(s["unreal"]["shadersRemaining"].asNumber()));
        sleepPump(5);
    }

    const std::vector<Config> configs = {
        {"baseline: Nanite + Lumen GI/reflections + VSM",
         {{"r.Nanite", "1"}, {"r.DynamicGlobalIlluminationMethod", "1"}, {"r.ReflectionMethod", "1"}, {"r.Shadow.Virtual.Enable", "1"},
          {"r.RDG.ParallelExecute", "1"}, {"r.GPUScene.ParallelUpdate", "1"}}},
        {"Lumen off (no dynamic GI, screen-space reflections)", {{"r.DynamicGlobalIlluminationMethod", "0"}, {"r.ReflectionMethod", "2"}}},
        {"Virtual Shadow Maps off (conventional shadow maps)", {{"r.DynamicGlobalIlluminationMethod", "1"}, {"r.ReflectionMethod", "1"}, {"r.Shadow.Virtual.Enable", "0"}}},
        {"Nanite off", {{"r.Shadow.Virtual.Enable", "1"}, {"r.Nanite", "0"}}},
        {"RDG parallel execute off", {{"r.Nanite", "1"}, {"r.RDG.ParallelExecute", "0"}}},
        {"GPU Scene parallel update off", {{"r.RDG.ParallelExecute", "1"}, {"r.GPUScene.ParallelUpdate", "0"}}},
    };

    Json report = Json::object();
    report["engine"] = h["engine"];
    report["resolution"] = std::to_string(width) + "x" + std::to_string(height);
    report["stressActors"] = stress;
    report["warmupSeconds"] = warmup;
    report["sampleSeconds"] = seconds;
    Json results = Json::array();
    std::printf("\n%-54s %9s %9s %9s %9s %9s\n", "configuration", "GPU p50", "GPU p95", "frame", "render", "game");
    for (const Config& cfg : configs) {
        for (const auto& [name, value] : cfg.cvars) {
            Json cv = Json::object();
            cv["name"] = name;
            cv["value"] = value;
            call("unreal.setCVar", cv);
        }
        sleepPump(warmup);
        std::vector<double> gpu, frame, render, game;
        Json first = call("engine.status");
        const auto t0 = std::chrono::steady_clock::now();
        Json last = first;
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds)) {
            sleepPump(0.5);
            last = call("engine.status");
            const Json& u = last["unreal"];
            gpu.push_back(u["gpuMs"].asNumber());
            frame.push_back(u["frameMs"].asNumber());
            render.push_back(u["renderThreadMs"].asNumber());
            game.push_back(u["gameThreadMs"].asNumber());
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Json r = Json::object();
        r["name"] = cfg.name;
        Json cvs = Json::object();
        for (const auto& [n, v] : cfg.cvars) cvs[n] = v;
        r["cvars"] = cvs;
        r["gpuMsP50"] = percentile(gpu, 0.5);
        r["gpuMsP95"] = percentile(gpu, 0.95);
        r["frameMsP50"] = percentile(frame, 0.5);
        r["renderThreadMsP50"] = percentile(render, 0.5);
        r["gameThreadMsP50"] = percentile(game, 0.5);
        r["framesDeliveredPerSecond"] =
            (last["unreal"]["frames"]["delivered"].asNumber() - first["unreal"]["frames"]["delivered"].asNumber()) / elapsed;
        r["captureToReadyMs"] = last["unreal"]["frames"]["avgCaptureToReadyMs"];
        r["usedPhysicalBytes"] = last["unreal"]["usedPhysicalBytes"];
        r["streamingTextureBytes"] = last["unreal"]["streamingTextureBytes"];
        r["nonStreamingTextureBytes"] = last["unreal"]["nonStreamingTextureBytes"];
        r["samples"] = double(gpu.size());
        results.push_back(r);
        std::printf("%-54s %8.2fms %8.2fms %8.2fms %8.2fms %8.2fms\n", cfg.name, r["gpuMsP50"].asNumber(),
                    r["gpuMsP95"].asNumber(), r["frameMsP50"].asNumber(), r["renderThreadMsP50"].asNumber(),
                    r["gameThreadMsP50"].asNumber());
    }
    // Restore the project's renderer baseline.
    for (const auto& [name, value] : configs.front().cvars) {
        Json cv = Json::object();
        cv["name"] = name;
        cv["value"] = value;
        call("unreal.setCVar", cv);
    }
    Json stop = Json::object();
    stop["continuous"] = false;
    call("viewport.render", stop);
    report["results"] = results;
    if (!out.empty()) titan::writeFileAtomic(out, report.dump(2) + "\n");
    return 0;
}
