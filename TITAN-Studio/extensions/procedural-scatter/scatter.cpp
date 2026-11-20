// TITAN Extension: Procedural Scatter (titan.procedural.scatter). Original TITAN code.
//
// A deterministic, recipe-driven scatter generator — the first World Builder operator.
// Given a recipe {seed, count, radius, minSpacing, shape, minScale, maxScale, center,
// palette}, it proposes scene objects. It never mutates the scene itself: the host turns
// the proposal into a preview, and only an explicit approval commits it as one undoable
// transaction tagged with the recipe id (so it can be regenerated selectively later).
//
// Determinism: uses its own PCG32 generator and only IEEE-754 basic arithmetic plus
// sqrt/sin/cos, so identical recipes produce identical output on the same platform.
#include "titan/extension_api.h"
#include "titan/json.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

const TitanHostApi* g_host = nullptr;
std::string g_result;  // storage for the string returned by invoke()

struct Pcg32 {
    uint64_t state = 0, inc = 0;
    explicit Pcg32(uint64_t seed, uint64_t seq = 0x5851F42D4C957F2Dull) {
        inc = (seq << 1u) | 1u;
        next();
        state += seed;
        next();
    }
    uint32_t next() {
        uint64_t old = state;
        state = old * 6364136223846793005ull + inc;
        auto xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        auto rot = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }
    double unit() { return next() * (1.0 / 4294967296.0); }  // [0, 1)
    double range(double lo, double hi) { return lo + (hi - lo) * unit(); }
};

titan::Json error(const std::string& message) {
    titan::Json e = titan::Json::object();
    e["message"] = message;
    titan::Json j = titan::Json::object();
    j["error"] = e;
    return j;
}

titan::Json param(const char* name, const char* type, double def, double min, double max, const char* label) {
    titan::Json p = titan::Json::object();
    p["name"] = name;
    p["type"] = type;
    p["default"] = def;
    p["min"] = min;
    p["max"] = max;
    p["label"] = label;
    return p;
}

titan::Json describe() {
    titan::Json j = titan::Json::object();
    j["id"] = "titan.procedural.scatter";
    j["title"] = "Procedural Scatter";
    j["summary"] = "Deterministically scatters primitives in a disc with minimum spacing.";
    titan::Json params = titan::Json::array();
    params.push_back(param("seed", "int", 1, 0, 4294967295.0, "Seed"));
    params.push_back(param("count", "int", 24, 1, 500, "Count"));
    params.push_back(param("radius", "float", 9, 1, 200, "Radius (m)"));
    params.push_back(param("minSpacing", "float", 1.2, 0, 20, "Min spacing (m)"));
    params.push_back(param("minScale", "float", 0.4, 0.05, 10, "Min scale"));
    params.push_back(param("maxScale", "float", 1.2, 0.05, 10, "Max scale"));
    titan::Json shape = titan::Json::object();
    shape["name"] = "shape";
    shape["type"] = "enum";
    shape["default"] = "cylinder";
    shape["options"] = titan::Json(titan::Json::Array{"cylinder", "cube", "sphere"});
    shape["label"] = "Shape";
    params.push_back(shape);
    j["params"] = params;
    return j;
}

double num(const titan::Json& r, const char* key, double def, double lo, double hi) {
    double v = r.getNumber(key, def);
    if (!std::isfinite(v)) v = def;
    return std::min(hi, std::max(lo, v));
}

titan::Json generate(const titan::Json& params) {
    const titan::Json& recipe = params["recipe"];
    if (!recipe.isObject()) return error("params.recipe must be an object");
    const auto seed = static_cast<uint64_t>(num(recipe, "seed", 1, 0, 4294967295.0));
    const int count = static_cast<int>(num(recipe, "count", 24, 1, 500));
    const double radius = num(recipe, "radius", 9, 1, 200);
    const double spacing = num(recipe, "minSpacing", 1.2, 0, 20);
    double minScale = num(recipe, "minScale", 0.4, 0.05, 10);
    double maxScale = num(recipe, "maxScale", 1.2, 0.05, 10);
    if (minScale > maxScale) std::swap(minScale, maxScale);
    const std::string shape = recipe.getString("shape", "cylinder");
    if (shape != "cylinder" && shape != "cube" && shape != "sphere") return error("unsupported shape: " + shape);
    double cx = 0, cz = 0;
    if (const titan::Json* c = recipe.find("center"); c && c->isArray() && c->size() == 3) {
        cx = c->at(0).asNumber();
        cz = c->at(2).asNumber();
    }

    // Keep-out discs supplied by the host (existing objects), so scatter avoids them.
    struct Disc { double x, z, r; };
    std::vector<Disc> placed;
    if (const titan::Json* avoid = params.find("avoid"); avoid && avoid->isArray())
        for (const auto& a : avoid->asArray())
            placed.push_back({a.at(0).asNumber(), a.at(1).asNumber(), a.at(2).asNumber()});

    Pcg32 rng(seed);
    titan::Json objects = titan::Json::array();
    int produced = 0;
    const int maxAttempts = count * 40;
    for (int attempt = 0; attempt < maxAttempts && produced < count; ++attempt) {
        // Uniform point in disc.
        double rr = radius * std::sqrt(rng.unit());
        double th = rng.unit() * 6.283185307179586;
        double x = cx + rr * std::cos(th), z = cz + rr * std::sin(th);
        double s = rng.range(minScale, maxScale);
        double yaw = rng.range(0, 360);
        double tint = rng.range(-0.06, 0.06);
        double footprint = 0.5 * s + spacing * 0.5;
        bool ok = true;
        for (const auto& d : placed) {
            double dx = d.x - x, dz = d.z - z;
            if (dx * dx + dz * dz < (d.r + footprint) * (d.r + footprint)) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        placed.push_back({x, z, footprint});
        ++produced;

        double height = shape == "cylinder" ? s * 3.0 : s;
        titan::Json o = titan::Json::object();
        o["name"] = std::string(shape == "cylinder" ? "Column " : shape == "cube" ? "Block " : "Orb ") +
                    std::to_string(produced);
        o["type"] = "mesh";
        o["shape"] = shape;
        titan::Json t = titan::Json::object();
        t["position"] = titan::Json(titan::Json::Array{x, height * 0.5, z});
        t["rotation"] = titan::Json(titan::Json::Array{0.0, yaw, 0.0});
        t["scale"] = titan::Json(titan::Json::Array{s, height, s});
        o["transform"] = t;
        o["color"] = titan::Json(titan::Json::Array{0.70 + tint, 0.72 + tint, 0.70 + tint});
        objects.push_back(o);
    }

    titan::Json out = titan::Json::object();
    out["objects"] = objects;
    out["requested"] = count;
    out["produced"] = produced;
    if (produced < count)
        out["warning"] = "spacing constraints allowed only " + std::to_string(produced) + " of " +
                         std::to_string(count) + " objects";
    return out;
}

int initialize(const TitanHostApi* host) {
    if (!host || host->api_version != TITAN_EXTENSION_API_VERSION) return 1;
    g_host = host;
    if (g_host->log) g_host->log(g_host->host_context, TITAN_LOG_INFO, "Procedural Scatter initialized");
    return 0;
}

void shutdown() {
    g_host = nullptr;
    g_result.clear();
}

const char* invoke(const char* command, const char* paramsJson) {
    try {
        std::string cmd = command ? command : "";
        titan::Json params = paramsJson && *paramsJson ? titan::Json::parse(paramsJson) : titan::Json::object();
        titan::Json out;
        if (cmd == "describe") out = describe();
        else if (cmd == "generate") out = generate(params);
        else out = error("unknown command: " + cmd);
        g_result = out.dump();
    } catch (const std::exception& e) {
        g_result = error(std::string("exception: ") + e.what()).dump();
    } catch (...) {
        g_result = error("unknown exception").dump();
    }
    return g_result.c_str();
}

const TitanExtensionApi kApi = {TITAN_EXTENSION_API_VERSION, "titan.procedural.scatter", &initialize, &shutdown,
                                &invoke};

}  // namespace

extern "C" TITAN_EXTENSION_EXPORT const TitanExtensionApi* titan_extension_entry(void) { return &kApi; }
