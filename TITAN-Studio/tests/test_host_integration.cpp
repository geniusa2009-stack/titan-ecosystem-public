// TITAN engine host integration tests: spawns the real titan-engine-host process and drives
// it over the Unix socket protocol exactly as TITAN Studio does.
#include <cmath>
#include <fstream>
#include <set>
#include "titan_test.h"

#include "titan/capabilities.h"
#include "titan/ipc.h"
#include "titan/project.h"
#include "titan/protocol.h"

#include <fcntl.h>
#include <filesystem>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using namespace titan;
namespace fs = std::filesystem;

namespace {
struct HostProcess {
    pid_t pid = -1;
    std::string socket;
    explicit HostProcess(const std::string& tag, std::vector<std::string> extra = {}) {
        socket = (fs::temp_directory_path() / ("titan-it-" + tag + "-" + std::to_string(::getpid()) + ".sock")).string();
        std::vector<std::string> args = {TITAN_HOST_EXE, "--socket", socket, "--extensions", TITAN_EXTENSIONS_DIR};
        args.insert(args.end(), extra.begin(), extra.end());
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);  // keep test output readable
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        if (posix_spawn(&pid, TITAN_HOST_EXE, &fa, nullptr, argv.data(), environ) != 0) pid = -1;
        posix_spawn_file_actions_destroy(&fa);
    }
    int waitExit() {
        int status = 0;
        if (pid > 0 && ::waitpid(pid, &status, 0) == pid) {
            pid = -1;
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        return -1;
    }
    ~HostProcess() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            ::waitpid(pid, nullptr, 0);
        }
    }
};

Json params(std::initializer_list<std::pair<const char*, Json>> kv) {
    Json p = Json::object();
    for (const auto& [k, v] : kv) p[k] = v;
    return p;
}

Json ok(ipc::Client& c, const std::string& method, Json p = Json::object(), std::vector<Json>* events = nullptr) {
    auto r = c.call(method, std::move(p), 5000, events);
    if (!r) throw titan_test::Failure(method + ": no response");
    if (r->contains("error")) throw titan_test::Failure(method + ": " + (*r)["error"].dump());
    return (*r)["result"];
}

int errorCode(ipc::Client& c, const std::string& method, Json p = Json::object()) {
    auto r = c.call(method, std::move(p));
    if (!r || !r->contains("error")) return 0;
    return static_cast<int>((*r)["error"]["code"].asNumber());
}

void hello(ipc::Client& c) {
    Json proto = Json::object();
    proto["min"] = 1;
    proto["max"] = 1;
    ok(c, "session.hello", params({{"client", Json("integration-test")}, {"protocol", proto}}));
}

ObjectId idByName(const Json& scene, const std::string& name) {
    for (const auto& o : scene["objects"].asArray())
        if (o["name"].asString() == name) return o["id"].asUInt();
    throw titan_test::Failure("object not found: " + name);
}

fs::path freshProjectDir(const std::string& tag) {
    fs::path p = fs::temp_directory_path() / ("titan-it-proj-" + tag + "-" + std::to_string(::getpid()));
    fs::remove_all(p);
    return p;
}
}  // namespace

TEST("host: negotiation is required and version mismatch is rejected") {
    HostProcess host("nego");
    ipc::Client c;
    CHECK(c.connect(host.socket, 3000));
    CHECK(errorCode(c, "scene.query") == protocol::kNotNegotiated);
    Json proto = Json::object();
    proto["min"] = 7;
    proto["max"] = 9;
    CHECK(errorCode(c, "session.hello", params({{"protocol", proto}})) == protocol::kVersionMismatch);
    hello(c);
    CHECK(errorCode(c, "no.such.method") == protocol::kMethodNotFound);
    CHECK(errorCode(c, "scene.query") == protocol::kNoProject);
    ok(c, "engine.shutdown");
    CHECK(host.waitExit() == 0);
}

TEST("host: end-to-end edit, events, undo/redo, pick, save, reopen") {
    HostProcess host("e2e");
    fs::path dir = freshProjectDir("e2e");
    ipc::Client a, b;  // two editors attached to the same engine
    CHECK(a.connect(host.socket, 3000) && b.connect(host.socket, 3000));
    hello(a);
    hello(b);

    Json opened = ok(a, "project.create", params({{"path", Json(dir.string())}, {"name", Json("E2E")}}));
    CHECK(opened["scene"]["objects"].size() == 10);
    ObjectId cube = idByName(opened["scene"], "Hero Cube");

    ok(a, "selection.set", params({{"ids", Json(Json::Array{Json(cube)})}}));
    // Client B sees the selection made by client A.
    bool sawSelection = false;
    for (int i = 0; i < 5 && !sawSelection; ++i) {
        auto ev = b.receive(1000);
        if (ev && (*ev)["event"].isString() && (*ev)["event"].asString() == "selection.changed")
            sawSelection = (*ev)["data"]["ids"].at(0).asUInt() == cube;
    }
    CHECK(sawSelection);

    Json t = Json::object();
    t["position"] = Json(Json::Array{Json(4.0), Json(1.0), Json(-2.0)});
    std::vector<Json> events;
    Json upd = ok(a, "object.update", params({{"id", Json(cube)}, {"set", params({{"transform", t}})}}), &events);
    CHECK(upd["object"]["transform"]["position"].at(0).asNumber() == 4.0);
    Json st = ok(a, "engine.status");
    CHECK(st["dirty"].asBool() && st["canUndo"].asBool());
    CHECK(st["undoLabel"].asString() == "Transform Hero Cube");

    // Engine picking sees the moved cube at its new location.
    Json hit = ok(a, "scene.pick", params({{"origin", Json(Json::Array{Json(4), Json(10), Json(-2)})},
                                           {"direction", Json(Json::Array{Json(0), Json(-1), Json(0)})}}));
    CHECK(hit["id"].asUInt() == cube);

    ok(a, "edit.undo");
    CHECK(ok(a, "object.get", params({{"id", Json(cube)}}))["transform"]["position"].at(0).asNumber() == 0.0);
    CHECK(!ok(a, "engine.status")["dirty"].asBool());
    ok(a, "edit.redo");
    CHECK(ok(a, "object.get", params({{"id", Json(cube)}}))["transform"]["position"].at(0).asNumber() == 4.0);

    // Invalid edits are rejected without changing state.
    CHECK(errorCode(a, "object.update", params({{"id", Json(cube)}, {"set", params({{"secret", Json(1)}})}})) ==
          protocol::kInvalidParams);
    CHECK(errorCode(a, "object.update", params({{"id", Json(999999)}, {"set", params({{"name", Json("x")}})}})) ==
          protocol::kNotFound);
    CHECK(errorCode(a, "project.close") == protocol::kConflict);  // unsaved changes protected

    Json saved = ok(a, "project.save");
    CHECK(saved["saveMs"].asNumber() >= 0);
    CHECK(!ok(a, "engine.status")["dirty"].asBool());
    ok(a, "project.close");
    CHECK(errorCode(a, "scene.query") == protocol::kNoProject);

    Json reopened = ok(a, "project.open", params({{"path", Json(dir.string())}}));
    ObjectId again = idByName(reopened["scene"], "Hero Cube");
    CHECK(again == cube);  // stable ids across save/load
    for (const auto& o : reopened["scene"]["objects"].asArray())
        if (o["id"].asUInt() == cube) CHECK(o["transform"]["position"].at(2).asNumber() == -2.0);
    CHECK(reopened["state"]["selection"].at(0).asUInt() == cube);  // editor state restored

    ok(a, "engine.shutdown");
    CHECK(host.waitExit() == 0);
    fs::remove_all(dir);
}

TEST("host: world builder preview -> approve -> undo -> regenerate") {
    HostProcess host("wb");
    fs::path dir = freshProjectDir("wb");
    ipc::Client c;
    CHECK(c.connect(host.socket, 3000));
    hello(c);
    Json opened = ok(c, "project.create", params({{"path", Json(dir.string())}, {"name", Json("WB")}}));
    const size_t base = opened["scene"]["objects"].size();

    Json exts = ok(c, "extensions.list");
    bool loaded = false;
    for (const auto& e : exts["extensions"].asArray())
        if (e["id"].asString() == "titan.procedural.scatter") loaded = e["status"].asString() == "loaded";
    CHECK(loaded);

    Json recipe = params({{"seed", Json(7)}, {"count", Json(12)}, {"radius", Json(10)}});
    Json p1 = ok(c, "worldbuilder.preview",
                 params({{"extension", Json("titan.procedural.scatter")}, {"recipe", recipe}}));
    Json p2 = ok(c, "worldbuilder.preview",
                 params({{"extension", Json("titan.procedural.scatter")}, {"recipe", recipe}}));
    CHECK(p1["objects"].dump() == p2["objects"].dump());  // deterministic preview
    // Preview does not touch the scene.
    CHECK(ok(c, "scene.query")["objects"].size() == base);
    // Approving a superseded proposal is refused.
    CHECK(errorCode(c, "worldbuilder.approve", params({{"proposalId", p1["proposalId"]}})) == protocol::kConflict);
    Json approved = ok(c, "worldbuilder.approve", params({{"proposalId", p2["proposalId"]}}));
    const size_t made = approved["created"].size();
    CHECK(made == p2["objects"].size() && made > 0);
    Json scene = ok(c, "scene.query");
    CHECK(scene["objects"].size() == base + made + 1);  // + group
    CHECK(scene["recipes"].size() == 1);
    std::string recipeId = approved["recipeId"].asString();

    ok(c, "edit.undo");  // one transaction for the whole generation
    CHECK(ok(c, "scene.query")["objects"].size() == base);
    CHECK(ok(c, "scene.query")["recipes"].size() == 0);
    ok(c, "edit.redo");

    // Selective regeneration replaces only this recipe's output, keeping the group id.
    Json recipe2 = params({{"seed", Json(8)}, {"count", Json(5)}, {"radius", Json(10)}});
    Json p3 = ok(c, "worldbuilder.preview",
                 params({{"extension", Json("titan.procedural.scatter")}, {"recipe", recipe2},
                         {"recipeId", Json(recipeId)}}));
    CHECK(p3["replaces"].size() == made);
    Json re = ok(c, "worldbuilder.approve", params({{"proposalId", p3["proposalId"]}}));
    CHECK(re["groupId"].asUInt() == approved["groupId"].asUInt());
    Json scene2 = ok(c, "scene.query");
    CHECK(scene2["objects"].size() == base + 5 + 1);
    CHECK(scene2["recipes"].at(0)["params"]["seed"].asNumber() == 8);

    ok(c, "project.save");
    ok(c, "project.close");
    Json reopened = ok(c, "project.open", params({{"path", Json(dir.string())}}));
    CHECK(reopened["scene"]["recipes"].at(0)["id"].asString() == recipeId);  // recipe persisted
    ok(c, "engine.shutdown");
    CHECK(host.waitExit() == 0);
    fs::remove_all(dir);
}

TEST("host: assets.refresh imports project assets incrementally; assets.list reports them") {
    HostProcess host("assets");
    fs::path dir = freshProjectDir("assets");
    ipc::Client c;
    CHECK(c.connect(host.socket, 3000));
    hello(c);
    CHECK(errorCode(c, "assets.list") == protocol::kNoProject);
    ok(c, "project.create", params({{"path", Json(dir.string())}, {"name", Json("Assets")}}));
    // A real WAV and a glTF that references a missing texture.
    auto le = [](uint32_t v, int n) { std::string s; for (int i = 0; i < n; ++i) s += char(v >> (8 * i)); return s; };
    const std::string fmt = le(1, 2) + le(1, 2) + le(22050, 4) + le(44100, 4) + le(2, 2) + le(16, 2);
    const std::string data(2000, '\0');
    const std::string body = "WAVE" + std::string("fmt ") + le(16, 4) + fmt + "data" + le(uint32_t(data.size()), 4) + data;
    fs::create_directories(dir / "Assets/Audio");
    std::ofstream(dir / "Assets/Audio/beep.wav", std::ios::binary) << "RIFF" + le(uint32_t(body.size()), 4) + body;
    std::ofstream(dir / "Assets/scene.gltf") << R"({"asset":{"version":"2.0"},"images":[{"uri":"tex.png"}]})";
    std::vector<Json> events;
    Json r = ok(c, "assets.refresh", Json::object(), &events);
    CHECK(r["stats"]["imported"].asNumber() == 1 && r["stats"]["failed"].asNumber() == 1);
    bool sawEvent = false;  // events are broadcast after the response
    for (const auto& e : events) sawEvent |= e["event"].asString() == "assets.changed";
    for (int i = 0; i < 5 && !sawEvent; ++i) {
        auto ev = c.receive(1000);
        sawEvent = ev && (*ev)["event"].isString() && (*ev)["event"].asString() == "assets.changed" &&
                   (*ev)["data"]["imported"].asNumber() == 1;
    }
    CHECK(sawEvent);
    CHECK(r["assets"].size() == 2);
    const Json* wav = nullptr;
    const Json* gltf = nullptr;
    for (const auto& a : r["assets"].asArray()) (a["path"].asString() == "Audio/beep.wav" ? wav : gltf) = &a;
    CHECK(wav && (*wav)["status"].asString() == "ok" && (*wav)["metadata"]["sampleRate"].asNumber() == 22050);
    CHECK(gltf && (*gltf)["status"].asString() == "failed" &&
          (*gltf)["error"].asString().find("missing dependency 'tex.png'") != std::string::npos);
    r = ok(c, "assets.refresh");  // nothing changed: the wav is reused, the glTF retries and fails again
    CHECK(r["stats"]["imported"].asNumber() == 0 && r["stats"]["reused"].asNumber() == 1);
    Json listed = ok(c, "assets.list");
    CHECK(listed["assets"].size() == 2 && !listed.contains("stats"));
    ok(c, "engine.shutdown");
    CHECK(host.waitExit() == 0);
    fs::remove_all(dir);
}

TEST("host: prefab.create saves a hierarchy; prefab.instantiate places linked copies in one undo step") {
    HostProcess host("prefab");
    fs::path dir = freshProjectDir("prefab");
    ipc::Client c;
    CHECK(c.connect(host.socket, 3000));
    hello(c);
    Json opened = ok(c, "project.create", params({{"path", Json(dir.string())}, {"name", Json("Prefabs")}}));
    const ObjectId cube = idByName(opened["scene"], "Hero Cube");
    Json child = ok(c, "object.create", params({{"shape", Json("sphere")}, {"name", Json("Antenna")}, {"parent", Json(cube)}}));
    const size_t base = ok(c, "scene.query")["objects"].size();
    Json made = ok(c, "prefab.create", params({{"id", Json(cube)}, {"name", Json("Robot/Body?")}}));
    CHECK(made["path"].asString() == "Prefabs/Robot_Body_.tprefab" && made["objects"].asNumber() == 2);
    CHECK(fs::exists(dir / "Assets/Prefabs/Robot_Body_.tprefab"));
    CHECK(errorCode(c, "prefab.create", params({{"id", Json(cube)}, {"name", Json("Robot/Body?")}})) == protocol::kInvalidParams);
    // the source objects are now linked to the prefab
    CHECK(ok(c, "object.get", params({{"id", Json(cube)}}))["prefab"]["id"].asString() == made["prefabId"].asString());
    Json inst = ok(c, "prefab.instantiate", params({{"path", Json(made["path"])},
                                                     {"position", Json(Json::Array{Json(4.0), Json(0.0), Json(-2.0)})}}));
    CHECK(inst["ids"].size() == 2);
    const ObjectId newRoot = inst["root"].asUInt();
    Json placed = ok(c, "object.get", params({{"id", Json(newRoot)}}));
    CHECK(placed["transform"]["position"].at(0).asNumber() == 4.0 && placed["prefab"]["local"].asNumber() == 1);
    CHECK(ok(c, "scene.query")["objects"].size() == base + 2);
    ok(c, "edit.undo");  // one step removes the whole instance
    CHECK(ok(c, "scene.query")["objects"].size() == base);
    ok(c, "edit.redo");
    CHECK(ok(c, "scene.query")["objects"].size() == base + 2);
    CHECK(errorCode(c, "prefab.instantiate", params({{"path", Json("../../etc/passwd.tprefab")}})) == protocol::kInvalidParams);
    CHECK(errorCode(c, "prefab.instantiate", params({{"path", Json("Prefabs/none.tprefab")}})) == protocol::kInvalidParams);
    ok(c, "project.save");
    ok(c, "project.close");
    Json re = ok(c, "project.open", params({{"path", Json(dir.string())}}));
    bool linked = false;
    for (const auto& o : re["scene"]["objects"].asArray())
        linked |= o["id"].asUInt() == newRoot && o["prefab"]["id"].asString() == made["prefabId"].asString();
    CHECK(linked);  // links persist in the saved scene
    (void)child;
    ok(c, "engine.shutdown");
    CHECK(host.waitExit() == 0);
    fs::remove_all(dir);
}

TEST("host: duplicate, group and ungroup keep world positions and are single undo steps") {
    HostProcess host("edit");
    fs::path dir = freshProjectDir("edit");
    ipc::Client c;
    CHECK(c.connect(host.socket, 3000));
    hello(c);
    Json opened = ok(c, "project.create", params({{"path", Json(dir.string())}, {"name", Json("Edit")}}));
    const ObjectId cube = idByName(opened["scene"], "Hero Cube");
    const ObjectId sphere = idByName(opened["scene"], "Sphere");
    auto world = [&](ObjectId id) {  // translation column of the world matrix
        const Json m = ok(c, "object.get", params({{"id", Json(id)}}))["worldMatrix"];
        return Vec3{float(m.at(12).asNumber()), float(m.at(13).asNumber()), float(m.at(14).asNumber())};
    };
    auto near = [](Vec3 a, Vec3 b) { return std::fabs(a.x - b.x) < 1e-4 && std::fabs(a.y - b.y) < 1e-4 && std::fabs(a.z - b.z) < 1e-4; };
    auto count = [&] { return ok(c, "scene.query")["objects"].size(); };
    const size_t base = count();
    const Vec3 cubeW = world(cube), sphereW = world(sphere);
    ok(c, "object.create", params({{"shape", Json("cube")}, {"name", Json("Hat")}, {"parent", Json(cube)}}));  // child
    // Duplicate copies the hierarchy once even when parent and child are both selected.
    const ObjectId hat = idByName(ok(c, "scene.query"), "Hat");
    Json dup = ok(c, "object.duplicate", params({{"ids", Json(Json::Array{Json(cube), Json(hat)})},
                                                 {"offset", Json(Json::Array{Json(0.0), Json(0.0), Json(3.0)})}}));
    CHECK(dup["ids"].size() == 1 && count() == base + 3);
    const ObjectId copy = dup["ids"].at(0).asUInt();
    CHECK(near(world(copy), cubeW + Vec3{0, 0, 3}));
    CHECK(ok(c, "object.get", params({{"id", Json(copy)}}))["name"].asString() == "Hero Cube copy");
    ok(c, "edit.undo");
    CHECK(count() == base + 1);  // one step removed the copy and its child
    // Group two siblings: same world positions, new group selected; ungroup restores the flat layout.
    Json grp = ok(c, "object.group", params({{"ids", Json(Json::Array{Json(cube), Json(sphere)})}, {"name", Json("Pair")}}));
    const ObjectId g = grp["id"].asUInt();
    CHECK(ok(c, "object.get", params({{"id", Json(cube)}}))["parent"].asUInt() == g);
    CHECK(near(world(cube), cubeW) && near(world(sphere), sphereW));
    ok(c, "edit.undo");
    CHECK(ok(c, "object.get", params({{"id", Json(cube)}}))["parent"].asUInt() == 0 && count() == base + 1);
    ok(c, "edit.redo");
    Json ug = ok(c, "object.ungroup", params({{"id", Json(g)}}));
    CHECK(ug["ids"].size() == 2 && count() == base + 1);
    CHECK(near(world(cube), cubeW) && near(world(sphere), sphereW));
    CHECK(errorCode(c, "object.ungroup", params({{"id", Json(cube)}})) == protocol::kInvalidParams);  // not a group
    CHECK(errorCode(c, "object.group", params({{"ids", Json(Json::Array{Json(cube), Json(hat)})}})) == 0);  // nested: hat rides along
    ok(c, "engine.shutdown");
    CHECK(host.waitExit() == 0);
    fs::remove_all(dir);
}

TEST("host: unsaved edits survive an engine crash and are recovered in one undo step") {
    fs::path dir = freshProjectDir("crash");
    ObjectId cube = 0, added = 0;
    {
        HostProcess host("crash1", {"--autosave-seconds", "0.3"});
        ipc::Client c;
        CHECK(c.connect(host.socket, 3000));
        hello(c);
        Json opened = ok(c, "project.create", params({{"path", Json(dir.string())}, {"name", Json("Crash")}}));
        cube = idByName(opened["scene"], "Hero Cube");
        Json t = Json::object();
        t["position"] = Json(Json::Array{Json(7.0), Json(1.0), Json(7.0)});
        ok(c, "object.update", params({{"id", Json(cube)}, {"set", params({{"transform", t}})}}));
        added = ok(c, "object.create", params({{"shape", Json("sphere")}, {"name", Json("Unsaved")}}))["id"].asUInt();
        auto recoveryHasLatest = [&] {  // the periodic tick must have captured the last edit
            std::ifstream in(dir / "Saved/Autosave.json");
            return std::string((std::istreambuf_iterator<char>(in)), {}).find("Unsaved") != std::string::npos;
        };
        for (int i = 0; i < 40 && !recoveryHasLatest(); ++i) c.receive(100);
        CHECK(recoveryHasLatest());
        ::kill(host.pid, SIGKILL);  // crash: no save, no clean close
        host.waitExit();
    }
    HostProcess host("crash2");
    ipc::Client c;
    CHECK(c.connect(host.socket, 3000));
    hello(c);
    Json reopened = ok(c, "project.open", params({{"path", Json(dir.string())}}));
    CHECK(reopened["recovery"]["available"].asBool());
    const size_t savedCount = reopened["scene"]["objects"].size();
    ok(c, "project.recover");
    CHECK(ok(c, "object.get", params({{"id", Json(cube)}}))["transform"]["position"].at(0).asNumber() == 7.0);
    CHECK(ok(c, "object.get", params({{"id", Json(added)}}))["name"].asString() == "Unsaved");
    CHECK(ok(c, "engine.status")["dirty"].asBool());  // recovered work still has to be saved
    ok(c, "edit.undo");  // one step back to the saved scene
    CHECK(ok(c, "scene.query")["objects"].size() == savedCount);
    ok(c, "edit.redo");
    const ObjectId fresh = ok(c, "object.create", params({{"shape", Json("cube")}}))["id"].asUInt();
    CHECK(fresh > added);  // ids are never reused after recovery
    ok(c, "project.save");
    CHECK(!fs::exists(dir / "Saved/Autosave.json"));  // the save supersedes the recovery point
    ok(c, "project.close");
    CHECK(!ok(c, "project.open", params({{"path", Json(dir.string())}})).contains("recovery"));
    CHECK(errorCode(c, "project.recover") == protocol::kNotFound);
    // A damaged recovery point is ignored, never loaded.
    std::ofstream(dir / "Saved/Autosave.json") << "{\"format\":1,\"scene\":{\"broken";
    ok(c, "project.close", params({{"force", Json(true)}}));
    CHECK(!ok(c, "project.open", params({{"path", Json(dir.string())}})).contains("recovery"));
    ok(c, "engine.shutdown");
    CHECK(host.waitExit() == 0);
    fs::remove_all(dir);
}

TEST("host: scene templates: built-ins, save the current scene, create a project from it") {
    const fs::path tdir = fs::temp_directory_path() / ("titan-it-templates-" + std::to_string(::getpid()));
    fs::remove_all(tdir);
    HostProcess host("tmpl", {"--templates", tdir.string()});
    ipc::Client c;
    CHECK(c.connect(host.socket, 3000));
    hello(c);
    Json list = ok(c, "templates.list");
    CHECK(list["templates"].size() == 3 && list["templates"].at(0)["builtin"].asBool());
    fs::path a = freshProjectDir("tmpl-a"), b = freshProjectDir("tmpl-b"), e = freshProjectDir("tmpl-e");
    Json empty = ok(c, "project.create", params({{"path", Json(e.string())}, {"name", Json("E")}, {"template", Json("Empty")}}));
    CHECK(empty["scene"]["objects"].size() == 0);
    Json ga = ok(c, "project.create", params({{"path", Json(a.string())}, {"name", Json("A")}, {"template", Json("Ground and Light")}, {"force", Json(true)}}));
    CHECK(ga["scene"]["objects"].size() == 3);
    ok(c, "object.create", params({{"shape", Json("cylinder")}, {"name", Json("Totem")}}));
    CHECK(errorCode(c, "project.saveTemplate", params({{"name", Json("Default")}})) == protocol::kInvalidParams);  // built-in name
    Json saved = ok(c, "project.saveTemplate", params({{"name", Json("My Yard")}, {"description", Json("ground + totem")}}));
    CHECK(saved["objects"].asNumber() == 4 && fs::exists(tdir / "My Yard.ttemplate"));
    CHECK(errorCode(c, "project.saveTemplate", params({{"name", Json("My Yard")}})) == protocol::kInvalidParams);  // exists
    std::ofstream(tdir / "broken.ttemplate") << "{nope";  // invalid files are skipped
    CHECK(ok(c, "templates.list")["templates"].size() == 4);
    Json fromUser = ok(c, "project.create", params({{"path", Json(b.string())}, {"name", Json("B")}, {"template", Json("My Yard")}, {"force", Json(true)}}));
    CHECK(fromUser["scene"]["objects"].size() == 4);
    idByName(fromUser["scene"], "Totem");
    CHECK(errorCode(c, "project.create", params({{"path", Json(freshProjectDir("tmpl-x").string())}, {"name", Json("X")},
                                                 {"template", Json("Nope")}, {"force", Json(true)}})) == protocol::kNotFound);
    ok(c, "engine.shutdown");
    CHECK(host.waitExit() == 0);
    for (const auto& d : {a, b, e, tdir}) fs::remove_all(d);
}

TEST("host: extensions can be disabled (persisted, never loaded), re-enabled and rescanned") {
    const fs::path state = fs::temp_directory_path() / ("titan-it-extstate-" + std::to_string(::getpid()) + ".json");
    fs::remove(state);
    const std::string scatter = "titan.procedural.scatter";
    auto statusOf = [&](ipc::Client& c, const std::string& method) {
        const Json r = ok(c, method, method == "extensions.list" ? Json::object() : params({{"id", Json(scatter)}, {"enabled", Json(false)}}));
        for (const auto& e : r["extensions"].asArray())  // keep the result alive while iterating
            if (e["id"].asString() == scatter) return e["status"].asString() + (e["enabled"].asBool() ? "+on" : "+off");
        return std::string("absent");
    };
    fs::path dir = freshProjectDir("ext");
    Json recipe = Json::object();
    recipe["seed"] = 1; recipe["count"] = 3; recipe["radius"] = 4; recipe["minSpacing"] = 0.5; recipe["shape"] = "cube";
    auto preview = [&](ipc::Client& c) {
        return errorCode(c, "worldbuilder.preview", params({{"extension", Json(scatter)}, {"recipe", recipe}}));
    };
    {
        HostProcess host("ext1", {"--extension-state", state.string()});
        ipc::Client c;
        CHECK(c.connect(host.socket, 3000));
        hello(c);
        CHECK(statusOf(c, "extensions.list") == "loaded+on");
        ok(c, "project.create", params({{"path", Json(dir.string())}, {"name", Json("Ext")}}));
        CHECK(preview(c) == 0);
        CHECK(statusOf(c, "extensions.setEnabled") == "disabled+off");  // disables and rescans
        CHECK(preview(c) != 0);                                         // no longer usable
        CHECK(fs::exists(state));
        CHECK(errorCode(c, "extensions.setEnabled", params({{"id", Json("no.such.ext")}, {"enabled", Json(false)}})) == protocol::kNotFound);
        ok(c, "engine.shutdown");
        CHECK(host.waitExit() == 0);
    }
    HostProcess host("ext2", {"--extension-state", state.string()});
    ipc::Client c;
    CHECK(c.connect(host.socket, 3000));
    hello(c);
    CHECK(statusOf(c, "extensions.list") == "disabled+off");  // persisted across engine restarts
    Json on = ok(c, "extensions.setEnabled", params({{"id", Json(scatter)}, {"enabled", Json(true)}}));
    bool loaded = false;
    for (const auto& e : on["extensions"].asArray()) loaded |= e["id"].asString() == scatter && e["status"].asString() == "loaded";
    CHECK(loaded);
    ok(c, "project.open", params({{"path", Json(dir.string())}}));
    ok(c, "extensions.reload");
    CHECK(preview(c) == 0);
    ok(c, "engine.shutdown");
    CHECK(host.waitExit() == 0);
    fs::remove(state);
    fs::remove_all(dir);
}

TEST("host: capability contract: versions in hello, unadvertised capabilities refused as unsupported") {
    HostProcess host("caps");
    ipc::Client c;
    CHECK(c.connect(host.socket, 3000));
    Json proto = Json::object();
    proto["min"] = 1;
    proto["max"] = 1;
    Json hi = ok(c, "session.hello", params({{"client", Json("caps-test")}, {"protocol", proto}}));
    CHECK(hi["capabilities"].size() == hi["capabilityVersions"].asObject().size());
    for (const auto& cap : hi["capabilities"].asArray()) {
        const CapabilitySpec* spec = findCapability(cap.asString());
        CHECK(spec && hi["capabilityVersions"][cap.asString()].asNumber() == spec->version);
    }
    // viewport.frames is an Unreal-backend capability: the reference host refuses it explicitly.
    CHECK(errorCode(c, "viewport.configure", params({{"width", Json(64)}, {"height", Json(64)}})) == protocol::kUnsupported);
    CHECK(errorCode(c, "no.such.method") == protocol::kMethodNotFound);  // unknown stays "not found"
    // Every advertised capability's methods are implemented (answer with something other than not-found).
    for (const auto& cap : hi["capabilities"].asArray())
        for (const char* m : findCapability(cap.asString())->methods) {
            if (std::string(m) == "extensions.reload" || std::string(m) == "extensions.setEnabled") continue;  // mutate state
            const int code = errorCode(c, m);
            CHECK(code != protocol::kMethodNotFound && code != protocol::kUnsupported);
        }
    ok(c, "engine.shutdown");
    CHECK(host.waitExit() == 0);
}

TEST("host: multi-object edits are component-wise, atomic, one undo step, and persist") {
    HostProcess host("multi");
    fs::path dir = freshProjectDir("multi");
    ipc::Client c;
    CHECK(c.connect(host.socket, 3000));
    hello(c);
    Json opened = ok(c, "project.create", params({{"path", Json(dir.string())}, {"name", Json("Multi")}}));
    const ObjectId a = idByName(opened["scene"], "Hero Cube"), b = idByName(opened["scene"], "Sphere");
    auto pos = [&](ObjectId id) { return ok(c, "object.get", params({{"id", Json(id)}}))["transform"]["position"]; };
    auto scl = [&](ObjectId id) { return ok(c, "object.get", params({{"id", Json(id)}}))["transform"]["scale"]; };
    const Json pa = pos(a), pb = pos(b);
    const Json ids = Json(Json::Array{Json(a), Json(b)});
    // Absolute X for both; Y and Z (different per object) are kept.
    ok(c, "object.transformMany", params({{"ids", ids}, {"position", Json(Json::Array{Json(5.0), Json(nullptr), Json(nullptr)})}}));
    CHECK(pos(a).at(0).asNumber() == 5.0 && pos(b).at(0).asNumber() == 5.0);
    CHECK(pos(a).at(2).asNumber() == pa.at(2).asNumber() && pos(b).at(2).asNumber() == pb.at(2).asNumber());
    // Relative: +1 on Z for both, scale x2 on Y.
    ok(c, "object.transformMany", params({{"ids", ids}, {"mode", Json("offset")},
                                          {"position", Json(Json::Array{Json(nullptr), Json(nullptr), Json(1.0)})},
                                          {"scale", Json(Json::Array{Json(nullptr), Json(2.0), Json(nullptr)})}}));
    CHECK(std::fabs(pos(b).at(2).asNumber() - (pb.at(2).asNumber() + 1)) < 1e-5);
    CHECK(scl(a).at(1).asNumber() == 4.0);  // Hero Cube scale y was 2
    // Invalid batch: scale 0 on one axis would hit both -> refused, nothing changed.
    const double before = scl(a).at(0).asNumber();
    CHECK(errorCode(c, "object.transformMany", params({{"ids", ids}, {"scale", Json(Json::Array{Json(0.0), Json(nullptr), Json(nullptr)})}})) == protocol::kInvalidParams);
    CHECK(errorCode(c, "object.transformMany", params({{"ids", Json(Json::Array{Json(a), Json(999999)})}, {"position", Json(Json::Array{Json(0.0), Json(0.0), Json(0.0)})}})) == protocol::kNotFound);
    CHECK(errorCode(c, "object.transformMany", params({{"ids", ids}, {"position", Json(Json::Array{Json("x"), Json(nullptr), Json(nullptr)})}})) == protocol::kInvalidParams);
    CHECK(scl(a).at(0).asNumber() == before && pos(a).at(0).asNumber() == 5.0);
    // Shared property for many objects; one undo step reverts each batch.
    ok(c, "object.updateMany", params({{"ids", ids}, {"set", params({{"visible", Json(false)}})}}));
    CHECK(!ok(c, "object.get", params({{"id", Json(a)}}))["visible"].asBool() && !ok(c, "object.get", params({{"id", Json(b)}}))["visible"].asBool());
    CHECK(errorCode(c, "object.updateMany", params({{"ids", ids}, {"set", params({{"name", Json("X")}})}})) == protocol::kInvalidParams);
    ok(c, "edit.undo");
    CHECK(ok(c, "object.get", params({{"id", Json(a)}}))["visible"].asBool() && ok(c, "object.get", params({{"id", Json(b)}}))["visible"].asBool());
    ok(c, "edit.undo");  // the offset batch
    CHECK(std::fabs(pos(b).at(2).asNumber() - pb.at(2).asNumber()) < 1e-5 && scl(a).at(1).asNumber() == 2.0);
    ok(c, "edit.redo");
    ok(c, "project.save");
    ok(c, "project.close");
    ok(c, "project.open", params({{"path", Json(dir.string())}}));
    CHECK(pos(a).at(0).asNumber() == 5.0 && scl(a).at(1).asNumber() == 4.0);  // persisted
    ok(c, "engine.shutdown");
    CHECK(host.waitExit() == 0);
    fs::remove_all(dir);
}

TEST("host: reparenting keeps world transforms, refuses cycles and shear, is one undo step, and persists") {
    HostProcess host("reparent");
    fs::path dir = freshProjectDir("reparent");
    ipc::Client c;
    CHECK(c.connect(host.socket, 3000));
    hello(c);
    Json opened = ok(c, "project.create", params({{"path", Json(dir.string())}, {"name", Json("Reparent")}}));
    const ObjectId a = idByName(opened["scene"], "Hero Cube"), b = idByName(opened["scene"], "Sphere");
    auto get = [&](ObjectId id) { return ok(c, "object.get", params({{"id", Json(id)}})); };
    auto vec = [](double x, double y, double z) { return Json(Json::Array{Json(x), Json(y), Json(z)}); };
    auto make = [&](const char* name, Json pos, Json rot, Json scale) {
        return ObjectId(ok(c, "object.create", params({{"name", Json(name)}, {"type", Json("group")}, {"shape", Json("none")},
            {"transform", params({{"position", pos}, {"rotation", rot}, {"scale", scale}})}}))["id"].asNumber());
    };
    const Json ta = get(a)["transform"], tb = get(b)["transform"];
    const ObjectId g = make("Pivot", vec(10, 0, 0), vec(0, 0, 0), vec(2, 2, 2));
    const Json ids = Json(Json::Array{Json(a), Json(b)});
    Json r = ok(c, "object.reparent", params({{"ids", ids}, {"parent", Json(g)}}));
    CHECK(r["changed"].asNumber() == 2);
    CHECK(get(a)["parent"].asNumber() == g && get(b)["parent"].asNumber() == g);
    // World kept: local = (world - 10 on X) / 2.
    CHECK(std::fabs(get(a)["transform"]["position"].at(0).asNumber() - (ta["position"].at(0).asNumber() - 10) / 2) < 1e-4);
    CHECK(std::fabs(get(a)["transform"]["scale"].at(1).asNumber() - ta["scale"].at(1).asNumber() / 2) < 1e-4);
    // Cycles are refused: a group under its own child, or under itself.
    CHECK(errorCode(c, "object.reparent", params({{"ids", Json(Json::Array{Json(g)})}, {"parent", Json(a)}})) == protocol::kInvalidParams);
    CHECK(errorCode(c, "object.reparent", params({{"ids", Json(Json::Array{Json(g)})}, {"parent", Json(g)}})) == protocol::kInvalidParams);
    CHECK(errorCode(c, "object.reparent", params({{"ids", ids}, {"parent", Json(999999)}})) == protocol::kNotFound);
    // Shear: a rotated child cannot keep its world transform under a non-uniformly scaled parent -> nothing changes.
    const ObjectId squash = make("Squash", vec(0, 0, 0), vec(0, 0, 0), vec(3, 1, 1));
    const ObjectId tilted = make("Tilted", vec(0, 0, 0), vec(0, 0, 45), vec(1, 1, 1));
    CHECK(errorCode(c, "object.reparent", params({{"ids", Json(Json::Array{Json(a), Json(tilted)})}, {"parent", Json(squash)}})) == protocol::kInvalidParams);
    CHECK(get(a)["parent"].asNumber() == g && get(tilted)["parent"].asNumber() == 0);
    // keepWorld=false keeps the local transform as it is.
    const Json localTilted = get(tilted)["transform"];
    ok(c, "object.reparent", params({{"ids", Json(Json::Array{Json(tilted)})}, {"parent", Json(squash)}, {"keepWorld", Json(false)}}));
    CHECK(get(tilted)["transform"].dump() == localTilted.dump() && get(tilted)["parent"].asNumber() == squash);
    ok(c, "edit.undo");
    CHECK(get(tilted)["parent"].asNumber() == 0);
    // Back to the root with keepWorld: the original transforms return.
    ok(c, "object.reparent", params({{"ids", ids}, {"parent", Json(0.0)}}));
    for (int k = 0; k < 3; ++k) CHECK(std::fabs(get(b)["transform"]["position"].at(size_t(k)).asNumber() - tb["position"].at(size_t(k)).asNumber()) < 1e-4);
    CHECK(ok(c, "object.reparent", params({{"ids", ids}, {"parent", Json(0.0)}}))["changed"].asNumber() == 0);  // no-op
    ok(c, "edit.undo");  // one step puts both back under the pivot
    CHECK(get(a)["parent"].asNumber() == g && get(b)["parent"].asNumber() == g);
    ok(c, "project.save");
    ok(c, "project.close");
    ok(c, "project.open", params({{"path", Json(dir.string())}}));
    CHECK(get(a)["parent"].asNumber() == g && get(b)["parent"].asNumber() == g);
    ok(c, "engine.shutdown");
    CHECK(host.waitExit() == 0);
    fs::remove_all(dir);
}

TEST("host: locked objects are not pickable or movable, the lock persists, unlocked scenes serialize unchanged") {
    HostProcess host("lock");
    fs::path dir = freshProjectDir("lock");
    ipc::Client c;
    CHECK(c.connect(host.socket, 3000));
    hello(c);
    Json opened = ok(c, "project.create", params({{"path", Json(dir.string())}, {"name", Json("Lock")}}));
    const ObjectId a = idByName(opened["scene"], "Hero Cube"), b = idByName(opened["scene"], "Sphere");
    auto sceneFile = [&] {
        for (const auto& e : fs::directory_iterator(dir / "Scenes")) {
            std::ifstream in(e.path());
            return std::string((std::istreambuf_iterator<char>(in)), {});
        }
        return std::string();
    };
    ok(c, "project.save");
    CHECK(sceneFile().find("\"locked\"") == std::string::npos);  // nothing locked -> no new key on disk
    auto pickDown = [&] {
        return ok(c, "scene.pick", params({{"origin", Json(Json::Array{Json(0), Json(10), Json(0)})},
                                           {"direction", Json(Json::Array{Json(0), Json(-1), Json(0)})}}))["id"].asUInt();
    };
    CHECK(pickDown() == a);
    ok(c, "object.update", params({{"id", Json(a)}, {"set", params({{"locked", Json(true)}})}}));
    CHECK(ok(c, "object.get", params({{"id", Json(a)}})).getBool("locked", false));
    CHECK(pickDown() != a);  // picks through the locked cube
    Json t = Json::object();
    t["position"] = Json(Json::Array{Json(9.0), Json(0.0), Json(0.0)});
    CHECK(errorCode(c, "object.update", params({{"id", Json(a)}, {"set", params({{"transform", t}})}})) == protocol::kInvalidParams);
    const Json ids = Json(Json::Array{Json(a), Json(b)});
    CHECK(errorCode(c, "object.transformMany", params({{"ids", ids}, {"position", Json(Json::Array{Json(1.0), Json(nullptr), Json(nullptr)})}})) == protocol::kInvalidParams);
    CHECK(ok(c, "object.get", params({{"id", Json(b)}}))["transform"]["position"].at(0).asNumber() != 1.0);  // batch untouched
    ok(c, "object.updateMany", params({{"ids", ids}, {"set", params({{"locked", Json(true)}})}}));  // lock both, one step
    CHECK(ok(c, "object.get", params({{"id", Json(b)}})).getBool("locked", false));
    ok(c, "edit.undo");
    CHECK(!ok(c, "object.get", params({{"id", Json(b)}})).getBool("locked", false) && ok(c, "object.get", params({{"id", Json(a)}})).getBool("locked", false));
    ok(c, "project.save");
    ok(c, "project.close");
    ok(c, "project.open", params({{"path", Json(dir.string())}}));
    CHECK(ok(c, "object.get", params({{"id", Json(a)}})).getBool("locked", false) && !ok(c, "object.get", params({{"id", Json(b)}})).getBool("locked", false));
    ok(c, "object.update", params({{"id", Json(a)}, {"set", params({{"locked", Json(false)}})}}));
    ok(c, "object.update", params({{"id", Json(a)}, {"set", params({{"transform", t}})}}));  // movable again
    CHECK(ok(c, "object.get", params({{"id", Json(a)}}))["transform"]["position"].at(0).asNumber() == 9.0);
    ok(c, "engine.shutdown");
    CHECK(host.waitExit() == 0);
    fs::remove_all(dir);
}

TEST("host: camera bookmarks are validated and persist with the project") {
    HostProcess host("bookmarks");
    fs::path dir = freshProjectDir("bookmarks");
    ipc::Client c;
    CHECK(c.connect(host.socket, 3000));
    hello(c);
    ok(c, "project.create", params({{"path", Json(dir.string())}, {"name", Json("Bookmarks")}}));
    auto mark = [](const char* name, double distance) {
        Json cam = Json::object();
        cam["target"] = Json(Json::Array{Json(1.0), Json(2.0), Json(3.0)});
        cam["yaw"] = 40.0;
        cam["pitch"] = 12.0;
        cam["distance"] = distance;
        Json b = Json::object();
        b["name"] = name;
        b["camera"] = cam;
        return b;
    };
    ok(c, "editor.setState", params({{"bookmarks", Json(Json::Array{mark("Front", 9.0), mark("Top", 30.0)})}}));
    CHECK(errorCode(c, "editor.setState", params({{"bookmarks", Json(Json::Array{mark("Bad", 0.0)})}})) == protocol::kInvalidParams);
    CHECK(errorCode(c, "editor.setState", params({{"bookmarks", Json(Json::Array{mark("", 5.0)})}})) == protocol::kInvalidParams);
    Json many = Json::array();
    for (int i = 0; i < 33; ++i) many.push_back(mark("M", 5.0));
    CHECK(errorCode(c, "editor.setState", params({{"bookmarks", many}})) == protocol::kInvalidParams);
    ok(c, "project.close");
    Json reopened = ok(c, "project.open", params({{"path", Json(dir.string())}}));
    const Json& marks = reopened["editor"]["bookmarks"];
    CHECK(marks.isArray() && marks.asArray().size() == 2);  // the refused updates changed nothing
    CHECK(marks.at(0)["name"].asString() == "Front" && marks.at(1)["camera"]["distance"].asNumber() == 30.0);
    ok(c, "engine.shutdown");
    CHECK(host.waitExit() == 0);
    fs::remove_all(dir);
}

TEST("host: copy/paste keeps relationships with fresh ids, is one undo step, persists, and pastes across projects") {
    HostProcess host("clip");
    fs::path dir = freshProjectDir("clip"), other = freshProjectDir("clip-other");
    ipc::Client c;
    CHECK(c.connect(host.socket, 3000));
    hello(c);
    Json opened = ok(c, "project.create", params({{"path", Json(dir.string())}, {"name", Json("Clip")}}));
    const ObjectId hero = idByName(opened["scene"], "Hero Cube"), props = idByName(opened["scene"], "Props");
    auto count = [&] { return size_t(ok(c, "engine.status")["objectCount"].asNumber()); };
    const size_t before = count();
    Json copied = ok(c, "object.copy", params({{"ids", Json(Json::Array{Json(hero), Json(props)})}}));
    CHECK(copied["count"].asNumber() == 5);  // hero + props + 3 crates
    CHECK(errorCode(c, "object.copy", params({{"ids", Json(Json::Array{Json(999999)})}})) == protocol::kNotFound);
    Json pasted = ok(c, "object.paste", params({{"clipboard", copied["clipboard"]}}));
    CHECK(pasted["ids"].size() == 5 && pasted["roots"].size() == 2 && count() == before + 5);
    std::set<ObjectId> fresh;
    for (const auto& v : pasted["ids"].asArray()) fresh.insert(v.asUInt());
    CHECK(fresh.size() == 5 && !fresh.count(hero) && !fresh.count(props));
    for (const auto& v : pasted["ids"].asArray()) {  // every parent is the root or another pasted object
        const ObjectId p = ok(c, "object.get", params({{"id", v}}))["parent"].asUInt();
        CHECK(p == kNoObject || fresh.count(p) == 1);
    }
    CHECK(ok(c, "engine.status")["selection"].size() == 2);  // the pasted roots
    // Malformed or foreign clipboards and unknown parents change nothing.
    Json bad = copied["clipboard"];
    bad["version"] = 99;
    CHECK(errorCode(c, "object.paste", params({{"clipboard", bad}})) == protocol::kInvalidParams);
    CHECK(errorCode(c, "object.paste", params({{"clipboard", Json("text")}})) == protocol::kInvalidParams);
    CHECK(errorCode(c, "object.paste", params({{"clipboard", copied["clipboard"]}, {"parent", Json(999999)}})) == protocol::kNotFound);
    CHECK(count() == before + 5);
    ok(c, "edit.undo");
    CHECK(count() == before);
    ok(c, "edit.redo");
    CHECK(count() == before + 5 && ok(c, "object.get", params({{"id", pasted["ids"].at(0)}}))["name"].asString() == "Hero Cube");
    ok(c, "project.save");
    ok(c, "project.close");
    ok(c, "project.open", params({{"path", Json(dir.string())}}));
    CHECK(count() == before + 5);
    ok(c, "project.close", params({{"force", Json(true)}}));
    // Into another project: the clipboard is self-contained.
    ok(c, "project.create", params({{"path", Json(other.string())}, {"name", Json("Other")}}));
    const size_t otherBefore = count();
    CHECK(ok(c, "object.paste", params({{"clipboard", copied["clipboard"]}}))["ids"].size() == 5 && count() == otherBefore + 5);
    // Sheared placements: copy and paste are refused with kInvalidParams and change nothing.
    auto v3 = [](double x, double y, double z) { return Json(Json::Array{Json(x), Json(y), Json(z)}); };
    auto create = [&](const char* name, ObjectId parent, Json rot, Json scale) {
        return ObjectId(ok(c, "object.create", params({{"name", Json(name)}, {"type", Json("group")}, {"shape", Json("none")},
            {"parent", Json(parent)}, {"transform", params({{"rotation", rot}, {"scale", scale}})}}))["id"].asNumber());
    };
    const ObjectId squash = create("Squash", kNoObject, v3(0, 0, 0), v3(3, 1, 1));
    const ObjectId tilted = create("Tilted", squash, v3(0, 0, 45), v3(1, 1, 1));
    CHECK(errorCode(c, "object.copy", params({{"ids", Json(Json::Array{Json(tilted)})}})) == protocol::kInvalidParams);
    const ObjectId loose = create("Loose", kNoObject, v3(0, 0, 45), v3(1, 1, 1));
    const Json looseClip = ok(c, "object.copy", params({{"ids", Json(Json::Array{Json(loose)})}}))["clipboard"];
    const size_t n = count();
    const double rev = ok(c, "engine.status")["revision"].asNumber();
    CHECK(errorCode(c, "object.paste", params({{"clipboard", looseClip}, {"parent", Json(squash)}})) == protocol::kInvalidParams);
    CHECK(count() == n && ok(c, "engine.status")["revision"].asNumber() == rev);
    ok(c, "engine.shutdown");
    CHECK(host.waitExit() == 0);
    fs::remove_all(dir);
    fs::remove_all(other);
}

TEST("host: undo history lists the existing stack and jumps several steps at once") {
    HostProcess host("history");
    fs::path dir = freshProjectDir("history");
    ipc::Client c;
    CHECK(c.connect(host.socket, 3000));
    hello(c);
    Json opened = ok(c, "project.create", params({{"path", Json(dir.string())}, {"name", Json("History")}}));
    const ObjectId a = idByName(opened["scene"], "Hero Cube"), b = idByName(opened["scene"], "Sphere");
    auto rename = [&](ObjectId id, const char* name) { ok(c, "object.update", params({{"id", Json(id)}, {"set", params({{"name", Json(name)}})}})); };
    auto nameOf = [&](ObjectId id) { return ok(c, "object.get", params({{"id", Json(id)}}))["name"].asString(); };
    Json h = ok(c, "edit.history");
    CHECK(h["entries"].size() == 0 && h["position"].asNumber() == 0 && h["saved"].asNumber() == 0);
    rename(a, "A1");
    rename(b, "B1");
    rename(a, "A2");
    h = ok(c, "edit.history");
    CHECK(h["entries"].size() == 3 && h["position"].asNumber() == 3);
    CHECK(h["entries"].at(0).asString().find("Rename") == 0);
    Json g = ok(c, "edit.goto", params({{"position", Json(0.0)}}));
    CHECK(g["steps"].asNumber() == 3 && nameOf(a) == "Hero Cube" && nameOf(b) == "Sphere");
    CHECK(ok(c, "edit.history")["entries"].size() == 3);  // redo entries stay listed
    ok(c, "edit.goto", params({{"position", Json(2.0)}}));
    CHECK(nameOf(a) == "A1" && nameOf(b) == "B1");
    CHECK(ok(c, "edit.goto", params({{"position", Json(2.0)}}))["steps"].asNumber() == 0);
    CHECK(errorCode(c, "edit.goto", params({{"position", Json(4.0)}})) == protocol::kInvalidParams);
    CHECK(errorCode(c, "edit.goto", params({{"position", Json(1.5)}})) == protocol::kInvalidParams);
    CHECK(errorCode(c, "edit.goto", params({{"position", Json("x")}})) == protocol::kInvalidParams);
    ok(c, "project.save");
    CHECK(ok(c, "edit.history")["saved"].asNumber() == 2);
    ok(c, "edit.goto", params({{"position", Json(1.0)}}));
    CHECK(ok(c, "engine.status")["dirty"].asBool());
    rename(b, "B9");  // a new edit drops the redo branch, as single-step undo does
    h = ok(c, "edit.history");
    CHECK(h["entries"].size() == 2 && h["position"].asNumber() == 2 && h["saved"].asNumber() == -1);
    ok(c, "engine.shutdown");
    CHECK(host.waitExit() == 0);
    fs::remove_all(dir);
}

TEST("host: survives malformed frames and abrupt client disconnects") {
    HostProcess host("robust");
    {
        ipc::Client idle;  // connects and vanishes without a word
        CHECK(idle.connect(host.socket, 3000));
        ipc::Fd raw = ipc::connectUnix(host.socket, 1000);
        CHECK(static_cast<bool>(raw));
        // A frame whose payload is not JSON gets an error response...
        std::string frame = protocol::encodeFrame(std::string_view("{not json"));
        CHECK(::write(raw.get(), frame.data(), frame.size()) == ssize_t(frame.size()));
        // ...and an oversized length header makes the host drop that connection only.
        const unsigned char huge[4] = {0x7F, 0xFF, 0xFF, 0xFF};
        CHECK(::write(raw.get(), huge, 4) == 4);
    }  // both sockets close abruptly here
    ipc::Client c;
    CHECK(c.connect(host.socket, 3000));
    hello(c);
    CHECK(ok(c, "engine.status")["metrics"]["requests"].asNumber() >= 1);
    ok(c, "engine.shutdown");
    CHECK(host.waitExit() == 0);
}

TITAN_TEST_MAIN()
