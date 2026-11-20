// TITAN Extensions SDK tests: manifest validation, loading, determinism of the scatter extension.
#include "titan_test.h"

#include "extension_host.h"
#include "titan/project.h"

#include <filesystem>
#include <unistd.h>

using namespace titan;
namespace fs = std::filesystem;

static Json goodManifest() {
    return Json::parse(R"({
      "manifestVersion": 1, "id": "titan.test.ok", "name": "Ok", "version": "1.0.0",
      "publisher": "TITAN", "license": "LicenseRef-TITAN-Proprietary", "category": "editor",
      "apiVersion": 1, "engine": {"min": "0.6.0", "maxExclusive": "1.0.0"},
      "platforms": [")" + currentPlatform() + R"("], "dependencies": [], "capabilities": ["log"],
      "entry": {"library": "lib.dylib"}, "commands": []})");
}

TEST("manifest: valid manifest has no problems") {
    ExtensionManifest m;
    auto problems = validateManifest(goodManifest(), m);
    CHECK(problems.empty());
    CHECK(m.id == "titan.test.ok");
}

TEST("manifest: each invalid field is reported") {
    struct Case {
        const char* key;
        Json value;
    };
    std::vector<Case> cases = {
        {"apiVersion", Json(2)},
        {"id", Json("bad id/../x")},
        {"version", Json("one")},
        {"platforms", Json(Json::Array{Json("amiga-m68k")})},
        {"capabilities", Json(Json::Array{Json("process.spawn")})},
        {"category", Json("toaster")},
    };
    for (const auto& c : cases) {
        Json j = goodManifest();
        j[c.key] = c.value;
        ExtensionManifest m;
        CHECK(!validateManifest(j, m).empty());
    }
    for (const char* lib : {"../evil.dylib", "/usr/lib/libc.dylib", "sub/dir.dylib"}) {
        Json j = goodManifest();
        j["entry"]["library"] = lib;
        ExtensionManifest m;
        CHECK(!validateManifest(j, m).empty());
    }
    Json j = goodManifest();
    j["engine"]["min"] = "2.0.0";
    j["engine"]["maxExclusive"] = "3.0.0";
    ExtensionManifest m;
    CHECK(!validateManifest(j, m).empty());
    CHECK(!validateManifest(Json::parse("[]"), m).empty());
}

TEST("semver: parse and ordering") {
    CHECK(SemVer::parse("0.6.0-alpha").has_value());
    CHECK(!SemVer::parse("1.2").has_value());
    CHECK(*SemVer::parse("0.6.0") < *SemVer::parse("0.10.0"));
}

TEST("host: loads the scatter extension and reports a broken one") {
    fs::path tmp = fs::temp_directory_path() / ("titan-ext-" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp / "broken");
    Json bad = goodManifest();
    bad["id"] = "titan.test.broken";
    bad["entry"]["library"] = "missing.dylib";
    writeFileAtomic(tmp / "broken" / "titan-extension.json", bad.dump());
    fs::create_directories(tmp / "needs-missing");
    Json dep = goodManifest();
    dep["id"] = "titan.test.dep";
    dep["dependencies"] = Json(Json::Array{Json("titan.not.installed")});
    writeFileAtomic(tmp / "needs-missing" / "titan-extension.json", dep.dump());

    std::vector<std::string> logs;
    ExtensionHost host([&](int, const std::string& id, const std::string& msg) { logs.push_back(id + ": " + msg); });
    host.loadFrom({TITAN_EXTENSIONS_DIR, tmp});
    CHECK(host.isLoaded("titan.procedural.scatter"));
    CHECK(!host.isLoaded("titan.test.broken"));
    CHECK(!host.isLoaded("titan.test.dep"));
    Json all = host.describeAll();
    int failed = 0;
    for (const auto& e : all.asArray())
        if (e["status"].asString() == "failed") ++failed;
    CHECK(failed == 2);
    CHECK_THROWS(host.invoke("titan.procedural.scatter", "rm -rf", Json::object()));  // undeclared command
    host.unloadAll();
    fs::remove_all(tmp);
}

TEST("scatter: deterministic output, seed sensitivity, constraint handling") {
    ExtensionHost host({});
    host.loadFrom({TITAN_EXTENSIONS_DIR});
    auto gen = [&](int seed, int count, double spacing) {
        Json p = Json::object();
        p["recipe"] = Json::object();
        p["recipe"]["seed"] = seed;
        p["recipe"]["count"] = count;
        p["recipe"]["radius"] = 8;
        p["recipe"]["minSpacing"] = spacing;
        p["avoid"] = Json(Json::Array{Json(Json::Array{Json(0), Json(0), Json(2)})});
        return host.invoke("titan.procedural.scatter", "generate", p);
    };
    Json a = gen(42, 30, 1.0), b = gen(42, 30, 1.0), c = gen(43, 30, 1.0);
    CHECK(a.dump() == b.dump());
    CHECK(a.dump() != c.dump());
    CHECK(a["objects"].size() == 30);
    for (const auto& o : a["objects"].asArray()) {
        const auto& pos = o["transform"]["position"];
        double x = pos.at(0).asNumber(), z = pos.at(2).asNumber();
        CHECK(x * x + z * z <= 8.0 * 8.0 + 1e-6);   // inside the disc
        CHECK(std::sqrt(x * x + z * z) >= 2.0);       // outside the keep-out disc
    }
    Json crowded = gen(1, 500, 20.0);
    CHECK(crowded["produced"].asNumber() < 500);
    CHECK(crowded.contains("warning"));
    Json bad = Json::object();
    bad["recipe"] = Json::object();
    bad["recipe"]["shape"] = "teapot";
    CHECK_THROWS(host.invoke("titan.procedural.scatter", "generate", bad));
    Json d = host.invoke("titan.procedural.scatter", "describe", Json::object());
    CHECK(d["params"].size() >= 6);
}

TITAN_TEST_MAIN()
